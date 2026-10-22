#include "desktop.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <unistd.h>

typedef struct {
    uint32_t magic;
    atomic_uint epoch, paused;
} shared_state;
typedef struct {
    char id[33];
    sb_dview view;
    uint64_t at;
    unsigned epoch;
} observation;
struct sb_desktop {
    sb_host *host;
    sb_os_desktop *os;
    pthread_mutex_t mutex;
    pthread_t thread;
    atomic_bool quit, connecting;
    bool thread_started, leased;
    int gate, mapfd;
    shared_state *shared;
    char lease[33], actor[64], error[256];
    uint64_t deadline;
    observation observations[8];
    unsigned next, input_epoch, lease_epoch;
};
static const char *s(yyjson_val *v, const char *key) {
    const char *p = sb_string(yyjson_obj_get(v, key));
    return p ? p : "";
}
static bool nonce(char out[33]) {
    unsigned char b[16];
    sb_error e = {0};
    if (!sb_platform_random(b, sizeof(b), &e))
        return false;
    for (unsigned i = 0; i < 16; i++)
        snprintf(out + 2 * i, 3, "%02x", b[i]);
    return true;
}
static bool hex(const char *p) { return p && strlen(p) == 32 && strspn(p, "0123456789abcdef") == 32; }
static char *result(yyjson_mut_doc *d, yyjson_mut_val *v, const char *error, const char *image) {
    yyjson_mut_val *root = yyjson_mut_obj(d), *content = yyjson_mut_arr(d), *entry = yyjson_mut_obj(d);
    if (error)
        yyjson_mut_obj_add_strcpy(d, v, "error", error);
    yyjson_mut_obj_add_bool(d, root, "isError", error != NULL);
    yyjson_mut_obj_add_val(d, root, "structuredContent", v);
    char *text = yyjson_mut_val_write(v, 0, NULL);
    yyjson_mut_obj_add_str(d, entry, "type", "text");
    yyjson_mut_obj_add_strcpy(d, entry, "text", text ? text : "{}");
    free(text);
    yyjson_mut_arr_add_val(content, entry);
    if (image) {
        entry = yyjson_mut_obj(d);
        yyjson_mut_obj_add_str(d, entry, "type", "image");
        yyjson_mut_obj_add_str(d, entry, "mimeType", "image/png");
        yyjson_mut_obj_add_strcpy(d, entry, "data", image);
        yyjson_mut_arr_add_val(content, entry);
    }
    yyjson_mut_obj_add_val(d, root, "content", content);
    return sb_posix_json(d, root);
}
static char *failure(const char *text) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    return result(d, yyjson_mut_obj(d), text, NULL);
}
static void interrupt(void *ctx) {
    sb_desktop *d = ctx;
    if (d && d->shared) {
        atomic_fetch_add(&d->shared->epoch, 1);
        atomic_store(&d->shared->paused, 1);
    }
}
/* Called only during an operation, while its owner holds mutex. The event callback only writes atomics. */
static bool input_allowed(void *ctx) {
    sb_desktop *d = ctx;
    return !atomic_load(&d->quit) && d->leased && sb_platform_millis() < d->deadline &&
           !atomic_load(&d->shared->paused) && atomic_load(&d->shared->epoch) == d->input_epoch;
}
void sb_posix_desktop_pause(sb_desktop *d) { interrupt(d); }
static void release(sb_desktop *d) {
    if (d->leased)
        flock(d->gate, LOCK_UN);
    d->leased = false;
    d->lease[0] = d->actor[0] = 0;
    d->deadline = 0;
    memset(d->observations, 0, sizeof(d->observations));
}
static void expire(sb_desktop *d) {
    if (d->leased &&
        (sb_platform_millis() >= d->deadline || d->lease_epoch != atomic_load(&d->shared->epoch)))
        release(d);
}
static void *connect_thread(void *ctx) {
    sb_desktop *d = ctx;
    sb_error e = {0};
    bool ok = sb_os_desktop_connect(d->os, &e);
    pthread_mutex_lock(&d->mutex);
    snprintf(d->error, sizeof(d->error), "%s", ok ? "" : e.text);
    pthread_mutex_unlock(&d->mutex);
    atomic_store(&d->connecting, false);
    while (!atomic_load(&d->quit)) {
        sb_os_desktop_pump(d->os);
        if (!pthread_mutex_trylock(&d->mutex)) {
            expire(d);
            if (!sb_os_desktop_ready(d->os))
                release(d);
            pthread_mutex_unlock(&d->mutex);
        }
        struct timespec t = {.tv_nsec = 10000000};
        nanosleep(&t, NULL);
    }
    return NULL;
}
static bool shared_open(sb_desktop *d, sb_error *e) {
    const char *base = getenv("XDG_RUNTIME_DIR");
    char fallback[SB_PATH_CAP], directory[SB_PATH_CAP], session[65];
    if (!base || *base != '/') {
        struct passwd *pw = getpwuid(geteuid());
        if (!pw)
            return false;
        snprintf(fallback, sizeof(fallback), "%s/.cache", pw->pw_dir);
        if (mkdir(fallback, 0700) && errno != EEXIST)
            return false;
        base = fallback;
    }
#ifdef __APPLE__
    const char *display = "aqua";
#else
    const char *display = getenv("WAYLAND_DISPLAY");
    if (!display || !*display)
        display = getenv("DISPLAY");
    if (!display)
        display = "aqua";
#endif
    if (!sb_platform_sha256(display, strlen(display), session, e))
        return false;
    if (snprintf(directory, sizeof(directory), "%s/ShellBot-desktop-%.24s", base, session) >=
        (int)sizeof(directory))
        return false;
    if (mkdir(directory, 0700) && errno != EEXIST)
        return false;
    sb_root *r = sb_platform_root(directory, e);
    if (!r || !sb_posix_private(r, e)) {
        sb_platform_root_close(r);
        return false;
    }
    d->gate = openat(r->fd, "lease.lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    bool created = false;
    d->mapfd = openat(r->fd, "state", O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (d->mapfd >= 0)
        created = true;
    else if (errno == EEXIST)
        d->mapfd = openat(r->fd, "state", O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
    sb_platform_root_close(r);
    struct stat a, b;
    if (d->gate < 0 || d->mapfd < 0 || fstat(d->gate, &a) || fstat(d->mapfd, &b) || !S_ISREG(a.st_mode) ||
        !S_ISREG(b.st_mode) || a.st_uid != geteuid() || b.st_uid != geteuid() || a.st_nlink != 1 ||
        b.st_nlink != 1 || (a.st_mode & 077) || (b.st_mode & 077))
        return false;
    if (created && ftruncate(d->mapfd, sizeof(shared_state)))
        return false;
    if (!created && b.st_size != (off_t)sizeof(shared_state))
        return false;
    d->shared = mmap(NULL, sizeof(shared_state), PROT_READ | PROT_WRITE, MAP_SHARED, d->mapfd, 0);
    if (d->shared == MAP_FAILED) {
        d->shared = NULL;
        return false;
    }
    if (created) {
        atomic_init(&d->shared->epoch, 1);
        atomic_init(&d->shared->paused, 0);
        d->shared->magic = 0x53424435;
    }
    return d->shared->magic == 0x53424435;
}
sb_desktop *sb_posix_desktop_new(sb_host *host, sb_error *e) {
    if (!host->project || !host->project->policy.desktop.enabled)
        return NULL;
    sb_desktop *d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->host = host;
    d->gate = d->mapfd = -1;
    pthread_mutex_init(&d->mutex, NULL);
    atomic_init(&d->quit, false);
    atomic_init(&d->connecting, false);
    if (!shared_open(d, e)) {
        if (!*e->text)
            sb_fail(e, "Cannot open private desktop session state.");
        sb_posix_desktop_free(d);
        return NULL;
    }
    d->os = sb_os_desktop_new(&host->project->policy.desktop, interrupt, input_allowed, d, e);
    if (!d->os) {
        sb_posix_desktop_free(d);
        return NULL;
    }
    return d;
}
void sb_posix_desktop_free(sb_desktop *d) {
    if (d) {
        atomic_store(&d->quit, true);
        if (d->os)
            sb_os_desktop_cancel(d->os);
        if (d->thread_started)
            pthread_join(d->thread, NULL);
        release(d);
        sb_os_desktop_free(d->os);
        if (d->shared)
            munmap(d->shared, sizeof(shared_state));
        if (d->gate >= 0)
            close(d->gate);
        if (d->mapfd >= 0)
            close(d->mapfd);
        pthread_mutex_destroy(&d->mutex);
        free(d);
    }
}
bool sb_desktop_available(sb_desktop *d) { return d && sb_os_desktop_ready(d->os); }
yyjson_mut_val *sb_desktop_capabilities(sb_desktop *d, yyjson_mut_doc *doc) {
    yyjson_mut_val *v = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_bool(doc, v, "available", sb_desktop_available(d));
    yyjson_mut_obj_add_val(doc, v, "system_permissions", sb_os_desktop_permissions(doc));
    if (!d)
        return v;
    pthread_mutex_lock(&d->mutex);
    expire(d);
    yyjson_mut_obj_add_str(doc, v, "provider", sb_os_desktop_provider(d->os));
    yyjson_mut_obj_add_bool(doc, v, "connecting", atomic_load(&d->connecting));
    yyjson_mut_obj_add_strcpy(doc, v, "last_failure", d->error);
    yyjson_mut_obj_add_bool(doc, v, "local_connect_required", !sb_desktop_available(d));
    yyjson_mut_obj_add_bool(doc, v, "paused", atomic_load(&d->shared->paused));
    yyjson_mut_obj_add_bool(doc, v, "leased", d->leased);
    yyjson_mut_obj_add_bool(doc, v, "human_input_pause_supported", sb_os_desktop_human_pause(d->os));
    yyjson_mut_obj_add_bool(doc, v, "foreign_injected_input_detection", false);
    bool wayland = !strncmp(sb_os_desktop_provider(d->os), "Wayland", 7);
    yyjson_mut_obj_add_bool(doc, v, "window_list_and_focus", !wayland);
    yyjson_mut_obj_add_bool(doc, v, "foreground_change_guard", !wayland);
    yyjson_mut_obj_add_bool(doc, v, "input_target_identity_guard", !wayland);
    yyjson_mut_obj_add_str(doc, v, "text_input",
                           !strcmp(sb_os_desktop_provider(d->os), "X11 / XTest")
                               ? "current_layout_direct_keys_only"
                               : "Unicode");
    yyjson_mut_obj_add_uint(doc, v, "max_lease_seconds", d->host->project->policy.desktop.lease_seconds);
    yyjson_mut_obj_add_uint(doc, v, "observation_ttl_ms", 30000);
    yyjson_mut_obj_add_bool(doc, v, "local_owner_resume_only", true);
    yyjson_mut_obj_add_bool(doc, v, "screenshots_persisted", false);
    yyjson_mut_obj_add_bool(doc, v, "automatic_replay", false);
    yyjson_mut_obj_add_bool(doc, v, "elevated_input", false);
    pthread_mutex_unlock(&d->mutex);
    return v;
}
char *sb_posix_desktop_owner(sb_host *h, const char *command) {
    sb_desktop *d = h->desktop;
    if (!d)
        return sb_strdup("{\"error\":\"Desktop is disabled for this project\"}");
    /* Emergency stop must interrupt an action even while that action holds mutex, or audit is unhealthy. */
    if (!strcmp(command, "desktop-pause"))
        interrupt(d);
    if (strcmp(command, "desktop-status") &&
        !sb_audit_operation(h->audit, "desktop_owner_intent", "local-owner", h->project->id, "desktop",
                            command, "local"))
        return sb_strdup("{\"error\":\"Desktop audit unavailable\"}");
    pthread_mutex_lock(&d->mutex);
    if (!strcmp(command, "desktop-connect")) {
        if (!d->thread_started) {
            atomic_store(&d->connecting, true);
            if (pthread_create(&d->thread, NULL, connect_thread, d)) {
                atomic_store(&d->connecting, false);
                snprintf(d->error, sizeof(d->error), "Cannot start desktop connection thread");
            } else
                d->thread_started = true;
        }
    } else if (!strcmp(command, "desktop-pause")) {
        release(d);
    } else if (!strcmp(command, "desktop-resume")) {
        atomic_fetch_add(&d->shared->epoch, 1);
        atomic_store(&d->shared->paused, 0);
    } else if (strcmp(command, "desktop-status")) {
        pthread_mutex_unlock(&d->mutex);
        return sb_strdup("{\"error\":\"Unknown desktop owner command\"}");
    }
    pthread_mutex_unlock(&d->mutex);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    return sb_posix_json(doc, sb_desktop_capabilities(d, doc));
}
static bool integer(yyjson_val *v, const char *key, int fallback, int low, int high, int *out) {
    yyjson_val *p = yyjson_obj_get(v, key);
    *out = fallback;
    if (!p)
        return true;
    if (!yyjson_is_int(p) || (yyjson_is_uint(p) && yyjson_get_uint(p) > INT_MAX))
        return false;
    int64_t n = yyjson_get_sint(p);
    if (n < low || n > high)
        return false;
    *out = (int)n;
    return true;
}
static bool arguments(sb_desktop *d, const char *name, yyjson_val *v) {
    int tool = sb_project_tool_index(name), n;
    if (!yyjson_is_obj(v) || tool < 15 || tool > 26)
        return false;
    const char *keys[20] = {"lease_id", "request_key", "approval_id"};
    size_t count = 3;
    if (tool == 15) {
        keys[count++] = "action";
        keys[count++] = "duration_seconds";
    }
    if (tool >= 19 && tool <= 24)
        keys[count++] = "screen_id";
    if (tool == 17) {
        const char *a[] = {"monitor_id", "max_dimension", "x", "y", "width", "height"};
        for (unsigned i = 0; i < 6; i++)
            keys[count++] = a[i];
    }
    if (tool == 19)
        keys[count++] = "window_id";
    if (tool >= 20 && tool <= 22) {
        keys[count++] = "x";
        keys[count++] = "y";
    }
    if (tool == 20 || tool == 21)
        keys[count++] = "button";
    if (tool == 20)
        keys[count++] = "clicks";
    if (tool == 21) {
        keys[count++] = "to_x";
        keys[count++] = "to_y";
        keys[count++] = "duration_ms";
    }
    if (tool == 22) {
        keys[count++] = "delta";
        keys[count++] = "horizontal";
    }
    if (tool == 23 || tool == 26)
        keys[count++] = "text";
    if (tool == 24)
        keys[count++] = "keys";
    if (tool == 25 || tool == 26)
        keys[count++] = "expected_sequence";
    if (!sb_config_keys(v, keys, count))
        return false;
    const char *action = s(v, "action"), *key = s(v, "request_key");
    bool cleanup = tool == 15 && (!strcmp(action, "status") || !strcmp(action, "release"));
    if (tool == 15 && strcmp(action, "acquire") && strcmp(action, "renew") && strcmp(action, "release") &&
        strcmp(action, "status"))
        return false;
    if (!cleanup &&
        (!*key || strlen(key) > 64 ||
         strspn(key, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.") != strlen(key)))
        return false;
    if ((tool != 15 || !strcmp(action, "renew") || !strcmp(action, "release")) && !hex(s(v, "lease_id")))
        return false;
    if (!integer(v, "duration_seconds", (int)d->host->project->policy.desktop.lease_seconds, 1,
                 (int)d->host->project->policy.desktop.lease_seconds, &n) ||
        !integer(v, "max_dimension", 1600, 200, 1600, &n) || !integer(v, "monitor_id", 0, 0, 15, &n) ||
        !integer(v, "clicks", 1, 1, 2, &n) || !integer(v, "duration_ms", 200, 50, 1000, &n))
        return false;
    const char *coords[] = {"x", "y", "to_x", "to_y"};
    for (unsigned i = 0; i < 4; i++)
        if (!integer(v, coords[i], 0, -32768, 32767, &n))
            return false;
    if (tool >= 20 && tool <= 22 && (!yyjson_obj_get(v, "x") || !yyjson_obj_get(v, "y")))
        return false;
    if (tool == 21 && (!yyjson_obj_get(v, "to_x") || !yyjson_obj_get(v, "to_y")))
        return false;
    if (tool >= 19 && tool <= 24 && !hex(s(v, "screen_id")))
        return false;
    if (tool == 17) {
        unsigned crop = 0;
        const char *a[] = {"x", "y", "width", "height"};
        for (unsigned i = 0; i < 4; i++)
            crop += yyjson_obj_get(v, a[i]) != NULL;
        if (crop && crop != 4)
            return false;
        if (!integer(v, "width", 0, 1, 32768, &n) || !integer(v, "height", 0, 1, 32768, &n))
            return false;
    }
    if (tool == 22 && (!integer(v, "delta", 0, -1200, 1200, &n) || !n))
        return false;
    if (yyjson_obj_get(v, "button") && strcmp(s(v, "button"), "left") && strcmp(s(v, "button"), "right") &&
        strcmp(s(v, "button"), "middle"))
        return false;
    if (yyjson_obj_get(v, "horizontal") && !yyjson_is_bool(yyjson_obj_get(v, "horizontal")))
        return false;
    if ((tool == 23 || tool == 26) && (!yyjson_is_str(yyjson_obj_get(v, "text")) ||
                                       strlen(s(v, "text")) > 4096 || (tool == 23 && !*s(v, "text"))))
        return false;
    if (tool == 24) {
        yyjson_val *a = yyjson_obj_get(v, "keys");
        size_t size = yyjson_arr_size(a);
        if (!yyjson_is_arr(a) || !size || size > 4)
            return false;
        bool ctrl = false, alt = false;
        for (size_t i = 0; i < size; i++) {
            const char *k = sb_string(yyjson_arr_get(a, i));
            if (!k || strlen(k) > 12)
                return false;
            for (size_t j = 0; j < i; j++)
                if (!strcmp(k, sb_string(yyjson_arr_get(a, j))))
                    return false;
            if (i + 1 < size && strcmp(k, "CTRL") && strcmp(k, "ALT") && strcmp(k, "SHIFT") &&
                strcmp(k, "META"))
                return false;
            ctrl |= !strcmp(k, "CTRL");
            alt |= !strcmp(k, "ALT");
        }
        const char *last = sb_string(yyjson_arr_get(a, size - 1));
        char stop[8];
        snprintf(stop, sizeof(stop), "F%u", d->host->project->policy.desktop.emergency_function_key);
        if (!strcmp(last, "CTRL") || !strcmp(last, "ALT") || !strcmp(last, "SHIFT") ||
            !strcmp(last, "META") || (ctrl && alt && (!strcmp(last, "DELETE") || !strcmp(last, stop))))
            return false;
    }
    if (tool == 25 || tool == 26) {
        uint64_t sequence;
        if (!sb_integer(yyjson_obj_get(v, "expected_sequence"), &sequence) || sequence > UINT32_MAX)
            return false;
    }
    return true;
}
static char *canonical(yyjson_val *v) {
    const char *keys[20];
    size_t i, n, count = 0;
    yyjson_val *key, *value;
    yyjson_obj_foreach(v, i, n, key, value) {
        (void)value;
        if (strcmp(yyjson_get_str(key), "approval_id"))
            keys[count++] = yyjson_get_str(key);
    }
    for (size_t a = 1; a < count; a++)
        for (size_t b = a; b && strcmp(keys[b - 1], keys[b]) > 0; b--) {
            const char *p = keys[b];
            keys[b] = keys[b - 1];
            keys[b - 1] = p;
        }
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(d);
    for (i = 0; i < count; i++)
        yyjson_mut_obj_add_val(d, root, keys[i], yyjson_val_mut_copy(d, yyjson_obj_get(v, keys[i])));
    return sb_posix_json(d, root);
}
static void rect(yyjson_mut_doc *d, yyjson_mut_val *v, const char *key, sb_drect r) {
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_obj_add_int(d, o, "x", r.x);
    yyjson_mut_obj_add_int(d, o, "y", r.y);
    yyjson_mut_obj_add_int(d, o, "width", r.width);
    yyjson_mut_obj_add_int(d, o, "height", r.height);
    yyjson_mut_obj_add_val(d, v, key, o);
}
static bool contains(sb_drect r, int x, int y) {
    return x >= r.x && y >= r.y && (int64_t)x < ((int64_t)r.x + r.width) &&
           (int64_t)y < ((int64_t)r.y + r.height);
}
static char *b64(const unsigned char *b, size_t n) {
    static const char a[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *out = malloc((n + 2) / 3 * 4 + 1);
    if (!out)
        return NULL;
    size_t j = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)b[i] << 16;
        if (i + 1 < n)
            v |= (uint32_t)b[i + 1] << 8;
        if (i + 2 < n)
            v |= b[i + 2];
        out[j++] = a[v >> 18];
        out[j++] = a[(v >> 12) & 63];
        out[j++] = i + 1 < n ? a[(v >> 6) & 63] : '=';
        out[j++] = i + 2 < n ? a[v & 63] : '=';
    }
    out[j] = 0;
    return out;
}
char *sb_desktop_request(sb_desktop *d, const char *actor, const char *name, yyjson_val *v, bool prepare) {
    if (!d || !actor || !*actor || strlen(actor) >= 64)
        return failure("desktop_not_configured");
    pthread_mutex_lock(&d->mutex);
    expire(d);
    char *wire = NULL;
    if (!arguments(d, name, v)) {
        wire = failure("invalid_desktop_arguments");
        goto done;
    }
    if (prepare) {
        char target[256], hash[65];
        sb_error e = {0};
        snprintf(target, sizeof(target), "desktop/project:%s/worker:%s/lease:%s", d->host->project->id,
                 sb_execution_instance(d->host->execution), *s(v, "lease_id") ? s(v, "lease_id") : "acquire");
        char *parameters = canonical(v);
        sb_platform_sha256(target, strlen(target), hash, &e);
        yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *o = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, o, "target", target);
        yyjson_mut_obj_add_str(doc, o, "version", hash);
        yyjson_mut_obj_add_str(doc, o, "parameters", parameters);
        wire = sb_posix_json(doc, o);
        free(parameters);
        goto done;
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *o = yyjson_mut_obj(doc);
    sb_error e = {0};
    const char *error = NULL;
    char *image = NULL;
    yyjson_mut_obj_add_str(doc, o, "project_id", d->host->project->id);
    yyjson_mut_obj_add_str(doc, o, "host_instance_id", sb_execution_instance(d->host->execution));
    const char *action = s(v, "action");
    bool owns = d->leased && !strcmp(actor, d->actor) && !strcmp(s(v, "lease_id"), d->lease);
    if (!strcmp(name, "desktop_lease")) {
        if (!strcmp(action, "status")) {
            yyjson_mut_obj_add_bool(doc, o, "leased", d->leased);
            yyjson_mut_obj_add_bool(doc, o, "owned_by_caller", d->leased && !strcmp(actor, d->actor));
            if (d->leased && !strcmp(actor, d->actor)) {
                yyjson_mut_obj_add_str(doc, o, "lease_id", d->lease);
                yyjson_mut_obj_add_uint(doc, o, "expires_in_ms", d->deadline - sb_platform_millis());
            }
        } else if (!strcmp(action, "release")) {
            if (d->leased && !owns)
                error = "desktop_lease_not_owned";
            else {
                release(d);
                yyjson_mut_obj_add_bool(doc, o, "released", true);
            }
        } else if (!sb_os_desktop_ready(d->os))
            error = "desktop_requires_local_connection_and_system_permission";
        else if (atomic_load(&d->shared->paused))
            error = "desktop_paused: local owner must resume";
        else if (!strcmp(action, "renew") && !owns)
            error = "desktop_lease_not_owned";
        else if (!strcmp(action, "acquire") && (d->leased || flock(d->gate, LOCK_EX | LOCK_NB)))
            error = "desktop_busy";
        else {
            d->leased = true;
            d->lease_epoch = atomic_load(&d->shared->epoch);
            if (!owns) {
                if (!nonce(d->lease)) {
                    release(d);
                    error = "random_source_unavailable";
                } else
                    snprintf(d->actor, sizeof(d->actor), "%s", actor);
            }
            if (!error) {
                int duration;
                integer(v, "duration_seconds", (int)d->host->project->policy.desktop.lease_seconds, 1, 120,
                        &duration);
                d->deadline = sb_platform_millis() + (unsigned)duration * 1000u;
                yyjson_mut_obj_add_str(doc, o, "lease_id", d->lease);
                yyjson_mut_obj_add_uint(doc, o, "duration_seconds", (unsigned)duration);
            }
        }
    } else if (!owns)
        error = "desktop_lease_not_owned_or_expired";
    else if (atomic_load(&d->shared->paused))
        error = "desktop_paused: local owner must resume";
    else if (!sb_os_desktop_ready(d->os))
        error = "desktop_session_closed";
    else if (!strcmp(name, "window_list") && !strncmp(sb_os_desktop_provider(d->os), "Wayland", 7))
        error = "Wayland does not expose arbitrary windows; use desktop_state or desktop_capture";
    else if (!strcmp(name, "desktop_state") || !strcmp(name, "window_list") ||
             !strcmp(name, "desktop_capture")) {
        observation *view = &d->observations[d->next++ % 8];
        memset(view, 0, sizeof(*view));
        view->epoch = atomic_load(&d->shared->epoch);
        view->at = sb_platform_millis();
        if (!nonce(view->id) || !sb_os_desktop_view(d->os, &view->view, &e))
            error = *e.text ? e.text : "desktop_observation_failed";
        else {
            yyjson_mut_obj_add_str(doc, o, "screen_id", view->id);
            yyjson_mut_obj_add_uint(doc, o, "clipboard_sequence", view->view.clipboard);
            yyjson_mut_obj_add_uint(doc, o, "observation_ttl_ms", 30000);
            yyjson_mut_obj_add_str(doc, o, "coordinate_space",
                                   !strcmp(sb_os_desktop_provider(d->os), "macOS ScreenCaptureKit")
                                       ? "macOS_screen_points"
                                       : "desktop_session_coordinates");
            yyjson_mut_val *arr = yyjson_mut_arr(doc);
            for (unsigned i = 0; i < view->view.count; i++) {
                yyjson_mut_val *m = yyjson_mut_obj(doc);
                yyjson_mut_obj_add_uint(doc, m, "monitor_id", i);
                yyjson_mut_obj_add_real(doc, m, "scale", view->view.monitors[i].scale);
                rect(doc, m, "bounds", view->view.monitors[i].bounds);
                yyjson_mut_arr_add_val(arr, m);
            }
            yyjson_mut_obj_add_val(doc, o, "monitors", arr);
            arr = yyjson_mut_arr(doc);
            for (unsigned i = 0; i < view->view.window_count; i++) {
                yyjson_mut_val *w = yyjson_mut_obj(doc);
                char id[33];
                snprintf(id, sizeof(id), "%016llx%016llx", (unsigned long long)view->view.windows[i].native,
                         (unsigned long long)view->view.windows[i].pid);
                yyjson_mut_obj_add_strcpy(doc, w, "window_id", id);
                yyjson_mut_obj_add_strcpy(doc, w, "title", view->view.windows[i].title);
                rect(doc, w, "bounds", view->view.windows[i].bounds);
                yyjson_mut_arr_add_val(arr, w);
            }
            yyjson_mut_obj_add_val(doc, o, "windows", arr);
            if (!strcmp(name, "desktop_capture")) {
                int monitor, maximum;
                integer(v, "monitor_id", 0, 0, 15, &monitor);
                integer(v, "max_dimension", 1600, 200, 1600, &maximum);
                if ((unsigned)monitor >= view->view.count)
                    error = "unknown_monitor_id";
                else {
                    sb_drect crop = view->view.monitors[monitor].bounds;
                    if (yyjson_obj_get(v, "x")) {
                        integer(v, "x", 0, -32768, 32767, &crop.x);
                        integer(v, "y", 0, -32768, 32767, &crop.y);
                        integer(v, "width", 1, 1, 32768, &crop.width);
                        integer(v, "height", 1, 1, 32768, &crop.height);
                    }
                    sb_drect bounds = view->view.monitors[monitor].bounds;
                    if (!contains(bounds, crop.x, crop.y) ||
                        !contains(bounds, crop.x + crop.width - 1, crop.y + crop.height - 1))
                        error = "capture_crop_outside_monitor";
                    else {
                        unsigned char *pixels = NULL;
                        unsigned w = 0, h = 0;
                        sb_image png = {0};
                        if (!sb_os_desktop_capture(d->os, &view->view, (unsigned)monitor, &crop,
                                                   (unsigned)maximum, &pixels, &w, &h, &e) ||
                            !sb_posix_png(pixels, w, h, &png, &e))
                            error = *e.text ? e.text : "desktop_capture_failed";
                        else if (png.size > 1024u * 1024u)
                            error = "desktop_png_budget_exceeded";
                        else if (view->epoch != atomic_load(&d->shared->epoch))
                            error = "desktop_interrupted_during_capture";
                        else {
                            image = b64(png.encoded, png.size);
                            yyjson_mut_obj_add_uint(doc, o, "width", w);
                            yyjson_mut_obj_add_uint(doc, o, "height", h);
                            yyjson_mut_obj_add_uint(doc, o, "bytes", png.size);
                            rect(doc, o, "physical_region", crop);
                            yyjson_mut_obj_add_real(doc, o, "coordinate_units_per_image_x",
                                                    crop.width / (double)w);
                            yyjson_mut_obj_add_real(doc, o, "coordinate_units_per_image_y",
                                                    crop.height / (double)h);
                        }
                        free(pixels);
                        free(png.encoded);
                    }
                }
            }
        }
    } else if (!strcmp(name, "clipboard_read") || !strcmp(name, "clipboard_write")) {
        char *text = NULL;
        uint32_t sequence = 0;
        uint64_t expected = 0;
        sb_integer(yyjson_obj_get(v, "expected_sequence"), &expected);
        if (!sb_os_desktop_clipboard(d->os, !strcmp(name, "clipboard_write"), (uint32_t)expected,
                                     s(v, "text"), &text, &sequence, &e))
            error = e.text;
        else if (text)
            yyjson_mut_obj_add_strcpy(doc, o, "text", text);
        else
            yyjson_mut_obj_add_bool(doc, o, "written", true);
        yyjson_mut_obj_add_uint(doc, o, "clipboard_sequence", sequence);
        free(text);
    } else {
        observation *view = NULL;
        for (unsigned i = 0; i < 8; i++)
            if (!strcmp(d->observations[i].id, s(v, "screen_id")))
                view = &d->observations[i];
        if (!view || sb_platform_millis() - view->at > 30000 || view->epoch != atomic_load(&d->shared->epoch))
            error = "stale_observation";
        else {
            sb_dview fresh = {0};
            if (!sb_os_desktop_view(d->os, &fresh, &e))
                error = e.text;
            else if (strcmp(fresh.layout, view->view.layout) || fresh.focus != view->view.focus ||
                     memcmp(&fresh.focus_bounds, &view->view.focus_bounds, sizeof(sb_drect)))
                error = "stale_observation: layout or foreground changed";
            else {
                bool coords = !strcmp(name, "desktop_click") || !strcmp(name, "desktop_drag") ||
                              !strcmp(name, "desktop_scroll");
                int x = 0, y = 0, tx = 0, ty = 0;
                integer(v, "x", 0, -32768, 32767, &x);
                integer(v, "y", 0, -32768, 32767, &y);
                integer(v, "to_x", x, -32768, 32767, &tx);
                integer(v, "to_y", y, -32768, 32767, &ty);
                bool inside = !coords;
                for (unsigned i = 0; i < fresh.count; i++)
                    if (contains(fresh.monitors[i].bounds, x, y) &&
                        contains(fresh.monitors[i].bounds, tx, ty))
                        inside = true;
                const sb_dwindow *window = NULL;
                if (!strcmp(name, "window_focus")) {
                    for (unsigned i = 0; i < view->view.window_count; i++) {
                        char id[33];
                        snprintf(id, sizeof(id), "%016llx%016llx",
                                 (unsigned long long)view->view.windows[i].native,
                                 (unsigned long long)view->view.windows[i].pid);
                        if (!strcmp(id, s(v, "window_id")))
                            window = &view->view.windows[i];
                    }
                    if (!window)
                        error = "stale_window_id";
                }
                d->input_epoch = view->epoch;
                unsigned sent = 0;
                if (!inside)
                    error = "coordinates_outside_observed_monitor";
                else if (!error && !sb_os_desktop_input(d->os, name, v, &fresh, window, &sent, &e))
                    error = e.text;
                yyjson_mut_obj_add_uint(doc, o, "events_inserted", sent);
                yyjson_mut_obj_add_bool(doc, o, "application_success_confirmed", false);
                memset(d->observations, 0, sizeof(d->observations));
            }
        }
    }
    if (strcmp(name, "desktop_lease") && (sb_platform_millis() >= d->deadline ||
                                          atomic_load(&d->shared->paused) || !sb_os_desktop_ready(d->os))) {
        /* Never return newly read screen/clipboard data after its grant ended. Effects may already exist. */
        free(image);
        yyjson_mut_doc_free(doc);
        expire(d);
        wire = failure("desktop_interrupted_or_expired: inspect possible effects; do not replay input");
        goto done;
    }
    yyjson_mut_obj_add_bool(doc, o, "paused", atomic_load(&d->shared->paused));
    wire = result(doc, o, error, image);
    free(image);
done:
    pthread_mutex_unlock(&d->mutex);
    return wire;
}
