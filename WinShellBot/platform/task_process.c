#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wtsapi32.h>
#include "sb.h"
#include "execution.h"
#include "task_process.h"
#include "sandbox.h"
#include "conpty.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define TASK_RECORDS 32
#define TASK_ACTIVE 8
#define ARCHIVE_RECORDS 4096
#define OUTPUT_CAP 262144u
#define INPUT_RECORDS 1024
#define START_REPLY_MS 250u
typedef enum { START_QUEUED,START_RESERVING,START_ENVIRONMENT,START_CONTAINMENT,START_CREATING,START_ASSIGNING,START_READERS,START_INTENT,START_RESUMING,START_RECORDING,START_READY,START_FAILED,START_PHASES } startup_phase;
static const char *const startup_names[]={"queued","reserving_request_key","preparing_environment","preparing_containment","creating_process","assigning_job","starting_io","recording_start_intent","resuming_process","recording_started","ready","failed"};
typedef struct { char key[65], hash[65]; char *data; DWORD length; SHORT columns,rows; HRESULT resize_hresult; bool eof, sent, delivered,intent_recorded; } task_input;
typedef struct persistence_job {struct task *item;const char *event,*status;bool output;HANDLE thread,previous;struct persistence_job *next;} persistence_job;
typedef struct task {
    SRWLOCK lock,pty_lock,event_lock;CONDITION_VARIABLE event_condition;uint64_t event_issued,event_next;unsigned persistence_pending;volatile LONG references;
    char id[33], key[65], hash[65], actor[64];
    HANDLE job, process, input, output[2], readers[2], writer, input_event,pty_closer,starter,start_ready,archiver,archive_ready;
    HPCON pty;SHORT columns,rows;bool pty_closed;
    unsigned char *bytes[2]; size_t lengths[2], capacities[2], output_cap;
    bool output_done[2], input_closed, input_failed, finished;
    DWORD pid, exit_code; uint64_t started, timeout;
    const char *reason;
    task_input inputs[INPUT_RECORDS]; size_t input_count;
    sb_win_tasks *owner;
    char *command;
    bool durable_reserved, durable_warning, archived,archiving,archive_failed, gui, foreground_granted;char *archive_result;const char *pending_stop_event;unsigned monitor_pending;persistence_job *persistence_jobs;
    bool starting,startup_complete,process_resumed,startup_native_error_known,terminal_requested;startup_phase phase,failure_phase;uint64_t phase_at,phase_ms[START_PHASES];DWORD startup_native_error;char startup_error[256];
    struct task *retired_next;
} task;
typedef struct { char id[33],key[65],hash[65],actor[64]; char *receipt; } archived_task;
struct sb_win_tasks { SRWLOCK lock; task *items[TASK_RECORDS],*retired; archived_task archives[ARCHIVE_RECORDS]; size_t archive_count,archiving_count; HANDLE stop, monitor, worker; sb_execution_profile profile; bool elevated; sb_resource_limits limits; sb_win_sandbox *sandbox; sb_store *store; sb_audit *audit; };
typedef struct { task *item; unsigned stream; } reader_context;
static bool gui_desktop(void){
    wchar_t station[64]={0},desktop[64]={0};DWORD size=0,session=0;LPWSTR state=NULL;
    bool active=ProcessIdToSessionId(GetCurrentProcessId(),&session) && WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE,session,WTSConnectState,&state,&size) && size>=sizeof(WTS_CONNECTSTATE_CLASS) && (*(WTS_CONNECTSTATE_CLASS*)state==WTSActive || *(WTS_CONNECTSTATE_CLASS*)state==WTSDisconnected);WTSFreeMemory(state);
    bool ok=active && GetUserObjectInformationW(GetProcessWindowStation(),UOI_NAME,station,sizeof(station),&size) && !_wcsicmp(station,L"WinSta0") &&
        GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()),UOI_NAME,desktop,sizeof(desktop),&size) && !_wcsicmp(desktop,L"Default");
    /* Starting an ordinary GUI on Default does not require input-desktop or pixel access. */
    return ok;
}
bool sb_win_tasks_terminal_available(void){return sb_pty_available();}
static yyjson_mut_val *snapshot(sb_win_tasks *tasks, task *item, yyjson_mut_doc *doc);
static bool task_event_inner(task *item, const char *event, const char *status, bool output,bool startup) {
    sb_win_tasks *tasks=item->owner;
    if (!tasks || !tasks->store) return true;
    if(startup)AcquireSRWLockExclusive(&item->lock);
    if(item->finished)status=item->reason?item->reason:"completed";
    uint64_t ticket=item->event_issued++;
    yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL); yyjson_mut_val *body=doc ? snapshot(tasks,item,doc) : NULL;
    yyjson_doc *command=item->command ? yyjson_read(item->command,strlen(item->command),0) : NULL;
    if (body && command) {
        const char *keys[]={"executable","cwd","argv"};
        for (size_t i=0;i<3;++i) yyjson_mut_obj_add_val(doc,body,keys[i],yyjson_val_mut_copy(doc,yyjson_obj_get(yyjson_doc_get_root(command),keys[i])));
    }
    if (body) {
        yyjson_mut_val *inputs=yyjson_mut_arr(doc);
        for (size_t i=0;i<item->input_count;++i) {
            task_input *input=&item->inputs[i]; yyjson_mut_val *entry=yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc,entry,"input_key",input->key); yyjson_mut_obj_add_str(doc,entry,"sha256",input->hash);
            yyjson_mut_obj_add_bool(doc,entry,"eof",input->eof); yyjson_mut_obj_add_uint(doc,entry,"bytes",input->length);
            if(input->columns){yyjson_mut_obj_add_uint(doc,entry,"terminal_columns",(unsigned)input->columns);yyjson_mut_obj_add_uint(doc,entry,"terminal_rows",(unsigned)input->rows);}
            yyjson_mut_obj_add_str(doc,entry,"status",input->sent ? input->delivered ? "delivered" : "failed" : "queued"); yyjson_mut_arr_add_val(inputs,entry);
        }
        yyjson_mut_obj_add_val(doc,body,"input_receipts",inputs);
        if(!strcmp(event,"task_archived")){yyjson_mut_obj_remove_str(body,"archived");yyjson_mut_obj_add_bool(doc,body,"archived",true);yyjson_mut_obj_remove_str(body,"output_available");yyjson_mut_obj_add_bool(doc,body,"output_available",false);yyjson_mut_obj_remove_str(body,"stdin_closed");yyjson_mut_obj_add_bool(doc,body,"stdin_closed",true);}
        if (output) for (unsigned i=0;i<2;++i) {
            size_t retained=item->lengths[i]<OUTPUT_CAP ? item->lengths[i] : OUTPUT_CAP;
            char *encoded=sb_base64(item->bytes[i],retained);
            if (encoded) yyjson_mut_obj_add_strcpy(doc,body,i ? "stderr_base64" : "stdout_base64",encoded);
            yyjson_mut_obj_add_uint(doc,body,i ? "stderr_retained_bytes" : "stdout_retained_bytes",retained);
            yyjson_mut_obj_add_bool(doc,body,i ? "stderr_history_truncated" : "stdout_history_truncated",retained!=item->lengths[i]);
            free(encoded);
        }
    }
    ++item->persistence_pending;InterlockedIncrement(&item->references);ReleaseSRWLockExclusive(&item->lock);
    AcquireSRWLockExclusive(&item->event_lock);while(item->event_next!=ticket)SleepConditionVariableSRW(&item->event_condition,&item->event_lock,INFINITE,0);
    bool ok=body && sb_store_append(tasks->store,"task",item->id,item->actor,status,event,body) &&
        sb_audit_task(tasks->audit,event,item->actor,item->key,item->id,tasks->profile,item->hash,"lifecycle",item->pid);
    ++item->event_next;WakeAllConditionVariable(&item->event_condition);ReleaseSRWLockExclusive(&item->event_lock);
    AcquireSRWLockExclusive(&item->lock);--item->persistence_pending;if(!ok)item->durable_warning=true;InterlockedDecrement(&item->references);
    if(startup)ReleaseSRWLockExclusive(&item->lock);
    yyjson_doc_free(command); yyjson_mut_doc_free(doc); return ok;
}

static bool task_event(task *item,const char *event,const char *status,bool output){return task_event_inner(item,event,status,output,false);}
static bool startup_event(task *item,const char *event,const char *status){return task_event_inner(item,event,status,false,true);}
static DWORD WINAPI persist_monitor_event(void *parameter){persistence_job *job=parameter;if(job->previous)WaitForSingleObject(job->previous,INFINITE);AcquireSRWLockExclusive(&job->item->lock);task_event(job->item,job->event,job->status,job->output);--job->item->monitor_pending;ReleaseSRWLockExclusive(&job->item->lock);return 0;}
/* monitor 只排入有稳定线程句柄的生命周期记录，不能自己等待磁盘而漏掉其他任务期限。 */
static bool monitor_event(task *item,const char *event,const char *status,bool output){
    if(!item->owner->store)return true;persistence_job *job=calloc(1,sizeof(*job));if(!job){item->durable_warning=true;return false;}
    job->item=item;job->event=event;job->status=status;job->output=output;job->previous=item->persistence_jobs?item->persistence_jobs->thread:NULL;++item->monitor_pending;
    job->thread=CreateThread(NULL,0,persist_monitor_event,job,0,NULL);if(!job->thread){--item->monitor_pending;free(job);item->durable_warning=true;return false;}job->next=item->persistence_jobs;item->persistence_jobs=job;return true;
}

