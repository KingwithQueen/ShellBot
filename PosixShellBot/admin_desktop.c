#include "admin_desktop.h"
#include "admin_internal.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <spawn.h>
#include <poll.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __APPLE__
#include <libproc.h>
#endif
extern char **environ;
/* Both ends run the same installed executable. UID/PID come from the kernel,
   never from an actor, bearer, JSON request, or pathname ownership alone. */
bool sb_admin_peer(int fd, uid_t uid, pid_t pid) {
    pid_t actual = 0;
#ifdef __APPLE__
    uid_t user; gid_t group;
    socklen_t size = sizeof(actual);
    if (getpeereid(fd, &user, &group) || user != uid ||
        getsockopt(fd, SOL_LOCAL, LOCAL_PEERPID, &actual, &size))
        return false;
    char path[PROC_PIDPATHINFO_MAXSIZE];
    if (actual <= 0 || proc_pidpath(actual, path, sizeof(path)) <= 0)
        return false;
#else
    struct ucred u; socklen_t size = sizeof(u);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &u, &size) || u.uid != uid)
        return false;
    actual = u.pid;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%ld/exe", (long)actual);
#endif
    struct stat own, other;
    return actual > 0 && (!pid || actual == pid) && !stat(sb_posix_program(), &own) &&
           !stat(path, &other) && own.st_dev == other.st_dev && own.st_ino == other.st_ino &&
           own.st_uid == 0 && S_ISREG(own.st_mode) && !(own.st_mode & (0022 | S_ISUID | S_ISGID));
}
int sb_admin_desktop_connect(const char *path, uid_t uid, pid_t pid, sb_error *e) {
    if (!sb_project_path_valid(path) || strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path) || pid <= 1) {
        sb_fail(e, "Invalid native administrator channel address or owner process.");
        return -1;
    }
    char *copy = sb_strdup(path), *slash = copy ? strrchr(copy, '/') : NULL;
    if (!slash || slash == copy) { free(copy); return -1; }
    *slash = 0;
    sb_root *root = sb_platform_root(copy, e);
    struct stat parent, socket_stat;
    bool ok = root && !fstat(root->fd, &parent) && parent.st_uid == uid && !(parent.st_mode & 0077) &&
              !fstatat(root->fd, slash + 1, &socket_stat, AT_SYMLINK_NOFOLLOW) &&
              S_ISSOCK(socket_stat.st_mode) && socket_stat.st_uid == uid && !(socket_stat.st_mode & 0077);
    sb_platform_root_close(root); free(copy);
    if (!ok) { sb_fail(e, "Native administrator channel must be in the owner's private directory."); return -1; }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) || !sb_admin_peer(fd, uid, pid)) {
        close(fd); sb_fail(e, "Native administrator channel peer identity did not match the approved UI."); return -1;
    }
    return fd;
}
bool sb_admin_link_closing(sb_admin_link *link) {
    if (!link->launcher) return false;
    int status = 0;
    pid_t result = waitpid(link->launcher, &status, WNOHANG);
    if (result == link->launcher || (result < 0 && errno == ECHILD)) link->launcher = 0;
    return link->launcher != 0;
}
void sb_admin_link_close(sb_admin_link *link) {
    if (link->fd >= 0) { shutdown(link->fd, SHUT_RDWR); close(link->fd); link->fd = -1; }
    if (link->launcher) {
        int status = 0;
        if (sb_posix_wait_child(link->launcher, 3000, &status)) link->launcher = 0;
    }
    link->registry_sha256[0] = 0;
}
char *sb_admin_link_call(sb_admin_link *link, const char *request, sb_error *e) {
    char *reply = NULL;
    if (link->fd >= 0 && sb_posix_frame_write(link->fd, request, 3000, e))
        reply = sb_posix_frame_read(link->fd, 30000, e);
    if (!reply) {
        sb_admin_link_close(link);
        if (!*e->text) sb_fail(e, "管理员会话未连接；没有重试、提权或重放操作。");
    }
    return reply;
}
#ifdef __APPLE__
/* AppleScript embeds one shell command; quote each argv for sh first, then
   escape that command as an AppleScript string. No account secret is supplied. */
