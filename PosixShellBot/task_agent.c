#include "posix.h"
#include "restricted.h"
#include "command_memory.h"
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pwd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

/* This single-threaded helper owns task groups. Loss of the worker channel
   cancels them even when the multithreaded worker is killed abruptly. */
#define RECORDS 32
#define INPUTS 16
typedef struct {
    char key[65], hash[65];
    char *bytes;
    size_t length, at;
    bool eof, committed, done, failed;
} input;
typedef struct {
    char id[33], key[65], hash[65], actor[64];
    yyjson_doc *command;
    sb_file *cwd, *program;
    int in, out[2];
    pid_t pid;
    unsigned char *bytes[2];
    size_t length[2];
    input inputs[INPUTS];
    unsigned input_count,memory_failures;
    char memory_error[256];
    uint64_t started, timeout,memory_peak;
    bool prepared, running, finished, dirty, input_closed;
    int exit_code;
    const char *reason;
} task;
typedef struct {
    task items[RECORDS];
    unsigned count;
    sb_resource_limits limits;
    sb_restricted *restricted;
} agent;
static const char *str(yyjson_val *v, const char *k) { return sb_string(yyjson_obj_get(v, k)); }
static void close_fd(int *fd) {
    if (*fd >= 0)
        close(*fd);
    *fd = -1;
}
static bool random_id(char id[33]) {
    unsigned char b[16];
    sb_error e = {0};
    if (!sb_platform_random(b, sizeof(b), &e))
        return false;
    for (unsigned i = 0; i < 16; i++)
        snprintf(id + 2 * i, 3, "%02x", b[i]);
    return true;
}
static bool version_matches(sb_file *f, const char *wanted, sb_error *e) {
    char actual[65];
    return wanted && sb_platform_file_version(f, actual, e) && !strcmp(actual, wanted);
}
static bool native_program(sb_file *f) {
    unsigned char b[4];
    if (pread(f->fd, b, 4, 0) != 4)
        return false;
#ifdef __linux__
    return !memcmp(b, "\177ELF", 4);
#else
    const unsigned char magic[][4] = {{0xfe, 0xed, 0xfa, 0xce}, {0xce, 0xfa, 0xed, 0xfe},
                                      {0xfe, 0xed, 0xfa, 0xcf}, {0xcf, 0xfa, 0xed, 0xfe},
                                      {0xca, 0xfe, 0xba, 0xbe}, {0xbe, 0xba, 0xfe, 0xca},
                                      {0xca, 0xfe, 0xba, 0xbf}, {0xbf, 0xba, 0xfe, 0xca}};
    for (size_t i = 0; i < sizeof(magic) / 4; i++)
        if (!memcmp(b, magic[i], 4))
            return true;
    return false;
#endif
}
static yyjson_mut_val *snapshot(task *t, yyjson_mut_doc *d, bool history) {
    yyjson_mut_val *v = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, v, "task_id", t->id);
    yyjson_mut_obj_add_strcpy(d, v, "request_key", t->key);
    yyjson_mut_obj_add_strcpy(d, v, "request_sha256", t->hash);
    yyjson_mut_obj_add_strcpy(d, v, "_actor", t->actor);
    yyjson_mut_obj_add_uint(d, v, "process_id", t->pid > 0 ? (uint64_t)t->pid : 0);
    yyjson_mut_obj_add_bool(d, v, "running", t->running);
    yyjson_mut_obj_add_bool(d, v, "finished", t->finished);
    yyjson_mut_obj_add_str(d, v, "status",
                           t->prepared  ? "starting"
                           : t->running ? "running"
                           : t->reason  ? t->reason
                                        : "failed");
    if (t->finished)
        yyjson_mut_obj_add_int(d, v, "exit_code", t->exit_code);
    yyjson_mut_obj_add_uint(d, v, "stdout_bytes", t->length[0]);
    yyjson_mut_obj_add_uint(d, v, "stderr_bytes", t->length[1]);
#ifdef __APPLE__
    yyjson_mut_obj_add_uint(d,v,"memory_peak_bytes",t->memory_peak);
    yyjson_mut_obj_add_str(d,v,"memory_accounting","sampled_process_group_physical_footprint");
    if(*t->memory_error)yyjson_mut_obj_add_strcpy(d,v,"memory_monitor_error",t->memory_error);
