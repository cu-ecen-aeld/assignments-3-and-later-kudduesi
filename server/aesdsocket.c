#include <arpa/inet.h>
#include <asm-generic/socket.h>
#include <bits/time.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/syslog.h>
#include <sys/types.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#ifndef USE_AESD_CHAR_DEVICE
#define USE_AESD_CHAR_DEVICE 1
#endif

#define BACKLOG 10
#define PORT "9000"

#if USE_AESD_CHAR_DEVICE
#define AESDFILE "/dev/aesdchar"
#include "../aesd-char-driver/aesd_ioctl.h"
#define AESD_IOCTLSEEKTOCMD "AESDCHAR_IOCSEEKTO:"
#define AESD_IOCTLSEEKTOCMD_LEN strlen(AESD_IOCTLSEEKTOCMD)
#else
#define AESDFILE "/var/tmp/aesdsocketdata"
#endif

#define BUFSIZE 4096

volatile sig_atomic_t shutdown_flag = 0;

// start client thread and file
struct file_with_lock {
  pthread_mutex_t file_mut;
  FILE *file;
};

struct file_with_lock *fwl;

void file_with_lock_free(struct file_with_lock *fwl) {
  pthread_mutex_destroy(&fwl->file_mut);
  if (NULL != fwl->file) {
    fclose(fwl->file);
  }
  free(fwl);
}

struct client_node {
  int clientfd;
  struct sockaddr_storage inc_addr;
  socklen_t inc_addr_size;
  char ipstr[INET6_ADDRSTRLEN];
  bool operation_complete;
};

/**
 * client_node_new takes in struct members, allocates space for a new
 * `client_node` struct and assigns the members.
 *
 * This function also calls `inet_ntop` to get the calling ip addr
 */
struct client_node *client_node_new(int clientfd,
                                    struct sockaddr_storage inc_addr,
                                    socklen_t inc_addr_size) {
  struct client_node *c_node = malloc(sizeof(struct client_node));
  if (c_node == NULL) {
    return NULL;
  }
  c_node->clientfd = clientfd;
  c_node->inc_addr = inc_addr;
  c_node->inc_addr_size = inc_addr_size;
  c_node->operation_complete = false;
  struct sockaddr_in *s = (struct sockaddr_in *)&inc_addr;
  inet_ntop(AF_INET, &s->sin_addr, c_node->ipstr, sizeof c_node->ipstr);
  syslog(LOG_INFO, "Accepted connection from %s", c_node->ipstr);

  return c_node;
}

struct client_thread_node {
  struct client_node *client_node;
  pthread_t tid;

  struct client_thread_node *next;
};

// end client thread and file

// linked list functions

/**
 * list_add_to_start takes a pointer to a new `struct client_thread_node`
 * pointer and adds it to the front of the list
 */
void list_add_to_start(struct client_thread_node **head,
                       struct client_thread_node *new_node) {
  // *head doesn't point to anything, set new_node as head
  if (*head == NULL) {
    *head = new_node;
    (*head)->next = NULL;
    return;
  }
  new_node->next = *head;
  *head = new_node;
}

/**
 * list_remove_head takes in a `struct client_thread_node` double pointer
 * to remove the head from the list
 *
 * This function also handles freeing the memory of the removed list entry
 */
void list_remove_head(struct client_thread_node **head) {
  if (*head == NULL) {
    return;
  }

  struct client_thread_node *temp = *head;
  *head = (*head)->next;
  free(temp->client_node);
  free(temp);
}

/**
 * list_remove_using_client_file_node searches the linked list, starting from
 * the head node, looking for a list entry that has the same thread id as the
 * `cft` node
 *
 * This function handles freeing the memory of the removed list entry
 */
void list_remove_using_client_file_node(struct client_thread_node **head,
                                        struct client_thread_node *cft) {
  // empty list
  if (*head == NULL) {
    return;
  }

  struct client_thread_node *temp = *head;
  struct client_thread_node *prev = NULL;

  if (temp != NULL && temp->tid == cft->tid) {
    *head = temp->next;
    free(temp->client_node);
    free(temp);
    return;
  }

  while (temp != NULL && temp->tid != cft->tid) {
    prev = temp;
    temp = temp->next;
  }

  // value not found
  if (temp == NULL) {
    return;
  }

  // found the same thread id, time remove that from the list
  prev->next = temp->next;
  free(temp->client_node);
  free(temp);
}

// end linked list functions

/**
 * handle_connection is a pthread function meant to handle the client connection
 * and write to the AESD file
 *
 * The `void *_node` is cast into a type of `struct client_node`
 */
