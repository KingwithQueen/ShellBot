#ifndef SB_SANDBOX_H
#define SB_SANDBOX_H
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "sb_projects.h"
typedef struct sb_win_sandbox sb_win_sandbox;
sb_win_sandbox *sb_win_sandbox_new(sb_host *host, const char *instance, sb_error *error);
bool sb_win_sandbox_attributes(sb_win_sandbox *sandbox, LPPROC_THREAD_ATTRIBUTE_LIST attributes);
bool sb_win_sandbox_token(sb_win_sandbox *sandbox, HANDLE process);
bool sb_win_sandbox_program(sb_win_sandbox *sandbox, const wchar_t *path);
void sb_win_sandbox_free(sb_win_sandbox *sandbox);
bool sb_win_sandbox_recover(const sb_project *project, sb_error *error);
#endif
