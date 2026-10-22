#ifndef SB_POSIX_ADMIN_H
#define SB_POSIX_ADMIN_H
#include "posix.h"
int sb_posix_admin_agent(int argc, char **argv);
void sb_posix_admin_connect(sb_host *host);
void sb_posix_admin_close(sb_host *host);
#endif
