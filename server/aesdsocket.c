#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <syslog.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>
#include <sys/queue.h>
#include "../aesd-char-driver/aesd_ioctl.h"

#define AESD_IOCTL_CMD     "AESDCHAR_IOCSEEKTO:"
#define PORT               9000
#define BACKLOG            5
#define BUFFER_SIZE        1024

#ifndef USE_AESD_CHAR_DEVICE
#define USE_AESD_CHAR_DEVICE 1
#endif

#if USE_AESD_CHAR_DEVICE
#define STORAGE_PATH "/dev/aesdchar"
#else
#define STORAGE_PATH "/var/tmp/aesdsocketdata"
#endif

volatile sig_atomic_t stop_server = 0;
pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

struct thread_info {
    pthread_t thread_id;
    int client_fd;
    SLIST_ENTRY(thread_info) entries;
};
SLIST_HEAD(thread_list_head, thread_info) thread_list = SLIST_HEAD_INITIALIZER(thread_list);

void signal_handler(int sig) {
    syslog(LOG_INFO, "Caught signal, exiting");
    stop_server = 1;
}

void* client_handler(void *arg) {
    int clientfd = *(int*)arg;
    free(arg);

    char buffer[BUFFER_SIZE];
    ssize_t bytes_received;
    size_t total_len = 0;
    char *packet = NULL;

    while ((bytes_received = recv(clientfd, buffer, sizeof(buffer)-1, 0)) > 0) {
        buffer[bytes_received] = '\0';
        char *newline = strchr(buffer, '\n');
        if (!newline) {
            packet = realloc(packet, total_len + bytes_received + 1);
            if (!packet) break;
            memcpy(packet + total_len, buffer, bytes_received);
            total_len += bytes_received;
            continue;
        }

        size_t chunk_len = newline - buffer + 1;
        packet = realloc(packet, total_len + chunk_len + 1);
        if (!packet) break;
        memcpy(packet + total_len, buffer, chunk_len);
        total_len += chunk_len;
        packet[total_len] = '\0';

        pthread_mutex_lock(&file_mutex);

#if USE_AESD_CHAR_DEVICE
        int fd = open(STORAGE_PATH, O_RDWR | O_CREAT, 0644);
        if (fd < 0) {
            syslog(LOG_ERR, "open(%s) failed: %s", STORAGE_PATH, strerror(errno));
            pthread_mutex_unlock(&file_mutex);
            break;
        }

        if (strncmp(packet, AESD_IOCTL_CMD, strlen(AESD_IOCTL_CMD)) == 0) {
            struct aesd_seekto seekto;
            if (sscanf(packet,
                       AESD_IOCTL_CMD "%u,%u",
                       &seekto.write_cmd,
                       &seekto.write_cmd_offset) == 2) {
                if (ioctl(fd, AESDCHAR_IOCSEEKTO, &seekto) < 0) {
                    syslog(LOG_ERR, "ioctl() failed: %s", strerror(errno));
                }
            } else {
                syslog(LOG_ERR, "Malformed IOCSEEKTO cmd: %.*s",
                       (int)total_len, packet);
            }
        } else {
            ssize_t wlen = write(fd, packet, total_len);
            if (wlen < 0) {
                syslog(LOG_ERR, "write(%s) failed: %s", STORAGE_PATH, strerror(errno));
                close(fd);
                pthread_mutex_unlock(&file_mutex);
                break;
            }
        }

        {
            ssize_t rd;
            while ((rd = read(fd, buffer, sizeof(buffer))) > 0) {
                ssize_t sent = 0;
                while (sent < rd) {
                    ssize_t s = send(clientfd, buffer + sent, rd - sent, 0);
                    if (s < 0) {
                        syslog(LOG_ERR, "send() failed: %s", strerror(errno));
                        break;
                    }
                    sent += s;
                }
            }
            if (rd < 0) {
                syslog(LOG_ERR, "read(%s) failed: %s", STORAGE_PATH, strerror(errno));
            }
        }

        close(fd);

#else

        int fd = open(STORAGE_PATH, O_RDWR | O_CREAT | O_APPEND, 0644);
        if (fd < 0) {
            syslog(LOG_ERR, "open(%s) failed: %s", STORAGE_PATH, strerror(errno));
            pthread_mutex_unlock(&file_mutex);
            break;
        }
        write(fd, packet, total_len);
        lseek(fd, 0, SEEK_SET);
        ssize_t rd;
        while ((rd = read(fd, buffer, sizeof(buffer))) > 0) {
            send(clientfd, buffer, rd, 0);
        }
        close(fd);
#endif

        pthread_mutex_unlock(&file_mutex);
        free(packet);
        packet = NULL;
        total_len = 0;
    }

    free(packet);
    close(clientfd);
    return NULL;
}

int main(int argc, char *argv[]) {
    bool daemon_mode = false;
    if (argc > 1 && strcmp(argv[1], "-d") == 0) {
        daemon_mode = true;
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    openlog("aesdsocket", LOG_PID | LOG_CONS, LOG_USER);

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        syslog(LOG_ERR, "socket() failed: %s", strerror(errno));
        return -1;
    }

    int yes = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in serv = {0};
    serv.sin_family = AF_INET;
    serv.sin_addr.s_addr = INADDR_ANY;
    serv.sin_port = htons(PORT);

    if (bind(sockfd, (struct sockaddr*)&serv, sizeof(serv)) < 0) {
        syslog(LOG_ERR, "bind() failed: %s", strerror(errno));
        close(sockfd);
        return -1;
    }

    if (listen(sockfd, BACKLOG) < 0) {
        syslog(LOG_ERR, "listen() failed: %s", strerror(errno));
        close(sockfd);
        return -1;
    }

    if (daemon_mode) {
        pid_t pid = fork();
        if (pid > 0) exit(EXIT_SUCCESS);
        setsid();
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
    }

    SLIST_INIT(&thread_list);
    while (!stop_server) {
        int *clientfd_ptr = malloc(sizeof(int));
        if (!clientfd_ptr) continue;
        *clientfd_ptr = accept(sockfd, NULL, NULL);
        if (*clientfd_ptr < 0) {
            free(clientfd_ptr);
            if (stop_server) break;
            continue;
        }
        struct thread_info *node = malloc(sizeof(*node));
        node->client_fd = *clientfd_ptr;
        pthread_create(&node->thread_id, NULL, client_handler, clientfd_ptr);
        SLIST_INSERT_HEAD(&thread_list, node, entries);
    }

    struct thread_info *np;
    while (!SLIST_EMPTY(&thread_list)) {
        np = SLIST_FIRST(&thread_list);
        pthread_join(np->thread_id, NULL);
        SLIST_REMOVE_HEAD(&thread_list, entries);
        free(np);
    }

    close(sockfd);
    closelog();
    return 0;
}
