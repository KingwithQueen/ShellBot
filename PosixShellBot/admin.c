#include "admin.h"
#include "sb_projects.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#ifdef __APPLE__
#include <libproc.h>
#endif
typedef struct {
    int fd;
    pthread_mutex_t mutex;
} connection;
static const char *str(yyjson_val *v, const char *k) { return sb_string(yyjson_obj_get(v, k)); }
static bool eligible(const sb_project *p) {
    return p && p->enabled && !p->legacy_compatibility && p->policy.execution_enabled &&
           p->policy.administrator_allowed && p->policy.default_identity == SB_ID_CURRENT_USER &&
           p->policy.file_scope == SB_FILES_CURRENT_USER;
}
static bool digest(const sb_project *p, char hash[65], sb_error *e) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (!d)
        return false;
    yyjson_mut_val *v = yyjson_mut_obj(d);
    const char *keys[] = {"id", "token_env", "data_dir", "default_workspace"};
    const char *values[] = {p->id, p->token_env, p->data_dir, p->default_workspace_id};
    for (size_t i = 0; i < 4; i++)
        yyjson_mut_obj_add_strcpy(d, v, keys[i], values[i]);
    yyjson_mut_obj_add_uint(d, v, "revision", p->policy_revision);
    yyjson_mut_obj_add_uint(d, v, "tools", p->policy.tools);
    yyjson_mut_obj_add_uint(d, v, "approval", p->policy.approval_mode);
    yyjson_mut_obj_add_uint(d, v, "ttl", p->approval_ttl_seconds);
    yyjson_mut_obj_add_uint(d, v, "audit", p->audit_max_bytes);
    yyjson_mut_obj_add_bool(d, v, "read_only", p->policy.current_user_read_only);
    yyjson_mut_val *a = yyjson_mut_arr(d);
    yyjson_mut_obj_add_val(d, v, "directories", a);
    for (size_t i = 0; i < p->directory_count; i++) {
        const sb_project_directory *r = &p->directories[i];
        yyjson_mut_val *x = yyjson_mut_obj(d);
        yyjson_mut_obj_add_strcpy(d, x, "id", r->id);
        yyjson_mut_obj_add_strcpy(d, x, "path", r->path);
        yyjson_mut_obj_add_bool(d, x, "read_only", r->read_only);
        yyjson_mut_obj_add_bool(d, x, "workspace", r->workspace);
        yyjson_mut_arr_add_val(a, x);
    }
    a = yyjson_mut_arr(d);
    yyjson_mut_obj_add_val(d, v, "limits", a);
    const unsigned limits[] = {p->limits.active_tasks,     p->limits.task_records,
                               p->limits.output_bytes,     p->limits.task_timeout_ms,
                               p->limits.approval_records, p->limits.worker_memory_mb};
    for (size_t i = 0; i < 6; i++)
        yyjson_mut_arr_add_uint(d, a, limits[i]);
    char *wire = sb_posix_json(d, v);
    bool ok = wire && sb_platform_sha256(wire, strlen(wire), hash, e);
    free(wire);
    return ok;
}
static bool endpoint(uid_t uid, const char *id, struct sockaddr_un *a) {
    sb_error e = {0};
    char hash[65];
    if (!sb_platform_sha256(id, strlen(id), hash, &e))
        return false;
    memset(a, 0, sizeof(*a));
    a->sun_family = AF_UNIX;
#ifdef __APPLE__
    const char *root = "/private/var/run";
#else
    const char *root = "/run";
#endif
    int n = snprintf(a->sun_path, sizeof(a->sun_path), "%s/shellbot-admin-%u-%.16s.sock", root, (unsigned)uid,
                     hash);
    return n > 0 && (size_t)n < sizeof(a->sun_path);
}
static bool peer_uid(int fd, uid_t expected) {
#ifdef __APPLE__
    uid_t uid;
    gid_t gid;
    return !getpeereid(fd, &uid, &gid) && uid == expected;
#else
    struct ucred u;
    socklen_t n = sizeof(u);
    return !getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &u, &n) && u.uid == expected;
#endif
}
/* The grant is for a worker of this installed, root-owned executable, never
   for an arbitrary command which happens to run under the same uid. */
static bool peer_program(int fd) {
    pid_t pid = 0;
#ifdef __APPLE__
    socklen_t length = sizeof(pid);
    if (getsockopt(fd, SOL_LOCAL, LOCAL_PEERPID, &pid, &length) || pid <= 0)
        return false;
    char path[PROC_PIDPATHINFO_MAXSIZE];
    if (proc_pidpath(pid, path, sizeof(path)) <= 0)
        return false;
#else
    struct ucred u;
    socklen_t length = sizeof(u);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &u, &length) || u.pid <= 0)
        return false;
    pid = u.pid;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%ld/exe", (long)pid);
