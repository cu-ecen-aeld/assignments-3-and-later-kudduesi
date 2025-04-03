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

#define PORT 9000
#define BUF_SIZE 1024
#define DATA_FILE "/var/tmp/aesdsocketdata"

volatile sig_atomic_t stop_server = 0;
pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

void signal_handler(int sig){
    stop_server = 1;
    syslog(LOG_INFO, "Caught signal, exiting");
}

int write_client_data(int client_fd){
    char *buffer = NULL;
    size_t buf_size = 0;
    ssize_t num_bytes;
    int file_fd;

    file_fd = open(DATA_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if(file_fd < 0){
        syslog(LOG_ERR, "Failed to open %s: %s", DATA_FILE, strerror(errno));
        return -1;
    }

    while(1){
        char temp[BUF_SIZE];
        num_bytes = recv(client_fd, temp, BUF_SIZE, 0);
        if(num_bytes < 0){
            syslog(LOG_ERR, "Recv failed: %s", strerror(errno));
            free(buffer);
            close(file_fd);
            return -1;
        } 
        else if(num_bytes == 0){
            break;
        }
        char *new_buf = realloc(buffer, buf_size + num_bytes);
        if(new_buf == NULL){
            syslog(LOG_ERR, "Memory allocation failed");
            free(buffer);
            close(file_fd);
            return -1;
        }
        buffer = new_buf;
        memcpy(buffer + buf_size, temp, num_bytes);
        buf_size += num_bytes;
        if(memchr(buffer, '\n', buf_size) != NULL){
            break;
        }
    }

    if(write(file_fd, buffer, buf_size) < 0){
        syslog(LOG_ERR, "Write failed: %s", strerror(errno));
        free(buffer);
        close(file_fd);
        return -1;
    }

    free(buffer);
    close(file_fd);
    return 0;
}

int read_client_data(int client_fd){
    int read_fd = open(DATA_FILE, O_RDONLY);
    if(read_fd < 0){
        syslog(LOG_ERR, "Failed to open %s for reading: %s", DATA_FILE, strerror(errno));
        return -1;
    }
    char send_buf[BUF_SIZE];
    ssize_t bytes_read;
    while((bytes_read = read(read_fd, send_buf, BUF_SIZE)) > 0){
        ssize_t bytes_sent = 0;
        while(bytes_sent < bytes_read){
            ssize_t ret = send(client_fd, send_buf + bytes_sent, bytes_read - bytes_sent, 0);
            if(ret < 0){
                syslog(LOG_ERR, "send() failed: %s", strerror(errno));
                close(read_fd);
                return -1;
            }
            bytes_sent += ret;
        }
    }
    close(read_fd);
    return 0;
}

int handle_client_data(int client_fd){
    if(write_client_data(client_fd) < 0)
        return -1;
    if(read_client_data(client_fd) < 0)
        return -1;
    return 0;
}

struct thread_info{
    pthread_t thread_id;
    int client_fd;
    struct thread_info *next;
};

struct thread_info *thread_list = NULL;
pthread_mutex_t list_mutex = PTHREAD_MUTEX_INITIALIZER;

void add_thread_info(struct thread_info *node){
    pthread_mutex_lock(&list_mutex);
    node->next = thread_list;
    thread_list = node;
    pthread_mutex_unlock(&list_mutex);
}

void *thread_func(void *arg){
    struct thread_info *tinfo =(struct thread_info *)arg;
    pthread_mutex_lock(&file_mutex);
    handle_client_data(tinfo->client_fd);
    pthread_mutex_unlock(&file_mutex);
    close(tinfo->client_fd);
    return arg;
}

void *timestamp_thread(void *arg){
    char ts_buf[256];
    while(!stop_server){
        sleep(10);
        time_t now = time(NULL);
        struct tm tm_info;
        localtime_r(&now, &tm_info);
        strftime(ts_buf, sizeof(ts_buf), "timestamp:%a, %d %b %Y %T %z\n", &tm_info);
        pthread_mutex_lock(&file_mutex);
        int file_fd = open(DATA_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if(file_fd >= 0){
            write(file_fd, ts_buf, strlen(ts_buf));
            close(file_fd);
        } 
        else{
            syslog(LOG_ERR, "Failed to open %s for timestamp: %s", DATA_FILE, strerror(errno));
        }
        pthread_mutex_unlock(&file_mutex);
    }
    return NULL;
}

int main(int argc, char *argv[]){
    bool daemon_mode = false;
    if(argc > 1 && strcmp(argv[1], "-d") == 0)
        daemon_mode = true;
    struct sockaddr_in server, client;
    int socketfd, acceptfd;
    socklen_t client_len;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sa.sa_flags = 0;
    if(sigaction(SIGINT, &sa, NULL) < 0){
        perror("sigaction(SIGINT) failed"); 
        exit(EXIT_FAILURE);
    }
    if(sigaction(SIGTERM, &sa, NULL) < 0){
        perror("sigaction(SIGTERM) failed"); 
        exit(EXIT_FAILURE);
    }
    openlog("aesdsocket", LOG_PID | LOG_CONS, LOG_USER);
    socketfd = socket(AF_INET, SOCK_STREAM, 0);
    if(socketfd < 0){ 
        syslog(LOG_ERR, "Error creating socket: %s", strerror(errno)); 
        return -1; 
    }

    int yes = 1;
    if(setsockopt(socketfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0){
        syslog(LOG_ERR, "setsockopt failed: %s", strerror(errno));
        close(socketfd);
        return -1;
    }

    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = INADDR_ANY;
    server.sin_port = htons(PORT);
    if(bind(socketfd,(struct sockaddr *)&server, sizeof(server)) < 0){ 
        syslog(LOG_ERR, "Binding failed: %s", strerror(errno)); 
        close(socketfd); 
        return -1;
    }
    if(listen(socketfd, 5) < 0){ 
        syslog(LOG_ERR, "Listening failed: %s", strerror(errno)); 
        close(socketfd); 
        return -1;
    }
    if(daemon_mode){
        pid_t pid = fork();
        if(pid < 0){ 
            syslog(LOG_ERR, "Fork failed: %s", strerror(errno)); 
            close(socketfd); 
            return -1; 
        }
        if(pid > 0)
            exit(EXIT_SUCCESS);
        if(setsid() < 0){ 
            syslog(LOG_ERR, "setsid() failed: %s", strerror(errno)); 
            close(socketfd); 
            return -1; 
        }
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
    }

    pthread_t ts_thread;
    if(pthread_create(&ts_thread, NULL, timestamp_thread, NULL) != 0){
        syslog(LOG_ERR, "Failed to create timestamp thread: %s", strerror(errno));
        close(socketfd);
        return -1;
    }
    
    while(!stop_server){
        client_len = sizeof(client);
        acceptfd = accept(socketfd,(struct sockaddr *)&client, &client_len);
        if(acceptfd < 0){
            if(errno == EINTR && stop_server)
                break;
            syslog(LOG_ERR, "Accept failed: %s", strerror(errno));
            close(socketfd);
            return -1;
        }
        syslog(LOG_INFO, "Accepted connection from %s", inet_ntoa(client.sin_addr));
        struct thread_info *node = malloc(sizeof(struct thread_info));
        if(!node){ 
            syslog(LOG_ERR, "Memory allocation failed for thread info"); 
            close(acceptfd); 
            continue;
        }
        node->client_fd = acceptfd;
        node->next = NULL;
        if(pthread_create(&node->thread_id, NULL, thread_func, node) != 0){
            syslog(LOG_ERR, "pthread_create failed: %s", strerror(errno));
            close(acceptfd);
            free(node);
            continue;
        }
        add_thread_info(node);
        syslog(LOG_INFO, "Spawned thread for connection from %s", inet_ntoa(client.sin_addr));
    }
    struct thread_info *curr = thread_list;
    while(curr){
        pthread_join(curr->thread_id, NULL);
        struct thread_info *tmp = curr;
        curr = curr->next;
        free(tmp);
    }
    pthread_cancel(ts_thread);
    pthread_join(ts_thread, NULL);
    close(socketfd);
    if(unlink(DATA_FILE) < 0)
        syslog(LOG_ERR, "Failed to delete %s: %s", DATA_FILE, strerror(errno));
    else
        syslog(LOG_INFO, "Deleted %s", DATA_FILE);
    closelog();
    return 0;
}