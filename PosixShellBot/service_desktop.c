#include "service_desktop.h"
#include "admin_desktop.h"
#include "admin_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pwd.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define SERVICE_OUTPUT_LIMIT 131072
static void *service_reap(void *context) {
    pid_t pid = *(pid_t *)context; free(context); int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return NULL;
}
static void service_finish_launcher(pid_t pid, bool cancel) {
    int status = 0;
    pid_t owned;
    do { owned = waitpid(pid, &status, WNOHANG); } while (owned < 0 && errno == EINTR);
    if (owned != 0) return;
    if (cancel) kill(pid, SIGTERM);
    if (sb_posix_wait_child(pid, 3000, &status)) return;
    /* A root helper cannot retain a grant after its private channel closes.
       Reap the owned authorization launcher if the OS finishes its dialog
       asynchronously; never reuse or later signal a numeric native-job PID. */
    kill(pid, SIGTERM);
    pid_t *pending = malloc(sizeof(*pending)); pthread_t thread;
    if (pending) {
        *pending = pid;
        if (!pthread_create(&thread, NULL, service_reap, pending)) { pthread_detach(thread); return; }
        free(pending);
    }
    sb_posix_wait_child(pid, 1000, &status);
}
static bool service_channel_ready(int fd) {
    char byte; ssize_t n;
    do { n = recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT); } while (n < 0 && errno == EINTR);
    return n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
}
typedef struct {
    const char *action, *directory, *name, *projects, *data, *key;
    const char *registry_sha, *unit, *content_sha;
    const char *starts[SB_MAX_PROJECTS];
    unsigned count;
    bool oauth;
} service_request;
static const char *service_string(yyjson_val *v, const char *key) {
    const char *s = yyjson_get_str(yyjson_obj_get(v, key));
    return s ? s : "";
}
static bool service_sha(const char *s) {
    return s && strlen(s) == 64 && strspn(s, "0123456789abcdef") == 64;
}
static bool service_parse(service_request *s, yyjson_val *v, sb_error *e) {
    static const char *keys[] = {"action", "startup_dir", "name", "projects", "data", "key", "oauth",
        "starts", "expected_registry_sha256", "expected_unit_id", "expected_content_sha256"};
    bool seen[sizeof(keys) / sizeof(*keys)] = {0};
    if (!yyjson_is_obj(v)) goto bad;
    size_t i, max; yyjson_val *key, *value;
    yyjson_obj_foreach(v, i, max, key, value) {
        if (yyjson_get_len(key) != strlen(yyjson_get_str(key))) goto bad;
        unsigned j = 0;
        for (; j < sizeof(keys) / sizeof(*keys); j++) if (!strcmp(yyjson_get_str(key), keys[j])) break;
        if (j == sizeof(keys) / sizeof(*keys) || seen[j]) goto bad;
        if (j != 6 && j != 7 && (!yyjson_is_str(value) || yyjson_get_len(value) != strlen(yyjson_get_str(value)))) goto bad;
        seen[j] = true;
    }
    s->action = service_string(v, "action"); s->directory = service_string(v, "startup_dir");
    s->name = service_string(v, "name"); s->projects = service_string(v, "projects");
    s->data = service_string(v, "data"); s->key = service_string(v, "key");
    s->registry_sha = service_string(v, "expected_registry_sha256");
    s->unit = service_string(v, "expected_unit_id"); s->content_sha = service_string(v, "expected_content_sha256");
    if (!sb_project_path_valid(s->directory) ||
        (strcmp(s->action, "status") && strcmp(s->action, "install") && strcmp(s->action, "start") &&
         strcmp(s->action, "stop") && strcmp(s->action, "uninstall"))) goto bad;
    if (strcmp(s->action, "status") && (!*s->unit || !service_sha(s->content_sha))) goto bad;
    if (!strcmp(s->action, "install")) {
        if (!sb_identifier(s->name) || strlen(s->name) > 48 || !sb_project_path_valid(s->projects) ||
            !sb_project_path_valid(s->data) || !sb_project_path_valid(s->key) || !service_sha(s->registry_sha)) goto bad;
        yyjson_val *oauth = yyjson_obj_get(v, "oauth"), *starts = yyjson_obj_get(v, "starts");
        if ((oauth && !yyjson_is_bool(oauth)) || (starts && !yyjson_is_arr(starts))) goto bad;
        s->oauth = yyjson_get_bool(oauth);
        yyjson_arr_foreach(starts, i, max, value) {
            const char *id = yyjson_get_str(value);
            if (!id || yyjson_get_len(value) != strlen(id) || !sb_identifier(id) || s->count == SB_MAX_PROJECTS) goto bad;
            for (unsigned j = 0; j < s->count; j++) if (!strcmp(id, s->starts[j])) goto bad;
            s->starts[s->count++] = id;
        }
    } else {
        const unsigned install_keys[] = {2, 3, 4, 5, 6, 7, 8};
        for (unsigned j = 0; j < sizeof(install_keys) / sizeof(*install_keys); j++)
            if (seen[install_keys[j]]) goto bad;
        if ((!*s->unit && *s->content_sha) || (*s->unit && !service_sha(s->content_sha))) goto bad;
    }
    return true;
bad:
    sb_fail(e, "System service operation needs a fixed reviewed action, canonical paths and matching unit/configuration hashes.");
    return false;
}
/* Protected executable, fixed argv/environment, bounded separate JSON output.
   The child remains this helper's own unreaped process until it is stopped. */
