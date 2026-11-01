#ifndef SB_CONPTY_H
#define SB_CONPTY_H
/* Runtime resolution preserves a clear unsupported result on older Windows. */
typedef HRESULT (WINAPI *sb_pty_create_fn)(COORD,HANDLE,HANDLE,DWORD,HPCON*);
typedef HRESULT (WINAPI *sb_pty_resize_fn)(HPCON,COORD);
typedef void (WINAPI *sb_pty_close_fn)(HPCON);
static bool sb_pty_available(void) {
    HMODULE module=GetModuleHandleW(L"kernel32.dll");
    return GetProcAddress(module,"CreatePseudoConsole") && GetProcAddress(module,"ResizePseudoConsole") && GetProcAddress(module,"ClosePseudoConsole");
}
static HRESULT sb_pty_create(COORD size,HANDLE input,HANDLE output,HPCON *pty) {
    sb_pty_create_fn call=(sb_pty_create_fn)(void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"CreatePseudoConsole");
    return call?call(size,input,output,0,pty):E_NOTIMPL;
}
static HRESULT sb_pty_resize(HPCON pty,COORD size) {
    sb_pty_resize_fn call=(sb_pty_resize_fn)(void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"ResizePseudoConsole");
    return call?call(pty,size):E_NOTIMPL;
}
static void sb_pty_close(HPCON pty) {
    sb_pty_close_fn call=(sb_pty_close_fn)(void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"ClosePseudoConsole");
    if(call)call(pty);
}
#endif
