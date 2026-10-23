#include "command_tree.h"
#include <sys/resource.h>
#include <sys/wait.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#include <sys/syscall.h>
#else
#include <libproc.h>
#include <sys/proc.h>
#endif

/* A separate supervisor per command retains the leader until cleanup. Linux
 * additionally adopts orphaned descendants, even across setsid/double fork.
 * Darwin has no public subreaper or recursive kqueue fork tracking: lineage is
 * sampled and retained by birth identity, not presented as kernel containment. */
enum { TREE_CAP = 32768, WIRE_MAGIC = 0x53425435, START = 1, UPDATE = 2, FINAL = 3, FAILED = 4 };
typedef struct {
    uint32_t magic, type;
    int32_t command, exit_code, failure;
    uint32_t process_peak, cleanup_complete;
    uint64_t memory_peak;
    char reason[48], error[256];
} tree_wire;
_Static_assert(sizeof(tree_wire) <= 512, "Command reports must fit the POSIX minimum PIPE_BUF.");
typedef struct {
    pid_t pid, parent, group;
    uint64_t birth, birth_extra, memory;
    bool zombie;
} process;
typedef struct { process identity; int handle; bool alive; } member;
typedef struct {
    member *members;
    process *snapshot;
    unsigned count, snapshots;
    pid_t leader;
    uint64_t memory_peak;
    unsigned process_peak;
} tree;

const char *sb_command_tree_scope(void) {
#ifdef __linux__
    return "per-command subreaper; pidfd-bound observed and adopted descendants";
#else
    return "sampled descendants retained by process birth identity; fast unobserved orphans may escape";
#endif
}
const char *sb_command_tree_memory(void) {
#ifdef __linux__
    return "sampled_descendant_resident_bytes";
#else
    return "sampled_descendant_physical_footprint";
#endif
}
static bool same(const process *a, const process *b) {
    return a->pid == b->pid && a->birth == b->birth && a->birth_extra == b->birth_extra;
}
static bool inspect(pid_t pid, process *out) {
    memset(out, 0, sizeof(*out));
    out->pid = pid;
#ifdef __linux__
    char path[64], bytes[4096];
    snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    ssize_t n = read(fd, bytes, sizeof(bytes) - 1);
    int saved = errno;
    close(fd);
    errno = saved;
    if (n <= 0 || n == (ssize_t)sizeof(bytes) - 1) return false;
    bytes[n] = 0;
    char *end = strrchr(bytes, ')'), *save = NULL;
    if (!end || end[1] != ' ') { errno = EIO; return false; }
    unsigned field = 0;
    for (char *p = strtok_r(end + 2, " ", &save); p; p = strtok_r(NULL, " ", &save), field++) {
        if (field == 0) out->zombie = *p == 'Z' || *p == 'X';
        if (field == 1) out->parent = (pid_t)strtol(p, NULL, 10);
        if (field == 2) out->group = (pid_t)strtol(p, NULL, 10);
        if (field == 19) out->birth = strtoull(p, NULL, 10);
        if (field == 21) {
            long long pages = strtoll(p, NULL, 10);
            long page = sysconf(_SC_PAGESIZE);
            if (pages < 0 || page <= 0 || (uint64_t)pages > UINT64_MAX / (unsigned long)page) {
                errno = EOVERFLOW; return false;
            }
            out->memory = (uint64_t)pages * (unsigned long)page;
            return out->birth != 0;
        }
    }
    errno = EIO; return false;
#else
    struct proc_bsdinfo b = {0};
    if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &b, sizeof(b)) != (int)sizeof(b)) return false;
    out->parent = (pid_t)b.pbi_ppid;
    out->group = (pid_t)b.pbi_pgid;
    out->birth = b.pbi_start_tvsec;
    out->birth_extra = b.pbi_start_tvusec;
    out->zombie = b.pbi_status == SZOMB || (b.pbi_flags & PROC_FLAG_INEXIT);
    return true;