static char *service_command(char *const args[], int channel, int *result, sb_error *e) {
    int pipes[2]; *result = -1;
    if (!service_channel_ready(channel)) { sb_fail(e, "The approved UI channel closed or sent an unexpected extra operation."); return NULL; }
    if (pipe(pipes)) { sb_posix_error(e, "Create service result channel"); return NULL; }
    fcntl(pipes[0], F_SETFD, FD_CLOEXEC); fcntl(pipes[1], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t files; posix_spawnattr_t attr;
    posix_spawn_file_actions_init(&files); posix_spawnattr_init(&attr);
    int rc = posix_spawn_file_actions_addopen(&files, 0, "/dev/null", O_RDONLY, 0);
    if (!rc) rc = posix_spawn_file_actions_adddup2(&files, pipes[1], 1);
    if (!rc) rc = posix_spawn_file_actions_addopen(&files, 2, "/dev/null", O_WRONLY, 0);
    if (!rc) rc = posix_spawn_file_actions_addclose(&files, pipes[0]);
    if (!rc) rc = posix_spawn_file_actions_addclose(&files, pipes[1]);
    sigset_t empty, defaults; sigemptyset(&empty); sigemptyset(&defaults);
    sigaddset(&defaults, SIGTERM); sigaddset(&defaults, SIGINT); sigaddset(&defaults, SIGPIPE); sigaddset(&defaults, SIGCHLD);
    posix_spawnattr_setsigmask(&attr, &empty); posix_spawnattr_setsigdefault(&attr, &defaults);
    posix_spawnattr_setpgroup(&attr, 0);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    char *env[] = {"PATH=/usr/bin:/bin:/usr/sbin:/sbin", "LC_ALL=C", NULL};
    pid_t pid = 0;
    if (!rc) rc = posix_spawn(&pid, args[0], &files, &attr, args, env);
    posix_spawn_file_actions_destroy(&files); posix_spawnattr_destroy(&attr); close(pipes[1]);
    if (rc) { close(pipes[0]); sb_fail(e, "Protected service operation could not start."); return NULL; }
    char *wire = calloc(1, SERVICE_OUTPUT_LIMIT + 1); size_t used = 0;
    bool eof = false, done = false, overflow = false, disconnected = false, reaped_elsewhere = false; int status = 0;
    uint64_t deadline = sb_platform_millis() + 65000;
    while (wire && !sb_posix_signal && sb_platform_millis() < deadline && (!eof || !done)) {
        struct pollfd p[2] = {{pipes[0], POLLIN, 0}, {channel, POLLIN, 0}};
        int n = poll(p, 2, 50);
        if (n > 0 && (p[1].revents & (POLLHUP | POLLERR | POLLNVAL))) { disconnected = true; break; }
        if (n > 0 && (p[0].revents & (POLLIN | POLLHUP))) {
            char bytes[2048]; ssize_t got = read(pipes[0], bytes, sizeof(bytes));
            if (got > 0) {
                if ((size_t)got > SERVICE_OUTPUT_LIMIT - used) { overflow = true; break; }
                memcpy(wire + used, bytes, (size_t)got); used += (size_t)got;
            } else if (!got) eof = true;
            else if (errno != EINTR) break;
        }
        if (!done) {
            pid_t got = waitpid(pid, &status, WNOHANG); done = got == pid;
            if (got < 0 && errno == ECHILD) { done = true; reaped_elsewhere = true; }
        }
    }
    close(pipes[0]);
    if (!done) {
        pid_t owned;
        do { owned = waitpid(pid, &status, WNOHANG); } while (owned < 0 && errno == EINTR);
        if (owned == pid) done = true;
        else if (owned < 0) { done = true; reaped_elsewhere = true; }
        else { kill(-pid, SIGKILL); kill(pid, SIGKILL); while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {} }
    }
    if (!done || !eof || overflow || disconnected || reaped_elsewhere || !wire || memchr(wire, 0, used)) {
        free(wire); sb_fail(e, "Service operation interrupted or exceeded its result budget; retained startup receipts describe any partial change."); return NULL;
    }
    *result = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
    yyjson_doc *d = yyjson_read(wire, used, 0);
    bool valid = d && yyjson_is_obj(yyjson_doc_get_root(d)); yyjson_doc_free(d);
    if (!valid) { free(wire); sb_fail(e, "Service installer did not return a valid JSON result."); return NULL; }
    return wire;
}
static char *service_execute(service_request *s, uid_t owner, int channel, sb_error *e) {
    struct passwd *pw = getpwuid(owner);
    if (!pw || !owner || strlen(pw->pw_name) >= 64) { sb_fail(e, "The approved ordinary service account is unavailable."); return NULL; }
    char account[64], uid[24], option[40];
    snprintf(account, sizeof(account), "%s", pw->pw_name); snprintf(uid, sizeof(uid), "%u", (unsigned)owner);
    snprintf(option, sizeof(option), "--startup-%s", s->action);
    char *args[64]; unsigned n = 0;
    args[n++] = (char *)sb_posix_program(); args[n++] = option;
    args[n++] = "--startup-dir"; args[n++] = (char *)s->directory;
    args[n++] = "--expected-runtime-uid"; args[n++] = uid;
    args[n++] = "--expected-kind"; args[n++] = "system-service";
    if (*s->unit) { args[n++] = "--expected-unit-id"; args[n++] = (char *)s->unit; }
    if (*s->content_sha) { args[n++] = "--expected-content-sha256"; args[n++] = (char *)s->content_sha; }
    if (!strcmp(s->action, "install")) {
        args[n++] = "--kind"; args[n++] = "system-service"; args[n++] = "--service-user"; args[n++] = account;
        args[n++] = "--name"; args[n++] = (char *)s->name; args[n++] = "--projects"; args[n++] = (char *)s->projects;
        args[n++] = "--manager-data-dir"; args[n++] = (char *)s->data;
        args[n++] = "--credential-key-file"; args[n++] = (char *)s->key;
        args[n++] = "--expected-registry-sha256"; args[n++] = (char *)s->registry_sha;
        if (s->oauth) args[n++] = "--oauth";
        for (unsigned i = 0; i < s->count; i++) { args[n++] = "--start-project"; args[n++] = (char *)s->starts[i]; }
    }
    if (n >= sizeof(args) / sizeof(*args)) { sb_fail(e, "Service arguments exceed their fixed budget."); return NULL; }
    args[n] = NULL;
    int code = -1; char *operation = service_command(args, channel, &code, e);
    if (!operation) return NULL;
    yyjson_doc *op = yyjson_read(operation, strlen(operation), 0); free(operation);
    yyjson_mut_doc *out = yyjson_mut_doc_new(NULL); yyjson_mut_val *v = yyjson_mut_obj(out);
    yyjson_mut_obj_add_bool(out, v, "ok", code == 0);
    yyjson_mut_obj_add_strcpy(out, v, "action", s->action);
    yyjson_mut_obj_add_val(out, v, "operation", yyjson_val_mut_copy(out, yyjson_doc_get_root(op)));
    if (code) yyjson_mut_obj_add_strcpy(out, v, "error", service_string(yyjson_doc_get_root(op), "error"));
    if (!strcmp(s->action, "status")) {
        if (!code) yyjson_mut_obj_add_val(out, v, "status", yyjson_val_mut_copy(out, yyjson_doc_get_root(op)));
    } else {
        char *status_args[] = {(char *)sb_posix_program(), "--startup-status", "--startup-dir", (char *)s->directory,
            "--expected-runtime-uid", uid, "--expected-kind", "system-service", NULL};
        int status_code = -1; sb_error status_error = {0};
        char *status = service_command(status_args, channel, &status_code, &status_error);
        yyjson_doc *d = status ? yyjson_read(status, strlen(status), 0) : NULL; free(status);
        if (!status_code && d) yyjson_mut_obj_add_val(out, v, "status", yyjson_val_mut_copy(out, yyjson_doc_get_root(d)));
        else yyjson_mut_obj_add_strcpy(out, v, "status_error", *status_error.text ? status_error.text :
            (d ? service_string(yyjson_doc_get_root(d), "error") : "Current native service status is unavailable."));
        yyjson_doc_free(d);
    }
    yyjson_doc_free(op);
    return sb_posix_json(out, v);
}
int sb_service_desktop_cli(int argc, char **argv) {
    const char *user = NULL, *path = NULL, *pid = NULL, *expected = NULL; sb_error e = {0};
    if (getuid() || geteuid() || !sb_admin_trusted_path(sb_posix_program(), false, &e)) goto done;
    for (int i = 2; i < argc; i++) {
        const char **slot = !strcmp(argv[i], "--owner-uid") ? &user :
            !strcmp(argv[i], "--desktop-channel") ? &path : !strcmp(argv[i], "--desktop-pid") ? &pid :
            !strcmp(argv[i], "--expected-request-sha256") ? &expected : NULL;
        if (!slot || *slot || i + 1 >= argc) goto done;
        *slot = argv[++i];
    }
    if (!user || !*user || strspn(user, "0123456789") != strlen(user) ||
        !pid || !*pid || strspn(pid, "0123456789") != strlen(pid) || !path || !service_sha(expected)) goto done;
    unsigned long long u = strtoull(user, NULL, 10), p = strtoull(pid, NULL, 10);
    if (!u || u > UINT_MAX || (uid_t)u != u || p <= 1 || p > INT_MAX || !getpwuid((uid_t)u)) goto done;
    int fd = sb_admin_desktop_connect(path, (uid_t)u, (pid_t)p, &e);
    if (fd < 0) goto done;
    char *request = sb_posix_frame_read(fd, 5000, &e), hash[65];
    yyjson_doc *d = request ? yyjson_read(request, strlen(request), 0) : NULL;
    service_request s = {0};
    bool valid = request && sb_platform_sha256(request, strlen(request), hash, &e) && !strcmp(hash, expected) &&
                 service_parse(&s, yyjson_doc_get_root(d), &e);
    char *reply = valid ? service_execute(&s, (uid_t)u, fd, &e) : NULL;
    if (!reply) reply = sb_posix_json_error(*e.text ? e.text : "System service review did not match the approved request.");
    bool sent = reply && sb_posix_frame_write(fd, reply, 5000, &e);
    free(reply); yyjson_doc_free(d); free(request); shutdown(fd, SHUT_RDWR); close(fd);
    return valid && sent ? 0 : 2;
done:
    fprintf(stderr, "ShellBot system service helper: %s\n", *e.text ? e.text :
        "A protected root helper and a bound ordinary-account UI channel are required.");
    return 2;
}
char *sb_service_system_call(const char *directory, const char *request, atomic_bool *cancel, sb_error *e) {
    if (!cancel || atomic_load(cancel) || sb_posix_signal) { sb_fail(e, "System service operation was cancelled before authorization."); return NULL; }
    if (!request || strlen(request) > SB_REQUEST_LIMIT) { sb_fail(e, "System service request exceeds its fixed budget."); return NULL; }
    service_request parsed = {0}; yyjson_doc *d = request ? yyjson_read(request, strlen(request), 0) : NULL;
    bool valid = service_parse(&parsed, yyjson_doc_get_root(d), e); yyjson_doc_free(d);
    if (!valid) return NULL;
    if (getuid() != geteuid() || !geteuid() || !sb_admin_trusted_path(sb_posix_program(), false, e)) {
        sb_fail(e, "系统服务管理需要从 root 拥有且普通账户不可改写的安装位置打开管理界面。"); return NULL;
    }
    sb_root *root = sb_platform_root(directory, e);
    if (!root || !sb_posix_private(root, e)) { sb_platform_root_close(root); return NULL; }
    char nonce[17], name[64] = {0}, path[104], log_name[64], user[24], pid[24], sha[65];
    unsigned char random[8]; int listener = -1, log = -1, channel = -1; bool bound = false; pid_t launcher = 0;
    char *reply = NULL;
    if (!sb_platform_random(random, sizeof(random), e) || !sb_platform_sha256(request, strlen(request), sha, e)) goto done;
    for (unsigned i = 0; i < 8; i++) snprintf(nonce + 2 * i, 3, "%02x", random[i]);
    snprintf(name, sizeof(name), "service-%s.sock", nonce);
    int count = snprintf(path, sizeof(path), "%s/%s", directory, name);
    if (count < 0 || (size_t)count >= sizeof(path)) { sb_fail(e, "系统服务授权连接目录过长，请使用较短的私有应用数据目录。"); goto done; }
    struct sockaddr_un address = {.sun_family = AF_UNIX}; snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
    listener = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener < 0) goto done;
    fcntl(listener, F_SETFD, FD_CLOEXEC);
    if (bind(listener, (struct sockaddr *)&address, sizeof(address))) goto done;
    bound = true;
    if (chmod(path, 0600) || listen(listener, 2)) goto done;
    snprintf(log_name, sizeof(log_name), "service-launch-%s.log", nonce);
    log = openat(root->fd, log_name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (log < 0) goto done;
    snprintf(user, sizeof(user), "%u", (unsigned)geteuid()); snprintf(pid, sizeof(pid), "%ld", (long)getpid());
    char *args[] = {(char *)sb_posix_program(), "--service-desktop", "--owner-uid", user,
        "--desktop-channel", path, "--desktop-pid", pid, "--expected-request-sha256", sha, NULL};
    if (atomic_load(cancel) || sb_posix_signal) goto done;
    if (!sb_admin_authorize_launch(args, log, &launcher, e)) goto done;
    uint64_t deadline = sb_platform_millis() + 120000;
    while (!atomic_load(cancel) && !sb_posix_signal && sb_platform_millis() < deadline) {
        struct pollfd p = {.fd = listener, .events = POLLIN};
        if (poll(&p, 1, 100) > 0 && (p.revents & POLLIN)) {
            int fd = accept(listener, NULL, NULL);
            if (fd < 0) continue;
            fcntl(fd, F_SETFD, FD_CLOEXEC);
            if (!sb_admin_peer(fd, 0, 0)) { close(fd); continue; }
            channel = fd;
            if (sb_posix_frame_write(channel, request, 5000, e)) {
                uint64_t response_deadline = sb_platform_millis() + 135000;
                while (!atomic_load(cancel) && !sb_posix_signal && sb_platform_millis() < response_deadline) {
                    struct pollfd response = {.fd = channel, .events = POLLIN};
                    if (poll(&response, 1, 100) > 0) {
                        if (response.revents & POLLIN) reply = sb_posix_frame_read(channel, 5000, e);
                        break;
                    }
                }
            }
            break;
        }
        int status = 0; pid_t got = waitpid(launcher, &status, WNOHANG);
        if (got == launcher || (got < 0 && errno == ECHILD)) { launcher = 0; break; }
    }
done:
    if (channel >= 0) { shutdown(channel, SHUT_RDWR); close(channel); }
    if (listener >= 0) close(listener);
    if (log >= 0) close(log);
    if (bound) unlinkat(root->fd, name, 0);
    sb_platform_root_close(root);
    if (launcher) service_finish_launcher(launcher, !reply);
    if (!reply && !*e->text) sb_fail(e, "系统服务授权取消、超时或连接失败；没有自动重试。请检查保留的授权日志和服务状态。");
    return reply;
}
