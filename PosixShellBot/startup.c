#include "startup.h"
#include "sb_projects.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pwd.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define OUTPUT_LIMIT 16384
#define DESCRIPTION_LIMIT 32768
typedef struct {
    char bytes[DESCRIPTION_LIMIT];
    size_t n;
    bool failed;
} buffer;
typedef struct {
    const char *action, *directory, *kind, *name, *projects, *data, *desktop_data, *key, *keychain, *user;
    const char *starts[SB_MAX_PROJECTS];
    unsigned count;
    bool oauth;
    const char *expected_uid, *expected_unit, *expected_content, *expected_registry, *expected_kind;
    uid_t uid;
    char home[SB_PATH_CAP], account[64], unit[160], native[SB_PATH_CAP], domain[48];
    char *args[64];
    unsigned argc;
    buffer content;
} startup;
static void add(buffer *b, const char *s) {
    size_t n = strlen(s);
    if (n >= sizeof(b->bytes) - b->n) {
        b->failed = true;
        return;
    }
    memcpy(b->bytes + b->n, s, n + 1);
    b->n += n;
}
static bool absolute(const char *p) {
    if (!p || p[0] != '/' || !p[1] || strlen(p) >= SB_PATH_CAP)
        return false;
    const char *a = p + 1;
    for (const char *s = a;; s++) {
        if (((unsigned char)*s < 32 && *s) || (unsigned char)*s == 127)
            return false;
        if (*s == '/' || !*s) {
            size_t n = (size_t)(s - a);
            if (!n || (n == 1 && a[0] == '.') || (n == 2 && !memcmp(a, "..", 2)))
                return false;
            if (!*s)
                break;
            a = s + 1;
        }
    }
    return true;
}
/* Every component is opened without following links. Existing ancestors are
   never chmodded, replaced or removed. */
static int directory(const char *path, bool create, bool private, sb_error *e) {
    if (!absolute(path)) {
        sb_fail(e, "An absolute canonical path without control characters is required.");
        return -1;
    }
    char copy[SB_PATH_CAP];
    snprintf(copy, sizeof(copy), "%s", path + 1);
    int fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    char *save = NULL;
    for (char *p = strtok_r(copy, "/", &save); p && fd >= 0; p = strtok_r(NULL, "/", &save)) {
        int next = openat(fd, p, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0 && errno == ENOENT && create) {
            if (!mkdirat(fd, p, 0700) || errno == EEXIST)
                next = openat(fd, p, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        }
        close(fd);
        fd = next;
    }
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || st.st_uid != geteuid() || (st.st_mode & (private ? 077 : 022))) {
        if (fd >= 0)
            close(fd);
        sb_fail(e,
                "Startup directory is missing, linked, writable by others or owned by a different account.");
        return -1;
    }
    return fd;
}
static int parent(const char *path, bool create, char leaf[256], sb_error *e) {
    char p[SB_PATH_CAP];
    if (!absolute(path)) {
        sb_fail(e, "Invalid native startup path.");
        return -1;
    }
    snprintf(p, sizeof(p), "%s", path);
    char *s = strrchr(p, '/');
    if (!s || strlen(s + 1) > 255)
        return -1;
    snprintf(leaf, 256, "%s", s + 1);
    *s = 0;
    return directory(p, create, false, e);
}
/* A root launchd job opens log paths before dropping uid. Its ownership record
   and logs therefore require root-owned, non-writable ancestors as well as a
   private leaf; a root-owned leaf below a user-writable directory is unsafe. */
