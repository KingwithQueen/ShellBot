#ifndef SB_WINDOWS_UIA_H
#define SB_WINDOWS_UIA_H
#include "sb.h"
#include <windows.h>
int sb_win_uia_worker(void);
/* Checks shared human-input pause/epoch and same-account/session/window identity. */
bool sb_win_uia_guard(HWND window,DWORD pid,FILETIME created,LONG epoch,uint64_t deadline,bool input,bool require_foreground);
char *sb_win_uia_request(yyjson_val *request,bool (*guard)(void*),void *context,uint64_t deadline,sb_error *error);
#endif