static char *serialize(yyjson_mut_doc *doc, yyjson_mut_val *root) {
    yyjson_mut_doc_set_root(doc, root); char *json = yyjson_mut_write(doc, 0, NULL); yyjson_mut_doc_free(doc); return json;
}
bool sb_win_tasks_owned(sb_win_tasks *tasks, const char *actor, const char *id) {
    if (!tasks || !actor || !id) return false;
    bool found = false;
    AcquireSRWLockShared(&tasks->lock);
    for (size_t i = 0; i < TASK_RECORDS; ++i)
        if (tasks->items[i] && !strcmp(tasks->items[i]->actor, actor) && !strcmp(tasks->items[i]->id, id)) found = true;
    for (size_t i=0;i<tasks->archive_count;++i)
        if (!strcmp(tasks->archives[i].actor,actor) && !strcmp(tasks->archives[i].id,id)) found=true;
    ReleaseSRWLockShared(&tasks->lock);
    return found;
}
static char *fail(const char *message) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL); if (!doc) return NULL;
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_bool(doc, root, "_error", true); yyjson_mut_obj_add_strcpy(doc, root, "error", message);
    const char *code=NULL;bool retryable=false;
    if(!strncmp(message,"task_still_running:",strlen("task_still_running:"))){code="TASK_RUNNING";retryable=true;}
    else if(!strncmp(message,"task_persistence_pending:",strlen("task_persistence_pending:"))){code="TASK_PERSISTENCE_PENDING";retryable=true;}
    else if(!strncmp(message,"task_archive_pending:",strlen("task_archive_pending:"))){code="TASK_ARCHIVE_PENDING";retryable=true;}
    else if(!strncmp(message,"archive_receipt_budget_exceeded:",strlen("archive_receipt_budget_exceeded:")))code="ARCHIVE_RECEIPT_LIMIT";
    else if(!strncmp(message,"Cannot persist archive receipt;",strlen("Cannot persist archive receipt;"))){code="ARCHIVE_PERSIST_FAILED";retryable=true;}
    else if(!strncmp(message,"Unknown task_id for this caller",strlen("Unknown task_id for this caller")))code="TASK_NOT_OWNED_OR_UNKNOWN";
    if(code){yyjson_mut_obj_add_str(doc,root,"code",code);yyjson_mut_obj_add_bool(doc,root,"retryable",retryable);}
    return serialize(doc, root);
}
static char *limit_fail(const char *field,uint64_t actual,uint64_t maximum,const char *unit) {
    yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL);if(!doc)return NULL;
    yyjson_mut_val *root=sb_limit_error(doc,field,actual,maximum,unit);yyjson_mut_obj_add_bool(doc,root,"_error",true);return serialize(doc,root);
}
/* 持有 tasks->lock；任务状态只能在同一外层锁下转为 finished。 */
static void budget(sb_win_tasks *tasks,yyjson_mut_doc *doc,yyjson_mut_val *root) {
    unsigned active=0,records=0,starting=0;
    for(size_t i=0;i<TASK_RECORDS;++i)if(tasks->items[i]){++records;AcquireSRWLockShared(&tasks->items[i]->lock);if(!tasks->items[i]->finished)++active;if(tasks->items[i]->starting)++starting;ReleaseSRWLockShared(&tasks->items[i]->lock);}
    yyjson_mut_val *b=yyjson_mut_obj(doc);
    yyjson_mut_obj_add_uint(doc,b,"active_count",active);yyjson_mut_obj_add_uint(doc,b,"active_limit",tasks->limits.active_tasks);
    yyjson_mut_obj_add_uint(doc,b,"starting_count",starting);
    yyjson_mut_obj_add_uint(doc,b,"record_count",records);yyjson_mut_obj_add_uint(doc,b,"record_limit",tasks->limits.task_records);
    yyjson_mut_obj_add_uint(doc,b,"archived_count",tasks->archive_count);yyjson_mut_obj_add_uint(doc,b,"archived_limit",ARCHIVE_RECORDS);
    yyjson_mut_obj_add_uint(doc,b,"ended_count",records-active);yyjson_mut_obj_add_str(doc,b,"scope","backend_instance_all_callers");
    yyjson_mut_obj_add_val(doc,root,"task_budget",b);
}
static char *task_list(sb_win_tasks *tasks,yyjson_val *args) {
    const char *actor=sb_string(yyjson_obj_get(args,"actor"));uint64_t offset=0,maximum=0;
    if(!actor || !sb_integer(yyjson_obj_get(args,"offset"),&offset) || !sb_integer(yyjson_obj_get(args,"max_entries"),&maximum) || !maximum || maximum>1000)return fail("Invalid task list page.");
    yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL);if(!doc)return NULL;
    yyjson_mut_val *root=yyjson_mut_obj(doc),*array=yyjson_mut_arr(doc);uint64_t matched=0,returned=0;
    for(size_t i=0;i<TASK_RECORDS;++i){task *item=tasks->items[i];if(!item || strcmp(item->actor,actor))continue;
        if(matched++<offset || returned==maximum)continue;
        AcquireSRWLockShared(&item->lock);yyjson_mut_arr_add_val(array,snapshot(tasks,item,doc));ReleaseSRWLockShared(&item->lock);++returned;
    }
    if(yyjson_get_bool(yyjson_obj_get(args,"include_archived")))for(size_t i=0;i<tasks->archive_count;++i){archived_task *a=&tasks->archives[i];if(strcmp(a->actor,actor))continue;
        if(matched++<offset || returned==maximum)continue;
        yyjson_doc *saved=yyjson_read(a->receipt,strlen(a->receipt),0);yyjson_mut_arr_add_val(array,yyjson_val_mut_copy(doc,yyjson_doc_get_root(saved)));yyjson_doc_free(saved);++returned;
    }
    yyjson_mut_obj_add_val(doc,root,"tasks",array);yyjson_mut_obj_add_uint(doc,root,"next_offset",offset+returned);
    yyjson_mut_obj_add_bool(doc,root,"has_more",offset+returned<matched);yyjson_mut_obj_add_uint(doc,root,"caller_total",matched);budget(tasks,doc,root);return serialize(doc,root);
}
static UINT codepage(const char *name) {
    if(!name)return 0;
    if(!_stricmp(name,"utf-8") || !_stricmp(name,"utf8"))return CP_UTF8;
    if(!_stricmp(name,"gbk"))return 936;
    if(_strnicmp(name,"cp",2) || !name[2])return 0;
    unsigned value=0;for(const char *p=name+2;*p;++p){if(*p<'0'||*p>'9'||value>6553)return 0;value=value*10+(unsigned)(*p-'0');}
    return value && IsValidCodePage(value) ? value : 0;
}
static void decoded_output(yyjson_mut_doc *doc,yyjson_mut_val *result,const unsigned char *bytes,size_t length,UINT cp,uint64_t offset,bool terminal) {
    size_t size=length;int count=0;unsigned trim=0;
    /* 独立文本游标用于重读跨页的尾字节；原始字节游标始终不变。 */
    for(;trim<=3 && trim<=length;++trim){
        size=length-trim;if(!size){count=0;break;}
        count=MultiByteToWideChar(cp,MB_ERR_INVALID_CHARS,(const char*)bytes,(int)size,NULL,0);
        if(count)break;
    }
    bool valid=count>0 || !length;
    if(trim && terminal)valid=false;
    wchar_t *wide=valid && count ? malloc((size_t)count*sizeof(wchar_t)) : NULL;
    if(valid && count && (!wide || !MultiByteToWideChar(cp,MB_ERR_INVALID_CHARS,(const char*)bytes,(int)size,wide,count)))valid=false;
    int output=valid && count ? WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide,count,NULL,0,NULL,NULL):0;
    if(valid && count && !output)valid=false;
    char *text=output ? malloc((size_t)output) : NULL;
    if(output && (!text || !WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide,count,text,output,NULL,NULL)))valid=false;
    char name[24];if(cp==CP_UTF8)strcpy(name,"utf-8");else snprintf(name,sizeof(name),"cp%u",cp);
    yyjson_mut_obj_add_strcpy(doc,result,"text_encoding",name);
    yyjson_mut_obj_add_uint(doc,result,"text_next_offset",offset+(valid?size:0));
    yyjson_mut_obj_add_str(doc,result,"decode_status",valid?(trim?"partial":"complete"):"invalid_or_incomplete");
    if(valid)yyjson_mut_obj_add_strncpy(doc,result,"text",text?text:"",(size_t)output);
    else yyjson_mut_obj_add_str(doc,result,"decode_error","The page cannot be strictly decoded. Raw bytes remain in the task; use payload_mode bytes, retry from text_next_offset with more bytes, or select the correct encoding.");
    free(wide);free(text);
}
static bool key_valid(const char *key) { return key && *key && strlen(key) <= 64 && strspn(key, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.") == strlen(key); }
static wchar_t *to_wide(const char *value) {
    if (!value) return NULL;
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, NULL, 0);
    wchar_t *wide = size > 0 && size <= 32768 ? malloc((size_t)size * sizeof(*wide)) : NULL;
    if (wide && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, wide, size)) { free(wide); wide = NULL; }
    return wide;
}
static bool absolute_exe(const wchar_t *path) {
    size_t size = path ? wcslen(path) : 0;
    return size > 7 && ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) && path[1] == L':' &&
        (path[2] == L'\\' || path[2] == L'/') && !_wcsicmp(path + size - 4, L".exe") && !wcschr(path + 2, L':');
}
/* Windows CRT argument rules; the chosen executable may interpret its own arguments. */
static size_t quoted_size(const wchar_t *arg) {
    /* 精确计算转义后的长度，普通长参数不会再被最坏情况预估提前拒绝。 */
    size_t bound = 2u, slashes = 0;
    for (const wchar_t *p = arg; ; ++p) {
        if (*p == L'\\') { ++slashes; continue; }
        bound += (*p == L'"' || !*p) ? slashes * 2 : slashes;
        slashes = 0;
        if (!*p) break;
        bound += *p == L'"' ? 2u : 1u;
    }
    return bound;
}
static bool quote(wchar_t *line, size_t *used, const wchar_t *arg) {
    size_t bound=quoted_size(arg)+(*used ? 1u : 0u),slashes=0;
    if (*used >= SB_WINDOWS_COMMAND_CAP || bound >= SB_WINDOWS_COMMAND_CAP - *used) return false;
    if (*used) line[(*used)++] = L' ';
    line[(*used)++] = L'"';
    slashes = 0;
    for (const wchar_t *p = arg; ; ++p) {
        if (*p == L'\\') { ++slashes; continue; }
        size_t copies = (*p == L'"' || !*p) ? slashes * 2 : slashes;
        while (copies--) line[(*used)++] = L'\\';
        slashes = 0;
        if (!*p) break;
        if (*p == L'"') line[(*used)++] = L'\\';
        line[(*used)++] = *p;
    }
    line[(*used)++] = L'"'; line[*used] = 0; return true;
}
static DWORD WINAPI read_output(void *parameter) {
    reader_context context = *(reader_context *)parameter; free(parameter);
    task *item = context.item; unsigned stream = context.stream; unsigned char chunk[4096]; DWORD got;
    while (ReadFile(item->output[stream], chunk, sizeof(chunk), &got, NULL) && got) {
        AcquireSRWLockExclusive(&item->lock);
        size_t remaining = item->output_cap - item->lengths[stream], copy = got < remaining ? got : remaining;
        if (item->lengths[stream]+copy>item->capacities[stream]) {
            size_t capacity=item->capacities[stream];
            while(capacity<item->lengths[stream]+copy) { capacity*=2; if(capacity>item->output_cap)capacity=item->output_cap; }
            unsigned char *grown=realloc(item->bytes[stream],capacity);
            if(grown) { item->bytes[stream]=grown;item->capacities[stream]=capacity; }
            else { copy=0;if(!item->reason){item->reason="failed";TerminateJobObject(item->job,126);task_event(item,"task_output_allocation_failed","running",false);} }
        }
        memcpy(item->bytes[stream] + item->lengths[stream], chunk, copy); item->lengths[stream] += copy;
        if (copy < got && !item->reason) { item->reason = "output_limit"; TerminateJobObject(item->job,124);task_event(item,"task_output_limit","running",false); }
        ReleaseSRWLockExclusive(&item->lock);
    }
    AcquireSRWLockExclusive(&item->lock); item->output_done[stream] = true; ReleaseSRWLockExclusive(&item->lock); return 0;
}
static DWORD WINAPI close_terminal(void *parameter){task *item=parameter;AcquireSRWLockExclusive(&item->pty_lock);if(!item->pty_closed){item->pty_closed=true;sb_pty_close(item->pty);}ReleaseSRWLockExclusive(&item->pty_lock);return 0;}
static void terminal_drain(task *item){
    AcquireSRWLockExclusive(&item->lock);
    if(!item->readers[0]){reader_context *context=malloc(sizeof(*context));if(context){context->item=item;context->stream=0;item->output_done[0]=false;item->readers[0]=CreateThread(NULL,0,read_output,context,0,NULL);if(!item->readers[0])free(context);}}
    if(!item->readers[0] && item->output[0]){CloseHandle(item->output[0]);item->output[0]=NULL;}
    ReleaseSRWLockExclusive(&item->lock);
}
static DWORD WINAPI write_input(void *parameter) {
    task *item = parameter; size_t next = 0;bool can_close_input=true;
    for (;;) {
        WaitForSingleObject(item->input_event, 50);
        AcquireSRWLockExclusive(&item->lock);
        bool stop = item->finished || item->reason || item->input_closed;
        task_input *input = next < item->input_count ? &item->inputs[next] : NULL;
        ReleaseSRWLockExclusive(&item->lock);
        if (stop) break;
        if (!input) continue;
        AcquireSRWLockExclusive(&item->lock);
        bool recorded=task_event(item,"task_input_queued","running",false);input->intent_recorded=recorded;
        if(recorded && !item->finished && !item->reason)recorded=task_event(item,"task_input_started","running",false);
        recorded=recorded && !item->finished && !item->reason;
        bool stop_before_eof=!recorded && !item->finished;
        if(stop_before_eof){if(!item->reason)item->reason="failed";TerminateJobObject(item->job,125);}
        ReleaseSRWLockExclusive(&item->lock);
        /* 失败的 intent 不能通过关闭 stdin 间接送出 EOF；先确认 Job 已停止。 */
        if(stop_before_eof)for(;;){JOBOBJECT_BASIC_ACCOUNTING_INFORMATION ended={0};if(!QueryInformationJobObject(item->job,JobObjectBasicAccountingInformation,&ended,sizeof(ended),NULL)){can_close_input=false;break;}if(!ended.ActiveProcesses)break;Sleep(1);}
        DWORD written = 0;
        AcquireSRWLockShared(&item->pty_lock);HRESULT resized_hr=!recorded?E_ABORT:!input->columns?S_OK:item->pty_closed?HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE):sb_pty_resize(item->pty,(COORD){input->columns,input->rows});bool resized=SUCCEEDED(resized_hr);ReleaseSRWLockShared(&item->pty_lock);
        bool delivered = recorded && resized && (!input->length || (WriteFile(item->input, input->data, input->length, &written, NULL) && written == input->length));
        AcquireSRWLockExclusive(&item->lock);
        input->sent = true; input->delivered = delivered;
        input->resize_hresult=resized_hr;
        if(resized && input->columns){item->columns=input->columns;item->rows=input->rows;}
        free(input->data); input->data = NULL; ++next;
        if (!delivered) item->input_failed = true;
        task_event(item,delivered ? "task_input_delivered" : "task_input_failed","running",false);
        bool eof = input->eof || !delivered;
        ReleaseSRWLockExclusive(&item->lock);
        if (eof) break;
    }
    AcquireSRWLockExclusive(&item->lock);
    if (item->input && can_close_input) { CloseHandle(item->input); item->input = NULL; }
    item->input_closed = true;
    bool abandoned=false;for(size_t i=next;i<item->input_count;++i)if(!item->inputs[i].sent){item->inputs[i].sent=true;item->inputs[i].delivered=false;free(item->inputs[i].data);item->inputs[i].data=NULL;item->input_failed=true;abandoned=true;}if(abandoned)task_event(item,"task_input_abandoned",item->finished?(item->reason?item->reason:"completed"):"running",false);
    ReleaseSRWLockExclusive(&item->lock); return 0;
}
static void task_free(task *item);
static bool task_threads_done(task *item) {
    if(InterlockedCompareExchange(&item->references,0,0))return false;
    for(persistence_job *job=item->persistence_jobs;job;job=job->next)if(WaitForSingleObject(job->thread,0)!=WAIT_OBJECT_0)return false;
    HANDLE threads[]={item->readers[0],item->readers[1],item->writer,item->pty_closer,item->starter,item->archiver};
    for(unsigned i=0;i<6;++i)if(threads[i] && WaitForSingleObject(threads[i],0)!=WAIT_OBJECT_0)return false;
    return true;
}
static DWORD WINAPI monitor_tasks(void *parameter) {
    sb_win_tasks *tasks = parameter;
    while (WaitForSingleObject(tasks->stop, 25) == WAIT_TIMEOUT) {
        for (size_t i = 0; i < TASK_RECORDS; ++i) {
            AcquireSRWLockExclusive(&tasks->lock);task *item=tasks->items[i];if(item)InterlockedIncrement(&item->references);ReleaseSRWLockExclusive(&tasks->lock);if(!item)continue;
            AcquireSRWLockExclusive(&item->lock);
            if(item->starting){
                if(!item->reason && (WaitForSingleObject(tasks->stop,0)==WAIT_OBJECT_0 || (tasks->worker && WaitForSingleObject(tasks->worker,0)!=WAIT_TIMEOUT)))item->reason="cancelled";
                if(!item->reason && sb_platform_millis()-item->started>=item->timeout)item->reason="timed_out";
                if(item->reason){if(item->job)TerminateJobObject(item->job,125);if(item->process)TerminateProcess(item->process,125);}
                goto item_done;
            }
            if(item->pending_stop_event){const char *event=item->pending_stop_event;item->pending_stop_event=NULL;monitor_event(item,event,item->finished?(item->reason?item->reason:"completed"):"running",false);}
            if (!item->finished) {
                if (tasks->worker && WaitForSingleObject(tasks->worker,0) != WAIT_TIMEOUT && !item->reason) {
                    item->reason="cancelled"; TerminateJobObject(item->job,125);monitor_event(item,"task_worker_exited","running",false); SetEvent(item->input_event);
                }
                if(!item->process){
                    if(item->output_done[0] && item->output_done[1]){item->exit_code=125;item->finished=true;if(!item->reason)item->reason="failed";monitor_event(item,"task_completed",item->reason,true);}
                    goto item_done;
                }
                JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting = {0};
                bool queried = QueryInformationJobObject(item->job, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), NULL) != FALSE;
                if (!queried && !item->reason) { item->reason = "failed"; TerminateJobObject(item->job,125);monitor_event(item,"task_query_failed","running",false); }
                if (queried && accounting.ActiveProcesses && !item->reason && sb_platform_millis() - item->started >= item->timeout) {
                    item->reason = "timed_out"; TerminateJobObject(item->job,124);monitor_event(item,"task_timed_out","running",false);
                }
                /* Close on a separate thread while the reader keeps draining VT output. */
                if(item->pty && !item->pty_closer && ((queried && !accounting.ActiveProcesses) || item->reason))item->pty_closer=CreateThread(NULL,0,close_terminal,item,0,NULL);
                if (queried && !accounting.ActiveProcesses && item->output_done[0] && item->output_done[1] && WaitForSingleObject(item->process,0)==WAIT_OBJECT_0) {
                    if (!GetExitCodeProcess(item->process, &item->exit_code)) { item->exit_code = 125; if (!item->reason) item->reason = "failed"; }
                    item->finished = true; SetEvent(item->input_event);
                    monitor_event(item,"task_completed",item->reason ? item->reason : "completed",true);
                }
            }
item_done:
            ReleaseSRWLockExclusive(&item->lock);InterlockedDecrement(&item->references);
        }
        AcquireSRWLockExclusive(&tasks->lock);
        /* 已归档任务不再被调用方访问；只回收已退出的线程，绝不持全局锁等待。 */
        task *ready=NULL,**link=&tasks->retired;
        while(*link){task *item=*link;if(task_threads_done(item)){*link=item->retired_next;item->retired_next=ready;ready=item;}else link=&item->retired_next;}
        ReleaseSRWLockExclusive(&tasks->lock);
        while(ready){task *next=ready->retired_next;task_free(ready);ready=next;}
    }
    return 0;
}
static void task_free(task *item) {
    if (!item) return;
    while(item->persistence_jobs){persistence_job *job=item->persistence_jobs;WaitForSingleObject(job->thread,INFINITE);CloseHandle(job->thread);item->persistence_jobs=job->next;free(job);}
    if(item->archiver){WaitForSingleObject(item->archiver,INFINITE);CloseHandle(item->archiver);item->archiver=NULL;}
    if(item->starter){AcquireSRWLockExclusive(&item->lock);if(item->starting){if(!item->reason)item->reason="cancelled";if(item->job)TerminateJobObject(item->job,125);if(item->process)TerminateProcess(item->process,125);}ReleaseSRWLockExclusive(&item->lock);WaitForSingleObject(item->starter,INFINITE);CloseHandle(item->starter);item->starter=NULL;}
    if (item->job) TerminateJobObject(item->job, 125);
    AcquireSRWLockExclusive(&item->lock); bool unfinished=!item->finished;
    item->finished = true; if (unfinished && !item->reason) item->reason="cancelled";
    if (item->input_event) SetEvent(item->input_event); ReleaseSRWLockExclusive(&item->lock);
    if(item->pty){
        terminal_drain(item);
        if(item->pty_closer){WaitForSingleObject(item->pty_closer,INFINITE);CloseHandle(item->pty_closer);}else close_terminal(item);
    }
    HANDLE threads[] = {item->readers[0],item->readers[1],item->writer};
    for (size_t i = 0; i < 3; ++i) if (threads[i]) { CancelSynchronousIo(threads[i]); WaitForSingleObject(threads[i], INFINITE); CloseHandle(threads[i]); }
    if (unfinished && item->durable_reserved) {
        if (item->process && WaitForSingleObject(item->process,3000)==WAIT_OBJECT_0) GetExitCodeProcess(item->process,&item->exit_code);
        else item->reason="interrupted";
        AcquireSRWLockExclusive(&item->lock);task_event(item,"task_instance_closed",item->reason,true);ReleaseSRWLockExclusive(&item->lock);
    }
    HANDLE handles[] = {item->job,item->process,item->input,item->output[0],item->output[1],item->input_event,item->start_ready,item->archive_ready};
    for (size_t i = 0; i < 8; ++i) if (handles[i]) CloseHandle(handles[i]);
    for (size_t i = 0; i < INPUT_RECORDS; ++i) free(item->inputs[i].data);
    free(item->bytes[0]); free(item->bytes[1]); free(item->command); free(item);
}
sb_win_tasks *sb_win_tasks_new(sb_execution_profile profile) {
    const sb_resource_limits limits = {8,32,SB_TASK_OUTPUT_DEFAULT,3600000,64,SB_WORKER_MEMORY_DEFAULT};
    return sb_win_tasks_new_limited(profile, &limits);
}
sb_win_tasks *sb_win_tasks_new_limited(sb_execution_profile profile, const sb_resource_limits *limits) {
    HANDLE token = NULL; TOKEN_ELEVATION elevation = {0}; DWORD bytes = 0;
    bool ok = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) && sb_win_owner_token_allowed(token) &&
        GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &bytes);
    if (token) CloseHandle(token);
    if (!ok || profile == SB_EXEC_CONTROLLED || (profile == SB_EXEC_USER && elevation.TokenIsElevated && !sb_win_service_process()) || (profile == SB_EXEC_ADMINISTRATOR && !elevation.TokenIsElevated)) return NULL;
    sb_win_tasks *tasks = calloc(1, sizeof(*tasks)); if (!tasks) return NULL;
    tasks->limits = *limits;
    InitializeSRWLock(&tasks->lock); tasks->profile = profile; tasks->elevated = elevation.TokenIsElevated != 0;
    tasks->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (tasks->stop) tasks->monitor = CreateThread(NULL, 0, monitor_tasks, tasks, 0, NULL);
    if (!tasks->monitor) { if (tasks->stop) CloseHandle(tasks->stop); free(tasks); return NULL; }
    return tasks;
}
void sb_win_tasks_free(sb_win_tasks *tasks) {
    if (!tasks) return;
    /* stop/worker 句柄必须活到所有启动线程退出，不能在 late CreateProcess 前失效。 */
    SetEvent(tasks->stop);AcquireSRWLockExclusive(&tasks->lock);
    for(size_t i=0;i<TASK_RECORDS;++i)if(tasks->items[i]){task *item=tasks->items[i];AcquireSRWLockExclusive(&item->lock);if(!item->finished){if(!item->reason)item->reason="cancelled";if(item->job)TerminateJobObject(item->job,125);if(item->starting && item->process)TerminateProcess(item->process,125);SetEvent(item->input_event);}ReleaseSRWLockExclusive(&item->lock);}
    ReleaseSRWLockExclusive(&tasks->lock);WaitForSingleObject(tasks->monitor, INFINITE); CloseHandle(tasks->monitor);
    for(size_t i=0;i<TASK_RECORDS;++i){AcquireSRWLockShared(&tasks->lock);task *item=tasks->items[i];HANDLE archiver=item?item->archiver:NULL;ReleaseSRWLockShared(&tasks->lock);if(archiver)WaitForSingleObject(archiver,INFINITE);}
    for (size_t i = 0; i < TASK_RECORDS; ++i) task_free(tasks->items[i]);
    while(tasks->retired){task *item=tasks->retired;tasks->retired=item->retired_next;task_free(item);}
    for (size_t i=0;i<tasks->archive_count;++i)free(tasks->archives[i].receipt);
    if (tasks->worker) CloseHandle(tasks->worker);CloseHandle(tasks->stop); free(tasks);
}
bool sb_win_tasks_watch_worker(sb_win_tasks *tasks, HANDLE worker) {
    HANDLE copy=NULL;
    if (!tasks || !worker || !DuplicateHandle(GetCurrentProcess(),worker,GetCurrentProcess(),&copy,SYNCHRONIZE,FALSE,0)) return false;
    AcquireSRWLockExclusive(&tasks->lock);
    if (tasks->worker) { ReleaseSRWLockExclusive(&tasks->lock); CloseHandle(copy); return false; }
    tasks->worker=copy; ReleaseSRWLockExclusive(&tasks->lock); return true;
}
void sb_win_tasks_restrict(sb_win_tasks *tasks, sb_win_sandbox *sandbox) { tasks->sandbox=sandbox;tasks->profile=SB_EXEC_CONTROLLED;tasks->elevated=false; }
void sb_win_tasks_store(sb_win_tasks *tasks, sb_store *store, sb_audit *audit) {
    AcquireSRWLockExclusive(&tasks->lock); tasks->store=store; tasks->audit=audit; ReleaseSRWLockExclusive(&tasks->lock);
}
bool sb_win_tasks_cancel_all(sb_win_tasks *tasks) {
    if (!tasks) return true;
    bool ok = true;
    AcquireSRWLockExclusive(&tasks->lock);
    for (size_t i = 0; i < TASK_RECORDS; ++i) {
        task *item = tasks->items[i]; if (!item) continue;
        AcquireSRWLockExclusive(&item->lock);
        if (!item->finished) {
            if(item->starting){if(!item->reason)item->reason="cancelled";if(item->job)TerminateJobObject(item->job,125);if(item->process)TerminateProcess(item->process,125);SetEvent(item->input_event);}
            else if (TerminateJobObject(item->job, 125)) {if(!item->reason){item->reason="cancelled";item->pending_stop_event="task_authorization_cancelled";}SetEvent(item->input_event); }
            else ok = false;
        }
        ReleaseSRWLockExclusive(&item->lock);
    }
    ReleaseSRWLockExclusive(&tasks->lock); return ok;
}
static yyjson_mut_val *snapshot(sb_win_tasks *tasks, task *item, yyjson_mut_doc *doc) {
    yyjson_mut_val *result = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, result, "task_id", item->id);
    yyjson_mut_obj_add_strcpy(doc, result, "request_key", item->key);
    yyjson_mut_obj_add_strcpy(doc, result, "request_sha256", item->hash);
    yyjson_mut_obj_add_str(doc, result, "profile", sb_execution_profile_name(tasks->profile));
    yyjson_mut_obj_add_bool(doc, result, "elevated", tasks->elevated);
    if(tasks->sandbox){yyjson_mut_obj_add_bool(doc,result,"appcontainer",true);yyjson_mut_obj_add_bool(doc,result,"less_privileged_appcontainer",true);yyjson_mut_obj_add_bool(doc,result,"child_processes_allowed",false);}
    yyjson_mut_obj_add_uint(doc, result, "process_id", item->pid);
    yyjson_mut_obj_add_str(doc,result,"launch_mode",item->gui?"gui":"background");
    yyjson_mut_obj_add_str(doc,result,"io_mode",item->terminal_requested?"terminal":"pipe");
    if(item->pty){yyjson_mut_obj_add_uint(doc,result,"terminal_columns",item->columns);yyjson_mut_obj_add_uint(doc,result,"terminal_rows",item->rows);yyjson_mut_obj_add_str(doc,result,"terminal_output","stdout is merged UTF-8 VT bytes; stderr is empty. No terminal screen emulation; query raw output and verify application state.");}
    if(item->gui){DWORD session=0;ProcessIdToSessionId(GetCurrentProcessId(),&session);yyjson_mut_obj_add_uint(doc,result,"session_id",session);
        yyjson_mut_obj_add_str(doc,result,"desktop","WinSta0/Default");yyjson_mut_obj_add_bool(doc,result,"foreground_permission_granted",item->foreground_granted);
        yyjson_mut_obj_add_bool(doc,result,"process_created",item->process!=NULL);yyjson_mut_obj_add_str(doc,result,"window_verification","Use fresh window_list then window_focus; single-instance applications may reuse an existing process.");}
    /* 入口退出不代表整个 Job 已结束；用句柄等待状态区分真实退出码 259。 */
    yyjson_mut_val *entry=yyjson_mut_obj(doc),*job=yyjson_mut_obj(doc);DWORD wait=item->process?WaitForSingleObject(item->process,0):WAIT_FAILED,exit=0,entry_error=0;
    bool exited=wait==WAIT_OBJECT_0,exit_known=false;
    if(exited){exit_known=GetExitCodeProcess(item->process,&exit)!=0;if(!exit_known)entry_error=GetLastError();}
    else if(item->process && wait==WAIT_FAILED)entry_error=GetLastError();
    yyjson_mut_obj_add_uint(doc,entry,"pid",item->pid);yyjson_mut_obj_add_str(doc,entry,"state",!item->process?(item->starting && item->phase==START_CREATING?"creation_pending":"not_created"):exited?"exited":wait==WAIT_TIMEOUT?item->process_resumed?"running":"suspended":"unknown");
    if(exit_known)yyjson_mut_obj_add_uint(doc,entry,"exit_code",exit);else yyjson_mut_obj_add_null(doc,entry,"exit_code");
    yyjson_mut_obj_add_uint(doc,entry,"query_win32_error",entry_error);
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting={0};bool known=item->job && QueryInformationJobObject(item->job,JobObjectBasicAccountingInformation,&accounting,sizeof(accounting),NULL)!=0;DWORD job_error=known?0:item->job?GetLastError():0;
    yyjson_mut_obj_add_str(doc,job,"state",!item->job?"not_created":known?(accounting.ActiveProcesses?"running":"empty"):"unknown");
    if(known)yyjson_mut_obj_add_uint(doc,job,"active_process_count",accounting.ActiveProcesses);else yyjson_mut_obj_add_null(doc,job,"active_process_count");
    yyjson_mut_obj_add_uint(doc,job,"query_win32_error",job_error);
    yyjson_mut_obj_add_val(doc,result,"entry_process",entry);yyjson_mut_obj_add_val(doc,result,"job",job);
    yyjson_mut_obj_add_bool(doc,result,"entry_exited_job_running",exited && known && accounting.ActiveProcesses>0);
    yyjson_mut_obj_add_bool(doc,result,"output_drained",item->output_done[0] && item->output_done[1]);
    yyjson_mut_obj_add_str(doc, result, "status", item->finished ? (item->reason ? item->reason : "completed") : item->starting?"starting":"running");
    yyjson_mut_val *startup=yyjson_mut_obj(doc),*timings=yyjson_mut_obj(doc);uint64_t now=sb_platform_millis();
    yyjson_mut_obj_add_str(doc,startup,"phase",startup_names[item->phase]);yyjson_mut_obj_add_bool(doc,startup,"complete",item->startup_complete);yyjson_mut_obj_add_bool(doc,startup,"process_resumed",item->process_resumed);
    yyjson_mut_obj_add_uint(doc,startup,"elapsed_ms",(item->startup_complete?item->phase_at:now)-item->started);yyjson_mut_obj_add_uint(doc,startup,"phase_elapsed_ms",item->startup_complete?0:now-item->phase_at);
    for(unsigned i=0;i<START_PHASES;++i)if(item->phase_ms[i])yyjson_mut_obj_add_uint(doc,timings,startup_names[i],item->phase_ms[i]);yyjson_mut_obj_add_val(doc,startup,"phase_durations_ms",timings);
    if(item->startup_native_error_known)yyjson_mut_obj_add_uint(doc,startup,"win32_error",item->startup_native_error);else yyjson_mut_obj_add_null(doc,startup,"win32_error");
    if(item->phase==START_FAILED)yyjson_mut_obj_add_str(doc,startup,"failure_phase",startup_names[item->failure_phase]);
    if(*item->startup_error)yyjson_mut_obj_add_strcpy(doc,startup,"error",item->startup_error);yyjson_mut_obj_add_bool(doc,startup,"automatic_replay",false);yyjson_mut_obj_add_val(doc,result,"startup",startup);
    if (item->finished) yyjson_mut_obj_add_uint(doc, result, "exit_code", item->exit_code);
    if (item->reason && !item->finished) yyjson_mut_obj_add_str(doc, result, "termination_requested", item->reason);
    yyjson_mut_obj_add_uint(doc, result, "stdout_bytes", item->lengths[0]);
    yyjson_mut_obj_add_uint(doc, result, "stderr_bytes", item->lengths[1]);
    yyjson_mut_obj_add_bool(doc, result, "stdin_closed", item->input_closed || item->archived);
    yyjson_mut_obj_add_bool(doc, result, "stdin_failed", item->input_failed);
    yyjson_mut_obj_add_uint(doc, result, "timeout_ms", item->timeout);
    yyjson_mut_obj_add_bool(doc, result, "process_tree_job", true);
    yyjson_mut_obj_add_bool(doc, result, "archived", item->archived);
    yyjson_mut_obj_add_bool(doc, result, "output_available", !item->archived);
    if (tasks->store) { yyjson_mut_obj_add_bool(doc,result,"durable",item->durable_reserved); yyjson_mut_obj_add_bool(doc,result,"durable_warning",item->durable_warning); }
    unsigned queued_intents=0;for(size_t i=0;i<item->input_count;++i)if(!item->inputs[i].sent && !item->inputs[i].intent_recorded)++queued_intents;
    yyjson_mut_obj_add_bool(doc,result,"durable_pending",item->persistence_pending || item->monitor_pending || item->pending_stop_event || queued_intents || item->archiving);yyjson_mut_obj_add_uint(doc,result,"persistence_pending_count",item->persistence_pending+item->monitor_pending);yyjson_mut_obj_add_bool(doc,result,"archive_pending",item->archiving);
    return result;
}
static void startup_phase_set(task *item,startup_phase phase){AcquireSRWLockExclusive(&item->lock);uint64_t now=sb_platform_millis();item->phase_ms[item->phase]+=now-item->phase_at;item->phase=phase;item->phase_at=now;ReleaseSRWLockExclusive(&item->lock);}
static bool startup_authorization_stopped(sb_win_tasks *tasks){return WaitForSingleObject(tasks->stop,0)!=WAIT_TIMEOUT || (tasks->worker && WaitForSingleObject(tasks->worker,0)!=WAIT_TIMEOUT);}
static bool startup_cancelled(task *item){AcquireSRWLockExclusive(&item->lock);bool stopped=startup_authorization_stopped(item->owner);if(!item->reason && (stopped || sb_platform_millis()-item->started>=item->timeout))item->reason=stopped?"cancelled":"timed_out";bool cancelled=item->reason!=NULL;ReleaseSRWLockExclusive(&item->lock);return cancelled;}
static bool startup_target(yyjson_val *args,const char *name,const char *version,bool directory,sb_error *error){
    const char *expected=sb_string(yyjson_obj_get(args,version));if(!expected)return true;
    const char *path=sb_string(yyjson_obj_get(args,name));
    /* cwd 来自已打开目录的最终路径；仅归一化受信任本地盘的 extended 前缀。 */
    if(path && strlen(path)>6 && !strncmp(path,"\\\\?\\",4) && ((path[4]>='A' && path[4]<='Z') || (path[4]>='a' && path[4]<='z')) && path[5]==':' && path[6]=='\\')path+=4;
    char *relative=NULL,actual[65];sb_root *root=sb_platform_account_parent(path,&relative,error);sb_file *file=root?sb_platform_open(root,relative,directory,error):NULL;
    bool ok=file && sb_platform_file_version(file,actual,error) && !strcmp(actual,expected);sb_platform_close(file);sb_platform_root_close(root);free(relative);return ok;
}
static DWORD WINAPI startup_thread(void *parameter){
    task *item=parameter;sb_win_tasks *tasks=item->owner;yyjson_doc *doc=yyjson_read(item->command,strlen(item->command),0);yyjson_val *args=yyjson_doc_get_root(doc);
    wchar_t *env=NULL,*exe=NULL,*directory=NULL,*line=NULL;HANDLE child_input=NULL,child_output[2]={NULL,NULL},program_guard=INVALID_HANDLE_VALUE,cwd_guard=INVALID_HANDLE_VALUE;LPPROC_THREAD_ATTRIBUTE_LIST attributes=NULL;bool initialized=false,ok=doc!=NULL;DWORD native_error=0;bool native_known=false;const char *failure="Cannot decode reserved task startup.";PROCESS_INFORMATION process={0};HRESULT terminal_error=S_OK;
    bool terminal=!strcmp(sb_string(yyjson_obj_get(args,"io_mode"))?sb_string(yyjson_obj_get(args,"io_mode")):"pipe","terminal");uint64_t columns=120,rows=30;sb_error error={0};
    startup_phase_set(item,START_RESERVING);
    if(ok && tasks->store){ok=sb_audit_task(tasks->audit,"task_operation_requested",item->actor,item->key,item->id,tasks->profile,item->hash,"task_start",0);failure="Task request audit could not be persisted; no process was created.";}
    if(ok && tasks->store){ok=startup_event(item,"task_start_reserved","starting");AcquireSRWLockExclusive(&item->lock);item->durable_reserved=ok;ReleaseSRWLockExclusive(&item->lock);failure="Cannot persist startup reservation; no process was created and this instance retains the request key.";}
    if(!ok || startup_cancelled(item))goto done;
    startup_phase_set(item,START_ENVIRONMENT);failure="Cannot prepare task environment or approved target changed before startup.";
    ok=startup_target(args,"executable","executable_version",false,&error) && startup_target(args,"cwd","cwd_version",true,&error);
    const char *runtime=sb_string(yyjson_obj_get(args,"runtime_directory"));char *fallback=runtime?NULL:sb_platform_default_data_dir(&error);const char *excluded[]={sb_string(yyjson_obj_get(args,"token_env"))};
    if(ok)env=runtime || fallback?sb_platform_child_environment(runtime?runtime:fallback,excluded,1,&error):NULL;free(fallback);
    exe=to_wide(sb_string(yyjson_obj_get(args,"executable")));directory=to_wide(sb_string(yyjson_obj_get(args,"cwd")));line=calloc(SB_WINDOWS_COMMAND_CAP,sizeof(wchar_t));size_t used=0;
    ok=ok && absolute_exe(exe) && directory && line && env && (!tasks->sandbox || sb_win_sandbox_program(tasks->sandbox,exe)) && quote(line,&used,exe);
    yyjson_val *argv=yyjson_obj_get(args,"argv"),*argument;size_t idx,max;yyjson_arr_foreach(argv,idx,max,argument){wchar_t *wide=to_wide(sb_string(argument));ok=ok && wide && quote(line,&used,wide);free(wide);}
    if(!ok || startup_cancelled(item))goto done;
    if(item->gui && (tasks->profile!=SB_EXEC_USER || tasks->sandbox || !gui_desktop())){failure="gui_desktop_unavailable: same-session ordinary WinSta0/Default in active/disconnected state required; no unlock or cross-session fallback.";ok=false;goto done;}
    startup_phase_set(item,START_CONTAINMENT);failure="Cannot prepare task pipes or Job containment.";
    item->output_cap=tasks->limits.output_bytes;item->capacities[0]=item->capacities[1]=item->output_cap<SB_TEXT_LIMIT?item->output_cap:SB_TEXT_LIMIT;
    item->bytes[0]=malloc(item->capacities[0]);item->bytes[1]=malloc(item->capacities[1]);SECURITY_ATTRIBUTES security={sizeof(security),NULL,TRUE};
    /* 仅失败 API 紧邻读取错误码；分配失败、成功 API 的残留值不冒充原因。 */
#define START_API(call) ((call)?true:(native_error=GetLastError(),native_known=true,false))
    ok=item->bytes[0] && item->bytes[1] && START_API(CreatePipe(&child_input,&item->input,&security,4096)) && START_API(SetHandleInformation(item->input,HANDLE_FLAG_INHERIT,0));
    for(unsigned i=0;ok && i<2;++i)ok=START_API(CreatePipe(&item->output[i],&child_output[i],&security,4096)) && START_API(SetHandleInformation(item->output[i],HANDLE_FLAG_INHERIT,0));
    HANDLE job=CreateJobObjectW(NULL,NULL);JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits={0};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE|JOB_OBJECT_LIMIT_ACTIVE_PROCESS;limits.BasicLimitInformation.ActiveProcessLimit=tasks->sandbox?1:64;
    if(!job){native_error=GetLastError();native_known=true;}ok=ok && job && START_API(SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits)));
    AcquireSRWLockExclusive(&item->lock);item->job=job;ReleaseSRWLockExclusive(&item->lock);
    if(ok && terminal){HPCON pty=NULL;sb_integer(yyjson_obj_get(args,"terminal_columns"),&columns);sb_integer(yyjson_obj_get(args,"terminal_rows"),&rows);terminal_error=sb_pty_create((COORD){(SHORT)columns,(SHORT)rows},child_input,child_output[0],&pty);ok=SUCCEEDED(terminal_error);AcquireSRWLockExclusive(&item->lock);item->pty=pty;item->columns=(SHORT)columns;item->rows=(SHORT)rows;ReleaseSRWLockExclusive(&item->lock);}
    DWORD attribute_count=tasks->sandbox?5:2;SIZE_T attribute_size=0;InitializeProcThreadAttributeList(NULL,attribute_count,0,&attribute_size);attributes=attribute_size?malloc(attribute_size):NULL;initialized=attributes && START_API(InitializeProcThreadAttributeList(attributes,attribute_count,0,&attribute_size));
    HANDLE inherited[]={child_input,child_output[0],child_output[1]};ok=ok && initialized && (terminal?START_API(UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,item->pty,sizeof(item->pty),NULL,NULL)):START_API(UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof(inherited),NULL,NULL)));
    /* Win10+ 在原生创建内原子入 Job，关闭 suspended 创建返回前的孤儿进程空隙。 */
    HANDLE startup_jobs[]={item->job};ok=ok && START_API(UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_JOB_LIST,startup_jobs,sizeof(startup_jobs),NULL,NULL));
    if(ok && tasks->sandbox)ok=sb_win_sandbox_attributes(tasks->sandbox,attributes);