void *handle_connection(void *_node) {
  struct client_node *node = (struct client_node *)_node;

  // receive messages
  char *buffer = malloc(sizeof(char) * BUFSIZE);
  memset(buffer, 0, sizeof(char) * BUFSIZE);

  int read_bytes = 0;
  char *line = NULL;
  ssize_t read_count;

  while ((read_bytes = recv(node->clientfd, buffer, BUFSIZE, 0)) > 0) {
    syslog(LOG_DEBUG, "buffer read: %s", buffer);
    char *newline_pos = (char *)memchr(buffer, '\n', read_bytes);

    // found a newline in the buffer, write to the file and then
    // send file contents
    if (newline_pos != NULL) {

      pthread_mutex_lock(&(fwl->file_mut));
#if !USE_AESD_CHAR_DEVICE
      if (fwl->file == NULL) {
        syslog(LOG_ERR, "File pointer is NULL");
        pthread_mutex_unlock(&(fwl->file_mut));
        pthread_exit(NULL);
      }
      // the +1 is there to include the newline character from the buffer
      fwrite(buffer, sizeof(char), newline_pos - buffer + 1, fwl->file);
      // fflush is here to force the file to be written and not stored
      // in the kernel buffer
      fflush(fwl->file);

      rewind(fwl->file);

      size_t len = 0;
      while ((read_count = getline(&line, &len, fwl->file)) != -1) {
        // syslog(LOG_INFO, "Sending line %s", line);
        send(node->clientfd, line, read_count, 0);
      }
#else
      int char_dev = open(AESDFILE, O_RDWR);
      // check for the seekto command
      if (strncmp(buffer, AESD_IOCTLSEEKTOCMD, AESD_IOCTLSEEKTOCMD_LEN) == 0) {
        syslog(LOG_INFO, "aesd ioctl cmd found, parsing...");
        struct aesd_seekto seekto;
        sscanf(buffer, AESD_IOCTLSEEKTOCMD "%u,%u", &seekto.write_cmd,
               &seekto.write_cmd_offset);
        if ((ioctl(char_dev, AESDCHAR_IOCSEEKTO, &seekto)) < 0) {
          syslog(LOG_ERR, "error sending seekto cmd over ioctl");
        }
        syslog(LOG_INFO, "parsed ioctl: cmd: %u, cmd_offset: %u",
               seekto.write_cmd, seekto.write_cmd_offset);
        while ((read_count = read(char_dev, buffer, sizeof(buffer))) > 0) {
          // syslog(LOG_INFO, "Sending line to driver: %s", buffer);
          send(node->clientfd, buffer, read_count, 0);
        }
      } else {
        write(char_dev, buffer, newline_pos - buffer + 1);
        lseek(char_dev, 0, SEEK_SET);
        while ((read_count = read(char_dev, buffer, sizeof(buffer))) > 0) {
            // syslog(LOG_INFO, "Sending line to driver: %s", buffer);
            send(node->clientfd, buffer, read_count, 0);
          }
      }
      close(char_dev);
#endif
      pthread_mutex_unlock(&(fwl->file_mut));
    } else {
      // no newline character found, add whole buffer to file
      pthread_mutex_lock(&(fwl->file_mut));
#if !USE_AESD_CHAR_DEVICE
      fwrite(buffer, sizeof(char), read_bytes, fwl->file);
      fflush(fwl->file);
#else
      int char_dev = open(AESDFILE, O_RDWR);
      write(char_dev, buffer, read_bytes);
      close(char_dev);
#endif
      pthread_mutex_unlock(&(fwl->file_mut));
    }
  }

  node->operation_complete = true;
  free(line);
  free(buffer);
  if (read_bytes == 0) {
    syslog(LOG_INFO, "Closed connection from %s", node->ipstr);
  }
  if (read_bytes == -1) {
    syslog(LOG_ERR, "Error reading all bytes from server");
  }
  pthread_exit(NULL);
}

/**
 * handle_timestamp is a pthread function that writes a timestamp string
 * every (10) seconds to the AESD file
 */