static bool root_state(const char *path, sb_error *e) {
    char copy[SB_PATH_CAP];
    snprintf(copy, sizeof(copy), "%s", path + 1);
    int fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    char *save = NULL;
    bool ok = fd >= 0;
    for (char *p = strtok_r(copy, "/", &save); p && ok; p = strtok_r(NULL, "/", &save)) {
        bool last = !save || !*save;
        struct stat st;
        if (fstatat(fd, p, &st, AT_SYMLINK_NOFOLLOW)) {
            if (errno == ENOENT && last)
                break;
            ok = false;
            break;
        }
        ok = S_ISDIR(st.st_mode) && st.st_uid == 0 && !(st.st_mode & 022);
        if (!ok || last)
            break;
        int next = openat(fd, p, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        ok = next >= 0;
        close(fd);
        fd = next;
    }
    if (fd >= 0)
        close(fd);
    if (!ok)
        sb_fail(e, "System startup state and log paths need root-owned, non-writable ancestors; do not place "
                   "them below a user's home or /tmp.");
    return ok;
}
static bool state_scope(startup *s, sb_root *state, sb_error *e) {
    const char *runtime = !strcmp(s->kind, "desktop") ? s->desktop_data : s->data;
    struct stat a, b;
    sb_root *r = runtime ? sb_platform_root(runtime, e) : NULL;
    bool same =
        r && !fstat(r->fd, &a) && !fstat(state->fd, &b) && a.st_dev == b.st_dev && a.st_ino == b.st_ino;
    sb_platform_root_close(r);
    e->text[0] = 0;
    if (same) {
        sb_fail(e, "Startup ownership and runtime data must use distinct directories.");
        return false;
    }
    if (!s->projects)
        return true;
    sb_project_registry projects = {0};
    if (!sb_projects_load(&projects, s->projects, e))
        return false;
    bool ok = true;
    for (size_t i = 0; ok && i < projects.project_count; i++)
        for (size_t j = 0; ok && j < projects.projects[i].directory_count; j++) {
            sb_root *root = sb_platform_root(projects.projects[i].directories[j].path, e);
            ok = root && !sb_platform_roots_overlap(state, root);
            sb_platform_root_close(root);
        }
    sb_projects_free(&projects);
    if (!ok)
        sb_fail(e,
                "Startup ownership physically overlaps an exposed project root or that root is unavailable.");
    return ok;
}
static char *read_at(int fd, const char *name, bool private, bool *missing, sb_error *e) {
    *missing = false;
    int f = openat(fd, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (f < 0) {
        if (errno == ENOENT)
            *missing = true;
        else
            sb_posix_error(e, "Open startup record");
        return NULL;
    }
    struct stat st;
    char *out = NULL;
    if (!fstat(f, &st) && S_ISREG(st.st_mode) && st.st_nlink == 1 && st.st_uid == geteuid() &&
        !(st.st_mode & (private ? 077 : 022)) && st.st_size >= 0 && st.st_size < DESCRIPTION_LIMIT) {
        out = malloc((size_t)st.st_size + 1);
        size_t at = 0;
        while (out && at < (size_t)st.st_size) {
            ssize_t n = read(f, out + at, (size_t)st.st_size - at);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0) {
                free(out);
                out = NULL;
                break;
            }
            at += (size_t)n;
        }
        if (out) {
            struct stat now;
            if (memchr(out, 0, at) || fstat(f, &now) || now.st_size != st.st_size ||
                now.st_mtime != st.st_mtime || now.st_ctime != st.st_ctime) {
                free(out);
                out = NULL;
            } else
                out[at] = 0;
        }
    }
    close(f);
    if (!out)
        sb_fail(e, "Startup file must be bounded, regular, unlinked to other files and owned by this "
                   "installer account.");
    return out;
}
static bool create_at(int fd, const char *name, const char *text, mode_t mode, sb_error *e) {
    int f = openat(fd, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode);
    if (f < 0) {
        sb_posix_error(e, "Create startup file without overwrite");
        return false;
    }
    bool ok = sb_posix_write(f, text, strlen(text), e) && !fchmod(f, mode) && !fsync(f);
    if (!ok) {
        struct stat a, b;
        if (!fstat(f, &a) && !fstatat(fd, name, &b, AT_SYMLINK_NOFOLLOW) && a.st_dev == b.st_dev &&
            a.st_ino == b.st_ino)
            unlinkat(fd, name, 0);
    }
    close(f);
    return ok && !fsync(fd);
}
#ifdef __APPLE__
static void xml(buffer *b, const char *s) {
    for (; *s; s++) {
        const char *v = *s == '&'    ? "&amp;"
                        : *s == '<'  ? "&lt;"
                        : *s == '>'  ? "&gt;"
                        : *s == '\"' ? "&quot;"
                                     : NULL;
        if (v)
            add(b, v);
        else {
            char c[2] = {*s, 0};
            add(b, c);
        }
    }
}
#else
static void quote(buffer *b, const char *s, bool desktop) {
    add(b, "\"");
    for (; *s; s++) {
        if (*s == '%') {
            add(b, "%%");
            continue;
        }
        if (desktop) {
            if (*s == '\\')
                add(b, "\\\\\\\\");
            else if (*s == '\"' || *s == '$' || *s == '`') {
                add(b, "\\\\");
                char c[2] = {*s, 0};
                add(b, c);
            } else {
                char c[2] = {*s, 0};
                add(b, c);
            }
        } else if (*s == '$')
            add(b, "$$");
        else {
            if (*s == '\\' || *s == '\"')
                add(b, "\\");
            char c[2] = {*s, 0};
            add(b, c);
        }
    }
    add(b, "\"");
}
#endif
/* Fixed native executables, argv only; no shell, ambient tokens, PATH lookup or
   unbounded native-tool output. Only the user's actual systemd bus is selected. */
static int native(char *const args[], char output[OUTPUT_LIMIT + 1], sb_error *e) {
    output[0] = 0;
    int pipes[2];
    if (pipe(pipes)) {
        sb_posix_error(e, "Create native service control pipe");
        return -1;
    }
    fcntl(pipes[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipes[1], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, pipes[1], 1);
    posix_spawn_file_actions_adddup2(&fa, pipes[1], 2);
    posix_spawn_file_actions_addclose(&fa, pipes[0]);
    posix_spawn_file_actions_addclose(&fa, pipes[1]);
    char runtime[96], bus[160];
    snprintf(runtime, sizeof(runtime), "XDG_RUNTIME_DIR=/run/user/%lu", (unsigned long)geteuid());
    snprintf(bus, sizeof(bus), "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/%lu/bus",
             (unsigned long)geteuid());
    char *env[] = {"PATH=/usr/bin:/bin:/usr/sbin:/sbin", "LC_ALL=C", runtime, bus, NULL};
    pid_t pid = 0;
    int rc = posix_spawn(&pid, args[0], &fa, NULL, args, env);
    posix_spawn_file_actions_destroy(&fa);
    close(pipes[1]);
    if (rc) {
        close(pipes[0]);
        snprintf(e->text, sizeof(e->text), "Native service command could not start: %s", strerror(rc));
        return -1;
    }
    size_t used = 0;
    bool eof = false, done = false, reaped_elsewhere = false;
    int status = 0;
    uint64_t deadline = sb_platform_millis() + 20000;
    while (sb_platform_millis() < deadline && (!done || !eof)) {
        struct pollfd p = {pipes[0], POLLIN, 0};
        if (poll(&p, 1, 50) > 0) {
            char bytes[1024];
            ssize_t n = read(pipes[0], bytes, sizeof(bytes));
            if (n > 0) {
                size_t copy = (size_t)n;
                if (copy > OUTPUT_LIMIT - used)
                    copy = OUTPUT_LIMIT - used;
                memcpy(output + used, bytes, copy);
                used += copy;
            } else if (!n)
                eof = true;
        }
        if (!done) {
            pid_t got = waitpid(pid, &status, WNOHANG);
            done = got == pid;
            if (got < 0 && errno == ECHILD) { done = true; reaped_elsewhere = true; }
        }
    }
    close(pipes[0]);
    output[used] = 0;
    if (!done) {
        pid_t owned;
        do { owned = waitpid(pid, &status, WNOHANG); } while (owned < 0 && errno == EINTR);
        if (!owned) { kill(pid, SIGKILL); while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {} }
        sb_fail(e, "Native service command timed out.");
        return -1;
    }
    if (reaped_elsewhere) { sb_fail(e, "Native service command completion could not be verified."); return -1; }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}
static int control(startup *s, const char *command, char out[OUTPUT_LIMIT + 1], sb_error *e) {
#ifdef __APPLE__
    char target[240];
    snprintf(target, sizeof(target), "%s/%s", s->domain, s->unit);
    char *args[] = {"/bin/launchctl", (char *)command, target, NULL, NULL};
    if (!strcmp(command, "bootstrap")) {
        args[2] = s->domain;
        args[3] = s->native;
    }
#else
    char *args[10] = {"/usr/bin/systemctl"};
    unsigned n = 1;
    if (strcmp(s->kind, "system-service"))
        args[n++] = "--user";
    args[n++] = (char *)command;
    if (!strcmp(command, "show"))
        args[n++] = "--property=ActiveState,SubState,MainPID,ExecMainStatus,UnitFileState";
    if (strcmp(command, "daemon-reload"))
        args[n++] = s->unit;
    args[n] = NULL;
#endif
    return native(args, out, e);
}
#ifdef __APPLE__
static bool unload(startup *s, char out[OUTPUT_LIMIT + 1], sb_error *e) {
    if (control(s, "print", out, e))
        return true;
    const char *entry = strstr(out, "\n\tpid = ");
    char *end = NULL;
    long pid = entry ? strtol(entry + 8, &end, 10) : 0;
    bool have_pid = pid > 1 && end && (*end == '\n' || *end == '\r');
    if (control(s, "bootout", out, e))
        return false;
    /* bootout can return before the old process releases its state lease.
       Never signal this numeric pid: it is only a bounded completion check. */
    if (have_pid) {
        uint64_t deadline = sb_platform_millis() + 10000;
        while (!kill((pid_t)pid, 0) || errno == EPERM) {
            if (sb_platform_millis() >= deadline) {
                sb_fail(e, "Native job was unloaded but its previous process is still exiting; inspect "
                           "status before starting again.");
                return false;
            }
            struct timespec pause = {.tv_nsec = 50000000};
            nanosleep(&pause, NULL);
        }
    }
    return true;
}
#endif
static bool paths(startup *s, bool account_lookup, sb_error *e) {
    if (!s->kind ||
        (!strcmp(s->kind, "desktop")
             ? false
             : strcmp(s->kind, "user-service") && strcmp(s->kind, "system-service")) ||
        !sb_identifier(s->name) || strlen(s->name) > 48 || !absolute(s->directory)) {
        sb_fail(e, "Use desktop, user-service or system-service; a name of 1..48 identifier characters and "
                   "an absolute startup-dir are required.");
        return false;
    }
    bool system = !strcmp(s->kind, "system-service");
    struct passwd *pw = system && account_lookup ? getpwnam(s->user ? s->user : "") : getpwuid(geteuid());
    if (!pw || (!system && pw->pw_uid == 0) || (system && account_lookup && pw->pw_uid == 0)) {
        sb_fail(
            e,
            "Startup requires an existing non-root runtime account; system-service requires --service-user.");
        return false;
    }
    if (account_lookup) {
        if (strlen(pw->pw_name) >= sizeof(s->account) || strlen(pw->pw_dir) >= sizeof(s->home)) {
            sb_fail(e, "Runtime account name or home exceeds the native startup budget.");
            return false;
        }
        s->uid = pw->pw_uid;
        snprintf(s->account, sizeof(s->account), "%s", pw->pw_name);
    }
    snprintf(s->home, sizeof(s->home), "%s", pw->pw_dir);
    const char *suffix = !strcmp(s->kind, "desktop") ? "desktop" : "backend";
#ifdef __APPLE__
    snprintf(s->unit, sizeof(s->unit), "org.shellbot.%s.%s", s->name, suffix);
    if (system) {
        snprintf(s->native, sizeof(s->native), "/Library/LaunchDaemons/%s.plist", s->unit);
        strcpy(s->domain, "system");
    } else {
        snprintf(s->native, sizeof(s->native), "%s/Library/LaunchAgents/%s.plist", s->home, s->unit);
        snprintf(s->domain, sizeof(s->domain), "gui/%lu", (unsigned long)s->uid);
    }
#else
    snprintf(s->unit, sizeof(s->unit), "org.shellbot.%s.%s.%s", s->name, suffix,
             !strcmp(s->kind, "desktop") ? "desktop" : "service");
    if (system)
        snprintf(s->native, sizeof(s->native), "/etc/systemd/system/%s", s->unit);
    else {
        const char *xdg = getenv("XDG_CONFIG_HOME");
        char base[SB_PATH_CAP];
        if (xdg && *xdg) {
            if (!absolute(xdg)) {
                sb_fail(e, "XDG_CONFIG_HOME must be canonical and absolute.");
                return false;
            }
            snprintf(base, sizeof(base), "%s", xdg);
        } else {
            size_t n = strlen(s->home);
            if (n + 9 > sizeof(base))
                return false;
            memcpy(base, s->home, n);
            memcpy(base + n, "/.config", 9);
        }
        int n = snprintf(s->native, sizeof(s->native), "%s/%s/%s", base,
                         !strcmp(s->kind, "desktop") ? "autostart" : "systemd/user", s->unit);
        if (n < 0 || (size_t)n >= sizeof(s->native))
            return false;
    }
    strcpy(s->domain, system ? "system" : "user");
#endif
    return absolute(s->native);
}
static bool checked_file(const char *path, uid_t uid, bool key, sb_error *e) {
    char leaf[256];
    char copy[SB_PATH_CAP];
    if (!absolute(path))
        return false;
    snprintf(copy, sizeof(copy), "%s", path);
    char *slash = strrchr(copy, '/');
    if (strlen(slash + 1) >= sizeof(leaf)) {
        sb_fail(e, "File name exceeds native component limit.");
        return false;
    }
    snprintf(leaf, sizeof(leaf), "%s", slash + 1);
    *slash = 0;
    sb_root *root = sb_platform_root(copy, e);
    if (!root)
        return false;
    int fd = openat(root->fd, leaf, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    struct stat st, ps;
    bool ok = fd >= 0 && !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_nlink == 1 &&
              (st.st_uid == uid || (!key && st.st_uid == 0)) && !(st.st_mode & (key ? 077 : 022));
    if (key)
        ok = ok && st.st_size == 64 && !fstat(root->fd, &ps) && ps.st_uid == uid && !(ps.st_mode & 077);
    if (fd >= 0)
        close(fd);
    sb_platform_root_close(root);
    if (!ok)
        sb_fail(e, "Registry/key must be a safe regular file owned by its runtime account; keys require 64 "
                   "bytes and a private parent.");
    return ok;
}
static bool build(startup *s, sb_error *e) {
    if (!paths(s, true, e))
        return false;
    if (!strcmp(s->kind, "system-service") && !root_state(s->directory, e))
        return false;
    if (s->user && strcmp(s->kind, "system-service")) {
        sb_fail(e, "service-user is only valid for a system service.");
        return false;
    }
#ifndef __APPLE__
    if (s->keychain) {
        sb_fail(e, "Keychain selection is available on macOS only.");
        return false;
    }
#endif
    if (!strcmp(s->kind, "desktop")) {
#ifndef SB_NATIVE_DESKTOP
        sb_fail(e, "This build has no native management interface.");
        return false;
#endif
        if (!absolute(s->desktop_data) || s->projects || s->data || s->count || s->oauth || s->user ||
            s->keychain) {
            sb_fail(e, "Desktop startup requires only desktop-data-dir and an optional credential-key-file.");
            return false;
        }
    } else {
        if (!absolute(s->projects) || !absolute(s->data) || s->desktop_data ||
            (!strcmp(s->kind, "system-service") && !s->key)) {
            sb_fail(e, "Services require projects and manager-data-dir; system services additionally require "
                       "an explicit private credential-key-file.");
            return false;
        }
        if (!checked_file(s->projects, s->uid, false, e))
            return false;
        sb_project_registry r = {0};
        if (!sb_projects_load(&r, s->projects, e))
            return false;
        bool ok = !s->oauth || *r.public_origin;
        if (!ok)
            sb_fail(e, "OAuth services require a configured HTTPS public_url.");
        for (unsigned i = 0; i < s->count; i++) {
            const sb_project *p = sb_project_find(&r, s->starts[i]);
            if (!p || !p->enabled || p->legacy_compatibility) {
                sb_fail(e, "Startup projects must be enabled native projects; command identities are "
                           "verified by their workers.");
                ok = false;
                break;
            }
            for (unsigned j = 0; j < i; j++)
                if (!strcmp(s->starts[j], s->starts[i]))
                    ok = false;
        }
        for (size_t i = 0; ok && i < r.project_count; i++)
            for (size_t j = 0; j < r.projects[i].directory_count; j++) {
                const char *root = r.projects[i].directories[j].path;
                size_t n = strlen(root), dn = strlen(s->directory);
                if ((!strncmp(s->directory, root, n) && (s->directory[n] == '/' || !s->directory[n])) ||
                    (!strncmp(root, s->directory, dn) && (root[dn] == '/' || !root[dn]))) {
                    sb_fail(e, "Startup ownership directory must not overlap an exposed project directory.");
                    ok = false;
                }
            }
        sb_projects_free(&r);
        if (!ok)
            return false;
    }
    if (s->key && !checked_file(s->key, s->uid, true, e))
        return false;
    if (s->keychain && (!absolute(s->keychain) || s->key || !strcmp(s->kind, "system-service"))) {
        sb_fail(e, "A keychain is user-service only and cannot be combined with a key file.");
        return false;
    }
    s->args[s->argc++] = (char *)sb_posix_program();
    if (!strcmp(s->kind, "desktop")) {
        s->args[s->argc++] = "--desktop";
        s->args[s->argc++] = "--data-dir";
        s->args[s->argc++] = (char *)s->desktop_data;
    } else {
        s->args[s->argc++] = "--manager";
        s->args[s->argc++] = "--http";
        s->args[s->argc++] = "--credential-store";
        s->args[s->argc++] = "--projects";
        s->args[s->argc++] = (char *)s->projects;
        s->args[s->argc++] = "--manager-data-dir";
        s->args[s->argc++] = (char *)s->data;
        if (s->oauth)
            s->args[s->argc++] = "--oauth";
        for (unsigned i = 0; i < s->count; i++) {
            s->args[s->argc++] = "--start-project";
            s->args[s->argc++] = (char *)s->starts[i];
        }
    }
    if (s->key) {
        s->args[s->argc++] = "--credential-key-file";
        s->args[s->argc++] = (char *)s->key;
    }
    if (s->keychain) {
        s->args[s->argc++] = "--credential-keychain";
        s->args[s->argc++] = (char *)s->keychain;
    }
    buffer *b = &s->content;
#ifdef __APPLE__
    add(b,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\"><dict><key>Label</key><string>");
    xml(b, s->unit);
    add(b, "</string><key>ProgramArguments</key><array>");
    for (unsigned i = 0; i < s->argc; i++) {
        add(b, "<string>");
        xml(b, s->args[i]);
        add(b, "</string>");
    }
    add(b, "</array><key>RunAtLoad</key><true/><key>Umask</key><integer>63</integer><key>ThrottleInterval</"
           "key><integer>15</integer>");
    if (!strcmp(s->kind, "system-service")) {
        add(b, "<key>UserName</key><string>");
        xml(b, s->account);
        add(b, "</string>");
    }
    if (!strcmp(s->kind, "desktop"))
        add(b, "<key>LimitLoadToSessionType</key><string>Aqua</string><key>ProcessType</"
               "key><string>Interactive</string>");
    else {
        add(b, "<key>KeepAlive</key><dict><key>SuccessfulExit</key><false/></dict><key>ExitTimeOut</"
               "key><integer>30</integer><key>ProcessType</key><string>Background</string>");
        add(b, "<key>StandardOutPath</key><string>");
        xml(b, s->directory);
        add(b, "/stdout.log</string><key>StandardErrorPath</key><string>");
        xml(b, s->directory);
        add(b, "/stderr.log</string>");
    }
    add(b, "<key>EnvironmentVariables</key><dict><key>HOME</key><string>");
    xml(b, s->home);
    add(b, "</string><key>PATH</key><string>/usr/local/bin:/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin</"
           "string></dict></dict></plist>\n");
#else
    if (!strcmp(s->kind, "desktop")) {
        add(b, "[Desktop Entry]\nType=Application\nName=ShellBot\nComment=Open ShellBot management after "
               "login; projects require explicit start\nTerminal=false\nExec=");
        for (unsigned i = 0; i < s->argc; i++) {
            if (i)
                add(b, " ");
            quote(b, s->args[i], true);
        }
        add(b, "\nX-GNOME-Autostart-enabled=true\n");
    } else {
        add(b,
            "[Unit]\nDescription=ShellBot project "
            "manager\nStartLimitIntervalSec=120\nStartLimitBurst=3\n[Service]\nType=exec\nUMask="
            "0077\nRestart=on-failure\nRestartSec=15\nTimeoutStopSec=30\nKillMode=control-group\nExecStart=");
        for (unsigned i = 0; i < s->argc; i++) {
            if (i)
                add(b, " ");
            quote(b, s->args[i], false);
        }
        add(b, "\n");
        if (!strcmp(s->kind, "system-service")) {
            add(b, "User=");
            add(b, s->account);
            add(b, "\n");
        }
        add(b, "[Install]\nWantedBy=");
        add(b, !strcmp(s->kind, "system-service") ? "multi-user.target\n" : "default.target\n");
    }
#endif
    if (b->failed) {
        sb_fail(e, "Startup description exceeds its size budget.");
        return false;
    }
    return true;
}
static yyjson_mut_val *description(startup *s, yyjson_mut_doc *d) {
    yyjson_mut_val *v = yyjson_mut_obj(d), *args = yyjson_mut_arr(d);
    for (unsigned i = 0; i < s->argc; i++)
        yyjson_mut_arr_add_strcpy(d, args, s->args[i]);
    char sha[65];
    sb_error e = {0};
    sb_platform_sha256(s->content.bytes, s->content.n, sha, &e);
    yyjson_mut_obj_add_uint(d, v, "format_version", 1);
    yyjson_mut_obj_add_strcpy(d, v, "kind", s->kind);
    yyjson_mut_obj_add_strcpy(d, v, "name", s->name);
    yyjson_mut_obj_add_strcpy(d, v, "native_path", s->native);
    yyjson_mut_obj_add_strcpy(d, v, "unit_id", s->unit);
    yyjson_mut_obj_add_strcpy(d, v, "domain", s->domain);
    yyjson_mut_obj_add_strcpy(d, v, "content_sha256", sha);
    yyjson_mut_obj_add_strcpy(d, v, "contents", s->content.bytes);
    yyjson_mut_obj_add_val(d, v, "argv", args);
    yyjson_mut_obj_add_uint(d, v, "runtime_uid", s->uid);
    yyjson_mut_obj_add_bool(d, v, "requires_root", !strcmp(s->kind, "system-service"));
    yyjson_mut_obj_add_bool(d, v, "starts_now", false);
    yyjson_mut_obj_add_bool(d, v, "replays_tasks", false);
    yyjson_mut_obj_add_str(d, v, "activation",
                           !strcmp(s->kind, "system-service") ? "system boot"
                           : !strcmp(s->kind, "desktop")
                               ? "graphical login"
                               : "user login (Linux boot needs owner-managed linger)");
    return v;
}
static const char *str(yyjson_val *v, const char *key) {
    const char *s = yyjson_get_str(yyjson_obj_get(v, key));
    return s ? s : "";
}
static bool review(startup *s, yyjson_val *record, sb_error *e) {
    if ((s->expected_uid && (!*s->expected_uid || strspn(s->expected_uid, "0123456789") != strlen(s->expected_uid) ||
          strtoull(s->expected_uid, NULL, 10) != (uint64_t)s->uid)) ||
        (s->expected_unit && strcmp(s->expected_unit, s->unit)) ||
        (s->expected_kind && strcmp(s->expected_kind, s->kind)) ||
        (s->expected_content && (strlen(s->expected_content) != 64 ||
          strspn(s->expected_content, "0123456789abcdef") != 64))) {
        sb_fail(e, "Startup review does not match this runtime account or native unit; nothing changed.");
        return false;
    }
    if (s->expected_content) {
        char sha[65];
        const char *contents = record ? str(record, "contents") : s->content.bytes;
        if (!sb_platform_sha256(contents, strlen(contents), sha, e) || strcmp(sha, s->expected_content)) {
            sb_fail(e, "Startup contents changed since review; inspect the current plan or receipt first.");
            return false;
        }
    }
    if (s->expected_registry) {
        size_t size = 0; char sha[65];
        char *bytes = s->projects ? sb_platform_load_config(s->projects, &size, e) : NULL;
        bool ok = bytes && strlen(s->expected_registry) == 64 &&
                  strspn(s->expected_registry, "0123456789abcdef") == 64 &&
                  sb_platform_sha256(bytes, size, sha, e) && !strcmp(sha, s->expected_registry);
        free(bytes);
        if (!ok) { sb_fail(e, "Project registry changed since startup review; nothing changed."); return false; }
    }
    return true;
}
static bool load_record(startup *s, yyjson_val *v, sb_error *e) {
    s->kind = str(v, "kind");
    s->name = str(v, "name");
    s->uid = (uid_t)yyjson_get_uint(yyjson_obj_get(v, "runtime_uid"));
    if (yyjson_get_uint(yyjson_obj_get(v, "format_version")) != 1 ||
        (strcmp(s->kind, "system-service") && s->uid != geteuid()) || !paths(s, false, e) ||
        strcmp(s->native, str(v, "native_path")) || strcmp(s->unit, str(v, "unit_id")) ||
        strcmp(s->domain, str(v, "domain"))) {
        sb_fail(e, "Startup receipt does not match this account, platform or native path; nothing changed.");
        return false;
    }
    const char *bytes = str(v, "contents");
    char hash[65];
    return *bytes && sb_platform_sha256(bytes, strlen(bytes), hash, e) &&
           !strcmp(hash, str(v, "content_sha256"));
}
static bool matching(startup *s, yyjson_val *record, int *fd, char leaf[256], bool *missing, sb_error *e) {
    *fd = parent(s->native, false, leaf, e);
    if (*fd < 0)
        return false;
    char *content = read_at(*fd, leaf, false, missing, e);
    if (!content)
        return *missing;
    char hash[65];
    bool ok =
        sb_platform_sha256(content, strlen(content), hash, e) && !strcmp(hash, str(record, "content_sha256"));
    free(content);
    if (!ok)
        sb_fail(e, "Native startup file was changed; refusing to replace, stop or remove an unrelated "
                   "registration.");
    return ok;
}
bool sb_startup_option(const char *s) {
    return s && (!strcmp(s, "--startup-plan") || !strcmp(s, "--startup-install") ||
                 !strcmp(s, "--startup-status") || !strcmp(s, "--startup-start") ||
                 !strcmp(s, "--startup-stop") || !strcmp(s, "--startup-uninstall"));
}
int sb_startup_cli(int argc, char **argv) {
    startup s = {0};
    sb_error e = {0};
    int result = 2;
    sb_lease *lease = NULL;
    yyjson_doc *saved = NULL;
    yyjson_mut_doc *out = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *reply = NULL;
    int fd = -1;
    char leaf[256], native_out[OUTPUT_LIMIT + 1];
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (sb_startup_option(arg)) {
            if (s.action)
                goto bad;
            s.action = arg + 10;
        } else if (!strcmp(arg, "--oauth")) {
            if (s.oauth)
                goto bad;
            s.oauth = true;
        } else if (!strcmp(arg, "--start-project")) {
            if (i + 1 >= argc || s.count == SB_MAX_PROJECTS)
                goto bad;
            s.starts[s.count++] = argv[++i];
        } else {
            const char **slot = !strcmp(arg, "--startup-dir")           ? &s.directory
                                : !strcmp(arg, "--kind")                ? &s.kind
                                : !strcmp(arg, "--name")                ? &s.name
                                : !strcmp(arg, "--projects")            ? &s.projects
                                : !strcmp(arg, "--manager-data-dir")    ? &s.data
                                : !strcmp(arg, "--desktop-data-dir")    ? &s.desktop_data
                                : !strcmp(arg, "--credential-key-file") ? &s.key
                                : !strcmp(arg, "--credential-keychain") ? &s.keychain
                                : !strcmp(arg, "--service-user")        ? &s.user
                                : !strcmp(arg, "--expected-runtime-uid") ? &s.expected_uid
                                : !strcmp(arg, "--expected-unit-id") ? &s.expected_unit
                                : !strcmp(arg, "--expected-content-sha256") ? &s.expected_content
                                : !strcmp(arg, "--expected-registry-sha256") ? &s.expected_registry
                                : !strcmp(arg, "--expected-kind") ? &s.expected_kind
                                                                        : NULL;
            if (!slot || *slot || i + 1 >= argc)
                goto bad;
            *slot = argv[++i];
        }
    }
    if (!s.action || !absolute(s.directory))
        goto bad;
    if (geteuid() == 0 && strcmp(s.action, "plan") && !root_state(s.directory, &e))
        goto done;
    if (!strcmp(s.action, "plan") || !strcmp(s.action, "install")) {
        if (!build(&s, &e))
            goto done;
        if (!review(&s, NULL, &e))
            goto done;
        reply = description(&s, out);
        if (!strcmp(s.action, "plan")) {
            result = 0;
            goto done;
        }
        if (!strcmp(s.kind, "system-service") && geteuid() != 0) {
            sb_fail(&e, "System-service installation requires running this exact installer with sudo; the "
                        "backend runs as --service-user, never root.");
            goto done;
        }
        lease = sb_platform_lease(s.directory, &e);
        if (!lease)
            goto done;
        if (!state_scope(&s, lease->root, &e))
            goto done;
        if (!review(&s, NULL, &e))
            goto done;
        bool absent;
        char *old = read_at(lease->root->fd, "startup.json", true, &absent, &e);
        if (old) {
            saved = yyjson_read(old, strlen(old), 0);
            free(old);
            char *want = yyjson_mut_val_write(reply, 0, NULL),
                 *have = saved ? yyjson_val_write(yyjson_doc_get_root(saved), 0, NULL) : NULL;
            bool same = want && have && !strcmp(want, have);
            free(want);
            free(have);
            if (!same) {
                sb_fail(&e,
                        "Startup directory already records a different installation; uninstall it first.");
                goto done;
            }
        } else if (!absent)
            goto done;
        fd = parent(s.native, true, leaf, &e);
        if (fd < 0)
            goto done;
        bool missing;
        char *existing = read_at(fd, leaf, false, &missing, &e);
        if (existing) {
            bool same = saved && !strcmp(existing, s.content.bytes);
            free(existing);
            if (!same) {
                sb_fail(&e, "Native startup destination already exists; no file was overwritten.");
                goto done;
            }
        } else if (!missing)
            goto done;
        /* Write recoverable ownership receipt before registering anything. */
        if (!saved) {
            char *wire = yyjson_mut_val_write(reply, 0, NULL);
            bool ok = wire && strlen(wire) < DESCRIPTION_LIMIT &&
                      create_at(lease->root->fd, "startup.json", wire, 0600, &e);
            free(wire);
            if (!ok)
                goto done;
        }
        if (missing && !create_at(fd, leaf, s.content.bytes, 0644, &e))
            goto done;
#ifdef __APPLE__
        if (strcmp(s.kind, "desktop"))
            for (unsigned i = 0; i < 2; i++) {
                const char *name = i ? "stderr.log" : "stdout.log";
                int log = openat(lease->root->fd, name,
                                 O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
                struct stat st;
                bool ok = log >= 0 && !fstat(log, &st) && S_ISREG(st.st_mode) && st.st_nlink == 1 &&
                          st.st_uid == geteuid() && !(st.st_mode & 077);
                if (log >= 0)
                    close(log);
                if (!ok) {
                    sb_fail(&e, "Unsafe existing service log; registration retained for recovery.");
                    goto done;
                }
            }
#else
        if (strcmp(s.kind, "desktop") &&
            (control(&s, "daemon-reload", native_out, &e) || control(&s, "enable", native_out, &e))) {
            if (!*e.text)
                snprintf(e.text, sizeof(e.text),
                         "Native service registration failed: %.150s. Ownership receipt retained for "
                         "uninstall/retry.",
                         native_out);
            goto done;
        }
#endif
        yyjson_mut_obj_add_bool(out, reply, "installed", true);
        yyjson_mut_obj_add_bool(out, reply, "registered", true);
        result = 0;
        goto done;
    }
    if (s.kind || s.name || s.projects || s.data || s.desktop_data || s.key || s.keychain || s.user ||
        s.count || s.oauth || s.expected_registry)
        goto bad;
    struct stat state;
    if (lstat(s.directory, &state) && errno == ENOENT) {
        if (strcmp(s.action, "status") && strcmp(s.action, "uninstall")) {
            sb_fail(&e, "Startup is not installed.");
            goto done;
        }
        reply = yyjson_mut_obj(out);
        yyjson_mut_obj_add_bool(out, reply, "installed", false);
        yyjson_mut_obj_add_bool(out, reply, "registered", false);
        yyjson_mut_obj_add_bool(out, reply, "status_known", true);
        result = 0;
        goto done;
    }
    /* Read the ownership record and its matching native definition under the
       same lease used by install/uninstall, including explicit status. */
    lease = sb_platform_lease(s.directory, &e);
    if (!lease)
        goto done;
    fd = directory(s.directory, false, true, &e);
    if (fd < 0)
        goto done;
    bool absent;
    char *text = read_at(fd, "startup.json", true, &absent, &e);
    close(fd);
    fd = -1;
    if (!text) {
        if (absent && (!strcmp(s.action, "status") || !strcmp(s.action, "uninstall"))) {
            reply = yyjson_mut_obj(out);
            yyjson_mut_obj_add_bool(out, reply, "installed", false);
            yyjson_mut_obj_add_bool(out, reply, "registered", false);
            yyjson_mut_obj_add_bool(out, reply, "status_known", true);
            result = 0;
        }
        goto done;
    }
    saved = yyjson_read(text, strlen(text), 0);
    free(text);
    yyjson_val *record = yyjson_doc_get_root(saved);
    if (!saved || !load_record(&s, record, &e))
        goto done;
    if (!review(&s, record, &e))
        goto done;
    if (!strcmp(s.kind, "system-service") && geteuid() != 0) {
        sb_fail(&e, "System startup records and controls require the administrator installer account.");
        goto done;
    }
    bool missing;
    if (!matching(&s, record, &fd, leaf, &missing, &e))
        goto done;
    reply = yyjson_mut_obj(out);
    yyjson_mut_obj_add_bool(out, reply, "installed", !missing);
    yyjson_mut_obj_add_bool(out, reply, "registered", true);
    yyjson_mut_obj_add_strcpy(out, reply, "native_path", s.native);
    yyjson_mut_obj_add_strcpy(out, reply, "unit_id", s.unit);
    yyjson_mut_obj_add_strcpy(out, reply, "kind", s.kind);
    yyjson_mut_obj_add_strcpy(out, reply, "name", s.name);
    yyjson_mut_obj_add_strcpy(out, reply, "content_sha256", str(record, "content_sha256"));
    yyjson_mut_obj_add_uint(out, reply, "runtime_uid", s.uid);
    yyjson_mut_obj_add_val(out, reply, "argv", yyjson_val_mut_copy(out, yyjson_obj_get(record, "argv")));
    yyjson_mut_obj_add_bool(out, reply, "replays_tasks", false);
    if (!strcmp(s.action, "status")) {
        bool active = false, known = true; unsigned long process = 0;
        if (strcmp(s.kind, "desktop")) {
#ifdef __APPLE__
            int code = control(&s, "print", native_out, &e);
            active = !code && strstr(native_out, "state = running");
            known = code >= 0 && (!code || strstr(native_out, "Could not find service"));
            const char *entry = !code ? strstr(native_out, "\n\tpid = ") : NULL;
            char *end = NULL;
            unsigned long actual = entry ? strtoul(entry + 8, &end, 10) : 0;
            if (actual > 1 && actual <= INT_MAX && end && (*end == '\n' || *end == '\r')) process = actual;
#else
            int code = control(&s, "show", native_out, &e);
            active = !code && strstr(native_out, "ActiveState=active");
            known = !code;
            const char *entry = !code ? strstr(native_out, "MainPID=") : NULL;
            char *end = NULL;
            unsigned long actual = entry ? strtoul(entry + 8, &end, 10) : 0;
            if (actual > 1 && actual <= INT_MAX && end && (!*end || *end == '\n' || *end == '\r')) process = actual;
#endif
            yyjson_mut_obj_add_strcpy(out, reply, "native_state", native_out);
            if (!known && *e.text) yyjson_mut_obj_add_strcpy(out, reply, "status_error", e.text);
        }
        yyjson_mut_obj_add_bool(out, reply, "active", active);
        yyjson_mut_obj_add_bool(out, reply, "status_known", known);
        yyjson_mut_obj_add_uint(out, reply, "process_id", process);
        result = 0;
    } else if (!strcmp(s.action, "start") || !strcmp(s.action, "stop")) {
        if (missing || !strcmp(s.kind, "desktop")) {
            sb_fail(&e, "Start/stop controls require an installed backend service. Desktop autostart takes "
                        "effect at graphical login; use the management window to quit.");
            goto done;
        }
#ifdef __APPLE__
        int exists = control(&s, "print", native_out, &e);
        int code = !strcmp(s.action, "start") ? (exists ? control(&s, "bootstrap", native_out, &e)
                                                        : control(&s, "kickstart", native_out, &e))
                                              : (exists                       ? 0
                                                 : unload(&s, native_out, &e) ? 0
                                                                              : 1);
#else
        if (!strcmp(s.action, "start")) {
            /* An unloaded healthy unit has no failure state to reset. The
               subsequent start remains authoritative and reports bus errors. */
            sb_error ignored = {0};
            control(&s, "reset-failed", native_out, &ignored);
        }
        int code = control(&s, s.action, native_out, &e);
#endif
        if (code) {
            if (!*e.text)
                snprintf(e.text, sizeof(e.text), "Native service action failed: %.150s", native_out);
            goto done;
        }
        result = 0;
    } else if (!strcmp(s.action, "uninstall")) {
#ifndef __APPLE__
        /* A crash or manual deletion can leave a loaded unit / enable links but
           no unit file. Restore only our recorded bytes with exclusive create
           so systemd can stop and disable it before removing the registration. */
        if (missing && strcmp(s.kind, "desktop")) {
            if (!create_at(fd, leaf, str(record, "contents"), 0644, &e))
                goto done;
            missing = false;
        }
#endif
        if (strcmp(s.kind, "desktop")) {
#ifdef __APPLE__
            if (!unload(&s, native_out, &e)) {
                sb_fail(&e, "Native service could not be unloaded; registration retained.");
                goto done;
            }
#else
            if (!missing) {
                if (control(&s, "stop", native_out, &e)) {
                    sb_fail(&e, "Native service could not be stopped; registration retained.");
                    goto done;
                }
                sb_error ignored = {0};
                control(&s, "reset-failed", native_out, &ignored);
                if (control(&s, "disable", native_out, &e)) {
                    sb_fail(&e, "Native service could not be disabled; registration retained.");
                    goto done;
                }
            }
#endif
        }
        if (!missing) {
            bool gone;
            char *latest = read_at(fd, leaf, false, &gone, &e);
            char hash[65];
            bool same = latest && sb_platform_sha256(latest, strlen(latest), hash, &e) &&
                        !strcmp(hash, str(record, "content_sha256"));
            free(latest);
            if (!same) {
                sb_fail(&e, "Startup file changed during native shutdown; registration retained.");
                goto done;
            }
        }
        if (!missing && unlinkat(fd, leaf, 0)) {
            sb_posix_error(&e, "Remove owned startup file");
            goto done;
        }
        if (fsync(fd)) {
            sb_posix_error(&e, "Flush startup removal");
            goto done;
        }
#ifndef __APPLE__
        if (strcmp(s.kind, "desktop") && control(&s, "daemon-reload", native_out, &e)) {
            sb_fail(&e, "Registration removed but native daemon reload failed; receipt retained.");
            goto done;
        }
#endif
        unsigned char bytes[8];
        char name[64] = "uninstalled-";
        if (!sb_platform_random(bytes, sizeof(bytes), &e))
            goto done;
        for (unsigned i = 0; i < 8; i++)
            snprintf(name + 12 + 2 * i, 3, "%02x", bytes[i]);
        strcat(name, ".json");
        char *archive = yyjson_val_write(record, 0, NULL);
        bool retained = archive && create_at(lease->root->fd, name, archive, 0600, &e);
        free(archive);
        if (!retained || unlinkat(lease->root->fd, "startup.json", 0)) {
            if (!*e.text)
                sb_posix_error(&e, "Retain uninstalled startup receipt");
            goto done;
        }
        fsync(lease->root->fd);
        yyjson_mut_obj_add_bool(out, reply, "data_preserved", true);
        yyjson_mut_obj_remove_key(reply, "installed");
        yyjson_mut_obj_add_bool(out, reply, "installed", false);
        yyjson_mut_obj_remove_key(reply, "registered");
        yyjson_mut_obj_add_bool(out, reply, "registered", false);
        result = 0;
    } else
        goto bad;
    goto done;
bad:
    sb_fail(
        &e,
        "Use --startup-plan|install with --kind desktop|user-service|system-service --name ID --startup-dir "
        "ABS and explicit runtime paths; status|start|stop|uninstall require only --startup-dir.");
done:
    if (result) {
        reply = yyjson_mut_obj(out);
        yyjson_mut_obj_add_strcpy(out, reply, "error",
                                  *e.text ? e.text
                                          : "Startup operation failed; inspect the retained receipt.");
    }
    char *wire = sb_posix_json(out, reply);
    if (wire) {
        puts(wire);
        free(wire);
    }
    if (fd >= 0)
        close(fd);
    yyjson_doc_free(saved);
    sb_platform_lease_close(lease);
    return result;
}