#undef START_API
    if(!ok || startup_cancelled(item))goto done;
    STARTUPINFOEXW startup={0};startup.StartupInfo.cb=sizeof(startup);startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES;startup.StartupInfo.hStdInput=child_input;startup.StartupInfo.hStdOutput=child_output[0];startup.StartupInfo.hStdError=child_output[1];startup.lpAttributeList=attributes;
    /* 显式 NULL stdio 使 ConPTY 接管；清掉该标志会继承 console 父的 NUL/重定向句柄。 */
    if(terminal){startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES;startup.StartupInfo.hStdInput=startup.StartupInfo.hStdOutput=startup.StartupInfo.hStdError=NULL;}
    if(item->gui){startup.StartupInfo.lpDesktop=L"winsta0\\default";startup.StartupInfo.dwFlags|=STARTF_USESHOWWINDOW;startup.StartupInfo.wShowWindow=SW_SHOWNORMAL;}
    /* 准备环境/终端也可能等待；贴近真实创建再次核验已审批文件和目录。 */
    if(yyjson_obj_get(args,"executable_version")){
        program_guard=CreateFileW(exe,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,NULL);
        if(program_guard==INVALID_HANDLE_VALUE){native_error=GetLastError();native_known=true;failure="Cannot hold the approved program against concurrent replacement before startup.";ok=false;goto done;}
    }
    if(yyjson_obj_get(args,"cwd_version")){
        cwd_guard=CreateFileW(directory,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
        if(cwd_guard==INVALID_HANDLE_VALUE){native_error=GetLastError();native_known=true;failure="Cannot hold the approved working directory before startup.";ok=false;goto done;}
    }
    if(!startup_target(args,"executable","executable_version",false,&error) || !startup_target(args,"cwd","cwd_version",true,&error)){ok=false;failure="Approved program or working directory changed during startup preparation.";goto done;}
    startup_phase_set(item,START_CREATING);failure="Cannot create the suspended task process.";DWORD flags=CREATE_SUSPENDED|(tasks->sandbox?DETACHED_PROCESS:terminal?0:CREATE_NO_WINDOW)|CREATE_UNICODE_ENVIRONMENT|EXTENDED_STARTUPINFO_PRESENT;
    ok=tasks->sandbox?sb_win_sandbox_launch(tasks->sandbox,exe,line,true,flags,env,directory,&startup.StartupInfo,&process):CreateProcessW(exe,line,NULL,NULL,terminal?FALSE:TRUE,flags,env,directory,&startup.StartupInfo,&process);
    if(!ok){native_error=GetLastError();native_known=true;goto done;}
    AcquireSRWLockExclusive(&item->lock);item->process=process.hProcess;item->pid=process.dwProcessId;ReleaseSRWLockExclusive(&item->lock);
    startup_phase_set(item,START_ASSIGNING);failure="Suspended task is not in its creation-time Job.";BOOL contained=FALSE;ok=IsProcessInJob(item->process,item->job,&contained)!=FALSE;if(!ok){native_error=GetLastError();native_known=true;}ok=ok && contained;if(!ok)goto done;
    if(tasks->sandbox && !sb_win_sandbox_token(tasks->sandbox,item->process)){ok=false;failure="Suspended task token failed its containment identity check.";goto done;}
    if(startup_cancelled(item))goto done;
    startup_phase_set(item,START_READERS);failure="Cannot start task I/O threads.";
    if(child_input){CloseHandle(child_input);child_input=NULL;}for(unsigned i=0;i<2;++i)if(child_output[i]){CloseHandle(child_output[i]);child_output[i]=NULL;}
    for(unsigned i=0;ok && i<2;++i){reader_context *context=malloc(sizeof(*context));if(context){context->item=item;context->stream=i;item->readers[i]=CreateThread(NULL,0,read_output,context,0,NULL);if(!item->readers[i]){native_error=GetLastError();native_known=true;free(context);}}ok=item->readers[i]!=NULL;}
    if(ok){item->writer=CreateThread(NULL,0,write_input,item,0,NULL);ok=item->writer!=NULL;if(!ok){native_error=GetLastError();native_known=true;}}
    if(!ok || startup_cancelled(item))goto done;
    startup_phase_set(item,START_INTENT);failure="Cannot persist task start intent; the suspended process was not resumed.";ok=startup_event(item,"task_start_intent","starting");if(!ok || startup_cancelled(item))goto done;
    startup_phase_set(item,START_RESUMING);failure="Cannot resume contained task process.";
    AcquireSRWLockExclusive(&item->lock);
    if(item->reason || startup_authorization_stopped(tasks) || sb_platform_millis()-item->started>=item->timeout){if(!item->reason)item->reason=startup_authorization_stopped(tasks)?"cancelled":"timed_out";ok=false;}
    else{if(item->gui)item->foreground_granted=AllowSetForegroundWindow(process.dwProcessId)!=FALSE;ok=ResumeThread(process.hThread)!=(DWORD)-1;if(!ok){native_error=GetLastError();native_known=true;}item->process_resumed=ok;}
    ReleaseSRWLockExclusive(&item->lock);if(!ok)goto done;
    startup_phase_set(item,START_RECORDING);failure="Task resumed but its started receipt could not be persisted; inspect this task, never replay it.";ok=startup_event(item,"task_started","running");
done:
    if(initialized)DeleteProcThreadAttributeList(attributes);free(attributes);if(program_guard!=INVALID_HANDLE_VALUE)CloseHandle(program_guard);if(cwd_guard!=INVALID_HANDLE_VALUE)CloseHandle(cwd_guard);if(child_input)CloseHandle(child_input);for(unsigned i=0;i<2;++i)if(child_output[i])CloseHandle(child_output[i]);free(exe);free(directory);free(line);free(env);if(process.hThread)CloseHandle(process.hThread);yyjson_doc_free(doc);
    bool cancelled=startup_cancelled(item);if(!ok || cancelled){if(item->job)TerminateJobObject(item->job,125);if(item->process)TerminateProcess(item->process,125);SetEvent(item->input_event);}
    AcquireSRWLockExclusive(&item->lock);
    if(!ok && !item->reason)item->reason="failed";
    if(!ok && !cancelled){snprintf(item->startup_error,sizeof(item->startup_error),"%s%s",failure,*error.text?error.text:"");if(FAILED(terminal_error))snprintf(item->startup_error,sizeof(item->startup_error),"CreatePseudoConsole failed: HRESULT 0x%08lx. No task process resumed.",(unsigned long)terminal_error);}
    item->startup_native_error=native_error;item->startup_native_error_known=native_known;bool resumed=item->process_resumed;
    if(!resumed){item->output_done[0]=item->readers[0]==NULL;item->output_done[1]=item->readers[1]==NULL;if(!item->writer)item->input_closed=true;}
    ReleaseSRWLockExclusive(&item->lock);
    /* 启动失败也立即排空并关闭 ConPTY；不能把 conhost 留到调用方归档。 */
    if((!ok || cancelled) && item->pty){terminal_drain(item);close_terminal(item);}
    if(!ok || cancelled)startup_event(item,"task_start_failed",item->reason?item->reason:"failed");
    AcquireSRWLockExclusive(&item->lock);item->failure_phase=item->phase;ReleaseSRWLockExclusive(&item->lock);startup_phase_set(item,!ok || cancelled?START_FAILED:START_READY);
    AcquireSRWLockExclusive(&item->lock);item->startup_complete=true;item->starting=false;
    if(!item->process && item->output_done[0] && item->output_done[1]){item->exit_code=125;item->finished=true;}
    ReleaseSRWLockExclusive(&item->lock);SetEvent(item->start_ready);return 0;
}

