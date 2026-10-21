#ifndef SHELLBOT_NATIVE_UI_H
#define SHELLBOT_NATIVE_UI_H
#include <windows.h>
#include <commctrl.h>
#include <wchar.h>
#include <stdbool.h>

/* Shared metrics for native forms. Window content and fonts must move together
   when Windows changes the DPI; replacing a font alone clips existing fields. */
static inline UINT sb_native_dpi(HWND window) {
    UINT dpi=window?GetDpiForWindow(window):GetDpiForSystem();return dpi?dpi:96;
}
static inline HFONT sb_native_font(HWND window) {
    NONCLIENTMETRICSW metrics={0};metrics.cbSize=sizeof(metrics);
    if(!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS,sizeof(metrics),&metrics,0,sb_native_dpi(window))){
        metrics.lfMessageFont.lfHeight=-MulDiv(15,(int)sb_native_dpi(window),96);
        wcscpy(metrics.lfMessageFont.lfFaceName,L"Segoe UI");
    }
    return CreateFontIndirectW(&metrics.lfMessageFont);
}
static inline void sb_native_modal_loop(HWND window,const bool *done){
    MSG message;
    while(!*done){int result=GetMessageW(&message,NULL,0,0);
        if(result<=0){if(!result)PostQuitMessage((int)message.wParam);break;}
        if(!IsDialogMessageW(window,&message)){TranslateMessage(&message);DispatchMessageW(&message);}
    }
}
typedef struct {HWND parent;UINT from,to;HFONT font;} sb_native_scale;
static inline BOOL CALLBACK sb_native_scale_child(HWND child,LPARAM context) {
    sb_native_scale *scale=(sb_native_scale *)context;
    if(GetParent(child)==scale->parent){
        RECT r;GetWindowRect(child,&r);wchar_t name[32];GetClassNameW(child,name,32);
        if(!wcscmp(name,L"ComboBox"))SendMessageW(child,CB_GETDROPPEDCONTROLRECT,0,(LPARAM)&r);
        MapWindowPoints(NULL,scale->parent,(POINT *)&r,2);
        SetWindowPos(child,NULL,MulDiv(r.left,(int)scale->to,(int)scale->from),MulDiv(r.top,(int)scale->to,(int)scale->from),
            MulDiv(r.right-r.left,(int)scale->to,(int)scale->from),MulDiv(r.bottom-r.top,(int)scale->to,(int)scale->from),SWP_NOZORDER|SWP_NOACTIVATE);
        if(!wcscmp(name,WC_LISTVIEWW)){
            int columns=Header_GetItemCount(ListView_GetHeader(child));
            for(int i=0;i<columns;++i)ListView_SetColumnWidth(child,i,MulDiv(ListView_GetColumnWidth(child,i),(int)scale->to,(int)scale->from));
        }
    }
    SendMessageW(child,WM_SETFONT,(WPARAM)scale->font,FALSE);return TRUE;
}
static inline void sb_native_dpi_changed(HWND window,HFONT *font,UINT *dpi,WPARAM wp,LPARAM lp) {
    UINT next=HIWORD(wp);if(!next)next=sb_native_dpi(window);
    HFONT replacement=sb_native_font(window);if(!replacement)return;
    sb_native_scale scale={window,*dpi?*dpi:96,next,replacement};
    bool visible=IsWindowVisible(window)!=FALSE;
    if(visible)SendMessageW(window,WM_SETREDRAW,FALSE,0);
    EnumChildWindows(window,sb_native_scale_child,(LPARAM)&scale);
    HFONT previous=*font;*font=replacement;*dpi=next;
    RECT *r=(RECT *)lp;SetWindowPos(window,NULL,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);
    if(visible)SendMessageW(window,WM_SETREDRAW,TRUE,0);
    RedrawWindow(window,NULL,NULL,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN|RDW_FRAME);
    if(previous)DeleteObject(previous);
}
#endif
