#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdlib.h>
#include <wchar.h>

/* GUI entry points launch only the adjacent product with a fixed native mode.
   No shell, elevation request or executable selected by caller input. */
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE previous,PWSTR command,int show) {
    (void)instance;(void)previous;(void)command;(void)show;
    int argc=0;LPWSTR *argv=CommandLineToArgvW(GetCommandLineW(),&argc);int code=2;
    wchar_t image[32768];DWORD n=GetModuleFileNameW(NULL,image,32768);wchar_t *leaf=n && n<32768?wcsrchr(image,L'\\'):NULL,*line=NULL;
    if(!argv || !leaf)goto done;
    const wchar_t *mode=NULL,*path=NULL;
    if(argc==1)mode=L"--desktop";
    else if(argc==3 && !wcscmp(argv[1],L"--app-data-dir")) { mode=L"--desktop --app-data-dir";path=argv[2]; }
    else if(argc==3 && !wcscmp(argv[1],L"--startup-profile")) { mode=L"--resident --startup-profile";path=argv[2]; }
    else if(argc==2 && !wcscmp(argv[1],L"--project-settings"))mode=L"--project-settings";
    else if(argc==4 && !wcscmp(argv[1],L"--project-settings") && !wcscmp(argv[2],L"--projects")) { mode=L"--project-settings --projects";path=argv[3]; }
    if(!mode || (path && (!*path || wcschr(path,L'"') || wcslen(path)<3 || path[1]!=L':' || (path[2]!=L'\\' && path[2]!=L'/') ||
        path[wcslen(path)-1]==L'\\' || path[wcslen(path)-1]==L'/')))goto done;
    wcscpy(leaf+1,L"WinShellBot.exe");size_t capacity=wcslen(image)+(path?wcslen(path):0)+96;if(capacity>=32767)goto done;
    line=calloc(capacity,sizeof(wchar_t));if(!line)goto done;
    if(path)swprintf(line,capacity,L"\"%ls\" %ls \"%ls\"",image,mode,path);
    else swprintf(line,capacity,L"\"%ls\" %ls",image,mode);
    STARTUPINFOW startup={0};startup.cb=sizeof(startup);PROCESS_INFORMATION process={0};
    if(CreateProcessW(image,line,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,NULL,&startup,&process)) {
        CloseHandle(process.hThread);CloseHandle(process.hProcess);code=0;
    } else MessageBoxW(NULL,L"无法启动相邻的 WinShellBot.exe，请完整解压程序包。",L"ShellBot",MB_OK|MB_ICONERROR);
done:
    free(line);if(argv)LocalFree(argv);return code;
}
