#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdlib.h>
#include <wchar.h>

/* This GUI-subsystem companion only launches the adjacent product in resident
   mode. It never invokes a shell, requests elevation, or accepts command code. */
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE previous,PWSTR command,int show) {
    (void)instance;(void)previous;(void)command;(void)show;
    int argc=0;LPWSTR *argv=CommandLineToArgvW(GetCommandLineW(),&argc);int code=2;
    wchar_t image[32768];DWORD n=GetModuleFileNameW(NULL,image,32768);wchar_t *leaf=n && n<32768?wcsrchr(image,L'\\'):NULL,*line=NULL;
    if(!argv || argc!=3 || wcscmp(argv[1],L"--startup-profile") || !*argv[2] || wcschr(argv[2],L'"') ||
        wcslen(argv[2])<3 || argv[2][1]!=L':' || !leaf)goto done;
    wcscpy(leaf+1,L"WinShellBot.exe");size_t capacity=wcslen(image)+wcslen(argv[2])+64;if(capacity>=32767)goto done;
    line=calloc(capacity,sizeof(wchar_t));if(!line)goto done;
    swprintf(line,capacity,L"\"%ls\" --resident --startup-profile \"%ls\"",image,argv[2]);
    STARTUPINFOW startup={0};startup.cb=sizeof(startup);PROCESS_INFORMATION process={0};
    if(CreateProcessW(image,line,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,NULL,&startup,&process)) {
        CloseHandle(process.hThread);CloseHandle(process.hProcess);code=0;
    }
done:
    free(line);if(argv)LocalFree(argv);return code;
}
