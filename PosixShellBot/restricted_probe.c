/* Deliberately tiny, uninstrumented executable: tests the actual sandbox,
   without granting the main application's GUI or sanitizer runtime access. */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
int main(int argc, char **argv) {
    if (argc != 2 || geteuid() == 0)
        return 2;
    int fd = open(argv[1], O_RDONLY);
    if (fd >= 0) {
        close(fd);
        return 3;
    }
    if (errno != EACCES && errno != EPERM && errno != ENOENT)
        return 4;
    pid_t child = fork();
    if (child == 0)
        _exit(0);
    if (child > 0) {
        waitpid(child, NULL, 0);
        return 5;
    }
    if (errno != EPERM && errno != EACCES)
        return 6;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons(9);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int rc = connect(fd, (struct sockaddr *)&a, sizeof(a));
        int saved = errno;
        close(fd);
        if (rc == 0 || (saved != EPERM && saved != EACCES))
            return 7;
    } else if (errno != EPERM && errno != EACCES)
        return 8;
    return 0;
}