#endif
}
static bool gone(void) { return errno == ESRCH || errno == ENOENT; }
static bool enumerate(tree *t) {
    t->snapshots = 0;
#ifdef __linux__
    DIR *dir = opendir("/proc");
    if (!dir) return false;
    struct dirent *entry;
    bool ok = true;
    while ((entry = readdir(dir))) {
        char *end;
        long pid = strtol(entry->d_name, &end, 10);
        if (*end || pid <= 0 || pid > INT32_MAX) continue;
        process p;
        if (!inspect((pid_t)pid, &p)) continue; /* Unrelated accounts may be hidden. */
        if (t->snapshots == TREE_CAP) { ok = false; break; }
        t->snapshot[t->snapshots++] = p;
    }
    closedir(dir);
    return ok;
#else
    pid_t *pids = calloc(TREE_CAP, sizeof(*pids));
    if (!pids) return false;
    int size = proc_listpids(PROC_ALL_PIDS, 0, pids, TREE_CAP * (int)sizeof(*pids));
    bool ok = size > 0 && size < TREE_CAP * (int)sizeof(*pids) && size % (int)sizeof(*pids) == 0;
    for (int i = 0; ok && i < size / (int)sizeof(*pids); i++) {
        process p;
        if (pids[i] > 0 && inspect(pids[i], &p)) t->snapshot[t->snapshots++] = p;
    }
    free(pids);
    return ok;
#endif
}
static member *known(tree *t, const process *p) {
    for (unsigned i = 0; i < t->count; i++)
        if (same(&t->members[i].identity, p)) return &t->members[i];
    return NULL;
}
static bool owned_parent(tree *t, const process *p);
static bool add(tree *t, const process *p) {
    if (known(t, p)) return true;
    if (t->count == TREE_CAP) return false;
    int fd = -1;
#ifdef __linux__
    fd = (int)syscall(SYS_pidfd_open, p->pid, 0);
    if (fd < 0) return gone();
    fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
    process fresh;
    if (!inspect(p->pid, &fresh) || !same(&fresh, p) || fresh.parent != p->parent || !owned_parent(t, &fresh)) {
        if (fd >= 0) close(fd);
        return true; /* Identity changed during discovery; never claim the replacement. */
    }
    t->members[t->count++] = (member){fresh, fd, !fresh.zombie};
    return true;
}
static bool owned_parent(tree *t, const process *p) {
    if (p->parent == getpid()) return true;
    for (unsigned i = 0; i < t->count; i++) {
        member *m = &t->members[i];
        if (!m->alive || p->parent != m->identity.pid) continue;
        process parent;
#ifdef __linux__
        struct pollfd pfd = {m->handle, POLLIN, 0};
        if (poll(&pfd, 1, 0) != 0) continue;
#endif
        if (inspect(p->parent, &parent) && same(&m->identity, &parent)) return true;
    }
    /* The unreaped command leader anchors its original process group. */
    return p->group == t->leader;
}
static bool refresh(tree *t, unsigned *live, uint64_t *memory) {
    *live = 0; *memory = 0;
    if (!enumerate(t)) return false;
    bool changed;
    do {
        changed = false;
        for (unsigned i = 0; i < t->snapshots; i++) {
            process *p = &t->snapshot[i];
            if (p->pid == getpid() || known(t, p) || !owned_parent(t, p)) continue;
            unsigned before = t->count;
            if (!add(t, p)) return false;
            changed |= before != t->count;
        }
    } while (changed);
    for (unsigned i = 0; i < t->count; i++) {
        member *m = &t->members[i];
        process p;
#ifdef __linux__
        struct pollfd ready = {m->handle, POLLIN, 0};
        int rc = poll(&ready, 1, 0);
        if (rc < 0 || (ready.revents & (POLLNVAL | POLLERR))) return false;
        if (rc > 0) { m->alive = false; continue; }
#endif
        if (!inspect(m->identity.pid, &p)) {
            if (gone()) { m->alive = false; continue; }
            return false;
        }
        if (!same(&m->identity, &p) || p.zombie) { m->alive = false; continue; }
        m->alive = true;
#ifndef __linux__
        struct rusage_info_v2 usage = {0};
        if (proc_pid_rusage(p.pid, RUSAGE_INFO_V2, (rusage_info_t *)&usage)) {
            if (gone()) continue;
            return false;
        }
        process after;
        if (!inspect(p.pid, &after)) { if (gone()) continue; return false; }
        if (!same(&p, &after)) { m->alive = false; continue; }
        p.memory = usage.ri_phys_footprint;
#endif
        if (UINT64_MAX - *memory < p.memory) return false;
        *memory += p.memory;
        (*live)++;
    }
    if (*memory > t->memory_peak) t->memory_peak = *memory;
    if (*live > t->process_peak) t->process_peak = *live;
    return true;
}
static bool terminate(tree *t) {
    bool ok = true;
    /* Only signal the original group while its leader is still our unreaped child. */
    siginfo_t si = {0};
    if (waitid(P_PID, (id_t)t->leader, &si, WEXITED | WNOHANG | WNOWAIT) == 0)
        if (kill(-t->leader, SIGKILL) && errno != ESRCH) ok = false;
    for (unsigned i = 0; i < t->count; i++) {
        member *m = &t->members[i];
        if (!m->alive) continue;
#ifdef __linux__
        if (syscall(SYS_pidfd_send_signal, m->handle, SIGKILL, NULL, 0) && errno != ESRCH) ok = false;
#else
        process p;
        if (!inspect(m->identity.pid, &p)) { if (!gone()) ok = false; continue; }
        if (same(&m->identity, &p) && !p.zombie && kill(p.pid, SIGKILL) && errno != ESRCH) ok = false;
#endif
    }
    return ok;
}
static void reap_descendants(tree *t) {
    for (unsigned i = 0; i < t->count; ) {
        member *m = &t->members[i];
        pid_t pid = t->members[i].identity.pid;
        if (pid == t->leader || m->alive) { i++; continue; }
        while (waitpid(pid, NULL, WNOHANG) < 0 && errno == EINTR) {}
        if (m->handle >= 0) close(m->handle);
        *m = t->members[--t->count];
    }
}
static bool adopted_children_empty(const tree *t) {
#ifdef __linux__
    /* A /proc enumeration can race reparenting between its individual reads.
     * Do not finish on that stale empty snapshot while a new adopted child is
     * already waiting for this subreaper to discover and reap it. */
    char path[96];
    snprintf(path, sizeof(path), "/proc/self/task/%ld/children", (long)getpid());
    FILE *stream = fopen(path, "r");
    if (!stream) return false;
    long pid;
    bool empty = true;
    while (fscanf(stream, "%ld", &pid) == 1)
        if (pid != t->leader) { empty = false; break; }
    if (ferror(stream)) empty = false;
    fclose(stream);
    return empty;
#else
    (void)t;
    return true;
#endif
}
static bool wire(int fd, tree_wire *message, unsigned type, tree *t) {
    message->magic = WIRE_MAGIC; message->type = type;
    if (t) { message->memory_peak = t->memory_peak; message->process_peak = t->process_peak; }
    uint64_t until = sb_platform_millis() + 1000;
    for (;;) {
        /* One writer, nonblocking writes <= PIPE_BUF: a report is all or none.
         * Cancellation uses a different pipe, so a late cancel cannot destroy
         * a final report already queued by an exited supervisor. */
        ssize_t n = write(fd, message, sizeof(*message));
        if (n == sizeof(*message)) return true;
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (type == UPDATE) return true;
            if (sb_platform_millis() >= until) return false;
            struct pollfd ready = {fd, POLLOUT, 0};
            (void)poll(&ready, 1, 20);
            continue;
        }
        return false;
    }
}
static void close_others(const sb_command_spec *s, int channel, int extra) {
    DIR *dir = opendir("/dev/fd");
    int listing = dir ? dirfd(dir) : -1;
    long maximum = dir ? 0 : sysconf(_SC_OPEN_MAX);
    struct dirent *entry;
    for (int n = 3; ; n++) {
        int fd;
        if (dir) {
            if (!(entry = readdir(dir))) break;
            char *end; long number = strtol(entry->d_name, &end, 10);
            if (*end || number < 3 || number > INT32_MAX) continue;
            fd = (int)number;
        } else { if (n >= maximum) break; fd = n; }
        if (fd == listing || fd == channel || fd == extra || fd == s->program || fd == s->cwd ||
            fd == s->input || fd == s->output || fd == s->error || sb_restricted_has_fd(s->restricted, fd)) continue;
        close(fd);
    }
    if (dir) closedir(dir);
}
static void execute(const sb_command_spec *s, int report) {
    int failure = 0;
    if (setpgid(0, 0) || dup2(s->input, 0) < 0 || dup2(s->output, 1) < 0 ||
        dup2(s->error, 2) < 0 || fchdir(s->cwd)) failure = errno;
    close_others(s, report, -1);
    if (s->input > 2) close(s->input);
    if (s->output > 2) close(s->output);
    if (s->error > 2) close(s->error);
    struct rlimit core = {0, 0}, cpu = {(rlim_t)(s->timeout_ms / 1000 + 2), (rlim_t)(s->timeout_ms / 1000 + 3)};
    if (setrlimit(RLIMIT_CORE, &core) || setrlimit(RLIMIT_CPU, &cpu)) failure = errno;
#ifdef __linux__
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) ||
        prctl(PR_SET_PDEATHSIG, SIGKILL)) failure = errno;
