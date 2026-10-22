#include "posix.h"
#include <sys/socket.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <spawn.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <dirent.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
extern char **environ;
static char program[SB_PATH_CAP];
void sb_posix_set_program(const char *argv0) {
#ifdef __APPLE__
    char path[SB_PATH_CAP];
    uint32_t size = sizeof(path);
    if (!_NSGetExecutablePath(path, &size) && realpath(path, program))
        return;
#else
    ssize_t n = readlink("/proc/self/exe", program, sizeof(program) - 1);
    if (n > 0 && (size_t)n < sizeof(program) - 1) {
        program[n] = 0;
        return;
    }
#endif
    if (!realpath(argv0, program))
        program[0] = 0;
}
const char *sb_posix_program(void) { return program; }
void sb_posix_close_inherited(void) {
    DIR *directory = opendir("/dev/fd");
    if (directory) {
        struct dirent *entry;
        int listing = dirfd(directory);
        while ((entry = readdir(directory))) {
            char *end;
            long fd = strtol(entry->d_name, &end, 10);
            if (!*end && fd > 3 && fd != listing)
                close((int)fd);
        }
        closedir(directory);
    } else {
        long maximum = sysconf(_SC_OPEN_MAX);
        for (int fd = 4; fd < maximum; fd++)
            close(fd);
    }
}
bool sb_posix_peer(int fd, bool parent) {
#ifdef __APPLE__
    uid_t uid;
    gid_t gid;
    return (!parent || getppid() > 1) && !getpeereid(fd, &uid, &gid) && uid == geteuid();
#else
    struct ucred u;
    socklen_t size = sizeof(u);
    return !getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &u, &size) && u.uid == geteuid() &&
           (!parent || u.pid == getppid());
