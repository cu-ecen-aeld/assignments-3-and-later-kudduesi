#include <stdio.h>
#include <syslog.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

int main(int argc, char *argv[]) {
    openlog("writer-app", LOG_PID, LOG_USER);

    if (argc != 3) {
        syslog(LOG_ERR, "Usage: %s <writefile> <writestr>", argv[0]);
        fprintf(stderr, "Usage: %s <writefile> <writestr>\n", argv[0]);
        closelog();
        return 1;
    }

    const char *writefile = argv[1];
    const char *writestr = argv[2];

    int fd = open(writefile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) {
        syslog(LOG_ERR, "File %s could not be opened or created", writefile);
        perror("Error opening file");
        closelog();
        return 1;
    }

    ssize_t nr = write(fd, writestr, strlen(writestr));
    if (nr == -1) {
        syslog(LOG_ERR, "Could not write to file %s", writefile);
        perror("Error writing to file");
        close(fd);
        closelog();
        return 1;
    }

    syslog(LOG_DEBUG, "Writing '%s' to '%s'", writestr, writefile);
    printf("Successfully wrote to the file %s\n", writefile);

    close(fd);
    closelog();

    return 0;
}