#endif
    yyjson_mut_val *receipts = yyjson_mut_arr(d);
    for (unsigned i = 0; i < t->input_count; i++) {
        input *p = &t->inputs[i];
        yyjson_mut_val *r = yyjson_mut_obj(d);
        yyjson_mut_obj_add_strcpy(d, r, "input_key", p->key);
        yyjson_mut_obj_add_strcpy(d, r, "sha256", p->hash);
        yyjson_mut_obj_add_uint(d, r, "bytes", p->length);
        yyjson_mut_obj_add_uint(d, r, "written_bytes", p->at);
        yyjson_mut_obj_add_bool(d, r, "eof", p->eof);
        yyjson_mut_obj_add_str(d, r, "status", p->done ? (p->failed ? "failed" : "delivered") : "queued");
        yyjson_mut_arr_add_val(receipts, r);
    }
    yyjson_mut_obj_add_val(d, v, "input_receipts", receipts);
    if (history) {
        yyjson_val *args = yyjson_doc_get_root(t->command);
        const char *keys[] = {"executable", "cwd", "argv"};
        for (unsigned i = 0; i < 3; i++)
            yyjson_mut_obj_add_val(d, v, keys[i], yyjson_val_mut_copy(d, yyjson_obj_get(args, keys[i])));
        if (t->finished)
            for (unsigned i = 0; i < 2; i++) {
                size_t n = t->length[i] < 8192 ? t->length[i] : 8192;
                char *b64 = sb_base64(t->bytes[i], n);
                if (b64)
                    yyjson_mut_obj_add_strcpy(d, v, i ? "stderr_base64" : "stdout_base64", b64);
                free(b64);
                yyjson_mut_obj_add_bool(d, v, i ? "stderr_history_truncated" : "stdout_history_truncated",
                                        n < t->length[i]);
            }
    }
    return v;
}
static task *find(agent *a, yyjson_val *args, bool key) {
    const char *actor = str(args, "actor"), *id = str(args, key ? "request_key" : "task_id");
    if (!actor || !id)
        return NULL;
    for (unsigned i = 0; i < a->count; i++) {
        task *t = &a->items[i];
        if (!strcmp(actor, t->actor) && !strcmp(id, key ? t->key : t->id))
            return t;
    }
    return NULL;
}
static void stop_task(task *t, const char *reason) {
    if (t->running) {
        kill(-t->pid, SIGKILL);
        /* The unreaped leader cannot be reused. Account commands may move
           themselves to another group; never signal that unrelated group. */
        kill(t->pid,SIGKILL);
        if (!t->reason)
            t->reason = reason;
    }
}
static void drain(task *t, unsigned stream, agent *a) {
    if (t->out[stream] < 0)
        return;
    unsigned char b[8192];
    /* Bounded work per tick keeps a noisy task from starving control messages. */
    for (unsigned i = 0; i < 32; i++) {
        ssize_t n = read(t->out[stream], b, sizeof(b));
        if (n < 0 && (errno == EINTR))
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        if (n <= 0) {
            close_fd(&t->out[stream]);
            break;
        }
        size_t available = a->limits.output_bytes - t->length[0] - t->length[1],
               take = (size_t)n < available ? (size_t)n : available;
        if (take) {
            memcpy(t->bytes[stream] + t->length[stream], b, take);
            t->length[stream] += take;
        }
        if (take < (size_t)n)
            stop_task(t, "output_limit");
    }
}
static void tick(agent *a) {
    for (unsigned i = 0; i < a->count; i++) {
        task *t = &a->items[i];
        if (!t->running)
            continue;
        drain(t, 0, a);
        drain(t, 1, a);
#ifdef __APPLE__
        if(!t->reason){
            uint64_t usage=0;sb_error error={0};
            if(!sb_command_memory(t->pid,&usage,&error)){
                /* libproc can lose a process during exit before waitid sees it.
                   Three failed samples bound the grace period; no successful
                   sample is fabricated and the exit is still reaped below. */
                if(++t->memory_failures>=3){snprintf(t->memory_error,sizeof(t->memory_error),"%s",error.text);stop_task(t,"memory_monitor_unavailable");}
            }
            else {t->memory_failures=0;if(usage>t->memory_peak)t->memory_peak=usage;
                if(usage>(uint64_t)a->limits.worker_memory_mb*1024u*1024u)stop_task(t,"memory_limit");}
        }
#endif
        if (sb_platform_millis() - t->started >= t->timeout)
            stop_task(t, "timed_out");
        for (unsigned j = 0; j < t->input_count; j++) {
            input *p = &t->inputs[j];
            if (p->done)
                continue;
            if (!p->committed)
                break;
            if (t->in < 0) {
                p->done = true;
                p->failed = true;
                t->dirty = true;
                free(p->bytes);
                p->bytes = NULL;
                continue;
            }
            if (p->at < p->length) {
                ssize_t n = write(t->in, p->bytes + p->at, p->length - p->at);
                if (n > 0)
                    p->at += (size_t)n;
                else if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
                    break;
                else {
                    close_fd(&t->in);
                    p->failed = true;
                }
            }
            if (p->at == p->length || p->failed) {
                if (p->eof)
                    close_fd(&t->in);
                p->done = true;
                t->dirty = true;
                free(p->bytes);
                p->bytes = NULL;
            } else
                break;
        }
        siginfo_t info = {0};
        if (waitid(P_PID, (id_t)t->pid, &info, WEXITED | WNOHANG | WNOWAIT) == 0 && info.si_pid) {
            /* Leave the leader unreaped until group cleanup: its pid cannot be reused. */
            kill(-t->pid, SIGKILL);
            int status = 0;
            while (waitpid(t->pid, &status, 0) < 0 && errno == EINTR) {
            }
            t->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
            drain(t, 0, a);
            drain(t, 1, a);
            close_fd(&t->out[0]);
            close_fd(&t->out[1]);
            close_fd(&t->in);
            t->running = false;
            t->finished = true;
            if (!t->reason)
                t->reason = "completed";
            t->dirty = true;
            for (unsigned j = 0; j < t->input_count; j++)
                if (!t->inputs[j].done) {
                    t->inputs[j].failed = true;
                    t->inputs[j].done = true;
                    free(t->inputs[j].bytes);
                    t->inputs[j].bytes = NULL;
                }
        }
    }
}
static bool launch(task *t, agent *a, sb_error *e) {
    yyjson_val *args = yyjson_doc_get_root(t->command), *av = yyjson_obj_get(args, "argv");
    char *argv[66] = {0};
    argv[0] = (char *)str(args, "executable");
    for (size_t i = 0; i < yyjson_arr_size(av); i++)
        argv[i + 1] = (char *)sb_string(yyjson_arr_get(av, i));
    struct passwd *user = getpwuid(geteuid());
    char home[SB_PATH_CAP + 6], username[1024], logname[1024];
    if (!user || strlen(user->pw_dir) >= SB_PATH_CAP || strlen(user->pw_name) > 1000) {
        sb_fail(e, "Cannot resolve native command account.");
        return false;
    }
    snprintf(home, sizeof(home), "HOME=%s", user->pw_dir);
    snprintf(username, sizeof(username), "USER=%s", user->pw_name);
    snprintf(logname, sizeof(logname), "LOGNAME=%s", user->pw_name);
    char *env[] = {
        "PATH=/usr/bin:/bin:/usr/sbin:/sbin", home, username, logname, "LANG=C.UTF-8", "TMPDIR=/tmp", NULL};
    if (!version_matches(t->cwd, str(args, "cwd_version"), e) ||
        !version_matches(t->program, str(args, "executable_version"), e)) {
        sb_fail(e, "Prepared program or working directory changed; command was not launched.");
        return false;
    }
    int pipes[4][2];
    for (unsigned i = 0; i < 4; i++) {
        pipes[i][0] = pipes[i][1] = -1;
    }
    bool ok = true;
    for (unsigned i = 0; i < 4; i++)
        if (pipe(pipes[i])) {
            ok = false;
            break;
        } else {
            fcntl(pipes[i][0], F_SETFD, FD_CLOEXEC);
            fcntl(pipes[i][1], F_SETFD, FD_CLOEXEC);
        }
    pid_t pid = ok ? fork() : -1;
    if (pid == 0) {
        int failure = 0;
        if (setpgid(0, 0) || dup2(pipes[0][0], 0) < 0 || dup2(pipes[1][1], 1) < 0 ||
            dup2(pipes[2][1], 2) < 0 || fchdir(t->cwd->fd))
            failure = errno;
        struct rlimit core = {0, 0};
        setrlimit(RLIMIT_CORE, &core);
        struct rlimit cpu = {(rlim_t)(t->timeout / 1000 + 2), (rlim_t)(t->timeout / 1000 + 3)};
        setrlimit(RLIMIT_CPU, &cpu);
#ifdef __linux__
        struct rlimit memory = {(rlim_t)a->limits.worker_memory_mb * 1024u * 1024u,
                                (rlim_t)a->limits.worker_memory_mb * 1024u * 1024u};
        if ((!a->restricted && setrlimit(RLIMIT_AS, &memory)) || prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0))
            failure = errno;
#else
        (void)a;
#endif
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        sigset_t empty;
        sigemptyset(&empty);
        sigprocmask(SIG_SETMASK, &empty, NULL);
        for (unsigned i = 0; i < 4; i++) {
            if (pipes[i][0] > 2)
                close(pipes[i][0]);
            if (i < 3 && pipes[i][1] > 2)
                close(pipes[i][1]);
        }
        if (!failure) {
            if (a->restricted) {
                sb_restricted_exec(a->restricted, argv[0], t->program->fd, argv, env,
                                   a->limits.worker_memory_mb);
                failure = errno;
            } else {
#ifdef __linux__
                fexecve(t->program->fd, argv, env);
#else
                execve(argv[0], argv, env);
#endif
                failure = errno;
            }
        }
        (void)write(pipes[3][1], &failure, sizeof(failure));
        _exit(127);
    }
    if (pid < 0) {
        for (unsigned i = 0; i < 4; i++) {
            close_fd(&pipes[i][0]);
            close_fd(&pipes[i][1]);
        }
        sb_posix_error(e, "Fork command");
        return false;
    }
    t->pid = pid;
    setpgid(pid, pid);
    close_fd(&pipes[0][0]);
    close_fd(&pipes[1][1]);
    close_fd(&pipes[2][1]);
    close_fd(&pipes[3][1]);
    int exec_error = 0;
    struct pollfd p = {.fd = pipes[3][0], .events = POLLIN};
    int ready;
    do {
        ready = poll(&p, 1, 3000);
    } while (ready < 0 && errno == EINTR);
    ssize_t got = ready > 0 ? read(pipes[3][0], &exec_error, sizeof(exec_error)) : -1;
    close_fd(&pipes[3][0]);
    if (got != 0) {
        kill(-pid, SIGKILL);
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
        }
        for (unsigned i = 0; i < 3; i++) {
            close_fd(&pipes[i][0]);
            close_fd(&pipes[i][1]);
        }
        errno = exec_error ? exec_error : ETIMEDOUT;
        sb_posix_error(e, "Execute native command");
        return false;
    }
    t->in = pipes[0][1];
    t->out[0] = pipes[1][0];
    t->out[1] = pipes[2][0];
    fcntl(t->in, F_SETFL, O_NONBLOCK);
    fcntl(t->out[0], F_SETFL, O_NONBLOCK);
    fcntl(t->out[1], F_SETFL, O_NONBLOCK);
    t->started = sb_platform_millis();
    t->running = true;
    t->prepared = false;
    t->dirty = true;
    sb_platform_close(t->cwd);
    t->cwd = NULL;
    sb_platform_close(t->program);
    t->program = NULL;
    return true;
}
static yyjson_mut_val *handle(agent *a, const char *op, yyjson_val *args, yyjson_mut_doc *d) {
    sb_error e = {0};
    task *t = NULL;
    if (!strcmp(op, "configure")) {
        if (a->count || a->restricted) {
            sb_fail(&e, "Task supervisor is already configured.");
            goto fail;
        }
        yyjson_val *restricted = yyjson_obj_get(args, "restricted");
        if (restricted && !(a->restricted = sb_restricted_new(restricted, &e)))
            goto fail;
        uint64_t n;
        const char *keys[] = {"active_tasks",    "task_records",     "output_bytes",
                              "task_timeout_ms", "approval_records", "worker_memory_mb"};
        unsigned *fields[] = {&a->limits.active_tasks,     &a->limits.task_records,
                              &a->limits.output_bytes,     &a->limits.task_timeout_ms,
                              &a->limits.approval_records, &a->limits.worker_memory_mb};
        for (unsigned i = 0; i < 6; i++)
            if (sb_integer(yyjson_obj_get(args, keys[i]), &n))
                *fields[i] = (unsigned)n;
        return yyjson_mut_obj(d);
    }
    if (!strcmp(op, "tick") || !strcmp(op, "cancel_all")) {
        if (!strcmp(op, "cancel_all"))
            for (unsigned i = 0; i < a->count; i++)
                stop_task(&a->items[i], "cancelled");
        return yyjson_mut_obj(d);
    }
    if (!strcmp(op, "prepare")) {
        t = find(a, args, true);
        if (t) {
            if (strcmp(t->hash, str(args, "request_sha256")))
                goto conflict;
            yyjson_mut_val *v = snapshot(t, d, false);
            yyjson_mut_obj_add_bool(d, v, "created_new", false);
            return v;
        }
        unsigned active = 0;
        for (unsigned i = 0; i < a->count; i++)
            if (a->items[i].running || a->items[i].prepared)
                active++;
        if (active >= a->limits.active_tasks || a->count >= a->limits.task_records) {
            sb_fail(&e,
                    "Task capacity reached; retained request keys are never evicted within this instance.");
            goto fail;
        }
        task fresh = {.in = -1, .out = {-1, -1}, .prepared = true, .dirty = false};
        const char *cwd = str(args, "cwd"), *exe = str(args, "executable"), *actor = str(args, "actor"),
                   *key = str(args, "request_key"), *hash = str(args, "request_sha256");
        if (!cwd || !exe || !actor || strlen(actor) >= 64 || !key || strlen(key) > 64 || !hash ||
            strlen(hash) != 64) {
            sb_fail(&e, "Invalid normalized command.");
            goto fail;
        }
        sb_root *r = sb_platform_root(cwd, &e);
        fresh.cwd = r ? sb_platform_open(r, ".", true, &e) : NULL;
        sb_platform_root_close(r);
        char *leaf = NULL;
        r = sb_platform_account_parent(exe, &leaf, &e);
        fresh.program = r ? sb_platform_open(r, leaf, false, &e) : NULL;
        free(leaf);
        sb_platform_root_close(r);
        char sha[65];
        bool valid = fresh.cwd && fresh.program && !(fresh.program->initial.st_mode & (S_ISUID | S_ISGID)) &&
                     native_program(fresh.program) &&
                     version_matches(fresh.cwd, str(args, "cwd_version"), &e) &&
                     version_matches(fresh.program, str(args, "executable_version"), &e) &&
                     sb_platform_file_sha256(fresh.program, sha, &e) && str(args, "executable_sha256") &&
                     !strcmp(sha, str(args, "executable_sha256"));
        if (!valid) {
            sb_platform_close(fresh.cwd);
            sb_platform_close(fresh.program);
            sb_fail(&e, "Native executable or cwd snapshot is invalid (symlinks, scripts and set-id files "
                        "are refused).");
            goto fail;
        }
        char *command = yyjson_val_write(args, 0, NULL);
        fresh.command = command ? yyjson_read(command, strlen(command), 0) : NULL;
        free(command);
        fresh.bytes[0] = malloc(a->limits.output_bytes);
        fresh.bytes[1] = malloc(a->limits.output_bytes);
        if (!fresh.command || !fresh.bytes[0] || !fresh.bytes[1] || !random_id(fresh.id)) {
            yyjson_doc_free(fresh.command);
            free(fresh.bytes[0]);
            free(fresh.bytes[1]);
            sb_platform_close(fresh.cwd);
            sb_platform_close(fresh.program);
            sb_fail(&e, "Cannot reserve command resources.");
            goto fail;
        }
        strcpy(fresh.actor, actor);
        strcpy(fresh.key, key);
        strcpy(fresh.hash, hash);
        sb_integer(yyjson_obj_get(args, "timeout_ms"), &fresh.timeout);
        t = &a->items[a->count++];
        *t = fresh;
        yyjson_mut_val *v = snapshot(t, d, false);
        yyjson_mut_obj_add_bool(d, v, "created_new", true);
        return v;
    }
    t = find(a, args, false);
    if (!t) {
        sb_fail(&e, "Task is missing or belongs to another caller.");
        goto fail;
    }
    if (!strcmp(op, "owned"))
        return snapshot(t, d, false);
    if (!strcmp(op, "commit")) {
        if (t->prepared && !launch(t, a, &e)) {
            t->prepared = false;
            t->finished = true;
            t->reason = "failed";
            t->exit_code = 127;
            t->dirty = true;
            sb_platform_close(t->cwd);
            t->cwd = NULL;
            sb_platform_close(t->program);
            t->program = NULL;
            goto fail;
        }
        yyjson_mut_val *v = snapshot(t, d, false);
        yyjson_mut_obj_add_bool(d, v, "created_new", true);
        return v;
    }
    if (!strcmp(op, "abort")) {
        if (t->prepared) {
            t->prepared = false;
            t->finished = true;
            t->reason = "failed";
            t->dirty = true;
            sb_platform_close(t->cwd);
            t->cwd = NULL;
            sb_platform_close(t->program);
            t->program = NULL;
        }
        return snapshot(t, d, false);
    }
    if (!strcmp(op, "task_cancel")) {
        stop_task(t, "cancelled");
        return snapshot(t, d, false);
    }
    if (!strcmp(op, "input_prepare") || !strcmp(op, "input_commit")) {
        const char *key = str(args, "input_key"), *text = str(args, "text");
        if (!key || !text)
            goto conflict;
        char hash[65];
        sb_sha sha;
        sb_sha_init(&sha);
        sb_sha_update(&sha, text, strlen(text));
        bool eof = yyjson_get_bool(yyjson_obj_get(args, "eof"));
        unsigned char end = eof ? 1 : 0;
        sb_sha_update(&sha, &end, 1);
        sb_sha_finish(&sha, hash);
        input *p = NULL;
        for (unsigned i = 0; i < t->input_count; i++)
            if (!strcmp(t->inputs[i].key, key))
                p = &t->inputs[i];
        if (p) {
            if (strcmp(p->hash, hash))
                goto conflict;
        } else {
            if (!t->running || t->input_closed || t->input_count >= INPUTS) {
                sb_fail(&e, "Task stdin is closed or its receipt capacity is full.");
                goto fail;
            }
            p = &t->inputs[t->input_count++];
            strcpy(p->key, key);
            strcpy(p->hash, hash);
            p->bytes = sb_strdup(text);
            p->length = strlen(text);
            p->eof = eof;
            if (!p->bytes) {
                p->done = p->failed = true;
                sb_fail(&e, "Cannot reserve input bytes.");
                goto fail;
            }
            if (eof)
                t->input_closed = true;
        }
        if (!strcmp(op, "input_commit"))
            p->committed = true;
        t->dirty = true;
        return snapshot(t, d, false);
    }
    if (!strcmp(op, "task_output")) {
        uint64_t offset = 0, maximum = 16384;
        sb_integer(yyjson_obj_get(args, "offset"), &offset);
        sb_integer(yyjson_obj_get(args, "max_bytes"), &maximum);
        unsigned stream = str(args, "stream") && !strcmp(str(args, "stream"), "stderr") ? 1 : 0;
        if (offset > t->length[stream]) {
            sb_fail(&e, "Output offset exceeds retained bytes.");
            goto fail;
        }
        size_t n = t->length[stream] - (size_t)offset;
        if (n > maximum)
            n = (size_t)maximum;
        yyjson_mut_val *v = snapshot(t, d, false);
        char *b64 = sb_base64(t->bytes[stream] + offset, n);
        if (b64)
            yyjson_mut_obj_add_strcpy(d, v, "data_base64", b64);
        free(b64);
        yyjson_mut_obj_add_str(d, v, "encoding", "base64");
        char *decoded = NULL;
        size_t decoded_length = 0, consumed = 0;
        const char *encoding = NULL;
        sb_error utf8_error = {0};
        if (!n || (!memchr(t->bytes[stream] + offset, 0, n) &&
                   sb_decode(t->bytes[stream] + offset, n, true, "utf-8", false, &decoded, &decoded_length,
                             &consumed, &encoding, &utf8_error)))
            yyjson_mut_obj_add_strncpy(d, v, "text_utf8", (const char *)t->bytes[stream] + offset, n);
        free(decoded);
        yyjson_mut_obj_add_uint(d, v, "offset", offset);
        yyjson_mut_obj_add_uint(d, v, "next_offset", offset + n);
        yyjson_mut_obj_add_bool(d, v, "eof", t->finished && offset + n == t->length[stream]);
        return v;
    }
    return snapshot(t, d, false);
conflict:
    sb_fail(&e, "Idempotency key conflicts with previously reserved command or stdin bytes.");
fail: {
    yyjson_mut_val *v = yyjson_mut_obj(d);
    yyjson_mut_obj_add_bool(d, v, "_error", true);
    yyjson_mut_obj_add_strcpy(d, v, "error", *e.text ? e.text : "Invalid private task operation.");
    return v;
}
}
int sb_posix_task_agent(int fd) {
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (!sb_posix_peer(fd, true) || getuid() != geteuid())
        return 2;
    sb_posix_close_inherited();
    agent a = {.limits = {8, 32, 262144, 3600000, 64, 512}};
    while (!sb_posix_signal) {
        tick(&a);
        struct pollfd p = {.fd = fd, .events = POLLIN};
        int n = poll(&p, 1, 20);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 || (n > 0 && (p.revents & (POLLERR | POLLHUP | POLLNVAL))))
            break;
        if (!n)
            continue;
        sb_error e = {0};
        char *bytes = sb_posix_frame_read(fd, 3000, &e);
        if (!bytes)
            break;
        yyjson_doc *request = yyjson_read(bytes, strlen(bytes), 0);
        free(bytes);
        yyjson_val *root = yyjson_doc_get_root(request);
        const char *op = str(root, "op");
        yyjson_val *args = yyjson_obj_get(root, "args");
        if (!op || !yyjson_is_obj(args)) {
            yyjson_doc_free(request);
            break;
        }
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        if (!d) {
            yyjson_doc_free(request);
            break;
        }
        yyjson_mut_val *reply = handle(&a, op, args, d), *events = yyjson_mut_arr(d);
        unsigned active = 0;
        for (unsigned i = 0; i < a.count; i++) {
            if (a.items[i].running)
                active++;
            if (a.items[i].dirty) {
                yyjson_mut_arr_add_val(events, snapshot(&a.items[i], d, true));
                a.items[i].dirty = false;
            }
        }
        yyjson_mut_obj_add_uint(d, reply, "_active", active);
        yyjson_mut_obj_add_val(d, reply, "_events", events);
        char *response = sb_posix_json(d, reply);
        yyjson_doc_free(request);
        bool ok = response && sb_posix_frame_write(fd, response, 5000, &e);
        free(response);
        if (!ok)
            break;
    }
    for (unsigned i = 0; i < a.count; i++)
        stop_task(&a.items[i], "cancelled");
    for (unsigned pass = 0; pass < 100; pass++) {
        tick(&a);
        bool active = false;
        for (unsigned i = 0; i < a.count; i++)
            active |= a.items[i].running;
        if (!active)
            break;
        struct timespec pause = {.tv_nsec = 10000000};
        nanosleep(&pause, NULL);
    }
    for (unsigned i = 0; i < a.count; i++) {
        task *t = &a.items[i];
        if (t->running) {
            kill(-t->pid, SIGKILL);
            while (waitpid(t->pid, NULL, 0) < 0 && errno == EINTR) {
            }
        }
        close_fd(&t->in);
        close_fd(&t->out[0]);
        close_fd(&t->out[1]);
        sb_platform_close(t->cwd);
        sb_platform_close(t->program);
        free(t->bytes[0]);
        free(t->bytes[1]);
        yyjson_doc_free(t->command);
        for (unsigned j = 0; j < t->input_count; j++)
            free(t->inputs[j].bytes);
    }
    close(fd);
    sb_restricted_free(a.restricted);
    return 0;
}
