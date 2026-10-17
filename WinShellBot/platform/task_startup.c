#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <taskschd.h>
#include <sddl.h>
#include <oleauto.h>
#include "startup.h"
#include "execution.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef struct { BSTR task,user,image,arguments,directory,description; } expected_task;
static bool same_user(BSTR actual,BSTR expected) {
    if(!actual || !*actual)return false;
    if(!_wcsicmp(actual,expected))return true;
    BYTE sid[SECURITY_MAX_SID_SIZE];DWORD size=sizeof(sid),domain_size=256;wchar_t domain[256];SID_NAME_USE use;PSID wanted=NULL;
    bool ok=LookupAccountNameW(NULL,actual,sid,&size,domain,&domain_size,&use) &&
        ConvertStringSidToSidW(expected,&wanted) && EqualSid(sid,wanted);
    if(wanted)LocalFree(wanted);return ok;
}
static void expected_free(expected_task *e) {
    SysFreeString(e->task);SysFreeString(e->user);SysFreeString(e->image);
    SysFreeString(e->arguments);SysFreeString(e->directory);SysFreeString(e->description);
}
static bool expected_new(sb_startup *profile,expected_task *e,sb_error *error) {
    HANDLE token=NULL;DWORD size=0;TOKEN_USER *user=NULL;LPWSTR sid=NULL;
    wchar_t image[SB_PATH_CAP],task[80],*path=NULL;char hash[65];bool ok=false;
    DWORD n=GetModuleFileNameW(NULL,image,SB_PATH_CAP);
    if(!n || n>=SB_PATH_CAP || !sb_platform_sha256(profile->path,strlen(profile->path),hash,error) ||
        !OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))goto done;
    GetTokenInformation(token,TokenUser,NULL,0,&size);user=malloc(size);
    if(!user || !GetTokenInformation(token,TokenUser,user,size,&size) || !ConvertSidToStringSidW(user->User.Sid,&sid))goto done;
    wchar_t *leaf=wcsrchr(image,L'\\');if(!leaf)goto done;
    wcscpy(leaf+1,L"ShellBot-Launcher.exe");e->image=SysAllocString(image);*leaf=0;e->directory=SysAllocString(image);
    int chars=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,profile->path,-1,NULL,0);
    path=chars?calloc((size_t)chars,sizeof(wchar_t)):NULL;
    if(!path || !MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,profile->path,-1,path,chars))goto done;
    size_t capacity=wcslen(path)+48;wchar_t *arguments=calloc(capacity,sizeof(wchar_t));if(!arguments)goto done;
    swprintf(arguments,capacity,L"--startup-profile \"%ls\"",path);e->arguments=SysAllocString(arguments);free(arguments);
    swprintf(task,80,L"ShellBot-Admin-%.24hs",hash);e->task=SysAllocString(task);e->user=SysAllocString(sid);
    e->description=SysAllocString(L"ShellBot managed elevated logon v1");
    ok=e->task && e->user && e->image && e->directory && e->arguments && e->description;
