#ifndef SB_WINDOWS_EXECUTION_H
#define SB_WINDOWS_EXECUTION_H
#include <windows.h>
#include <stdbool.h>
#include "sb.h"
/* Windows-only declarations; native tokens never enter the portable core. */
bool sb_win_owner_token_allowed(HANDLE token);
int sb_win_execution_agent(int argc, wchar_t **argv);
int sb_win_owner_boundary_probe(const char *data_dir);
sb_execution *sb_win_shared_start(sb_error *error);
bool sb_win_shared_alive(sb_execution *execution);
char *sb_win_shared_exchange(sb_execution *execution, const char *request, sb_error *error);
sb_execution *sb_win_administrator_host(const sb_config *config, const sb_project *project, const char *instance, sb_error *error);
bool sb_win_execution_cancel_all(sb_execution *execution);
bool sb_win_execution_watch_worker(sb_execution *execution, HANDLE worker);
bool sb_win_shared_worker(DWORD pid, HANDLE *process);
#endif