#endif
    signal(SIGINT, SIG_DFL); signal(SIGTERM, SIG_DFL); signal(SIGPIPE, SIG_DFL); signal(SIGCHLD, SIG_DFL);
    sigset_t empty; sigemptyset(&empty); sigprocmask(SIG_SETMASK, &empty, NULL);
    if (!failure) {
        if (s->restricted) sb_restricted_exec(s->restricted, s->argv[0], s->program, s->argv, s->env, s->memory_mb);
        else {
#ifdef __linux__
            /* Finish all allocator/libc preparation before lowering AS. The
             * forked image may contain sanitizer shadow mappings larger than
             * the command limit; execute the bound native FD without another
             * allocator/interceptor running in that inherited image. */
            struct rlimit memory = {(rlim_t)s->memory_mb * 1024u * 1024u, (rlim_t)s->memory_mb * 1024u * 1024u};
            if (!setrlimit(RLIMIT_AS, &memory))
                syscall(SYS_execveat, s->program, "", s->argv, s->env, AT_EMPTY_PATH);
#else
            execve(s->argv[0], s->argv, s->env);
#endif
        }
        failure = errno;
    }
#ifdef __linux__
    (void)syscall(SYS_write, report, &failure, sizeof(failure));
#else
    (void)write(report, &failure, sizeof(failure));
