#include "native.h"
#include "sb_projects.h"
#include "project_form.h"
#include "migration.h"
#include "oauth.h"
#include "admin_desktop.h"
#include "admin_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdarg.h>
#include <dirent.h>

extern char **environ;
struct sb_native_manager {
    sb_lease *lease;
    char directory[256], registry[SB_PATH_CAP], data[512], key[SB_PATH_CAP];
    char selected[64], message[768];
    yyjson_doc *inspected;
    pid_t child;
    bool last_ok;
    atomic_bool cancel;
    pthread_mutex_t location_mutex;
    char *clipboard;
    sb_admin_link administrator;
    yyjson_doc *admin_state;
    char approval_backend[16], inspected_backend[16], inspected_project[64], inspected_broker[33];
    uint64_t inspected_generation;
};
const char *sb_gui_string(yyjson_val *o, const char *key) {
    const char *s = yyjson_get_str(yyjson_obj_get(o, key));
    return s ? s : "";
}
bool sb_gui_bool(yyjson_val *o, const char *key) { return yyjson_get_bool(yyjson_obj_get(o, key)); }
static void add_text(char *out, size_t *used, const char *format, ...) {
    if (*used >= SB_RESPONSE_LIMIT)
        return;
    va_list args;
    va_start(args, format);
    int n = vsnprintf(out + *used, SB_RESPONSE_LIMIT + 1 - *used, format, args);
    va_end(args);
    if (n > 0)
        *used += (size_t)n < SB_RESPONSE_LIMIT - *used ? (size_t)n : SB_RESPONSE_LIMIT - *used;
}
char *sb_gui_project_text(yyjson_val *p, yyjson_val *gateway, yyjson_val *status) {
    char *out = calloc(1, SB_RESPONSE_LIMIT + 1);
    size_t used = 0;
    if (!out)
        return NULL;
    if (!p) {
        add_text(out, &used, "请添加或导入一个项目。\n项目默认只读，需要你明确启动。");
        return out;
    }
    const char *id = sb_gui_string(p, "id"), *url = sb_gui_string(gateway, "public_url");
    add_text(out, &used, "项目：%s\n", id);
    if (*url)
        add_text(out, &used, "连接地址：%s/projects/%s/mcp\n", url, id);
    else
        add_text(out, &used, "本机连接地址：http://127.0.0.1:%llu/projects/%s/mcp\n",
                 (unsigned long long)yyjson_get_uint(yyjson_obj_get(gateway, "port")), id);
    size_t i, max;
    yyjson_val *v;
    yyjson_arr_foreach(yyjson_obj_get(status, "projects"), i, max,
                       v) if (!strcmp(sb_gui_string(v, "project_id"), id)) {
        add_text(out, &used, "状态：%s\n",
                 sb_gui_bool(v, "paused")    ? "已暂停新请求"
                 : sb_gui_bool(v, "running") ? "运行中"
                                             : "已停止");
        if (*sb_gui_string(v, "reason"))
            add_text(out, &used, "后台信息：%s\n", sb_gui_string(v, "reason"));
    }
    yyjson_val *policy = yyjson_obj_get(p, "policy");
    const char *approval = sb_gui_string(policy, "approval_mode");
    add_text(out, &used, "\n操作审批：%s\n",
             !strcmp(approval, "never")            ? "项目已预授权，按当前权限执行"
             : !strcmp(approval, "workspace_auto") ? "工作区自动，其他操作仍需审批"
                                                   : "每次操作需要本机确认");
    add_text(out, &used, "文件范围：%s\n",
             !strcmp(sb_gui_string(yyjson_obj_get(policy, "files"), "scope"), "current_user")
                 ? "当前账户可访问的文件"
                 : "仅所列目录");
    add_text(out, &used, "普通命令：%s\n管理员许可：%s\n\n目录：\n",
             sb_gui_bool(yyjson_obj_get(policy, "execution"), "enabled")
                 ? "允许以当前账户执行，仍按项目规则审批"
                 : "未启用",
             sb_gui_bool(yyjson_obj_get(policy, "execution"), "administrator_allowed")
                 ? "已允许；当前会话状态见“管理员”页" : "未允许");
    const char *arrays[] = {"workspaces", "extra_directories"};
    for (unsigned j = 0; j < 2; j++)
        yyjson_arr_foreach(yyjson_obj_get(p, arrays[j]), i, max, v)
            add_text(out, &used, "%s  %s\n", sb_gui_bool(v, "read_only") ? "只读" : "可读写",
                     sb_gui_string(v, "path"));
    add_text(out, &used, "\n记录保存位置：%s\n\n修改配置前请先停止后台；保存后手动启动后台和项目。\n",
             sb_gui_string(p, "data_dir"));
    return out;
}
char *sb_gui_request_text(yyjson_val *v, yyjson_val *configuration) {
    char *out = calloc(1, SB_RESPONSE_LIMIT + 1);
    size_t used = 0;
    if (!out)
        return NULL;
    if (!v) {
        add_text(out, &used,
                 "请选择一个申请或连接查看详情。\n\n操作审批：先查看完整操作，再允许或拒绝。\n连接申请：核对"
                 "客户端、项目和回调地址。\n已授权连接：可以随时撤销。\n");
        return out;
    }
    bool operation = *sb_gui_string(v, "approval_id");
    if (operation) {
        add_text(out, &used, "操作：%s\n项目：%s\n目标：%s\n发起者：%s\n状态：%s\n\n",
                 sb_gui_string(v, "tool"), sb_gui_string(v, "project_id"), sb_gui_string(v, "target"),
                 sb_gui_string(v, "actor"), sb_gui_string(v, "status"));
        if (yyjson_obj_get(v, "original_text"))
            add_text(out, &used, "原内容：\n%s\n\n", sb_gui_string(v, "original_text"));
        if (yyjson_obj_get(v, "text"))
            add_text(out, &used, "请求内容：\n%s\n\n", sb_gui_string(v, "text"));
        else
            add_text(out, &used, "点击“查看完整操作”，核对内容后再作出决定。\n");
        add_text(out, &used, "本次审批仅适用于此项目、此运行实例和这个具体操作。\n");
    } else {
        add_text(out, &used, "客户端：%s\n项目：%s\n回调地址：%s\n请求范围：%s\n状态：%s\n",
                 sb_gui_string(v, "client_name"), sb_gui_string(v, "project"),
                 sb_gui_string(v, "redirect_uri"), sb_gui_string(v, "scope"), sb_gui_string(v, "status"));
        if (yyjson_obj_get(v, "persistent"))
            add_text(out, &used, "连接期限：%s\n",
                     sb_gui_bool(v, "persistent") ? "持续至手动撤销" : "限时连接");
        if (*sb_gui_string(v, "revocation_reason"))
            add_text(out, &used, "撤销原因：%s\n", sb_gui_string(v, "revocation_reason"));
        add_text(out, &used,
                 "\n连接授权允许客户端按项目权限发起请求。每次操作审批、普通命令和管理员权限仍分别受项目设置"
                 "限制。\n持续授权还包括此项目今后允许的操作。\n");
        size_t i, max;
        yyjson_val *p;
        yyjson_arr_foreach(yyjson_obj_get(configuration, "projects"), i, max,
                           p) if (!strcmp(sb_gui_string(p, "id"), sb_gui_string(v, "project"))) {
            char *project = sb_gui_project_text(p, yyjson_obj_get(configuration, "gateway"), NULL);
            if (project) {
                add_text(out, &used, "\n当前项目权限：\n%s", project);
                free(project);
            }
        }
    }
    return out;
}
char *sb_gui_desktop_text(yyjson_val *v) {
    char *out = calloc(1, 4096);
    if (!out)
        return NULL;
    if (!v || *sb_gui_string(v, "error")) {
        snprintf(out, 4096, "请启用所选项目的桌面权限并启动项目，然后点击连接桌面。\n%s",
                 sb_gui_string(v, "error"));
        return out;
    }
    snprintf(
        out, 4096,
        "桌面连接：%s\n控制状态：%s\n短时租约：%s\n人工输入自动暂停：%s\n\n%"
        "s\n\n连接需要本机确认。一次连接不会自动申请租约；远程操作仍按项目审批规则执行。\n暂停后只有本机恢复"
        "，需重新申请租约、取得新截图。\n屏幕查看会披露共享屏幕上的其他应用，文件目录范围不会裁剪画面。",
        sb_gui_bool(v, "available")    ? "已连接"
        : sb_gui_bool(v, "connecting") ? "等待系统确认或连接中"
                                       : "未连接",
        sb_gui_bool(v, "paused") ? "已暂停" : "可申请控制", sb_gui_bool(v, "leased") ? "使用中" : "空闲",
        sb_gui_bool(v, "human_input_pause_supported") ? "支持" : "此后端不支持；可在本机或系统共享入口停止",
        sb_gui_string(v, "last_failure"));
    return out;
}
static char *load(const char *path) {
    sb_error e = {0};
    size_t n;
    return sb_platform_load_config(path, &n, &e);
}
static yyjson_doc *parse(char *bytes) {
    yyjson_doc *d = bytes ? yyjson_read(bytes, strlen(bytes), 0) : NULL;
    free(bytes);
    return d;
}
static yyjson_doc *registry(sb_native_manager *g) { return parse(load(g->registry)); }
static char *wire(const char *command, const char *project, const char *id) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *v = yyjson_mut_obj(d);
    yyjson_mut_obj_add_str(d, v, "command", command);
    if (project && *project)
        yyjson_mut_obj_add_str(d, v, "project_id", project);
    if (id && *id)
        yyjson_mut_obj_add_str(d, v, "id", id);
    return sb_posix_json(d, v);
}
static yyjson_doc *exchange(sb_native_manager *g, const char *command, const char *project, const char *id) {
    char *q = wire(command, project, id);
    sb_error e = {0};
    char *r = sb_owner_exchange(g->data, q, strlen(q), &e);
    free(q);
    return parse(r);
}
static bool online(sb_native_manager *g) {
    yyjson_doc *d = exchange(g, "status", NULL, NULL);
    bool ok = yyjson_obj_get(yyjson_doc_get_root(d), "manager_pid") != NULL;
    yyjson_doc_free(d);
    return ok;
}
static void admin_close(sb_native_manager *g);
static bool private_dir(const char *path, sb_error *e) {
    if (mkdir(path, 0700) && errno != EEXIST) {
        sb_posix_error(e, "Create desktop directory");
        return false;
    }
    sb_root *r = sb_platform_root(path, e);
    bool ok = r && sb_posix_private(r, e);
    sb_platform_root_close(r);
    return ok;
}
/* Immutable exports retain a flushed backup. Only the desktop's private copy is replaced. */
static bool save(sb_native_manager *g, const char *name, const char *json, sb_error *e) {
    char target[SB_PATH_CAP], output[SB_PATH_CAP], backup[SB_PATH_CAP], nonce[33];
    unsigned char random[16];
    if (!sb_platform_random(random, sizeof(random), e))
        return false;
    for (unsigned i = 0; i < 16; i++)
        snprintf(nonce + 2 * i, 3, "%02x", random[i]);
    snprintf(target, sizeof(target), "%s/%s", g->directory, name);
    snprintf(output, sizeof(output), "%s/new-%s.json", g->directory, nonce);
    snprintf(backup, sizeof(backup), "%s/backup-%s.json", g->directory, nonce);
    char *old = load(target);
    bool ok = sb_platform_export_config(output, backup, json, strlen(json), old ? old : "{}",
                                        old ? strlen(old) : 2, e);
    free(old);
    if (ok && rename(output, target)) {
        sb_posix_error(e, "Apply desktop configuration");
        ok = false;
    }
    if (ok && fsync(g->lease->root->fd)) {
        sb_posix_error(e, "Flush desktop configuration");
        ok = false;
    }
    return ok;
}
static bool save_registry(sb_native_manager *g, const char *json, sb_error *e) {
    sb_project_registry r = {0};
    bool ok = sb_projects_parse(&r, json, strlen(json), e);
    sb_projects_free(&r);
    return ok && save(g, strrchr(g->registry, '/') + 1, json, e);
}
static yyjson_doc *settings(sb_native_manager *g) {
    char path[SB_PATH_CAP];
    snprintf(path, sizeof(path), "%s/desktop.json", g->directory);
    yyjson_doc *d = parse(load(path));
    if (!d)
        d = yyjson_read("{\"http\":true,\"oauth\":false}", 27, 0);
    return d;
}
static char *run(sb_native_manager *g, char *const argv[], bool daemon, sb_error *e);
static yyjson_doc *startup(sb_native_manager *g, const char *action, sb_error *e) {
    char state[SB_PATH_CAP], id[64], hash[65];
    snprintf(state,sizeof(state),"%s/startup",g->directory);
    sb_platform_sha256(g->directory,strlen(g->directory),hash,e);
    snprintf(id,sizeof(id),"desktop-%.20s",hash);
    char *args[16]={(char *)sb_posix_program(),(char *)action,"--startup-dir",state,NULL};
    if(!strcmp(action,"--startup-install")){
        unsigned n=4;args[n++]="--kind";args[n++]="desktop";args[n++]="--name";args[n++]=id;
        args[n++]="--desktop-data-dir";args[n++]=g->directory;
        if(*g->key){args[n++]="--credential-key-file";args[n++]=g->key;}args[n]=NULL;
    }
    return parse(run(g,args,false,e));
}
/* Spawn a fixed local executable without a shell, with bounded capture and time. */
static char *run(sb_native_manager *g, char *const argv[], bool daemon, sb_error *e) {
    int pipes[2] = {-1, -1};
    if (pipe(pipes)) {
        sb_posix_error(e, "Create backend output channel");
        return NULL;
    }
    fcntl(pipes[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipes[1], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    if (daemon) {
        char log[SB_PATH_CAP];
        snprintf(log, sizeof(log), "%s/backend.log", g->directory);
        int fd = open(log, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0600);
        struct stat st;
        if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_uid != geteuid() ||
            (st.st_mode & 077)) {
            if (fd >= 0)
                close(fd);
            close(pipes[0]);
            close(pipes[1]);
            posix_spawn_file_actions_destroy(&fa);
            sb_fail(e, "Unsafe backend log file.");
            return NULL;
        }
        posix_spawn_file_actions_adddup2(&fa, fd, 1);
        posix_spawn_file_actions_adddup2(&fa, fd, 2);
        posix_spawn_file_actions_addclose(&fa, fd);
        close(pipes[1]);
        pipes[1] = fd;
    } else {
        posix_spawn_file_actions_adddup2(&fa, pipes[1], 1);
        posix_spawn_file_actions_adddup2(&fa, pipes[1], 2);
        posix_spawn_file_actions_addclose(&fa, pipes[0]);
        posix_spawn_file_actions_addclose(&fa, pipes[1]);
    }
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t empty, defaults;
    sigemptyset(&empty);
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);
    sigaddset(&defaults, SIGTERM);
    sigaddset(&defaults, SIGINT);
    posix_spawnattr_setsigmask(&attr, &empty);
    posix_spawnattr_setsigdefault(&attr, &defaults);
    posix_spawnattr_setpgroup(&attr, 0);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETPGROUP);
    pid_t pid = 0;
    int rc = posix_spawn(&pid, sb_posix_program(), &fa, &attr, argv, environ);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&fa);
    close(pipes[1]);
    if (rc) {
        close(pipes[0]);
        snprintf(e->text, sizeof(e->text), "Cannot launch backend: %s", strerror(rc));
        return NULL;
    }
    if (daemon) {
        close(pipes[0]);
        g->child = pid;
        return sb_strdup("{}");
    }
    char *out = malloc(SB_RESPONSE_LIMIT + 1);
    size_t at = 0;
    uint64_t end = sb_platform_millis() + 20000;
    int status = 0;
    bool finished = false;
    while (out && sb_platform_millis() < end && at < SB_RESPONSE_LIMIT) {
        struct pollfd p = {.fd = pipes[0], .events = POLLIN};
        int n = poll(&p, 1, 100);
        if (n > 0) {
            ssize_t got = read(pipes[0], out + at, SB_RESPONSE_LIMIT - at);
            if (got > 0)
                at += (size_t)got;
            else if (!got) {
                finished = true;
                break;
            }
        }
    }
    close(pipes[0]);
    if (!finished) {
        kill(-pid, SIGTERM);
        if (!sb_posix_wait_child(pid, 1000, &status)) {
            kill(-pid, SIGKILL);
            waitpid(pid, &status, 0);
        }
        free(out);
        sb_fail(e, "Backend operation timed out or exceeded its output limit.");
        return NULL;
    }
    waitpid(pid, &status, 0);
    out[at] = 0;
    if (!WIFEXITED(status) || WEXITSTATUS(status)) {
        sb_fail(e, out);
        free(out);
        return NULL;
    }
    return out;
}
static bool credentials(sb_native_manager *g, yyjson_val *projects, sb_error *e) {
    char *args[14] = {(char *)sb_posix_program(), "--credential-status", "--manager-data-dir", g->data, NULL};
    unsigned n = 4;
    if (*g->key) {
        args[n++] = "--credential-key-file";
        args[n++] = g->key;
        args[n] = NULL;
    }
    yyjson_doc *catalog = parse(run(g, args, false, e));
    if (!catalog)
        return false;
    size_t i, max;
    yyjson_val *p;
    yyjson_arr_foreach(projects, i, max, p) {
        if (yyjson_obj_get(p, "enabled") && !sb_gui_bool(p, "enabled"))
            continue;
        const char *id = sb_gui_string(p, "id"), *env = sb_gui_string(p, "token_env");
        bool found = false;
        size_t j, jmax;
        yyjson_val *entry;
        yyjson_arr_foreach(yyjson_obj_get(yyjson_doc_get_root(catalog), "projects"), j, jmax, entry) {
            if (!strcmp(id, sb_gui_string(entry, "id"))) {
                found = true;
                if (strcmp(env, sb_gui_string(entry, "token_env"))) {
                    sb_fail(e, "Credential reference changed; resolve it with the credential CLI.");
                    yyjson_doc_free(catalog);
                    return false;
                }
            }
        }
        if (!found && !getenv(env)) {
            char *generate[16] = {(char *)sb_posix_program(),
                                  "--credential-generate",
                                  (char *)id,
                                  "--token-env",
                                  (char *)env,
                                  "--manager-data-dir",
                                  g->data,
                                  NULL};
            n = 7;
            if (*g->key) {
                generate[n++] = "--credential-key-file";
                generate[n++] = g->key;
                generate[n] = NULL;
            }
            char *r = run(g, generate, false, e);
            if (!r) {
                yyjson_doc_free(catalog);
                return false;
            }
            free(r);
        }
    }
    yyjson_doc_free(catalog);
    yyjson_doc *cfg = registry(g);
    const char *owner_env = sb_gui_string(yyjson_obj_get(yyjson_doc_get_root(cfg), "gateway"), "owner_token_env");
    if (*owner_env) {
        sb_lease *l = sb_platform_lease(g->data, e);
        sb_secret_options opts = {.key_file=*g->key?g->key:NULL};
        sb_secrets *s = l ? sb_secrets_open(l,&opts,e) : NULL;
        char *bearer = s ? sb_owner_bearer(s,owner_env,true,e) : NULL;
        bool ready = bearer != NULL;
        if(bearer){sb_posix_wipe(bearer,strlen(bearer));free(bearer);}
        sb_secrets_close(s);sb_platform_lease_close(l);
        yyjson_doc_free(cfg);return ready;
    }
    yyjson_doc_free(cfg);
    return true;
}
static bool launch(sb_native_manager *g, sb_error *e) {
    if (online(g)) {
        sb_fail(e, "Manager is already running; the desktop attached to it.");
        return false;
    }
    yyjson_doc *cfg = registry(g), *s = settings(g);
    yyjson_val *projects = yyjson_obj_get(yyjson_doc_get_root(cfg), "projects"),
               *opts = yyjson_doc_get_root(s);
    bool ok = false;
    if (!yyjson_arr_size(projects)) {
        sb_fail(e, "请先添加一个项目。");
        goto done;
    }
    bool http = sb_gui_bool(opts, "http"), oauth = sb_gui_bool(opts, "oauth");
    if (oauth && !http) {
        sb_fail(e, "OAuth 需要启用本机 HTTP 网关。");
        goto done;
    }
    if (http && !credentials(g, projects, e))
        goto done;
    char *args[52] = {(char *)sb_posix_program(), "--manager", "--projects", g->registry,
                      "--manager-data-dir",       g->data,     NULL};
    unsigned n = 6;
    if (http) {
        args[n++] = "--http";
        args[n++] = "--credential-store";
    }
    if (oauth)
        args[n++] = "--oauth";
    if (*g->key) {
        args[n++] = "--credential-key-file";
        args[n++] = g->key;
    }
    yyjson_val *automatic = yyjson_obj_get(opts, "project_options");
    size_t ai, an; yyjson_val *ap;
    yyjson_arr_foreach(projects, ai, an, ap) {
        const char *pid = sb_gui_string(ap, "id");
        if (sb_gui_bool(yyjson_obj_get(automatic, pid), "auto_start") &&
            (!yyjson_obj_get(ap, "enabled") || sb_gui_bool(ap, "enabled"))) {
            args[n++] = "--start-project"; args[n++] = (char *)pid;
        }
    }
    args[n] = NULL;
    char *r = run(g, args, true, e);
    if (!r)
        goto done;
    free(r);
    for (unsigned i = 0; i < 100; i++) {
        if (online(g)) {
            ok = true;
            break;
        }
        if (waitpid(g->child, NULL, WNOHANG) == g->child) {
            g->child = 0;
            break;
        }
        struct timespec t = {.tv_nsec = 100000000};
        nanosleep(&t, NULL);
    }
    if (!ok) {
        if (g->child) {
            int status = 0;
            kill(g->child, SIGTERM);
            if (!sb_posix_wait_child(g->child, 2000, &status)) {
                kill(g->child, SIGKILL);
                waitpid(g->child, &status, 0);
            }
            g->child = 0;
        }
        sb_fail(e, "后台未就绪；请查看数据目录中的 backend.log。凭据服务可能需要先解锁。");
    }
done:
    yyjson_doc_free(cfg);
    yyjson_doc_free(s);
    return ok;
}
static bool stopped(sb_native_manager *g, sb_error *e) {
    if (g->administrator.fd >= 0 || sb_admin_link_closing(&g->administrator)) {
        sb_fail(e, "请先结束管理员会话，再修改或迁移项目配置。"); return false;
    }
    if (online(g)) {
        sb_fail(e, "请先停止后台，再保存配置。停止会结束运行中的项目和命令。");
        return false;
    }
    /* A stopped gateway is not sufficient if another manager still owns Data. */
    sb_lease *l = sb_platform_lease(g->data, e);
    if (!l)
        return false;
    sb_platform_lease_close(l);
    return true;
}
static bool stop_manager(sb_native_manager *g, sb_error *e) {
    yyjson_doc *d = exchange(g, "stop", NULL, NULL);
    bool ok = sb_gui_bool(yyjson_doc_get_root(d), "stopping");
    yyjson_doc_free(d);
    if (!ok) {
        sb_fail(e, "无法停止后台；没有向其他进程发送信号。");
        return false;
    }
    for (unsigned i = 0; i < 120; i++) {
        sb_lease *l = sb_platform_lease(g->data, e);
        if (l) {
            sb_platform_lease_close(l);
            if (g->child) {
                waitpid(g->child, NULL, WNOHANG);
                g->child = 0;
            }
            e->text[0] = 0;
            return true;
        }
        struct timespec t = {.tv_nsec = 100000000};
        nanosleep(&t, NULL);
    }
    sb_fail(e, "后台仍在结束任务，请稍后刷新。");
    return false;
}
static yyjson_val *find(yyjson_doc *doc, const char *array, const char *key, const char *id) {
    size_t i, max;
    yyjson_val *v;
    yyjson_arr_foreach(yyjson_obj_get(yyjson_doc_get_root(doc), array), i, max,
                       v) if (!strcmp(sb_gui_string(v, key), id)) return v;
    return NULL;
}
static char *project_request(sb_native_manager *g, const char *request, sb_error *e) {
    yyjson_doc *local = yyjson_read(request, strlen(request), 0);
    const char *command = sb_gui_string(yyjson_doc_get_root(local), "command");
    if (!strncmp(command, "desktop-", 8)) {
        yyjson_doc *cfg = registry(g);
        yyjson_val *project = find(cfg, "projects", "id", g->selected);
        const char *path = sb_gui_string(project, "data_dir");
        char *reply = *path ? sb_owner_exchange(path, request, strlen(request), e) : NULL;
        yyjson_doc_free(cfg);
        yyjson_doc_free(local);
        return reply;
    }
    yyjson_doc_free(local);
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *v = yyjson_mut_obj(d);
    yyjson_mut_obj_add_str(d, v, "command", "owner");
    yyjson_mut_obj_add_str(d, v, "project_id", g->selected);
    yyjson_mut_obj_add_str(d, v, "request", request);
    char *q = sb_posix_json(d, v), *reply = sb_owner_exchange(g->data, q, strlen(q), e);
    free(q);
    yyjson_doc *p = parse(reply);
    char *out = NULL;
    if (*sb_gui_string(yyjson_doc_get_root(p), "error"))
        sb_fail(e, sb_gui_string(yyjson_doc_get_root(p), "error"));
    else if (*sb_gui_string(yyjson_doc_get_root(p), "response"))
        out = sb_strdup(sb_gui_string(yyjson_doc_get_root(p), "response"));
    yyjson_doc_free(p);
    return out;
}
static bool edit(sb_native_manager *g, yyjson_val *q, sb_error *e) {
    const char *action = sb_gui_string(q, "action"), *id = sb_gui_string(q, "id"),
               *path = sb_gui_string(q, "path");
    if (!stopped(g, e))
        return false;
    char *old = load(g->registry), *output = NULL;
    bool ok = false;
    if (!old) {
        sb_fail(e, "项目配置不可读，原文件已保留。请恢复备份后重新打开界面。");
        return false;
    }
    if (!strcmp(action, "import")) {
        output = load(path);
        if (!output)
            sb_fail(e, "无法读取选择的项目配置。");
    } else if (!strcmp(action, "remove"))
        output = sb_projects_remove(old, strlen(old), id, e);
    else {
        if (!sb_identifier(id)) {
            sb_fail(e, "项目标识只能使用字母、数字、下划线或连字符（最多 63 字符）。");
            goto done;
        }
        sb_root *r = sb_platform_root(path, e);
        if (!r)
            goto done;
        yyjson_doc *cfg = registry(g);
        yyjson_val *existing = find(cfg, "projects", "id", id);
        bool replace = !strcmp(action, "edit");
        if (replace && (!existing || yyjson_arr_size(yyjson_obj_get(existing, "workspaces")) != 1 ||
                        yyjson_arr_size(yyjson_obj_get(existing, "extra_directories")))) {
            sb_fail(e, "此项目含复杂目录配置；请使用原配置编辑工具后重新导入。");
            sb_platform_root_close(r);
            yyjson_doc_free(cfg);
            goto done;
        }
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *p = existing ? yyjson_val_mut_copy(d, existing) : yyjson_mut_obj(d);
        yyjson_mut_obj_remove_key(p, "id");
        yyjson_mut_obj_add_str(d, p, "id", id);
        char data[SB_PATH_CAP], env[64];
        snprintf(data, sizeof(data), "%s/project-%s", g->directory, id);
        /* Hashing avoids environment-name truncation/case collisions. */
        char hash[65];
        sb_platform_sha256(id, strlen(id), hash, e);
        snprintf(env, sizeof(env), "SHELLBOT_GUI_%.40s", hash);
        if (!existing) {
            yyjson_mut_obj_add_strcpy(d, p, "data_dir", data);
            yyjson_mut_obj_add_strcpy(d, p, "token_env", env);
        }
        unsigned mode = (unsigned)yyjson_get_uint(yyjson_obj_get(q, "access"));
        if (mode > 2)
            mode = 0;
        yyjson_mut_val *ws = yyjson_mut_arr(d), *w = yyjson_mut_obj(d), *policy = yyjson_mut_obj(d),
                       *files = yyjson_mut_obj(d), *execution = yyjson_mut_obj(d), *tools = yyjson_mut_arr(d);
        yyjson_mut_obj_add_str(d, w, "id", "workspace");
        yyjson_mut_obj_add_strcpy(d, w, "path", r->path);
        yyjson_mut_obj_add_bool(d, w, "read_only", mode == 0);
        yyjson_mut_arr_add_val(ws, w);
        yyjson_mut_obj_remove_key(p, "workspaces");
        yyjson_mut_obj_remove_key(p, "default_workspace_id");
        yyjson_mut_obj_add_val(d, p, "workspaces", ws);
        yyjson_mut_obj_add_str(d, policy, "approval_mode", "always");
        yyjson_mut_obj_add_str(d, files, "scope", mode == 2 ? "current_user" : "configured");
        if (mode == 2)
            yyjson_mut_obj_add_bool(d, files, "current_user_read_only", false);
        yyjson_mut_obj_add_bool(d, execution, "enabled", mode == 2);
        if (mode == 2)
            yyjson_mut_obj_add_str(d, execution, "default_identity", "current_user");
        static const char *const names[] = {
            "host_info",   "host_capabilities", "fs_list",         "fs_read",         "fs_search",
            "fs_write",    "fs_patch",          "fs_stat",         "fs_mkdir",        "fs_copy",
            "fs_move",     "fs_delete",         "approval_status", "approval_cancel", "task_start",
            "task_status", "task_output",       "task_input",      "task_cancel"};
        for (unsigned i = 0; i < (mode == 2 ? 19u : 14u); i++)
            yyjson_mut_arr_add_str(d, tools, names[i]);
        const char *media[] = {"image_view", "document_info", "document_read", "document_preview", "artifact_download"};
        for (unsigned i = 0; i < sizeof(media)/sizeof(media[0]); i++)
            yyjson_mut_arr_add_str(d, tools, media[i]);
        if (mode != 0)
            yyjson_mut_arr_add_str(d, tools, "artifact_upload");
        /* Ordinary directory edits preserve the independent desktop permission and tool switches. */
        yyjson_val *previous = yyjson_obj_get(existing, "policy"),
                   *desktop = yyjson_obj_get(previous, "desktop");
        if (desktop)
            yyjson_mut_obj_add_val(d, policy, "desktop", yyjson_val_mut_copy(d, desktop));
        yyjson_val *prior_tools = yyjson_obj_get(previous, "tools");
        size_t ti, tn;
        yyjson_val *t;
        yyjson_arr_foreach(prior_tools, ti, tn, t) if (sb_desktop_tool(sb_string(t)))
            yyjson_mut_arr_add_strcpy(d, tools, sb_string(t));
        yyjson_mut_obj_add_val(d, policy, "files", files);
        yyjson_mut_obj_add_val(d, policy, "execution", execution);
        yyjson_mut_obj_add_val(d, policy, "tools", tools);
        yyjson_mut_obj_remove_key(p, "policy");
        yyjson_mut_obj_add_val(d, p, "policy", policy);
        char *definition = sb_posix_json(d, p);
        output = sb_projects_edit(old, strlen(old), definition, strlen(definition), id, replace, e);
        free(definition);
        sb_platform_root_close(r);
        yyjson_doc_free(cfg);
    }
    if (output) {
        ok = save_registry(g, output, e);
        if (ok && *id)
            snprintf(g->selected, sizeof(g->selected), "%s", id);
    }
done:
    free(old);
    free(output);
    return ok;
}
static bool desktop_permissions(sb_native_manager *g, yyjson_val *q, sb_error *e) {
    if (!stopped(g, e))
        return false;
    const char *keys[] = {"enabled", "control", "clipboard_read", "clipboard_write"};
    for (unsigned i = 0; i < 4; i++)
        if (!yyjson_is_bool(yyjson_obj_get(q, keys[i]))) {
            sb_fail(e, "桌面权限需要明确的开关值。");
            return false;
        }
    bool enabled = sb_gui_bool(q, "enabled");
    if (!enabled && (sb_gui_bool(q, "control") || sb_gui_bool(q, "clipboard_read") ||
                     sb_gui_bool(q, "clipboard_write"))) {
        sb_fail(e, "请先启用屏幕查看，再启用输入或剪贴板。");
        return false;
    }
    yyjson_doc *cfg = registry(g);
    yyjson_val *existing = find(cfg, "projects", "id", g->selected);
    if (!existing) {
        yyjson_doc_free(cfg);
        sb_fail(e, "请先选择项目。");
        return false;
    }
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *p = yyjson_val_mut_copy(d, existing), *policy = yyjson_mut_obj_get(p, "policy");
    if (!policy) {
        policy = yyjson_mut_obj(d);
        yyjson_mut_obj_add_val(d, p, "policy", policy);
    }
    yyjson_val *prior = yyjson_obj_get(yyjson_obj_get(existing, "policy"), "desktop");
    yyjson_mut_val *desktop = prior ? yyjson_val_mut_copy(d, prior) : yyjson_mut_obj(d);
    for (unsigned i = 0; i < 4; i++) {
        yyjson_mut_obj_remove_key(desktop, keys[i]);
        yyjson_mut_obj_add_bool(d, desktop, keys[i], sb_gui_bool(q, keys[i]));
    }
    yyjson_mut_obj_remove_key(policy, "desktop");
    yyjson_mut_obj_add_val(d, policy, "desktop", desktop);
    sb_project_registry parsed = {0};
    char *old = load(g->registry);
    bool ok = old && sb_projects_parse(&parsed, old, strlen(old), e);
    const sb_project *project = ok ? sb_project_find(&parsed, g->selected) : NULL;
    if (project) {
        yyjson_mut_val *tools = yyjson_mut_arr(d);
        for (unsigned i = 0; i < SB_PROJECT_TOOL_COUNT; i++) {
            bool allow = (project->policy.tools & (UINT64_C(1) << i)) != 0;
            if (i >= 15 && i < 27)
                allow = enabled && (i <= 18 || (i <= 24 && sb_gui_bool(q, "control")) ||
                                    (i == 25 && sb_gui_bool(q, "clipboard_read")) ||
                                    (i == 26 && sb_gui_bool(q, "clipboard_write")));
            if (allow)
                yyjson_mut_arr_add_str(d, tools, sb_project_tool_name(i));
        }
        yyjson_mut_obj_remove_key(policy, "tools");
        yyjson_mut_obj_add_val(d, policy, "tools", tools);
    }
    char *definition = sb_posix_json(d, p),
         *output = project ? sb_projects_edit(old, strlen(old), definition, strlen(definition), g->selected,
                                              true, e)
                           : NULL;
    ok = output && save_registry(g, output, e);
    free(output);
    free(definition);
    free(old);
    sb_projects_free(&parsed);
    yyjson_doc_free(cfg);
    return ok;
}
static bool configure(sb_native_manager *g, yyjson_val *q, sb_error *e) {
    if (!stopped(g, e))
        return false;
    uint64_t port = yyjson_get_uint(yyjson_obj_get(q, "port"));
    const char *url = sb_gui_string(q, "url");
    if (port < 1 || port > 65535) {
        sb_fail(e, "端口必须在 1–65535 之间。");
        return false;
    }
    bool oauth = sb_gui_bool(q, "oauth"), http = sb_gui_bool(q, "http");
    if(sb_gui_bool(q,"remote_owner")&&!http){sb_fail(e,"远程所有者审批需要启用本机 HTTP 网关。");return false;}
    sb_config probe;
    sb_config_defaults(&probe);
    bool valid = !*url || sb_config_public_url(&probe, url, e);
    sb_config_free(&probe);
    if (!valid)
        return false;
    if (oauth && (!http || strncmp(url, "https://", 8))) {
        sb_fail(e, "OAuth 需要 HTTP 网关和 HTTPS 公网地址。");
        return false;
    }
    yyjson_doc *old = registry(g);
    if (!old) {
        sb_fail(e, "项目配置不可读，无法保存连接设置。");
        return false;
    }
    yyjson_mut_doc *d = yyjson_doc_mut_copy(old, NULL);
    yyjson_mut_val *root = yyjson_mut_doc_get_root(d), *gateway = yyjson_mut_obj_get(root,"gateway");
    if(!gateway)gateway=yyjson_mut_obj(d);
    yyjson_mut_obj_remove_key(gateway,"port");yyjson_mut_obj_remove_key(gateway,"public_url");
    yyjson_mut_obj_add_uint(d, gateway, "port", port);
    if (*url)
        yyjson_mut_obj_add_str(d, gateway, "public_url", url);
    yyjson_mut_obj_remove_key(gateway,"owner_token_env");
    if(sb_gui_bool(q,"remote_owner")) {
        const char *env=sb_gui_string(q,"owner_env");
        yyjson_mut_obj_add_strcpy(d,gateway,"owner_token_env",*env?env:"SHELLBOT_OWNER_TOKEN");
    }
    if(yyjson_obj_get(q,"default_project_id")){
        yyjson_mut_obj_remove_key(gateway,"default_project_id");
        if(*sb_gui_string(q,"default_project_id"))yyjson_mut_obj_add_strcpy(d,gateway,"default_project_id",sb_gui_string(q,"default_project_id"));
    }
    yyjson_mut_obj_remove_key(root, "gateway");
    yyjson_mut_obj_add_val(d, root, "gateway", gateway);
    char *json = sb_posix_json(d, root);
    bool ok = save_registry(g, json, e);
    free(json);
    yyjson_doc_free(old);
    if (ok) {
        yyjson_doc *s=settings(g);d=yyjson_doc_mut_copy(s,NULL);root=yyjson_mut_doc_get_root(d);
        yyjson_mut_obj_remove_key(root,"http");yyjson_mut_obj_remove_key(root,"oauth");
        yyjson_mut_obj_add_bool(d, root, "http", http);
        yyjson_mut_obj_add_bool(d, root, "oauth", oauth);
        json = sb_posix_json(d, root);
        ok = save(g, "desktop.json", json, e);
        yyjson_doc_free(s);
        free(json);
    }
    return ok;
}
static void copy_doc(yyjson_mut_doc *d, yyjson_mut_val *root, const char *key, yyjson_doc *input) {
    yyjson_mut_val *v = yyjson_val_mut_copy(d, yyjson_doc_get_root(input));
    if (v)
        yyjson_mut_obj_add_val(d, root, key, v);
}
#include "native_admin.inl"
#include "profile.inl"
static char *snapshot(sb_native_manager *g) {
    yyjson_doc *cfg = registry(g), *state = exchange(g, "status", NULL, NULL), *s = settings(g),
               *oauth = NULL, *approvals = NULL, *desktop = NULL;
    sb_error startup_error={0};yyjson_doc *autostart=startup(g,"--startup-status",&startup_error);
    bool active = yyjson_obj_get(yyjson_doc_get_root(state), "manager_pid") != NULL;
    sb_error admin_error = {0}; admin_refresh(g, &admin_error);
    if (!find(cfg, "projects", "id", g->selected)) {
        yyjson_val *first = yyjson_arr_get(yyjson_obj_get(yyjson_doc_get_root(cfg), "projects"), 0);
        snprintf(g->selected, sizeof(g->selected), "%s", sb_gui_string(first, "id"));
    }
    if (active) {
        oauth = exchange(g, "oauth-status", NULL, NULL);
        sb_error e = {0};
        approvals = parse(approval_request(g, "{\"command\":\"list\"}", &e));
        desktop = parse(project_request(g, "{\"command\":\"desktop-status\"}", &e));
    }
    if (!active && !strcmp(g->approval_backend, "administrator")) {
        sb_error e = {0}; approvals = parse(approval_request(g, "{\"command\":\"list\"}", &e));
    }
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *v = yyjson_mut_obj(d);
    copy_doc(d, v, "config", cfg);
    char *registry_bytes = load(g->registry); char registry_hash[65] = {0}; sb_error hash_error = {0};
    if (registry_bytes) sb_platform_sha256(registry_bytes, strlen(registry_bytes), registry_hash, &hash_error);
    free(registry_bytes);
    yyjson_mut_obj_add_strcpy(d, v, "registry_sha256", registry_hash);
    copy_doc(d, v, "settings", s);
    copy_doc(d, v, "status", state);
    copy_doc(d, v, "oauth", oauth);
    copy_doc(d, v, "approvals", approvals);
    copy_doc(d, v, "desktop", desktop);
    copy_doc(d, v, "administrator", g->admin_state);
    yyjson_mut_obj_add_bool(d, v, "administrator_connected", g->administrator.fd >= 0);
    yyjson_mut_obj_add_bool(d, v, "administrator_closing", g->administrator.fd < 0 && sb_admin_link_closing(&g->administrator));
    yyjson_mut_obj_add_strcpy(d, v, "administrator_error", admin_error.text);
    yyjson_mut_obj_add_strcpy(d, v, "approval_backend", g->approval_backend);
    copy_doc(d, v, "startup", autostart);
    yyjson_doc *pending=pending_migration(g);copy_doc(d,v,"migration_pending",pending);yyjson_doc_free(pending);
    yyjson_doc_free(autostart);
    copy_doc(d, v, "inspected", g->inspected);
    yyjson_mut_obj_add_bool(d, v, "online", active);
    yyjson_mut_obj_add_bool(d, v, "ok", g->last_ok);
    yyjson_mut_obj_add_bool(d, v, "owned", g->child > 0);
    yyjson_mut_obj_add_strcpy(d, v, "selected", g->selected);
    yyjson_mut_obj_add_strcpy(d, v, "message", g->message);
    yyjson_mut_obj_add_strcpy(d, v, "directory", g->directory);
    yyjson_mut_obj_add_strcpy(d, v, "manager_directory", g->data);
    if (g->clipboard) {
        yyjson_mut_obj_add_strcpy(d, v, "clipboard", g->clipboard);
        sb_posix_wipe(g->clipboard, strlen(g->clipboard)); free(g->clipboard); g->clipboard = NULL;
    }
    yyjson_doc_free(cfg);
    yyjson_doc_free(s);
    yyjson_doc_free(state);
    yyjson_doc_free(oauth);
    yyjson_doc_free(approvals);
    yyjson_doc_free(desktop);
    return sb_posix_json(d, v);
}
char *sb_desktop_work(sb_native_manager *g, const char *request) {
    yyjson_doc *d = yyjson_read(request, strlen(request), 0);
    yyjson_val *q = yyjson_doc_get_root(d);
    const char *a = sb_gui_string(q, "action"), *id = sb_gui_string(q, "id"),
               *project = sb_gui_string(q, "project");
    sb_error e = {0};
    bool ok = true;
    atomic_store(&g->cancel, false);
    if (g->child && waitpid(g->child, NULL, WNOHANG) == g->child)
        g->child = 0;
    if (*project && strcmp(g->selected, project)) {
        snprintf(g->selected, sizeof(g->selected), "%s", project);
        clear_inspection(g);
    }
    if (!strcmp(a, "refresh")) {
    } else if (!strcmp(a, "start_manager"))
        ok = launch(g, &e);
    else if (!strcmp(a, "stop_manager")) {
        admin_close(g);
        ok = stop_manager(g, &e);
    }
    else if (!strcmp(a, "exit")) {
        admin_close(g);
        if (g->child)
            ok = stop_manager(g, &e);
    } else if (!strcmp(a, "add") || !strcmp(a, "edit") || !strcmp(a, "remove") || !strcmp(a, "import"))
        ok = edit(g, q, &e);
    else if (!strcmp(a, "settings"))
        ok = configure(g, q, &e);
    else if (!strcmp(a, "configure_project"))
        ok = configure_project(g, q, &e);
    else if (!strcmp(a, "migrate_application") || !strcmp(a, "migrate_project"))
        ok = migrate_profile(g, q, !strcmp(a, "migrate_application"), &e);
    else if(!strcmp(a,"recover_migration"))
        ok=recover_migration(g,q,&e);
    else if (!strcmp(a, "admin_settings")) ok = admin_save(g, q, &e);
    else if (!strcmp(a, "admin_start")) {
        if (yyjson_obj_get(q, "root_registry") || yyjson_obj_get(q, "root_state")) ok = admin_save(g, q, &e);
        if (ok) ok = admin_start(g, &e);
    }
    else if (!strcmp(a, "admin_stop")) admin_close(g);
    else if (!strcmp(a, "admin_revoke")) ok = admin_control(g, "revoke", &e);
    else if (!strcmp(a, "admin_connect_project")) ok = admin_connect_project(g, &e);
    else if (!strcmp(a, "approval_backend")) {
        const char *backend = sb_gui_string(q, "backend");
        ok = !strcmp(backend, "ordinary") || !strcmp(backend, "administrator");
        if (ok) { snprintf(g->approval_backend, sizeof(g->approval_backend), "%s", backend); clear_inspection(g); }
        else sb_fail(&e, "请选择普通或管理员审批池。");
    }
    else if (!strcmp(a, "copy_owner_token"))
        ok = owner_credential(g, &e);
    else if (!strcmp(a,"startup_install") || !strcmp(a,"startup_uninstall")) {
        yyjson_doc *r=startup(g,!strcmp(a,"startup_install")?"--startup-install":"--startup-uninstall",&e);
        ok=r&&!*sb_gui_string(yyjson_doc_get_root(r),"error");
        if(!ok&&r)sb_fail(&e,sb_gui_string(yyjson_doc_get_root(r),"error"));
        yyjson_doc_free(r);
    }
    else if (!strcmp(a, "desktop_permissions"))
        ok = desktop_permissions(g, q, &e);
    else if (!strcmp(a, "desktop-connect") || !strcmp(a, "desktop-pause") || !strcmp(a, "desktop-resume")) {
        char request[80];
        snprintf(request, sizeof(request), "{\"command\":\"%s\"}", a);
        yyjson_doc *r = parse(project_request(g, request, &e));
        ok = r && !yyjson_obj_get(yyjson_doc_get_root(r), "error");
        if (!ok && r)
            sb_fail(&e, sb_gui_string(yyjson_doc_get_root(r), "error"));
        yyjson_doc_free(r);
    } else if (!strcmp(a, "start") || !strcmp(a, "stop_project") || !strcmp(a, "pause") ||
               !strcmp(a, "resume")) {
        yyjson_doc *r = exchange(g, a, g->selected, NULL);
        yyjson_val *v = yyjson_doc_get_root(r);
        ok = r && !*sb_gui_string(v, "error");
        if (!ok)
            sb_fail(&e, r ? sb_gui_string(v, "error") : "本机后台未连接。");
        yyjson_doc_free(r);
    } else if (!strcmp(a, "inspect") || !strcmp(a, "approve") || !strcmp(a, "deny") || !strcmp(a, "revoke")) {
        yyjson_mut_doc *out = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *v = yyjson_mut_obj(out);
        yyjson_mut_obj_add_str(out, v, "command", a);
        yyjson_mut_obj_add_str(out, v, "id", id);
        yyjson_val *inspected = yyjson_doc_get_root(g->inspected);
        if (strcmp(a, "inspect")) {
            bool anchor = !strcmp(g->inspected_backend, g->approval_backend) && !strcmp(g->inspected_project, g->selected);
            if (anchor && !strcmp(g->approval_backend, "administrator")) {
                anchor = admin_refresh(g, &e) && !strcmp(g->inspected_broker, sb_gui_string(yyjson_doc_get_root(g->admin_state), "broker_id")) &&
                    g->inspected_generation == yyjson_get_uint(yyjson_obj_get(admin_selected(g), "generation"));
            }
            if (!anchor || strcmp(id, sb_gui_string(inspected, "approval_id"))) {
                sb_fail(&e, "请先查看此操作的详细内容，再作出决定。");
                ok = false;
            } else {
                const char *hash = sb_gui_string(inspected, "operation_sha256");
                if (!*hash)
                    hash = sb_gui_string(inspected, "sha256");
                yyjson_mut_obj_add_str(out, v, "expected_sha256", hash);
                if (*sb_gui_string(inspected, "host_instance_id"))
                    yyjson_mut_obj_add_str(out, v, "expected_host_instance_id",
                                           sb_gui_string(inspected, "host_instance_id"));
                if (yyjson_obj_get(inspected, "policy_revision"))
                    yyjson_mut_obj_add_uint(out, v, "expected_policy_revision",
                                            yyjson_get_uint(yyjson_obj_get(inspected, "policy_revision")));
            }
        }
        char *wire_bytes = sb_posix_json(out, v);
        yyjson_doc *reply = ok ? parse(approval_request(g, wire_bytes, &e)) : NULL;
        free(wire_bytes);
        if (ok) {
            ok = reply && !*sb_gui_string(yyjson_doc_get_root(reply), "error");
            if (!ok)
                sb_fail(&e, reply ? sb_gui_string(yyjson_doc_get_root(reply), "error") : "操作详情不可用。");
        }
        if (ok) {
            yyjson_doc_free(g->inspected);
            g->inspected = reply;
            snprintf(g->inspected_backend, sizeof(g->inspected_backend), "%s", g->approval_backend);
            snprintf(g->inspected_project, sizeof(g->inspected_project), "%s", g->selected);
            snprintf(g->inspected_broker, sizeof(g->inspected_broker), "%s", sb_gui_string(yyjson_doc_get_root(g->admin_state), "broker_id"));
            g->inspected_generation = yyjson_get_uint(yyjson_obj_get(admin_selected(g), "generation"));
        } else
            yyjson_doc_free(reply);
    } else if (!strcmp(a, "oauth-approve") || !strcmp(a, "oauth-deny") || !strcmp(a, "oauth-revoke") ||
               !strcmp(a, "oauth-remove-client")) {
        yyjson_mut_doc *out = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *v = yyjson_mut_obj(out);
        yyjson_mut_obj_add_str(out, v, "command", a);
        yyjson_mut_obj_add_str(out, v, "id", id);
        if (!strcmp(a, "oauth-approve"))
            yyjson_mut_obj_add_bool(out, v, "persistent", sb_gui_bool(q, "persistent"));
        char *bytes = sb_posix_json(out, v);
        yyjson_doc *r = parse(sb_owner_exchange(g->data, bytes, strlen(bytes), &e));
        free(bytes);
        ok = r && !*sb_gui_string(yyjson_doc_get_root(r), "error");
        if (!ok)
            sb_fail(&e, r ? sb_gui_string(yyjson_doc_get_root(r), "error") : "OAuth 后台未连接。");
        yyjson_doc_free(r);
    } else {
        ok = false;
        sb_fail(&e, "Unknown desktop action.");
    }
    if (strcmp(a, "refresh") && !(ok && (!strcmp(a,"migrate_application") || !strcmp(a,"migrate_project"))))
        snprintf(g->message, sizeof(g->message), "%s",
                 ok ? "操作已完成。" : (*e.text ? e.text : "操作未完成。"));
    g->last_ok = ok;
    yyjson_doc_free(d);
    return snapshot(g);
}
sb_native_manager *sb_desktop_open(const char *directory, const char *key, sb_error *e) {
    if (!directory || *directory != '/' || strlen(directory) > 240) {
        sb_fail(e, "Desktop requires an absolute data directory.");
        return NULL;
    }
    if (!private_dir(directory, e))
        return NULL;
    char redirected[256];
    if(!follow_location(directory,redirected,sizeof(redirected),e))return NULL;
    directory=redirected;
    sb_lease *lease = sb_platform_lease(directory, e);
    if (!lease) {
        sb_root *r = sb_platform_root(directory, e);
        if (r && sb_posix_private(r, e)) {
            int fd = openat(r->fd, "raise.request", O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK,
                            0600);
            if (fd >= 0)
                close(fd);
        }
        sb_platform_root_close(r);
        return NULL;
    }
    sb_native_manager *g = calloc(1, sizeof(*g));
    if (!g) {
        sb_platform_lease_close(lease);
        return NULL;
    }
    g->lease = lease;
    g->administrator.fd = -1;
    snprintf(g->approval_backend, sizeof(g->approval_backend), "ordinary");
    atomic_init(&g->cancel, false);
    pthread_mutex_init(&g->location_mutex,NULL);
    snprintf(g->directory, sizeof(g->directory), "%s", lease->root->path);
    snprintf(g->registry, sizeof(g->registry), "%s/projects.json", g->directory);
    snprintf(g->data, sizeof(g->data), "%s/manager", g->directory);
    if (key)
        snprintf(g->key, sizeof(g->key), "%s", key);
    if (!active_profile(g, e)) goto fail;
    char sock[SB_PATH_CAP];
    if (!sb_posix_socket_path(g->data, sock, 104, e) || !private_dir(g->data, e))
        goto fail;
    struct stat st;
    if (fstatat(lease->root->fd, "projects.json", &st, AT_SYMLINK_NOFOLLOW)) {
        if (errno != ENOENT ||
            !save_registry(g, "{\"format_version\":1,\"gateway\":{\"port\":8765},\"projects\":[]}", e))
            goto fail;
    }
    yyjson_doc *cfg = registry(g);
    if (!cfg) {
        sb_fail(e, "已有项目配置损坏或不可读，请恢复备份；没有替换原文件。");
        goto fail;
    }
    sb_project_registry validated = {0};
    char *bytes = yyjson_val_write(yyjson_doc_get_root(cfg), 0, NULL);
    bool valid = bytes && sb_projects_parse(&validated, bytes, strlen(bytes), e);
    free(bytes);
    sb_projects_free(&validated);
    if (!valid) {
        yyjson_doc_free(cfg);
        goto fail;
    }
    yyjson_val *first = yyjson_arr_get(yyjson_obj_get(yyjson_doc_get_root(cfg), "projects"), 0);
    snprintf(g->selected, sizeof(g->selected), "%s", sb_gui_string(first, "id"));
    yyjson_doc_free(cfg);
    return g;
fail:
    sb_desktop_close(g);
    return NULL;
}
const char *sb_desktop_directory(sb_native_manager *g) { return g->directory; }
void sb_desktop_cancel(sb_native_manager *g) { if (g) atomic_store(&g->cancel, true); }
bool sb_desktop_raise(sb_native_manager *g) { pthread_mutex_lock(&g->location_mutex);bool raised=!unlinkat(g->lease->root->fd,"raise.request",0);pthread_mutex_unlock(&g->location_mutex);return raised; }
void sb_desktop_close(sb_native_manager *g) {
    if (g) {
        admin_close(g);
        yyjson_doc_free(g->inspected);
        if(g->clipboard){sb_posix_wipe(g->clipboard,strlen(g->clipboard));free(g->clipboard);}
        sb_platform_lease_close(g->lease);
        pthread_mutex_destroy(&g->location_mutex);
        free(g);
    }
}