#endif
    struct stat own, other;
    return !stat(sb_posix_program(), &own) && !stat(path, &other) && own.st_dev == other.st_dev &&
           own.st_ino == other.st_ino && own.st_uid == 0 && !(own.st_mode & 0022);
}
bool sb_posix_administrator_available(sb_host *h) {
    connection *c = h->administrator_context;
    if (!c || c->fd < 0)
        return false;
    struct pollfd p = {.fd = c->fd, .events = POLLIN};
    if (poll(&p, 1, 0) > 0 && (p.revents & (POLLHUP | POLLERR | POLLNVAL)))
        h->administrator_backend = false;
    return h->administrator_backend;
}
void sb_posix_admin_connect(sb_host *h) {
    if (!eligible(h->project) || geteuid() == 0)
        return;
    struct sockaddr_un a;
    if (!endpoint(geteuid(), h->project->id, &a))
        return;
    struct stat st;
    if (lstat(a.sun_path, &st) || !S_ISSOCK(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0077))
        return;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) || !peer_uid(fd, 0)) {
        close(fd);
        return;
    }
    sb_error e = {0};
    char hash[65];
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (!d || !digest(h->project, hash, &e)) {
        yyjson_mut_doc_free(d);
        close(fd);
        return;
    }
    yyjson_mut_val *v = yyjson_mut_obj(d);
    yyjson_mut_obj_add_str(d, v, "project_sha256", hash);
    yyjson_mut_obj_add_str(d, v, "instance", sb_execution_instance(h->execution));
    char *wire = sb_posix_json(d, v);
    bool sent = wire && sb_posix_frame_write(fd, wire, 3000, &e);
    free(wire);
    wire = sent ? sb_posix_frame_read(fd, 5000, &e) : NULL;
    yyjson_doc *reply = wire ? yyjson_read(wire, strlen(wire), 0) : NULL;
    free(wire);
    bool ok = reply && yyjson_get_bool(yyjson_obj_get(yyjson_doc_get_root(reply), "ready"));
    yyjson_doc_free(reply);
    connection *c = ok ? calloc(1, sizeof(*c)) : NULL;
    if (!c) {
        close(fd);
        return;
    }
    c->fd = fd;
    pthread_mutex_init(&c->mutex, NULL);
    h->administrator_context = c;
    h->administrator_backend = true;
}
void sb_posix_admin_close(sb_host *h) {
    connection *c = h->administrator_context;
    if (c) {
        close(c->fd);
        pthread_mutex_destroy(&c->mutex);
        free(c);
    }
    h->administrator_context = NULL;
    h->administrator_backend = false;
}
char *sb_posix_administrator_call(sb_host *h, const char *actor, const char *name, yyjson_val *args) {
    bool task = !strncmp(name, "task_", 5),
         approval = !strcmp(name, "approval_status") || !strcmp(name, "approval_cancel");
    if (!task && !approval)
        return sb_posix_json_error("POSIX administrator backend grants command tasks only.");
    if (!eligible(h->project) || !sb_posix_administrator_available(h))
        return sb_posix_json_error("Project administrator agent is unavailable; the local owner must "
                                   "explicitly start and bind a new session.");
    connection *c = h->administrator_context;
    pthread_mutex_lock(&c->mutex);
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *v = d ? yyjson_mut_obj(d) : NULL;
    if (v) {
        yyjson_mut_obj_add_strcpy(d, v, "name", name);
        yyjson_mut_obj_add_strcpy(d, v, "actor", actor);
        yyjson_mut_obj_add_val(d, v, "args", yyjson_val_mut_copy(d, args));
    }
    char *wire = v ? sb_posix_json(d, v) : NULL;
    sb_error e = {0};
    bool sent = wire && sb_posix_frame_write(c->fd, wire, 5000, &e);
    free(wire);
    char *reply = sent ? sb_posix_frame_read(c->fd, 8000, &e) : NULL;
    if (!reply) {
        shutdown(c->fd, SHUT_RDWR);
        h->administrator_backend = false;
    }
    pthread_mutex_unlock(&c->mutex);
    return reply ? reply
                 : sb_posix_json_error("Administrator channel closed; inspect existing task effects. No "
                                       "request was replayed or downgraded.");
}
/* Every ancestor is opened without following links. Root never creates state
   or a log underneath a user-writable directory. */