#endif
    _exit(127);
}
static int supervise(const sb_command_spec *s, int channel, int control) {
    signal(SIGCHLD, SIG_DFL);
    signal(SIGPIPE, SIG_IGN);
    sb_posix_signal = 0;
    tree_wire result = {.exit_code = -1};
    tree t = {0};
    int failed[2] = {-1, -1};
    int gate[2] = {-1, -1};
    pid_t owner = getppid();
#ifdef __linux__
    if (prctl(PR_SET_CHILD_SUBREAPER, 1) || prctl(PR_SET_DUMPABLE, 0)) goto setup_fail;
    int probe = (int)syscall(SYS_pidfd_open, getpid(), 0);
    if (probe < 0) goto setup_fail;
    close(probe);
#endif
    t.members = calloc(TREE_CAP, sizeof(*t.members));
    t.snapshot = calloc(TREE_CAP, sizeof(*t.snapshot));
    if (!t.members || !t.snapshot) goto setup_fail;
    close_others(s, channel, control);
    if (pipe(failed) || pipe(gate)) goto setup_fail;
    fcntl(failed[0], F_SETFD, FD_CLOEXEC); fcntl(failed[1], F_SETFD, FD_CLOEXEC);
    fcntl(gate[0], F_SETFD, FD_CLOEXEC); fcntl(gate[1], F_SETFD, FD_CLOEXEC);
    t.leader = fork();
    if (t.leader < 0) goto setup_fail;
    if (!t.leader) {
        close(channel); close(control); close(failed[0]); close(gate[1]);
        unsigned char permit;
        ssize_t n; do { n = read(gate[0], &permit, 1); } while (n < 0 && errno == EINTR);
        close(gate[0]);
        if (n != 1 || permit != 1) _exit(127);
        execute(s, failed[1]);
    }
    close(gate[0]); gate[0] = -1;
    close(failed[1]); failed[1] = -1;
    close(s->input); close(s->output); close(s->error);
    setpgid(t.leader, t.leader);
    process leader;
    if (!inspect(t.leader, &leader) || !add(&t, &leader)) goto child_fail;
    unsigned char permit = 1;
    if (write(gate[1], &permit, 1) != 1) goto child_fail;
    close(gate[1]); gate[1] = -1;
    struct pollfd p = {failed[0], POLLIN, 0};
    int ready;
    do { ready = poll(&p, 1, 3000); } while (ready < 0 && errno == EINTR && !sb_posix_signal);
    int exec_error = 0;
    ssize_t got = ready > 0 ? read(failed[0], &exec_error, sizeof(exec_error)) : -1;
    close(failed[0]); failed[0] = -1;
    if (got != 0) { errno = exec_error ? exec_error : ETIMEDOUT; goto child_fail; }
    result.command = t.leader;
    wire(channel, &result, START, &t);
    uint64_t started = sb_platform_millis(), stopping = 0, last_update = 0;
    unsigned failures = 0;
    bool leader_exited = false, complete = false;
    int leader_status = 0;
    for (;;) {
        uint64_t now = sb_platform_millis(), memory = 0;
        unsigned live = 0;
        bool sampled = refresh(&t, &live, &memory);
        if (!sampled) {
            if (++failures >= 3 && !*result.reason) {
                strcpy(result.reason, "resource_monitor_unavailable");
                snprintf(result.error, sizeof(result.error), "Cannot observe the complete owned descendant set.");
            }
        } else {
            failures = 0;
            if (!*result.reason && memory > (uint64_t)s->memory_mb * 1024u * 1024u) strcpy(result.reason, "memory_limit");
            if (!*result.reason && live > SB_COMMAND_TREE_PROCESS_LIMIT) strcpy(result.reason, "process_limit");
        }
        reap_descendants(&t);
        siginfo_t exited = {0};
        if (!leader_exited && waitid(P_PID, (id_t)t.leader, &exited, WEXITED | WNOHANG | WNOWAIT) == 0 && exited.si_pid) {
            leader_exited = true;
            result.exit_code = exited.si_code == CLD_EXITED ? exited.si_status : 128 + exited.si_status;
        }
        unsigned char cancel;
        ssize_t n = read(control, &cancel, 1);
        struct pollfd peer = {control, POLLIN, 0};
        poll(&peer, 1, 0);
        if (!*result.reason && (n >= 0 || sb_posix_signal || getppid() != owner || (peer.revents & (POLLHUP | POLLERR)))) strcpy(result.reason, "cancelled");
        if (!*result.reason && now - started >= s->timeout_ms) strcpy(result.reason, "timed_out");
        if (leader_exited || *result.reason) {
            if (!stopping) stopping = now;
            (void)terminate(&t);
            reap_descendants(&t);
            /* An empty observed set is complete even if Darwin refuses to
             * signal a group containing only an unreaped zombie. */
            if (sampled && !live && adopted_children_empty(&t)) { complete = true; break; }
            if (now - stopping >= 3000) break;
        }
        if (now - last_update >= 100) {
            if (!wire(channel, &result, UPDATE, &t) && !*result.reason) strcpy(result.reason, "cancelled");
            last_update = now;
        }
        struct timespec pause = {.tv_nsec = SB_COMMAND_TREE_SAMPLE_MS * 1000000};
        nanosleep(&pause, NULL);
    }
    terminate(&t);
    reap_descendants(&t);
    if (waitpid(t.leader, &leader_status, WNOHANG) == t.leader && !leader_exited)
        result.exit_code = WIFEXITED(leader_status) ? WEXITSTATUS(leader_status) : 128 + WTERMSIG(leader_status);
    result.cleanup_complete = complete;
    if (!*result.reason) strcpy(result.reason, complete ? "completed" : "cleanup_incomplete");
    if (!complete && !*result.error) strcpy(result.error, "Descendant cleanup did not reach an observed empty set before its deadline.");
    wire(channel, &result, FINAL, &t);
    for (unsigned i = 0; i < t.count; i++) if (t.members[i].handle >= 0) close(t.members[i].handle);
    free(t.members); free(t.snapshot); close(channel); close(control);
    return 0;
child_fail:
    result.failure = errno ? errno : EIO;
    terminate(&t);
    kill(t.leader, SIGKILL); /* Our direct, unreaped child, not a sampled PID. */
    while (waitpid(t.leader, NULL, 0) < 0 && errno == EINTR) {}
setup_fail:
    if (!result.failure) result.failure = errno ? errno : EIO;
    wire(channel, &result, FAILED, &t);
    if (failed[0] >= 0) close(failed[0]);
    if (failed[1] >= 0) close(failed[1]);
    if (gate[0] >= 0) close(gate[0]);
    if (gate[1] >= 0) close(gate[1]);
    for (unsigned i = 0; i < t.count; i++) if (t.members[i].handle >= 0) close(t.members[i].handle);
    free(t.members); free(t.snapshot); close(channel); close(control);
    return 127;
}
bool sb_command_watch_start(const sb_command_spec *s, sb_command_watch *w, sb_error *e) {
    memset(w, 0, sizeof(*w)); w->fd = w->cancel_fd = -1;
    int reports[2] = {-1, -1}, control[2] = {-1, -1};
    bool pipes_ok = pipe(reports) == 0 && pipe(control) == 0;
    int descriptors[] = {reports[0], reports[1], control[0], control[1]};
    for (unsigned i = 0; pipes_ok && i < 4; i++)
        pipes_ok = fcntl(descriptors[i], F_SETFD, FD_CLOEXEC) == 0 &&
                   fcntl(descriptors[i], F_SETFL, O_NONBLOCK) == 0;
    if (!pipes_ok) {
        int saved = errno;
        for (unsigned i = 0; i < 4; i++) if (descriptors[i] >= 0) close(descriptors[i]);
        errno = saved; sb_posix_error(e, "Create command supervisor pipes"); return false;
    }
    pid_t pid = fork();
    if (!pid) { close(reports[0]); close(control[1]); int rc = supervise(s, reports[1], control[0]); _exit(rc); }
    close(reports[1]); close(control[0]);
    if (pid < 0) { close(reports[0]); close(control[1]); sb_posix_error(e, "Fork command supervisor"); return false; }
    tree_wire message = {0};
    uint64_t until = sb_platform_millis() + 5000;
    size_t received = 0;
    while (received < sizeof(message) && sb_platform_millis() < until) {
        struct pollfd ready = {reports[0], POLLIN, 0};
        int rc = poll(&ready, 1, 20);
        if (rc < 0 && errno == EINTR) continue;
        if (rc < 0) break;
        if (!rc) continue;
        ssize_t n = read(reports[0], (unsigned char *)&message + received, sizeof(message) - received);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (n <= 0) break;
        received += (size_t)n;
    }
    if (received != sizeof(message) || message.magic != WIRE_MAGIC || message.type != START || message.command <= 0) {
        close(reports[0]); close(control[1]);
        /* Closing the channel requests cleanup; the supervisor is never killed
         * before it has had time to kill/reap its own descendants. */
        int status;
        if (!sb_posix_wait_child(pid, 4500, &status)) { kill(pid, SIGTERM); sb_posix_wait_child(pid, 3500, &status); }
        errno = message.failure ? message.failure : ETIMEDOUT;
        sb_posix_error(e, "Start owned command tree"); return false;
    }
    w->fd = reports[0]; w->cancel_fd = control[1]; w->supervisor = pid; w->command = message.command;
    return true;
}
bool sb_command_watch_poll(sb_command_watch *w) {
    if (w->final) return true;
    if (w->fd < 0) return false;
    for (;;) {
        tree_wire m;
        ssize_t n = read(w->fd, w->frame + w->received, sizeof(m) - w->received);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        w->received += (unsigned)n;
        if (w->received < sizeof(m)) continue;
        memcpy(&m, w->frame, sizeof(m)); w->received = 0;
        if (m.magic != WIRE_MAGIC || (m.type != UPDATE && m.type != FINAL) || m.command != w->command) return false;
        w->memory_peak = m.memory_peak; w->process_peak = m.process_peak;
        snprintf(w->reason, sizeof(w->reason), "%.*s", (int)sizeof(m.reason), m.reason);
        snprintf(w->error, sizeof(w->error), "%.*s", (int)sizeof(m.error), m.error);
        if (m.type == FINAL) { w->final = true; w->cleanup_complete = m.cleanup_complete != 0; w->exit_code = m.exit_code; return true; }
    }
}
void sb_command_watch_cancel(sb_command_watch *w) {
    if (w->cancel_fd >= 0 && !w->cancel_sent && !w->final) {
        unsigned char cancel = 1;
        ssize_t n; do { n = write(w->cancel_fd, &cancel, 1); } while (n < 0 && errno == EINTR);
        if (n == 1) w->cancel_sent = true;
    }
}
void sb_command_watch_close(sb_command_watch *w) {
    if (w->fd >= 0) close(w->fd);
    if (w->cancel_fd >= 0) close(w->cancel_fd);
    w->fd = w->cancel_fd = -1;
}