static char *start(sb_win_tasks *tasks, yyjson_val *args) {
    yyjson_val *mode_value=yyjson_obj_get(args,"launch_mode");const char *launch=sb_string(mode_value);
    if(mode_value && (!launch || (strcmp(launch,"background") && strcmp(launch,"gui"))))return fail("Invalid normalized launch_mode.");
    bool gui=launch && !strcmp(launch,"gui");
    const char *io=sb_string(yyjson_obj_get(args,"io_mode"));bool terminal=io && !strcmp(io,"terminal");uint64_t columns=120,rows=30;
    if(terminal && (gui || tasks->profile!=SB_EXEC_USER || tasks->sandbox || !sb_pty_available() || !sb_integer(yyjson_obj_get(args,"terminal_columns"),&columns) || !columns || columns>1000 || !sb_integer(yyjson_obj_get(args,"terminal_rows"),&rows) || !rows || rows>1000))return fail("conpty_unavailable_or_invalid_size: Windows 10 1809+, ordinary current_user, background launch and 1..1000 cells required.");
    const char *key = sb_string(yyjson_obj_get(args, "request_key")), *actor = sb_string(yyjson_obj_get(args, "actor"));
    const char *hash = sb_string(yyjson_obj_get(args, "request_sha256"));
    for(size_t i=0;key && actor && hash && i<tasks->archive_count;++i){
        archived_task *a=&tasks->archives[i];if(strcmp(a->actor,actor) || strcmp(a->key,key))continue;
        if(strcmp(a->hash,hash))return fail("request_key already binds a different archived request.");
        yyjson_doc *saved=yyjson_read(a->receipt,strlen(a->receipt),0);yyjson_mut_doc *doc=saved?yyjson_doc_mut_copy(saved,NULL):NULL;yyjson_doc_free(saved);
        if(!doc)return NULL;yyjson_mut_val *root=yyjson_mut_doc_get_root(doc);yyjson_mut_obj_add_bool(doc,root,"created_new",false);return serialize(doc,root);
    }
    const char *executable = sb_string(yyjson_obj_get(args, "executable")), *cwd = sb_string(yyjson_obj_get(args, "cwd"));
    const char *token_env = sb_string(yyjson_obj_get(args, "token_env"));
    yyjson_val *argv = yyjson_obj_get(args, "argv"); uint64_t timeout = 0;
    if (!key_valid(key) || !actor || !*actor || strlen(actor) >= 64 || !hash || strlen(hash) != 64 || strspn(hash,"0123456789abcdef") != 64 ||
        !token_env || !sb_integer(yyjson_obj_get(args, "timeout_ms"), &timeout) || timeout < 100 || timeout > tasks->limits.task_timeout_ms || !yyjson_is_arr(argv) || yyjson_arr_size(argv) > SB_TASK_ARG_LIMIT) return fail("Invalid normalized task request.");
    size_t slot = TASK_RECORDS, active = 0;
    for (size_t i = 0; i < TASK_RECORDS; ++i) {
        task *old = tasks->items[i]; if (!old) { if (slot == TASK_RECORDS && i < tasks->limits.task_records) slot = i; continue; }
        AcquireSRWLockExclusive(&old->lock);
        bool same = !strcmp(old->actor,actor) && !strcmp(old->key,key);
        if (!old->finished) ++active;
        if (same) {
            char *result;
            if (strcmp(old->hash,hash)) result = fail("request_key already binds a different request.");
            else { yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL); if(doc) { yyjson_mut_val *value=snapshot(tasks,old,doc); yyjson_mut_obj_add_bool(doc,value,"created_new",false); result=serialize(doc,value); } else result=NULL; }
            ReleaseSRWLockExclusive(&old->lock); return result;
        }
        ReleaseSRWLockExclusive(&old->lock);
    }
    if (slot >= tasks->limits.task_records || active >= tasks->limits.active_tasks) {
        yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL);if(!doc)return NULL;yyjson_mut_val *root=yyjson_mut_obj(doc);
        yyjson_mut_obj_add_bool(doc,root,"_error",true);yyjson_mut_obj_add_str(doc,root,"code","task_budget_exceeded");
        yyjson_mut_obj_add_str(doc,root,"budget_kind",active>=tasks->limits.active_tasks?"active_tasks":"task_records");
        yyjson_mut_obj_add_str(doc,root,"error",active>=tasks->limits.active_tasks?"Active task limit reached; wait for completion or cancel an owned task.":"Task record limit reached; use task_list, read needed output, then task_archive ended tasks.");
        return serialize(doc,root);
    }
    wchar_t *validated=to_wide(executable);uint64_t command_length=validated?quoted_size(validated):0;bool valid=absolute_exe(validated) && cwd && *cwd;free(validated);
    size_t index,count;yyjson_val *arg;yyjson_arr_foreach(argv,index,count,arg){
        const char *text=sb_string(arg);int units=text?MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text,-1,NULL,0):0;
        if(units>(int)SB_WINDOWS_COMMAND_CAP){char field[40];snprintf(field,sizeof(field),"argv[%zu]",index);return limit_fail(field,(uint64_t)units-1,SB_WINDOWS_COMMAND_CAP-1,"UTF-16 units");}
        wchar_t *wide=to_wide(text);if(wide)command_length+=1+quoted_size(wide);valid=valid && wide;free(wide);
    }
    if(command_length>=SB_WINDOWS_COMMAND_CAP)return limit_fail("command_line",command_length,SB_WINDOWS_COMMAND_CAP-1,"UTF-16 units (quoted executable and argv)");
    if(!valid)return fail("Executable must be an absolute local .exe path; the quoted command line must fit 32766 UTF-16 units. Use fs_write/artifact_upload plus a script path, or task_input for large source text.");
    task *item=calloc(1,sizeof(*item));if(!item)return fail("Cannot allocate task startup record.");
    InitializeSRWLock(&item->lock);InitializeSRWLock(&item->pty_lock);InitializeSRWLock(&item->event_lock);InitializeConditionVariable(&item->event_condition);item->owner=tasks;item->gui=gui;item->terminal_requested=terminal;item->columns=(SHORT)columns;item->rows=(SHORT)rows;item->starting=true;item->phase=START_QUEUED;item->started=item->phase_at=sb_platform_millis();item->timeout=timeout;
    strcpy(item->key,key);strcpy(item->hash,hash);strcpy(item->actor,actor);item->command=yyjson_val_write(args,0,NULL);
    unsigned char random[16];sb_error error={0};bool ok=item->command && sb_platform_random(random,sizeof(random),&error);
    for(unsigned i=0;ok && i<16;++i)snprintf(item->id+i*2,3,"%02x",random[i]);
    item->input_event=CreateEventW(NULL,FALSE,FALSE,NULL);item->start_ready=CreateEventW(NULL,TRUE,FALSE,NULL);ok=ok && item->input_event && item->start_ready;
    tasks->items[slot]=item;if(ok)item->starter=CreateThread(NULL,0,startup_thread,item,0,NULL);
    if(!item->starter){tasks->items[slot]=NULL;item->starting=false;item->finished=true;task_free(item);return fail("Cannot schedule task startup; no process was created.");}
    /* 兼容快速启动；慢原生调用留在专用线程，绝不占住整个请求通道。 */
    WaitForSingleObject(item->start_ready,START_REPLY_MS);
    yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL);if(!doc)return NULL;AcquireSRWLockExclusive(&item->lock);yyjson_mut_val *result=snapshot(tasks,item,doc);yyjson_mut_obj_add_bool(doc,result,"created_new",true);
    if(item->startup_complete && *item->startup_error){yyjson_mut_obj_add_bool(doc,result,"_error",true);yyjson_mut_obj_add_strcpy(doc,result,"error",item->startup_error);}
    ReleaseSRWLockExclusive(&item->lock);return serialize(doc,result);
}
static DWORD WINAPI archive_thread(void *parameter){
    task *item=parameter;sb_win_tasks *tasks=item->owner;AcquireSRWLockExclusive(&item->lock);
    yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL);yyjson_mut_val *body=doc?snapshot(tasks,item,doc):NULL;
    if(body){yyjson_mut_obj_remove_str(body,"archived");yyjson_mut_obj_add_bool(doc,body,"archived",true);yyjson_mut_obj_remove_str(body,"output_available");yyjson_mut_obj_add_bool(doc,body,"output_available",false);yyjson_mut_obj_remove_str(body,"archive_pending");yyjson_mut_obj_add_bool(doc,body,"archive_pending",false);yyjson_mut_obj_remove_str(body,"durable_pending");yyjson_mut_obj_add_bool(doc,body,"durable_pending",false);yyjson_mut_obj_remove_str(body,"stdin_closed");yyjson_mut_obj_add_bool(doc,body,"stdin_closed",true);}
    char *receipt=body?yyjson_mut_val_write(body,0,NULL):NULL;yyjson_mut_doc_free(doc);
    bool ok=receipt && task_event(item,"task_archived",item->reason?item->reason:"completed",false);
    ReleaseSRWLockExclusive(&item->lock);AcquireSRWLockExclusive(&tasks->lock);AcquireSRWLockExclusive(&item->lock);--tasks->archiving_count;item->archiving=false;item->archive_failed=!ok;
    if(ok){archived_task *saved=&tasks->archives[tasks->archive_count++];strcpy(saved->id,item->id);strcpy(saved->key,item->key);strcpy(saved->hash,item->hash);strcpy(saved->actor,item->actor);saved->receipt=receipt;item->archive_result=receipt;item->archived=true;for(size_t i=0;i<TASK_RECORDS;++i)if(tasks->items[i]==item)tasks->items[i]=NULL;SetEvent(item->input_event);item->retired_next=tasks->retired;tasks->retired=item;}
    else free(receipt);
    SetEvent(item->archive_ready);ReleaseSRWLockExclusive(&item->lock);ReleaseSRWLockExclusive(&tasks->lock);return 0;
}
/* 调用时持 global+item；返回前必释放，等待绝不持全局锁。 */
static char *archive_request(sb_win_tasks *tasks,task *item){
    const char *error=NULL;
    if(!item->finished)error="task_still_running: only ended tasks can be archived; cancel and wait for termination first.";
    else if(!item->archiving && (item->persistence_pending || item->monitor_pending || item->pending_stop_event || (item->writer && WaitForSingleObject(item->writer,0)!=WAIT_OBJECT_0)))error="task_persistence_pending: terminal/input events are still settling or being flushed; retry this same task_id.";
    else if(!item->archiving && tasks->archive_count+tasks->archiving_count==ARCHIVE_RECORDS)error="archive_receipt_budget_exceeded: compact request_key receipts are full; no record was discarded.";
    if(error){ReleaseSRWLockExclusive(&item->lock);ReleaseSRWLockExclusive(&tasks->lock);return fail(error);}
    if(!item->archiving){
        if(item->archiver){CloseHandle(item->archiver);item->archiver=NULL;}
        if(!item->archive_ready)item->archive_ready=CreateEventW(NULL,TRUE,FALSE,NULL);else ResetEvent(item->archive_ready);
        item->archiving=true;item->archive_failed=false;++tasks->archiving_count;
        if(item->archive_ready)item->archiver=CreateThread(NULL,0,archive_thread,item,0,NULL);
        if(!item->archiver){item->archiving=false;--tasks->archiving_count;ReleaseSRWLockExclusive(&item->lock);ReleaseSRWLockExclusive(&tasks->lock);return fail("Cannot schedule archive persistence; the task and output remain available.");}
    }
    InterlockedIncrement(&item->references);ReleaseSRWLockExclusive(&item->lock);ReleaseSRWLockExclusive(&tasks->lock);WaitForSingleObject(item->archive_ready,START_REPLY_MS);
    AcquireSRWLockExclusive(&item->lock);char *reply=item->archived?sb_strdup(item->archive_result):fail(item->archiving?"task_archive_pending: archive is still being flushed; retry the same task_id. No record or output was released yet.":"Cannot persist archive receipt; the task and its output remain available.");ReleaseSRWLockExclusive(&item->lock);InterlockedDecrement(&item->references);return reply;
}

