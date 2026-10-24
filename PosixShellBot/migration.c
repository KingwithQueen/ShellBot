#include "migration.h"
#include "sb_projects.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool same(const struct stat *a, const struct stat *b) {
#ifdef __APPLE__
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_size == b->st_size &&
        a->st_mtimespec.tv_sec == b->st_mtimespec.tv_sec && a->st_mtimespec.tv_nsec == b->st_mtimespec.tv_nsec &&
        a->st_ctimespec.tv_sec == b->st_ctimespec.tv_sec && a->st_ctimespec.tv_nsec == b->st_ctimespec.tv_nsec;
#else
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_size == b->st_size &&
        a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
        a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
#endif
}
sb_root *sb_migration_destination(const char *path, sb_error *e) {
    if (!sb_project_path_valid(path)) { sb_fail(e, "迁移目标必须为规范的绝对路径。"); return NULL; }
    if (mkdir(path, 0700) && errno != EEXIST) { sb_posix_error(e, "Create migration destination"); return NULL; }
    sb_root *r = sb_platform_root(path, e);
    if (!r || !sb_posix_private(r, e)) { sb_platform_root_close(r); return NULL; }
    DIR *d = fdopendir(openat(r->fd,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC));
    bool empty = d != NULL;
    struct dirent *entry;errno=0;
    while (empty && (entry = readdir(d))) if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) empty = false;
    if(errno)empty=false;
    if (d) closedir(d);
    if (!empty) { sb_platform_root_close(r); sb_fail(e, "迁移目标必须是空的私有目录；没有覆盖已有文件。"); return NULL; }
    return r;
}
static bool copy_tree(int source, int target, unsigned depth, sb_migration *s, sb_error *e) {
    if (depth > 64 || s->files + s->directories > 100000 || sb_posix_signal ||
        (s->cancel && atomic_load(s->cancel))) { sb_fail(e, "迁移已取消或超出目录数量/深度限制；原数据保留。"); return false; }
    struct stat before, after;
    if (fstat(source, &before)) return false;
    DIR *d = fdopendir(openat(source,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC));
    if (!d) return false;
    bool ok = true;
    struct dirent *entry;
    while (ok) {
        errno=0;entry=readdir(d);if(!entry){if(errno)ok=false;break;}
        const char *name = entry->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        if (sb_posix_signal || (s->cancel && atomic_load(s->cancel)) || s->files + s->directories >= 100000) { ok = false; break; }
        struct stat st, opened, final;
        if (fstatat(source, name, &st, AT_SYMLINK_NOFOLLOW) || st.st_uid != geteuid()) { ok = false; break; }
        if (!strcmp(name, "host.lock") && S_ISREG(st.st_mode) && st.st_nlink == 1 && !(st.st_mode & 077)) continue;
        if ((!strcmp(name, "owner.sock") && S_ISSOCK(st.st_mode)) || !strcmp(name, "raise.request") ||
            !strcmp(name, "owner.endpoint.json") || !strncmp(name, ".owner-endpoint-", 16)) continue;
        bool directory = S_ISDIR(st.st_mode);
        if ((!directory && (!S_ISREG(st.st_mode) || st.st_nlink != 1)) || (st.st_mode & (S_ISUID | S_ISGID)) ||
            (directory && (st.st_mode & 0022))) { ok = false; break; }
        int in = openat(source, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK | (directory ? O_DIRECTORY : 0));
        if (in < 0 || fstat(in, &opened) || !same(&st, &opened)) { if (in >= 0) close(in); ok = false; break; }
        if (directory) {
            ok = !mkdirat(target, name, 0700);
            int out = ok ? openat(target, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_DIRECTORY) : -1;
            s->directories++;
            ok = out >= 0 && copy_tree(in, out, depth + 1, s, e) && !fsync(out);
            if (out >= 0) close(out);
        } else {
            int out = openat(target, name, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
            ok = out >= 0 && st.st_size >= 0 && (uint64_t)st.st_size <= UINT64_C(8589934592) - s->bytes;
            sb_sha hash; sb_sha_init(&hash);
            unsigned char *buf=malloc(65536); uint64_t size = 0;
            ok=ok&&buf!=NULL;
            while (ok) {
                if (sb_posix_signal || (s->cancel && atomic_load(s->cancel))) { ok = false; break; }
                ssize_t n = read(in, buf, 65536);
                if (n < 0 && errno == EINTR) continue;
                if (n < 0) { ok = false; break; }
                if (!n) break;
                size += (uint64_t)n;
                if (size > (uint64_t)st.st_size || !sb_posix_write(out, buf, (size_t)n, e)) { ok = false; break; }
                sb_sha_update(&hash, buf, (size_t)n);
            }
            char wanted[65], actual[65]; sb_sha_finish(&hash, wanted);
            ok = ok && size == (uint64_t)st.st_size && !fstat(in, &final) && same(&opened, &final) &&
                !fsync(out) && lseek(out, 0, SEEK_SET) == 0;
            sb_sha_init(&hash);
            while (ok) {
                if(sb_posix_signal||(s->cancel&&atomic_load(s->cancel))){ok=false;break;}
                ssize_t n = read(out, buf, 65536);
                if (n < 0 && errno == EINTR) continue;
                if (n < 0) { ok = false; break; }
                if (!n) break;
                sb_sha_update(&hash, buf, (size_t)n);
            }
            sb_sha_finish(&hash, actual);
            ok = ok && !strcmp(wanted, actual) && !fchmod(out, st.st_mode & 0700) && !fsync(out);
            if (out >= 0) close(out);
            free(buf);
            s->files++; s->bytes += size;
        }
        close(in);
        if (ok && (fstatat(source, name, &final, AT_SYMLINK_NOFOLLOW) || !same(&opened, &final))) ok = false;
    }
    closedir(d);
    ok = ok && !fstat(source, &after) && same(&before, &after) && !fsync(target);
    if (!ok && !*e->text) sb_fail(e, "迁移取消、目录发生变化或含链接/特殊文件；源数据保留，未切换位置。请保留目标供检查。");
    return ok;
}
bool sb_migration_copy(sb_root *source, sb_root *target, sb_migration *s, sb_error *e) {
    if (sb_platform_roots_overlap(source, target)) { sb_fail(e, "源目录与目标目录不能重叠或互为别名。"); return false; }
    return copy_tree(source->fd, target->fd, 0, s, e);
}
bool sb_migration_write(sb_root *root, const char *name, const char *bytes, size_t length, sb_error *e) {
    unsigned char random[16]; char temp[64];
    if (!sb_platform_random(random, sizeof(random), e)) return false;
    strcpy(temp, ".migration-");
    for (unsigned i = 0; i < 16; i++) snprintf(temp + 11 + i * 2, 3, "%02x", random[i]);
    int fd = openat(root->fd, temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    bool ok = sb_posix_write(fd, bytes, length, e) && !fsync(fd);
    close(fd);
    struct stat st;
    if (!fstatat(root->fd, name, &st, AT_SYMLINK_NOFOLLOW))
        ok = ok && S_ISREG(st.st_mode) && st.st_uid == geteuid() && st.st_nlink == 1 && !(st.st_mode & 077);
    else if (errno != ENOENT) ok = false;
    if (ok) ok = !renameat(root->fd, temp, root->fd, name) && !fsync(root->fd);
    if (!ok && !*e->text) sb_fail(e, "迁移配置写入失败；原位置及准备文件保留。");
    return ok;
}
bool sb_migration_can_start(sb_lease *lease, sb_error *e) {
    struct stat st;
    if (!fstatat(lease->root->fd,"migration-guard.json",&st,AT_SYMLINK_NOFOLLOW) || errno!=ENOENT) {
        sb_fail(e,"This Data directory was retired or has an interrupted migration. Use the active native profile; reinstall old services with its new registry and manager Data. Original data was retained.");return false;
    }
    return true;
}
