#ifndef SB_SHARED_ADMIN_H
#define SB_SHARED_ADMIN_H
#include "sb.h"
typedef struct sb_shared_admin sb_shared_admin;
sb_shared_admin *sb_shared_admin_new(void);
char *sb_shared_admin_request(sb_shared_admin *state, yyjson_val *request);
void sb_shared_admin_free(sb_shared_admin *state);
#endif
