#ifndef SB_WINDOWS_EXECUTION_H
#define SB_WINDOWS_EXECUTION_H
#include <windows.h>
#include <stdbool.h>
/* Windows-only declarations; native tokens never enter the portable core. */
bool sb_win_owner_token_allowed(HANDLE token);
int sb_win_execution_agent(int argc, wchar_t **argv);
int sb_win_owner_boundary_probe(const char *data_dir);
#endif
