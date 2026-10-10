/* Test-only child: performs fixed operations in an isolated test workspace. */
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <string.h>
#include <fcntl.h>
#include <io.h>
#include <winsock2.h>

static void json_string(const char *value) {
    const unsigned char *p = (const unsigned char *)value;
    putchar('"');
    while (*p) {
        if (*p == '"' || *p == '\\') { putchar('\\'); putchar(*p); }
        else if (*p < 32) { printf("\\u%04x", (unsigned int)*p); }
        else { putchar(*p); }
        ++p;
    }
    putchar('"');
}

static int identity(void) {
    HANDLE token = NULL;
    TOKEN_ELEVATION elevation;
    DWORD returned = 0, capacity = GetCurrentDirectoryW(0, NULL);
    wchar_t *directory = NULL;
    char *utf8 = NULL;
    int length, result = 2;
    if (!capacity || !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return 2;
    if (!GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &returned)) goto done;
    directory = calloc(capacity, sizeof(*directory));
    if (!directory || !GetCurrentDirectoryW(capacity, directory)) goto done;
    length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, directory, -1, NULL, 0, NULL, NULL);
    if (length <= 0) goto done;
    utf8 = malloc((size_t)length);
    if (!utf8 || !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, directory, -1, utf8, length, NULL, NULL)) goto done;
    DWORD ac=0,lpac=0;
    GetTokenInformation(token,TokenIsAppContainer,&ac,sizeof(ac),&returned);
    BOOL lpac_supported=GetTokenInformation(token,TokenIsLessPrivilegedAppContainer,&lpac,sizeof(lpac),&returned);
    printf("{\"appcontainer\":%s,\"lpac_query_supported\":%s,\"lpac_query_result\":%s,\"elevated\":%s,\"pid\":%lu,\"token_inherited\":%s,\"project_tokens_inherited\":%s,\"canary_inherited\":%s,\"cwd\":",
           ac?"true":"false",lpac_supported?"true":"false",lpac?"true":"false",
           elevation.TokenIsElevated ? "true" : "false", GetCurrentProcessId(),
           GetEnvironmentVariableW(L"SHELLBOT_TOKEN", NULL, 0) ? "true" : "false",
           (GetEnvironmentVariableW(L"SHELLBOT_A_TOKEN", NULL, 0) || GetEnvironmentVariableW(L"SHELLBOT_B_TOKEN", NULL, 0)) ? "true" : "false",
           GetEnvironmentVariableW(L"SHELLBOT_M2_CANARY", NULL, 0) ? "true" : "false");
    json_string(utf8);
    puts("}");
    result = ferror(stdout) ? 3 : 0;
done:
    free(utf8);
    free(directory);
    CloseHandle(token);
    return result;
}

static BOOL open_test(const wchar_t *path,DWORD access,DWORD flags) {
    HANDLE file=CreateFileW(path,access,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,flags,NULL);
    if(file==INVALID_HANDLE_VALUE)return FALSE;CloseHandle(file);return TRUE;
}
static int sandbox(int argc,wchar_t **argv) {
    if(argc!=8)return 2;
    BOOL outside=open_test(argv[2],GENERIC_READ,0),readonly_write=open_test(argv[3],GENERIC_WRITE,0),private_read=open_test(argv[4],GENERIC_READ,0);
    BOOL dac=open_test(argv[4],WRITE_DAC,0);
    HANDLE pipe=CreateFileW(argv[5],GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_EXISTING,SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,NULL);
    BOOL owner=pipe!=INVALID_HANDLE_VALUE;if(owner)CloseHandle(pipe);
    HANDLE manager=OpenProcess(PROCESS_VM_READ|PROCESS_VM_WRITE|PROCESS_DUP_HANDLE,FALSE,wcstoul(argv[6],NULL,10));
    BOOL injection=manager!=NULL;if(manager)CloseHandle(manager);
    HKEY key=NULL;BOOL registry=RegOpenKeyExW(HKEY_CURRENT_USER,L"Software",0,KEY_READ,&key)==ERROR_SUCCESS;if(registry)RegCloseKey(key);
    WSADATA ws;BOOL network=FALSE;
    if(!WSAStartup(MAKEWORD(2,2),&ws)){SOCKET s=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);struct sockaddr_in a={0};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);a.sin_port=htons((u_short)wcstoul(argv[7],NULL,10));network=connect(s,(struct sockaddr *)&a,sizeof(a))==0;closesocket(s);WSACleanup();}
    wchar_t executable[32768],command[32768];GetModuleFileNameW(NULL,executable,32768);swprintf(command,32768,L"\"%ls\" identity",executable);
    STARTUPINFOW si={0};si.cb=sizeof(si);PROCESS_INFORMATION child={0};BOOL children=CreateProcessW(executable,command,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,NULL,&si,&child);
    if(children){if(WaitForSingleObject(child.hProcess,3000)!=WAIT_OBJECT_0)TerminateProcess(child.hProcess,125);CloseHandle(child.hThread);CloseHandle(child.hProcess);}
    PROCESS_MITIGATION_SYSTEM_CALL_DISABLE_POLICY mitigation={0};GetProcessMitigationPolicy(GetCurrentProcess(),ProcessSystemCallDisablePolicy,&mitigation,sizeof(mitigation));
    printf("{\"outside_read\":%s,\"readonly_write\":%s,\"private_read\":%s,\"change_acl\":%s,\"owner_pipe\":%s,\"process_injection\":%s,\"registry_read\":%s,\"network\":%s,\"children\":%s,\"win32k_disabled\":%s}",
        outside?"true":"false",readonly_write?"true":"false",private_read?"true":"false",dac?"true":"false",owner?"true":"false",injection?"true":"false",registry?"true":"false",network?"true":"false",children?"true":"false",mitigation.DisallowWin32kSystemCalls?"true":"false");return 0;
}

int wmain(int argc, wchar_t **argv) {
    if(argc>=2 && !wcscmp(argv[1],L"sandbox"))return sandbox(argc,argv);
    if (argc != 2) return 2;
    if (_setmode(_fileno(stdin), _O_BINARY) == -1 || _setmode(_fileno(stdout), _O_BINARY) == -1) return 2;
    if (!wcscmp(argv[1], L"identity")) return identity();
    if (!wcscmp(argv[1], L"append")) {
        HANDLE file = CreateFileW(L"once.txt", FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD written = 0;
        BOOL ok;
        if (file == INVALID_HANDLE_VALUE) return 2;
        ok = WriteFile(file, "ONE", 3, &written, NULL);
        CloseHandle(file);
        if (!ok || written != 3) return 3;
        return fwrite("done", 1, 4, stdout) == 4 ? 0 : 3;
    }
    if (!wcscmp(argv[1], L"stdin")) {
        char buffer[4096];
        size_t length;
        while ((length = fread(buffer, 1, sizeof(buffer), stdin)) != 0) {
            if (fwrite(buffer, 1, length, stdout) != length) return 3;
        }
        return ferror(stdin) || ferror(stdout) ? 3 : 0;
    }
    if (!wcscmp(argv[1], L"escaped")) {
        char bytes[16384];
        memset(bytes, 1, sizeof(bytes));
        return fwrite(bytes, 1, sizeof(bytes), stdout) == sizeof(bytes) ? 0 : 3;
    }
    if (!wcscmp(argv[1], L"wait")) { Sleep(60000); return 0; }
    return 2;
}
