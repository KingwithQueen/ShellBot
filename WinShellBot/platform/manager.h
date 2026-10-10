#ifndef SHELLBOT_MANAGER_H
#define SHELLBOT_MANAGER_H
#include <stdbool.h>
#include <wchar.h>
bool sb_manager_option(const char *option);
int sb_manager_cli(int argc, char **argv);
int sb_project_worker(int argc, wchar_t **argv);
#endif
