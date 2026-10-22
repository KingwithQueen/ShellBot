#include "native.h"
#include <gtk/gtk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    sb_desktop *desktop;
    GtkApplication *app;
    GtkWidget *window, *projects, *requests, *details, *request_detail, *message, *summary, *tray_label;
    GtkWidget *http, *oauth, *port, *url, *persistent;
    yyjson_doc *model;
    char project[64], request_id[65], request_type[32];
    bool busy, rendering, tray, quitting, settings_loaded;
    GDBusConnection *bus;
    GDBusNodeInfo *sni_info, *menu_info;
    guint sni_object, menu_object, watch;
    const char *smoke;
    unsigned ticks;
} view;
static view ui;
static GVariant *tray_pixels;
static void submit(const char *action);
static void present(void) { gtk_window_present(GTK_WINDOW(ui.window)); }
static GtkWidget *label(const char *s) {
    GtkWidget *w = gtk_label_new(s);
    gtk_label_set_xalign(GTK_LABEL(w), 0);
    gtk_label_set_wrap(GTK_LABEL(w), TRUE);
    return w;
}
static GtkWidget *box(bool vertical, int spacing) {
    return gtk_box_new(vertical ? GTK_ORIENTATION_VERTICAL : GTK_ORIENTATION_HORIZONTAL, spacing);
}
static void append(GtkWidget *parent, GtkWidget *child) { gtk_box_append(GTK_BOX(parent), child); }
static GtkWidget *scroll(GtkWidget *child) {
    GtkWidget *w = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(w), child);
    gtk_widget_set_vexpand(w, TRUE);
    gtk_widget_set_hexpand(w, TRUE);
    return w;
}
static GtkWidget *text(void) {
    GtkWidget *w = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(w), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(w), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(w), 12);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(w), 12);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(w), 10);
    return w;
}
static void set_text(GtkWidget *widget, const char *s) {
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(widget));
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    char *old = gtk_text_buffer_get_text(buffer, &start, &end, TRUE);
    if (strcmp(old, s ? s : ""))
        gtk_text_buffer_set_text(buffer, s ? s : "", -1);
    g_free(old);
}
static void pretty(GtkWidget *widget, yyjson_val *v) {
    char *s = sb_gui_request_text(v, yyjson_obj_get(yyjson_doc_get_root(ui.model), "config"));
    set_text(widget, s ? s : "");
    free(s);
}
static yyjson_val *model(const char *key) { return yyjson_obj_get(yyjson_doc_get_root(ui.model), key); }
static yyjson_val *project_config(void) {
    size_t i, max;
    yyjson_val *p;
    yyjson_arr_foreach(yyjson_obj_get(model("config"), "projects"), i, max,
                       p) if (!strcmp(sb_gui_string(p, "id"), ui.project)) return p;
    return NULL;
}
static void alert(const char *s) {
    GtkAlertDialog *d = gtk_alert_dialog_new("%s", s);
    gtk_alert_dialog_show(d, GTK_WINDOW(ui.window));
    g_object_unref(d);
}
static void clear_list(GtkWidget *list) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(list)))
        gtk_list_box_remove(GTK_LIST_BOX(list), child);
}
static void select_project(GtkListBox *list, GtkListBoxRow *row, gpointer unused) {
    (void)list;
    (void)unused;
    if (ui.rendering || !row || ui.busy)
        return;
    snprintf(ui.project, sizeof(ui.project), "%s", (char *)g_object_get_data(G_OBJECT(row), "id"));
    submit("refresh");
}
static yyjson_val *current_request(void) {
    yyjson_val *a = !strcmp(ui.request_type, "operation") ? yyjson_obj_get(model("approvals"), "approvals")
                                                          : yyjson_obj_get(model("oauth"), ui.request_type);
    size_t i, max;
    yyjson_val *v;
    yyjson_arr_foreach(
        a, i, max,
        v) if (!strcmp(sb_gui_string(v, !strcmp(ui.request_type, "operation") ? "approval_id" : "id"),
                       ui.request_id)) return v;
    return NULL;
}
static void request_selected(GtkListBox *list, GtkListBoxRow *row, gpointer unused) {
    (void)list;
    (void)unused;
    if (ui.rendering || !row)
        return;
    snprintf(ui.request_id, sizeof(ui.request_id), "%s", (char *)g_object_get_data(G_OBJECT(row), "id"));
    snprintf(ui.request_type, sizeof(ui.request_type), "%s",
             (char *)g_object_get_data(G_OBJECT(row), "type"));
    gtk_check_button_set_active(GTK_CHECK_BUTTON(ui.persistent), FALSE);
    pretty(ui.request_detail, current_request());
}
static void row_add(GtkWidget *list, const char *s, const char *id, const char *type, bool selected) {
    GtkWidget *row = gtk_list_box_row_new(), *l = label(s);
    gtk_widget_set_margin_start(l, 10);
    gtk_widget_set_margin_end(l, 10);
    gtk_widget_set_margin_top(l, 10);
    gtk_widget_set_margin_bottom(l, 10);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), l);
    g_object_set_data_full(G_OBJECT(row), "id", g_strdup(id), g_free);
    if (type)
        g_object_set_data_full(G_OBJECT(row), "type", g_strdup(type), g_free);
    gtk_list_box_append(GTK_LIST_BOX(list), row);
    if (selected)
        gtk_list_box_select_row(GTK_LIST_BOX(list), GTK_LIST_BOX_ROW(row));
}
static void render(void) {
    ui.rendering = true;
    snprintf(ui.project, sizeof(ui.project), "%s", sb_gui_string(yyjson_doc_get_root(ui.model), "selected"));
    bool active = sb_gui_bool(yyjson_doc_get_root(ui.model), "online");
    gtk_label_set_text(GTK_LABEL(ui.summary),
                       active ? "后台运行中 · 项目独立运行" : "后台已停止 · 点击启动后台后可启动项目");
    gtk_label_set_text(GTK_LABEL(ui.message), sb_gui_string(yyjson_doc_get_root(ui.model), "message"));
    if (!ui.settings_loaded) {
        yyjson_val *gateway = yyjson_obj_get(model("config"), "gateway"), *opts = model("settings");
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui.port), yyjson_get_uint(yyjson_obj_get(gateway, "port")));
        gtk_editable_set_text(GTK_EDITABLE(ui.url), sb_gui_string(gateway, "public_url"));
        gtk_check_button_set_active(GTK_CHECK_BUTTON(ui.http), sb_gui_bool(opts, "http"));
        gtk_check_button_set_active(GTK_CHECK_BUTTON(ui.oauth), sb_gui_bool(opts, "oauth"));
        ui.settings_loaded = true;
    }
    clear_list(ui.projects);
    size_t i, max;
    yyjson_val *p;
    yyjson_arr_foreach(yyjson_obj_get(model("config"), "projects"), i, max, p) {
        const char *id = sb_gui_string(p, "id");
        if (!*ui.project)
            snprintf(ui.project, sizeof(ui.project), "%s", id);
        const char *state = "已停止";
        size_t j, jmax;
        yyjson_val *v;
        yyjson_arr_foreach(yyjson_obj_get(model("status"), "projects"), j, jmax,
                           v) if (!strcmp(id, sb_gui_string(v, "project_id"))) state =
            sb_gui_bool(v, "paused")    ? "已暂停"
            : sb_gui_bool(v, "running") ? "运行中"
                                        : "已停止";
        char line[180];
        snprintf(line, sizeof(line), "%s\n%s", id, state);
        row_add(ui.projects, line, id, NULL, !strcmp(id, ui.project));
    }
    yyjson_val *cfg = project_config();
    char *description = sb_gui_project_text(cfg, yyjson_obj_get(model("config"), "gateway"), model("status"));
    set_text(ui.details, description);
    free(description);
    clear_list(ui.requests);
    const char *types[] = {"operation", "pending", "connections", "clients"};
    const char *titles[] = {"操作审批", "连接申请", "已授权连接", "客户端"};
    for (unsigned t = 0; t < 4; t++) {
        yyjson_val *a =
            t ? yyjson_obj_get(model("oauth"), types[t]) : yyjson_obj_get(model("approvals"), "approvals");
        yyjson_arr_foreach(a, i, max, p) {
            const char *id = sb_gui_string(p, t ? "id" : "approval_id");
            char line[512];
            snprintf(line, sizeof(line), "%s · %s\n%s · %s", titles[t],
                     sb_gui_string(p, t ? "client_name" : "tool"), sb_gui_string(p, t ? "project" : "target"),
                     sb_gui_string(p, "status"));
            row_add(ui.requests, line, id, types[t],
                    !strcmp(id, ui.request_id) && !strcmp(types[t], ui.request_type));
        }
    }
    yyjson_val *inspected = model("inspected");
    pretty(ui.request_detail, !strcmp(ui.request_type, "operation") &&
                                      !strcmp(ui.request_id, sb_gui_string(inspected, "approval_id"))
                                  ? inspected
                                  : current_request());
    ui.rendering = false;
}
static void work_thread(GTask *task, gpointer source, gpointer data, GCancellable *cancel) {
    (void)source;
    (void)cancel;
    g_task_return_pointer(task, sb_desktop_work(ui.desktop, data), free);
}
static void done(GObject *source, GAsyncResult *result, gpointer data) {
    (void)source;
    (void)data;
    GError *e = NULL;
    char *json = g_task_propagate_pointer(G_TASK(result), &e);
    ui.busy = false;
    if (json) {
        yyjson_doc_free(ui.model);
        ui.model = yyjson_read(json, strlen(json), 0);
        free(json);
        render();
    }
    if (ui.quitting) {
        if (sb_gui_bool(yyjson_doc_get_root(ui.model), "online") &&
            sb_gui_bool(yyjson_doc_get_root(ui.model), "owned")) {
            ui.quitting = false;
            alert("后台尚未停止，请稍后重试退出。");
        } else
            g_application_quit(G_APPLICATION(ui.app));
    }
    if (e) {
        alert(e->message);
        g_error_free(e);
    }
}
static void send_doc(yyjson_mut_doc *d, yyjson_mut_val *q) {
    if (ui.busy) {
        yyjson_mut_doc_free(d);
        return;
    }
    yyjson_mut_obj_add_strcpy(d, q, "project", ui.project);
    char *bytes = sb_posix_json(d, q);
    ui.busy = true;
    GTask *task = g_task_new(NULL, NULL, done, NULL);
    g_task_set_task_data(task, bytes, free);
    g_task_run_in_thread(task, work_thread);
    g_object_unref(task);
}
static void submit(const char *action) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *q = yyjson_mut_obj(d);
    yyjson_mut_obj_add_str(d, q, "action", action);
    send_doc(d, q);
}
static void clicked(GtkButton *button, gpointer action) {
    (void)button;
    submit(action);
}
static GtkWidget *button(GtkWidget *parent, const char *title, const char *action) {
    GtkWidget *b = gtk_button_new_with_label(title);
    append(parent, b);
    g_signal_connect(b, "clicked", G_CALLBACK(clicked), (gpointer)action);
    return b;
}
static void quit_response(GObject *d, GAsyncResult *result, gpointer unused) {
    (void)unused;
    int response = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(d), result, NULL);
    if (response == 0 && !ui.busy) {
        ui.quitting = true;
        submit("exit");
    }
}
static void quit(void) {
    if (ui.busy) {
        alert("当前操作完成后可以退出。");
        return;
    }
    GtkAlertDialog *d = gtk_alert_dialog_new(
        "退出 ShellBot？本次启动的后台、项目和命令将结束。连接到既有后台时会保留后台运行。");
    const char *buttons[] = {"退出", "取消", NULL};
    gtk_alert_dialog_set_buttons(d, buttons);
    gtk_alert_dialog_set_cancel_button(d, 1);
    gtk_alert_dialog_set_default_button(d, 1);
    gtk_alert_dialog_choose(d, GTK_WINDOW(ui.window), NULL, quit_response, NULL);
    g_object_unref(d);
}
static void quit_clicked(GtkButton *b, gpointer data) {
    (void)b;
    (void)data;
    quit();
}
static gboolean closing(GtkWindow *window, gpointer data) {
    (void)data;
    if (ui.tray) {
        gtk_widget_set_visible(GTK_WIDGET(window), FALSE);
    } else
        alert("当前桌面没有可用的托盘。窗口会继续保留，可最小化；需要结束程序时请点击退出。");
    return TRUE;
}
typedef struct {
    GtkWidget *dialog, *id, *path, *access;
    bool editing;
} editor;
static void folder_chosen(GObject *source, GAsyncResult *result, gpointer data) {
    GError *e = NULL;
    GFile *file = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), result, &e);
    if (file) {
        char *p = g_file_get_path(file);
        gtk_editable_set_text(GTK_EDITABLE(data), p);
        g_free(p);
        g_object_unref(file);
    }
    g_clear_error(&e);
    g_object_unref(data);
}
static void choose_folder(GtkButton *b, gpointer entry) {
    (void)b;
    GtkFileDialog *d = gtk_file_dialog_new();
    gtk_file_dialog_set_title(d, "选择项目目录");
    gtk_file_dialog_select_folder(d, GTK_WINDOW(ui.window), NULL, folder_chosen, g_object_ref(entry));
    g_object_unref(d);
}
static void editor_response(GtkButton *button, gpointer data) {
    editor *ed = data;
    if (GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "save"))) {
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *q = yyjson_mut_obj(d);
        yyjson_mut_obj_add_str(d, q, "action", ed->editing ? "edit" : "add");
        yyjson_mut_obj_add_strcpy(d, q, "id", gtk_editable_get_text(GTK_EDITABLE(ed->id)));
        yyjson_mut_obj_add_strcpy(d, q, "path", gtk_editable_get_text(GTK_EDITABLE(ed->path)));
        yyjson_mut_obj_add_uint(d, q, "access", gtk_drop_down_get_selected(GTK_DROP_DOWN(ed->access)));
        send_doc(d, q);
    }
    gtk_window_destroy(GTK_WINDOW(ed->dialog));
}
static void edit_clicked(GtkButton *b, gpointer editing) {
    (void)b;
    if (ui.busy)
        return;
    if (sb_gui_bool(yyjson_doc_get_root(ui.model), "online")) {
        alert("请先停止后台，再修改项目。现有任务将随后台停止。");
        return;
    }
    editor *ed = g_new0(editor, 1);
    ed->editing = GPOINTER_TO_INT(editing);
    ed->dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(ed->dialog), ed->editing ? "编辑项目" : "添加项目");
    gtk_window_set_transient_for(GTK_WINDOW(ed->dialog), GTK_WINDOW(ui.window));
    gtk_window_set_modal(GTK_WINDOW(ed->dialog), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(ed->dialog), 620, 340);
    GtkWidget *content = box(true, 10);
    gtk_widget_set_margin_start(content, 16);
    gtk_widget_set_margin_end(content, 16);
    gtk_widget_set_margin_top(content, 16);
    gtk_widget_set_margin_bottom(content, 16);
    gtk_window_set_child(GTK_WINDOW(ed->dialog), content);
    g_object_set_data_full(G_OBJECT(ed->dialog), "editor", ed, g_free);
    ed->id = gtk_entry_new();
    ed->path = gtk_entry_new();
    const char *options[] = {"只读所选目录", "可修改所选目录（每次审批）",
                             "当前账户文件与普通命令（每次审批）", NULL};
    ed->access = gtk_drop_down_new_from_strings(options);
    append(content, label("项目标识"));
    append(content, ed->id);
    append(content, label("工作目录"));
    GtkWidget *line = box(false, 6);
    append(line, ed->path);
    gtk_widget_set_hexpand(ed->path, TRUE);
    GtkWidget *choose = gtk_button_new_with_label("选择目录…");
    append(line, choose);
    g_signal_connect(choose, "clicked", G_CALLBACK(choose_folder), ed->path);
    append(content, line);
    append(content, label("访问范围"));
    append(content, ed->access);
    append(content, label("普通命令以当前账户运行，可访问该账户的文件。连接授权与每次操作审批独立；不会获得管"
                          "理员权限。编辑将使用上面选定的权限方案。"));
    if (ed->editing) {
        yyjson_val *p = project_config(), *w = yyjson_arr_get(yyjson_obj_get(p, "workspaces"), 0),
                   *policy = yyjson_obj_get(p, "policy");
        gtk_editable_set_text(GTK_EDITABLE(ed->id), ui.project);
        gtk_widget_set_sensitive(ed->id, FALSE);
        gtk_editable_set_text(GTK_EDITABLE(ed->path), sb_gui_string(w, "path"));
        gtk_drop_down_set_selected(GTK_DROP_DOWN(ed->access),
                                   sb_gui_bool(yyjson_obj_get(policy, "execution"), "enabled") ? 2
                                   : sb_gui_bool(w, "read_only")                               ? 0
                                                                                               : 1);
    }
    GtkWidget *buttons = box(false, 8), *cancel = gtk_button_new_with_label("取消"),
              *save = gtk_button_new_with_label("保存");
    append(buttons, cancel);
    append(buttons, save);
    append(content, buttons);
    g_object_set_data(G_OBJECT(save), "save", GINT_TO_POINTER(1));
    g_signal_connect(save, "clicked", G_CALLBACK(editor_response), ed);
    g_signal_connect(cancel, "clicked", G_CALLBACK(editor_response), ed);
    gtk_window_present(GTK_WINDOW(ed->dialog));
}
static void remove_response(GObject *dialog, GAsyncResult *result, gpointer data) {
    int response = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(dialog), result, NULL);
    if (response == 0) {
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *q = yyjson_mut_obj(d);
        yyjson_mut_obj_add_str(d, q, "action", "remove");
        yyjson_mut_obj_add_strcpy(d, q, "id", data);
        send_doc(d, q);
        ui.project[0] = 0;
    }
    g_free(data);
}
static void remove_clicked(GtkButton *b, gpointer data) {
    (void)b;
    (void)data;
    GtkAlertDialog *d =
        gtk_alert_dialog_new("移除项目 %s？项目文件、运行记录和凭据会保留。请先停止后台。", ui.project);
    const char *buttons[] = {"移除", "取消", NULL};
    gtk_alert_dialog_set_buttons(d, buttons);
    gtk_alert_dialog_set_cancel_button(d, 1);
    gtk_alert_dialog_set_default_button(d, 1);
    gtk_alert_dialog_choose(d, GTK_WINDOW(ui.window), NULL, remove_response, g_strdup(ui.project));
    g_object_unref(d);
}
static void import_chosen(GObject *source, GAsyncResult *result, gpointer unused) {
    (void)unused;
    GError *e = NULL;
    GFile *f = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, &e);
    if (f) {
        char *p = g_file_get_path(f);
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *q = yyjson_mut_obj(d);
        yyjson_mut_obj_add_str(d, q, "action", "import");
        yyjson_mut_obj_add_strcpy(d, q, "path", p);
        send_doc(d, q);
        g_free(p);
        g_object_unref(f);
    }
    g_clear_error(&e);
}
static void import_clicked(GtkButton *b, gpointer data) {
    (void)b;
    (void)data;
    GtkFileDialog *d = gtk_file_dialog_new();
    gtk_file_dialog_set_title(d, "导入项目配置（原文件保留）");
    gtk_file_dialog_open(d, GTK_WINDOW(ui.window), NULL, import_chosen, NULL);
    g_object_unref(d);
}
static void settings_save(GtkButton *b, gpointer unused) {
    (void)b;
    (void)unused;
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *q = yyjson_mut_obj(d);
    yyjson_mut_obj_add_str(d, q, "action", "settings");
    yyjson_mut_obj_add_uint(d, q, "port",
                            (unsigned)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(ui.port)));
    yyjson_mut_obj_add_strcpy(d, q, "url", gtk_editable_get_text(GTK_EDITABLE(ui.url)));
    yyjson_mut_obj_add_bool(d, q, "http", gtk_check_button_get_active(GTK_CHECK_BUTTON(ui.http)));
    yyjson_mut_obj_add_bool(d, q, "oauth", gtk_check_button_get_active(GTK_CHECK_BUTTON(ui.oauth)));
    send_doc(d, q);
}
static void decision(GtkButton *b, gpointer data) {
    (void)b;
    const char *verb = data;
    if (!*ui.request_id)
        return;
    const char *action = !strcmp(ui.request_type, "operation") ? verb
                         : !strcmp(verb, "approve")            ? "oauth-approve"
                         : !strcmp(verb, "deny")               ? "oauth-deny"
                         : !strcmp(verb, "revoke")             ? "oauth-revoke"
                                                               : "oauth-remove-client";
    if (!strcmp(verb, "inspect")) {
        if (strcmp(ui.request_type, "operation")) {
            pretty(ui.request_detail, current_request());
            return;
        }
    } else if (!strcmp(ui.request_type, "pending") && strcmp(verb, "approve") && strcmp(verb, "deny"))
        return;
    else if (!strcmp(ui.request_type, "connections") && strcmp(verb, "revoke"))
        return;
    else if (!strcmp(ui.request_type, "clients") && strcmp(verb, "remove"))
        return;
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *q = yyjson_mut_obj(d);
    yyjson_mut_obj_add_str(d, q, "action", action);
    yyjson_mut_obj_add_strcpy(d, q, "id", ui.request_id);
    yyjson_mut_obj_add_bool(d, q, "persistent", gtk_check_button_get_active(GTK_CHECK_BUTTON(ui.persistent)));
    send_doc(d, q);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(ui.persistent), FALSE);
}
/* KDE's deployed interface name is retained alongside the freedesktop interface. */
static const char sni_xml[] =
    "<node><interface name='org.kde.StatusNotifierItem'><property name='Category' type='s' "
    "access='read'/><property name='Id' type='s' access='read'/><property name='Title' type='s' "
    "access='read'/><property name='Status' type='s' access='read'/><property name='WindowId' type='u' "
    "access='read'/><property name='IconName' type='s' access='read'/><property name='IconPixmap' "
    "type='a(iiay)' access='read'/><property name='ItemIsMenu' type='b' access='read'/><property name='Menu' "
    "type='o' access='read'/><property name='ToolTip' type='(sa(iiay)ss)' access='read'/><method "
    "name='Activate'><arg type='i' direction='in'/><arg type='i' direction='in'/></method><method "
    "name='SecondaryActivate'><arg type='i' direction='in'/><arg type='i' direction='in'/></method><method "
    "name='ContextMenu'><arg type='i' direction='in'/><arg type='i' direction='in'/></method><method "
    "name='Scroll'><arg type='i' direction='in'/><arg type='s' direction='in'/></method><signal "
    "name='NewStatus'><arg type='s'/></signal></interface></node>";
