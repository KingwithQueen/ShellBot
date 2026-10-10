/* Internal activation probe. Never executes client instructions. */
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <wchar.h>
#include <stdlib.h>

static BOOL access_path(const wchar_t *path, DWORD access, BOOL directory) {
    HANDLE file=CreateFileW(path,access,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,
        directory?FILE_FLAG_BACKUP_SEMANTICS:0,NULL);
    if(file==INVALID_HANDLE_VALUE)return FALSE;CloseHandle(file);return TRUE;
}
int wmain(int argc,wchar_t **argv) {
    if(argc!=7)return 2;
    HANDLE token=NULL;DWORD container=0,bytes=0;TOKEN_ELEVATION elevation={0};
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token) ||
        !GetTokenInformation(token,TokenIsAppContainer,&container,sizeof(container),&bytes) || !container ||
        !GetTokenInformation(token,TokenElevation,&elevation,sizeof(elevation),&bytes) || elevation.TokenIsElevated)return 3;
    CloseHandle(token);
    PROCESS_MITIGATION_SYSTEM_CALL_DISABLE_POLICY policy={0};
    if(!GetProcessMitigationPolicy(GetCurrentProcess(),ProcessSystemCallDisablePolicy,&policy,sizeof(policy)) || !policy.DisallowWin32kSystemCalls)return 4;
    BOOL writable=wcstoul(argv[2],NULL,10)!=0;
    if(!access_path(argv[1],FILE_LIST_DIRECTORY,TRUE) || access_path(argv[1],FILE_ADD_FILE,TRUE)!=writable)return 5;
    if(access_path(argv[3],GENERIC_READ,FALSE) || GetLastError()!=ERROR_ACCESS_DENIED)return 6;
    if(access_path(argv[1],WRITE_DAC,TRUE) || access_path(argv[1],WRITE_OWNER,TRUE))return 7;
    if(*argv[4] && (!access_path(argv[4],FILE_LIST_DIRECTORY,TRUE) || access_path(argv[4],FILE_ADD_FILE,TRUE)))return 8;
    HKEY registry=NULL;LONG r=RegOpenKeyExW(HKEY_CURRENT_USER,L"Software",0,KEY_READ,&registry);
    if(r==ERROR_SUCCESS){RegCloseKey(registry);return 9;}
    HANDLE parent=OpenProcess(PROCESS_VM_READ|PROCESS_VM_WRITE|PROCESS_DUP_HANDLE,FALSE,wcstoul(argv[5],NULL,10));
    if(parent){CloseHandle(parent);return 10;}
    wchar_t executable[32768],command[32768];GetModuleFileNameW(NULL,executable,32768);swprintf(command,32768,L"\"%ls\"",executable);
    STARTUPINFOW startup={0};startup.cb=sizeof(startup);PROCESS_INFORMATION child={0};
    if(CreateProcessW(executable,command,NULL,NULL,FALSE,CREATE_SUSPENDED|DETACHED_PROCESS,NULL,NULL,&startup,&child)) {
        TerminateProcess(child.hProcess,125);CloseHandle(child.hThread);CloseHandle(child.hProcess);return 13;
    }
    /* The parent has a working loopback listener. Zero-capability LPAC may
       reject Winsock initialization itself; granting registry/network caps
       merely to make a negative probe initialize would widen the boundary. */
    WSADATA network;if(WSAStartup(MAKEWORD(2,2),&network))return 0;
    SOCKET socket_handle=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);struct sockaddr_in address={0};
    address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=htons((u_short)wcstoul(argv[6],NULL,10));
    int connected=connect(socket_handle,(struct sockaddr *)&address,sizeof(address)),error=WSAGetLastError();
    closesocket(socket_handle);WSACleanup();
    if(connected==0 || error!=WSAEACCES)return 12;
    return 0;
}
