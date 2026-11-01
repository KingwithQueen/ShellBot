#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "uia.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* One disposable MTA process per request: a hung provider cannot own the desktop thread. */
char *sb_win_uia_request(yyjson_val *request,bool (*guard)(void*),void *context,uint64_t deadline,sb_error *error){
    SECURITY_ATTRIBUTES sa={sizeof(sa),NULL,TRUE};HANDLE input=NULL,send=NULL,output=NULL,receive=NULL,job=NULL;
    PROCESS_INFORMATION process={0};LPPROC_THREAD_ATTRIBUTE_LIST attributes=NULL;bool initialized=false,ok=false;
    char *wire=yyjson_val_write(request,0,NULL),*reply=NULL;size_t length=wire?strlen(wire):0,used=0;
    wchar_t exe[32768],line[32768];DWORD n=GetModuleFileNameW(NULL,exe,32768),native=0;
    if(!length || length>=65535 || !n || n>32740 || _snwprintf(line,32768,L"\"%ls\" --uia-worker",exe)<0){sb_fail(error,"uia_worker_request_size_or_path_invalid");goto done;}
    if(!CreatePipe(&input,&send,&sa,65536) || !SetHandleInformation(send,HANDLE_FLAG_INHERIT,0) || !CreatePipe(&receive,&output,&sa,65536) || !SetHandleInformation(receive,HANDLE_FLAG_INHERIT,0))goto failed;
    job=CreateJobObjectW(NULL,NULL);JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits={0};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!job || !SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits)))goto failed;
    SIZE_T bytes=0;InitializeProcThreadAttributeList(NULL,1,0,&bytes);attributes=malloc(bytes);
    if(!attributes || !(initialized=InitializeProcThreadAttributeList(attributes,1,0,&bytes)!=0))goto failed;
    HANDLE handles[]={input,output};if(!UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,handles,sizeof(handles),NULL,NULL))goto failed;
    STARTUPINFOEXW startup={0};startup.StartupInfo.cb=sizeof(startup);startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES;startup.StartupInfo.hStdInput=input;startup.StartupInfo.hStdOutput=startup.StartupInfo.hStdError=output;startup.lpAttributeList=attributes;
    if(!guard(context)){sb_fail(error,"uia_guard_changed_before_worker_start");goto done;}
    if(!CreateProcessW(exe,line,NULL,NULL,TRUE,CREATE_SUSPENDED|CREATE_NO_WINDOW|EXTENDED_STARTUPINFO_PRESENT,NULL,NULL,&startup.StartupInfo,&process) || !AssignProcessToJobObject(job,process.hProcess) || ResumeThread(process.hThread)==(DWORD)-1)goto failed;
    CloseHandle(input);input=NULL;CloseHandle(output);output=NULL;
    DWORD sent=0;if(!WriteFile(send,wire,(DWORD)length,&sent,NULL) || sent!=length)goto failed;CloseHandle(send);send=NULL;
    reply=malloc(1048577);if(!reply){sb_fail(error,"uia_reply_allocation_failed");goto done;}
    for(;;){
        if(!guard(context)){sb_fail(error,"uia_guard_changed_during_request: possible action effects must be reviewed");goto done;}
        if(sb_platform_millis()>=deadline){sb_fail(error,"uia_provider_deadline: possible action effects must be reviewed; no automatic replay");goto done;}
        DWORD available=0;
        if(!PeekNamedPipe(receive,NULL,0,NULL,&available,NULL)){if(GetLastError()!=ERROR_BROKEN_PIPE)goto failed;available=0;}
        if(available){if(available>1048576-used){sb_fail(error,"uia_reply_budget_exceeded: maximum 1048576 encoded bytes");goto done;}DWORD got=0;if(!ReadFile(receive,reply+used,available,&got,NULL))goto failed;used+=got;continue;}
        if(WaitForSingleObject(process.hProcess,0)==WAIT_OBJECT_0)break;
        Sleep(5);
    }
    DWORD exit=0;if(!GetExitCodeProcess(process.hProcess,&exit) || exit || !used){sb_fail(error,"uia_worker_exited_without_complete_reply");goto done;}
    reply[used]=0;ok=true;goto done;
failed:
    native=GetLastError();snprintf(error->text,sizeof(error->text),"uia_worker_transport_failed: Win32 %lu; review possible effects before retry",native);
done:
    if(job){TerminateJobObject(job,125);CloseHandle(job);}if(process.hProcess){if(!ok)TerminateProcess(process.hProcess,125);WaitForSingleObject(process.hProcess,2000);CloseHandle(process.hProcess);}if(process.hThread)CloseHandle(process.hThread);
    if(initialized)DeleteProcThreadAttributeList(attributes);free(attributes);free(wire);
    HANDLE handles_to_close[]={input,send,output,receive};for(unsigned i=0;i<4;++i)if(handles_to_close[i])CloseHandle(handles_to_close[i]);
    if(!ok){free(reply);reply=NULL;}return reply;
}
