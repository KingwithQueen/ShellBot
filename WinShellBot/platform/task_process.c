#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "sb.h"
#include "execution.h"
#include "task_process.h"
#include "sandbox.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define TASK_RECORDS 32
#define TASK_ACTIVE 8
#define ARCHIVE_RECORDS 4096
#define OUTPUT_CAP 262144u
#define INPUT_RECORDS 1024
typedef struct { char key[65], hash[65]; char *data; DWORD length; bool eof, sent, delivered; } task_input;
typedef struct task {
    SRWLOCK lock;
    char id[33], key[65], hash[65], actor[64];
    HANDLE job, process, input, output[2], readers[2], writer, input_event;
    unsigned char *bytes[2]; size_t lengths[2], capacities[2], output_cap;
    bool output_done[2], input_closed, input_failed, finished;
    DWORD pid, exit_code; uint64_t started, timeout;
    const char *reason;
    task_input inputs[INPUT_RECORDS]; size_t input_count;
    sb_win_tasks *owner;
    char *command;
    bool durable_reserved, durable_warning, archived;
    struct task *retired_next;
} task;
typedef struct { char id[33],key[65],hash[65],actor[64]; char *receipt; } archived_task;
struct sb_win_tasks { SRWLOCK lock; task *items[TASK_RECORDS],*retired; archived_task archives[ARCHIVE_RECORDS]; size_t archive_count; HANDLE stop, monitor, worker; sb_execution_profile profile; bool elevated; sb_resource_limits limits; sb_win_sandbox *sandbox; sb_store *store; sb_audit *audit; };
typedef struct { task *item; unsigned stream; } reader_context;
static yyjson_mut_val *snapshot(sb_win_tasks *tasks, task *item, yyjson_mut_doc *doc);
static bool task_event(task *item, const char *event, const char *status, bool output) {
    sb_win_tasks *tasks=item->owner;
    if (!tasks || !tasks->store) return true;
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
            yyjson_mut_obj_add_str(doc,entry,"status",input->sent ? input->delivered ? "delivered" : "failed" : "queued"); yyjson_mut_arr_add_val(inputs,entry);
        }
        yyjson_mut_obj_add_val(doc,body,"input_receipts",inputs);
        if (output) for (unsigned i=0;i<2;++i) {
            size_t retained=item->lengths[i]<OUTPUT_CAP ? item->lengths[i] : OUTPUT_CAP;
            char *encoded=sb_base64(item->bytes[i],retained);
            if (encoded) yyjson_mut_obj_add_strcpy(doc,body,i ? "stderr_base64" : "stdout_base64",encoded);
            yyjson_mut_obj_add_uint(doc,body,i ? "stderr_retained_bytes" : "stdout_retained_bytes",retained);
            yyjson_mut_obj_add_bool(doc,body,i ? "stderr_history_truncated" : "stdout_history_truncated",retained!=item->lengths[i]);
            free(encoded);
        }
    }
    bool ok=body && sb_store_append(tasks->store,"task",item->id,item->actor,status,event,body) &&
        sb_audit_task(tasks->audit,event,item->actor,item->key,item->id,tasks->profile,item->hash,"lifecycle",item->pid);
    if (!ok) item->durable_warning=true;
    yyjson_doc_free(command); yyjson_mut_doc_free(doc); return ok;
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
    return serialize(doc, root);
}
static char *limit_fail(const char *field,uint64_t actual,uint64_t maximum,const char *unit) {
    yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL);if(!doc)return NULL;
    yyjson_mut_val *root=sb_limit_error(doc,field,actual,maximum,unit);yyjson_mut_obj_add_bool(doc,root,"_error",true);return serialize(doc,root);
}
/* 持有 tasks->lock；任务状态只能在同一外层锁下转为 finished。 */
static void budget(sb_win_tasks *tasks,yyjson_mut_doc *doc,yyjson_mut_val *root) {
    unsigned active=0,records=0;
    for(size_t i=0;i<TASK_RECORDS;++i)if(tasks->items[i]){++records;if(!tasks->items[i]->finished)++active;}
    yyjson_mut_val *b=yyjson_mut_obj(doc);
    yyjson_mut_obj_add_uint(doc,b,"active_count",active);yyjson_mut_obj_add_uint(doc,b,"active_limit",tasks->limits.active_tasks);
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
    else yyjson_mut_obj_add_str(doc,result,"decode_error","The page cannot be strictly decoded; raw data_base64 is intact. Retry from text_next_offset with more bytes, or select the correct encoding.");
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
            else { copy=0;if(!item->reason){item->reason="failed";task_event(item,"task_output_allocation_failed","running",false);TerminateJobObject(item->job,126);} }
        }
        memcpy(item->bytes[stream] + item->lengths[stream], chunk, copy); item->lengths[stream] += copy;
        if (copy < got && !item->reason) { item->reason = "output_limit"; task_event(item,"task_output_limit","running",false); TerminateJobObject(item->job, 124); }
        ReleaseSRWLockExclusive(&item->lock);
    }
    AcquireSRWLockExclusive(&item->lock); item->output_done[stream] = true; ReleaseSRWLockExclusive(&item->lock); return 0;
}
static DWORD WINAPI write_input(void *parameter) {
    task *item = parameter; size_t next = 0;
    for (;;) {
        WaitForSingleObject(item->input_event, 50);
        AcquireSRWLockExclusive(&item->lock);
        bool stop = item->finished || item->reason || item->input_closed;
        task_input *input = next < item->input_count ? &item->inputs[next] : NULL;
        ReleaseSRWLockExclusive(&item->lock);
        if (stop) break;
        if (!input) continue;
        AcquireSRWLockExclusive(&item->lock);
        bool recorded=task_event(item,"task_input_started","running",false);
        ReleaseSRWLockExclusive(&item->lock);
        DWORD written = 0;
        bool delivered = recorded && (!input->length || (WriteFile(item->input, input->data, input->length, &written, NULL) && written == input->length));
        AcquireSRWLockExclusive(&item->lock);
        input->sent = true; input->delivered = delivered;
        free(input->data); input->data = NULL; ++next;
        if (!delivered) item->input_failed = true;
        task_event(item,delivered ? "task_input_delivered" : "task_input_failed","running",false);
        bool eof = input->eof || !delivered;
        ReleaseSRWLockExclusive(&item->lock);
        if (eof) break;
    }
    AcquireSRWLockExclusive(&item->lock);
    if (item->input) { CloseHandle(item->input); item->input = NULL; }
    item->input_closed = true;
    ReleaseSRWLockExclusive(&item->lock); return 0;
}
static void task_free(task *item);
static bool task_threads_done(task *item) {
    HANDLE threads[]={item->readers[0],item->readers[1],item->writer};
    for(unsigned i=0;i<3;++i)if(threads[i] && WaitForSingleObject(threads[i],0)!=WAIT_OBJECT_0)return false;
    return true;
}
static DWORD WINAPI monitor_tasks(void *parameter) {
    sb_win_tasks *tasks = parameter;
    while (WaitForSingleObject(tasks->stop, 25) == WAIT_TIMEOUT) {
        AcquireSRWLockExclusive(&tasks->lock);
        for (size_t i = 0; i < TASK_RECORDS; ++i) {
            task *item = tasks->items[i]; if (!item) continue;
            AcquireSRWLockExclusive(&item->lock);
            if (!item->finished) {
                if (tasks->worker && WaitForSingleObject(tasks->worker,0) != WAIT_TIMEOUT && !item->reason) {
                    item->reason="cancelled"; task_event(item,"task_worker_exited","running",false); TerminateJobObject(item->job,125); SetEvent(item->input_event);
                }
                JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting = {0};
                bool queried = QueryInformationJobObject(item->job, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), NULL) != FALSE;
                if (!queried && !item->reason) { item->reason = "failed"; task_event(item,"task_query_failed","running",false); TerminateJobObject(item->job, 125); }
                if (queried && accounting.ActiveProcesses && !item->reason && sb_platform_millis() - item->started >= item->timeout) {
                    item->reason = "timed_out"; task_event(item,"task_timed_out","running",false); TerminateJobObject(item->job, 124);
                }
                if (queried && !accounting.ActiveProcesses && item->output_done[0] && item->output_done[1]) {
                    if (!GetExitCodeProcess(item->process, &item->exit_code)) { item->exit_code = 125; if (!item->reason) item->reason = "failed"; }
                    item->finished = true; SetEvent(item->input_event);
                    task_event(item,"task_completed",item->reason ? item->reason : "completed",true);
                }
            }
            ReleaseSRWLockExclusive(&item->lock);
        }
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
    if (item->job) TerminateJobObject(item->job, 125);
    AcquireSRWLockExclusive(&item->lock); bool unfinished=!item->finished;
    item->finished = true; if (unfinished && !item->reason) item->reason="cancelled";
    if (item->input_event) SetEvent(item->input_event); ReleaseSRWLockExclusive(&item->lock);
    HANDLE threads[] = {item->readers[0],item->readers[1],item->writer};
    for (size_t i = 0; i < 3; ++i) if (threads[i]) { CancelSynchronousIo(threads[i]); WaitForSingleObject(threads[i], INFINITE); CloseHandle(threads[i]); }
    if (unfinished && item->durable_reserved) {
        if (item->process && WaitForSingleObject(item->process,3000)==WAIT_OBJECT_0) GetExitCodeProcess(item->process,&item->exit_code);
        else item->reason="interrupted";
        task_event(item,"task_instance_closed",item->reason,true);
    }
    HANDLE handles[] = {item->job,item->process,item->input,item->output[0],item->output[1],item->input_event};
    for (size_t i = 0; i < 6; ++i) if (handles[i]) CloseHandle(handles[i]);
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
    SetEvent(tasks->stop); WaitForSingleObject(tasks->monitor, INFINITE); CloseHandle(tasks->monitor); CloseHandle(tasks->stop);
    for (size_t i = 0; i < TASK_RECORDS; ++i) task_free(tasks->items[i]);
    while(tasks->retired){task *item=tasks->retired;tasks->retired=item->retired_next;task_free(item);}
    for (size_t i=0;i<tasks->archive_count;++i)free(tasks->archives[i].receipt);
    if (tasks->worker) CloseHandle(tasks->worker); free(tasks);
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
            if (TerminateJobObject(item->job, 125)) { item->reason = "cancelled"; task_event(item,"task_authorization_cancelled","running",false); SetEvent(item->input_event); }
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
    yyjson_mut_obj_add_str(doc, result, "status", item->finished ? (item->reason ? item->reason : "completed") : "running");
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
    if (tasks->store) { yyjson_mut_obj_add_bool(doc,result,"durable",true); yyjson_mut_obj_add_bool(doc,result,"durable_warning",item->durable_warning); }
    return result;
}
static char *start(sb_win_tasks *tasks, yyjson_val *args) {
    if (tasks->store && (!sb_store_healthy(tasks->store) || !sb_audit_healthy(tasks->audit))) return fail("Persistent state or audit unavailable; task was not started.");
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
    const char *runtime=sb_string(yyjson_obj_get(args,"runtime_directory"));sb_error runtime_error={0};char *fallback=runtime?NULL:sb_platform_default_data_dir(&runtime_error);
    const char *exclude[]={token_env};wchar_t *env=runtime || fallback ? sb_platform_child_environment(runtime?runtime:fallback,exclude,1,&runtime_error):NULL;free(fallback);
    wchar_t *exe = to_wide(executable), *directory = to_wide(cwd), *line = calloc(SB_WINDOWS_COMMAND_CAP,sizeof(wchar_t)); size_t used = 0;
    bool valid = absolute_exe(exe) && directory && line && env && (!tasks->sandbox || sb_win_sandbox_program(tasks->sandbox,exe)) && quote(line,&used,exe);
    size_t idx,max; yyjson_val *argument;uint64_t command_length=exe?quoted_size(exe):0;
    yyjson_arr_foreach(argv,idx,max,argument) {
        const char *text=sb_string(argument);int units=text?MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text,-1,NULL,0):0;
        if(units>(int)SB_WINDOWS_COMMAND_CAP){char field[40];snprintf(field,sizeof(field),"argv[%zu]",idx);free(exe);free(directory);free(line);free(env);return limit_fail(field,(uint64_t)units-1,SB_WINDOWS_COMMAND_CAP-1,"UTF-16 units");}
        wchar_t *wide=to_wide(text);if(wide)command_length+=1+quoted_size(wide);valid=valid && wide && quote(line,&used,wide);free(wide);
    }
    if(command_length>=SB_WINDOWS_COMMAND_CAP){free(exe);free(directory);free(line);free(env);return limit_fail("command_line",command_length,SB_WINDOWS_COMMAND_CAP-1,"UTF-16 units (quoted executable and argv)");}
    if (!valid) { free(exe); free(directory); free(line); free(env); return fail("Executable must be an absolute local .exe path; the quoted command line must fit 32766 UTF-16 units. Use fs_write/artifact_upload plus a script path, or task_input for large source text."); }
    task *item = calloc(1,sizeof(*item));
    if (!item) { free(exe); free(directory); free(line); free(env); return fail("Cannot allocate task."); }
    InitializeSRWLock(&item->lock); strcpy(item->key,key); strcpy(item->hash,hash); strcpy(item->actor,actor); item->timeout=timeout; item->owner=tasks;
    item->command=tasks->store ? yyjson_val_write(args,0,NULL) : NULL;
    unsigned char random[16]; sb_error error={0}; bool ok = sb_platform_random(random,sizeof(random),&error);
    for(size_t i=0;ok && i<16;++i) snprintf(item->id+i*2,3,"%02x",random[i]);
    item->output_cap=tasks->limits.output_bytes;
    /* 只为实际输出增长缓冲，较高配额不再等于每个空闲任务都预分配最大容量。 */
    item->capacities[0]=item->capacities[1]=item->output_cap<SB_TEXT_LIMIT ? item->output_cap : SB_TEXT_LIMIT;
    item->bytes[0]=malloc(item->capacities[0]); item->bytes[1]=malloc(item->capacities[1]);
    SECURITY_ATTRIBUTES security={sizeof(security),NULL,TRUE}; HANDLE child_input=NULL,child_output[2]={NULL,NULL};
    ok = ok && item->bytes[0] && item->bytes[1] && CreatePipe(&child_input,&item->input,&security,4096) && SetHandleInformation(item->input,HANDLE_FLAG_INHERIT,0);
    for(unsigned i=0;ok && i<2;++i) ok=CreatePipe(&item->output[i],&child_output[i],&security,4096) && SetHandleInformation(item->output[i],HANDLE_FLAG_INHERIT,0);
    item->job=CreateJobObjectW(NULL,NULL); item->input_event=CreateEventW(NULL,FALSE,FALSE,NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits={0}; limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE|JOB_OBJECT_LIMIT_ACTIVE_PROCESS; limits.BasicLimitInformation.ActiveProcessLimit=tasks->sandbox?1:64;
    ok=ok && item->job && item->input_event && SetInformationJobObject(item->job,JobObjectExtendedLimitInformation,&limits,sizeof(limits));
    DWORD attribute_count=tasks->sandbox?4:1;
    SIZE_T attribute_size=0; InitializeProcThreadAttributeList(NULL,attribute_count,0,&attribute_size);
    LPPROC_THREAD_ATTRIBUTE_LIST attributes=attribute_size?malloc(attribute_size):NULL;
    bool initialized=attributes && InitializeProcThreadAttributeList(attributes,attribute_count,0,&attribute_size);
    HANDLE inherited[]={child_input,child_output[0],child_output[1]};
    ok=ok && initialized && UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof(inherited),NULL,NULL);
    if(ok && tasks->sandbox)ok=sb_win_sandbox_attributes(tasks->sandbox,attributes);
    STARTUPINFOEXW startup={0}; startup.StartupInfo.cb=sizeof(startup); startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES; startup.StartupInfo.hStdInput=child_input; startup.StartupInfo.hStdOutput=child_output[0]; startup.StartupInfo.hStdError=child_output[1]; startup.lpAttributeList=attributes;
    PROCESS_INFORMATION process={0};
    unsigned phase=1;
    DWORD flags=CREATE_SUSPENDED|(tasks->sandbox?DETACHED_PROCESS:CREATE_NO_WINDOW)|CREATE_UNICODE_ENVIRONMENT|EXTENDED_STARTUPINFO_PRESENT;
    ok=ok && (tasks->sandbox?sb_win_sandbox_launch(tasks->sandbox,exe,line,true,flags,env,directory,&startup.StartupInfo,&process):CreateProcessW(exe,line,NULL,NULL,TRUE,flags,env,directory,&startup.StartupInfo,&process));
    if(process.hProcess) { item->process=process.hProcess; item->pid=process.dwProcessId; }
    if(ok){phase=2;ok=AssignProcessToJobObject(item->job,item->process)!=FALSE;}
    if(ok && tasks->sandbox){phase=3;ok=sb_win_sandbox_token(tasks->sandbox,item->process);}
    DWORD native_error=GetLastError();
    if(initialized) DeleteProcThreadAttributeList(attributes); free(attributes);
    if(child_input) CloseHandle(child_input); for(unsigned i=0;i<2;++i) if(child_output[i]) CloseHandle(child_output[i]);
    free(exe);free(directory);free(line);free(env);
    for(unsigned i=0;ok && i<2;++i) {
        reader_context *context=malloc(sizeof(*context)); if(context) { context->item=item;context->stream=i;item->readers[i]=CreateThread(NULL,0,read_output,context,0,NULL); if(!item->readers[i]) free(context); } ok=item->readers[i]!=NULL;
    }
    if(ok) { item->writer=CreateThread(NULL,0,write_input,item,0,NULL); ok=item->writer!=NULL; }
    item->started=sb_platform_millis();
    if (ok && tasks->store) {
        ok=item->command && task_event(item,"task_start_intent","starting",false);
        if (ok) { item->durable_reserved=true; tasks->items[slot]=item; }
    }
    if(ok) ok=ResumeThread(process.hThread)!=(DWORD)-1;
    if(ok && tasks->store) { AcquireSRWLockExclusive(&item->lock); ok=task_event(item,"task_started","running",false); ReleaseSRWLockExclusive(&item->lock); }
    if(process.hThread) CloseHandle(process.hThread);
    if(!ok) {
        if(item->job) TerminateJobObject(item->job,125);
        if(item->process) TerminateProcess(item->process,125);
        if (item->durable_reserved) { AcquireSRWLockExclusive(&item->lock); item->reason="failed"; ReleaseSRWLockExclusive(&item->lock); SetEvent(item->input_event); return fail("Task startup was interrupted after its durable intent; request_key remains reserved. Review this task before submitting another key."); }
        task_free(item);char message[192];snprintf(message,sizeof(message),"Cannot create and contain task (phase %u, Windows error %lu). No uncontained process is resumed.",phase,native_error);return fail(message);
    }
    tasks->items[slot]=item;
    yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL); if(!doc) return NULL;
    AcquireSRWLockExclusive(&item->lock); yyjson_mut_val *result=snapshot(tasks,item,doc); yyjson_mut_obj_add_bool(doc,result,"created_new",true); ReleaseSRWLockExclusive(&item->lock); return serialize(doc,result);
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
    yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL); yyjson_mut_val *result=doc?snapshot(tasks,item,doc):NULL; const char *error=NULL;
    if(!doc) { ReleaseSRWLockExclusive(&item->lock); ReleaseSRWLockExclusive(&tasks->lock); return NULL; }
    if(!strcmp(name,"task_archive")) {
        if(!item->finished)error="task_still_running: only ended tasks can be archived; cancel and wait for termination first.";
        else if(tasks->archive_count==ARCHIVE_RECORDS)error="archive_receipt_budget_exceeded: compact request_key receipts are full; no record was discarded.";
        else {
            item->archived=true;result=snapshot(tasks,item,doc);char *receipt=yyjson_mut_val_write(result,0,NULL);
            if(!receipt || !task_event(item,"task_archived",item->reason?item->reason:"completed",false)){free(receipt);item->archived=false;error="Cannot persist archive receipt; the task and its output remain available.";}
            else {
                archived_task *a=&tasks->archives[tasks->archive_count++];strcpy(a->id,item->id);strcpy(a->key,item->key);strcpy(a->hash,item->hash);strcpy(a->actor,item->actor);a->receipt=receipt;
                for(size_t i=0;i<TASK_RECORDS;++i)if(tasks->items[i]==item)tasks->items[i]=NULL;
                SetEvent(item->input_event);item->retired_next=tasks->retired;tasks->retired=item;
                ReleaseSRWLockExclusive(&item->lock);ReleaseSRWLockExclusive(&tasks->lock);return serialize(doc,result);
            }
        }
    } else if(!strcmp(name,"task_cancel")) {
        if(!item->finished && !item->reason) { if(TerminateJobObject(item->job,125)) { item->reason="cancelled";task_event(item,"task_cancel_requested","running",false);SetEvent(item->input_event); } else error="Cannot terminate the task job."; }
        result=snapshot(tasks,item,doc);
    } else if(!strcmp(name,"task_output")) {
        const char *encoding=sb_string(yyjson_obj_get(args,"text_encoding"));UINT cp=codepage(encoding);
        if(encoding && !cp){yyjson_mut_doc_free(doc);ReleaseSRWLockExclusive(&item->lock);ReleaseSRWLockExclusive(&tasks->lock);return fail("Unsupported encoding: use utf-8, cp936/gbk or a supported Windows cpNNNN.");}
        const char *stream=sb_string(yyjson_obj_get(args,"stream")); unsigned selected=stream && !strcmp(stream,"stderr")?1:0; uint64_t offset=0,maximum=0;
        if(!stream || (strcmp(stream,"stdout") && strcmp(stream,"stderr")) || !sb_integer(yyjson_obj_get(args,"offset"),&offset) || !sb_integer(yyjson_obj_get(args,"max_bytes"),&maximum) || maximum<1 || maximum>SB_TASK_OUTPUT_PAGE_LIMIT || offset>item->lengths[selected]) error="Invalid output stream, byte offset or limit.";
        else {
            size_t length=item->lengths[selected]-(size_t)offset; if(length>maximum) length=(size_t)maximum;
            char *encoded=sb_base64(item->bytes[selected]+(size_t)offset,length);
            if(!encoded) error="Cannot encode task output.";
            else { yyjson_mut_obj_add_strcpy(doc,result,"data_base64",encoded);free(encoded); yyjson_mut_obj_add_str(doc,result,"encoding","base64");
                const unsigned char *page=item->bytes[selected]+(size_t)offset;
                if(encoding)decoded_output(doc,result,page,length,cp,offset,item->finished && offset+length==item->lengths[selected]);
                if (!length || (!memchr(page,0,length) && MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,(const char *)page,(int)length,NULL,0)))
                    yyjson_mut_obj_add_strncpy(doc,result,"text_utf8",(const char *)page,length); yyjson_mut_obj_add_uint(doc,result,"next_offset",offset+length); yyjson_mut_obj_add_bool(doc,result,"eof",item->finished && offset+length==item->lengths[selected]); }
        }
    } else if(!strcmp(name,"task_input")) {
        const char *key=sb_string(yyjson_obj_get(args,"input_key")),*text=sb_string(yyjson_obj_get(args,"text")); yyjson_val *eof_value=yyjson_obj_get(args,"eof");
        bool eof=yyjson_get_bool(eof_value); size_t length=text?strlen(text):0; char hash[65]; sb_error hash_error={0}; task_input *existing=NULL; size_t queued=0;
        if (tasks->store && (!sb_store_healthy(tasks->store) || !sb_audit_healthy(tasks->audit))) error="Persistent state or audit unavailable; stdin was not queued.";
        else if(!key_valid(key) || !text || length>SB_TASK_INPUT_LIMIT || !yyjson_is_bool(eof_value) || (!length && !eof) || !sb_platform_sha256(text,length,hash,&hash_error)) error="Invalid input key, UTF-8 text or EOF flag.";
        else {
            for(size_t i=0;i<item->input_count;++i) { if(item->inputs[i].data) queued+=item->inputs[i].length; if(!strcmp(item->inputs[i].key,key)) existing=&item->inputs[i]; }
            if(existing && (strcmp(existing->hash,hash) || existing->eof!=eof)) error="input_key already binds different input.";
            else if(!existing && (item->finished || item->reason || item->input_closed || (item->input_count && item->inputs[item->input_count-1].eof))) error="Task stdin is closed or closing.";
            else if(!existing && length>SB_TASK_INPUT_QUEUE_LIMIT-queued) error="Stdin queue is full; wait for delivery and retry the same input_key.";
            else if(!existing && item->input_count>=INPUT_RECORDS) error="Task input receipt budget reached.";
            else if(!existing) {
                char *copy=sb_strdup(text); if(!copy) error="Cannot queue stdin data.";
                else {
                    existing=&item->inputs[item->input_count++];strcpy(existing->key,key);strcpy(existing->hash,hash);existing->data=copy;existing->length=(DWORD)length;existing->eof=eof;
                    if (!task_event(item,"task_input_queued","running",false)) {
                        existing->sent=true; existing->delivered=false; item->reason="failed"; TerminateJobObject(item->job,125); error="Cannot persist stdin intent; input was not sent.";
                    }
                    SetEvent(item->input_event);
                }
            }
            if(!error && existing) { yyjson_mut_obj_add_strcpy(doc,result,"input_key",key); yyjson_mut_obj_add_str(doc,result,"input_status",existing->sent?(existing->delivered?"delivered":"failed"):"queued"); }
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