static const char menu_xml[] =
    "<node><interface name='com.canonical.dbusmenu'><property name='Version' type='u' "
    "access='read'/><property name='TextDirection' type='s' access='read'/><property name='Status' type='s' "
    "access='read'/><property name='IconThemePath' type='as' access='read'/><method name='GetLayout'><arg "
    "type='i' direction='in'/><arg type='i' direction='in'/><arg type='as' direction='in'/><arg type='u' "
    "direction='out'/><arg type='(ia{sv}av)' direction='out'/></method><method "
    "name='GetGroupProperties'><arg type='ai' direction='in'/><arg type='as' direction='in'/><arg "
    "type='a(ia{sv})' direction='out'/></method><method name='Event'><arg type='i' direction='in'/><arg "
    "type='s' direction='in'/><arg type='v' direction='in'/><arg type='u' direction='in'/></method><method "
    "name='AboutToShow'><arg type='i' direction='in'/><arg type='b' direction='out'/></method><signal "
    "name='LayoutUpdated'><arg type='u'/><arg type='i'/></signal>"
    "<method name='GetProperty'><arg type='i' direction='in'/><arg type='s' direction='in'/><arg type='v' "
    "direction='out'/></method>"
    "<method name='EventGroup'><arg type='a(isvu)' direction='in'/><arg type='ai' direction='out'/></method>"
    "<method name='AboutToShowGroup'><arg type='ai' direction='in'/><arg type='ai' direction='out'/><arg "
    "type='ai' direction='out'/></method>"
    "</interface></node>";