done:
    free(path);free(user);if(sid)LocalFree(sid);if(token)CloseHandle(token);return ok;
}
static bool text_equal(BSTR actual,BSTR expected,bool insensitive) {
    return actual && expected && (insensitive?!_wcsicmp(actual,expected):!wcscmp(actual,expected));
}
static bool binding(IRegisteredTask *task,const expected_task *e) {
    ITaskDefinition *definition=NULL;IPrincipal *principal=NULL;IActionCollection *actions=NULL;IAction *action=NULL;
    IExecAction *exec=NULL;ITriggerCollection *triggers=NULL;ITrigger *trigger=NULL;ILogonTrigger *logon=NULL;ITaskSettings *settings=NULL;
    BSTR user=NULL,trigger_user=NULL,path=NULL,arguments=NULL,directory=NULL,limit=NULL,interval=NULL;
    TASK_RUNLEVEL_TYPE level=TASK_RUNLEVEL_LUA;TASK_LOGON_TYPE type=TASK_LOGON_NONE;TASK_ACTION_TYPE action_type=TASK_ACTION_COM_HANDLER;
    TASK_TRIGGER_TYPE2 trigger_type=TASK_TRIGGER_EVENT;TASK_INSTANCES_POLICY instances=TASK_INSTANCES_PARALLEL;
    LONG action_count=0,trigger_count=0;INT restarts=0;VARIANT_BOOL trigger_enabled=VARIANT_FALSE,available=VARIANT_FALSE,battery=VARIANT_TRUE,stop_battery=VARIANT_TRUE,demand=VARIANT_FALSE,hard=VARIANT_TRUE;
    bool ok=SUCCEEDED(IRegisteredTask_get_Definition(task,&definition)) && SUCCEEDED(ITaskDefinition_get_Principal(definition,&principal)) &&
        SUCCEEDED(IPrincipal_get_UserId(principal,&user)) && same_user(user,e->user) &&
        SUCCEEDED(IPrincipal_get_LogonType(principal,&type)) && type==TASK_LOGON_INTERACTIVE_TOKEN &&
        SUCCEEDED(IPrincipal_get_RunLevel(principal,&level)) && level==TASK_RUNLEVEL_HIGHEST &&
        SUCCEEDED(ITaskDefinition_get_Actions(definition,&actions)) && SUCCEEDED(IActionCollection_get_Count(actions,&action_count)) && action_count==1 &&
        SUCCEEDED(IActionCollection_get_Item(actions,1,&action)) && SUCCEEDED(IAction_get_Type(action,&action_type)) && action_type==TASK_ACTION_EXEC &&
        SUCCEEDED(IAction_QueryInterface(action,&IID_IExecAction,(void **)&exec)) &&
        SUCCEEDED(IExecAction_get_Path(exec,&path)) && text_equal(path,e->image,true) &&
        SUCCEEDED(IExecAction_get_Arguments(exec,&arguments)) && text_equal(arguments,e->arguments,false) &&
        SUCCEEDED(IExecAction_get_WorkingDirectory(exec,&directory)) && text_equal(directory,e->directory,true) &&
        SUCCEEDED(ITaskDefinition_get_Triggers(definition,&triggers)) && SUCCEEDED(ITriggerCollection_get_Count(triggers,&trigger_count)) && trigger_count==1 &&
        SUCCEEDED(ITriggerCollection_get_Item(triggers,1,&trigger)) && SUCCEEDED(ITrigger_get_Type(trigger,&trigger_type)) && trigger_type==TASK_TRIGGER_LOGON &&
        SUCCEEDED(ITrigger_get_Enabled(trigger,&trigger_enabled)) && trigger_enabled==VARIANT_TRUE &&
        SUCCEEDED(ITrigger_QueryInterface(trigger,&IID_ILogonTrigger,(void **)&logon)) &&
        SUCCEEDED(ILogonTrigger_get_UserId(logon,&trigger_user)) && same_user(trigger_user,e->user) &&
        SUCCEEDED(ITaskDefinition_get_Settings(definition,&settings)) &&
        SUCCEEDED(ITaskSettings_get_MultipleInstances(settings,&instances)) && instances==TASK_INSTANCES_IGNORE_NEW &&
        SUCCEEDED(ITaskSettings_get_ExecutionTimeLimit(settings,&limit)) && limit && !wcscmp(limit,L"PT0S") &&
        SUCCEEDED(ITaskSettings_get_RestartCount(settings,&restarts)) && restarts==3 &&
        SUCCEEDED(ITaskSettings_get_RestartInterval(settings,&interval)) && interval && !wcscmp(interval,L"PT1M") &&
        SUCCEEDED(ITaskSettings_get_StartWhenAvailable(settings,&available)) && available==VARIANT_TRUE &&
        SUCCEEDED(ITaskSettings_get_DisallowStartIfOnBatteries(settings,&battery)) && battery==VARIANT_FALSE &&
        SUCCEEDED(ITaskSettings_get_StopIfGoingOnBatteries(settings,&stop_battery)) && stop_battery==VARIANT_FALSE &&
        SUCCEEDED(ITaskSettings_get_AllowDemandStart(settings,&demand)) && demand==VARIANT_TRUE &&
        SUCCEEDED(ITaskSettings_get_AllowHardTerminate(settings,&hard)) && hard==VARIANT_FALSE;
    SysFreeString(user);SysFreeString(trigger_user);SysFreeString(path);SysFreeString(arguments);SysFreeString(directory);SysFreeString(limit);SysFreeString(interval);
    if(settings)ITaskSettings_Release(settings);if(logon)ILogonTrigger_Release(logon);if(trigger)ITrigger_Release(trigger);if(triggers)ITriggerCollection_Release(triggers);
    if(exec)IExecAction_Release(exec);if(action)IAction_Release(action);if(actions)IActionCollection_Release(actions);if(principal)IPrincipal_Release(principal);if(definition)ITaskDefinition_Release(definition);
    return ok;
}
static HRESULT definition_new(ITaskService *service,const expected_task *e,ITaskDefinition **out) {
    ITaskDefinition *definition=NULL;IRegistrationInfo *info=NULL;IPrincipal *principal=NULL;ITriggerCollection *triggers=NULL;
    ITrigger *trigger=NULL;ILogonTrigger *logon=NULL;IActionCollection *actions=NULL;IAction *action=NULL;IExecAction *exec=NULL;ITaskSettings *settings=NULL;
    BSTR unlimited=SysAllocString(L"PT0S"),minute=SysAllocString(L"PT1M");HRESULT result=E_OUTOFMEMORY;
#define STEP(call) do { result=(call);if(FAILED(result))goto done; } while(0)
    if(!unlimited || !minute)goto done;
    STEP(ITaskService_NewTask(service,0,&definition));STEP(ITaskDefinition_get_RegistrationInfo(definition,&info));
    STEP(IRegistrationInfo_put_Description(info,e->description));STEP(ITaskDefinition_get_Principal(definition,&principal));
    STEP(IPrincipal_put_UserId(principal,e->user));STEP(IPrincipal_put_LogonType(principal,TASK_LOGON_INTERACTIVE_TOKEN));STEP(IPrincipal_put_RunLevel(principal,TASK_RUNLEVEL_HIGHEST));
    STEP(ITaskDefinition_get_Triggers(definition,&triggers));STEP(ITriggerCollection_Create(triggers,TASK_TRIGGER_LOGON,&trigger));
    STEP(ITrigger_put_Enabled(trigger,VARIANT_TRUE));STEP(ITrigger_QueryInterface(trigger,&IID_ILogonTrigger,(void **)&logon));STEP(ILogonTrigger_put_UserId(logon,e->user));
    STEP(ITaskDefinition_get_Actions(definition,&actions));STEP(IActionCollection_Create(actions,TASK_ACTION_EXEC,&action));
    STEP(IAction_QueryInterface(action,&IID_IExecAction,(void **)&exec));STEP(IExecAction_put_Path(exec,e->image));
    STEP(IExecAction_put_Arguments(exec,e->arguments));STEP(IExecAction_put_WorkingDirectory(exec,e->directory));
    STEP(ITaskDefinition_get_Settings(definition,&settings));STEP(ITaskSettings_put_Enabled(settings,VARIANT_TRUE));
    STEP(ITaskSettings_put_MultipleInstances(settings,TASK_INSTANCES_IGNORE_NEW));STEP(ITaskSettings_put_ExecutionTimeLimit(settings,unlimited));
    STEP(ITaskSettings_put_RestartCount(settings,3));STEP(ITaskSettings_put_RestartInterval(settings,minute));
    STEP(ITaskSettings_put_StartWhenAvailable(settings,VARIANT_TRUE));STEP(ITaskSettings_put_DisallowStartIfOnBatteries(settings,VARIANT_FALSE));
    STEP(ITaskSettings_put_StopIfGoingOnBatteries(settings,VARIANT_FALSE));STEP(ITaskSettings_put_AllowDemandStart(settings,VARIANT_TRUE));
    STEP(ITaskSettings_put_AllowHardTerminate(settings,VARIANT_FALSE));
    *out=definition;definition=NULL;result=S_OK;
done:
    SysFreeString(unlimited);SysFreeString(minute);
    if(settings)ITaskSettings_Release(settings);if(exec)IExecAction_Release(exec);if(action)IAction_Release(action);if(actions)IActionCollection_Release(actions);
    if(logon)ILogonTrigger_Release(logon);if(trigger)ITrigger_Release(trigger);if(triggers)ITriggerCollection_Release(triggers);
    if(principal)IPrincipal_Release(principal);if(info)IRegistrationInfo_Release(info);if(definition)ITaskDefinition_Release(definition);
    return result;
#undef STEP
}
int sb_startup_admin_logon(const char *mode,sb_startup *profile,sb_error *error) {
    bool guard=!strcmp(mode,"ordinary-logon-guard");
    bool query=guard || !strcmp(mode,"--admin-logon-status"),enable=!strcmp(mode,"--admin-logon-enable");
    if(profile->service || (!query && !sb_win_elevated_owner())) {sb_fail(error,"Highest-privilege logon registration requires a tray profile and an already elevated interactive owner; this command never requests UAC.");return 2;}
    expected_task expected={0};ITaskService *service=NULL;ITaskFolder *folder=NULL;IRegisteredTask *existing=NULL,*created=NULL;ITaskDefinition *definition=NULL;
    VARIANT empty,user;VariantInit(&empty);VariantInit(&user);BSTR root=SysAllocString(L"\\");HRESULT apartment=CoInitializeEx(NULL,COINIT_MULTITHREADED),result=E_FAIL;int code=2;
    if((FAILED(apartment) && apartment!=RPC_E_CHANGED_MODE) || !root || !expected_new(profile,&expected,error))goto done;
    result=CoCreateInstance(&CLSID_TaskScheduler,NULL,CLSCTX_INPROC_SERVER,&IID_ITaskService,(void **)&service);if(FAILED(result))goto done;
    result=ITaskService_Connect(service,empty,empty,empty,empty);if(FAILED(result))goto done;
    result=ITaskService_GetFolder(service,root,&folder);if(FAILED(result))goto done;
    result=ITaskFolder_GetTask(folder,expected.task,&existing);
    if(FAILED(result) && result!=HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))goto done;
    bool owned=existing && binding(existing,&expected);VARIANT_BOOL enabled=VARIANT_FALSE;
    if(existing && FAILED(IRegisteredTask_get_Enabled(existing,&enabled)))goto done;
    if(guard) {
        if(existing)sb_fail(error,"Remove this snapshot's existing highest-privilege task before choosing ordinary logon startup.");
        else code=0;
        goto done;
    }
    if(query) {
        printf("{\"registered\":%s,\"owned_entry\":%s,\"logon_enabled\":%s,\"registration_method\":\"task_scheduler\",\"highest_privileges\":%s,\"interactive_logon\":%s,\"task_name\":\"%ls\"}\n",
            existing?"true":"false",owned?"true":"false",owned && enabled==VARIANT_TRUE?"true":"false",owned?"true":"false",owned?"true":"false",expected.task);code=0;goto done;
    }
    if(existing && !owned){sb_fail(error,"The named task has another action, principal, trigger or settings; it was not overwritten or deleted.");goto done;}
    if(enable) {
        if(GetFileAttributesW(expected.image)==INVALID_FILE_ATTRIBUTES){sb_fail(error,"ShellBot-Launcher.exe is missing; task was not registered.");goto done;}
        /* One startup mechanism per snapshot. Never delete another Run value. */
        char hash[65];wchar_t value[64];HKEY key=NULL;
        if(!sb_platform_sha256(profile->path,strlen(profile->path),hash,error))goto done;
        swprintf(value,64,L"ShellBot-%.24hs",hash);
        LONG opened=RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",0,KEY_QUERY_VALUE,&key);
        if(opened==ERROR_SUCCESS) {
            LONG found=RegQueryValueExW(key,value,NULL,NULL,NULL,NULL);RegCloseKey(key);
            if(found!=ERROR_FILE_NOT_FOUND){sb_fail(error,"Remove this snapshot's existing ordinary logon entry before choosing highest-privilege startup.");goto done;}
        } else if(opened!=ERROR_FILE_NOT_FOUND)goto done;
        if(existing)result=IRegisteredTask_put_Enabled(existing,VARIANT_TRUE);
        else {
            result=definition_new(service,&expected,&definition);if(FAILED(result))goto done;
            V_VT(&user)=VT_BSTR;V_BSTR(&user)=expected.user;
            result=ITaskFolder_RegisterTaskDefinition(folder,expected.task,definition,TASK_CREATE,user,empty,TASK_LOGON_INTERACTIVE_TOKEN,empty,&created);
        }
        if(FAILED(result))goto done;
        IRegisteredTask *verify=NULL;result=ITaskFolder_GetTask(folder,expected.task,&verify);
        VARIANT_BOOL active=VARIANT_FALSE;
        bool verified=SUCCEEDED(result) && binding(verify,&expected) && SUCCEEDED(IRegisteredTask_get_Enabled(verify,&active)) && active==VARIANT_TRUE;if(verify)IRegisteredTask_Release(verify);
        if(!verified){sb_fail(error,"Task registration returned but the exact stored Windows definition could not be verified.");goto done;}
    } else if(existing) {
        result=ITaskFolder_DeleteTask(folder,expected.task,0);if(FAILED(result))goto done;
        IRegisteredTask *verify=NULL;result=ITaskFolder_GetTask(folder,expected.task,&verify);if(verify)IRegisteredTask_Release(verify);
        if(result!=HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)){sb_fail(error,"Windows task removal could not be confirmed; inspect the current registration.");goto done;}
    }
    printf("{\"logon_enabled\":%s,\"registration_method\":\"task_scheduler\",\"highest_privileges\":true,\"windows_managed\":true,\"start_requested\":false,\"running_configuration_changed\":false}\n",enable?"true":"false");code=0;
done:
    if(code && !*error->text){char message[160];snprintf(message,sizeof(message),"Windows Task Scheduler operation failed (0x%08lx); no success was assumed.",(unsigned long)result);sb_fail(error,message);}
    if(definition)ITaskDefinition_Release(definition);if(created)IRegisteredTask_Release(created);if(existing)IRegisteredTask_Release(existing);
    if(folder)ITaskFolder_Release(folder);if(service)ITaskService_Release(service);SysFreeString(root);expected_free(&expected);if(SUCCEEDED(apartment))CoUninitialize();return code;
}
