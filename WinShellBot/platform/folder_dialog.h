#ifndef SHELLBOT_FOLDER_DIALOG_H
#define SHELLBOT_FOLDER_DIALOG_H
#include <windows.h>
#include <stdbool.h>

/* Updates the edit only after a filesystem directory is accepted. */
bool sb_choose_directory(HWND owner,HWND edit,const wchar_t *title);
#endif