void *handle_timestamp() {
  // from strftime man page
  char outstr[BUFSIZE];
  memset(&outstr, 0, BUFSIZE);
  time_t t;
  struct tm *tmp;

  struct timespec wakey;
  clock_gettime(CLOCK_REALTIME, &wakey);

  // this initial sleep call is here because if this thread started first
  // prior to the client socket threads the test would fail as the first
  // line in the temporary file is a timestamp and not a client read
  // sleep(10);

  // global shutdown flag signal
  while (!shutdown_flag) {
    wakey.tv_sec += 10;
    if (shutdown_flag) {
      break;
    }
    clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &wakey, NULL);
    t = time(NULL);
    tmp = localtime(&t);
    if (tmp == NULL) {
      syslog(LOG_ERR, "localtime error");
      pthread_exit(NULL);
    }

    char *rfc_2822 = "timestamp:%a, %d %b %Y %T %z%n";

    if (strftime(outstr, sizeof(outstr), rfc_2822, tmp) == 0) {
      syslog(LOG_ERR, "strftime returned 0");
      pthread_exit(NULL);
    }

    // everything looks ok, write to file
    pthread_mutex_lock(&(fwl->file_mut));
    if (fwl->file == NULL) {
      syslog(LOG_ERR, "File pointer is NULL");
      pthread_mutex_unlock(&(fwl->file_mut));
      pthread_exit(NULL);
    }
    fwrite(outstr, sizeof(char), sizeof(outstr), fwl->file);
    fflush(fwl->file);
    pthread_mutex_unlock(&(fwl->file_mut));
    // sleep(10);
  }

  pthread_exit(NULL);
}

/**
 * raise_shutdown_flag catches the SIG_INT and SIG_TERM signals and changes
 * the `shutdown_flag` to (1), causing the infinite while loops to exit
 * and close the program
 */
void raise_shutdown_flag(int signo) { shutdown_flag = 1; }

void print_usage(void) {
  printf("USAGE for aesdsocket\n");
  printf("aesdsocket [-d]\n");
  printf("OPTIONS:\n");
  printf("\t-d: run aesdsocket as a daemon\n");
}

int main(int argc, char **argv) {

  bool daemon;
  if (argc == 2 && strncmp(argv[1], "-d", 2) == 0) {
    daemon = true;
  } else if (argc == 1) {
    daemon = false;
  } else {
    print_usage();
    return (-1);
  }
  struct sigaction sa = {
      .sa_handler = &raise_shutdown_flag,
      // .sa_mask = {0},
      // .sa_flags = 0
  };
  sigaction(SIGINT, &sa, NULL);
  // avoid signal blocking issues
  sigemptyset(&sa.sa_mask);
  sigaction(SIGTERM, &sa, NULL);

  openlog("aesdsocket", LOG_PID, LOG_USER);
  int sockfd;
  int clientfd;
  struct addrinfo hints;
  struct addrinfo *res;

  // from beej's guide to sockets
  // https://beej.us/guide/bgnet/html/split/system-calls-or-bust.html#system-calls-or-bust

  memset(&hints, 0, sizeof hints);
  // do not care if IPv4 or IPv6
  hints.ai_family = AF_UNSPEC;
  // TCP stream sockets
  hints.ai_socktype = SOCK_STREAM;
  // fill in my IP for me
  hints.ai_flags = AI_PASSIVE;

  int status;
  if ((status = getaddrinfo(NULL, PORT, &hints, &res)) != 0) {
    syslog(LOG_ERR, "Error getting addrinfo");
    closelog();
    return (-1);
  }

  sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (sockfd == -1) {
    syslog(LOG_ERR, "Error on getting socket file descriptor");
    freeaddrinfo(res);
    closelog();
    return (-1);
  }

  // get rid of "Adress already in use" error message
  int yes = 1;
  if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes) == -1) {
    syslog(LOG_ERR, "Error on setsockopt");
    freeaddrinfo(res);
    closelog();
    close(sockfd);
    return (-1);
  }

  if (bind(sockfd, res->ai_addr, res->ai_addrlen) == -1) {
    syslog(LOG_ERR, "Error on binding socket");
    freeaddrinfo(res);
    closelog();
    close(sockfd);
    return (-1);
  }

  // fork here if in daemon mode
  int dev_null;
  if (daemon) {
    pid_t fork_pid = fork();
    if (fork_pid == -1) {
      syslog(LOG_ERR, "Error on creating fork");
      freeaddrinfo(res);
      closelog();
      close(sockfd);
      return (-1);
    }

    // parent process, end here
    if (fork_pid != 0) {
      syslog(LOG_INFO, "Exiting at fork for parent");
      freeaddrinfo(res);
      closelog();
      close(sockfd);
      return (0);
    }

    // if not the parent process, child process will take from here
    chdir("/");

    // redirect stdin/out/err to dev/null
    if ((dev_null = open("dev/null", O_RDWR)) < 0) {
      syslog(LOG_ERR, "Error opening dev/null");
      return (-1);
    }

    if (dup2(dev_null, STDIN_FILENO) == -1) {
      syslog(LOG_ERR, "Error redirecting stdin to dev/null");
      return (-1);
    }
    if (dup2(dev_null, STDOUT_FILENO) == -1) {
      syslog(LOG_ERR, "Error redirecting stdout to dev/null");
      return (-1);
    }
    if (dup2(dev_null, STDERR_FILENO) == -1) {
      syslog(LOG_ERR, "Error redirecting stderr to dev/null");
      return (-1);
    }
  }

  if (listen(sockfd, BACKLOG) == -1) {
    syslog(LOG_ERR, "Error on listen");
    freeaddrinfo(res);
    closelog();
    close(sockfd);
    return (-1);
  }

  // now can accept incoming connections
  fwl = malloc(sizeof(struct file_with_lock));
  fwl->file = NULL;
  if (pthread_mutex_init(&(fwl->file_mut), NULL) < 0) {
    syslog(LOG_ERR, "Error initializing mutex");
    freeaddrinfo(res);
    closelog();
    close(sockfd);
    return (-1);
  }

  // check if the file already exists (bad exit could cause this)
  // and delete it before creating a new one
