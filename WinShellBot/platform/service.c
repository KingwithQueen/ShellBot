#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sddl.h>
#include <aclapi.h>
#include "startup.h"
#include "execution.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static wchar_t *wide(const char *s) {
    int n=s?MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s,-1,NULL,0):0;wchar_t *v=n?calloc((size_t)n,sizeof(wchar_t)):NULL;
    if(v && !MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s,-1,v,n)){free(v);v=NULL;}return v;
}
static wchar_t *command_line(const sb_startup *p) {
    wchar_t image[SB_PATH_CAP];DWORD n=GetModuleFileNameW(NULL,image,SB_PATH_CAP);wchar_t *path=wide(p->path),*out=NULL;
    size_t capacity=n+(path?wcslen(path):0)+64;
    if(n && n<SB_PATH_CAP && path && capacity<32767){out=calloc(capacity,sizeof(wchar_t));if(out)swprintf(out,capacity,L"\"%ls\" --service-run --startup-profile \"%ls\"",image,path);}
    free(path);return out;
}
static bool binding(SC_HANDLE service,const sb_startup *p) {
    DWORD size=0;QueryServiceConfigW(service,NULL,0,&size);QUERY_SERVICE_CONFIGW *config=size && size<65536?malloc(size):NULL;
    wchar_t *expected=command_line(p);bool ok=config && expected && QueryServiceConfigW(service,config,size,&size) &&
        config->dwServiceType==SERVICE_WIN32_OWN_PROCESS && !_wcsicmp(config->lpServiceStartName,SB_SERVICE_ACCOUNT) && !wcscmp(config->lpBinaryPathName,expected);
    free(config);free(expected);return ok;
}
static bool elevated(void) {
    HANDLE token=NULL;TOKEN_ELEVATION elevation={0};DWORD size=0;
    bool ok=OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token) && GetTokenInformation(token,TokenElevation,&elevation,sizeof(elevation),&size) && elevation.TokenIsElevated;
    if(token)CloseHandle(token);return ok;
}
static bool service_identity(void) {
    return sb_win_service_process();
}
/* Existing directories are accepted only if a prior install already gave this
   virtual service access through a protected private DACL. No conversion of an
   unrelated existing user Data directory or workspace ACL is performed. */
