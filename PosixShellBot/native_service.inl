/* The native window manages one explicitly configured backend registration.
   Login autostart and administrator operation grants remain independent. */
static void service_clear_preview(sb_native_manager *g) {
    yyjson_doc_free(g->service_preview); g->service_preview = NULL;
    g->service_plan_hash[0] = g->service_preview_options[0] = g->service_preview_registry[0] = 0;
}
static bool service_json_hash(yyjson_val *v, char hash[65], sb_error *e) {
    char *bytes = v ? yyjson_val_write(v, 0, NULL) : NULL;
    bool ok = bytes && sb_platform_sha256(bytes, strlen(bytes), hash, e); free(bytes); return ok;
}
static void service_defaults(sb_native_manager *g, char name[64], char user[SB_PATH_CAP], char system[SB_PATH_CAP]) {
    char hash[65] = {0}; sb_error e = {0}; sb_platform_sha256(g->directory, strlen(g->directory), hash, &e);
    snprintf(name, 64, "backend-%.20s", hash); snprintf(user, SB_PATH_CAP, "%s/service", g->directory);
#ifdef __APPLE__
    snprintf(system, SB_PATH_CAP, "/Library/Application Support/ShellBotStartup-ui-%.20s", hash);
#else
    snprintf(system, SB_PATH_CAP, "/var/lib/ShellBotStartup-ui-%.20s", hash);
#endif
}
static yyjson_doc *service_options(sb_native_manager *g) {
    yyjson_doc *s = settings(g); yyjson_val *saved = yyjson_obj_get(yyjson_doc_get_root(s), "backend_service");
    if (saved) { char *bytes = yyjson_val_write(saved, 0, NULL); yyjson_doc_free(s); return parse(bytes); }
    char name[64], user[SB_PATH_CAP], system[SB_PATH_CAP]; service_defaults(g, name, user, system);
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL); yyjson_mut_val *v = yyjson_mut_obj(d);
    yyjson_mut_obj_add_str(d, v, "kind", "user-service"); yyjson_mut_obj_add_strcpy(d, v, "name", name);
    yyjson_mut_obj_add_strcpy(d, v, "directory", user); yyjson_mut_obj_add_strcpy(d, v, "key", g->key);
    yyjson_mut_obj_add_bool(d, v, "oauth", sb_gui_bool(yyjson_doc_get_root(s), "oauth"));
    yyjson_mut_obj_add_val(d, v, "starts", yyjson_mut_arr(d)); yyjson_doc_free(s);
    return parse(sb_posix_json(d, v));
}
static bool service_system(yyjson_val *v) { return !strcmp(sb_gui_string(v, "kind"), "system-service"); }
static bool service_valid(yyjson_val *v, sb_error *e) {
    const char *kind = sb_gui_string(v, "kind"), *name = sb_gui_string(v, "name"), *path = sb_gui_string(v, "directory"), *key = sb_gui_string(v, "key");
    yyjson_val *starts = yyjson_obj_get(v, "starts"), *oauth = yyjson_obj_get(v, "oauth");
    if (!yyjson_is_obj(v) || (strcmp(kind, "user-service") && strcmp(kind, "system-service")) || !sb_identifier(name) || strlen(name) > 48 ||
        !sb_project_path_valid(path) || strpbrk(path, "\r\n\t") || (*key && (!sb_project_path_valid(key) || strpbrk(key, "\r\n\t"))) ||
        !yyjson_is_bool(oauth) || !yyjson_is_arr(starts) || yyjson_arr_size(starts) > SB_MAX_PROJECTS) goto bad;
    size_t i, n; yyjson_val *item;
    yyjson_arr_foreach(starts, i, n, item) {
        const char *id = yyjson_get_str(item);
        if (!id || yyjson_get_len(item) != strlen(id) || !sb_identifier(id)) goto bad;
        for (size_t j = 0; j < i; j++) if (!strcmp(id, yyjson_get_str(yyjson_arr_get(starts, j)))) goto bad;
    }
    return true;
bad:
    sb_fail(e, "服务设置需要有效类型、名称、规范绝对路径、独立启动项目列表和 OAuth 开关。"); return false;
}
static yyjson_doc *service_form(sb_native_manager *g, yyjson_val *q, sb_error *e) {
    yyjson_doc *old = service_options(g); yyjson_mut_doc *d = yyjson_doc_mut_copy(old, NULL);
    yyjson_mut_val *v = yyjson_mut_doc_get_root(d);
    const char *keys[] = {"kind", "name", "directory", "key", "oauth", "starts"};
    for (unsigned i = 0; i < 6; i++) {
        yyjson_val *value = yyjson_obj_get(q, keys[i]);
        if (value) { yyjson_mut_obj_remove_key(v, keys[i]); yyjson_mut_obj_add_val(d, v, keys[i], yyjson_val_mut_copy(d, value)); }
    }
    if (yyjson_obj_get(q, "kind") && !yyjson_obj_get(q, "directory") && strcmp(sb_gui_string(q, "kind"), sb_gui_string(yyjson_doc_get_root(old), "kind"))) {
        char name[64], user[SB_PATH_CAP], system[SB_PATH_CAP]; service_defaults(g, name, user, system);
        yyjson_mut_obj_remove_key(v, "directory"); yyjson_mut_obj_add_strcpy(d, v, "directory", service_system(q) ? system : user);
    }
    yyjson_doc *out = parse(sb_posix_json(d, v)); yyjson_doc_free(old);
    if (!out || !service_valid(yyjson_doc_get_root(out), e)) { yyjson_doc_free(out); return NULL; }
    return out;
}
static bool service_binding(sb_native_manager *g, yyjson_val *s, yyjson_val *o, sb_error *e) {
    if (!sb_gui_bool(s, "registered")) return true;
    yyjson_val *args = yyjson_obj_get(s, "argv");
    bool project = false, data = false, valid = yyjson_is_arr(args), http = false, store = false, oauth = false, key_seen = false;
    size_t starts_seen = 0; yyjson_val *starts = yyjson_obj_get(o, "starts");
    bool manager = yyjson_arr_size(args) > 1 && !strcmp(sb_gui_string(s, "kind"), sb_gui_string(o, "kind"));
    const char *mode = yyjson_get_str(yyjson_arr_get(args, 1)); manager = manager && mode && !strcmp(mode, "--manager");
    size_t n = yyjson_arr_size(args);
    for (size_t i = 0; i < n; i++) {
        yyjson_val *a = yyjson_arr_get(args, i);
        const char *arg = yyjson_get_str(a);
        if (!arg || yyjson_get_len(a) != strlen(arg)) { valid = false; break; }
        if (i < 2) continue;
        const char *next = i + 1 < n ? yyjson_get_str(yyjson_arr_get(args, i + 1)) : NULL;
        if (!strcmp(arg, "--http")) { if (http) valid = false; http = true; continue; }
        if (!strcmp(arg, "--credential-store")) { if (store) valid = false; store = true; continue; }
        if (!strcmp(arg, "--oauth")) { if (oauth) valid = false; oauth = true; continue; }
        if (!next || yyjson_get_len(yyjson_arr_get(args, i + 1)) != strlen(next)) { valid = false; break; }
        if (!strcmp(arg, "--projects")) { if (project || strcmp(next, g->registry)) valid = false; else project = true; }
        else if (!strcmp(arg, "--manager-data-dir")) { if (data || strcmp(next, g->data)) valid = false; else data = true; }
        else if (!strcmp(arg, "--credential-key-file")) { if (key_seen || strcmp(next, sb_gui_string(o, "key"))) valid = false; key_seen = true; }
        else if (!strcmp(arg, "--start-project")) {
            const char *id = yyjson_get_str(yyjson_arr_get(starts, starts_seen++)); if (!id || strcmp(id, next)) valid = false;
        } else valid = false;
        i++;
    }
    const char *program = yyjson_get_str(yyjson_arr_get(args, 0));
    if (!valid || !http || !store || oauth != sb_gui_bool(o, "oauth") || starts_seen != yyjson_arr_size(starts) || key_seen != (*sb_gui_string(o, "key") != 0) ||
        !program || strcmp(program, sb_posix_program()) || !manager || !project || !data ||
        strcmp(sb_gui_string(s, "kind"), sb_gui_string(o, "kind")) || strcmp(sb_gui_string(s, "name"), sb_gui_string(o, "name")) ||
        yyjson_get_uint(yyjson_obj_get(s, "runtime_uid")) != (uint64_t)geteuid()) {
        sb_fail(e, "此登记属于其他程序、账户或后台数据；没有接管、停止或改写它。"); return false;
    }
    return true;
}
static yyjson_doc *service_cli(sb_native_manager *g, const char *action, yyjson_val *o, yyjson_val *pins, const char *registry_sha, sb_error *e) {
    char operation[40], uid[24]; snprintf(operation, sizeof(operation), "--startup-%s", action); snprintf(uid, sizeof(uid), "%u", (unsigned)geteuid());
    char *args[128] = {(char *)sb_posix_program(), operation, "--startup-dir", (char *)sb_gui_string(o, "directory"), NULL}; unsigned n = 4;
    if (!strcmp(action, "plan") || !strcmp(action, "install")) {
        args[n++] = "--kind"; args[n++] = (char *)sb_gui_string(o, "kind"); args[n++] = "--name"; args[n++] = (char *)sb_gui_string(o, "name");
        args[n++] = "--projects"; args[n++] = g->registry; args[n++] = "--manager-data-dir"; args[n++] = g->data;
        if (*sb_gui_string(o, "key")) { args[n++] = "--credential-key-file"; args[n++] = (char *)sb_gui_string(o, "key"); }
        if (service_system(o)) { struct passwd *p = getpwuid(geteuid()); if (!p) return NULL; args[n++] = "--service-user"; args[n++] = p->pw_name; }
        if (sb_gui_bool(o, "oauth")) args[n++] = "--oauth";
        size_t i, count; yyjson_val *id; yyjson_arr_foreach(yyjson_obj_get(o, "starts"), i, count, id) { args[n++] = "--start-project"; args[n++] = (char *)yyjson_get_str(id); }
    }
    args[n++] = "--expected-runtime-uid"; args[n++] = uid; args[n++] = "--expected-kind"; args[n++] = (char *)sb_gui_string(o, "kind");
    if (pins) { args[n++] = "--expected-unit-id"; args[n++] = (char *)sb_gui_string(pins, "unit_id"); args[n++] = "--expected-content-sha256"; args[n++] = (char *)sb_gui_string(pins, "content_sha256"); }
    if (registry_sha) { args[n++] = "--expected-registry-sha256"; args[n++] = (char *)registry_sha; }
    args[n] = NULL; return parse(run(g, args, false, e));
}
static void service_state_set(sb_native_manager *g, yyjson_doc *state, bool stale) {
    yyjson_mut_doc *d = yyjson_doc_mut_copy(state, NULL); yyjson_mut_val *v = yyjson_mut_doc_get_root(d);
    yyjson_mut_obj_remove_key(v, "checked_at"); yyjson_mut_obj_add_uint(d, v, "checked_at", g->service_checked_at);
    yyjson_mut_obj_remove_key(v, "stale"); yyjson_mut_obj_add_bool(d, v, "stale", stale);
    yyjson_doc_free(g->service_state); g->service_state = parse(sb_posix_json(d, v));
}
static void service_cache_load(sb_native_manager *g, yyjson_val *o) {
    char hash[65]; sb_error e = {0}; if (!service_json_hash(o, hash, &e)) return;
    if (g->service_cache_loaded && !strcmp(hash, g->service_cache_hash)) return;
    yyjson_doc_free(g->service_state); g->service_state = NULL; g->service_checked_at = 0;
    snprintf(g->service_cache_hash, sizeof(g->service_cache_hash), "%s", hash); g->service_cache_loaded = true;
    yyjson_doc *s = settings(g); yyjson_val *obs = yyjson_obj_get(yyjson_doc_get_root(s), "service_observation"), *state = yyjson_obj_get(obs, "status");
    if (!strcmp(hash, sb_gui_string(obs, "settings_sha256")) && yyjson_is_obj(state)) {
        g->service_checked_at = yyjson_get_uint(yyjson_obj_get(obs, "checked_at"));
        yyjson_doc *copy = parse(yyjson_val_write(state, 0, NULL)); service_state_set(g, copy, true); yyjson_doc_free(copy);
    }
    yyjson_doc_free(s);
}
static bool service_observe_save(sb_native_manager *g, yyjson_val *o, sb_error *e) {
    yyjson_doc *s = settings(g); yyjson_mut_doc *d = yyjson_doc_mut_copy(s, NULL); yyjson_mut_val *v = yyjson_mut_doc_get_root(d), *obs = yyjson_mut_obj(d);
    char hash[65] = {0}; bool ok = service_json_hash(o, hash, e);
    yyjson_mut_obj_add_strcpy(d, obs, "settings_sha256", hash); yyjson_mut_obj_add_uint(d, obs, "checked_at", g->service_checked_at);
    yyjson_mut_obj_add_val(d, obs, "status", yyjson_val_mut_copy(d, yyjson_doc_get_root(g->service_state)));
    yyjson_mut_obj_remove_key(v, "service_observation"); yyjson_mut_obj_add_val(d, v, "service_observation", obs);
    char *bytes = sb_posix_json(d, v); ok = ok && bytes && save(g, "desktop.json", bytes, e); free(bytes); yyjson_doc_free(s); return ok;
}
static bool service_accept(sb_native_manager *g, yyjson_doc *state, yyjson_val *o, sb_error *e) {
    yyjson_val *s = yyjson_doc_get_root(state);
    if (!state || *sb_gui_string(s, "error") || !service_binding(g, s, o, e)) {
        if (!*e->text) sb_fail(e, state ? sb_gui_string(s, "error") : "服务状态不可用。");
        if (g->service_state) service_state_set(g, g->service_state, true);
        snprintf(g->service_error, sizeof(g->service_error), "%s", e->text); return false;
    }
    g->service_checked_at = (uint64_t)time(NULL); service_state_set(g, state, false); g->service_error[0] = 0; return true;
}
static bool service_refresh_user(sb_native_manager *g, yyjson_val *o, sb_error *e) {
    yyjson_doc *s = service_cli(g, "status", o, NULL, NULL, e); bool ok = service_accept(g, s, o, e); yyjson_doc_free(s); return ok;
}
static bool service_may_exist(sb_native_manager *g, yyjson_val *o) {
    if (sb_gui_bool(yyjson_doc_get_root(g->service_state), "registered")) return true;
    if (!service_system(o) && g->service_state && !sb_gui_bool(yyjson_doc_get_root(g->service_state), "stale") && !*g->service_error) return false;
    char record[SB_PATH_CAP]; snprintf(record, sizeof(record), "%s/startup.json", sb_gui_string(o, "directory"));
    struct stat st; if (lstat(record, &st) && errno == ENOENT) return false;
    /* Ordinary UI cannot inspect a retained root-private receipt directory.
       A recent explicit absent observation permits the following edit; old
       cached observations never silently authorize editing/migration. */
    uint64_t now = (uint64_t)time(NULL);
    return !(service_system(o) && g->service_state && !sb_gui_bool(yyjson_doc_get_root(g->service_state), "stale") &&
        !*g->service_error && now >= g->service_checked_at && now - g->service_checked_at <= 30);
}
static bool service_blocked(sb_native_manager *g, sb_error *e) {
    yyjson_doc *d = service_options(g); yyjson_val *o = yyjson_doc_get_root(d); bool blocked = !service_valid(o, e);
    if (!blocked) {
        service_cache_load(g, o);
        if (!service_system(o) && !service_refresh_user(g, o, e)) blocked = true;
        bool potential = service_may_exist(g, o);
        if (potential && service_system(o) && !sb_gui_bool(yyjson_doc_get_root(g->service_state), "registered") && !*e->text)
            sb_fail(e, "系统服务的归属状态需要重新核对；请先在“服务”页明确检查状态，再修改或迁移。");
        blocked = blocked || potential;
    }
    yyjson_doc_free(d);
    if (blocked && !*e->text) sb_fail(e, "请先在“服务”页停止并卸载后台服务，再手动启动、修改配置或迁移数据。");
    return blocked;
}
static bool service_save_options(sb_native_manager *g, yyjson_val *q, sb_error *e) {
    yyjson_doc *next = service_form(g, q, e), *old = service_options(g); char before[65], after[65];
    bool ok = next && service_json_hash(yyjson_doc_get_root(old), before, e) && service_json_hash(yyjson_doc_get_root(next), after, e);
    if (ok && !strcmp(before, after)) { yyjson_doc_free(next); yyjson_doc_free(old); return true; }
    if (ok && service_blocked(g, e)) ok = false;
    if (ok && !service_system(yyjson_doc_get_root(next))) {
        yyjson_doc *target = service_cli(g, "status", yyjson_doc_get_root(next), NULL, NULL, e);
        ok = target && !*sb_gui_string(yyjson_doc_get_root(target), "error") && !sb_gui_bool(yyjson_doc_get_root(target), "registered");
        if (!ok && !*e->text) sb_fail(e, "目标服务目录已有登记或不可用；原设置保留，请选择独立的私有位置。");
        yyjson_doc_free(target);
    }
    if (ok) {
        yyjson_doc *s = settings(g); yyjson_mut_doc *d = yyjson_doc_mut_copy(s, NULL); yyjson_mut_val *v = yyjson_mut_doc_get_root(d);
        yyjson_mut_obj_remove_key(v, "backend_service"); yyjson_mut_obj_add_val(d, v, "backend_service", yyjson_val_mut_copy(d, yyjson_doc_get_root(next)));
        yyjson_mut_obj_remove_key(v, "service_observation"); char *bytes = sb_posix_json(d, v);
        ok = bytes && save(g, "desktop.json", bytes, e); free(bytes); yyjson_doc_free(s);
        if (ok) { service_clear_preview(g); g->service_cache_loaded = false; g->service_error[0] = 0; }
    }
    yyjson_doc_free(next); yyjson_doc_free(old); return ok;
}
static bool service_current_form(sb_native_manager *g, yyjson_val *q, yyjson_val *o, sb_error *e) {
    yyjson_doc *d = service_form(g, q, e); char a[65], b[65];
    bool ok = d && service_json_hash(o, a, e) && service_json_hash(yyjson_doc_get_root(d), b, e) && !strcmp(a, b);
    yyjson_doc_free(d); if (!ok && !*e->text) sb_fail(e, "服务表单已改变，请先保存并重新预览，随后再确认操作。"); return ok;
}
static bool service_data_idle(sb_native_manager *g, sb_error *e) {
    if (g->child || online(g)) { sb_fail(e, "请先停止当前后台；服务不会接管或停止已有后台。"); return false; }
    sb_lease *lease = sb_platform_lease(g->data, e); if (!lease) return false; sb_platform_lease_close(lease); return true;
}
static bool service_prepare_credentials(sb_native_manager *g, yyjson_val *o, sb_error *e) {
    char original[SB_PATH_CAP]; snprintf(original, sizeof(original), "%s", g->key); snprintf(g->key, sizeof(g->key), "%s", sb_gui_string(o, "key"));
    yyjson_doc *cfg = registry(g); yyjson_val *projects = yyjson_obj_get(yyjson_doc_get_root(cfg), "projects");
    bool ok = yyjson_arr_size(projects) && credentials(g, projects, e);
    if (ok) {
        char *args[8] = {(char *)sb_posix_program(), "--credential-status", "--manager-data-dir", g->data, NULL};
        if (*g->key) { args[4] = "--credential-key-file"; args[5] = g->key; args[6] = NULL; }
        yyjson_doc *catalog = parse(run(g, args, false, e)); size_t i, n; yyjson_val *p;
        ok = catalog != NULL;
        yyjson_arr_foreach(projects, i, n, p) {
            if (yyjson_obj_get(p, "enabled") && !sb_gui_bool(p, "enabled")) continue;
            if (!find(catalog, "projects", "id", sb_gui_string(p, "id"))) ok = false;
        }
        yyjson_doc_free(catalog);
        if (!ok && !*e->text) sb_fail(e, "服务不能依赖本窗口环境中的凭据；请先用凭据工具把原凭据保存到加密存储。");
    }
    snprintf(g->key, sizeof(g->key), "%s", original); yyjson_doc_free(cfg); return ok;
}
static yyjson_doc *service_privileged(sb_native_manager *g, const char *action, yyjson_val *o, yyjson_val *pins, const char *hash, sb_error *e) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL); yyjson_mut_val *v = yyjson_mut_obj(d);
    yyjson_mut_obj_add_str(d, v, "action", action); yyjson_mut_obj_add_strcpy(d, v, "startup_dir", sb_gui_string(o, "directory"));
    if (pins) { yyjson_mut_obj_add_strcpy(d, v, "expected_unit_id", sb_gui_string(pins, "unit_id")); yyjson_mut_obj_add_strcpy(d, v, "expected_content_sha256", sb_gui_string(pins, "content_sha256")); }
    if (!strcmp(action, "install")) {
        yyjson_mut_obj_add_strcpy(d, v, "name", sb_gui_string(o, "name")); yyjson_mut_obj_add_strcpy(d, v, "projects", g->registry); yyjson_mut_obj_add_strcpy(d, v, "data", g->data);
        yyjson_mut_obj_add_strcpy(d, v, "key", sb_gui_string(o, "key")); yyjson_mut_obj_add_bool(d, v, "oauth", sb_gui_bool(o, "oauth"));
        yyjson_mut_obj_add_val(d, v, "starts", yyjson_val_mut_copy(d, yyjson_obj_get(o, "starts"))); yyjson_mut_obj_add_strcpy(d, v, "expected_registry_sha256", hash);
    }
    char *bytes = sb_posix_json(d, v); yyjson_doc *reply = parse(sb_service_system_call(g->directory, bytes, &g->cancel, e)); free(bytes); return reply;
}
static bool service_action(sb_native_manager *g, const char *action, yyjson_val *q, sb_error *e) {
    if (!strcmp(action, "settings")) return service_save_options(g, q, e);
    if (!strcmp(action, "plan") && !service_save_options(g, q, e)) return false;
    yyjson_doc *d = service_options(g); yyjson_val *o = yyjson_doc_get_root(d); service_cache_load(g, o);
    bool ok = service_valid(o, e) && service_current_form(g, q, o, e); yyjson_doc *reply = NULL;
    if (!ok) goto done;
    if (!strcmp(action, "plan")) {
        service_clear_preview(g); char *bytes = load(g->registry);
        ok = bytes && sb_platform_sha256(bytes, strlen(bytes), g->service_preview_registry, e); free(bytes);
        reply = ok ? service_cli(g, "plan", o, NULL, g->service_preview_registry, e) : NULL;
        ok = reply && !*sb_gui_string(yyjson_doc_get_root(reply), "error") && service_json_hash(o, g->service_preview_options, e) && service_json_hash(yyjson_doc_get_root(reply), g->service_plan_hash, e);
        if (ok) { g->service_preview = reply; reply = NULL; }
        goto done;
    }
    bool install = !strcmp(action, "install"), refresh = !strcmp(action, "refresh");
    if (!install && !refresh && strcmp(action, "start") && strcmp(action, "stop") && strcmp(action, "uninstall")) { sb_fail(e, "未知服务操作。"); ok = false; goto done; }
    if (!service_system(o) && !service_refresh_user(g, o, e)) { ok = false; goto done; }
    yyjson_val *state = yyjson_doc_get_root(g->service_state), *pins = install ? yyjson_doc_get_root(g->service_preview) : state;
    if (install) {
        char option_hash[65], registry_hash[65], *bytes = load(g->registry);
        ok = bytes && service_json_hash(o, option_hash, e) && sb_platform_sha256(bytes, strlen(bytes), registry_hash, e) &&
            *g->service_plan_hash && !strcmp(g->service_plan_hash, sb_gui_string(q, "expected_plan_sha256")) &&
            !strcmp(option_hash, g->service_preview_options) && !strcmp(registry_hash, g->service_preview_registry);
        free(bytes); if (!ok) { sb_fail(e, "安装预览已变化或尚未查看，请重新预览完整服务配置再确认安装。"); goto done; }
        if (!service_data_idle(g, e) || !service_prepare_credentials(g, o, e)) { ok = false; goto done; }
    } else if (!refresh) {
        ok = state && sb_gui_bool(state, "registered") && !sb_gui_bool(state, "stale") &&
            !strcmp(sb_gui_string(q, "expected_unit_id"), sb_gui_string(state, "unit_id")) &&
            !strcmp(sb_gui_string(q, "expected_content_sha256"), sb_gui_string(state, "content_sha256"));
        if (!ok) { sb_fail(e, "服务登记已变化或状态尚未检查，请先明确检查状态再确认操作。"); goto done; }
        if (!service_binding(g, state, o, e)) { ok = false; goto done; }
        if (!strcmp(action, "start") && online(g)) {
            yyjson_doc *manager = exchange(g, "status", NULL, NULL);
            uint64_t pid = yyjson_get_uint(yyjson_obj_get(yyjson_doc_get_root(manager), "manager_pid"));
            ok = sb_gui_bool(state, "active") && pid && pid == yyjson_get_uint(yyjson_obj_get(state, "process_id")); yyjson_doc_free(manager);
            if (ok) goto done;
            sb_fail(e, "已有独立后台正在占用数据，服务没有接管或停止它。"); goto done;
        }
        if (!strcmp(action, "start") && !service_data_idle(g, e)) { ok = false; goto done; }
    }
    if ((!strcmp(action, "stop") || !strcmp(action, "uninstall")) && online(g)) {
        yyjson_doc *manager = exchange(g, "status", NULL, NULL); uint64_t pid = yyjson_get_uint(yyjson_obj_get(yyjson_doc_get_root(manager), "manager_pid"));
        bool own = sb_gui_bool(state, "active") && pid && pid == yyjson_get_uint(yyjson_obj_get(state, "process_id")); yyjson_doc_free(manager);
        if (!own) { sb_fail(e, "已有后台不是此服务进程；没有停止或卸载登记。"); ok = false; goto done; }
    }
    if (service_system(o)) {
        reply = service_privileged(g, refresh ? "status" : action, o, refresh ? NULL : pins, install ? g->service_preview_registry : NULL, e);
        yyjson_val *result = yyjson_doc_get_root(reply), *status = yyjson_obj_get(result, "status");
        ok = reply && sb_gui_bool(result, "ok");
        if (!ok && reply) sb_fail(e, sb_gui_string(result, "error"));
        if (yyjson_is_obj(status)) {
            yyjson_doc *observed = parse(yyjson_val_write(status, 0, NULL)); bool accepted = service_accept(g, observed, o, e); yyjson_doc_free(observed); ok = ok && accepted;
        } else { if (g->service_state) service_state_set(g, g->service_state, true); ok = false; }
    } else {
        yyjson_doc_free(reply); reply = service_cli(g, refresh ? "status" : action, o, refresh ? NULL : pins, install ? g->service_preview_registry : NULL, e);
        ok = reply && !*sb_gui_string(yyjson_doc_get_root(reply), "error");
        if (!ok && reply) sb_fail(e, sb_gui_string(yyjson_doc_get_root(reply), "error"));
        if (ok) ok = service_refresh_user(g, o, e);
    }
    if (ok) { if (!refresh) service_clear_preview(g); ok = service_observe_save(g, o, e); }
done:
    if (!ok) { if (!*e->text) sb_fail(e, "服务操作失败；请核对配置和原生服务状态。"); snprintf(g->service_error, sizeof(g->service_error), "%s", e->text); }
    yyjson_doc_free(reply); yyjson_doc_free(d); return ok;
}
static void service_snapshot(sb_native_manager *g, yyjson_mut_doc *d, yyjson_mut_val *v) {
    yyjson_doc *opts = service_options(g); yyjson_val *o = yyjson_doc_get_root(opts); sb_error e = {0}; service_cache_load(g, o);
    char previous_error[sizeof(g->service_error)]; snprintf(previous_error, sizeof(previous_error), "%s", g->service_error);
    if (service_valid(o, &e) && !service_system(o)) service_refresh_user(g, o, &e);
    else if (*e.text) snprintf(g->service_error, sizeof(g->service_error), "%s", e.text);
    if (!*g->service_error && *previous_error) snprintf(g->service_error, sizeof(g->service_error), "%s", previous_error);
    copy_doc(d, v, "service_settings", opts); copy_doc(d, v, "service", g->service_state); copy_doc(d, v, "service_plan", g->service_preview);
    yyjson_mut_obj_add_strcpy(d, v, "service_plan_sha256", g->service_plan_hash); yyjson_mut_obj_add_strcpy(d, v, "service_error", g->service_error);
    yyjson_mut_obj_add_uint(d, v, "service_checked_at", g->service_checked_at);
    char name[64], user[SB_PATH_CAP], system[SB_PATH_CAP]; service_defaults(g, name, user, system); yyjson_mut_val *defaults = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, defaults, "user-service", user); yyjson_mut_obj_add_strcpy(d, defaults, "system-service", system); yyjson_mut_obj_add_val(d, v, "service_default_directories", defaults);
    yyjson_doc_free(opts);
}
char *sb_gui_service_text(yyjson_val *status, yyjson_val *plan, bool system, const char *error) {
    char *out = calloc(1, SB_RESPONSE_LIMIT + 1); size_t n = 0; if (!out) return NULL;
    add_text(out, &n, "%s\n", system ? "系统服务：后台始终以当前普通账户运行；管理操作逐次请求系统授权。" : "当前账户服务：登录后的独立后台；不等于无人登录时开机运行。");
    if (yyjson_is_obj(status)) {
        add_text(out, &n, "登记：%s\n原生文件：%s\n运行：%s\n", sb_gui_bool(status, "registered") ? "已有归属记录" : "未登记", sb_gui_bool(status, "installed") ? "已安装" : "不存在",
            sb_gui_bool(status, "stale") ? "缓存状态，需明确重新检查" : !sb_gui_bool(status, "status_known") && sb_gui_bool(status, "registered") ? "无法确认" : sb_gui_bool(status, "active") ? "运行中" : "未运行");
        time_t stamp = (time_t)yyjson_get_uint(yyjson_obj_get(status, "checked_at")); struct tm local; char checked[64] = "尚未检查";
        if (stamp && localtime_r(&stamp, &local)) strftime(checked, sizeof(checked), "%Y-%m-%d %H:%M:%S %Z", &local);
        add_text(out, &n, "服务：%s\n安装位置：%s\n", sb_gui_string(status, "unit_id"), sb_gui_string(status, "native_path"));
        if (sb_gui_bool(status, "registered")) add_text(out, &n, "运行账户 UID：%llu\n服务进程：%llu\n",
            (unsigned long long)yyjson_get_uint(yyjson_obj_get(status, "runtime_uid")), (unsigned long long)yyjson_get_uint(yyjson_obj_get(status, "process_id")));
        add_text(out, &n, "上次检查：%s\n", checked);
    } else add_text(out, &n, "状态尚未检查。系统服务不会在定期刷新时弹出授权窗口。\n");
    if (error && *error) add_text(out, &n, "\n检查或操作提示：%s\n", error);
    add_text(out, &n, "\n服务启动列表独立于窗口的项目自动启动。退出窗口保留独立服务；停止/卸载会结束所属项目和任务，保留数据、凭据和历史。\n");
    if (yyjson_is_obj(plan)) {
        add_text(out, &n, "\n完整安装预览（安装仅登记，不立即启动）\n原生位置：%s\n运行 UID：%llu\n参数：\n", sb_gui_string(plan, "native_path"), (unsigned long long)yyjson_get_uint(yyjson_obj_get(plan, "runtime_uid")));
        size_t i, count; yyjson_val *arg; yyjson_arr_foreach(yyjson_obj_get(plan, "argv"), i, count, arg) add_text(out, &n, "  %s\n", yyjson_get_str(arg));
        add_text(out, &n, "\n原生描述：\n%s\n", sb_gui_string(plan, "contents"));
    }
    return out;
}
