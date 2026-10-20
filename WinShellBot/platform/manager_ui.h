#ifndef SHELLBOT_MANAGER_UI_H
#define SHELLBOT_MANAGER_UI_H
#include <windows.h>
typedef struct sb_desktop_app sb_desktop_app;
int sb_manager_ui(const char *data_dir);
int sb_manager_resident_ui(const char *data_dir,HANDLE stop,HANDLE manager_thread,const char *profile_path);
int sb_manager_desktop_ui(sb_desktop_app *app);
#endif
