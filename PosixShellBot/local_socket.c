#include "local_socket.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __APPLE__
#define SB_LOCAL_BASE "/private/tmp"
#else
#define SB_LOCAL_BASE "/tmp"
#endif
#define SB_LOCAL_LEAF "channel.sock"

struct sb_local_socket {
    sb_root *base;
    int directory;
    bool creator, bound;
    char token[SB_LOCAL_SOCKET_TOKEN_LENGTH + 1], name[96];
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    struct stat base_identity, directory_identity, socket_identity;
};

static bool same(const struct stat *a, const struct stat *b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino;
}
static bool snapshot(const struct stat *a, const struct stat *b) {
    if (!same(a, b) || a->st_mode != b->st_mode || a->st_uid != b->st_uid ||
        a->st_gid != b->st_gid || a->st_nlink != b->st_nlink || a->st_size != b->st_size)
        return false;
#ifdef __APPLE__
    return a->st_mtimespec.tv_sec == b->st_mtimespec.tv_sec &&
        a->st_mtimespec.tv_nsec == b->st_mtimespec.tv_nsec &&
        a->st_ctimespec.tv_sec == b->st_ctimespec.tv_sec &&
        a->st_ctimespec.tv_nsec == b->st_ctimespec.tv_nsec;
#else
    return a->st_mtim.tv_sec == b->st_mtim.tv_sec &&
        a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
        a->st_ctim.tv_sec == b->st_ctim.tv_sec &&
        a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
#endif
}
static bool base_private(const struct stat *st) {
    return S_ISDIR(st->st_mode) && st->st_uid == 0 &&
        (!(st->st_mode & 0022) || (st->st_mode & S_ISVTX));
}
static bool directory_private(const struct stat *st) {
    return S_ISDIR(st->st_mode) && st->st_uid == geteuid() && (st->st_mode & 07777) == 0700;
}
static bool socket_owned(const struct stat *st) {
    return S_ISSOCK(st->st_mode) && st->st_uid == geteuid() && st->st_nlink == 1;
}
static bool socket_private(const struct stat *st) {
    return socket_owned(st) && (st->st_mode & 07777) == 0600;
}
bool sb_local_socket_token_valid(const char *token) {
    if (!token || strlen(token) != SB_LOCAL_SOCKET_TOKEN_LENGTH) return false;
    for (size_t i = 0; i < SB_LOCAL_SOCKET_TOKEN_LENGTH; i++)
        if (!((token[i] >= '0' && token[i] <= '9') || (token[i] >= 'a' && token[i] <= 'f')))
            return false;
    return true;
}
static bool base_check(const sb_local_socket *s, sb_error *e) {
    struct stat held, current;
    sb_root *base = sb_platform_root(SB_LOCAL_BASE, e);
    bool ok = base && !fstat(s->base->fd, &held) && !fstat(base->fd, &current) &&
        base_private(&held) && base_private(&current) &&
        same(&held, &s->base_identity) && same(&held, &current);
    sb_platform_root_close(base);
    if (!ok) sb_fail(e, "Fixed local socket base is unsafe or has changed.");
    return ok;
}
static bool directory_check(const sb_local_socket *s, sb_error *e) {
    struct stat held, named;
    bool ok = s && s->directory >= 0 && base_check(s, e) &&
        !fstat(s->directory, &held) &&
        !fstatat(s->base->fd, s->name, &named, AT_SYMLINK_NOFOLLOW) &&
        directory_private(&held) && directory_private(&named) &&
        same(&held, &s->directory_identity) && same(&held, &named);
    if (!ok) sb_fail(e, "Private local socket directory is unsafe or has changed.");
    return ok;
}
static sb_local_socket *allocate(const char *token, sb_error *e) {
    if (!sb_local_socket_token_valid(token)) {
        sb_fail(e, "Invalid local socket token."); return NULL;
    }
    sb_local_socket *s = calloc(1, sizeof(*s));
    if (!s) { sb_fail(e, "Out of memory."); return NULL; }
    s->directory = -1;
    memcpy(s->token, token, sizeof(s->token));
    int a = snprintf(s->name, sizeof(s->name), "shellbot-ipc-%llu-%s", (unsigned long long)geteuid(), token);
    int b = snprintf(s->path, sizeof(s->path), "%s/%s/" SB_LOCAL_LEAF, SB_LOCAL_BASE, s->name);
    s->base = sb_platform_root(SB_LOCAL_BASE, e);
    if (a < 0 || (size_t)a >= sizeof(s->name) || b < 0 || (size_t)b >= sizeof(s->path) ||
        !s->base || fstat(s->base->fd, &s->base_identity) || !base_private(&s->base_identity)) {
        sb_fail(e, "Fixed local socket base is unavailable or unsafe.");
        sb_local_socket_close(s); return NULL;
    }
    return s;
}
static bool directory_open(sb_local_socket *s, sb_error *e) {
    struct stat named, opened;
    if (fstatat(s->base->fd, s->name, &named, AT_SYMLINK_NOFOLLOW)) {
        sb_posix_error(e, "Inspect local socket directory"); return false;
    }
    if (!directory_private(&named)) {
        sb_fail(e, "Local socket directory is not a private directory owned by this user."); return false;
    }
    s->directory = openat(s->base->fd, s->name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (s->directory < 0 || fstat(s->directory, &opened) || !snapshot(&named, &opened)) {
        sb_fail(e, "Local socket directory changed while opening."); return false;
    }
    s->directory_identity = opened;
    return directory_check(s, e);
}
sb_local_socket *sb_local_socket_create(sb_error *e) {
    for (unsigned attempt = 0; attempt < 32; attempt++) {
        unsigned char random[16]; char token[33];
        if (!sb_platform_random(random, sizeof(random), e)) return NULL;
        for (unsigned i = 0; i < 16; i++) snprintf(token + i * 2, 3, "%02x", random[i]);
        sb_local_socket *s = allocate(token, e);
        if (!s) return NULL;
        if (mkdirat(s->base->fd, s->name, 0700)) {
            int saved = errno;
            sb_local_socket_close(s);
            if (saved == EEXIST) continue;
            errno = saved; sb_posix_error(e, "Create private local socket directory"); return NULL;
        }
        /* Save its identity before accepting responsibility for any cleanup. */
        if (!directory_open(s, e)) { sb_local_socket_close(s); return NULL; }
        s->creator = true;
        return s;
    }
    sb_fail(e, "Cannot reserve a unique private local socket directory."); return NULL;
}
sb_local_socket *sb_local_socket_open(const char *token, sb_error *e) {
    sb_local_socket *s = allocate(token, e);
    if (!s) return NULL;
    if (!directory_open(s, e) ||
        fstatat(s->directory, SB_LOCAL_LEAF, &s->socket_identity, AT_SYMLINK_NOFOLLOW) ||
        !socket_private(&s->socket_identity)) {
        sb_fail(e, "Private local socket endpoint is unavailable or unsafe.");
        sb_local_socket_close(s); return NULL;
    }
    s->bound = true;
    if (!sb_local_socket_check(s, e)) { sb_local_socket_close(s); return NULL; }
    return s;
}
const char *sb_local_socket_path(const sb_local_socket *s) { return s ? s->path : NULL; }
const char *sb_local_socket_token(const sb_local_socket *s) { return s ? s->token : NULL; }
bool sb_local_socket_check(const sb_local_socket *s, sb_error *e) {
    struct stat st;
    bool ok = s && s->bound && directory_check(s, e) &&
        !fstatat(s->directory, SB_LOCAL_LEAF, &st, AT_SYMLINK_NOFOLLOW) &&
        socket_private(&st) && same(&st, &s->socket_identity);
    if (!ok) sb_fail(e, "Private local socket endpoint changed or became unsafe.");
    return ok;
}
bool sb_local_socket_bind(sb_local_socket *s, int fd, int backlog, sb_error *e) {
    if (!s || !s->creator || s->bound || fd < 0 || backlog < 1 || !directory_check(s, e)) {
        sb_fail(e, "Invalid private local socket bind."); return false;
    }
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    memcpy(a.sun_path, s->path, strlen(s->path) + 1);
    socklen_t length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(a.sun_path) + 1);
    /* Never unlink before binding: an existing name belongs to somebody else. */
    if (bind(fd, (struct sockaddr *)&a, length)) {
        sb_posix_error(e, "Bind private local socket"); return false;
    }
    if (fstatat(s->directory, SB_LOCAL_LEAF, &s->socket_identity, AT_SYMLINK_NOFOLLOW) ||
        !socket_owned(&s->socket_identity)) {
        sb_fail(e, "Cannot identify the newly bound private socket."); return false;
    }
    s->bound = true;
    if (!directory_check(s, e) || fchmodat(s->directory, SB_LOCAL_LEAF, 0600, 0) ||
        !sb_local_socket_check(s, e) || listen(fd, backlog)) {
        if (e && !*e->text) sb_posix_error(e, "Prepare private local socket");
        return false;
    }
    return sb_local_socket_check(s, e);
}
static bool reclaim_unchanged(const sb_local_socket *s, const struct stat *directory,
                              const struct stat *socket, bool absent, sb_error *e) {
    struct stat now;
    if (!directory_check(s, e) || fstat(s->directory, &now) || !snapshot(directory, &now)) {
        sb_fail(e, "Local socket directory changed during stale inspection."); return false;
    }
    if (fstatat(s->directory, SB_LOCAL_LEAF, &now, AT_SYMLINK_NOFOLLOW)) {
        if (absent && errno == ENOENT) return true;
    } else if (!absent && socket_private(&now) && snapshot(socket, &now)) return true;
    sb_fail(e, "Local socket changed during stale inspection."); return false;
}
bool sb_local_socket_reclaim(const char *token, sb_error *e) {
    sb_local_socket *s = allocate(token, e);
    if (!s) return false;
    struct stat dir, endpoint;
    if (fstatat(s->base->fd, s->name, &dir, AT_SYMLINK_NOFOLLOW) && errno == ENOENT) {
        bool safe = base_check(s, e); sb_local_socket_close(s); return safe;
    }
    bool ok = directory_open(s, e);
    bool absent = false;
    if (ok && fstat(s->directory, &dir)) ok = false;
    if (ok && fstatat(s->directory, SB_LOCAL_LEAF, &endpoint, AT_SYMLINK_NOFOLLOW)) {
        absent = errno == ENOENT;
        ok = absent;
    } else if (ok) ok = socket_private(&endpoint);
    if (!ok) {
        sb_fail(e, "Cannot reclaim an unsafe local socket endpoint.");
        sb_local_socket_close(s); return false;
    }
    if (!absent) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) || fcntl(fd, F_SETFL, O_NONBLOCK)) {
            if (fd >= 0) close(fd);
            sb_posix_error(e, "Inspect stale local socket"); sb_local_socket_close(s); return false;
        }
        struct sockaddr_un address = {.sun_family = AF_UNIX};
        memcpy(address.sun_path, s->path, strlen(s->path) + 1);
        socklen_t length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(address.sun_path) + 1);
        int connected = connect(fd, (struct sockaddr *)&address, length), failure = errno;
        close(fd);
        if (!connected || (failure != ECONNREFUSED && failure != ENOENT)) {
            sb_fail(e, "Local socket is active or its stale state cannot be confirmed.");
            sb_local_socket_close(s); return false;
        }
    }
    ok = reclaim_unchanged(s, &dir, &endpoint, absent, e);
    if (ok && !absent && unlinkat(s->directory, SB_LOCAL_LEAF, 0)) ok = false;
    /* Never remove a replacement directory or recursively remove unknown files. */
    struct stat named;
    if (ok && (fstatat(s->base->fd, s->name, &named, AT_SYMLINK_NOFOLLOW) ||
               !same(&named, &s->directory_identity) || !directory_private(&named) ||
               unlinkat(s->base->fd, s->name, AT_REMOVEDIR))) ok = false;
    if (!ok && e && !*e->text) sb_posix_error(e, "Remove confirmed stale local socket");
    sb_local_socket_close(s); return ok;
}
void sb_local_socket_close(sb_local_socket *s) {
    if (!s) return;
    if (s->creator && s->directory >= 0 && s->base) {
        struct stat st;
        if (s->bound && !fstatat(s->directory, SB_LOCAL_LEAF, &st, AT_SYMLINK_NOFOLLOW) &&
            socket_owned(&st) && same(&st, &s->socket_identity))
            (void)unlinkat(s->directory, SB_LOCAL_LEAF, 0);
        if (!fstatat(s->base->fd, s->name, &st, AT_SYMLINK_NOFOLLOW) &&
            directory_private(&st) && same(&st, &s->directory_identity))
            (void)unlinkat(s->base->fd, s->name, AT_REMOVEDIR);
    }
    if (s->directory >= 0) close(s->directory);
    sb_platform_root_close(s->base);
    free(s);
}
