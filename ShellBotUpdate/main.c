#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <objbase.h>
#include <stdio.h>
#include <wchar.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

/* 控制台使用 Unicode，重定向输出使用 UTF-8，中文安装路径也能完整返回 JOB。 */
static void write_text(DWORD stream,const wchar_t *text) {
    HANDLE handle=GetStdHandle(stream);DWORD mode=0,written=0;
    if(GetConsoleMode(handle,&mode)){WriteConsoleW(handle,text,(DWORD)wcslen(text),&written,NULL);return;}
    int count=WideCharToMultiByte(CP_UTF8,0,text,-1,NULL,0,NULL,NULL);char *bytes=count>0?malloc((size_t)count):NULL;
    if(bytes){WideCharToMultiByte(CP_UTF8,0,text,-1,bytes,count,NULL,NULL);WriteFile(handle,bytes,(DWORD)(count-1),&written,NULL);free(bytes);}
}

/* 独立更新器只接受本地包，安装位置固定为自身目录。 */
int wmain(int argc,wchar_t **argv) {
    bool quiet=false;const wchar_t *package=NULL;
    if(argc==3 && !wcscmp(argv[1],L"--quiet")){quiet=true;package=argv[2];}
    else if(argc==2)package=argv[1];else if(argc!=1)return 2;
    wchar_t root[32768],core[32768],zip[32768],job[32768],script[32768],powershell[32768];
    const wchar_t *error=L"无法初始化更新器。";DWORD n=GetModuleFileNameW(NULL,root,32768);wchar_t *leaf=n && n<32768?wcsrchr(root,L'\\'):NULL;
    if(!leaf)goto failed;if(leaf==root+2){error=L"请将 ShellBot 安装在独立文件夹中。";goto failed;}*leaf=0;
    if(wcslen(root)>30000){error=L"安装目录过长。";goto failed;}
    swprintf(core,32768,L"%ls\\WinShellBot.exe",root);
    DWORD attr=GetFileAttributesW(core);if(attr==INVALID_FILE_ATTRIBUTES || (attr&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))){error=L"找不到核心程序 WinShellBot.exe，请将 ShellBotUpdate.exe 放在 ShellBot 根目录。";goto failed;}
    if(!package){OPENFILENAMEW pick={0};zip[0]=0;pick.lStructSize=sizeof(pick);pick.lpstrFilter=L"ShellBot ZIP 安装包\0*.zip\0\0";pick.lpstrFile=zip;pick.nMaxFile=32768;pick.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;if(!GetOpenFileNameW(&pick))return 1;package=zip;}
    else if(!GetFullPathNameW(package,32768,zip,NULL)){error=L"无法解析安装包路径。";goto failed;}
    if(wcslen(zip)>30000 || !wcsrchr(zip,L'.') || _wcsicmp(wcsrchr(zip,L'.'),L".zip") || GetFileAttributesW(zip)==INVALID_FILE_ATTRIBUTES){error=L"请选择存在的本地 ZIP 安装包。";goto failed;}
    swprintf(job,32768,L"%ls\\.shellbot-update",root);attr=GetFileAttributesW(job);
    if(attr!=INVALID_FILE_ATTRIBUTES && (attr&FILE_ATTRIBUTE_REPARSE_POINT)){error=L"更新缓存目录不能是链接。";goto failed;}
    if(!CreateDirectoryW(job,NULL) && GetLastError()!=ERROR_ALREADY_EXISTS)goto failed;
    GUID id;wchar_t nonce[40];if(FAILED(CoCreateGuid(&id)) || !StringFromGUID2(&id,nonce,40))goto failed;
    swprintf(job,32768,L"%ls\\.shellbot-update\\job-%ls",root,nonce);if(!CreateDirectoryW(job,NULL))goto failed;
    swprintf(script,32768,L"%ls\\update.ps1",job);
    HRSRC resource=FindResourceW(NULL,MAKEINTRESOURCEW(101),RT_RCDATA);HGLOBAL loaded=resource?LoadResource(NULL,resource):NULL;DWORD size=resource?SizeofResource(NULL,resource):0;const void *bytes=loaded?LockResource(loaded):NULL;
    if(bytes && size>=3 && !memcmp(bytes,"\xef\xbb\xbf",3)){bytes=(const unsigned char*)bytes+3;size-=3;}
    HANDLE file=CreateFileW(script,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);DWORD written=0;const unsigned char bom[]={0xef,0xbb,0xbf};
    bool ok=file!=INVALID_HANDLE_VALUE && bytes && WriteFile(file,bom,3,&written,NULL) && written==3 && WriteFile(file,bytes,size,&written,NULL) && written==size;
    if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);if(!ok)goto failed;
    n=GetSystemDirectoryW(powershell,32768);if(!n || n>32000)goto failed;wcscat(powershell,L"\\WindowsPowerShell\\v1.0\\powershell.exe");
    size_t cap=wcslen(script)+wcslen(root)+wcslen(zip)+wcslen(job)+wcslen(powershell)+256;
    if(cap>=32767){error=L"更新参数总长度超过 Windows 容量。";goto failed;}
    wchar_t *line=calloc(cap,sizeof(wchar_t));if(!line)goto failed;
    swprintf(line,cap,L"\"%ls\" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"%ls\" -Root \"%ls\" -Package \"%ls\" -Job \"%ls\" -ParentId %lu %ls",powershell,script,root,zip,job,(unsigned long)GetCurrentProcessId(),quiet?L"-Quiet":L"");
    STARTUPINFOW start={0};PROCESS_INFORMATION process={0};start.cb=sizeof(start);
    ok=CreateProcessW(powershell,line,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,root,&start,&process)!=FALSE;free(line);
    if(!ok)goto failed;CloseHandle(process.hThread);CloseHandle(process.hProcess);
    write_text(STD_OUTPUT_HANDLE,L"JOB=");write_text(STD_OUTPUT_HANDLE,job);write_text(STD_OUTPUT_HANDLE,L"\n");return 0;
failed:
    write_text(STD_ERROR_HANDLE,L"ShellBotUpdate: ");write_text(STD_ERROR_HANDLE,error);write_text(STD_ERROR_HANDLE,L"\n");if(!quiet)MessageBoxW(NULL,error,L"ShellBotUpdate",MB_OK|MB_ICONERROR);return 2;
}
