#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "window_capture.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

typedef struct {volatile LONG refs;HANDLE pipe,done;char *data;size_t used;DWORD error;bool budget;} capture_reader;
static void reader_release(capture_reader *r){if(r && !InterlockedDecrement(&r->refs)){if(r->pipe)CloseHandle(r->pipe);if(r->done)CloseHandle(r->done);free(r->data);free(r);}}
/* 专用同步读取线程持续排空大 PNG；主线程按事件等待，不逐块 Sleep 限制吞吐。 */
static DWORD WINAPI read_capture(void *parameter){
    capture_reader *r=parameter;
    for(;;){DWORD got=0;BYTE extra=0;size_t left=SB_RESPONSE_LIMIT-r->used;BOOL ok=ReadFile(r->pipe,left?r->data+r->used:(void*)&extra,left?(DWORD)left:1,&got,NULL);
        if(!ok){DWORD error=GetLastError();r->error=error==ERROR_BROKEN_PIPE || error==ERROR_HANDLE_EOF?0:error;break;}
        if(!got)break;if(!left){r->budget=true;break;}r->used+=got;
    }
    r->data[r->used]=0;SetEvent(r->done);reader_release(r);return 0;
}
char *sb_win_capture_request(yyjson_val *request,bool (*guard)(void*),void *context,uint64_t deadline,sb_error *error){
    SECURITY_ATTRIBUTES sa={sizeof(sa),NULL,TRUE};HANDLE input=NULL,send=NULL,output=NULL,receive=NULL,job=NULL,thread=NULL;PROCESS_INFORMATION process={0};capture_reader *reader=NULL;
    LPPROC_THREAD_ATTRIBUTE_LIST attributes=NULL;bool initialized=false,ok=false;char *wire=yyjson_val_write(request,0,NULL),*reply=NULL;size_t length=wire?strlen(wire):0;
    wchar_t exe[32768],line[32768];DWORD n=GetModuleFileNameW(NULL,exe,32768),native=0;
    if(!length || length>=8191 || !n || n>32720 || _snwprintf(line,32768,L"\"%ls\" --window-capture-worker",exe)<0){sb_fail(error,"capture_worker_request_size_or_path_invalid");goto done;}
    if(!CreatePipe(&input,&send,&sa,8192) || !SetHandleInformation(send,HANDLE_FLAG_INHERIT,0) || !CreatePipe(&receive,&output,&sa,65536) || !SetHandleInformation(receive,HANDLE_FLAG_INHERIT,0))goto failed;
    job=CreateJobObjectW(NULL,NULL);JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits={0};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!job || !SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits)))goto failed;
    SIZE_T bytes=0;InitializeProcThreadAttributeList(NULL,1,0,&bytes);attributes=malloc(bytes);if(!attributes || !(initialized=InitializeProcThreadAttributeList(attributes,1,0,&bytes)!=0))goto failed;
    HANDLE handles[]={input,output};if(!UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,handles,sizeof(handles),NULL,NULL))goto failed;
    STARTUPINFOEXW startup={0};startup.StartupInfo.cb=sizeof(startup);startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES;startup.StartupInfo.hStdInput=input;startup.StartupInfo.hStdOutput=startup.StartupInfo.hStdError=output;startup.lpAttributeList=attributes;
    if(!guard(context)){sb_fail(error,"capture_guard_changed_before_worker_start");goto done;}
    if(!CreateProcessW(exe,line,NULL,NULL,TRUE,CREATE_SUSPENDED|CREATE_NO_WINDOW|EXTENDED_STARTUPINFO_PRESENT,NULL,NULL,&startup.StartupInfo,&process) || !AssignProcessToJobObject(job,process.hProcess) || ResumeThread(process.hThread)==(DWORD)-1)goto failed;
    CloseHandle(input);input=NULL;CloseHandle(output);output=NULL;DWORD sent=0;if(!WriteFile(send,wire,(DWORD)length,&sent,NULL) || sent!=length)goto failed;CloseHandle(send);send=NULL;
    reader=calloc(1,sizeof(*reader));if(!reader){sb_fail(error,"capture_reply_allocation_failed");goto done;}reader->refs=1;reader->pipe=receive;receive=NULL;reader->done=CreateEventW(NULL,TRUE,FALSE,NULL);reader->data=malloc(SB_RESPONSE_LIMIT+1u);
    if(!reader->done || !reader->data){sb_fail(error,"capture_reply_allocation_failed");goto done;}InterlockedIncrement(&reader->refs);thread=CreateThread(NULL,0,read_capture,reader,0,NULL);if(!thread){reader_release(reader);goto failed;}
    for(;;){
        if(!guard(context)){sb_fail(error,"capture_guard_changed_during_request");goto done;}uint64_t now=sb_platform_millis();if(now>=deadline){sb_fail(error,"capture_worker_deadline");goto done;}
        DWORD wait=(DWORD)(deadline-now);if(wait>20)wait=20;DWORD status=WaitForSingleObject(reader->done,wait);if(status==WAIT_OBJECT_0)break;if(status!=WAIT_TIMEOUT)goto failed;
    }
    if(!guard(context)){sb_fail(error,"capture_guard_changed_after_transfer");goto done;}
    if(reader->budget){sb_fail(error,"capture_reply_budget_exceeded");goto done;}if(reader->error){SetLastError(reader->error);goto failed;}
    DWORD state=WAIT_TIMEOUT;while((state=WaitForSingleObject(process.hProcess,0))==WAIT_TIMEOUT){
        if(!guard(context)){sb_fail(error,"capture_guard_changed_before_exit");goto done;}uint64_t now=sb_platform_millis();if(now>=deadline){sb_fail(error,"capture_worker_deadline");goto done;}
        DWORD wait=(DWORD)(deadline-now);if(wait>20)wait=20;WaitForSingleObject(process.hProcess,wait);
    }
    DWORD exit=0;if(state!=WAIT_OBJECT_0 || !GetExitCodeProcess(process.hProcess,&exit))goto failed;
    if(exit || !reader->used){sb_fail(error,"capture_worker_exited_without_complete_reply");goto done;}
    reply=reader->data;reader->data=NULL;ok=true;goto done;
failed:
    native=GetLastError();snprintf(error->text,sizeof(error->text),"capture_worker_transport_failed: Win32 %lu",native);
done:
    if(job){TerminateJobObject(job,125);CloseHandle(job);}if(process.hProcess){if(!ok)TerminateProcess(process.hProcess,125);WaitForSingleObject(process.hProcess,500);CloseHandle(process.hProcess);}if(process.hThread)CloseHandle(process.hThread);
    /* 引用计数确保即便取消需要延后完成，也不会释放读取线程仍在使用的管道/缓冲。 */
    if(thread){CancelSynchronousIo(thread);WaitForSingleObject(thread,500);CloseHandle(thread);}reader_release(reader);
    if(initialized)DeleteProcThreadAttributeList(attributes);free(attributes);free(wire);HANDLE close[]={input,send,output,receive};for(unsigned i=0;i<4;++i)if(close[i])CloseHandle(close[i]);return reply;
}