static char *tasks_call(sb_win_tasks *tasks,const char *name,yyjson_val *args) {
    if(!tasks || !name || !yyjson_is_obj(args)) return fail("Task backend unavailable.");
    AcquireSRWLockExclusive(&tasks->lock);
    if(!strcmp(name,"task_start")) { char *result=start(tasks,args); ReleaseSRWLockExclusive(&tasks->lock); return result; }
    if(!strcmp(name,"task_list")){char *reply=task_list(tasks,args);ReleaseSRWLockExclusive(&tasks->lock);return reply;}
    const char *id=sb_string(yyjson_obj_get(args,"task_id")),*actor=sb_string(yyjson_obj_get(args,"actor")); task *item=NULL;
    for(size_t i=0;id && actor && i<TASK_RECORDS;++i) if(tasks->items[i] && !strcmp(tasks->items[i]->id,id) && !strcmp(tasks->items[i]->actor,actor)) item=tasks->items[i];
    for(size_t i=0;!item && id && actor && i<tasks->archive_count;++i){archived_task *a=&tasks->archives[i];if(strcmp(a->id,id) || strcmp(a->actor,actor))continue;
        char *reply=(!strcmp(name,"task_status") || !strcmp(name,"task_archive") || !strcmp(name,"task_cancel"))?sb_strdup(a->receipt):fail("task_archived: live output and stdin were released; only the compact receipt and request_key remain. Save needed output before archiving.");
        ReleaseSRWLockExclusive(&tasks->lock);return reply;
    }
    if(!item) { ReleaseSRWLockExclusive(&tasks->lock); return fail("Unknown task_id for this caller or host instance."); }
    AcquireSRWLockExclusive(&item->lock);
    if(!strcmp(name,"task_archive"))return archive_request(tasks,item);
    yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL); yyjson_mut_val *result=doc?snapshot(tasks,item,doc):NULL; const char *error=NULL;
    if(!doc) { ReleaseSRWLockExclusive(&item->lock); ReleaseSRWLockExclusive(&tasks->lock); return NULL; }
    if(!strcmp(name,"task_cancel")) {
        if(!item->finished && !item->reason) { if(item->starting){item->reason="cancelled";if(item->job)TerminateJobObject(item->job,125);if(item->process)TerminateProcess(item->process,125);SetEvent(item->input_event);}else if(TerminateJobObject(item->job,125)) { item->reason="cancelled";item->pending_stop_event="task_cancel_requested";SetEvent(item->input_event); } else error="Cannot terminate the task job."; }
        result=snapshot(tasks,item,doc);
    } else if(!strcmp(name,"task_output")) {
        const char *encoding=sb_string(yyjson_obj_get(args,"text_encoding"));UINT cp=codepage(encoding);
        if(encoding && !cp){yyjson_mut_doc_free(doc);ReleaseSRWLockExclusive(&item->lock);ReleaseSRWLockExclusive(&tasks->lock);return fail("Unsupported encoding: use utf-8, cp936/gbk or a supported Windows cpNNNN.");}
        const char *payload=sb_string(yyjson_obj_get(args,"payload_mode"));bool raw=!payload || strcmp(payload,"text"),text=payload && strcmp(payload,"bytes");
        if(yyjson_obj_get(args,"payload_mode") && (!payload || (strcmp(payload,"text") && strcmp(payload,"bytes") && strcmp(payload,"both")))){yyjson_mut_doc_free(doc);ReleaseSRWLockExclusive(&item->lock);ReleaseSRWLockExclusive(&tasks->lock);return fail("payload_mode must be text, bytes or both.");}
        const char *stream=sb_string(yyjson_obj_get(args,"stream")); unsigned selected=stream && !strcmp(stream,"stderr")?1:0; uint64_t offset=0,maximum=0;
        if(!stream || (strcmp(stream,"stdout") && strcmp(stream,"stderr")) || !sb_integer(yyjson_obj_get(args,"offset"),&offset) || !sb_integer(yyjson_obj_get(args,"max_bytes"),&maximum) || maximum<1 || maximum>SB_TASK_OUTPUT_PAGE_LIMIT || offset>item->lengths[selected]) error="Invalid output stream, byte offset or limit.";
        else {
            size_t length=item->lengths[selected]-(size_t)offset; if(length>maximum) length=(size_t)maximum;
            const unsigned char *retained=item->bytes[selected]?item->bytes[selected]:(const unsigned char*)"";
            char *encoded=raw?sb_base64(retained+(size_t)offset,length):NULL;
            if(raw && !encoded) error="Cannot encode task output.";
            else { if(raw){yyjson_mut_obj_add_strcpy(doc,result,"data_base64",encoded);free(encoded);yyjson_mut_obj_add_str(doc,result,"encoding","base64");}
                const unsigned char *page=retained+(size_t)offset;
                if((encoding && !payload) || text)decoded_output(doc,result,page,length,cp?cp:CP_UTF8,offset,item->finished && offset+length==item->lengths[selected]);
                if(payload)yyjson_mut_obj_add_strcpy(doc,result,"payload_mode",payload);
                if (!payload && (!length || (!memchr(page,0,length) && MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,(const char *)page,(int)length,NULL,0))))
                    yyjson_mut_obj_add_strncpy(doc,result,"text_utf8",(const char *)page,length); yyjson_mut_obj_add_uint(doc,result,"next_offset",offset+length); yyjson_mut_obj_add_bool(doc,result,"eof",item->finished && offset+length==item->lengths[selected]); }
        }
    } else if(!strcmp(name,"task_input")) {
        const char *key=sb_string(yyjson_obj_get(args,"input_key")),*text=sb_string(yyjson_obj_get(args,"text")); yyjson_val *eof_value=yyjson_obj_get(args,"eof");
        uint64_t columns=0,rows=0;yyjson_val *cv=yyjson_obj_get(args,"terminal_columns"),*rv=yyjson_obj_get(args,"terminal_rows");
        bool dimensions=(!cv && !rv) || (item->pty && sb_integer(cv,&columns) && columns && columns<=1000 && sb_integer(rv,&rows) && rows && rows<=1000);
        bool eof=yyjson_get_bool(eof_value); size_t length=text?strlen(text):0; char hash[65]; sb_error hash_error={0}; task_input *existing=NULL; size_t queued=0;
        if(item->starting)error="task_starting: inspect startup.phase and wait for a running task before sending input; no input was queued.";
        else if (tasks->store && (!sb_store_healthy(tasks->store) || !sb_audit_healthy(tasks->audit))) error="Persistent state or audit unavailable; stdin was not queued.";
        else if(item->pty && eof)error="terminal_eof_unsupported: send application exit input or task_cancel; no input queued.";
        else if((cv || rv) && !item->pty)error="terminal_resize_requires_terminal_task";
        else if(!dimensions || !key_valid(key) || !text || length>SB_TASK_INPUT_LIMIT || !yyjson_is_bool(eof_value) || (!length && !eof && !columns) || !sb_platform_sha256(text,length,hash,&hash_error)) error="Invalid input key, UTF-8 text or EOF flag.";
        else {
            for(size_t i=0;i<item->input_count;++i) { if(item->inputs[i].data) queued+=item->inputs[i].length; if(!strcmp(item->inputs[i].key,key)) existing=&item->inputs[i]; }
            if(existing && (strcmp(existing->hash,hash) || existing->eof!=eof || existing->columns!=columns || existing->rows!=rows)) error="input_key already binds different input.";
            else if(!existing && (item->finished || item->reason || item->input_closed || (item->input_count && item->inputs[item->input_count-1].eof))) error="Task stdin is closed or closing.";
            else if(!existing && length>SB_TASK_INPUT_QUEUE_LIMIT-queued) error="Stdin queue is full; wait for delivery and retry the same input_key.";
            else if(!existing && item->input_count>=INPUT_RECORDS) error="Task input receipt budget reached.";
            else if(!existing) {
                char *copy=sb_strdup(text); if(!copy) error="Cannot queue stdin data.";
                else {
                    existing=&item->inputs[item->input_count++];strcpy(existing->key,key);strcpy(existing->hash,hash);existing->data=copy;existing->length=(DWORD)length;existing->eof=eof;existing->columns=(SHORT)columns;existing->rows=(SHORT)rows;
                    SetEvent(item->input_event);
                }
            }
            if(!error && existing) { result=snapshot(tasks,item,doc);yyjson_mut_obj_add_strcpy(doc,result,"input_key",key); yyjson_mut_obj_add_str(doc,result,"input_status",existing->sent?(existing->delivered?"delivered":"failed"):"queued");if(existing->columns && existing->sent)yyjson_mut_obj_add_uint(doc,result,"terminal_resize_hresult",(uint32_t)existing->resize_hresult); }
        }
    } else if(strcmp(name,"task_status")) error="Unknown task operation.";
    ReleaseSRWLockExclusive(&item->lock); ReleaseSRWLockExclusive(&tasks->lock);
    if(error) { yyjson_mut_doc_free(doc); return fail(error); } return serialize(doc,result);
}
char *sb_win_tasks_call(sb_win_tasks *tasks,const char *name,yyjson_val *args) {
    char *wire=tasks_call(tasks,name,args);if(!tasks || !wire)return wire;
    yyjson_doc *input=yyjson_read(wire,strlen(wire),0);yyjson_mut_doc *doc=input?yyjson_doc_mut_copy(input,NULL):NULL;yyjson_doc_free(input);
    if(!doc)return wire;free(wire);yyjson_mut_val *root=yyjson_mut_doc_get_root(doc);yyjson_mut_obj_remove_str(root,"task_budget");
    AcquireSRWLockShared(&tasks->lock);budget(tasks,doc,root);ReleaseSRWLockShared(&tasks->lock);return serialize(doc,root);
}
