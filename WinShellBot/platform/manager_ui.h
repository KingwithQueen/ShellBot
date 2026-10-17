#ifndef SHELLBOT_MANAGER_UI_H
#define SHELLBOT_MANAGER_UI_H
#include <windows.h>
int sb_manager_ui(const char *data_dir);
int sb_manager_resident_ui(const char *data_dir,HANDLE stop,HANDLE manager_thread,const char *profile_path);
#endif