static char *authorization_script(char *const args[]) {
    size_t cap = SB_PATH_CAP * 20, used = 0;
    char *command = calloc(1, cap), *script = calloc(1, cap * 2);
    if (!command || !script) { free(command); free(script); return NULL; }
    for (unsigned i = 0; args[i]; i++) {
        if (used + 3 >= cap) goto fail;
        command[used++] = '\'';
        for (const char *p = args[i]; *p; p++) {
            if ((unsigned char)*p < 32 || used + 5 >= cap) goto fail;
            if (*p == '\'') { memcpy(command + used, "'\\''", 4); used += 4; }
            else command[used++] = *p;
        }
        command[used++] = '\''; command[used++] = ' ';
    }
    size_t n = (size_t)snprintf(script, cap * 2, "do shell script \"");
    for (size_t i = 0; i < used; i++) {
        if (command[i] == '\\' || command[i] == '"') script[n++] = '\\';
        script[n++] = command[i];
    }
    snprintf(script + n, cap * 2 - n, "\" with administrator privileges");
    free(command); return script;
fail:
    free(command); free(script); return NULL;
}
#endif
bool sb_admin_authorize_launch(char *const args[], int log, pid_t *pid, sb_error *e) {
    char *launcher[24];
#ifdef __APPLE__
    char *script = authorization_script(args);
    if (!script) { sb_fail(e, "Cannot encode the fixed administrator launch request."); return false; }
    launcher[0] = "/usr/bin/osascript"; launcher[1] = "-e"; launcher[2] = script; launcher[3] = NULL;
#else
    launcher[0] = "/usr/bin/pkexec";
    unsigned i = 0;
    for (; args[i] && i + 2 < sizeof(launcher) / sizeof(*launcher); i++) launcher[i + 1] = args[i];
    if (args[i]) { sb_fail(e, "Administrator launch exceeds its fixed argument budget."); return false; }
    launcher[i + 1] = NULL;
#endif
    posix_spawn_file_actions_t files; posix_spawnattr_t attr;
    posix_spawn_file_actions_init(&files); posix_spawnattr_init(&attr);
    int rc = posix_spawn_file_actions_addopen(&files, 0, "/dev/null", O_RDONLY, 0);
    if (!rc) rc = posix_spawn_file_actions_adddup2(&files, log, 1);
    if (!rc) rc = posix_spawn_file_actions_adddup2(&files, log, 2);
    if (!rc) rc = posix_spawn_file_actions_addclose(&files, log);
    sigset_t empty, defaults; sigemptyset(&empty); sigemptyset(&defaults);
    sigaddset(&defaults, SIGTERM); sigaddset(&defaults, SIGINT); sigaddset(&defaults, SIGPIPE); sigaddset(&defaults, SIGCHLD);
    posix_spawnattr_setsigmask(&attr, &empty); posix_spawnattr_setsigdefault(&attr, &defaults);
    posix_spawnattr_setpgroup(&attr, 0);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    if (!rc) rc = posix_spawn(pid, launcher[0], &files, &attr, launcher, environ);
    posix_spawn_file_actions_destroy(&files); posix_spawnattr_destroy(&attr);
#ifdef __APPLE__
    free(script);
#endif
    if (rc) { errno = rc; sb_posix_error(e, "Open system administrator authorization"); }
    return rc == 0;
}
bool sb_admin_link_start(sb_admin_link *link, const char *directory, const char *registry,
                         const char *state, const char *sha, atomic_bool *cancel, sb_error *e) {
    if (link->fd >= 0 || sb_admin_link_closing(link) || getuid() != geteuid() || !geteuid()) {
        sb_fail(e, "管理员会话已存在、仍在结束，或管理界面不是普通账户。"); return false;
    }
    if (!sb_project_path_valid(registry) || !sb_project_path_valid(state) || !sha || strlen(sha) != 64 ||
        strspn(sha, "0123456789abcdef") != 64) { sb_fail(e, "请先保存规范绝对路径和有效项目配置。"); return false; }
    if (!sb_admin_trusted_path(sb_posix_program(), false, e)) {
        sb_fail(e, "请先把完整程序安装到 root 拥有且普通账户不可改写的位置，再从该位置打开管理界面。"); return false;
    }
    sb_root *root = sb_platform_root(directory, e);
    if (!root || !sb_posix_private(root, e)) { sb_platform_root_close(root); return false; }
    unsigned char random[8]; char nonce[17], name[64], path[104], user[24], pid[24];
    int listener = -1, log = -1; bool bound = false, ok = false;
    if (!sb_platform_random(random, sizeof(random), e)) goto done;
    for (unsigned i = 0; i < 8; i++) snprintf(nonce + i * 2, 3, "%02x", random[i]);
    snprintf(name, sizeof(name), "admin-%s.sock", nonce);
    int count = snprintf(path, sizeof(path), "%s/%s", directory, name);
    if (count < 0 || (size_t)count >= sizeof(path)) { sb_fail(e, "管理员连接目录过长，请选择较短的私有应用数据目录。"); goto done; }
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
    listener = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener < 0) goto done;
    fcntl(listener, F_SETFD, FD_CLOEXEC);
    if (bind(listener, (struct sockaddr *)&address, sizeof(address))) goto done;
    bound = true;
    if (chmod(path, 0600) || listen(listener, 2)) goto done;
    char log_name[64]; snprintf(log_name, sizeof(log_name), "admin-launch-%s.log", nonce);
    log = openat(root->fd, log_name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (log < 0) goto done;
    snprintf(user, sizeof(user), "%u", (unsigned)geteuid()); snprintf(pid, sizeof(pid), "%ld", (long)getpid());
    char *args[] = {(char *)sb_posix_program(), "--admin-broker", "--projects", (char *)registry,
                    "--admin-dir", (char *)state, "--owner-uid", user, "--desktop-channel", path,
                    "--desktop-pid", pid, "--expected-registry-sha256", (char *)sha, NULL};
    if (!sb_admin_authorize_launch(args, log, &link->launcher, e)) goto done;
    uint64_t deadline = sb_platform_millis() + 120000;
    while (!atomic_load(cancel) && !sb_posix_signal && sb_platform_millis() < deadline) {
        struct pollfd p = {.fd = listener, .events = POLLIN};
        if (poll(&p, 1, 100) > 0 && (p.revents & POLLIN)) {
            int fd = accept(listener, NULL, NULL);
            if (fd < 0) continue;
            fcntl(fd, F_SETFD, FD_CLOEXEC);
            if (!sb_admin_peer(fd, 0, 0)) { close(fd); continue; }
            link->fd = fd;
            char *reply = sb_posix_frame_read(fd, 30000, e);
            yyjson_doc *d = reply ? yyjson_read(reply, strlen(reply), 0) : NULL; free(reply);
            yyjson_val *v = yyjson_doc_get_root(d);
            const char *hash = sb_string(yyjson_obj_get(v, "registry_sha256")), *id = sb_string(yyjson_obj_get(v, "broker_id"));
            ok = hash && !strcmp(hash, sha) && id && strlen(id) == 32 && strspn(id, "0123456789abcdef") == 32 &&
                 yyjson_get_uint(yyjson_obj_get(v, "owner_uid")) == (uint64_t)geteuid() && yyjson_get_bool(yyjson_obj_get(v, "healthy"));
            yyjson_doc_free(d);
            if (ok) snprintf(link->registry_sha256, sizeof(link->registry_sha256), "%s", sha);
            break;
        }
        if (!sb_admin_link_closing(link)) break;
    }
done:
    /* Cancellation can arrive while reading the authenticated broker hello. */
    if (atomic_load(cancel) || sb_posix_signal) ok = false;
    if (log >= 0) close(log);
    if (listener >= 0) close(listener);
    if (bound) unlinkat(root->fd, name, 0);
    sb_platform_root_close(root);
    if (!ok) {
        /* Cancel only our own unreaped authorization launcher. Closing the
           private listener also prevents a delayed prompt from granting a
           detached session, even if the launcher has already become root. */
        if (sb_admin_link_closing(link)) kill(link->launcher, SIGTERM);
        sb_admin_link_close(link);
        if (!*e->text) sb_fail(e, "管理员授权取消、超时或连接失败；请检查系统提示和应用数据目录的授权日志。");
    }
    return ok;
}
