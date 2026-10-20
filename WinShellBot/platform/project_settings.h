#ifndef SHELLBOT_PROJECT_SETTINGS_H
#define SHELLBOT_PROJECT_SETTINGS_H
#include <windows.h>
int sb_project_settings(HWND owner, const char *path, const char *snapshot);
char *sb_project_settings_edit(HWND owner,const char *snapshot);
#endif
