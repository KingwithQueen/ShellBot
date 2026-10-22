#include "posix.h"
#include "sb_projects.h"
#include <sys/socket.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
struct sb_execution {
    char instance[33];
    bool enabled, healthy, monitor_started;
    atomic_bool quit;
    pthread_mutex_t mutex;
    pthread_t monitor;
    sb_host *host;
    int fd;
    pid_t pid;
    sb_resource_limits limits;
};
static const char *str(yyjson_val *v, const char *key) { return sb_string(yyjson_obj_get(v, key)); }
static bool persist(sb_execution *x, yyjson_val *v, const char *event) {
    if (!x->host || !x->host->store)
        return false;
    const char *id = str(v, "task_id"), *actor = str(v, "_actor"), *status = str(v, "status");
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *body = d ? yyjson_val_mut_copy(d, v) : NULL;
    if (body)
        yyjson_mut_obj_remove_str(body, "_actor");
    bool ok = body && id && actor && status &&
              sb_store_append(x->host->store, "task", id, actor, status, event, body) &&
              sb_audit_task(x->host->audit, event, actor, str(v, "request_key"), id, SB_EXEC_USER,
                            str(v, "request_sha256"), "lifecycle",
                            yyjson_get_uint(yyjson_obj_get(v, "process_id")));
    yyjson_mut_doc_free(d);
    if (!ok)
        x->healthy = false;
    return ok;
}
static yyjson_doc *exchange(sb_execution *x, const char *op, yyjson_val *args, sb_error *e) {
    if (x->fd < 0) {
        sb_fail(e, "Task supervisor is unavailable; refresh host status before starting new work.");
        return NULL;
    }
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (!d)
        return NULL;
    yyjson_mut_val *r = yyjson_mut_obj(d);
    yyjson_mut_obj_add_str(d, r, "op", op);
    yyjson_mut_obj_add_val(d, r, "args", args ? yyjson_val_mut_copy(d, args) : yyjson_mut_obj(d));
    char *wire = sb_posix_json(d, r);
    bool sent = wire && sb_posix_frame_write(x->fd, wire, 5000, e);
    free(wire);
    char *response = sent ? sb_posix_frame_read(x->fd, 6000, e) : NULL;
    yyjson_doc *reply = response ? yyjson_read(response, strlen(response), 0) : NULL;
    free(response);
    if (!reply) {
        x->healthy = false;
        close(x->fd);
        x->fd = -1;
        return NULL;
    }
    yyjson_val *events = yyjson_obj_get(yyjson_doc_get_root(reply), "_events");
    size_t i, n;
    yyjson_val *entry;
    yyjson_arr_foreach(events, i, n, entry) persist(x, entry, "task_snapshot");
    return reply;
}
static void cancel_locked(sb_execution *x) {
    sb_error e = {0};
    yyjson_doc *d = exchange(x, "cancel_all", NULL, &e);
    yyjson_doc_free(d);
}
static void *monitor(void *context) {
    sb_execution *x = context;
    while (!atomic_load(&x->quit)) {
        struct timespec pause = {.tv_nsec = 100000000};
        nanosleep(&pause, NULL);
        pthread_mutex_lock(&x->mutex);
        if (x->fd >= 0) {
            sb_error e = {0};
            yyjson_doc *d = exchange(x, "tick", NULL, &e);
            yyjson_doc_free(d);
            if (!x->healthy)
                cancel_locked(x);
        }
        pthread_mutex_unlock(&x->mutex);
    }
    return NULL;
}
sb_execution *sb_execution_new(const sb_config *c, sb_error *e) {
    sb_execution *x = calloc(1, sizeof(*x));
    unsigned char b[16];
    if (!x || !sb_platform_random(b, sizeof(b), e)) {
        free(x);
        return NULL;
    }
    x->fd = -1;
    x->enabled = c->execution_enabled;
    x->limits = c->limits;
    x->healthy = true;
    atomic_init(&x->quit, false);
    pthread_mutex_init(&x->mutex, NULL);
    for (unsigned i = 0; i < 16; i++)
        snprintf(x->instance + 2 * i, 3, "%02x", b[i]);
    if (x->enabled && (c->execution_profile != SB_EXEC_USER || geteuid() == 0)) {
        sb_fail(e, "POSIX commands require a non-root ordinary current-user project.");
        sb_execution_free(x);
        return NULL;
    }
    return x;
}
bool sb_posix_execution_bind(sb_execution *x, sb_host *h, sb_error *e) {
    if (!x)
        return false;
    x->host = h;
    if (!x->enabled)
        return true;
    if (!h->project || !h->store) {
        sb_fail(e, "Native commands require a project and durable state.");
        return false;
    }
    char *argv[] = {(char *)sb_posix_program(), "--task-agent", NULL};
    if (!sb_posix_spawn_channel(argv, &x->fd, &x->pid, e))
        return false;
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (!d)
        return false;
    yyjson_mut_val *v = yyjson_mut_obj(d);
    const char *keys[] = {"active_tasks",    "task_records",     "output_bytes",
                          "task_timeout_ms", "approval_records", "worker_memory_mb"};
    unsigned values[] = {x->limits.active_tasks,    x->limits.task_records,     x->limits.output_bytes,
                         x->limits.task_timeout_ms, x->limits.approval_records, x->limits.worker_memory_mb};
    for (unsigned i = 0; i < 6; i++)
        yyjson_mut_obj_add_uint(d, v, keys[i], values[i]);
    char *bytes = sb_posix_json(d, v);
    yyjson_doc *config = bytes ? yyjson_read(bytes, strlen(bytes), 0) : NULL;
    free(bytes);
    yyjson_doc *reply = config ? exchange(x, "configure", yyjson_doc_get_root(config), e) : NULL;
    yyjson_doc_free(config);
    bool ok = reply != NULL;
    yyjson_doc_free(reply);
    if (!ok)
        return false;
    if (pthread_create(&x->monitor, NULL, monitor, x)) {
        sb_fail(e, "Cannot start task lifecycle monitor.");
        return false;
    }
    x->monitor_started = true;
    return true;
}
void sb_execution_prepare_project(sb_execution *x, sb_host *h) {
    (void)x;
    (void)h;
}
void sb_posix_execution_cancel_all(sb_execution *x) {
    if (x && x->enabled) {
        pthread_mutex_lock(&x->mutex);
        cancel_locked(x);
        pthread_mutex_unlock(&x->mutex);
    }
}
void sb_execution_free(sb_execution *x) {
    if (!x)
        return;
    atomic_store(&x->quit, true);
    if (x->monitor_started)
        pthread_join(x->monitor, NULL);
    if (x->fd >= 0 && x->host && x->host->store) {
        cancel_locked(x);
        for (unsigned i = 0; i < 40; i++) {
            sb_error e = {0};
            yyjson_doc *r = exchange(x, "tick", NULL, &e);
            bool active = r && yyjson_get_uint(yyjson_obj_get(yyjson_doc_get_root(r), "_active"));
            yyjson_doc_free(r);
            if (!active)
                break;
            struct timespec pause = {.tv_nsec = 25000000};
            nanosleep(&pause, NULL);
        }
    }
    if (x->fd >= 0) {
        shutdown(x->fd, SHUT_RDWR);
        close(x->fd);
    }
    if (x->pid > 0) {
        int status;
        if (!sb_posix_wait_child(x->pid, 3000, &status)) {
            kill(x->pid, SIGTERM);
            if (!sb_posix_wait_child(x->pid, 1500, &status)) {
                kill(x->pid, SIGKILL);
                sb_posix_wait_child(x->pid, 1000, &status);
            }
        }
    }
    pthread_mutex_destroy(&x->mutex);
    free(x);
}
bool sb_execution_available(sb_execution *x) {
    if (!x)
        return false;
    pthread_mutex_lock(&x->mutex);
    bool ok = x->enabled && x->healthy && x->fd >= 0;
    pthread_mutex_unlock(&x->mutex);
    return ok;
}
bool sb_execution_restricted_verified(sb_execution *x) {
    (void)x;
    return false;
}
bool sb_execution_program_permitted(sb_execution *x, const char *p) {
    return sb_execution_available(x) && p && *p == '/';
}
const char *sb_execution_instance(sb_execution *x) { return x ? x->instance : ""; }
bool sb_execution_task_owned(sb_execution *x, const char *actor, const char *id) {
    if (!x || !x->enabled || !actor || !id)
        return false;
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (!d)
        return false;
    yyjson_mut_val *v = yyjson_mut_obj(d);
    yyjson_mut_obj_add_str(d, v, "actor", actor);
    yyjson_mut_obj_add_str(d, v, "task_id", id);
    char *wire = sb_posix_json(d, v);
    yyjson_doc *args = wire ? yyjson_read(wire, strlen(wire), 0) : NULL;
    free(wire);
    sb_error e = {0};
    pthread_mutex_lock(&x->mutex);
    yyjson_doc *reply = args ? exchange(x, "owned", yyjson_doc_get_root(args), &e) : NULL;
    bool ok = reply && !yyjson_get_bool(yyjson_obj_get(yyjson_doc_get_root(reply), "_error"));
    pthread_mutex_unlock(&x->mutex);
    yyjson_doc_free(args);
    yyjson_doc_free(reply);
    return ok;
}
yyjson_mut_val *sb_execution_status(sb_execution *x, yyjson_mut_doc *d) {
    yyjson_mut_val *v = yyjson_mut_obj(d);
    yyjson_mut_obj_add_bool(d, v, "available", sb_execution_available(x));
    yyjson_mut_obj_add_strcpy(d, v, "host_instance_id", sb_execution_instance(x));
    yyjson_mut_obj_add_str(d, v, "profile", "current_user");
    yyjson_mut_obj_add_bool(d, v, "restricted_verified", false);
    yyjson_mut_obj_add_str(d, v, "containment",
                           "process groups; current-user commands are not a filesystem sandbox");
#ifdef __linux__
    yyjson_mut_obj_add_str(d, v, "memory_limit", "per-command RLIMIT_AS; excludes supervisor and worker");
#else
    yyjson_mut_obj_add_str(d, v, "memory_limit", "worker_memory_mb is not enforced on macOS");
#endif
    return v;
}
char *sb_execution_task(sb_execution *x, const char *name, yyjson_val *args, sb_error *e) {
    if (!x || !x->enabled)
        return sb_posix_json_error("Command execution is disabled.");
    pthread_mutex_lock(&x->mutex);
    yyjson_doc *reply = NULL;
    bool changing = !strcmp(name, "task_start") || !strcmp(name, "task_input");
    if (changing && (!x->healthy || !sb_store_healthy(x->host->store) || !sb_audit_healthy(x->host->audit))) {
        sb_fail(e,
                "Durable state, audit or supervisor is unavailable; command and stdin effects are blocked.");
        goto done;
    }
    if (!strcmp(name, "task_start")) {
        reply = exchange(x, "prepare", args, e);
        yyjson_val *r = yyjson_doc_get_root(reply);
        if (!reply || yyjson_get_bool(yyjson_obj_get(r, "_error")) ||
            !yyjson_get_bool(yyjson_obj_get(r, "created_new")))
            goto done;
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *v = d ? yyjson_val_mut_copy(d, r) : NULL;
        if (v)
            yyjson_mut_obj_add_strcpy(d, v, "actor", str(args, "actor"));
        char *wire = d ? sb_posix_json(d, v) : NULL;
        yyjson_doc *commit = wire ? yyjson_read(wire, strlen(wire), 0) : NULL;
        free(wire);
        bool saved = persist(x, r, "task_reserved");
        yyjson_doc_free(reply);
        reply = commit ? exchange(x, saved ? "commit" : "abort", yyjson_doc_get_root(commit), e) : NULL;
        yyjson_doc_free(commit);
        if (!saved) {
            yyjson_doc_free(reply);
            reply = NULL;
            sb_fail(e, "Task intent could not be flushed; command was not launched.");
        }
    } else if (!strcmp(name, "task_input")) {
        reply = exchange(x, "input_prepare", args, e);
        yyjson_val *r = yyjson_doc_get_root(reply);
        if (!reply || yyjson_get_bool(yyjson_obj_get(r, "_error")))
            goto done;
        bool saved = persist(x, r, "task_input_reserved");
        yyjson_doc_free(reply);
        reply = NULL;
        if (saved)
            reply = exchange(x, "input_commit", args, e);
        else {
            cancel_locked(x);
            sb_fail(e, "Input intent could not be flushed; queued bytes were not delivered.");
        }
    } else
        reply = exchange(x, name, args, e);
done:;
    char *wire = NULL;
    if (reply) {
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *v = d ? yyjson_val_mut_copy(d, yyjson_doc_get_root(reply)) : NULL;
        if (v) {
            yyjson_mut_obj_remove_str(v, "_events");
            yyjson_mut_obj_remove_str(v, "_active");
            yyjson_mut_obj_remove_str(v, "_actor");
            if (!x->healthy)
                yyjson_mut_obj_add_str(d, v, "persistence_warning",
                                       "Task effects may have occurred; query the original task and owner "
                                       "history before any new request.");
        }
        wire = d ? sb_posix_json(d, v) : NULL;
        yyjson_doc_free(reply);
    }
    if (!x->healthy)
        cancel_locked(x);
    pthread_mutex_unlock(&x->mutex);
    return wire;
}
