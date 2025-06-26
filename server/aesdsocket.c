#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <syslog.h>
#include <sys/stat.h>
#include <pthread.h>
#include <sys/queue.h>
#include <time.h>

#ifndef USE_AESD_CHAR_DEVICE
# define USE_AESD_CHAR_DEVICE 1
#endif

#define PORT					(9000)
#define BACKLOG					(50)
#if USE_AESD_CHAR_DEVICE
# include "../aesd-char-driver/aesd_ioctl.h"
# define AESD_IOCTL_CMD			"AESDCHAR_IOCSEEKTO:"
# define DATA_FILE				"/dev/aesdchar"
#else
# define DATA_FILE				"/var/tmp/aesdsocketdata"
#endif
#define BUFFER_SIZE				(1024)
#define TIMESTAMP_INTERVAL_SEC	(10)
#define TIMESTAMP_FORMAT		"%a, %d %b %Y %T %z\n"

volatile sig_atomic_t server_stop = 0;
pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

int sockfd = -1;
#if !USE_AESD_CHAR_DEVICE
FILE* fp = NULL;
timer_t timerid;
struct sigevent sev;
struct itimerspec its;
#endif

typedef struct thread_node {
    pthread_t thread_id;
    int client_fd;
    SLIST_ENTRY(thread_node) entries;
} thread_node_t;

SLIST_HEAD(thread_list_head, thread_node);
struct thread_list_head thread_list = SLIST_HEAD_INITIALIZER(thread_list);

void signal_handler(int signo) {
    syslog(LOG_INFO, "Caught signal, exiting");
    server_stop = 1;
    if (sockfd != -1) {
        shutdown(sockfd, SHUT_RDWR); 
    }
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
        char *newline_pos = strchr(buffer, '\n');
        if (!newline_pos) {
            packet = realloc(packet, total_len + bytes_received + 1);
            if (!packet) break;
            memcpy(packet + total_len, buffer, bytes_received + 1);
            total_len += bytes_received;
            continue;
        }

        size_t chunk_len = newline_pos - buffer + 1;
        packet = realloc(packet, total_len + chunk_len + 1);
        if (!packet) break;
        memcpy(packet + total_len, buffer, chunk_len);
        total_len += chunk_len;
        packet[total_len] = '\0';

        pthread_mutex_lock(&file_mutex);
#if USE_AESD_CHAR_DEVICE
		int fd = open(DATA_FILE, O_RDWR);
		if (fd == -1) {
		    syslog(LOG_ERR, "open error: %s", strerror(errno));
		    break;
		}

		if (strncmp(packet, AESD_IOCTL_CMD, strlen(AESD_IOCTL_CMD)) == 0) {
			struct aesd_seekto seekto;
			if (sscanf(packet, AESD_IOCTL_CMD "%u,%u", &seekto.write_cmd, &seekto.write_cmd_offset) == 2) {
				if (ioctl(fd, AESDCHAR_IOCSEEKTO, &seekto) < 0) {
					syslog(LOG_ERR, "ioctl error: %s", strerror(errno));
				}
			}
		}
		else {
			ssize_t write_len = write(fd, packet, total_len);
			if (write_len < 0) {
				syslog(LOG_ERR, "write to /dev/aesdchar failed: %s", strerror(errno));
				close(fd);
				break;
			}
		}
		
		size_t read_len;
		while ((read_len = read(fd, buffer, sizeof(buffer))) > 0) {
			if (send(clientfd, buffer, read_len, 0) < 0) {
				syslog(LOG_ERR, "send to client failed: %s", strerror(errno));
				close(fd);
				break;
			}
		}
		if (read_len < 0) {
			syslog(LOG_ERR, "read from /dev/aesdchar failed: %s", strerror(errno));
		}
		close(fd);
#else
        fwrite(packet, 1, total_len, fp);
        fflush(fp);

        fseek(fp, 0, SEEK_SET);
        while (!feof(fp)) {
            size_t read_len = fread(buffer, 1, sizeof(buffer), fp);
            if (read_len > 0) send(clientfd, buffer, read_len, 0);
        }
 #endif
        pthread_mutex_unlock(&file_mutex);

        free(packet);
        packet = NULL;
        total_len = 0;
    }

    if (packet) free(packet);
    close(clientfd);
    return NULL;
}

