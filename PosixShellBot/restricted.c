#include "restricted.h"
#include <sys/wait.h>
#include <sys/resource.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#ifdef __linux__
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <linux/seccomp.h>
#include <linux/filter.h>
#include <linux/audit.h>
#include <stddef.h>
#else
#include <sandbox.h>
#endif
typedef struct {
    char *path;
    int fd;
    bool readonly;
} resource;
struct sb_restricted {
    resource roots[SB_MAX_PROJECT_DIRECTORIES], files[16];
    size_t nr, nf;
};
static const char *str(yyjson_val *v, const char *k) { return sb_string(yyjson_obj_get(v, k)); }
static bool within(const char *parent, const char *child) {
    size_t n = strlen(parent);
    return !strncmp(parent, child, n) && (!child[n] || child[n] == '/' || n == 1);
}
yyjson_mut_val *sb_restricted_config(yyjson_mut_doc *d, const sb_host *h) {
    yyjson_mut_val *v = yyjson_mut_obj(d), *roots = yyjson_mut_arr(d), *files = yyjson_mut_arr(d);
    yyjson_mut_obj_add_val(d, v, "roots", roots);
    yyjson_mut_obj_add_val(d, v, "files", files);
    if (!h->project)
        return v;
    for (size_t i = 0; i < h->project->directory_count; i++) {
        const sb_project_directory *p = &h->project->directories[i];
        yyjson_mut_val *r = yyjson_mut_obj(d);
        yyjson_mut_obj_add_strcpy(d, r, "path", p->path);
        yyjson_mut_obj_add_bool(d, r, "read_only", p->read_only);
        yyjson_mut_arr_add_val(roots, r);
    }
    for (size_t i = 0; i < h->project->policy.runtime_file_count; i++)
        yyjson_mut_arr_add_strcpy(d, files, h->project->policy.runtime_files[i]);
    return v;
}
void sb_restricted_free(sb_restricted *r) {
    if (!r)
        return;
    for (size_t i = 0; i < r->nr; i++) {
        close(r->roots[i].fd);
        free(r->roots[i].path);
    }
    for (size_t i = 0; i < r->nf; i++) {
        close(r->files[i].fd);
        free(r->files[i].path);
    }
    free(r);
}
sb_restricted *sb_restricted_new(yyjson_val *v, sb_error *e) {
    yyjson_val *roots = yyjson_obj_get(v, "roots"), *files = yyjson_obj_get(v, "files");
    if (!yyjson_is_arr(roots) || !yyjson_is_arr(files) || !yyjson_arr_size(roots) ||
        yyjson_arr_size(roots) > SB_MAX_PROJECT_DIRECTORIES || !yyjson_arr_size(files) ||
        yyjson_arr_size(files) > 16) {
        sb_fail(e, "Restricted execution requires configured directories and exact runtime_files.");
        return NULL;
    }
    sb_restricted *r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    for (size_t i = 0; i < yyjson_arr_size(roots); i++) {
        yyjson_val *entry = yyjson_arr_get(roots, i);
        const char *path = str(entry, "path");
        sb_root *root = path ? sb_platform_root(path, e) : NULL;
        if (!root)
            goto fail;
        /* Runtime mounts must not reveal the host filesystem or runtime tree. */
        const char *reserved[] = {"/usr", "/lib", "/lib64", "/System", "/dev", "/proc", "/sys", "/etc"};
        bool safe = true;
        for (size_t j = 0; j < sizeof(reserved) / sizeof(*reserved); j++)
            if (within(path, reserved[j]) || within(reserved[j], path))
                safe = false;
        if (!safe) {
            sb_platform_root_close(root);
            sb_fail(e, "Restricted directory overlaps a reserved OS runtime path.");
            goto fail;
        }
        resource *a = &r->roots[r->nr++];
        a->fd = dup(root->fd);
        a->path = sb_strdup(path);
        a->readonly = yyjson_get_bool(yyjson_obj_get(entry, "read_only"));
        sb_platform_root_close(root);
        if (a->fd < 0 || !a->path)
            goto fail;
        fcntl(a->fd, F_SETFD, FD_CLOEXEC);
    }
    for (size_t i = 0; i < yyjson_arr_size(files); i++) {
        const char *path = sb_string(yyjson_arr_get(files, i));
        char *leaf = NULL;
        sb_root *root = path ? sb_platform_account_parent(path, &leaf, e) : NULL;
        sb_file *file = root ? sb_platform_open(root, leaf, false, e) : NULL;
        free(leaf);
        sb_platform_root_close(root);
        if (!file)
            goto fail;
        bool safe = !(file->initial.st_mode & (S_ISUID | S_ISGID));
        for (size_t j = 0; j < r->nr; j++)
            if (within(r->roots[j].path, path))
                safe = false;
        if (!safe) {
            sb_platform_close(file);
            sb_fail(e, "Restricted runtime files must be non-set-id and outside exposed directories.");
            goto fail;
        }
        resource *a = &r->files[r->nf++];
        a->fd = dup(file->fd);
        a->path = sb_strdup(path);
        a->readonly = true;
        sb_platform_close(file);
        if (a->fd < 0 || !a->path)
            goto fail;
        fcntl(a->fd, F_SETFD, FD_CLOEXEC);
    }
    return r;
fail:
    sb_restricted_free(r);
    return NULL;
}
#ifdef __linux__
static int filter_fd(void) {
    struct sock_filter b[160];
    unsigned n = 0;
#define EMIT(x)                                                                                              \
    do {                                                                                                     \
        struct sock_filter item = x;                                                                         \
        b[n++] = item;                                                                                       \
    } while (0)
    EMIT(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)));
    EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0));
    EMIT(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));
    EMIT(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)));
    EMIT(BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 0x40000000, 0, 1));
    EMIT(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));
    const int denied[] = {SYS_clone,
                          SYS_clone3,
                          SYS_fork,
                          SYS_vfork,
                          SYS_socket,
                          SYS_socketpair,
                          SYS_ptrace,
                          SYS_process_vm_readv,
                          SYS_process_vm_writev,
                          SYS_kill,
                          SYS_tkill,
                          SYS_tgkill,
                          SYS_pidfd_open,
                          SYS_pidfd_getfd,
                          SYS_pidfd_send_signal,
                          SYS_setns,
                          SYS_unshare,
                          SYS_mount,
                          SYS_umount2,
                          SYS_pivot_root,
                          SYS_chroot,
                          SYS_setsid,
                          SYS_setpgid,
                          SYS_bpf,
                          SYS_perf_event_open,
                          SYS_userfaultfd,
                          SYS_io_uring_setup,
                          SYS_open_by_handle_at,
                          SYS_link,
                          SYS_linkat,
                          SYS_mknod,
                          SYS_mknodat,
                          SYS_keyctl,
                          SYS_add_key,
                          SYS_request_key,
                          SYS_reboot,
                          SYS_kexec_load,
                          SYS_init_module,
                          SYS_finit_module,
                          SYS_delete_module};
    for (size_t i = 0; i < sizeof(denied) / sizeof(*denied); i++) {
        EMIT(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (unsigned)denied[i], 0, 1));
        EMIT(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM));
    }
    EMIT(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
#undef EMIT
    int fd = (int)syscall(SYS_memfd_create, "shellbot-seccomp", 0);
    if (fd < 0)
        return -1;
    if (write(fd, b, n * sizeof(*b)) != (ssize_t)(n * sizeof(*b)) || lseek(fd, 0, SEEK_SET) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}
void sb_restricted_exec(sb_restricted *r, const char *exe, int program, char *const av[], char *const env[],
                        unsigned memory_mb) {
    struct stat st;
    if (lstat("/usr/bin/bwrap", &st) || !S_ISREG(st.st_mode) || st.st_uid != 0 || (st.st_mode & 0022) ||
        (st.st_mode & (S_ISUID | S_ISGID))) {
        errno = EPERM;
        return;
    }
    int sec = filter_fd();
    if (sec < 0)
        return;
    char *a[384];
    char numbers[64][24];
    size_t n = 0, f = 0;
#define ADD(x) a[n++] = (char *)(x)
#define FD(x) (snprintf(numbers[f], sizeof(numbers[f]), "%d", (x)), numbers[f++])
    ADD("/usr/bin/bwrap");
    ADD("--unshare-all");
    ADD("--die-with-parent");
    ADD("--cap-drop");
    ADD("ALL");
    ADD("--clearenv");
    ADD("--setenv");
    ADD("PATH");
    ADD("/usr/bin:/bin");
    ADD("--setenv");
    ADD("LANG");
    ADD("C.UTF-8");
    ADD("--setenv");
    ADD("HOME");
    ADD("/nonexistent");
    ADD("--setenv");
    ADD("TMPDIR");
    ADD("/tmp");
    const char *system[] = {"/usr/lib", "/usr/lib64", "/lib", "/lib64", "/etc/ld.so.cache"};
    for (size_t i = 0; i < sizeof(system) / sizeof(*system); i++)
        if (!stat(system[i], &st)) {
            ADD("--ro-bind");
            ADD(system[i]);
            ADD(system[i]);
        }
    ADD("--dev");
    ADD("/dev");
    ADD("--size");
    ADD("67108864");
    ADD("--tmpfs");
    ADD("/tmp");
    for (size_t i = 0; i < r->nr; i++) {
        fcntl(r->roots[i].fd, F_SETFD, 0);
        ADD(r->roots[i].readonly ? "--ro-bind-fd" : "--bind-fd");
        ADD(FD(r->roots[i].fd));
        ADD(r->roots[i].path);
    }
    for (size_t i = 0; i < r->nf; i++) {
        if (!strcmp(exe, r->files[i].path))
            continue;
        fcntl(r->files[i].fd, F_SETFD, 0);
        ADD("--ro-bind-fd");
        ADD(FD(r->files[i].fd));
        ADD(r->files[i].path);
    }
    fcntl(program, F_SETFD, 0);
    ADD("--ro-bind-fd");
    ADD(FD(program));
    ADD(exe);
    ADD("--seccomp");
    ADD(FD(sec));
    /* Placeholder ancestors belong to the private tmpfs, not the host. Make
       them readonly too; only explicit rw mounts and /tmp may accept writes. */
    ADD("--remount-ro");
    ADD("/");
    char cwd[SB_PATH_CAP];
    if (!getcwd(cwd, sizeof(cwd))) {
        close(sec);
        return;
    }
    ADD("--chdir");
    ADD(cwd);
    ADD("--");
    for (size_t i = 0; av[i]; i++)
        ADD(av[i]);
    a[n] = NULL;
    /* Build argv before the command ceiling: an instrumented supervisor can
       need temporary ASan mappings while preparing its trusted sandbox. */
    if (memory_mb) {
        struct rlimit limit = {(rlim_t)memory_mb * 1024u * 1024u, (rlim_t)memory_mb * 1024u * 1024u};
        if (setrlimit(RLIMIT_AS, &limit)) {
            close(sec);
            return;
        }
    }
    execve(a[0], a, env);
    int saved = errno;
    close(sec);
    errno = saved;
#undef ADD
#undef FD
}
#else
static bool quote(char *out, size_t cap, const char *s) {
    size_t n = 0;
    for (; *s; s++) {
        if ((unsigned char)*s < 32)
            return false;
        if (*s == '"' || *s == '\\') {
            if (n + 2 >= cap)
                return false;
            out[n++] = '\\';
        }
        if (n + 1 >= cap)
            return false;
        out[n++] = *s;
    }
    out[n] = 0;
    return true;
}
void sb_restricted_exec(sb_restricted *r, const char *exe, int program, char *const av[], char *const env[],
                        unsigned memory_mb) {
    char profile[65536] =
        "(version 1)(deny default)(allow sysctl-read)(allow file-read* (subpath "
        "\"/System/Library\") (subpath \"/usr/lib\") (literal \"/\") (literal \"/dev/null\") (literal "
        "\"/dev/urandom\"))(allow file-write-data (literal \"/dev/null\"))";
    char path[SB_PATH_CAP * 2];
    size_t n = strlen(profile);
    for (size_t i = 0; i < r->nr; i++) {
        if (!quote(path, sizeof(path), r->roots[i].path)) {
            errno = EINVAL;
            return;
        }
        int z = snprintf(profile + n, sizeof(profile) - n, "(allow file-read* %s (subpath \"%s\"))",
                         r->roots[i].readonly ? "" : "file-write*", path);
        if (z < 0 || (size_t)z >= sizeof(profile) - n) {
            errno = E2BIG;
            return;
        }
        n += (size_t)z;
    }
    for (size_t i = 0; i <= r->nf; i++) {
        const char *p = i == r->nf ? exe : r->files[i].path;
        if (!quote(path, sizeof(path), p)) {
            errno = EINVAL;
            return;
        }
        int z = snprintf(profile + n, sizeof(profile) - n, "(allow file-read* process-exec (literal \"%s\"))",
                         path);
        if (z < 0 || (size_t)z >= sizeof(profile) - n) {
            errno = E2BIG;
            return;
        }
        n += (size_t)z;
    }
    char *error = NULL;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    int rc = sandbox_init(profile, 0, &error);
    if (error)
        sandbox_free_error(error);
#pragma clang diagnostic pop
    if (rc) {
        errno = EPERM;
        return;
    }
    (void)program;
    (void)memory_mb;
    execve(exe, av, env);
}
#endif
bool sb_restricted_verify(sb_restricted *r, const char *private_path, sb_error *e) {
    char probe[SB_PATH_CAP];
    snprintf(probe, sizeof(probe), "%s", sb_posix_program());
    char *slash = strrchr(probe, '/');
    if (!slash || (size_t)(slash - probe) + 1 + strlen("ShellBot-RestrictedProbe") >= sizeof(probe)) {
        sb_fail(e, "Cannot locate restricted probe.");
        return false;
    }
    strcpy(slash + 1, "ShellBot-RestrictedProbe");
    char *leaf = NULL;
    sb_root *root = sb_platform_account_parent(probe, &leaf, e);
    sb_file *f = root ? sb_platform_open(root, leaf, false, e) : NULL;
    free(leaf);
    sb_platform_root_close(root);
    if (!f)
        return false;
    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        if (fchdir(r->roots[0].fd))
            _exit(120);
        char *av[] = {probe, (char *)private_path, NULL};
        char *env[] = {"PATH=/usr/bin:/bin", NULL};
        sb_restricted_exec(r, probe, f->fd, av, env, 0);
        _exit(121);
    }
    sb_platform_close(f);
    int status = 0;
    bool ok = pid > 0 && sb_posix_wait_child(pid, 8000, &status);
    if (pid > 0 && !ok) {
        kill(-pid, SIGKILL);
        waitpid(pid, &status, 0);
    }
    if (!ok || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (e)
            snprintf(
                e->text, sizeof(e->text),
                "Native restricted isolation probe failed (status %d); no restricted commands are available.",
                status);
        return false;
    }
    return true;
}
