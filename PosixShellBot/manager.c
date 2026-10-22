#include "posix.h"
#include "sb_projects.h"
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const sb_project *project;
    pthread_mutex_t mutex;
    pid_t pid;
    int fd, status;
    bool paused;
    char instance[33], reason[128];
    char *authorization;
} worker;
typedef struct {
    sb_project_registry registry;
    sb_lease *lease;
    sb_audit *audit;
    sb_owner *owner;
    sb_http_server *http;
    char *snapshot;
    worker workers[SB_MAX_PROJECTS];
} manager;
static const char *str(yyjson_val *v, const char *key) { return sb_string(yyjson_obj_get(v, key)); }
static char *ok(void) { return sb_strdup("{\"ok\":true}"); }
static char *worker_exchange(worker *w, const char *op, const char *wire, const sb_request_context *transport,
                             sb_error *e) {
    if (w->fd < 0) {
        sb_fail(e, "Project worker is stopped.");
        return NULL;
    }
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (!d)
        return NULL;
    yyjson_mut_val *v = yyjson_mut_obj(d);
    yyjson_mut_obj_add_str(d, v, "op", op);
    if (wire)
        yyjson_mut_obj_add_strcpy(d, v, "wire", wire);
    if (transport) {
        const char *keys[] = {"actor", "version", "method", "name"},
                   *values[] = {transport->actor, transport->version, transport->method, transport->name};
        for (unsigned i = 0; i < 4; i++)
            if (values[i])
                yyjson_mut_obj_add_strcpy(d, v, keys[i], values[i]);
    }
    char *request = sb_posix_json(d, v);
    bool sent = request && sb_posix_frame_write(w->fd, request, 5000, e);
    free(request);
    char *response = sent ? sb_posix_frame_read(w->fd, 8000, e) : NULL;
    if (!response) {
        /* A partial frame cannot be reused: close rather than assigning a late
           response to a different request. The worker observes EOF and exits. */
        close(w->fd);
        w->fd = -1;
        snprintf(w->reason, sizeof(w->reason), "private channel closed; inspect effects before restart");
    }
    return response;
}
int sb_posix_worker_run(sb_host *h, int fd) {
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (!sb_posix_peer(fd, true))
        return 2;
    sb_error e = {0};
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (!d)
        return 2;
    yyjson_mut_val *ready = yyjson_mut_obj(d);
    yyjson_mut_obj_add_bool(d, ready, "ready", true);
    yyjson_mut_obj_add_strcpy(d, ready, "host_instance_id", sb_execution_instance(h->execution));
    char *bytes = sb_posix_json(d, ready);
    bool sent = bytes && sb_posix_frame_write(fd, bytes, 3000, &e);
    free(bytes);
    if (!sent)
        return 2;
    bool paused = false;
    while (!sb_posix_signal && !sb_owner_stopping(h->owner)) {
        struct pollfd p = {.fd = fd, .events = POLLIN};
        int n = poll(&p, 1, 100);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 || (n > 0 && (p.revents & (POLLERR | POLLHUP | POLLNVAL))))
            break;
        if (!n)
            continue;
        bytes = sb_posix_frame_read(fd, 5000, &e);
        if (!bytes)
            break;
        yyjson_doc *request = yyjson_read(bytes, strlen(bytes), 0);
        free(bytes);
        yyjson_val *root = yyjson_doc_get_root(request);
        const char *op = str(root, "op"), *wire = str(root, "wire");
        char *reply = NULL;
        int status = 200;
        bool stop = false;
        if (op && !strcmp(op, "call") && wire && !paused) {
            const char *actor = str(root, "actor");
            if (!actor || strcmp(actor, "http-bearer"))
                reply = sb_strdup("{\"error\":\"Invalid private actor\"}");
            else {
                sb_request_context ctx = {.actor = actor,
                                          .project = h->project,
                                          .policy_revision = h->project->policy_revision,
                                          .version = str(root, "version"),
                                          .method = str(root, "method"),
                                          .name = str(root, "name")};
                size_t length;
                reply = sb_dispatch(h, NULL, wire, strlen(wire), &ctx, &length, &status);
            }
        } else if (op && !strcmp(op, "owner") && wire)
            reply = sb_policy_owner_request(h->policy, wire, strlen(wire));
        else if (op && !strcmp(op, "pause")) {
            paused = true;
            sb_posix_execution_cancel_all(h->execution);
            reply = ok();
        } else if (op && !strcmp(op, "resume")) {
            paused = false;
            reply = ok();
        } else if (op && !strcmp(op, "shutdown")) {
            sb_posix_execution_cancel_all(h->execution);
            reply = ok();
            stop = true;
        } else {
            reply = sb_strdup("{\"error\":\"Worker is paused or private operation is invalid\"}");
            status = 503;
        }
        d = yyjson_mut_doc_new(NULL);
        if (!d) {
            free(reply);
            yyjson_doc_free(request);
            break;
        }
        yyjson_mut_val *v = yyjson_mut_obj(d);
        yyjson_mut_obj_add_uint(d, v, "status", (unsigned)status);
        if (reply)
            yyjson_mut_obj_add_strcpy(d, v, "body", reply);
        char *response = sb_posix_json(d, v);
        free(reply);
        yyjson_doc_free(request);
        sent = response && sb_posix_frame_write(fd, response, 5000, &e);
        free(response);
        if (!sent || stop)
            break;
    }
    sb_posix_execution_cancel_all(h->execution);
    close(fd);
    return 0;
}
static void refresh(worker *w) {
    if (w->pid > 0) {
        int status;
        pid_t p = waitpid(w->pid, &status, WNOHANG);
        if (p == w->pid) {
            w->status = status;
            w->pid = 0;
            if (w->fd >= 0)
                close(w->fd);
            w->fd = -1;
            w->paused = false;
            snprintf(w->reason, sizeof(w->reason), "worker exited (%d); explicit start required",
                     WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
        }
    }
}
static bool start(manager *m, worker *w, sb_error *e) {
    refresh(w);
    if (w->pid > 0) {
        if (w->fd >= 0)
            return true;
        sb_fail(e, "The previous worker is still closing; stop it before starting another instance.");
        return false;
    }
    if (!w->project->enabled) {
        sb_fail(e, "Project is disabled.");
        return false;
    }
    if (!sb_audit_project(m->audit, "project_start_requested", w->project->id, w->project->policy_revision,
                          0)) {
        sb_fail(e, "Manager audit unavailable; project start blocked.");
        return false;
    }
    char *argv[] = {(char *)sb_posix_program(),
                    "--project-worker",
                    "--projects",
                    m->snapshot,
                    "--project",
                    (char *)w->project->id,
                    NULL};
    if (!sb_posix_spawn_channel(argv, &w->fd, &w->pid, e))
        return false;
    char *reply = sb_posix_frame_read(w->fd, 8000, e);
    yyjson_doc *d = reply ? yyjson_read(reply, strlen(reply), 0) : NULL;
    free(reply);
    yyjson_val *v = yyjson_doc_get_root(d);
    const char *instance = str(v, "host_instance_id");
    bool ready = yyjson_get_bool(yyjson_obj_get(v, "ready")) && instance && strlen(instance) == 32;
    if (ready)
        strcpy(w->instance, instance);
    yyjson_doc_free(d);
    if (!ready) {
        close(w->fd);
        w->fd = -1;
        int status;
        if (!sb_posix_wait_child(w->pid, 1500, &status)) {
            kill(w->pid, SIGKILL);
            sb_posix_wait_child(w->pid, 2000, &status);
        }
        w->pid = 0;
        snprintf(w->reason, sizeof(w->reason), "worker initialization failed");
        sb_fail(
            e, "Project worker initialization failed; inspect project configuration and private Data locks.");
        return false;
    }
    w->paused = false;
    w->reason[0] = 0;
    if (!sb_audit_project(m->audit, "project_started", w->project->id, w->project->policy_revision,
                          (uint64_t)w->pid)) {
        sb_fail(e,
                "Worker started but manager audit could not be flushed; inspect its status before retrying.");
        return false;
    }
    return true;
}
static void stop(manager *m, worker *w) {
    refresh(w);
    if (w->pid <= 0)
        return;
    sb_error e = {0};
    char *reply = worker_exchange(w, "shutdown", NULL, NULL, &e);
    free(reply);
    if (w->fd >= 0)
        close(w->fd);
    w->fd = -1;
    int status;
    if (!sb_posix_wait_child(w->pid, 3000, &status)) {
        kill(w->pid, SIGTERM);
        if (!sb_posix_wait_child(w->pid, 2000, &status)) {
            kill(w->pid, SIGKILL);
            sb_posix_wait_child(w->pid, 2000, &status);
        }
    }
    sb_audit_project(m->audit, "project_stopped", w->project->id, w->project->policy_revision,
                     (uint64_t)w->pid);
    w->pid = 0;
    w->paused = false;
    snprintf(w->reason, sizeof(w->reason), "stopped by local owner");
}
static worker *get_worker(manager *m, const char *id) {
    if (!id)
        return NULL;
    for (size_t i = 0; i < m->registry.project_count; i++)
        if (!strcmp(id, m->workers[i].project->id))
            return &m->workers[i];
    return NULL;
}
static yyjson_mut_val *worker_status(worker *w, yyjson_mut_doc *d) {
    refresh(w);
    yyjson_mut_val *v = yyjson_mut_obj(d);
    yyjson_mut_obj_add_str(d, v, "project_id", w->project->id);
    yyjson_mut_obj_add_bool(d, v, "enabled", w->project->enabled);
    yyjson_mut_obj_add_uint(d, v, "policy_revision", w->project->policy_revision);
    yyjson_mut_obj_add_bool(d, v, "running", w->pid > 0);
    yyjson_mut_obj_add_bool(d, v, "paused", w->paused);
    yyjson_mut_obj_add_uint(d, v, "process_id", w->pid > 0 ? (uint64_t)w->pid : 0);
    yyjson_mut_obj_add_strcpy(d, v, "host_instance_id", w->instance);
    yyjson_mut_obj_add_strcpy(d, v, "reason", w->reason);
    return v;
}
static char *owner_request(void *context, const char *request, size_t length) {
    manager *m = context;
    yyjson_doc *parsed = yyjson_read(request, length, 0);
    yyjson_val *root = yyjson_doc_get_root(parsed);
    const char *command = str(root, "command"), *id = str(root, "project_id");
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *v = d ? yyjson_mut_obj(d) : NULL;
    sb_error e = {0};
    if (!command || !v)
        goto invalid;
    if (!strcmp(command, "status")) {
        yyjson_mut_val *list = yyjson_mut_arr(d);
        for (size_t i = 0; i < m->registry.project_count; i++) {
            worker *w = &m->workers[i];
            pthread_mutex_lock(&w->mutex);
            yyjson_mut_arr_add_val(list, worker_status(w, d));
            pthread_mutex_unlock(&w->mutex);
        }
        yyjson_mut_obj_add_val(d, v, "projects", list);
        yyjson_mut_obj_add_uint(d, v, "manager_pid", (uint64_t)getpid());
        yyjson_mut_obj_add_bool(d, v, "http", m->http != NULL);
    } else {
        worker *w = get_worker(m, id);
        if (!w)
            goto invalid;
        pthread_mutex_lock(&w->mutex);
        refresh(w);
        if (!strcmp(command, "start")) {
            if (!start(m, w, &e))
                yyjson_mut_obj_add_strcpy(d, v, "error", e.text);
        } else if (!strcmp(command, "stop_project"))
            stop(m, w);
        else if (!strcmp(command, "pause") || !strcmp(command, "resume")) {
            bool pause = !strcmp(command, "pause");
            if (w->pid <= 0)
                sb_fail(&e, "Project worker is stopped.");
            else {
                char *r = worker_exchange(w, pause ? "pause" : "resume", NULL, NULL, &e);
                if (r) {
                    w->paused = pause;
                    free(r);
                    sb_audit_project(m->audit, pause ? "project_paused" : "project_resumed", w->project->id,
                                     w->project->policy_revision, (uint64_t)w->pid);
                }
            }
            if (*e.text)
                yyjson_mut_obj_add_strcpy(d, v, "error", e.text);
        } else if (!strcmp(command, "owner")) {
            const char *wire = str(root, "request");
            char *response = wire ? worker_exchange(w, "owner", wire, NULL, &e) : NULL;
            yyjson_doc *reply = response ? yyjson_read(response, strlen(response), 0) : NULL;
            const char *body = str(yyjson_doc_get_root(reply), "body");
            if (body)
                yyjson_mut_obj_add_strcpy(d, v, "response", body);
            else
                yyjson_mut_obj_add_strcpy(d, v, "error", *e.text ? e.text : "Invalid project owner request.");
            yyjson_doc_free(reply);
            free(response);
        } else if (strcmp(command, "project_status")) {
            pthread_mutex_unlock(&w->mutex);
            goto invalid;
        }
        yyjson_mut_obj_add_val(d, v, "project", worker_status(w, d));
        pthread_mutex_unlock(&w->mutex);
    }
    yyjson_doc_free(parsed);
    return sb_posix_json(d, v);
invalid:
    yyjson_doc_free(parsed);
    yyjson_mut_doc_free(d);
    return sb_strdup("{\"error\":\"Invalid manager command or project id\"}");
}
static void *route_open(void *context, const char *path, const char *auth, bool *health, int *status) {
    manager *m = context;
    worker *w = NULL;
    (void)health;
    if (!strcmp(path, "/mcp"))
        w = get_worker(m, m->registry.default_project_id);
    else {
        for (size_t i = 0; i < m->registry.project_count; i++) {
            char endpoint[100];
            snprintf(endpoint, sizeof(endpoint), "/projects/%s/mcp", m->workers[i].project->id);
            if (!strcmp(path, endpoint))
                w = &m->workers[i];
        }
    }
    if (!w) {
        *status = 404;
        return NULL;
    }
    if (!sb_posix_secret(auth, w->authorization)) {
        *status = 401;
        return NULL;
    }
    pthread_mutex_lock(&w->mutex);
    refresh(w);
    if (w->pid <= 0 || w->fd < 0 || w->paused || !w->project->enabled) {
        pthread_mutex_unlock(&w->mutex);
        *status = 503;
        return NULL;
    }
    return w;
}
static char *route_call(void *context, void *binding, const char *request, size_t length,
                        const sb_request_context *transport, size_t *out_length, int *status) {
    (void)context;
    (void)length;
    worker *w = binding;
    sb_error e = {0};
    char *reply = worker_exchange(w, "call", request, transport, &e);
    yyjson_doc *d = reply ? yyjson_read(reply, strlen(reply), 0) : NULL;
    free(reply);
    yyjson_val *v = yyjson_doc_get_root(d);
    const char *body = str(v, "body");
    *status = d ? (int)yyjson_get_uint(yyjson_obj_get(v, "status")) : 503;
    char *result = body ? sb_strdup(body) : NULL;
    yyjson_doc_free(d);
    *out_length = result ? strlen(result) : 0;
    return result;
}
static void route_close(void *context, void *binding) {
    (void)context;
    worker *w = binding;
    pthread_mutex_unlock(&w->mutex);
}
static bool validate_directories(manager *m, sb_error *e) {
    sb_root *data[SB_MAX_PROJECTS] = {0};
    bool ok = false;
    for (size_t i = 0; i < m->registry.project_count; i++) {
        const sb_project *p = &m->registry.projects[i];
        sb_lease *lease = sb_platform_lease(p->data_dir, e);
        if (!lease)
            goto done;
        sb_platform_lease_close(lease);
        data[i] = sb_platform_root(p->data_dir, e);
        if (!data[i] || sb_platform_roots_overlap(data[i], m->lease->root)) {
            sb_fail(e, "Manager Data and project Data must be separate private directories.");
            goto done;
        }
        for (size_t j = 0; j < i; j++)
            if (sb_platform_roots_overlap(data[i], data[j])) {
                sb_fail(e, "Project Data directories overlap.");
                goto done;
            }
    }
    for (size_t i = 0; i < m->registry.project_count; i++) {
        const sb_project *p = &m->registry.projects[i];
        for (size_t j = 0; j < p->directory_count; j++) {
            sb_root *r = sb_platform_root(p->directories[j].path, e);
            if (!r)
                goto done;
            bool overlap = sb_platform_roots_overlap(r, m->lease->root);
            for (size_t k = 0; k < m->registry.project_count; k++)
                overlap |= sb_platform_roots_overlap(r, data[k]);
            sb_platform_root_close(r);
            if (overlap) {
                sb_fail(e, "Private manager/project Data overlaps a configured file root.");
                goto done;
            }
        }
    }
    ok = true;
done:
    for (size_t i = 0; i < SB_MAX_PROJECTS; i++)
        sb_platform_root_close(data[i]);
    return ok;
}
bool sb_posix_manager_option(const char *s) {
    const char *options[] = {
        "--manager",      "--manager-data-dir", "--management-status", "--project-status", "--project-start",
        "--project-stop", "--project-pause",    "--project-resume",    "--project-owner",  "--manager-stop",
        "--project-list", "--project-create",   "--project-configure", "--project-remove"};
    for (unsigned i = 0; i < sizeof(options) / sizeof(*options); i++)
        if (!strcmp(s, options[i]))
            return true;
    return false;
}
int sb_posix_manager_cli(int argc, char **argv) {
    const char *projects = NULL, *data = NULL, *command = NULL, *id = NULL, *definition = NULL,
               *output = NULL, *backup = NULL, *request_file = NULL;
    const char *starts[SB_MAX_PROJECTS];
    size_t start_count = 0;
    bool launch = false, http = false, list = false;
    sb_error e = {0};
    int result = 2;
    for (int i = 1; i < argc; i++) {
        const char *s = argv[i];
        if (!strcmp(s, "--manager")) {
            if (launch)
                goto bad;
            launch = true;
        } else if (!strcmp(s, "--http")) {
            if (http)
                goto bad;
            http = true;
        } else if (!strcmp(s, "--start-project")) {
            if (i + 1 >= argc || start_count == SB_MAX_PROJECTS)
                goto bad;
            starts[start_count++] = argv[++i];
        } else if (!strcmp(s, "--management-status") || !strcmp(s, "--manager-stop")) {
            if (command)
                goto bad;
            command = !strcmp(s, "--manager-stop") ? "stop" : "status";
        } else if (!strcmp(s, "--project-status") || !strcmp(s, "--project-start") ||
                   !strcmp(s, "--project-stop") || !strcmp(s, "--project-pause") ||
                   !strcmp(s, "--project-resume") || !strcmp(s, "--project-owner") ||
                   !strcmp(s, "--project-remove") || !strcmp(s, "--project-configure")) {
            if (command || i + 1 >= argc)
                goto bad;
            command = !strcmp(s, "--project-status") ? "project_status"
                      : !strcmp(s, "--project-stop") ? "stop_project"
                                                     : s + 10;
            id = argv[++i];
        } else {
            const char **slot = !strcmp(s, "--projects")           ? &projects
                                : !strcmp(s, "--manager-data-dir") ? &data
                                : !strcmp(s, "--project-definition") || !strcmp(s, "--project-create")
                                    ? &definition
                                : !strcmp(s, "--output")       ? &output
                                : !strcmp(s, "--backup")       ? &backup
                                : !strcmp(s, "--request")      ? &request_file
                                : !strcmp(s, "--project-list") ? &projects
                                                               : NULL;
            if (!slot || *slot || i + 1 >= argc)
                goto bad;
            if (!strcmp(s, "--project-list"))
                list = true;
            if (!strcmp(s, "--project-create")) {
                if (command)
                    goto bad;
                command = "create";
            }
            *slot = argv[++i];
        }
    }
    if (command && !launch && !list && !start_count && !http && !projects && !definition && !output &&
        !backup) {
        if (!data)
            goto bad;
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        if (!d)
            goto done;
        yyjson_mut_val *v = yyjson_mut_obj(d);
        yyjson_mut_obj_add_str(d, v, "command", command);
        if (id)
            yyjson_mut_obj_add_str(d, v, "project_id", id);
        if (request_file) {
            size_t n;
            char *bytes = sb_platform_load_config(request_file, &n, &e);
            if (!bytes) {
                yyjson_mut_doc_free(d);
                goto done;
            }
            yyjson_mut_obj_add_strcpy(d, v, "request", bytes);
            free(bytes);
        }
        char *request = sb_posix_json(d, v),
             *reply = request ? sb_owner_exchange(data, request, strlen(request), &e) : NULL;
        free(request);
        if (reply) {
            puts(reply);
            yyjson_doc *p = yyjson_read(reply, strlen(reply), 0);
            result = p && !yyjson_obj_get(yyjson_doc_get_root(p), "error") ? 0 : 2;
            yyjson_doc_free(p);
            free(reply);
        }
        goto done;
    }
    if (list && !command && !launch && projects && !data && !http && !definition && !output && !backup &&
        !start_count) {
        sb_project_registry r = {0};
        if (sb_projects_load(&r, projects, &e)) {
            char *summary = sb_projects_summary(&r);
            if (summary) {
                puts(summary);
                free(summary);
                result = 0;
            }
        }
        sb_projects_free(&r);
        goto done;
    }
    if (command && projects && !launch && !data && !http && !start_count && !request_file && output &&
        backup) {
        size_t n = 0, pn = 0;
        char *original = sb_platform_load_config(projects, &n, &e),
             *project = definition ? sb_platform_load_config(definition, &pn, &e) : NULL, *edited = NULL;
        if (original) {
            if (!strcmp(command, "remove") && id && !definition)
                edited = sb_projects_remove(original, n, id, &e);
            else if ((!strcmp(command, "create") || !strcmp(command, "configure")) && project) {
                yyjson_doc *definition_doc = yyjson_read(project, pn, 0);
                const char *edit_id = id ? id : str(yyjson_doc_get_root(definition_doc), "id");
                edited =
                    sb_projects_edit(original, n, project, pn, edit_id, !strcmp(command, "configure"), &e);
                yyjson_doc_free(definition_doc);
            }
        }
        if (edited && sb_platform_export_config(output, backup, edited, strlen(edited), original, n, &e)) {
            puts("New registry and original backup exported; restart the manager to apply the registry.");
            result = 0;
        }
        free(original);
        free(project);
        free(edited);
        goto done;
    }
    if (!launch || !projects || !data || command || definition || output || backup || list || request_file)
        goto bad;
    manager *m = calloc(1, sizeof(*m));
    if (!m)
        goto done;
    size_t initialized = 0;
    size_t n = 0;
    char *original = sb_platform_load_config(projects, &n, &e);
    if (!original || !sb_projects_parse(&m->registry, original, n, &e)) {
        free(original);
        goto cleanup;
    }
    m->lease = sb_platform_lease(data, &e);
    if (!m->lease || !validate_directories(m, &e)) {
        free(original);
        goto cleanup;
    }
    m->audit = sb_audit_new(m->lease, SB_AUDIT_LIMIT, &e);
    if (!m->audit) {
        free(original);
        goto cleanup;
    }
    char nonce[33];
    unsigned char b[16];
    if (!sb_platform_random(b, sizeof(b), &e)) {
        free(original);
        goto cleanup;
    }
    for (unsigned i = 0; i < 16; i++)
        snprintf(nonce + 2 * i, 3, "%02x", b[i]);
    size_t cap = strlen(data) + 80;
    m->snapshot = malloc(cap);
    char *saved = malloc(cap);
    if (!m->snapshot || !saved) {
        free(saved);
        free(original);
        goto cleanup;
    }
    snprintf(m->snapshot, cap, "%s/registry-%s.json", data, nonce);
    snprintf(saved, cap, "%s/original-%s.json", data, nonce);
    bool exported = sb_platform_export_config(m->snapshot, saved, original, n, original, n, &e);
    free(saved);
    free(original);
    if (!exported)
        goto cleanup;
    for (size_t i = 0; i < m->registry.project_count; i++) {
        worker *w = &m->workers[i];
        w->project = &m->registry.projects[i];
        w->fd = -1;
        pthread_mutex_init(&w->mutex, NULL);
        initialized++;
        if (http && w->project->enabled) {
            w->authorization = sb_posix_bearer(w->project->token_env, &e);
            if (!w->authorization)
                goto cleanup;
            for (size_t j = 0; j < i; j++)
                if (sb_posix_secret(w->authorization, m->workers[j].authorization)) {
                    sb_fail(&e, "Projects must use different bearer credentials.");
                    goto cleanup;
                }
        }
    }
    for (size_t i = 0; i < start_count; i++) {
        worker *w = get_worker(m, starts[i]);
        if (!w || !start(m, w, &e)) {
            if (!w)
                sb_fail(&e, "Unknown start-project id.");
            goto cleanup;
        }
    }
    if (http) {
        sb_config c;
        sb_config_defaults(&c);
        c.port = m->registry.port;
        if (*m->registry.public_origin && !sb_config_public_url(&c, m->registry.public_origin, &e))
            goto cleanup;
        sb_http_routes routes = {.open = route_open, .call = route_call, .close = route_close};
        m->http = sb_http_start(&c, &routes, m, &e);
        if (!m->http)
            goto cleanup;
    }
    m->owner = sb_owner_start_handler(data, owner_request, m, &e);
    if (!m->owner)
        goto cleanup;
    while (!sb_posix_signal && !sb_owner_stopping(m->owner)) {
        for (size_t i = 0; i < initialized; i++) {
            worker *w = &m->workers[i];
            if (!pthread_mutex_trylock(&w->mutex)) {
                refresh(w);
                pthread_mutex_unlock(&w->mutex);
            }
        }
        struct timespec pause = {.tv_nsec = 100000000};
        nanosleep(&pause, NULL);
    }
    result = 0;
cleanup:
    sb_http_stop(m->http);
    sb_owner_stop(m->owner);
    for (size_t i = 0; i < initialized; i++) {
        worker *w = &m->workers[i];
        stop(m, w);
        pthread_mutex_destroy(&w->mutex);
        if (w->authorization) {
            memset(w->authorization, 0, strlen(w->authorization));
            free(w->authorization);
        }
    }
    sb_audit_free(m->audit);
    sb_platform_lease_close(m->lease);
    sb_projects_free(&m->registry);
    free(m->snapshot);
    free(m);
    goto done;
bad:
    sb_fail(&e, "Invalid manager arguments. Use --help for project management commands.");
done:
    if (result && *e.text)
        fprintf(stderr, "ShellBot: %s\n", e.text);
    return result;
}