#if !USE_AESD_CHAR_DEVICE
void timestamp_callback(union sigval arg) {
    time_t now;
    struct tm *timeinfo;
    char timestamp[100];

    time(&now);
    timeinfo = localtime(&now);
    strftime(timestamp, sizeof(timestamp), TIMESTAMP_FORMAT, timeinfo);

    pthread_mutex_lock(&file_mutex);

    if (fp) {
        fprintf(fp, "timestamp: %s", timestamp);
        fflush(fp);
    }

    pthread_mutex_unlock(&file_mutex);
}
#endif

int main(int argc, char *argv[]) {
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    char client_ip[INET_ADDRSTRLEN];
    int result = -1;
    int as_daemon = 0;

    if (argc == 2 && strcmp(argv[1], "-d") == 0) {
        as_daemon = 1;
    }

    openlog("aesdsocket", LOG_PID, LOG_USER);
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

#if !USE_AESD_CHAR_DEVICE
    fp = fopen(DATA_FILE, "a+");
    if (!fp) {
        syslog(LOG_ERR, "Failed to open file: %s", strerror(errno));
        goto cleanup;
    }
#endif
    
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {
        syslog(LOG_ERR, "Socket creation failed: %s", strerror(errno));
        goto cleanup;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(PORT);

    if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) != 0) {
        syslog(LOG_ERR, "Bind failed: %s", strerror(errno));
        goto cleanup;
    }

    if (as_daemon) {
        pid_t pid = fork();
        if (pid < 0) exit(-1);
        if (pid > 0) exit(EXIT_SUCCESS);
        umask(0);
        setsid();
        chdir("/");
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
    }

#if !USE_AESD_CHAR_DEVICE
    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_THREAD;
    sev.sigev_notify_function = timestamp_callback;
    sev.sigev_value.sival_ptr = &timerid;
    
    if (timer_create(CLOCK_MONOTONIC, &sev, &timerid) == -1) {
        syslog(LOG_ERR, "timer_create failed");
        goto cleanup;
    }

    its.it_value.tv_sec = TIMESTAMP_INTERVAL_SEC;
    its.it_value.tv_nsec = 0;
    its.it_interval.tv_sec = TIMESTAMP_INTERVAL_SEC;
    its.it_interval.tv_nsec = 0;
    
    if (timer_settime(timerid, 0, &its, NULL) == -1) {
        syslog(LOG_ERR, "timer_settime failed");
        goto cleanup;
    }
#endif

    if (listen(sockfd, BACKLOG) != 0) {
        syslog(LOG_ERR, "Listen failed: %s", strerror(errno));
        goto cleanup;
    }

    while (!server_stop) {
        int *clientfd_ptr = malloc(sizeof(int));
        if (!clientfd_ptr) continue;

        *clientfd_ptr = accept(sockfd, (struct sockaddr *)&client_addr, &client_addr_len);
        if (*clientfd_ptr == -1) {
            free(clientfd_ptr);
            if (errno == EINTR || server_stop) break;
            syslog(LOG_ERR, "Accept failed: %s", strerror(errno));
            continue;
        }

        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        syslog(LOG_INFO, "Accepted connection from %s", client_ip);

        thread_node_t *new_node = malloc(sizeof(thread_node_t));
        if (!new_node) {
            close(*clientfd_ptr);
            free(clientfd_ptr);
            continue;
        }

        new_node->client_fd = *clientfd_ptr;
        pthread_create(&new_node->thread_id, NULL, client_handler, clientfd_ptr);
        SLIST_INSERT_HEAD(&thread_list, new_node, entries);
    }

    // Join all threads
    thread_node_t *np;
    while (!SLIST_EMPTY(&thread_list)) {
        np = SLIST_FIRST(&thread_list);
        pthread_join(np->thread_id, NULL);
        SLIST_REMOVE_HEAD(&thread_list, entries);
        free(np);
    }

    result = 0;

cleanup:
    if (sockfd != -1) close(sockfd);
#if !USE_AESD_CHAR_DEVICE
	timer_delete(timerid);
    if (fp) fclose(fp);
    remove(DATA_FILE);
#endif
    closelog();
    
    return result;
}