static GVariant *menu_props(int id) {
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
    if (!id) {
        g_variant_builder_add(&b, "{sv}", "children-display", g_variant_new_string("submenu"));
        return g_variant_builder_end(&b);
    }
    g_variant_builder_add(&b, "{sv}", "label",
                          g_variant_new_string(id == 1 ? "打开 ShellBot" : "退出 ShellBot"));
    g_variant_builder_add(&b, "{sv}", "enabled", g_variant_new_boolean(TRUE));
    g_variant_builder_add(&b, "{sv}", "visible", g_variant_new_boolean(TRUE));
    return g_variant_builder_end(&b);
}
static void tray_method(GDBusConnection *c, const gchar *sender, const gchar *path, const gchar *interface,
                        const gchar *method, GVariant *params, GDBusMethodInvocation *call, gpointer data) {
    (void)c;
    (void)sender;
    (void)path;
    (void)data;
    if (!strcmp(interface, "com.canonical.dbusmenu")) {
        if (!strcmp(method, "GetProperty")) {
            gint id;
            const gchar *name;
            g_variant_get(params, "(i&s)", &id, &name);
            GVariant *props = g_variant_ref_sink(menu_props(id));
            GVariant *value = id >= 0 && id <= 2 ? g_variant_lookup_value(props, name, NULL) : NULL;
            if (value) {
                g_dbus_method_invocation_return_value(call, g_variant_new("(v)", value));
                g_variant_unref(value);
            } else
                g_dbus_method_invocation_return_dbus_error(
                    call, "com.canonical.dbusmenu.Error.UnknownProperty", "Unknown menu item or property.");
            g_variant_unref(props);
            return;
        }
        if (!strcmp(method, "EventGroup") || !strcmp(method, "AboutToShowGroup")) {
            bool events = !strcmp(method, "EventGroup");
            GVariant *array = g_variant_get_child_value(params, 0);
            GVariantBuilder errors, updates;
            g_variant_builder_init(&errors, G_VARIANT_TYPE("ai"));
            g_variant_builder_init(&updates, G_VARIANT_TYPE("ai"));
            for (gsize i = 0; i < g_variant_n_children(array); i++) {
                GVariant *entry = g_variant_get_child_value(array, i),
                         *id_value = events ? g_variant_get_child_value(entry, 0) : g_variant_ref(entry);
                int id = g_variant_get_int32(id_value);
                if (id < 0 || id > 2)
                    g_variant_builder_add(&errors, "i", id);
                else if (events) {
                    GVariant *event = g_variant_get_child_value(entry, 1);
                    if (!strcmp(g_variant_get_string(event, NULL), "clicked")) {
                        if (id == 1)
                            present();
                        if (id == 2)
                            quit();
                    }
                    g_variant_unref(event);
                }
                g_variant_unref(id_value);
                g_variant_unref(entry);
            }
            g_variant_unref(array);
            if (events)
                g_dbus_method_invocation_return_value(call,
                                                      g_variant_new("(@ai)", g_variant_builder_end(&errors)));
            else
                g_dbus_method_invocation_return_value(call, g_variant_new("(@ai@ai)",
                                                                          g_variant_builder_end(&updates),
                                                                          g_variant_builder_end(&errors)));
            return;
        }
        if (!strcmp(method, "GetLayout")) {
            GVariant *parent_value = g_variant_get_child_value(params, 0),
                     *depth_value = g_variant_get_child_value(params, 1);
            int parent = g_variant_get_int32(parent_value), depth = g_variant_get_int32(depth_value);
            g_variant_unref(parent_value);
            g_variant_unref(depth_value);
            if (parent < 0 || parent > 2) {
                g_dbus_method_invocation_return_dbus_error(call, "com.canonical.dbusmenu.Error.UnknownId",
                                                           "Unknown menu item.");
                return;
            }
            GVariantBuilder children;
            g_variant_builder_init(&children, G_VARIANT_TYPE("av"));
            for (int id = 1; parent == 0 && depth != 0 && id <= 2; id++)
                g_variant_builder_add(&children, "v",
                                      g_variant_new("(i@a{sv}@av)", id, menu_props(id),
                                                    g_variant_new_array(G_VARIANT_TYPE_VARIANT, NULL, 0)));
            g_dbus_method_invocation_return_value(call, g_variant_new("(u(i@a{sv}@av))", 1u, parent,
                                                                      menu_props(parent),
                                                                      g_variant_builder_end(&children)));
            return;
        }
        if (!strcmp(method, "GetGroupProperties")) {
            GVariantBuilder b;
            g_variant_builder_init(&b, G_VARIANT_TYPE("a(ia{sv})"));
            for (int id = 1; id <= 2; id++)
                g_variant_builder_add(&b, "(i@a{sv})", id, menu_props(id));
            g_dbus_method_invocation_return_value(call,
                                                  g_variant_new("(@a(ia{sv}))", g_variant_builder_end(&b)));
            return;
        }
        if (!strcmp(method, "AboutToShow")) {
            g_dbus_method_invocation_return_value(call, g_variant_new("(b)", FALSE));
            return;
        }
        if (!strcmp(method, "Event")) {
            GVariant *v = g_variant_get_child_value(params, 0), *event = g_variant_get_child_value(params, 1);
            int id = g_variant_get_int32(v);
            if (!strcmp(g_variant_get_string(event, NULL), "clicked")) {
                if (id == 1)
                    present();
                if (id == 2)
                    quit();
            }
            g_variant_unref(v);
            g_variant_unref(event);
        }
    } else if (!strcmp(method, "Activate") || !strcmp(method, "SecondaryActivate") ||
               !strcmp(method, "ContextMenu"))
        present();
    g_dbus_method_invocation_return_value(call, NULL);
}
static GVariant *tray_property(GDBusConnection *c, const gchar *sender, const gchar *path,
                               const gchar *interface, const gchar *name, GError **error, gpointer data) {
    (void)c;
    (void)sender;
    (void)path;
    (void)error;
    (void)data;
    if (!strcmp(interface, "com.canonical.dbusmenu")) {
        if (!strcmp(name, "Version"))
            return g_variant_new_uint32(3);
        if (!strcmp(name, "TextDirection"))
            return g_variant_new_string("ltr");
        if (!strcmp(name, "IconThemePath"))
            return g_variant_new_strv(NULL, 0);
        return g_variant_new_string("normal");
    }
    if (!strcmp(name, "Category"))
        return g_variant_new_string("ApplicationStatus");
    if (!strcmp(name, "Id"))
        return g_variant_new_string("shellbot");
    if (!strcmp(name, "Title"))
        return g_variant_new_string("ShellBot");
    if (!strcmp(name, "Status"))
        return g_variant_new_string("Active");
    if (!strcmp(name, "WindowId"))
        return g_variant_new_uint32(0);
    if (!strcmp(name, "ItemIsMenu"))
        return g_variant_new_boolean(FALSE);
    if (!strcmp(name, "Menu"))
        return g_variant_new_object_path("/MenuBar");
    if (!strcmp(name, "IconPixmap"))
        return tray_pixels ? g_variant_ref(tray_pixels)
                           : g_variant_new_array(G_VARIANT_TYPE("(iiay)"), NULL, 0);
    if (!strcmp(name, "ToolTip"))
        return g_variant_new("(s@a(iiay)ss)", "shellbot",
                             g_variant_new_array(G_VARIANT_TYPE("(iiay)"), NULL, 0), "ShellBot",
                             "点击打开管理界面");
    return g_variant_new_string("shellbot");
}
static void registered(GObject *source, GAsyncResult *result, gpointer data) {
    (void)data;
    GError *e = NULL;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &e);
    GVariant *v = r ? g_variant_get_child_value(r, 0) : NULL;
    GVariant *value = v ? g_variant_get_variant(v) : NULL;
    bool had_tray = ui.tray;
    ui.tray = value && g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN) && g_variant_get_boolean(value);
    if (had_tray && !ui.tray)
        present();
    if (value)
        g_variant_unref(value);
    if (v)
        g_variant_unref(v);
    gtk_label_set_text(GTK_LABEL(ui.tray_label),
                       ui.tray ? "关闭窗口后可从托盘恢复。" : "托盘不可用，窗口将保持可访问。");
    if (r)
        g_variant_unref(r);
    g_clear_error(&e);
}
static void host_check(void) {
    if (ui.bus)
        g_dbus_connection_call(
            ui.bus, "org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
            "org.freedesktop.DBus.Properties", "Get",
            g_variant_new("(ss)", "org.kde.StatusNotifierWatcher", "IsStatusNotifierHostRegistered"),
            G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, registered, NULL);
}
static void registration_done(GObject *source, GAsyncResult *result, gpointer data) {
    (void)data;
    GError *e = NULL;
    GVariant *v = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &e);
    if (v) {
        g_variant_unref(v);
        host_check();
    }
    g_clear_error(&e);
}
static void watcher(GDBusConnection *connection, const gchar *name, const gchar *owner, gpointer unused) {
    (void)owner;
    (void)unused;
    g_dbus_connection_call(connection, name, "/StatusNotifierWatcher", "org.kde.StatusNotifierWatcher",
                           "RegisterStatusNotifierItem",
                           g_variant_new("(s)", g_dbus_connection_get_unique_name(connection)), NULL,
                           G_DBUS_CALL_FLAGS_NONE, 3000, NULL, registration_done, NULL);
}
static void lost(GDBusConnection *connection, const gchar *name, gpointer unused) {
    (void)connection;
    (void)name;
    (void)unused;
    ui.tray = false;
    gtk_label_set_text(GTK_LABEL(ui.tray_label), "托盘不可用，窗口将保持可访问。");
    present();
}
static void tray_init(void) {
    char *parent = g_path_get_dirname(sb_posix_program());
    char *icon = g_build_filename(parent, "share", "ShellBot", "shellbot-icon-small.png", NULL);
    if (!g_file_test(icon, G_FILE_TEST_IS_REGULAR)) {
        g_free(icon);
        icon = g_build_filename(parent, "..", "share", "ShellBot", "shellbot-icon-small.png", NULL);
    }
    g_free(parent);
    GdkPixbuf *pixels = gdk_pixbuf_new_from_file_at_scale(icon, 32, 32, TRUE, NULL);
    g_free(icon);
    if (pixels) {
        int w = gdk_pixbuf_get_width(pixels), h = gdk_pixbuf_get_height(pixels),
            stride = gdk_pixbuf_get_rowstride(pixels), channels = gdk_pixbuf_get_n_channels(pixels);
        const guchar *bytes = gdk_pixbuf_read_pixels(pixels);
        guchar *argb = g_malloc((gsize)w * h * 4);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                const guchar *p = bytes + y * stride + x * channels;
                guchar *out = argb + 4 * (y * w + x);
                out[0] = channels == 4 ? p[3] : 255;
                out[1] = p[0];
                out[2] = p[1];
                out[3] = p[2];
            }
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("a(iiay)"));
        g_variant_builder_add(&b, "(ii@ay)", w, h,
                              g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, argb, (gsize)w * h * 4, 1));
        tray_pixels = g_variant_ref_sink(g_variant_builder_end(&b));
        g_free(argb);
        g_object_unref(pixels);
    }
    GError *e = NULL;
    ui.bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &e);
    if (!ui.bus) {
        g_clear_error(&e);
        return;
    }
    ui.sni_info = g_dbus_node_info_new_for_xml(sni_xml, &e);
    ui.menu_info = g_dbus_node_info_new_for_xml(menu_xml, &e);
    static const GDBusInterfaceVTable table = {.method_call = tray_method, .get_property = tray_property};
    ui.sni_object = g_dbus_connection_register_object(ui.bus, "/StatusNotifierItem",
                                                      ui.sni_info->interfaces[0], &table, NULL, NULL, &e);
    ui.menu_object = g_dbus_connection_register_object(ui.bus, "/MenuBar", ui.menu_info->interfaces[0],
                                                       &table, NULL, NULL, &e);
    g_clear_error(&e);
    ui.watch = g_bus_watch_name_on_connection(ui.bus, "org.kde.StatusNotifierWatcher",
                                              G_BUS_NAME_WATCHER_FLAGS_NONE, watcher, lost, NULL, NULL);
}
static gboolean timer(gpointer unused) {
    (void)unused;
    if (sb_desktop_raise(ui.desktop))
        present();
    host_check();
    if (sb_posix_signal && !ui.busy) {
        ui.quitting = true;
        submit("exit");
        return G_SOURCE_REMOVE;
    }
#ifdef SHELLBOT_TESTS
    if (ui.smoke && !ui.busy && ++ui.ticks == 4) {
        GtkWidget *child = gtk_window_get_child(GTK_WINDOW(ui.window));
        GdkPaintable *paint = gtk_widget_paintable_new(child);
        GtkSnapshot *s = gtk_snapshot_new();
        gdk_paintable_snapshot(paint, GDK_SNAPSHOT(s), gtk_widget_get_width(child),
                               gtk_widget_get_height(child));
        GskRenderNode *node = gtk_snapshot_free_to_node(s);
        if (node) {
            GdkTexture *texture =
                gsk_renderer_render_texture(gtk_native_get_renderer(GTK_NATIVE(ui.window)), node, NULL);
            char *png = g_strconcat(ui.smoke, ".png", NULL);
            gdk_texture_save_to_png(texture, png);
            g_free(png);
            g_object_unref(texture);
            gsk_render_node_unref(node);
        }
        g_object_unref(paint);
        bool had_tray = ui.tray;
        if (had_tray)
            gtk_window_close(GTK_WINDOW(ui.window));
        bool hidden = had_tray && !gtk_widget_get_visible(ui.window);
        present();
        char *report = g_strdup_printf("{\"status\":\"passed\",\"native\":\"GTK4\",\"window\":true,\"hide_"
                                       "restore\":%s,\"tray_registered\":%s,\"no_tray_reachable\":%s}\n",
                                       hidden && gtk_widget_get_visible(ui.window) ? "true" : "false",
                                       ui.tray ? "true" : "false",
                                       !had_tray && gtk_widget_get_visible(ui.window) ? "true" : "false");
        g_file_set_contents(ui.smoke, report, -1, NULL);
        g_free(report);
        ui.quitting = true;
        submit("exit");
        return G_SOURCE_REMOVE;
    }
#endif
    if (!ui.busy && !ui.quitting)
        submit("refresh");
    return G_SOURCE_CONTINUE;
}
static void activate(GtkApplication *app, gpointer data) {
    (void)data;
    if (ui.window) {
        present();
        return;
    }
    ui.app = app;
    ui.window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(ui.window), "ShellBot");
    gtk_window_set_default_size(GTK_WINDOW(ui.window), 1080, 760);
    GtkWidget *root = box(true, 12);
    gtk_widget_set_margin_start(root, 18);
    gtk_widget_set_margin_end(root, 18);
    gtk_widget_set_margin_top(root, 14);
    gtk_widget_set_margin_bottom(root, 14);
    gtk_window_set_child(GTK_WINDOW(ui.window), root);
    ui.summary = label("ShellBot · 本机项目管理");
    append(root, ui.summary);
    GtkWidget *toolbar = box(false, 6);
    button(toolbar, "启动后台", "start_manager");
    button(toolbar, "停止后台", "stop_manager");
    button(toolbar, "刷新", "refresh");
    GtkWidget *exit = gtk_button_new_with_label("退出 ShellBot");
    g_signal_connect(exit, "clicked", G_CALLBACK(quit_clicked), NULL);
    append(toolbar, exit);
    append(root, toolbar);
    GtkWidget *tabs = gtk_notebook_new();
    gtk_widget_set_vexpand(tabs, TRUE);
    append(root, tabs);
    GtkWidget *projects = box(false, 12), *left = box(true, 8), *right = box(true, 8);
    gtk_widget_set_size_request(left, 240, -1);
    gtk_widget_set_hexpand(left, FALSE);
    append(projects, left);
    append(projects, right);
    gtk_widget_set_hexpand(right, TRUE);
    ui.projects = gtk_list_box_new();
    g_signal_connect(ui.projects, "row-selected", G_CALLBACK(select_project), NULL);
    append(left, scroll(ui.projects));
    GtkWidget *edits = box(false, 4);
    GtkWidget *b = gtk_button_new_with_label("添加");
    g_signal_connect(b, "clicked", G_CALLBACK(edit_clicked), GINT_TO_POINTER(0));
    append(edits, b);
    b = gtk_button_new_with_label("编辑");
    g_signal_connect(b, "clicked", G_CALLBACK(edit_clicked), GINT_TO_POINTER(1));
    append(edits, b);
    b = gtk_button_new_with_label("移除");
    g_signal_connect(b, "clicked", G_CALLBACK(remove_clicked), NULL);
    append(edits, b);
    append(left, edits);
    b = gtk_button_new_with_label("导入现有项目配置…");
    g_signal_connect(b, "clicked", G_CALLBACK(import_clicked), NULL);
    append(left, b);
    GtkWidget *actions = box(false, 6);
    button(actions, "启动项目", "start");
    button(actions, "停止项目", "stop_project");
    button(actions, "暂停新请求", "pause");
    button(actions, "恢复请求", "resume");
    append(right, actions);
    ui.details = text();
    append(right, scroll(ui.details));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), projects, gtk_label_new("项目"));
    GtkWidget *requests = box(false, 12), *rr = box(true, 8);
    ui.requests = gtk_list_box_new();
    g_signal_connect(ui.requests, "row-selected", G_CALLBACK(request_selected), NULL);
    GtkWidget *rs = scroll(ui.requests);
    gtk_widget_set_size_request(rs, 300, -1);
    gtk_widget_set_hexpand(rs, FALSE);
    gtk_widget_set_hexpand(rr, TRUE);
    append(requests, rs);
    append(requests, rr);
    ui.request_detail = text();
    append(rr, scroll(ui.request_detail));
    ui.persistent = gtk_check_button_new_with_label("持续授权至手动撤销：包括此项目今后允许的操作");
    append(rr, ui.persistent);
    append(rr, label("未选中时为限时连接。连接授权不代替每次操作审批；普通命令仍受项目权限限制。"));
    actions = box(false, 4);
    const char *verbs[] = {"inspect", "approve", "deny", "revoke", "remove"},
               *titles[] = {"查看完整操作", "允许", "拒绝", "撤销连接/操作", "删除客户端"};
    for (unsigned i = 0; i < 5; i++) {
        b = gtk_button_new_with_label(titles[i]);
        g_signal_connect(b, "clicked", G_CALLBACK(decision), (gpointer)verbs[i]);
        append(actions, b);
    }
    append(rr, actions);
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), requests, gtk_label_new("审批与连接"));
    GtkWidget *settings_box = box(true, 12);
    ui.http = gtk_check_button_new_with_label("启用本机 HTTP 网关（仅监听回环地址）");
    ui.oauth = gtk_check_button_new_with_label("启用 OAuth 连接授权");
    ui.port = gtk_spin_button_new_with_range(1, 65535, 1);
    ui.url = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(ui.url), "https://shellbot.example.com");
    append(settings_box, ui.http);
    append(settings_box, label("本机端口"));
    append(settings_box, ui.port);
    append(settings_box, label("公网 HTTPS 地址（隧道由你配置）"));
    append(settings_box, ui.url);
    append(settings_box, ui.oauth);
    append(settings_box,
           label("凭据由系统钥匙串保护。保存前请停止后台；修改后点击启动后台，不会自动启动项目。"));
    b = gtk_button_new_with_label("保存连接设置");
    g_signal_connect(b, "clicked", G_CALLBACK(settings_save), NULL);
    append(settings_box, b);
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), settings_box, gtk_label_new("连接设置"));
    ui.tray_label = label("正在连接系统托盘…");
    ui.message = label("");
    append(root, ui.tray_label);
    append(root, ui.message);
    g_signal_connect(ui.window, "close-request", G_CALLBACK(closing), NULL);
    tray_init();
    present();
    submit("refresh");
    g_timeout_add_seconds(2, timer, NULL);
}
int sb_native_main(int argc, char **argv) {
    const char *base = NULL, *key = NULL;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--data-dir") && i + 1 < argc)
            base = argv[++i];
        else if (!strcmp(argv[i], "--credential-key-file") && i + 1 < argc)
            key = argv[++i];
#ifdef SHELLBOT_TESTS
        else if (!strcmp(argv[i], "--ui-smoke") && i + 1 < argc)
            ui.smoke = argv[++i];
#endif
        else {
            fprintf(stderr, "Invalid desktop arguments.\n");
            return 2;
        }
    }
    char *default_base = NULL;
    if (!base) {
        default_base = g_build_filename(g_get_user_data_dir(), "ShellBot", NULL);
        g_mkdir_with_parents(g_get_user_data_dir(), 0700);
        base = default_base;
    }
    sb_error e = {0};
    ui.desktop = sb_desktop_open(base, key, &e);
    g_free(default_base);
    if (!ui.desktop) {
        fprintf(stderr, "ShellBot: %s\n", e.text);
        return 2;
    }
    GtkApplication *app = gtk_application_new("org.shellbot.desktop", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(app), 1, argv);
    if (ui.watch)
        g_bus_unwatch_name(ui.watch);
    if (ui.bus) {
        if (ui.sni_object)
            g_dbus_connection_unregister_object(ui.bus, ui.sni_object);
        if (ui.menu_object)
            g_dbus_connection_unregister_object(ui.bus, ui.menu_object);
        g_object_unref(ui.bus);
    }
    if (ui.sni_info)
        g_dbus_node_info_unref(ui.sni_info);
    if (ui.menu_info)
        g_dbus_node_info_unref(ui.menu_info);
    if (tray_pixels)
        g_variant_unref(tray_pixels);
    yyjson_doc_free(ui.model);
    sb_desktop_close(ui.desktop);
    g_object_unref(app);
    return status;
}
