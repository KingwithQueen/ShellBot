/* The HTTP owner capability is deliberately narrower than local owner IPC. */
static bool remote_enabled(void *context) { return ((manager *)context)->owner_authorization != NULL; }
static bool remote_authorized(void *context, const char *authorization) {
    return sb_posix_secret(authorization, ((manager *)context)->owner_authorization);
}
static char *remote_call(void *context, const char *bytes, size_t length, int *status) {
    manager *m = context;
    *status = 400;
    yyjson_doc *input = length <= 65536 ? yyjson_read(bytes, length, 0) : NULL;
    yyjson_val *root = yyjson_doc_get_root(input);
    const char *command = str(root, "command"), *project = str(root, "project_id"), *backend = str(root, "backend");
    static const char *const keys[] = {"command", "project_id", "backend", "id", "expected_sha256",
        "expected_host_instance_id", "expected_policy_revision", "offset", "limit"};
    char *reply = NULL;
    if (!command || !sb_config_keys(root, keys, 9)) goto invalid;
    if (!strcmp(command, "projects")) {
        if (yyjson_obj_size(root) != 1) goto invalid;
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *v = yyjson_mut_obj(d), *items = yyjson_mut_arr(d);
        for (size_t i = 0; i < m->registry.project_count; i++) {
            worker *w = &m->workers[i];
            if (!w->project->enabled) continue;
            yyjson_mut_val *item = yyjson_mut_obj(d);
            yyjson_mut_obj_add_str(d, item, "project_id", w->project->id);
            const char *state = "busy";
            if (!pthread_mutex_trylock(&w->mutex)) {
                refresh(w); state = w->pid <= 0 ? "stopped" : w->paused ? "paused" : "running";
                pthread_mutex_unlock(&w->mutex);
            }
            yyjson_mut_obj_add_str(d, item, "state", state);
            yyjson_mut_arr_add_val(items, item);
        }
        yyjson_mut_obj_add_val(d, v, "projects", items);
        reply = sb_posix_json(d, v); *status = 200; goto done;
    }
    bool decision = !strcmp(command, "approve") || !strcmp(command, "deny") || !strcmp(command, "revoke");
    bool inspect = !strcmp(command, "inspect"), history = !strcmp(command, "history");
    const char *id = str(root, "id");
    if ((!decision && !inspect && !history && strcmp(command, "list")) || !sb_identifier(project) ||
        !backend || (strcmp(backend, "ordinary") && strcmp(backend, "administrator"))) goto invalid;
    if ((decision || inspect || id) && (!id || strlen(id) != 32 || strspn(id, "0123456789abcdef") != 32)) goto invalid;
    if (decision) {
        const char *hash = str(root, "expected_sha256"), *instance = str(root, "expected_host_instance_id");
        if (!hash || strlen(hash) != 64 || strspn(hash, "0123456789abcdef") != 64 || !instance ||
            strlen(instance) != 32 || strspn(instance, "0123456789abcdef") != 32 ||
            !yyjson_is_uint(yyjson_obj_get(root, "expected_policy_revision"))) goto invalid;
    } else if (yyjson_obj_get(root, "expected_sha256") || yyjson_obj_get(root, "expected_host_instance_id") ||
               yyjson_obj_get(root, "expected_policy_revision")) goto invalid;
    if (!history && (yyjson_obj_get(root, "offset") || yyjson_obj_get(root, "limit"))) goto invalid;
    if (!decision && !inspect && !history && id) goto invalid;
    worker *w = get_worker(m, project);
    if (!w || !w->project->enabled) { *status = 404; reply = sb_posix_json_error("Project unavailable."); goto done; }
    if (pthread_mutex_trylock(&w->mutex)) { *status = 429; reply = sb_posix_json_error("Project busy; no decision queued."); goto done; }
    refresh(w);
    if (w->pid <= 0) {
        *status = 503; reply = sb_posix_json_error("Project is stopped; explicit local startup is required.");
    } else {
        yyjson_mut_doc *d = yyjson_doc_mut_copy(input, NULL);
        yyjson_mut_val *v = yyjson_mut_doc_get_root(d);
        yyjson_mut_obj_remove_key(v, "project_id"); yyjson_mut_obj_remove_key(v, "backend");
        char *wire = sb_posix_json(d, v);
        sb_error e = {0};
        char *response = worker_exchange(w, !strcmp(backend, "ordinary") ? "remote-owner" : "remote-admin-owner", wire, NULL, &e);
        free(wire);
        yyjson_doc *parsed = response ? yyjson_read(response, strlen(response), 0) : NULL;
        const char *body = str(yyjson_doc_get_root(parsed), "body");
        reply = body ? sb_strdup(body) : sb_posix_json_error("Owner channel failed; inspect effects before retrying.");
        *status = body ? 200 : 503;
        yyjson_doc_free(parsed); free(response);
    }
    pthread_mutex_unlock(&w->mutex);
    goto done;
invalid:
    reply = sb_posix_json_error("Invalid limited owner request or missing inspected decision binding.");
done:
    yyjson_doc_free(input); return reply;
}
