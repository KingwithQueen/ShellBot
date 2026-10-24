#include "admin.h"
#include "admin_internal.h"
#include "admin_desktop.h"
#include <sys/socket.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <pwd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

typedef struct {
    const sb_project *project;
    bool granted;
    int fd, exit_status;
    pid_t pid;
    unsigned generation;
    char digest[65], directory[SB_PATH_CAP], instance[33], state[24];
} admin_slot;
typedef struct admin_broker admin_broker;
struct admin_broker {
    sb_project_registry registry;
    sb_lease *lease;
    sb_audit *audit;
    sb_owner *owner;
    uid_t uid;
    pthread_mutex_t mutex;
    bool failed;
    int ui_fd;
    char id[33], snapshot[SB_PATH_CAP], snapshot_digest[65];
    admin_slot slots[SB_MAX_PROJECTS];
    bool (*spawn)(admin_broker *, admin_slot *, sb_error *);
};
static const char *broker_str(yyjson_val *v, const char *key) {
    const char *s = sb_string(yyjson_obj_get(v, key));
    return s ? s : "";
}
static bool broker_audit(admin_broker *b, admin_slot *s, const char *event) {
    bool ok = sb_audit_project(b->audit, event, s ? s->project->id : "admin-broker",
                               s ? s->project->policy_revision : 0, s ? (uint64_t)s->pid : (uint64_t)getpid());
    if (!ok)
        b->failed = true;
    return ok;
}
static bool snapshot_matches(admin_broker *b, sb_error *e) {
    size_t size;
    char hash[65], *bytes = sb_platform_load_config(b->snapshot, &size, e);
    bool ok = bytes && sb_platform_sha256(bytes, size, hash, e) && !strcmp(hash, b->snapshot_digest);
    free(bytes);
    if (!ok)
        sb_fail(e, "The authorized registry snapshot changed; a new explicit root authorization is required.");
    return ok;
}
static bool broker_spawn(admin_broker *b, admin_slot *s, sb_error *e) {
    char uid[24];
    snprintf(uid, sizeof(uid), "%u", (unsigned)b->uid);
    char *args[] = {(char *)sb_posix_program(), "--admin-agent", "--projects", b->snapshot,
                    "--project", (char *)s->project->id, "--owner-uid", uid, "--admin-dir",
                    s->directory, "--expected-project-sha256", s->digest, "--supervised", NULL};
    return sb_posix_spawn_channel(args, &s->fd, &s->pid, e);
}
static bool slot_start(admin_broker *b, admin_slot *s, sb_error *e) {
    if (!s->granted || s->pid || b->failed || !snapshot_matches(b, e)) {
        if (!*e->text)
            sb_fail(e, "Project is ungranted, already running, or the broker audit is unavailable.");
        return false;
    }
    s->instance[0] = 0;
    s->exit_status = 0;
    if (!(b->spawn ? b->spawn : broker_spawn)(b, s, e)) {
        snprintf(s->state, sizeof(s->state), "failed");
        return false;
    }
    s->generation++;
    snprintf(s->state, sizeof(s->state), "starting");
    return broker_audit(b, s, "administrator_agent_started");
}
static void slot_stop(admin_broker *b, admin_slot *s) {
    if (s->fd >= 0) {
        shutdown(s->fd, SHUT_RDWR);
        close(s->fd);
        s->fd = -1;
    }
    if (s->pid > 0) {
        /* Only an unreaped child owned by this broker is signalled. Never use
           a caller PID, a process name, or a stored PID from an earlier run. */
        int status = 0;
        pid_t reaped = waitpid(s->pid, &status, WNOHANG);
        if (reaped == 0) {
            kill(s->pid, SIGTERM);
            if (!sb_posix_wait_child(s->pid, 8000, &status)) {
                kill(s->pid, SIGKILL);
                sb_posix_wait_child(s->pid, 2000, &status);
            }
        }
        s->exit_status = status;
        broker_audit(b, s, "administrator_agent_stopped");
        s->pid = 0;
    }
    s->instance[0] = 0;
    snprintf(s->state, sizeof(s->state), "%s", s->granted ? "stopped" : "revoked");
}
static void slot_exited(admin_broker *b, admin_slot *s, int status) {
    s->exit_status = status;
    broker_audit(b, s, "administrator_agent_exited");
    if (s->fd >= 0)
        close(s->fd);
    s->fd = -1;
    s->pid = 0;
    s->instance[0] = 0;
    snprintf(s->state, sizeof(s->state), "%s", WIFEXITED(status) && !WEXITSTATUS(status) ? "stopped" : "failed");
}
static void broker_tick(admin_broker *b) {
    for (size_t i = 0; i < b->registry.project_count; i++) {
        admin_slot *s = &b->slots[i];
        if (!s->pid)
            continue;
        struct pollfd p = {.fd = s->fd, .events = POLLIN};
        if (s->fd >= 0 && poll(&p, 1, 0) > 0 && (p.revents & POLLIN)) {
            sb_error e = {0};
            char *wire = sb_posix_frame_read(s->fd, 1000, &e);
            if (!wire) {
                /* A clean exit closes the channel just before waitpid can reap
                   it. Do not report a protocol failure merely for that race. */
                int status = 0;
                if (sb_posix_wait_child(s->pid, 250, &status))
                    slot_exited(b, s, status);
                else {
                    slot_stop(b, s);
                    snprintf(s->state, sizeof(s->state), "failed");
                }
                continue;
            }
            yyjson_doc *d = wire ? yyjson_read(wire, strlen(wire), 0) : NULL;
            free(wire);
            yyjson_val *v = yyjson_doc_get_root(d);
            const char *state = broker_str(v, "state"), *instance = broker_str(v, "instance");
            static const char *const keys[] = {"state", "instance"};
            bool waiting = !strcmp(state, "waiting") && !*instance && !strcmp(s->state, "starting");
            bool connected = !strcmp(state, "connected") && strlen(instance) == 32 &&
                             strspn(instance, "0123456789abcdef") == 32 && !strcmp(s->state, "waiting");
            if (!sb_config_keys(v, keys, 2) || (!waiting && !connected)) {
                slot_stop(b, s);
                snprintf(s->state, sizeof(s->state), "failed");
            } else {
                snprintf(s->state, sizeof(s->state), "%s", state);
                snprintf(s->instance, sizeof(s->instance), "%s", instance);
                broker_audit(b, s, connected ? "administrator_agent_connected" : "administrator_agent_waiting");
            }
            yyjson_doc_free(d);
        }
        if (!s->pid)
            continue;
        int status = 0;
        pid_t reaped = waitpid(s->pid, &status, WNOHANG);
        if (reaped == s->pid || (reaped < 0 && errno == ECHILD)) {
            slot_exited(b, s, reaped > 0 ? status : 0x100);
        }
    }
}
static char *broker_status(admin_broker *b) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (!d)
        return NULL;
    yyjson_mut_val *v = yyjson_mut_obj(d), *items = yyjson_mut_arr(d);
    yyjson_mut_obj_add_str(d, v, "broker_id", b->id);
    yyjson_mut_obj_add_uint(d, v, "process_id", (uint64_t)getpid());
    yyjson_mut_obj_add_uint(d, v, "owner_uid", (uint64_t)b->uid);
    yyjson_mut_obj_add_str(d, v, "registry_sha256", b->snapshot_digest);
    yyjson_mut_obj_add_bool(d, v, "healthy", !b->failed);
    for (size_t i = 0; i < b->registry.project_count; i++) {
        admin_slot *s = &b->slots[i];
        yyjson_mut_val *item = yyjson_mut_obj(d);
        yyjson_mut_obj_add_strcpy(d, item, "project_id", s->project->id);
        yyjson_mut_obj_add_bool(d, item, "granted", s->granted);
        yyjson_mut_obj_add_str(d, item, "project_sha256", s->digest);
        yyjson_mut_obj_add_uint(d, item, "generation", s->generation);
        yyjson_mut_obj_add_uint(d, item, "process_id", (uint64_t)s->pid);
        yyjson_mut_obj_add_str(d, item, "state", s->state);
        yyjson_mut_obj_add_str(d, item, "host_instance_id", s->instance);
        yyjson_mut_obj_add_int(d, item, "wait_status", s->exit_status);
        yyjson_mut_arr_add_val(items, item);
    }
    yyjson_mut_obj_add_val(d, v, "projects", items);
    return sb_posix_json(d, v);
}
static char *broker_request_locked(admin_broker *b, const char *bytes, size_t size) {
    yyjson_doc *d = yyjson_read(bytes, size, 0);
    yyjson_val *v = yyjson_doc_get_root(d);
    const char *command = broker_str(v, "command"), *id = broker_str(v, "project_id");
    static const char *const status_keys[] = {"command"};
    static const char *const decision_keys[] = {"command", "project_id", "expected_broker_id",
                                              "expected_project_sha256", "expected_generation"};
    char *reply = NULL;
    broker_tick(b);
    if (!strcmp(command, "status") && sb_config_keys(v, status_keys, 1)) {
        reply = broker_status(b);
        goto done;
    }
    admin_slot *s = NULL;
    for (size_t i = 0; i < b->registry.project_count; i++)
        if (!strcmp(id, b->slots[i].project->id))
            s = &b->slots[i];
    bool project_owner = !strcmp(command, "project-owner");
    static const char *const owner_keys[] = {"command", "project_id", "expected_broker_id",
        "expected_project_sha256", "expected_generation", "request"};
    if ((!project_owner && strcmp(command, "revoke") && strcmp(command, "restart")) || !s ||
        !sb_config_keys(v, project_owner ? owner_keys : decision_keys, project_owner ? 6 : 5) || strcmp(broker_str(v, "expected_broker_id"), b->id) ||
        strcmp(broker_str(v, "expected_project_sha256"), s->digest) ||
        !yyjson_is_uint(yyjson_obj_get(v, "expected_generation")) ||
        yyjson_get_uint(yyjson_obj_get(v, "expected_generation")) != s->generation || !s->granted || b->failed) {
        reply = sb_posix_json_error("Invalid or stale broker decision, or project has no remaining grant.");
        goto done;
    }
    sb_error e = {0};
    if (project_owner) {
        yyjson_val *request = yyjson_obj_get(v, "request");
        const char *operation = broker_str(request, "command");
        bool allowed = !strcmp(operation, "list") || !strcmp(operation, "inspect") ||
            !strcmp(operation, "history") || !strcmp(operation, "approve") || !strcmp(operation, "deny") || !strcmp(operation, "revoke");
        if (!allowed || !s->pid || strcmp(s->state, "connected"))
            reply = sb_posix_json_error("管理员项目尚未连接，或该入口不接受此所有者操作。");
        else {
            char *wire = yyjson_val_write(request, 0, NULL);
            reply = wire ? sb_owner_exchange(s->directory, wire, strlen(wire), &e) : NULL;
            free(wire);
            if (!reply) reply = sb_posix_json_error(*e.text ? e.text : "Administrator approval channel failed.");
        }
        goto done;
    }
    if (!strcmp(command, "restart") && !snapshot_matches(b, &e)) {
        reply = sb_posix_json_error(e.text);
        goto done;
    }
    if (!broker_audit(b, s, !strcmp(command, "revoke") ? "administrator_project_revoked" : "administrator_project_restart")) {
        reply = sb_posix_json_error("Broker audit failed; all administrator sessions are being revoked.");
        goto done;
    }
    if (!strcmp(command, "revoke"))
        s->granted = false;
    slot_stop(b, s);
    /* A restart reuses only the original explicit grant. It never loads new
       projects or changed policy from the caller's current registry. */
    if (!strcmp(command, "restart") && !slot_start(b, s, &e))
        reply = sb_posix_json_error(e.text);
    else
        reply = broker_status(b);
done:
    yyjson_doc_free(d);
    return reply;
}
static char *broker_request(void *context, const char *bytes, size_t size) {
    admin_broker *b = context;
    pthread_mutex_lock(&b->mutex);
    char *reply = broker_request_locked(b, bytes, size);
    pthread_mutex_unlock(&b->mutex);
    return reply;
}
static bool broker_prepare(admin_broker *b, const char *bytes, size_t size, const char *data, sb_error *e) {
    if (!sb_posix_socket_preflight(data, e) || !sb_projects_parse(&b->registry, bytes, size, e))
        return false;
    unsigned granted = 0;
    for (size_t i = 0; i < b->registry.project_count; i++) {
        admin_slot *s = &b->slots[i];
        s->fd = -1;
        s->project = &b->registry.projects[i];
        s->granted = sb_admin_project_eligible(s->project);
        granted += s->granted;
        snprintf(s->state, sizeof(s->state), "%s", s->granted ? "stopped" : "ungranted");
        if (!sb_admin_project_digest(s->project, s->digest, e))
            return false;
    }
    if (!granted) {
        sb_fail(e, "Registry contains no enabled projects eligible for administrator operations.");
        return false;
    }
    b->lease = sb_platform_lease(data, e);
    if (!b->lease)
        return false;
    b->audit = sb_audit_new(b->lease, UINT64_C(16777216), e);
    if (!b->audit)
        return false;
    unsigned char nonce[16];
    if (!sb_platform_random(nonce, sizeof(nonce), e) || !sb_platform_sha256(bytes, size, b->snapshot_digest, e))
        return false;
    for (size_t i = 0; i < sizeof(nonce); i++)
        snprintf(b->id + i * 2, 3, "%02x", nonce[i]);
    char name[64];
    snprintf(name, sizeof(name), "grant-%s.json", b->id);
    int count = snprintf(b->snapshot, sizeof(b->snapshot), "%s/%s", data, name);
    if (count < 0 || (size_t)count >= sizeof(b->snapshot)) {
        sb_fail(e, "Administrator snapshot path is too long.");
        return false;
    }
    sb_journal *journal = sb_platform_journal_open(b->lease, name, e);
    bool saved = journal && sb_posix_write(journal->fd, bytes, size, e) && !fsync(journal->fd) &&
                 !fsync(b->lease->root->fd);
    sb_platform_journal_close(journal);
    if (!saved)
        return false;
    for (size_t i = 0; i < b->registry.project_count; i++) {
        admin_slot *s = &b->slots[i];
        if (!s->granted)
            continue;
        /* Stable short private names avoid placing caller-controlled identifiers
           in paths. The complete project identity remains bound by policy/state. */
        char hash[65];
        if (!sb_platform_sha256(s->project->id, strlen(s->project->id), hash, e))
            return false;
        snprintf(name, sizeof(name), "project-%.16s", hash);
        count = snprintf(s->directory, sizeof(s->directory), "%s/%s", data, name);
        if (count < 0 || (size_t)count >= sizeof(s->directory)) {
            sb_fail(e, "Administrator project state path is too long.");
            return false;
        }
        if (!sb_posix_socket_preflight(s->directory, e)) return false;
        for (size_t j = 0; j < i; j++)
            if (!strcmp(s->directory, b->slots[j].directory)) {
                sb_fail(e, "Administrator project state identity collision.");
                return false;
            }
        if (mkdirat(b->lease->root->fd, name, 0700) && errno != EEXIST) {
            sb_posix_error(e, "Create administrator project state");
            return false;
        }
        sb_root *r = sb_platform_root(s->directory, e);
        bool private = r && sb_posix_private(r, e);
        for (size_t j = 0; private && j < s->project->directory_count; j++) {
            sb_root *exposed = sb_platform_root(s->project->directories[j].path, e);
            private = exposed && !sb_platform_roots_overlap(b->lease->root, exposed);
            sb_platform_root_close(exposed);
        }
        sb_platform_root_close(r);
        if (!private) {
            if (!*e->text)
                sb_fail(e, "Broker state must be private and outside every granted project directory.");
            return false;
        }
        if (!broker_audit(b, s, "administrator_project_granted"))
            return false;
    }
    return broker_audit(b, NULL, "administrator_broker_started");
}
static void broker_release(admin_broker *b) {
    if (b->ui_fd >= 0) { shutdown(b->ui_fd, SHUT_RDWR); close(b->ui_fd); b->ui_fd = -1; }
    /* Close every channel before waiting, so all project grants are revoked
       together even if one agent is slow to finish cancellation. */
    for (size_t i = 0; i < b->registry.project_count; i++)
        if (b->slots[i].pid && b->slots[i].fd >= 0)
            shutdown(b->slots[i].fd, SHUT_RDWR);
    for (size_t i = 0; i < b->registry.project_count; i++)
        slot_stop(b, &b->slots[i]);
    if (b->audit)
        broker_audit(b, NULL, "administrator_broker_stopped");
    sb_audit_free(b->audit);
    sb_platform_lease_close(b->lease);
    sb_projects_free(&b->registry);
}
int sb_posix_admin_broker(int argc, char **argv) {
    sb_error e = {0};
    const char *projects = NULL, *data = NULL, *owner = NULL, *ui_path = NULL, *ui_pid = NULL, *expected = NULL;
    int result = 2;
    admin_broker *b = calloc(1, sizeof(*b));
    char *bytes = NULL;
    if (!b)
        return 2;
    b->ui_fd = -1;
    for (size_t i = 0; i < SB_MAX_PROJECTS; i++)
        b->slots[i].fd = -1;
    pthread_mutex_init(&b->mutex, NULL);
    for (int i = 2; i < argc; i++) {
        const char **slot = !strcmp(argv[i], "--projects") ? &projects : !strcmp(argv[i], "--owner-uid") ?
                            &owner : !strcmp(argv[i], "--admin-dir") ? &data :
                            !strcmp(argv[i], "--desktop-channel") ? &ui_path :
                            !strcmp(argv[i], "--desktop-pid") ? &ui_pid :
                            !strcmp(argv[i], "--expected-registry-sha256") ? &expected : NULL;
        if (!slot || *slot || i + 1 >= argc) {
            sb_fail(&e, "Invalid or duplicate shared administrator broker option.");
            goto done;
        }
        *slot = argv[++i];
    }
    if (getuid() != 0 || geteuid() != 0) {
        sb_fail(&e, "Shared administrator broker requires explicit local sudo/root authorization.");
        goto done;
    }
    if (!projects || !data || !owner || !*owner || strspn(owner, "0123456789") != strlen(owner)) {
        sb_fail(&e, "Specify projects, owner-uid and admin-dir.");
        goto done;
    }
    errno = 0;
    unsigned long uid = strtoul(owner, NULL, 10);
    if (errno || !uid || uid > 0xfffffffeUL || !getpwuid((uid_t)uid)) {
        sb_fail(&e, "A real non-root owner uid is required.");
        goto done;
    }
    b->uid = (uid_t)uid;
    unsigned long pid = 0;
    if (ui_path || ui_pid || expected) {
        errno = 0;
        if (ui_pid && *ui_pid && strspn(ui_pid, "0123456789") == strlen(ui_pid)) pid = strtoul(ui_pid, NULL, 10);
        if (!ui_path || !ui_pid || !expected || errno || pid <= 1 || pid > INT_MAX ||
            strlen(expected) != 64 || strspn(expected, "0123456789abcdef") != 64) {
            sb_fail(&e, "Native administrator startup requires a complete, bound UI request."); goto done;
        }
    }
    if (!sb_admin_trusted_path(sb_posix_program(), false, &e) ||
        !sb_admin_trusted_path(projects, false, &e) || !sb_admin_trusted_path(data, true, &e))
        goto done;
    size_t size;
    bytes = sb_platform_load_config(projects, &size, &e);
    if (expected) {
        char hash[65];
        if (!bytes || !sb_platform_sha256(bytes, size, hash, &e) || strcmp(hash, expected)) {
            sb_fail(&e, "Root registry does not exactly match the UI configuration reviewed for this authorization."); goto done;
        }
        b->ui_fd = sb_admin_desktop_connect(ui_path, b->uid, (pid_t)pid, &e);
        if (b->ui_fd < 0) goto done;
    }
    if (!bytes || !broker_prepare(b, bytes, size, data, &e))
        goto done;
    for (size_t i = 0; i < b->registry.project_count; i++)
        if (b->slots[i].granted && !slot_start(b, &b->slots[i], &e))
            goto done;
    b->owner = sb_owner_start_handler(data, broker_request, b, &e);
    if (!b->owner)
        goto done;
    if (b->ui_fd >= 0) {
        char *hello = broker_request(b, "{\"command\":\"status\"}", 20);
        bool sent = hello && sb_posix_frame_write(b->ui_fd, hello, 3000, &e);
        free(hello);
        if (!sent) goto done;
    }
    fprintf(stderr, "Shared administrator broker %s started. Only the frozen, explicitly allowed projects "
                    "are granted. Ctrl+C revokes all sessions.\n", b->id);
    while (!sb_posix_signal && !sb_owner_stopping(b->owner)) {
        pthread_mutex_lock(&b->mutex);
        broker_tick(b);
        bool failed = b->failed;
        pthread_mutex_unlock(&b->mutex);
        if (failed) {
            sb_fail(&e, "Shared administrator audit failed; revoking all project sessions.");
            goto done;
        }
        if (b->ui_fd >= 0) {
            struct pollfd p = {.fd = b->ui_fd, .events = POLLIN};
            if (poll(&p, 1, 0) > 0) {
                if (p.revents & (POLLHUP | POLLERR | POLLNVAL)) break;
                if (p.revents & POLLIN) {
                    char *wire = sb_posix_frame_read(b->ui_fd, 3000, &e);
                    if (!wire) break;
                    char *reply = broker_request(b, wire, strlen(wire)); free(wire);
                    bool sent = reply && sb_posix_frame_write(b->ui_fd, reply, 3000, &e); free(reply);
                    if (!sent) break;
                }
            }
        }
        struct timespec pause = {.tv_nsec = 50000000};
        nanosleep(&pause, NULL);
    }
    result = 0;
done:
    sb_owner_stop(b->owner);
    broker_release(b);
    pthread_mutex_destroy(&b->mutex);
    free(bytes);
    free(b);
    if (result)
        fprintf(stderr, "ShellBot administrator broker: %s\n", *e.text ? e.text : "Initialization failed.");
    return result;
}
