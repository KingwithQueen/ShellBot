#ifndef SB_ADMIN_DESKTOP_H
#define SB_ADMIN_DESKTOP_H
#include "posix.h"
typedef struct {
    int fd;
    pid_t launcher;
    char registry_sha256[65];
} sb_admin_link;
bool sb_admin_peer(int fd, uid_t uid, pid_t pid);
int sb_admin_desktop_connect(const char *path, uid_t uid, pid_t pid, sb_error *error);
bool sb_admin_authorize_launch(char *const argv[], int log, pid_t *pid, sb_error *error);
bool sb_admin_link_start(sb_admin_link *link, const char *directory, const char *registry,
                         const char *state, const char *sha256, atomic_bool *cancel, sb_error *error);
char *sb_admin_link_call(sb_admin_link *link, const char *request, sb_error *error);
void sb_admin_link_close(sb_admin_link *link);
bool sb_admin_link_closing(sb_admin_link *link);
#endif
