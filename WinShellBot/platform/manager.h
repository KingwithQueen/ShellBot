#ifndef SHELLBOT_MANAGER_H
#define SHELLBOT_MANAGER_H
#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>
bool sb_manager_option(const char *option);
int sb_manager_cli(int argc, char **argv);
int sb_manager_startup(const char *configuration,const char *data,const char *const *projects,
                      size_t count,void *stop,void *ready,bool service_mode);
int sb_project_worker(int argc, wchar_t **argv);
#endif
