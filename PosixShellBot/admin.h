#ifndef SB_POSIX_ADMIN_H
#define SB_POSIX_ADMIN_H
#include "posix.h"
int sb_posix_admin_agent(int argc, char **argv);
int sb_posix_admin_broker(int argc, char **argv);
void sb_posix_admin_connect(sb_host *host);
void sb_posix_admin_close(sb_host *host);
char *sb_posix_admin_owner(sb_host *host, const char *request);
#endif