#endif
}
static bool transfer(int fd, void *bytes, size_t length, bool writing, uint64_t deadline, sb_error *e) {
    unsigned char *at = bytes;
    while (length) {
        uint64_t now = sb_platform_millis();
        if (now >= deadline) {
            sb_fail(e, "Private IPC timed out.");
            return false;
        }
        struct pollfd p = {.fd = fd, .events = writing ? POLLOUT : POLLIN};
        int timeout = (int)(deadline - now);
        if (timeout > 100)
            timeout = 100;
        int rc = poll(&p, 1, timeout);
        if (rc < 0 && errno == EINTR)
            continue;
        if (rc < 0) {
            sb_posix_error(e, "Poll private IPC");
            return false;
        }
        if (!rc)
            continue;
        ssize_t n = writing ? send(fd, at, length, MSG_DONTWAIT) : recv(fd, at, length, MSG_DONTWAIT);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (n <= 0) {
            sb_fail(e, "Private IPC peer exited or closed its channel.");
            return false;
        }
        at += n;
        length -= (size_t)n;
    }
    return true;
}
char *sb_posix_frame_read(int fd, unsigned timeout, sb_error *e) {
    uint32_t network;
    uint64_t deadline = sb_platform_millis() + timeout;
    if (!transfer(fd, &network, sizeof(network), false, deadline, e))
        return NULL;
    uint32_t length = ntohl(network);
    if (!length || length > 8u * 1024u * 1024u) {
        sb_fail(e, "Invalid private IPC frame size.");
        return NULL;
    }
    char *bytes = malloc((size_t)length + 1);
    if (!bytes) {
        sb_fail(e, "Out of memory.");
        return NULL;
    }
    if (!transfer(fd, bytes, length, false, deadline, e)) {
        free(bytes);
        return NULL;
    }
    bytes[length] = 0;
    return bytes;
}
bool sb_posix_frame_write(int fd, const char *bytes, unsigned timeout, sb_error *e) {
    size_t length = bytes ? strlen(bytes) : 0;
    if (!length || length > 8u * 1024u * 1024u) {
        sb_fail(e, "Invalid private IPC frame size.");
        return false;
    }
    uint32_t network = htonl((uint32_t)length);
    uint64_t deadline = sb_platform_millis() + timeout;
    return transfer(fd, &network, sizeof(network), true, deadline, e) &&
           transfer(fd, (void *)bytes, length, true, deadline, e);
}
bool sb_posix_spawn_channel(char *const argv[], int *channel, pid_t *pid, sb_error *e) {
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) {
        sb_posix_error(e, "Create worker channel");
        return false;
    }
    fcntl(pair[0], F_SETFD, FD_CLOEXEC);
    fcntl(pair[1], F_SETFD, FD_CLOEXEC);
    /* Reserve fd 3 as the sole inherited private channel. */
    if (pair[1] == 3) {
        int replacement = fcntl(pair[1], F_DUPFD_CLOEXEC, 4);
        if (replacement < 0) {
            close(pair[0]);
            close(pair[1]);
            return false;
        }
        close(pair[1]);
        pair[1] = replacement;
    }
    posix_spawn_file_actions_t files;
    posix_spawnattr_t attr;
    posix_spawn_file_actions_init(&files);
    posix_spawnattr_init(&attr);
    int rc = posix_spawn_file_actions_adddup2(&files, pair[1], 3);
    if (!rc && pair[0] != 3)
        rc = posix_spawn_file_actions_addclose(&files, pair[0]);
    if (!rc)
        rc = posix_spawn_file_actions_addclose(&files, pair[1]);
    for (int fd = 0; fd < 2 && !rc; fd++)
        rc = posix_spawn_file_actions_addopen(&files, fd, "/dev/null", fd ? O_WRONLY : O_RDONLY, 0);
    /* Keep diagnostics on the owner's stderr, including native sanitizer
       reports. Protocol output and command output still use private channels. */
    if (!rc)
        rc = fcntl(2, F_GETFD) >= 0 ? posix_spawn_file_actions_adddup2(&files, 2, 2)
                                    : posix_spawn_file_actions_addopen(&files, 2, "/dev/null", O_WRONLY, 0);
    sigset_t empty, defaults;
    sigemptyset(&empty);
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGINT);
    sigaddset(&defaults, SIGTERM);
    sigaddset(&defaults, SIGPIPE);
    posix_spawnattr_setsigmask(&attr, &empty);
    posix_spawnattr_setsigdefault(&attr, &defaults);
    posix_spawnattr_setpgroup(&attr, 0);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    if (!rc)
        rc = posix_spawn(pid, program, &files, &attr, argv, environ);
    posix_spawn_file_actions_destroy(&files);
    posix_spawnattr_destroy(&attr);
    close(pair[1]);
    if (rc) {
        close(pair[0]);
        errno = rc;
        sb_posix_error(e, "Spawn native worker");
        return false;
    }
    *channel = pair[0];
    return true;
}
bool sb_posix_wait_child(pid_t pid, unsigned timeout, int *status) {
    uint64_t end = sb_platform_millis() + timeout;
    do {
        pid_t result = waitpid(pid, status, WNOHANG);
        if (result == pid)
            return true;
        if (result < 0 && errno != EINTR)
            return errno == ECHILD;
        struct timespec pause = {.tv_nsec = 10000000};
        nanosleep(&pause, NULL);
    } while (sb_platform_millis() < end);
    return false;
}
char *sb_posix_json(yyjson_mut_doc *doc, yyjson_mut_val *root) {
    if (!doc)
        return NULL;
    yyjson_mut_doc_set_root(doc, root);
    char *bytes = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    return bytes;
}
char *sb_posix_json_error(const char *message) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (!d)
        return NULL;
    yyjson_mut_val *v = yyjson_mut_obj(d);
    yyjson_mut_obj_add_bool(d, v, "_error", true);
    yyjson_mut_obj_add_strcpy(d, v, "error", message);
    return sb_posix_json(d, v);
}