#if !USE_AESD_CHAR_DEVICE
  FILE *aesd_exists = fopen(AESDFILE, "r");
  if (aesd_exists != NULL) {
    fclose(aesd_exists);
    remove(AESDFILE);
  }

  // create file to read/write to
  fwl->file = fopen(AESDFILE, "a+");
  if (fwl->file == NULL) {
    syslog(LOG_ERR, "Error on opening aesdfile");
    freeaddrinfo(res);
    closelog();
    close(sockfd);
    pthread_mutex_destroy(&(fwl->file_mut));
    return (-1);
  }
#endif

  // start the timestamp thread
  pthread_t ts_thread;

#if !USE_AESD_CHAR_DEVICE
  bool start_timestamp = true;
#else
  bool start_timestamp = false;
#endif

  struct client_thread_node *head = NULL;

  // shutdown_flag is raised when SIGINT or SIGTERM is raised
  // this way the while loop has a way to exit
  while (!shutdown_flag) {
    struct sockaddr_storage inc_addr;
    socklen_t inc_addr_size = sizeof inc_addr;
    clientfd = accept(sockfd, (struct sockaddr *)&inc_addr, &inc_addr_size);
    if (clientfd == -1) {
      if (shutdown_flag)
        break;
      syslog(LOG_ERR, "Error on accepting client");
      continue; // continue trying to accept clients
    }

    if (start_timestamp) {
      pthread_create(&ts_thread, NULL, handle_timestamp, NULL);
      start_timestamp = false;
    }

    struct client_node *c_node =
        client_node_new(clientfd, inc_addr, inc_addr_size);
    if (c_node == NULL) {
      break;
    }

    pthread_t tid;
    pthread_create(&tid, NULL, handle_connection, (void *)c_node);

    struct client_thread_node *ct_node =
        malloc(sizeof(struct client_thread_node));
    if (ct_node == NULL) {
      break;
    }
    ct_node->client_node = c_node;
    ct_node->tid = tid;
    ct_node->next = NULL;

    list_add_to_start(&head, ct_node);

    struct client_thread_node *current = head;

    while (current != NULL) {
      if (current->client_node->operation_complete == true) {
        pthread_t join_id = current->tid;
        syslog(LOG_INFO, "Removing thread with id %lu", join_id);

        shutdown(current->client_node->clientfd, SHUT_RDWR);
        pthread_join(join_id, NULL);
        struct client_thread_node *temp = current->next;
        list_remove_using_client_file_node(&head, current);
        current = temp;
      } else {
        current = current->next;
      }
    }
  }

  syslog(LOG_INFO, "Cleaning up, exit signal caught");
  // cleanup any remaining threads
  while (head != NULL) {
    pthread_cancel(head->tid);
    pthread_join(head->tid, NULL);
    shutdown(head->client_node->clientfd, SHUT_RDWR);

    struct client_thread_node *temp = head->next;
    list_remove_head(&head);
    head = temp;
  }

#if !USE_AESD_CHAR_DEVICE
  pthread_cancel(ts_thread);
  pthread_join(ts_thread, NULL);
#endif

  freeaddrinfo(res);
  shutdown(sockfd, SHUT_RDWR);
  closelog();
  file_with_lock_free(fwl);
#if !USE_AESD_CHAR_DEVICE
  // if the character device is being used, the device file should not be
  // deleted
  // otherwise, delete the temporary file
  remove(AESDFILE);
#endif
  if (daemon) {
    close(dev_null);
  }
  return (0);
}