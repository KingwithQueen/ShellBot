#ifndef SHELLBOT_WINDOWS_DESKTOP_H
#define SHELLBOT_WINDOWS_DESKTOP_H
#include "sb_projects.h"
/* Disabled projects never create hooks, windows, screenshots or clipboard access. */
sb_desktop *sb_win_desktop_new(sb_host *host, sb_error *error);
void sb_win_desktop_free(sb_desktop *desktop);
char *sb_win_desktop_session_status(void);
typedef struct sb_win_desktop_session sb_win_desktop_session;
/* Hold shared pause/epoch state across project worker restarts. No hooks or UI. */
sb_win_desktop_session *sb_win_desktop_session_new(void);
void sb_win_desktop_session_free(sb_win_desktop_session *session);
/* Same-account local owner only. Never invoked through the remote owner API. */
char *sb_win_desktop_owner(const char *command, sb_audit *audit);
#endif
