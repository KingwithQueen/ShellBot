#ifndef SHELLBOT_STARTUP_H
#define SHELLBOT_STARTUP_H
#include "sb_projects.h"
#include <windows.h>
#define SB_SERVICE_NAME L"ShellBotService"
#define SB_SERVICE_ACCOUNT L"NT SERVICE\\ShellBotService"
typedef struct {
    bool service;
    char *path, *data, *configuration;
    char ids[SB_MAX_PROJECTS][64];
    size_t count, credential_count;
    struct { char name[64], value[257]; } credentials[SB_MAX_PROJECTS+1];
    sb_project_registry registry;
    HANDLE pin;
} sb_startup;
bool sb_startup_option(const char *option);
int sb_startup_cli(int argc,char **argv);
sb_startup *sb_startup_load(const char *path,sb_error *error);
void sb_startup_free(sb_startup *profile);
int sb_startup_manager(sb_startup *profile,HANDLE stop,HANDLE ready);
bool sb_startup_service_sid(unsigned char sid[SECURITY_MAX_SID_SIZE],DWORD *size);
bool sb_startup_grant_profile(sb_startup *profile,sb_error *error);
int sb_service_cli(const char *mode,sb_startup *profile,sb_error *error);
int sb_startup_admin_logon(const char *mode,sb_startup *profile,sb_error *error);
#endif
