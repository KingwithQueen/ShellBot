#ifndef SB_SERVICE_DESKTOP_H
#define SB_SERVICE_DESKTOP_H
#include "posix.h"
/* One reviewed operation, one system authorization, one authenticated channel.
   The returned JSON owns its memory; no reusable privileged grant is retained. */
char *sb_service_system_call(const char *directory, const char *request,
                            atomic_bool *cancel, sb_error *error);
int sb_service_desktop_cli(int argc, char **argv);
#endif