static bool trusted_path(const char *path, bool directory, sb_error *e) {
    if (!sb_project_path_valid(path)) {
        sb_fail(e, "Canonical root-owned paths are required.");
        return false;
    }
    char *copy = sb_strdup(path);
    if (!copy)
        return false;
    int fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    bool ok = fd >= 0;
    char *save = NULL, *part = strtok_r(copy + 1, "/", &save);
    while (ok && part) {
        char *next = strtok_r(NULL, "/", &save);
        int child =
            openat(fd, part, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | ((next || directory) ? O_DIRECTORY : 0));
        close(fd);
        fd = child;
        struct stat st;
        ok = fd >= 0 && !fstat(fd, &st) && st.st_uid == 0 && !(st.st_mode & 0022) &&
             ((next || directory)
                  ? S_ISDIR(st.st_mode)
                  : (S_ISREG(st.st_mode) && st.st_nlink == 1 && !(st.st_mode & (S_ISUID | S_ISGID))));
        part = next;
    }
    if (fd >= 0)
        close(fd);
    free(copy);
    if (!ok)
        sb_fail(e, "Administrator program, registry and state require root-owned, non-writable ancestor "
                   "chains without links.");
    return ok;
}
static bool initialize(sb_host *h, const sb_project *p, const char *data, const char *instance, sb_error *e) {
    h->project = p;
    h->administrator_host = true;
    h->administrator_backend = true;
    h->started_ms = sb_platform_millis();
    sb_config_defaults(&h->config);
    h->config.data_dir = sb_strdup(data);
    h->config.execution_enabled = true;
    h->config.execution_profile = SB_EXEC_ADMINISTRATOR;
    h->config.limits = p->limits;
    h->config.approval_ttl_seconds = p->approval_ttl_seconds;
    h->config.audit_max_bytes = p->audit_max_bytes;
    h->lease = sb_platform_lease(data, e);
    if (!h->lease)
        return false;
    for (size_t i = 0; i < p->directory_count; i++) {
        if (!sb_config_root(&h->config, p->directories[i].id, p->directories[i].path, e))
            return false;
        h->config.workspaces[h->config.workspace_count - 1].read_only = p->directories[i].read_only;
        if (sb_platform_roots_overlap(h->lease->root, h->config.workspaces[i].root)) {
            sb_fail(e, "Administrator state overlaps a configured directory.");
            return false;
        }
    }
    h->execution = sb_execution_new(&h->config, e);
    if (!h->execution)
        return false;
    sb_posix_execution_instance_bind(h->execution, instance);
    h->audit = sb_audit_new(h->lease, p->audit_max_bytes, e);
    if (!h->audit)
        return false;
    h->store = sb_store_new(h->lease, p->id, p->policy_revision, instance, "administrator", e);
    if (!h->store)
        return false;
    h->policy = sb_policy_new(p->approval_ttl_seconds, h->audit, e);
    if (!h->policy)
        return false;
    sb_policy_store(h->policy, h->store);
    sb_policy_limit(h->policy, p->limits.approval_records);
    if (!sb_posix_execution_bind(h->execution, h, e))
        return false;
    h->owner = sb_owner_start(h, e);
    return h->owner && sb_audit_project(h->audit, "administrator_session_bound", p->id, p->policy_revision,
                                        (uint64_t)getpid());
}
int sb_posix_admin_agent(int argc, char **argv) {
    sb_error e = {0};
    const char *projects = NULL, *id = NULL, *data = NULL, *owner = NULL;
    int result = 2, listenfd = -1, fd = -1;
    bool bound = false;
    sb_project_registry registry = {0};
    sb_host h = {0};
    struct sockaddr_un address = {0};
    for (int i = 2; i < argc; i++) {
        const char **slot = !strcmp(argv[i], "--projects")    ? &projects
                            : !strcmp(argv[i], "--project")   ? &id
                            : !strcmp(argv[i], "--owner-uid") ? &owner
                            : !strcmp(argv[i], "--admin-dir") ? &data
                                                              : NULL;
        if (!slot || *slot || i + 1 >= argc) {
            sb_fail(&e, "Invalid administrator agent arguments.");
            goto done;
        }
        *slot = argv[++i];
    }
    if (getuid() != 0 || geteuid() != 0) {
        sb_fail(&e, "Administrator agent requires explicit local sudo/root authorization; it never requests "
                    "or records a password.");
        goto done;
    }
    if (!projects || !id || !data || !owner || !*owner || strspn(owner, "0123456789") != strlen(owner)) {
        sb_fail(&e, "Specify projects, project, owner-uid and admin-dir.");
        goto done;
    }
    unsigned long uid = strtoul(owner, NULL, 10);
    if (!uid || uid > 0xfffffffeUL || !getpwuid((uid_t)uid)) {
        sb_fail(&e, "A real non-root owner uid is required.");
        goto done;
    }
    if (!trusted_path(sb_posix_program(), false, &e) || !trusted_path(projects, false, &e) ||
        !trusted_path(data, true, &e))
        goto done;
    if (!sb_projects_load(&registry, projects, &e))
        goto done;
    const sb_project *p = sb_project_find(&registry, id);
    if (!eligible(p)) {
        sb_fail(&e, "Selected project does not grant administrator commands.");
        goto done;
    }
    char wanted[65];
    if (!digest(p, wanted, &e) || !endpoint((uid_t)uid, id, &address))
        goto done;
    listenfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listenfd < 0)
        goto done;
    fcntl(listenfd, F_SETFD, FD_CLOEXEC);
    /* Never unlink a preexisting endpoint, including an earlier live grant. */
    if (bind(listenfd, (struct sockaddr *)&address, sizeof(address))) {
        sb_posix_error(&e, "Bind exclusive administrator endpoint");
        goto done;
    }
    bound = true;
    if (chown(address.sun_path, (uid_t)uid, (gid_t)-1) || chmod(address.sun_path, 0600) ||
        listen(listenfd, 2)) {
        sb_posix_error(&e, "Protect administrator endpoint");
        goto done;
    }
    fprintf(stderr,
            "Administrator project %s: waiting up to 120 seconds for the configured owner's ordinary worker. "
            "Ctrl+C revokes this session.\n",
            id);
    uint64_t deadline = sb_platform_millis() + 120000;
    while (!sb_posix_signal && sb_platform_millis() < deadline) {
        struct pollfd pollfd = {.fd = listenfd, .events = POLLIN};
        if (poll(&pollfd, 1, 100) <= 0)
            continue;
        fd = accept(listenfd, NULL, NULL);
        if (fd < 0)
            continue;
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        if (!peer_uid(fd, (uid_t)uid) || !peer_program(fd)) {
            close(fd);
            fd = -1;
            continue;
        }
        char *wire = sb_posix_frame_read(fd, 3000, &e);
        yyjson_doc *hello = wire ? yyjson_read(wire, strlen(wire), 0) : NULL;
        free(wire);
        yyjson_val *v = yyjson_doc_get_root(hello);
        const char *hash = str(v, "project_sha256"), *instance = str(v, "instance");
        bool valid = hash && !strcmp(hash, wanted) && instance && strlen(instance) == 32 &&
                     strspn(instance, "0123456789abcdef") == 32;
        if (valid)
            valid = initialize(&h, p, data, instance, &e);
        yyjson_doc_free(hello);
        if (!valid) {
            close(fd);
            fd = -1;
            goto done;
        }
        if (!sb_posix_frame_write(fd, "{\"ready\":true}", 3000, &e))
            goto done;
        break;
    }
    if (fd < 0) {
        sb_fail(&e, "Administrator authorization expired before an ordinary project worker connected.");
        goto done;
    }
    close(listenfd);
    listenfd = -1;
    while (!sb_posix_signal && !sb_owner_stopping(h.owner)) {
        struct pollfd pollfd = {.fd = fd, .events = POLLIN};
        int n = poll(&pollfd, 1, 100);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 || (n > 0 && (pollfd.revents & (POLLHUP | POLLERR | POLLNVAL))))
            break;
        if (!n)
            continue;
        char *wire = sb_posix_frame_read(fd, 5000, &e);
        if (!wire)
            break;
        yyjson_doc *request = yyjson_read(wire, strlen(wire), 0);
        free(wire);
        yyjson_val *v = yyjson_doc_get_root(request);
        const char *actor = str(v, "actor"), *name = str(v, "name");
        yyjson_val *args = yyjson_obj_get(v, "args");
        bool allowed = actor && *actor && strlen(actor) < 64 && name &&
                       (!strncmp(name, "task_", 5) || !strcmp(name, "approval_status") ||
                        !strcmp(name, "approval_cancel")) &&
                       yyjson_is_obj(args);
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *reply = allowed && d ? sb_tool_call(&h, actor, name, args, d) : NULL;
        wire =
            reply ? sb_posix_json(d, reply) : sb_posix_json_error("Administrator command request rejected.");
        if (!reply)
            yyjson_mut_doc_free(d);
        yyjson_doc_free(request);
        bool sent = wire && sb_posix_frame_write(fd, wire, 5000, &e);
        free(wire);
        if (!sent)
            break;
    }
    result = 0;
done:
    if (fd >= 0)
        close(fd);
    if (listenfd >= 0)
        close(listenfd);
    if (bound)
        unlink(address.sun_path);
    sb_owner_stop(h.owner);
    sb_execution_free(h.execution);
    sb_policy_free(h.policy);
    sb_store_free(h.store);
    sb_audit_free(h.audit);
    sb_platform_lease_close(h.lease);
    sb_config_free(&h.config);
    sb_projects_free(&registry);
    if (result)
        fprintf(stderr, "ShellBot administrator: %s\n",
                *e.text ? e.text : "Native agent initialization failed.");
    return result;
}
