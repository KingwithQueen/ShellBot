#ifndef SB_RESTRICTED_H
#define SB_RESTRICTED_H
#include "posix.h"
#include "sb_projects.h"
typedef struct sb_restricted sb_restricted;
yyjson_mut_val *sb_restricted_config(yyjson_mut_doc *d, const sb_host *h);
sb_restricted *sb_restricted_new(yyjson_val *config, sb_error *e);
void sb_restricted_free(sb_restricted *r);
bool sb_restricted_verify(sb_restricted *r, const char *private_path, sb_error *e);
/* Child only; returns on failure, never executes without isolation. */
void sb_restricted_exec(sb_restricted *r, const char *exe, int executable_fd, char *const argv[],
                        char *const env[], unsigned memory_mb);
#endif
