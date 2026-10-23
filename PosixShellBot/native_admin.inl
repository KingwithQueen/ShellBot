static void clear_inspection(sb_native_manager *g) {
    yyjson_doc_free(g->inspected); g->inspected = NULL;
    g->inspected_backend[0] = g->inspected_project[0] = g->inspected_broker[0] = 0;
    g->inspected_generation = 0;
}
static void admin_close(sb_native_manager *g) {
    sb_admin_link_close(&g->administrator);
    yyjson_doc_free(g->admin_state); g->admin_state = NULL;
    clear_inspection(g);
}
static bool admin_refresh(sb_native_manager *g, sb_error *e) {
    if (g->administrator.fd < 0) return false;
    yyjson_doc *d = parse(sb_admin_link_call(&g->administrator, "{\"command\":\"status\"}", e));
    yyjson_val *v = yyjson_doc_get_root(d);
    const char *id = sb_gui_string(v, "broker_id");
    bool valid = d && strlen(id) == 32 && strspn(id, "0123456789abcdef") == 32 &&
        yyjson_get_uint(yyjson_obj_get(v, "owner_uid")) == (uint64_t)geteuid() &&
        !strcmp(sb_gui_string(v, "registry_sha256"), g->administrator.registry_sha256) &&
        (!g->admin_state || !strcmp(id, sb_gui_string(yyjson_doc_get_root(g->admin_state), "broker_id"))) &&
        sb_gui_bool(v, "healthy");
    if (!valid) {
        yyjson_doc_free(d); admin_close(g);
        if (!*e->text) sb_fail(e, "管理员会话身份或状态已改变，请重新明确授权。");
        return false;
    }
    yyjson_doc_free(g->admin_state); g->admin_state = d;
    return true;
}
static yyjson_val *admin_selected(sb_native_manager *g) {
    return find(g->admin_state, "projects", "project_id", g->selected);
}
static bool admin_configuration_matches(sb_native_manager *g, sb_error *e) {
    char *bytes = load(g->registry), hash[65];
    bool ok = bytes && sb_platform_sha256(bytes, strlen(bytes), hash, e) &&
        !strcmp(hash, g->administrator.registry_sha256);
    free(bytes);
    if (!ok) sb_fail(e, "当前项目配置与本次管理员授权不同，请结束会话后重新授权。");
    return ok;
}
static char *admin_request(sb_native_manager *g, const char *command, const char *request, sb_error *e) {
    if (!admin_refresh(g, e) || !admin_configuration_matches(g, e)) {
        if (!*e->text) sb_fail(e, "本窗口尚未启动管理员会话。");
        return NULL;
    }
    yyjson_val *state = yyjson_doc_get_root(g->admin_state), *slot = admin_selected(g);
    if (!slot || !sb_gui_bool(slot, "granted")) { sb_fail(e, "此项目不在本次管理员授权范围内。"); return NULL; }
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL); yyjson_mut_val *v = yyjson_mut_obj(d);
    yyjson_mut_obj_add_str(d, v, "command", command);
    yyjson_mut_obj_add_str(d, v, "project_id", g->selected);
    yyjson_mut_obj_add_str(d, v, "expected_broker_id", sb_gui_string(state, "broker_id"));
    yyjson_mut_obj_add_str(d, v, "expected_project_sha256", sb_gui_string(slot, "project_sha256"));
    yyjson_mut_obj_add_uint(d, v, "expected_generation", yyjson_get_uint(yyjson_obj_get(slot, "generation")));
    yyjson_doc *q = request ? yyjson_read(request, strlen(request), 0) : NULL;
    if (request) yyjson_mut_obj_add_val(d, v, "request", yyjson_val_mut_copy(d, yyjson_doc_get_root(q)));
    char *wire = sb_posix_json(d, v), *reply = wire ? sb_admin_link_call(&g->administrator, wire, e) : NULL;
    free(wire); yyjson_doc_free(q);
    return reply;
}
static char *approval_request(sb_native_manager *g, const char *request, sb_error *e) {
    return !strcmp(g->approval_backend, "administrator") ? admin_request(g, "project-owner", request, e)
                                                        : project_request(g, request, e);
}
static bool admin_save(sb_native_manager *g, yyjson_val *q, sb_error *e) {
    if (g->administrator.fd >= 0 || sb_admin_link_closing(&g->administrator)) {
        sb_fail(e, "请先结束本窗口的管理员会话，再修改管理员设置。"); return false;
    }
    const char *registry_path = sb_gui_string(q, "root_registry"), *state_path = sb_gui_string(q, "root_state");
    if (!sb_project_path_valid(registry_path) || !sb_project_path_valid(state_path) ||
        strpbrk(registry_path, "\r\n\t") || strpbrk(state_path, "\r\n\t")) {
        sb_fail(e, "请选择规范绝对路径；管理员注册表和状态目录由 root 拥有。"); return false;
    }
    yyjson_doc *s = settings(g); yyjson_mut_doc *d = yyjson_doc_mut_copy(s, NULL);
    yyjson_mut_val *root = yyjson_mut_doc_get_root(d), *admin = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, admin, "registry", registry_path);
    yyjson_mut_obj_add_strcpy(d, admin, "state", state_path);
    yyjson_mut_obj_remove_key(root, "administrator"); yyjson_mut_obj_add_val(d, root, "administrator", admin);
    char *bytes = sb_posix_json(d, root); bool ok = bytes && save(g, "desktop.json", bytes, e);
    free(bytes); yyjson_doc_free(s); return ok;
}
static bool admin_start(sb_native_manager *g, sb_error *e) {
    char *bytes = load(g->registry), hash[65]; sb_project_registry r = {0}; unsigned allowed = 0;
    bool valid = bytes && sb_projects_parse(&r, bytes, strlen(bytes), e) && sb_platform_sha256(bytes, strlen(bytes), hash, e);
    if (valid) for (size_t i = 0; i < r.project_count; i++) allowed += sb_admin_project_eligible(&r.projects[i]);
    sb_projects_free(&r); free(bytes);
    if (!valid || !allowed) { if (!*e->text) sb_fail(e, "请先在项目设置中明确允许管理员操作，并选择当前账户文件范围和命令身份。"); return false; }
    yyjson_doc *s = settings(g); yyjson_val *opts = yyjson_obj_get(yyjson_doc_get_root(s), "administrator");
    bool ok = sb_admin_link_start(&g->administrator, g->directory, sb_gui_string(opts, "registry"),
                                  sb_gui_string(opts, "state"), hash, &g->cancel, e);
    yyjson_doc_free(s); clear_inspection(g);
    return ok && admin_refresh(g, e);
}
static bool admin_control(sb_native_manager *g, const char *command, sb_error *e) {
    yyjson_doc *d = parse(admin_request(g, command, NULL, e));
    bool ok = d && !*sb_gui_string(yyjson_doc_get_root(d), "error");
    if (!ok && d) sb_fail(e, sb_gui_string(yyjson_doc_get_root(d), "error"));
    yyjson_doc_free(d); clear_inspection(g); return ok;
}
static bool admin_connect_project(sb_native_manager *g, sb_error *e) {
    if (!online(g)) { sb_fail(e, "请先启动普通后台，再连接所选项目的管理员代理。"); return false; }
    if (!admin_refresh(g, e)) { if (!*e->text) sb_fail(e, "请先启动本窗口的管理员会话。"); return false; }
    const char *state = sb_gui_string(admin_selected(g), "state");
    if (strcmp(state, "waiting") && strcmp(state, "starting") && !admin_control(g, "restart", e)) return false;
    uint64_t deadline = sb_platform_millis() + 5000;
    bool ready = false;
    while (!atomic_load(&g->cancel) && sb_platform_millis() < deadline && admin_refresh(g, e)) {
        state = sb_gui_string(admin_selected(g), "state");
        if (!strcmp(state, "waiting")) { ready = true; break; }
        if (!strcmp(state, "failed") || !strcmp(state, "revoked") || !*state) break;
        struct timespec pause = {.tv_nsec = 50000000}; nanosleep(&pause, NULL);
    }
    if (!ready) { if (!*e->text) sb_fail(e, "管理员代理尚未准备好，请刷新后检查项目状态。"); return false; }
    yyjson_doc *stopped_reply = exchange(g, "stop_project", g->selected, NULL);
    bool ok = stopped_reply && !*sb_gui_string(yyjson_doc_get_root(stopped_reply), "error");
    if (!ok) sb_fail(e, "无法停止普通项目；没有自动重试或启动新任务。");
    yyjson_doc_free(stopped_reply);
    yyjson_doc *started_reply = ok ? exchange(g, "start", g->selected, NULL) : NULL;
    ok = ok && started_reply && !*sb_gui_string(yyjson_doc_get_root(started_reply), "error");
    if (!ok && started_reply) sb_fail(e, sb_gui_string(yyjson_doc_get_root(started_reply), "error"));
    yyjson_doc_free(started_reply); clear_inspection(g); return ok;
}
static const char *admin_state_label(const char *state) {
    return !strcmp(state, "connected") ? "已连接项目" : !strcmp(state, "waiting") ? "等待重启项目以连接" :
        !strcmp(state, "starting") ? "启动中" : !strcmp(state, "revoked") ? "本次授权已撤销" :
        !strcmp(state, "stopped") ? "已停止，可明确重启" : !strcmp(state, "failed") ? "异常退出，请检查记录" : "未授权";
}
char *sb_gui_admin_text(yyjson_val *state, const char *selected, bool connected, bool closing) {
    char *out = calloc(1, SB_RESPONSE_LIMIT + 1); size_t used = 0; if (!out) return NULL;
    add_text(out, &used, "管理员会话：%s\n", connected ? "当前窗口已持有授权" : closing ? "正在结束，请稍后刷新" : "未启动或已断开");
    if (connected) {
        add_text(out, &used, "会话：%s\n\n", sb_gui_string(state, "broker_id"));
        size_t i, n; yyjson_val *p;
        yyjson_arr_foreach(yyjson_obj_get(state, "projects"), i, n, p)
            add_text(out, &used, "%s%s：%s\n", !strcmp(sb_gui_string(p, "project_id"), selected) ? "所选项目 · " : "",
                     sb_gui_string(p, "project_id"), admin_state_label(sb_gui_string(p, "state")));
    }
    add_text(out, &used, "\n仅管理本窗口明确启动的会话，不接管独立命令行会话。\n重启并连接项目会结束该项目当前任务；撤销只影响所选项目。\n修改项目配置前先结束管理员会话；新权限需要再次确认。\n关闭窗口保留会话；退出程序或通道中断会结束本窗口的管理员会话。\n");
    return out;
}