static bool existing_data(const wchar_t *path,PSID service) {
    PSECURITY_DESCRIPTOR descriptor=NULL;PSID owner=NULL;PACL acl=NULL;BYTE system[SECURITY_MAX_SID_SIZE];DWORD size=sizeof(system),revision=0;
    SECURITY_DESCRIPTOR_CONTROL control=0;bool found=false;
    bool ok=GetNamedSecurityInfoW(path,SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,&owner,NULL,&acl,NULL,&descriptor)==ERROR_SUCCESS &&
        acl && owner && CreateWellKnownSid(WinLocalSystemSid,NULL,system,&size) && GetSecurityDescriptorControl(descriptor,&control,&revision) && (control&SE_DACL_PROTECTED);
    for(DWORD i=0;ok && i<acl->AceCount;++i){ACCESS_ALLOWED_ACE *ace=NULL;
        ok=GetAce(acl,i,(void **)&ace) && ace->Header.AceType==ACCESS_ALLOWED_ACE_TYPE && !(ace->Header.AceFlags&INHERITED_ACE) &&
            (EqualSid(&ace->SidStart,owner) || EqualSid(&ace->SidStart,system) || EqualSid(&ace->SidStart,service));
        if(ok && EqualSid(&ace->SidStart,service) && (ace->Mask&FILE_ALL_ACCESS)==FILE_ALL_ACCESS)found=true;
    }
    if(descriptor)LocalFree(descriptor);return ok && found;
}
static bool provision_data(sb_startup *p,sb_error *error) {
    BYTE sid[SECURITY_MAX_SID_SIZE];DWORD size;LPWSTR service_sid=NULL,owner_sid=NULL;PSECURITY_DESCRIPTOR descriptor=NULL,profile_security=NULL;PSID owner=NULL;
    bool ok=sb_startup_service_sid(sid,&size) && ConvertSidToStringSidW(sid,&service_sid) &&
        GetSecurityInfo(p->pin,SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION,&owner,NULL,NULL,NULL,&profile_security)==ERROR_SUCCESS && ConvertSidToStringSidW(owner,&owner_sid);
    if(ok){wchar_t text[768];swprintf(text,768,L"O:%lsD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;%ls)(A;OICI;FA;;;%ls)",owner_sid,owner_sid,service_sid);
        ok=ConvertStringSecurityDescriptorToSecurityDescriptorW(text,SDDL_REVISION_1,&descriptor,NULL)!=FALSE;}
    SECURITY_ATTRIBUTES attributes={sizeof(attributes),descriptor,FALSE};
    for(size_t i=0;ok && i<=p->registry.project_count;++i) {
        const char *data=i?p->registry.projects[i-1].data_dir:p->data;wchar_t *path=wide(data);DWORD flags=path?GetFileAttributesW(path):INVALID_FILE_ATTRIBUTES;
        if(flags!=INVALID_FILE_ATTRIBUTES)ok=(flags&FILE_ATTRIBUTE_DIRECTORY) && !(flags&FILE_ATTRIBUTE_REPARSE_POINT) && existing_data(path,sid);
        else ok=path && GetLastError()==ERROR_FILE_NOT_FOUND;
        /* Missing immediate parent is a configuration error, not implicit
           recursive directory creation or permission propagation. */
        free(path);
    }
    for(size_t i=0;ok && i<=p->registry.project_count;++i) {
        const char *data=i?p->registry.projects[i-1].data_dir:p->data;wchar_t *path=wide(data);
        if(GetFileAttributesW(path)==INVALID_FILE_ATTRIBUTES)ok=CreateDirectoryW(path,&attributes)!=FALSE;
        free(path);
    }
    if(descriptor)LocalFree(descriptor);if(profile_security)LocalFree(profile_security);if(service_sid)LocalFree(service_sid);if(owner_sid)LocalFree(owner_sid);
    if(!ok)sb_fail(error,"Service Data requires new directories with existing parents, or prior private service Data. Existing unrelated Data/ACLs were not converted; newly created directories are retained if installation fails.");return ok;
}
static SERVICE_STATUS_HANDLE status_handle;
static SERVICE_STATUS status;
static SRWLOCK status_lock=SRWLOCK_INIT;
static HANDLE stop_event;
static sb_startup *running_profile;
static void report(DWORD state,DWORD code) {
    AcquireSRWLockExclusive(&status_lock);status.dwServiceType=SERVICE_WIN32_OWN_PROCESS;status.dwCurrentState=state;
    status.dwControlsAccepted=state==SERVICE_RUNNING?SERVICE_ACCEPT_STOP|SERVICE_ACCEPT_SHUTDOWN:0;
    status.dwWin32ExitCode=code?ERROR_SERVICE_SPECIFIC_ERROR:NO_ERROR;status.dwServiceSpecificExitCode=code;
    status.dwWaitHint=(state==SERVICE_START_PENDING || state==SERVICE_STOP_PENDING)?20000:0;
    status.dwCheckPoint=status.dwWaitHint?status.dwCheckPoint+1:0;
    if(status_handle)SetServiceStatus(status_handle,&status);ReleaseSRWLockExclusive(&status_lock);
}
static DWORD WINAPI control(DWORD event,DWORD type,void *data,void *context) {
    (void)type;(void)data;(void)context;
    if(event==SERVICE_CONTROL_STOP || event==SERVICE_CONTROL_SHUTDOWN){SetEvent(stop_event);report(SERVICE_STOP_PENDING,0);return NO_ERROR;}
    if(event==SERVICE_CONTROL_INTERROGATE){AcquireSRWLockShared(&status_lock);SetServiceStatus(status_handle,&status);ReleaseSRWLockShared(&status_lock);return NO_ERROR;}
    return ERROR_CALL_NOT_IMPLEMENTED;
}
typedef struct {HANDLE ready;int code;} service_work;
static DWORD WINAPI run(void *parameter) {service_work *work=parameter;work->code=sb_startup_manager(running_profile,stop_event,work->ready);return (DWORD)work->code;}
static void WINAPI service_main(DWORD argc,LPWSTR *argv) {
    (void)argc;(void)argv;status_handle=RegisterServiceCtrlHandlerExW(SB_SERVICE_NAME,control,NULL);if(!status_handle)return;
    stop_event=CreateEventW(NULL,TRUE,FALSE,NULL);report(SERVICE_START_PENDING,0);
    service_work work={CreateEventW(NULL,TRUE,FALSE,NULL),2};HANDLE thread=stop_event && work.ready?CreateThread(NULL,0,run,&work,0,NULL):NULL;
    if(!thread){report(SERVICE_STOPPED,2);goto done;}
    bool ready=false;uint64_t deadline=sb_platform_millis()+180000;
    HANDLE events[]={work.ready,thread};
    while(!ready && WaitForSingleObject(thread,0)==WAIT_TIMEOUT && sb_platform_millis()<deadline) {
        DWORD result=WaitForMultipleObjects(2,events,FALSE,2000);if(result==WAIT_OBJECT_0)ready=true;else report(SERVICE_START_PENDING,0);
    }
    if(ready)report(SERVICE_RUNNING,0);else SetEvent(stop_event);
    while(WaitForSingleObject(thread,2000)==WAIT_TIMEOUT) {
        if(WaitForSingleObject(stop_event,0)==WAIT_OBJECT_0){report(SERVICE_STOP_PENDING,0);break;}
    }
    if(WaitForSingleObject(thread,30000)!=WAIT_OBJECT_0)ExitProcess(3);
    report(SERVICE_STOPPED,ready?(DWORD)work.code:2);CloseHandle(thread);
done:
    if(work.ready)CloseHandle(work.ready);if(stop_event)CloseHandle(stop_event);stop_event=NULL;
}
static const char *state_name(DWORD state) {
    return state==SERVICE_RUNNING?"running":state==SERVICE_STOPPED?"stopped":state==SERVICE_START_PENDING?"starting":state==SERVICE_STOP_PENDING?"stopping":"other";
}
int sb_service_cli(const char *mode,sb_startup *p,sb_error *error) {
    if(p && !p->service){sb_fail(error,"Windows service commands require a service startup profile.");return 2;}
    if(!strcmp(mode,"--service-run")) {
        if(!service_identity()){sb_fail(error,"Service runtime requires its own virtual service identity in Session 0; use --startup-console only for foreground diagnostics.");return 2;}
        running_profile=p;SERVICE_TABLE_ENTRYW entries[]={{(LPWSTR)SB_SERVICE_NAME,service_main},{NULL,NULL}};
        if(!StartServiceCtrlDispatcherW(entries)){sb_fail(error,"Service runtime must be launched by the Windows service controller.");return 2;}return 0;
    }
    bool install=!strcmp(mode,"--service-install"),query=!strcmp(mode,"--service-status");
    if(install && !elevated()){sb_fail(error,"Installing the Windows service requires an elevated owner terminal; no UAC is requested automatically.");return 2;}
    SC_HANDLE scm=OpenSCManagerW(NULL,NULL,SC_MANAGER_CONNECT|(install?SC_MANAGER_CREATE_SERVICE:0)),service=NULL;int code=2;bool created=false;wchar_t *command=NULL;
    if(!scm)goto done;
    DWORD access=SERVICE_QUERY_CONFIG|SERVICE_QUERY_STATUS;
    if(install)access|=SERVICE_CHANGE_CONFIG|DELETE;
    else if(!strcmp(mode,"--service-start"))access|=SERVICE_START;
    else if(!strcmp(mode,"--service-stop"))access|=SERVICE_STOP;
    else if(!strcmp(mode,"--service-uninstall"))access|=DELETE;
    service=OpenServiceW(scm,SB_SERVICE_NAME,access);
    if(!service && GetLastError()==ERROR_SERVICE_DOES_NOT_EXIST && query){puts("{\"service_installed\":false}");code=0;goto done;}
    if(!service && install && GetLastError()==ERROR_SERVICE_DOES_NOT_EXIST) {
        command=command_line(p);if(!command)goto done;
        service=CreateServiceW(scm,SB_SERVICE_NAME,L"ShellBot 后台服务",access,SERVICE_WIN32_OWN_PROCESS,SERVICE_AUTO_START,SERVICE_ERROR_NORMAL,
            command,NULL,NULL,NULL,SB_SERVICE_ACCOUNT,NULL);created=service!=NULL;
    }
    if(!service)goto done;
    if(p && !binding(service,p)){sb_fail(error,"Service name, executable, profile or account does not match; no existing service was modified.");goto done;}
    SERVICE_STATUS_PROCESS current={0};DWORD bytes=0;
    if(!QueryServiceStatusEx(service,SC_STATUS_PROCESS_INFO,(BYTE *)&current,sizeof(current),&bytes))goto done;
    if(query){
        DWORD needed=0;QueryServiceConfigW(service,NULL,0,&needed);QUERY_SERVICE_CONFIGW *config=needed && needed<65536?malloc(needed):NULL;
        SERVICE_DELAYED_AUTO_START_INFO delayed={0};
        bool inspected=config && QueryServiceConfigW(service,config,needed,&needed) && QueryServiceConfig2W(service,SERVICE_CONFIG_DELAYED_AUTO_START_INFO,(BYTE *)&delayed,sizeof(delayed),&needed);
        if(!inspected){free(config);goto done;}
        printf("{\"service_installed\":true,\"state\":\"%s\",\"process_id\":%lu,\"exit_code\":%lu,\"service_exit_code\":%lu,\"owned_binding\":%s,\"automatic_start\":%s,\"delayed_start\":%s,\"virtual_service_account\":%s}\n",
            state_name(current.dwCurrentState),current.dwProcessId,current.dwWin32ExitCode,current.dwServiceSpecificExitCode,p?"true":"false",
            config->dwStartType==SERVICE_AUTO_START?"true":"false",delayed.fDelayedAutostart?"true":"false",!_wcsicmp(config->lpServiceStartName,SB_SERVICE_ACCOUNT)?"true":"false");
        free(config);code=0;goto done;
    }
    if(install) {
        SERVICE_DELAYED_AUTO_START_INFO desired_delay={TRUE};
        if(!ChangeServiceConfigW(service,SERVICE_NO_CHANGE,SERVICE_AUTO_START,SERVICE_NO_CHANGE,NULL,NULL,NULL,NULL,NULL,NULL,NULL) ||
            !ChangeServiceConfig2W(service,SERVICE_CONFIG_DELAYED_AUTO_START_INFO,&desired_delay))goto done;
        if(created) {
            SERVICE_DELAYED_AUTO_START_INFO delayed={TRUE};SERVICE_SID_INFO sid={SERVICE_SID_TYPE_UNRESTRICTED};
            SERVICE_DESCRIPTIONW description={L"未登录时提供授权项目 MCP。独立虚拟账户；不开放桌面或自动提权。停止不重放旧任务。"};
            if(!ChangeServiceConfig2W(service,SERVICE_CONFIG_DELAYED_AUTO_START_INFO,&delayed) || !ChangeServiceConfig2W(service,SERVICE_CONFIG_SERVICE_SID_INFO,&sid) ||
                !ChangeServiceConfig2W(service,SERVICE_CONFIG_DESCRIPTION,&description) || !provision_data(p,error) || !sb_startup_grant_profile(p,error))goto done;
        }
        puts("{\"service_installed\":true,\"automatic_start\":true,\"delayed_start\":true,\"virtual_service_account\":true,\"start_requested\":false}");code=0;
    } else if(!strcmp(mode,"--service-start")) {
        if(current.dwCurrentState!=SERVICE_RUNNING && !StartServiceW(service,0,NULL))goto done;
        puts("{\"start_requested\":true,\"running_verified\":false}");code=0;
    } else if(!strcmp(mode,"--service-stop")) {
        SERVICE_STATUS result={0};
        if(current.dwCurrentState!=SERVICE_STOPPED && current.dwCurrentState!=SERVICE_STOP_PENDING && !ControlService(service,SERVICE_CONTROL_STOP,&result))goto done;
        uint64_t deadline=sb_platform_millis()+30000;
        while(current.dwCurrentState!=SERVICE_STOPPED && sb_platform_millis()<deadline){Sleep(100);if(!QueryServiceStatusEx(service,SC_STATUS_PROCESS_INFO,(BYTE *)&current,sizeof(current),&bytes))goto done;}
        if(current.dwCurrentState!=SERVICE_STOPPED){sb_fail(error,"Service has not finished stopping; no files or registration were removed.");goto done;}
        puts("{\"service_stopped\":true}");code=0;
    } else if(!strcmp(mode,"--service-uninstall")) {
        if(current.dwCurrentState!=SERVICE_STOPPED){sb_fail(error,"Stop this service and verify it is stopped before uninstalling; Data is retained.");goto done;}
        if(DeleteService(service)){puts("{\"service_removed\":true,\"data_preserved\":true,\"profile_preserved\":true}");code=0;}
    }
done:
    if(code && created && service)DeleteService(service);
    if(service)CloseServiceHandle(service);if(scm)CloseServiceHandle(scm);free(command);
    if(code && !*error->text)sb_fail(error,"Windows service operation failed; inspect permissions and current service state.");return code;
}
