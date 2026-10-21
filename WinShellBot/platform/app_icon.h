#ifndef SHELLBOT_APP_ICON_H
#define SHELLBOT_APP_ICON_H
#include <windows.h>
#include "../app/resources.h"

/* LoadIcon returns a module-owned shared handle. Callers must not destroy it. */
static inline HICON sb_app_icon(void) {
    HICON icon=LoadIconW(GetModuleHandleW(NULL),MAKEINTRESOURCEW(IDI_SHELLBOT));
    return icon?icon:LoadIconW(NULL,IDI_APPLICATION);
}
#endif
