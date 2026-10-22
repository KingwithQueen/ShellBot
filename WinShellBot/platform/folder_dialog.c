#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <shlobj.h>
#include "folder_dialog.h"
#include <stdlib.h>
#include <wchar.h>

static void initial_folder(IFileOpenDialog *picker,HWND edit) {
    int length=GetWindowTextLengthW(edit);
    wchar_t *path=calloc((size_t)length+1,sizeof(*path));
    if(!path)return;
    GetWindowTextW(edit,path,length+1);
    for(wchar_t *p=path;*p;++p)if(*p==L'/')*p=L'\\';
    /* New project paths often do not exist yet. Start at the nearest existing
       parent instead of falling back to an unrelated remembered location. */
    while(*path){
        DWORD attributes=GetFileAttributesW(path);
        if(attributes!=INVALID_FILE_ATTRIBUTES && (attributes&FILE_ATTRIBUTE_DIRECTORY)){
            IShellItem *folder=NULL;
            if(SUCCEEDED(SHCreateItemFromParsingName(path,NULL,&IID_IShellItem,(void **)&folder))){
                IFileOpenDialog_SetFolder(picker,folder);IShellItem_Release(folder);
            }
            break;
        }
        wchar_t *slash=wcsrchr(path,L'\\');
        if(!slash)break;
        if(slash==path+2 && path[1]==L':' && slash[1])slash[1]=L'\0';
        else *slash=L'\0';
    }
    free(path);
}

bool sb_choose_directory(HWND owner,HWND edit,const wchar_t *title) {
    /* Shell dialogs require an STA on the calling UI thread. Balance even
       S_FALSE: a prior permissions dialog may already have initialized COM. */
    HRESULT apartment=CoInitializeEx(NULL,COINIT_APARTMENTTHREADED|COINIT_DISABLE_OLE1DDE);
    HRESULT result=apartment;IFileOpenDialog *picker=NULL;IShellItem *item=NULL;PWSTR path=NULL;
    bool chosen=false;
    if(FAILED(result))goto done;
    result=CoCreateInstance(&CLSID_FileOpenDialog,NULL,CLSCTX_INPROC_SERVER,&IID_IFileOpenDialog,(void **)&picker);
    if(FAILED(result))goto done;
    FILEOPENDIALOGOPTIONS options=0;
    result=IFileOpenDialog_GetOptions(picker,&options);
    if(FAILED(result))goto done;
    result=IFileOpenDialog_SetOptions(picker,options|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST|FOS_NOCHANGEDIR|FOS_DONTADDTORECENT);
    if(FAILED(result))goto done;
    result=IFileOpenDialog_SetTitle(picker,title);
    if(FAILED(result))goto done;
    result=IFileOpenDialog_SetOkButtonLabel(picker,L"选择此目录");
    if(FAILED(result))goto done;
    initial_folder(picker,edit);
    result=IFileOpenDialog_Show(picker,owner);
    if(FAILED(result))goto done;
    result=IFileOpenDialog_GetResult(picker,&item);
    if(FAILED(result))goto done;
    result=IShellItem_GetDisplayName(item,SIGDN_FILESYSPATH,&path);
    if(FAILED(result))goto done;
    if(SetWindowTextW(edit,path))chosen=true;else result=E_FAIL;
done:
    CoTaskMemFree(path);
    if(item)IShellItem_Release(item);
    if(picker)IFileOpenDialog_Release(picker);
    if(SUCCEEDED(apartment))CoUninitialize();
    if(FAILED(result) && result!=HRESULT_FROM_WIN32(ERROR_CANCELLED)){
        wchar_t message[240];
        swprintf(message,sizeof(message)/sizeof(*message),L"无法完成目录选择，请重试，或在目录输入框中填写完整路径。\n错误代码：0x%08lX",(unsigned long)result);
        MessageBoxW(owner,message,L"ShellBot 目录选择",MB_OK|MB_ICONERROR);
    }
    return chosen;
}
