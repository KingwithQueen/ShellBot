#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sddl.h>
#include "sb_projects.h"
#include "execution.h"
#include "manager.h"
#include "manager_ui.h"
#include "sandbox.h"
#include "desktop.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* Owner management stays local. The optional gateway authenticates each route
   before reserving its worker; workers never receive bearer credentials. */
#define WORKER_IO_MS 8000
#define WORKER_REQUEST_LIMIT (SB_REQUEST_LIMIT + 65536u)
typedef struct {
    CRITICAL_SECTION lock;
    const sb_project *project;
    char *snapshot;
    HANDLE pipe, process, job;
    DWORD pid, exit_code;
    char instance[33];
    char data_identity[50];
    const char *state;
    sb_root *data_root;
    char token[257];
    size_t token_length;
    bool administrator_allowed;
    bool paused;
    uint64_t authorization_version, administrator_generation;
    sb_root *administrator_data_root;
    char administrator_data_identity[50];
} worker;
/* This context never references manager/project memory. ShellExecute may wait
   on the secure desktop; terminal manager shutdown must not wait for UAC. */
typedef struct {
    HANDLE thread;
    volatile LONG completed, cancelled;
    sb_execution *execution;
    sb_error error;
    char *initialize_request;
} administrator_startup;
typedef struct {
    sb_project_registry registry;
    worker workers[SB_MAX_PROJECTS];
    size_t initialized;
    sb_lease *lease;
    sb_root *data_root;
    sb_audit *audit;
    sb_owner *owner;
    sb_config gateway_config;
    sb_http_server *gateway;
    sb_win_desktop_session *desktop_session;
    HANDLE stop;
    sb_mutex *administrator_lock;
    sb_execution *administrator;
    uint64_t administrator_generation, administrator_sequence;
    administrator_startup *administrator_starting;
    sb_error administrator_startup_error;
    uint64_t administrator_launch_attempts;
    char owner_token[257];
    size_t owner_token_length;
} manager;

static wchar_t *wide(const char *s) {
    int n = s ? MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0) : 0;
    wchar_t *result = n ? calloc((size_t)n, sizeof(wchar_t)) : NULL;
    if (result && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, result, n)) { free(result); result = NULL; }
    return result;
}
static bool ordinary(void) {
    HANDLE token = NULL; TOKEN_ELEVATION elevation = {0}; DWORD n;
    bool ok = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) && sb_win_owner_token_allowed(token) &&
        GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &n) && !elevation.TokenIsElevated;
    if (token) CloseHandle(token);
    return ok;
}
static bool same_executable(HANDLE process) {
    wchar_t mine[SB_PATH_CAP], theirs[SB_PATH_CAP]; DWORD size = SB_PATH_CAP;
    return GetModuleFileNameW(NULL, mine, SB_PATH_CAP) && QueryFullProcessImageNameW(process, 0, theirs, &size) && !_wcsicmp(mine, theirs);
}
static PSECURITY_DESCRIPTOR security(void) {
    HANDLE token = NULL; DWORD size = 0; void *user = NULL; LPWSTR sid = NULL;
    PSECURITY_DESCRIPTOR descriptor = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return NULL;
    GetTokenInformation(token, TokenUser, NULL, 0, &size);
    user = size ? malloc(size) : NULL;
    if (user && GetTokenInformation(token, TokenUser, user, size, &size) && ConvertSidToStringSidW(((TOKEN_USER *)user)->User.Sid, &sid)) {
        wchar_t sddl[256]; swprintf(sddl, 256, L"D:P(A;;GA;;;SY)(A;;GA;;;%ls)", sid);
        ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &descriptor, NULL);
    }
    if (sid) LocalFree(sid);
    free(user); CloseHandle(token); return descriptor;
}
static bool io(HANDLE pipe, void *buffer, DWORD size, bool writing, bool asynchronous, uint64_t deadline) {
    DWORD offset = 0;
    while (offset < size) {
        DWORD got = 0; OVERLAPPED ov = {0};
        if (asynchronous) {
            if (sb_platform_millis() >= deadline) return false;
            ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
            if (!ov.hEvent) return false;
        }
        BOOL ok = writing ? WriteFile(pipe, (char *)buffer + offset, size - offset, &got, asynchronous ? &ov : NULL) :
            ReadFile(pipe, (char *)buffer + offset, size - offset, &got, asynchronous ? &ov : NULL);
        if (!ok && asynchronous && GetLastError() == ERROR_IO_PENDING) {
            uint64_t now = sb_platform_millis();
            DWORD remaining = now < deadline ? (DWORD)(deadline - now) : 0;
            if (WaitForSingleObject(ov.hEvent, remaining) == WAIT_OBJECT_0) ok = GetOverlappedResult(pipe, &ov, &got, FALSE);
            else { CancelIoEx(pipe, &ov); GetOverlappedResult(pipe, &ov, &got, TRUE); ok = FALSE; }
        }
        if (ov.hEvent) CloseHandle(ov.hEvent);
        if (!ok || !got) return false;
        offset += got;
    }
    return true;
}
static bool send_frame(HANDLE pipe, const char *json, bool async, uint64_t deadline) {
    size_t length = json ? strlen(json) : 0;
    if (!length || length > SB_RESPONSE_LIMIT) return false;
    uint32_t size = (uint32_t)length;
    return io(pipe, &size, sizeof(size), true, async, deadline) && io(pipe, (void *)json, size, true, async, deadline);
}
static char *receive_frame(HANDLE pipe, bool async, uint64_t deadline, unsigned maximum) {
    uint32_t length = 0;
    if (!io(pipe, &length, sizeof(length), false, async, deadline) || !length || length > maximum) return NULL;
    char *result = calloc((size_t)length + 1, 1);
    if (result && !io(pipe, result, length, false, async, deadline)) { free(result); result = NULL; }
    return result;
}
static char *error_json(const char *message) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL); if (!doc) return NULL;
    yyjson_mut_val *root = yyjson_mut_obj(doc); yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_strcpy(doc, root, "error", message);
    char *json = yyjson_mut_write(doc, 0, NULL); yyjson_mut_doc_free(doc); return json;
}
static char *administrator_exchange_locked(manager *m, worker *w, const char *command, const char *actor, yyjson_val *operation, const char *fault) {
    yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL); sb_error error={0}; char hash[65];
    char *bytes=yyjson_val_write(operation,0,NULL), *reply=NULL;
    if (!doc || !bytes || !sb_platform_sha256(bytes,strlen(bytes),hash,&error)) goto done;
    yyjson_mut_val *root=yyjson_mut_obj(doc); yyjson_mut_doc_set_root(doc,root);
    yyjson_mut_obj_add_str(doc,root,"command",command); yyjson_mut_obj_add_str(doc,root,"project_id",w->project->id);
    yyjson_mut_obj_add_uint(doc,root,"policy_revision",w->project->policy_revision);
    yyjson_mut_obj_add_uint(doc,root,"authorization_version",w->authorization_version);
    yyjson_mut_obj_add_str(doc,root,"host_instance_id",w->instance); yyjson_mut_obj_add_str(doc,root,"actor",actor);
    yyjson_mut_obj_add_uint(doc,root,"sequence",++m->administrator_sequence); yyjson_mut_obj_add_uint(doc,root,"deadline_ms",sb_platform_millis()+WORKER_IO_MS);
    yyjson_mut_obj_add_str(doc,root,"operation_sha256",hash); yyjson_mut_obj_add_val(doc,root,"operation",yyjson_val_mut_copy(doc,operation));
    /* Owner-only diagnostic uses fixed invalid bindings, never arbitrary packets. */
    if (fault) {
        yyjson_mut_obj_remove_str(root,fault);
        if (!strcmp(fault,"project_id")) yyjson_mut_obj_add_str(doc,root,fault,"__unregistered_project__");
        else if (!strcmp(fault,"policy_revision")) yyjson_mut_obj_add_uint(doc,root,fault,w->project->policy_revision+1);
        else if (!strcmp(fault,"authorization_version")) yyjson_mut_obj_add_uint(doc,root,fault,w->authorization_version+1);
        else if (!strcmp(fault,"host_instance_id")) yyjson_mut_obj_add_str(doc,root,fault,"00000000000000000000000000000000");
        else if (!strcmp(fault,"operation_sha256")) yyjson_mut_obj_add_str(doc,root,fault,"0000000000000000000000000000000000000000000000000000000000000000");
        else if (!strcmp(fault,"actor")) yyjson_mut_obj_add_str(doc,root,fault,"untrusted-client");
        else yyjson_mut_obj_add_uint(doc,root,fault,0);
    }
    char *request=yyjson_mut_write(doc,0,NULL);
    reply=sb_win_shared_exchange(m->administrator,request,&error); free(request);
done:
    yyjson_mut_doc_free(doc); free(bytes);
    return reply ? reply : error_json(*error.text ? error.text : "Cannot encode administrator operation.");
}
static char *administrator_operation_locked(manager *m, worker *w, const char *command, const char *actor, yyjson_val *operation) {
    return administrator_exchange_locked(m,w,command,actor,operation,NULL);
}
static bool administrator_ok(const char *reply) {
    yyjson_doc *doc=reply ? yyjson_read(reply,strlen(reply),0) : NULL;
    bool ok=doc && yyjson_is_obj(yyjson_doc_get_root(doc)) && !yyjson_obj_get(yyjson_doc_get_root(doc),"error");
    yyjson_doc_free(doc); return ok;
}
static DWORD WINAPI administrator_start_thread(void *parameter) {
    administrator_startup *startup=parameter;
    sb_execution *execution=sb_win_shared_start(&startup->error);
    if (execution && !InterlockedCompareExchange(&startup->cancelled,0,0)) {
        char *reply=sb_win_shared_exchange(execution,startup->initialize_request,&startup->error);
        if (!administrator_ok(reply)) {
            sb_execution_free(execution); execution=NULL;
            if (!*startup->error.text) sb_fail(&startup->error,"Administrator initialization rejected.");
        }
        free(reply);
    }
    if (InterlockedCompareExchange(&startup->cancelled,0,0)) {
        sb_execution_free(execution); execution=NULL;
        sb_fail(&startup->error,"Administrator startup cancelled by owner.");
    }
    startup->execution=execution;
    InterlockedExchange(&startup->completed,1);
    return 0;
}
/* Call under administrator_lock. Publication happens only after full handshake
   and credential removal; status polling never launches or retries elevation. */
static void administrator_refresh_locked(manager *m) {
    administrator_startup *startup=m->administrator_starting;
    if (!startup || !InterlockedCompareExchange(&startup->completed,0,0)) return;
    if (!InterlockedCompareExchange(&startup->cancelled,0,0) && sb_win_shared_alive(startup->execution)) {
        m->administrator=startup->execution; startup->execution=NULL;
        ++m->administrator_generation;
    }
    m->administrator_startup_error=startup->error;
    sb_execution_free(startup->execution);
    /* completed is published just before the thread returns; join before free. */
    WaitForSingleObject(startup->thread,INFINITE); CloseHandle(startup->thread);
    free(startup->initialize_request); free(startup); m->administrator_starting=NULL;
}
static bool administrator_begin_locked(manager *m, sb_error *error) {
    administrator_startup *startup=calloc(1,sizeof(*startup));
    yyjson_mut_doc *init=yyjson_mut_doc_new(NULL);
    if (startup && init) {
        yyjson_mut_val *root=yyjson_mut_obj(init), *envs=yyjson_mut_arr(init); yyjson_mut_doc_set_root(init,root);
        yyjson_mut_obj_add_str(init,root,"command","initialize");
        for (size_t i=0;i<m->registry.project_count;++i) yyjson_mut_arr_add_str(init,envs,m->registry.projects[i].token_env);
        if (*m->registry.owner_token_env) yyjson_mut_arr_add_str(init,envs,m->registry.owner_token_env);
        yyjson_mut_obj_add_val(init,root,"token_envs",envs);
        startup->initialize_request=yyjson_mut_write(init,0,NULL);
        if (startup->initialize_request) startup->thread=CreateThread(NULL,0,administrator_start_thread,startup,0,NULL);
    }
    yyjson_mut_doc_free(init);
    if (!startup || !startup->thread) {
        if (startup) free(startup->initialize_request);
        free(startup); sb_fail(error,"Cannot allocate administrator startup thread."); return false;
    }
    m->administrator_starting=startup; memset(&m->administrator_startup_error,0,sizeof(m->administrator_startup_error));
    ++m->administrator_launch_attempts; return true;
}
static void administrator_detach(manager *m, worker *w) {
    sb_platform_mutex_lock(m->administrator_lock);
    administrator_refresh_locked(m);
    if (w->administrator_generation && w->administrator_generation == m->administrator_generation && sb_win_shared_alive(m->administrator)) {
        yyjson_doc *empty=yyjson_read("{}",2,0);
        char *reply=administrator_operation_locked(m,w,"detach","manager-owner",yyjson_doc_get_root(empty));
        if (!administrator_ok(reply)) { sb_execution_free(m->administrator); m->administrator=NULL; }
        free(reply); yyjson_doc_free(empty);
    }
    w->administrator_generation=0; sb_platform_root_close(w->administrator_data_root); w->administrator_data_root=NULL;
    sb_platform_mutex_unlock(m->administrator_lock);
}
static void reap(manager *m, worker *w, const char *state) {
    administrator_detach(m,w);
    if (!strcmp(state,"stopped") && w->pipe && w->process && WaitForSingleObject(w->process,0)==WAIT_TIMEOUT) {
        /* Give our healthy worker a bounded chance to flush terminal records. */
        if (send_frame(w->pipe,"{\"kind\":\"shutdown\",\"request\":{},\"administrator_backend\":false,\"owner_url\":\"\"}",true,sb_platform_millis()+1000))
            WaitForSingleObject(w->process,3000);
    }
    /* Every handle here came from our own CreateProcess/Job allocation. */
    if (w->job) TerminateJobObject(w->job, 125);
    if (w->process) {
        WaitForSingleObject(w->process, 3000);
        GetExitCodeProcess(w->process, &w->exit_code); CloseHandle(w->process);
    }
    if (w->pipe) CloseHandle(w->pipe);
    if (w->job) CloseHandle(w->job);
    w->process = w->pipe = w->job = NULL; w->state = state;
    if(w->project && w->project->policy.default_identity==SB_ID_RESTRICTED) {
        sb_error cleanup={0};if(!sb_win_sandbox_recover(w->project,&cleanup))w->state="cleanup_required";
    }
}
static void refresh(manager *m, worker *w) {
    if (w->process && WaitForSingleObject(w->process, 0) != WAIT_TIMEOUT) {
        reap(m, w, "failed");
        sb_audit_project(m->audit, "project_failed", w->project->id, w->project->policy_revision, w->pid);
    }
}
static wchar_t *environment(const sb_project_registry *registry) {
    static const wchar_t *names[] = {L"ALLUSERSPROFILE",L"APPDATA",L"COMPUTERNAME",L"ComSpec",L"HOMEDRIVE",L"HOMEPATH",L"LOCALAPPDATA",L"PATH",L"PATHEXT",L"ProgramData",L"ProgramFiles",L"ProgramFiles(x86)",L"SystemDrive",L"SystemRoot",L"TEMP",L"TMP",L"USERDOMAIN",L"USERNAME",L"USERPROFILE",L"windir"};
    wchar_t *block = calloc(32768, sizeof(wchar_t)); size_t used = 0;
    if (!block) return NULL;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        bool excluded = false;
        wchar_t *owner=wide(registry->owner_token_env);
        if (owner && !_wcsicmp(owner,names[i])) excluded=true;
        free(owner);
        for (size_t j = 0; j < registry->project_count; ++j) {
            wchar_t *key = wide(registry->projects[j].token_env);
            if (!key) { free(block); return NULL; }
            if (!_wcsicmp(key, names[i])) excluded = true;
            free(key);
        }
        if (excluded) continue;
        DWORD n = GetEnvironmentVariableW(names[i], NULL, 0); if (!n) continue;
        size_t key_length = wcslen(names[i]);
        if (n + key_length + 2 >= 32768 - used) { free(block); return NULL; }
        memcpy(block + used, names[i], key_length * sizeof(wchar_t)); used += key_length; block[used++] = L'=';
        DWORD got = GetEnvironmentVariableW(names[i], block + used, n);
        if (!got || got >= n) { free(block); return NULL; }
        used += got + 1;
    }
    return block;
}
static bool start_worker(manager *m, worker *w, sb_error *error) {
    refresh(m, w);
    if (w->process) return true;
    if (!w->project->enabled) { sb_fail(error, "Project is disabled."); return false; }
    if(w->project->policy.default_identity==SB_ID_RESTRICTED && !sb_win_sandbox_recover(w->project,error))return false;
    if (!sb_audit_project(m->audit, "project_start_intent", w->project->id, w->project->policy_revision, w->pid)) {
        sb_fail(error, "Manager audit unavailable; project startup blocked."); return false;
    }
    HANDLE child = INVALID_HANDLE_VALUE;
    PSECURITY_DESCRIPTOR descriptor = security();
    unsigned char random[16]; char hex[33]; wchar_t name[128], exe[SB_PATH_CAP], line[SB_PATH_CAP];
    bool ok = descriptor && sb_platform_random(random, sizeof(random), error) && GetModuleFileNameW(NULL, exe, SB_PATH_CAP);
    for (size_t i = 0; ok && i < sizeof(random); ++i) snprintf(hex + i * 2, 3, "%02x", random[i]);
    if (ok) {
        swprintf(name, 128, L"\\\\.\\pipe\\ShellBot-worker-%lu-%hs", GetCurrentProcessId(), hex);
        SECURITY_ATTRIBUTES sa = {sizeof(sa), descriptor, FALSE};
        w->pipe = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, &sa);
        if (w->pipe == INVALID_HANDLE_VALUE) w->pipe = NULL;
        child = w->pipe ? CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
            SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, NULL) : INVALID_HANDLE_VALUE;
        ok = w->pipe && child != INVALID_HANDLE_VALUE && SetHandleInformation(child, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    }
    if (descriptor) LocalFree(descriptor);
    SIZE_T attr_size = 0; STARTUPINFOEXW startup = {0}; PROCESS_INFORMATION process = {0};
    startup.StartupInfo.cb = sizeof(startup);
    InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
    startup.lpAttributeList = attr_size ? malloc(attr_size) : NULL;
    bool initialized = startup.lpAttributeList && InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attr_size);
    wchar_t *env = ok ? environment(&m->registry) : NULL;
    if (ok && initialized && env) {
        ok = UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &child, sizeof(child), NULL, NULL) != FALSE;
        swprintf(line, SB_PATH_CAP, L"\"%ls\" --project-worker %llu %lu", exe, (unsigned long long)(uintptr_t)child, GetCurrentProcessId());
        w->job = CreateJobObjectW(NULL, NULL);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_JOB_MEMORY;
        limits.BasicLimitInformation.ActiveProcessLimit = 1024;
        limits.JobMemoryLimit = (SIZE_T)w->project->limits.worker_memory_mb * 1024 * 1024;
        ok = ok && w->job && SetInformationJobObject(w->job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) &&
            CreateProcessW(exe, line, NULL, NULL, TRUE, CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
                env, NULL, &startup.StartupInfo, &process);
        if (ok) {
            w->process = process.hProcess; w->pid = process.dwProcessId;
            ok = AssignProcessToJobObject(w->job, w->process) && ResumeThread(process.hThread) != (DWORD)-1;
            if (!ok) TerminateProcess(w->process, 125); /* Own suspended child, not a searched PID. */
        }
    } else ok = false;
    if (process.hThread) CloseHandle(process.hThread);
    if (child != INVALID_HANDLE_VALUE) CloseHandle(child);
    if (initialized) DeleteProcThreadAttributeList(startup.lpAttributeList);
    free(startup.lpAttributeList); free(env);
    uint64_t deadline = sb_platform_millis() + WORKER_IO_MS;
    char *greeting = ok && send_frame(w->pipe, w->snapshot, true, deadline) && send_frame(w->pipe, w->data_identity, true, deadline) ?
        receive_frame(w->pipe, true, deadline, 4096) : NULL;
    yyjson_doc *reply = greeting ? yyjson_read(greeting, strlen(greeting), 0) : NULL;
    yyjson_val *root = yyjson_doc_get_root(reply);
    const char *id = sb_string(yyjson_obj_get(root, "project_id")), *instance = sb_string(yyjson_obj_get(root, "host_instance_id"));
    uint64_t revision = 0, pid = 0;
    ok = reply && yyjson_is_true(yyjson_obj_get(root, "ready")) && id && !strcmp(id, w->project->id) &&
        sb_integer(yyjson_obj_get(root, "policy_revision"), &revision) && revision == w->project->policy_revision &&
        sb_integer(yyjson_obj_get(root, "pid"), &pid) && pid == w->pid && instance && strlen(instance) == 32 &&
        strspn(instance, "0123456789abcdef") == 32;
    if (ok) strcpy(w->instance, instance);
    const char *startup_failure=sb_string(yyjson_obj_get(root,"error"));
    if (!ok && startup_failure) sb_fail(error,startup_failure);
    yyjson_doc_free(reply); free(greeting);
    if (!ok) { reap(m, w, "failed"); if (!*error->text) sb_fail(error, "Worker startup failed; no elevation or automatic restart attempted."); return false; }
    w->state = "running"; w->exit_code = STILL_ACTIVE;
    sb_audit_project(m->audit, "project_started", w->project->id, w->project->policy_revision, w->pid);
    return true;
}
static yyjson_mut_val *status(manager *m, worker *w, yyjson_mut_doc *doc) {
    refresh(m, w);
    yyjson_mut_val *value = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, value, "project_id", w->project->id);
    yyjson_mut_obj_add_uint(doc, value, "policy_revision", w->project->policy_revision);
    yyjson_mut_obj_add_str(doc, value, "state", w->state);
    yyjson_mut_obj_add_uint(doc, value, "worker_pid", w->pid);
    yyjson_mut_obj_add_uint(doc, value, "exit_code", w->exit_code);
    yyjson_mut_obj_add_strcpy(doc, value, "host_instance_id", w->instance);
    yyjson_mut_obj_add_bool(doc, value, "enabled", w->project->enabled);
    yyjson_mut_obj_add_bool(doc, value, "paused", w->paused);
    yyjson_mut_obj_add_bool(doc, value, "automatic_restart", false);
    sb_platform_mutex_lock(m->administrator_lock);
    administrator_refresh_locked(m);
    yyjson_mut_obj_add_bool(doc, value, "administrator_backend", w->administrator_allowed && sb_win_shared_alive(m->administrator));
    yyjson_mut_obj_add_bool(doc, value, "administrator_allowed", w->administrator_allowed);
    yyjson_mut_obj_add_uint(doc, value, "authorization_version", w->authorization_version);
    sb_platform_mutex_unlock(m->administrator_lock);
    return value;
}

static bool token_matches(const worker *w, const char *authorization) {
    if (!authorization || _strnicmp(authorization, "Bearer ", 7) || !w->token_length ||
        strlen(authorization + 7) != w->token_length) return false;
    volatile unsigned difference = 0;
    for (size_t i = 0; i < w->token_length; ++i)
        difference |= (unsigned)((unsigned char)authorization[i + 7] ^ (unsigned char)w->token[i]);
    return difference == 0;
}
static bool administrator_eligible(const worker *w) {
    return w->project->enabled && !w->project->legacy_compatibility && w->project->policy.file_scope == SB_FILES_CURRENT_USER &&
        w->project->policy.default_identity == SB_ID_CURRENT_USER;
}
static bool administrator_register_locked(manager *m, worker *w, sb_error *error) {
    if (!sb_win_shared_alive(m->administrator)) { sb_fail(error,"Shared administrator agent unavailable; explicit owner startup required."); return false; }
    if (w->administrator_generation == m->administrator_generation) return true;
    if (!w->process || !administrator_eligible(w)) { sb_fail(error,"Project is not eligible for the shared administrator agent."); return false; }
    size_t size=strlen(w->project->data_dir)+64; char *data=malloc(size);
    if (data) snprintf(data,size,"%s\\administrator-state",w->project->data_dir);
    sb_lease *lease=data ? sb_platform_lease(data,error) : NULL;
    if (!lease) { free(data); return false; }
    sb_platform_root_close(w->administrator_data_root);
    w->administrator_data_root=sb_platform_root(data,error); sb_platform_lease_close(lease); free(data);
    if (!w->administrator_data_root || !sb_platform_root_identity(w->administrator_data_root,w->administrator_data_identity,error)) return false;
    yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL); if (!doc) return false;
    yyjson_mut_val *root=yyjson_mut_obj(doc); yyjson_mut_doc_set_root(doc,root);
    yyjson_mut_obj_add_str(doc,root,"command","register"); yyjson_mut_obj_add_str(doc,root,"snapshot",w->snapshot);
    yyjson_mut_obj_add_uint(doc,root,"worker_pid",w->pid); yyjson_mut_obj_add_str(doc,root,"host_instance_id",w->instance);
    yyjson_mut_obj_add_uint(doc,root,"authorization_version",w->authorization_version); yyjson_mut_obj_add_bool(doc,root,"allowed",w->administrator_allowed);
    yyjson_mut_obj_add_str(doc,root,"data_identity",w->administrator_data_identity);
    char *request=yyjson_mut_write(doc,0,NULL), *reply=sb_win_shared_exchange(m->administrator,request,error);
    bool ok=administrator_ok(reply); if (ok) w->administrator_generation=m->administrator_generation;
    else if (!*error->text) sb_fail(error,"Shared agent rejected project registration; administrator operation blocked.");
    free(request); free(reply); yyjson_mut_doc_free(doc); return ok;
}
static char *administrator_call(manager *m, worker *w, const char *command, const char *actor, yyjson_val *operation) {
    sb_platform_mutex_lock(m->administrator_lock); sb_error error={0}; char *reply=NULL;
    administrator_refresh_locked(m);
    const char *name=sb_string(yyjson_obj_get(operation,"name"));
    bool result_tool=name && (!strcmp(name,"task_status") || !strcmp(name,"task_output") || !strcmp(name,"task_cancel") || !strcmp(name,"approval_status") || !strcmp(name,"approval_cancel"));
    if (!strcmp(command,"call") && !w->administrator_allowed && !result_tool) reply=error_json("administrator_not_allowed");
    else if (administrator_register_locked(m,w,&error)) reply=administrator_operation_locked(m,w,command,actor,operation);
    else reply=error_json(*error.text ? error.text : "Administrator registration unavailable.");
    sb_platform_mutex_unlock(m->administrator_lock); return reply;
}
static char *worker_exchange(manager *m, worker *w, const char *wire, yyjson_val *original, const char *actor, uint64_t deadline, unsigned maximum) {
    yyjson_doc *parsed=yyjson_read(wire,strlen(wire),0);
    yyjson_mut_doc *trusted=parsed ? yyjson_doc_mut_copy(parsed,NULL) : NULL;
    if (trusted) {
        sb_platform_mutex_lock(m->administrator_lock);
        administrator_refresh_locked(m);
        yyjson_mut_obj_add_bool(trusted,yyjson_mut_doc_get_root(trusted),"administrator_backend",w->administrator_allowed && sb_win_shared_alive(m->administrator));
        char url[384]={0};
        if (m->gateway && m->owner_token_length) {
            if (*m->registry.public_origin) snprintf(url,sizeof(url),"%s/owner",m->registry.public_origin);
            else snprintf(url,sizeof(url),"http://127.0.0.1:%u/owner",m->registry.port);
        }
        yyjson_mut_obj_add_strcpy(trusted,yyjson_mut_doc_get_root(trusted),"owner_url",url);
        sb_platform_mutex_unlock(m->administrator_lock);
    }
    char *envelope=trusted ? yyjson_mut_write(trusted,0,NULL) : NULL;
    bool sent=envelope && send_frame(w->pipe,envelope,true,deadline);
    free(envelope); yyjson_mut_doc_free(trusted); yyjson_doc_free(parsed);
    if (!sent) return NULL;
    char *frame=receive_frame(w->pipe,true,deadline,maximum);
    yyjson_doc *reply=frame ? yyjson_read(frame,strlen(frame),0) : NULL;
    yyjson_val *request=yyjson_obj_get(yyjson_doc_get_root(reply),"administrator_request");
    if (request) {
        const char *method=sb_string(yyjson_obj_get(original,"method")); yyjson_val *params=yyjson_obj_get(original,"params");
        static const char *const keys[]={"administrator_request"}, *const request_keys[]={"name","arguments"};
        sb_error canonical_error={0}; char *canonical=method && !strcmp(method,"tools/call") ? sb_mcp_operation(params,&canonical_error) : NULL;
        yyjson_doc *expected=canonical ? yyjson_read(canonical,strlen(canonical),0) : NULL;
        bool valid=expected && sb_config_keys(yyjson_doc_get_root(reply),keys,1) && sb_config_keys(request,request_keys,2) &&
            yyjson_equals(request,yyjson_doc_get_root(expected));
        yyjson_doc_free(expected); free(canonical);
        /* A compromised worker cannot change the operation or choose an actor. */
        char *response=valid ? administrator_call(m,w,"call",actor,request) : error_json("Worker administrator operation did not match the authenticated request.");
        free(frame); frame=NULL;
        if (response && send_frame(w->pipe,response,true,deadline)) frame=receive_frame(w->pipe,true,deadline,maximum);
        free(response);
    }
    yyjson_doc_free(reply); return frame;
}
static bool gateway_credentials(manager *m, sb_error *error) {
    for (size_t i = 0; i < m->registry.project_count; ++i) {
        worker *w = &m->workers[i];
        if (!w->project->enabled) continue;
        const char *token = getenv(w->project->token_env);
        size_t length = token ? strlen(token) : 0;
        if (length < 32 || length > 256) goto invalid;
        for (size_t j = 0; j < length; ++j) if ((unsigned char)token[j] <= 32 || (unsigned char)token[j] >= 127) goto invalid;
        memcpy(w->token, token, length + 1); w->token_length = length;
        for (size_t j = 0; j < i; ++j) if (w->token_length == m->workers[j].token_length &&
            !memcmp(w->token, m->workers[j].token, length)) {
            sb_fail(error, "Enabled projects must have distinct bearer token values; gateway startup blocked."); return false;
        }
    }
    if (*m->registry.owner_token_env) {
        const char *token=getenv(m->registry.owner_token_env); size_t length=token ? strlen(token) : 0;
        if (length<32 || length>256) goto invalid_owner;
        for (size_t j=0;j<length;++j) if ((unsigned char)token[j]<=32 || (unsigned char)token[j]>=127) goto invalid_owner;
        for (size_t j=0;j<m->registry.project_count;++j) {
            const char *project_token=getenv(m->registry.projects[j].token_env);
            if (project_token && !strcmp(token,project_token)) goto invalid_owner;
        }
        memcpy(m->owner_token,token,length+1); m->owner_token_length=length;
    }
    return true;
invalid_owner:
    sb_fail(error,"owner_token_env requires an independent 32..256 byte visible-ASCII owner bearer; gateway startup blocked."); return false;
invalid:
    sb_fail(error, "Every enabled project requires a 32..256 byte visible-ASCII bearer in its token_env; gateway startup blocked."); return false;
}
static void *gateway_open(void *context, const char *path, const char *authorization, bool *health, int *http_status) {
    manager *m = context; worker *w = NULL;
    const char *suffix = !strcmp(path, "/mcp") ? "mcp" : !strcmp(path, "/healthz") ? "healthz" : NULL;
    if (suffix) {
        for (size_t i = 0; i < m->registry.project_count; ++i)
            if (!strcmp(m->workers[i].project->id, m->registry.default_project_id)) w = &m->workers[i];
    } else if (!strncmp(path, "/projects/", 10)) {
        const char *id = path + 10, *end = strchr(id, '/');
        if (end && (!strcmp(end + 1, "mcp") || !strcmp(end + 1, "healthz"))) {
            suffix = end + 1;
            for (size_t i = 0; i < m->registry.project_count; ++i)
                if (strlen(m->workers[i].project->id) == (size_t)(end - id) &&
                    !memcmp(m->workers[i].project->id, id, (size_t)(end - id))) w = &m->workers[i];
        }
    }
    if (!w) { *http_status = 404; return NULL; }
    if (!w->project->enabled) { *http_status = 503; return NULL; }
    if (!token_matches(w, authorization)) { *http_status = 401; return NULL; }
    if (WaitForSingleObject(m->stop, 0) == WAIT_OBJECT_0) { *http_status = 503; return NULL; }
    if (!TryEnterCriticalSection(&w->lock)) { *http_status = 429; return NULL; }
    refresh(m, w);
    if (!w->process) { LeaveCriticalSection(&w->lock); *http_status = 503; return NULL; }
    *health = !strcmp(suffix, "healthz"); *http_status = 200; return w;
}
static void gateway_close(void *context, void *binding) { (void)context; LeaveCriticalSection(&((worker *)binding)->lock); }
static bool owner_enabled(void *context) { return ((manager *)context)->owner_token_length!=0; }
static bool owner_authorized(void *context, const char *authorization) {
    manager *m=context;
    if (!m->owner_token_length || !authorization || _strnicmp(authorization,"Bearer ",7) || strlen(authorization+7)!=m->owner_token_length) return false;
    volatile unsigned difference=0;
    for (size_t i=0;i<m->owner_token_length;++i) difference|=(unsigned)((unsigned char)authorization[i+7]^(unsigned char)m->owner_token[i]);
    return difference==0;
}
/* A limited owner API. It cannot dispatch MCP, start a backend, modify policy,
   or accept caller-selected actors. Decisions carry the inspected binding. */
static char *owner_call(void *context, const char *request, size_t length, int *http_status) {
    manager *m=context; *http_status=400;
    yyjson_doc *input=length<=65536 ? yyjson_read(request,length,0) : NULL;
    yyjson_val *root=yyjson_doc_get_root(input);
    static const char *const keys[]={"command","project_id","backend","id","expected_sha256","expected_host_instance_id","expected_policy_revision","offset","limit"};
    const char *command=sb_string(yyjson_obj_get(root,"command")), *project=sb_string(yyjson_obj_get(root,"project_id")), *backend=sb_string(yyjson_obj_get(root,"backend"));
    char *reply=NULL;
    if (!sb_config_keys(root,keys,9) || !command) goto invalid;
    if (WaitForSingleObject(m->stop,0)==WAIT_OBJECT_0) { *http_status=503; reply=error_json("Manager is stopping."); goto done; }
    if (!strcmp(command,"projects")) {
        if (yyjson_obj_size(root)!=1) goto invalid;
        yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL); yyjson_mut_val *body=yyjson_mut_obj(doc), *items=yyjson_mut_arr(doc);
        yyjson_mut_doc_set_root(doc,body);
        for (size_t i=0;i<m->registry.project_count;++i) {
            worker *w=&m->workers[i]; if (!w->project->enabled || w->project->legacy_compatibility) continue;
            yyjson_mut_val *item=yyjson_mut_obj(doc); yyjson_mut_obj_add_str(doc,item,"project_id",w->project->id);
            if (TryEnterCriticalSection(&w->lock)) {
                refresh(m,w); yyjson_mut_obj_add_str(doc,item,"state",w->state);
                yyjson_mut_obj_add_bool(doc,item,"administrator_allowed",w->administrator_allowed); LeaveCriticalSection(&w->lock);
            } else yyjson_mut_obj_add_str(doc,item,"state","busy");
            yyjson_mut_arr_add_val(items,item);
        }
        yyjson_mut_obj_add_val(doc,body,"projects",items); reply=yyjson_mut_write(doc,0,NULL); yyjson_mut_doc_free(doc); *http_status=200; goto done;
    }
    bool decision=!strcmp(command,"approve") || !strcmp(command,"deny") || !strcmp(command,"revoke");
    bool inspect=!strcmp(command,"inspect");
    if (!decision && !inspect && strcmp(command,"list") && strcmp(command,"history")) goto invalid;
    if (!sb_identifier(project) || !backend || (strcmp(backend,"ordinary") && strcmp(backend,"administrator"))) goto invalid;
    if ((decision || inspect) && !sb_string(yyjson_obj_get(root,"id"))) goto invalid;
    if (decision && (!sb_string(yyjson_obj_get(root,"expected_sha256")) || !sb_string(yyjson_obj_get(root,"expected_host_instance_id")) ||
        !yyjson_is_uint(yyjson_obj_get(root,"expected_policy_revision")))) goto invalid;
    if (!decision && (yyjson_obj_get(root,"expected_sha256") || yyjson_obj_get(root,"expected_host_instance_id") || yyjson_obj_get(root,"expected_policy_revision"))) goto invalid;
    if (strcmp(command,"history") && (yyjson_obj_get(root,"offset") || yyjson_obj_get(root,"limit"))) goto invalid;
    if (!decision && !inspect && strcmp(command,"history") && yyjson_obj_get(root,"id")) goto invalid;
    worker *w=NULL;
    for (size_t i=0;i<m->registry.project_count;++i) if (!strcmp(project,m->workers[i].project->id)) w=&m->workers[i];
    if (!w || !w->project->enabled || w->project->legacy_compatibility) { *http_status=404; reply=error_json("Approval project unavailable."); goto done; }
    if (!TryEnterCriticalSection(&w->lock)) { *http_status=429; reply=error_json("Project is busy. No decision was queued."); goto done; }
    refresh(m,w);
    if (!w->process) { *http_status=503; reply=error_json("Project is stopped. Explicit local owner startup is required."); }
    else {
        yyjson_mut_doc *doc=yyjson_doc_mut_copy(input,NULL); yyjson_mut_val *operation=yyjson_mut_doc_get_root(doc);
        yyjson_mut_obj_remove_str(operation,"project_id"); yyjson_mut_obj_remove_str(operation,"backend");
        char *wire=doc ? yyjson_mut_write(doc,0,NULL) : NULL;
        yyjson_doc *parsed=wire ? yyjson_read(wire,strlen(wire),0) : NULL;
        if (!strcmp(backend,"administrator")) reply=parsed ? administrator_call(m,w,"remote-owner","manager-owner",yyjson_doc_get_root(parsed)) : NULL;
        else if (doc) {
            yyjson_mut_val *envelope=yyjson_mut_obj(doc); yyjson_mut_obj_add_str(doc,envelope,"kind","remote-owner"); yyjson_mut_obj_add_val(doc,envelope,"request",operation);
            char *frame=yyjson_mut_val_write(envelope,0,NULL);
            reply=frame ? worker_exchange(m,w,frame,NULL,"manager-owner",sb_platform_millis()+WORKER_IO_MS,SB_RESPONSE_LIMIT-1024) : NULL; free(frame);
            if (!reply) { reap(m,w,"failed"); sb_audit_project(m->audit,"project_failed",w->project->id,w->project->policy_revision,w->pid); }
        }
        free(wire); yyjson_doc_free(parsed); yyjson_mut_doc_free(doc); *http_status=reply ? 200 : 503;
    }
    LeaveCriticalSection(&w->lock);
    if (!reply) reply=error_json("Owner channel unavailable. Review state before retrying; no automatic replay.");
    goto done;
invalid:
    reply=error_json("Invalid limited owner request or missing inspected decision binding.");
done:
    yyjson_doc_free(input); return reply;
}
static char *gateway_call(void *context, void *binding, const char *request, size_t length,
                          const sb_request_context *transport, size_t *response_length, int *http_status) {
    manager *m = context; worker *w = binding;
    *response_length = 0; *http_status = 500;
    yyjson_doc *input = yyjson_read(request, length, 0);
    if (!input) {
        *http_status = 400;
        char *failure = sb_strdup("{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32700,\"message\":\"Parse error.\"}}");
        *response_length = failure ? strlen(failure) : 0; return failure;
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL); char *wire = NULL, *response = NULL;
    const char *headers[] = {transport->version, transport->method, transport->name};
    const char *names[] = {"version", "method", "name"};
    if (!doc) goto done;
    yyjson_mut_val *envelope = yyjson_mut_obj(doc); yyjson_mut_doc_set_root(doc, envelope);
    yyjson_mut_obj_add_str(doc, envelope, "kind", "http");
    yyjson_mut_obj_add_val(doc, envelope, "request", yyjson_val_mut_copy(doc, yyjson_doc_get_root(input)));
    for (size_t i = 0; i < 3; ++i) if (headers[i]) {
        if (strlen(headers[i]) > 8192) { *http_status = 400; response = error_json("Protocol header exceeds its limit."); goto done; }
        yyjson_mut_obj_add_str(doc, envelope, names[i], headers[i]);
    }
    wire = yyjson_mut_write(doc, 0, NULL);
    if (!wire || strlen(wire) > WORKER_REQUEST_LIMIT) { *http_status = 413; response = error_json("Worker request frame exceeds its limit."); goto done; }
    uint64_t deadline = sb_platform_millis() + WORKER_IO_MS;
    char *frame = worker_exchange(m,w,wire,yyjson_doc_get_root(input),"project-mcp",deadline,SB_RESPONSE_LIMIT);
    yyjson_doc *reply = frame ? yyjson_read(frame, strlen(frame), 0) : NULL;
    yyjson_val *root = yyjson_doc_get_root(reply), *body = yyjson_obj_get(root, "body"); uint64_t status_code = 0;
    static const char *const keys[] = {"status", "body"};
    if (reply && sb_config_keys(root, keys, 2) && sb_integer(yyjson_obj_get(root, "status"), &status_code) &&
        status_code >= 200 && status_code <= 599 && body && (yyjson_is_obj(body) || (status_code == 202 && yyjson_is_null(body)))) {
        *http_status = (int)status_code;
        if (!yyjson_is_null(body)) response = yyjson_val_write(body, 0, NULL);
        if (!response && status_code != 202) { *http_status = 500; response = error_json("Cannot allocate worker response."); }
    } else {
        reap(m, w, "failed"); *http_status = 503;
        response = error_json("Worker disconnected or exceeded its deadline; review possible side effects before any new request. No automatic replay.");
        sb_audit_project(m->audit, "project_failed", w->project->id, w->project->policy_revision, w->pid);
    }
    yyjson_doc_free(reply); free(frame);
done:
    yyjson_doc_free(input); yyjson_mut_doc_free(doc); free(wire);
    *response_length = response ? strlen(response) : 0; return response;
}
static yyjson_mut_val *project_links(manager *m, worker *w, yyjson_mut_doc *doc) {
    yyjson_mut_val *links = yyjson_mut_obj(doc); char local[160], public_url[416];
    snprintf(local, sizeof(local), "http://127.0.0.1:%u/projects/%s/mcp", m->registry.port, w->project->id);
    yyjson_mut_obj_add_str(doc, links, "project_id", w->project->id);
    yyjson_mut_obj_add_strcpy(doc, links, "local_mcp_url", local);
    if (*m->registry.public_origin) {
        snprintf(public_url, sizeof(public_url), "%s/projects/%s/mcp", m->registry.public_origin, w->project->id);
        yyjson_mut_obj_add_strcpy(doc, links, "public_mcp_url", public_url);
    }
    yyjson_mut_obj_add_str(doc, links, "token_env", w->project->token_env);
    yyjson_mut_obj_add_bool(doc, links, "gateway_running", m->gateway != NULL);
    yyjson_mut_obj_add_bool(doc, links, "enabled", w->project->enabled);
    yyjson_mut_obj_add_bool(doc, links, "default_route", !strcmp(w->project->id, m->registry.default_project_id));
    return links;
}
/* Local owner only. Keep configuration details out of the limited remote API. */
static void management_details(manager *m, worker *w, yyjson_mut_doc *doc, yyjson_mut_val *value) {
    const sb_project *p=w->project;
    yyjson_mut_obj_add_val(doc,value,"links",project_links(m,w,doc));
    yyjson_mut_obj_add_str(doc,value,"approval_mode",p->legacy_compatibility ? "legacy" :
        p->policy.approval_mode==SB_PROJECT_ALWAYS ? "always" : p->policy.approval_mode==SB_PROJECT_WORKSPACE_AUTO ? "workspace_auto" : "never");
    yyjson_mut_obj_add_str(doc,value,"file_scope",p->policy.file_scope==SB_FILES_CONFIGURED ? "configured" : "current_user");
    yyjson_mut_obj_add_bool(doc,value,"current_user_read_only",p->policy.current_user_read_only);
    yyjson_mut_obj_add_str(doc,value,"default_identity",p->policy.default_identity==SB_ID_RESTRICTED ? "restricted" :
        p->policy.default_identity==SB_ID_CURRENT_USER ? "current_user" : "administrator");
    yyjson_mut_obj_add_bool(doc,value,"execution_enabled",p->policy.execution_enabled);
    yyjson_mut_obj_add_bool(doc,value,"desktop_enabled",p->policy.desktop.enabled);
    yyjson_mut_obj_add_bool(doc,value,"desktop_control",p->policy.desktop.control);
    yyjson_mut_obj_add_bool(doc,value,"clipboard_read",p->policy.desktop.clipboard_read);
    yyjson_mut_obj_add_bool(doc,value,"clipboard_write",p->policy.desktop.clipboard_write);
    yyjson_mut_obj_add_bool(doc,value,"legacy_compatibility",p->legacy_compatibility);
    yyjson_mut_obj_add_bool(doc,value,"configured_administrator_allowed",p->policy.administrator_allowed);
    yyjson_mut_obj_add_str(doc,value,"data_dir",p->data_dir);
    yyjson_mut_val *directories=yyjson_mut_arr(doc);
    for(size_t i=0;i<p->directory_count;++i) {
        const sb_project_directory *d=&p->directories[i]; yyjson_mut_val *item=yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc,item,"workspace_id",d->id); yyjson_mut_obj_add_str(doc,item,"path",d->path);
        yyjson_mut_obj_add_bool(doc,item,"workspace",d->workspace); yyjson_mut_obj_add_bool(doc,item,"read_only",d->read_only);
        yyjson_mut_arr_add_val(directories,item);
    }
    yyjson_mut_obj_add_val(doc,value,"directories",directories);
}
static char *manager_request(void *context, const char *request, size_t length) {
    manager *m = context;
    yyjson_doc *input = yyjson_read(request, length, 0);
    yyjson_val *root = yyjson_doc_get_root(input);
    static const char *const keys[] = {"command", "id", "request"};
    const char *command = sb_string(yyjson_obj_get(root, "command")), *id = sb_string(yyjson_obj_get(root, "id"));
    yyjson_val *payload = yyjson_obj_get(root, "request");
    char *json = NULL;
    if (!sb_config_keys(root, keys, 3) || !command || (yyjson_obj_get(root, "id") && !sb_identifier(id)) ||
        (payload && !yyjson_is_obj(payload))) { json = error_json("Invalid manager request."); goto done; }
    if (!strcmp(command, "manager-stop") && !id && !payload) {
        json = sb_strdup("{\"status\":\"stopping\"}"); goto done;
    }
    if (WaitForSingleObject(m->stop, 0) == WAIT_OBJECT_0) { json = error_json("Manager is stopping."); goto done; }
    if ((!strcmp(command,"desktop-status") || !strcmp(command,"desktop-pause") || !strcmp(command,"desktop-resume")) && !id && !payload) {
        json=sb_win_desktop_owner(command,m->audit); goto done;
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) goto done;
    yyjson_mut_val *result = yyjson_mut_obj(doc); yyjson_mut_doc_set_root(doc, result);
    if ((!strcmp(command,"administrator-start") || !strcmp(command,"administrator-status") || !strcmp(command,"administrator-stop")) && !id && !payload) {
        sb_error error={0}; sb_platform_mutex_lock(m->administrator_lock);
        administrator_refresh_locked(m);
        if (!strcmp(command,"administrator-start") && !m->administrator_starting && !sb_win_shared_alive(m->administrator)) {
            if (sb_audit_project(m->audit,"administrator_agent_start_intent","manager",m->administrator_generation+1,GetCurrentProcessId())) {
                sb_execution_free(m->administrator); m->administrator=NULL;
                administrator_begin_locked(m,&error);
            } else sb_fail(&error,"Manager audit unavailable; administrator startup blocked.");
            if (*error.text) yyjson_mut_obj_add_strcpy(doc,result,"error",error.text);
        } else if (!strcmp(command,"administrator-stop")) {
            if (m->administrator_starting) InterlockedExchange(&m->administrator_starting->cancelled,1);
            sb_execution_free(m->administrator); m->administrator=NULL;
        }
        yyjson_mut_obj_add_bool(doc,result,"administrator_alive",sb_win_shared_alive(m->administrator));
        yyjson_mut_obj_add_bool(doc,result,"administrator_starting",m->administrator_starting!=NULL);
        yyjson_mut_obj_add_bool(doc,result,"startup_cancel_requested",m->administrator_starting && InterlockedCompareExchange(&m->administrator_starting->cancelled,0,0));
        if (*m->administrator_startup_error.text) yyjson_mut_obj_add_strcpy(doc,result,"startup_error",m->administrator_startup_error.text);
        yyjson_mut_obj_add_uint(doc,result,"administrator_launch_attempts",m->administrator_launch_attempts);
        yyjson_mut_obj_add_uint(doc,result,"administrator_generation",m->administrator_generation);
        yyjson_mut_obj_add_bool(doc,result,"automatic_restart",false);
        if (m->administrator) yyjson_mut_obj_add_val(doc,result,"execution_status",sb_execution_status(m->administrator,doc));
        sb_platform_mutex_unlock(m->administrator_lock);
    } else if ((!strcmp(command, "project-status") || !strcmp(command,"management-status")) && !id && !payload) {
        yyjson_mut_val *array = yyjson_mut_arr(doc);
        for (size_t i = 0; i < m->registry.project_count; ++i) {
            worker *w = &m->workers[i];
            if (TryEnterCriticalSection(&w->lock)) {
                yyjson_mut_val *item=status(m,w,doc);
                if(!strcmp(command,"management-status"))management_details(m,w,doc,item);
                yyjson_mut_arr_add_val(array,item); LeaveCriticalSection(&w->lock);
            }
            else {
                yyjson_mut_val *busy = yyjson_mut_obj(doc); yyjson_mut_obj_add_str(doc, busy, "project_id", w->project->id);
                yyjson_mut_obj_add_str(doc, busy, "state", "busy"); yyjson_mut_arr_add_val(array, busy);
            }
        }
        yyjson_mut_obj_add_val(doc, result, "projects", array);
        yyjson_mut_obj_add_uint(doc, result, "manager_pid", GetCurrentProcessId());
        yyjson_mut_obj_add_bool(doc, result, "elevated", false);
        yyjson_mut_obj_add_uint(doc, result, "owner_workers", 4);
        yyjson_mut_obj_add_bool(doc, result, "http_gateway", m->gateway != NULL);
        yyjson_mut_obj_add_uint(doc, result, "http_workers", m->gateway ? 4 : 0);
        yyjson_mut_obj_add_uint(doc, result, "http_connection_queue", m->gateway ? 4 : 0);
        if(!strcmp(command,"management-status")) {
            yyjson_mut_obj_add_str(doc,result,"version",SB_VERSION);
            yyjson_mut_obj_add_bool(doc,result,"remote_owner_enabled",m->owner_token_length!=0);
            yyjson_mut_obj_add_bool(doc,result,"pause_persisted",false);
            sb_platform_mutex_lock(m->administrator_lock); administrator_refresh_locked(m);
            yyjson_mut_obj_add_bool(doc,result,"administrator_alive",sb_win_shared_alive(m->administrator));
            yyjson_mut_obj_add_bool(doc,result,"administrator_starting",m->administrator_starting!=NULL);
            sb_platform_mutex_unlock(m->administrator_lock);
        }
    } else {
        worker *w = NULL;
        for (size_t i = 0; id && i < m->registry.project_count; ++i) if (!strcmp(id, m->workers[i].project->id)) w = &m->workers[i];
        const char *failure = NULL;
        if (!w) failure = "Unknown project id.";
        else if (!TryEnterCriticalSection(&w->lock)) { failure = "Project has an in-flight request; retry management after it completes."; w = NULL; }
        else {
            sb_error error = {0}; refresh(m, w);
            if (!strcmp(command, "project-start") && !payload) {
                if(w->paused)failure="Project is paused; explicit project-resume is required.";
                else if (!start_worker(m, w, &error)) failure = error.text;
                else yyjson_mut_obj_add_val(doc, result, "project", status(m, w, doc));
            } else if (!strcmp(command, "project-stop") && !payload) {
                reap(m, w, "stopped"); w->paused=false;
                sb_audit_project(m->audit, "project_stopped", w->project->id, w->project->policy_revision, w->pid);
                yyjson_mut_obj_add_val(doc, result, "project", status(m, w, doc));
            } else if (!strcmp(command,"project-pause") && !payload) {
                reap(m,w,"stopped"); w->paused=true;
                if(!strcmp(w->state,"stopped"))w->state="paused";
                sb_audit_project(m->audit,"project_paused",w->project->id,w->project->policy_revision,w->pid);
                yyjson_mut_obj_add_val(doc,result,"project",status(m,w,doc));
            } else if (!strcmp(command,"project-resume") && !payload) {
                if(!w->paused)failure="Project is not paused; use explicit project-start.";
                else if(!start_worker(m,w,&error))failure=error.text;
                else {
                    w->paused=false; sb_audit_project(m->audit,"project_resumed",w->project->id,w->project->policy_revision,w->pid);
                    yyjson_mut_obj_add_val(doc,result,"project",status(m,w,doc));
                }
            } else if (!strcmp(command, "project-status") && !payload) yyjson_mut_obj_add_val(doc, result, "project", status(m, w, doc));
            else if (!strcmp(command, "project-links") && !payload) yyjson_mut_obj_add_val(doc, result, "links", project_links(m, w, doc));
            else if (!strcmp(command,"project-administrator-boundary-probe") && !payload) {
                sb_platform_mutex_lock(m->administrator_lock);
                if (!w->administrator_allowed || !administrator_register_locked(m,w,&error)) failure=*error.text ? error.text : "Administrator project must be permitted and running.";
                else {
                    static const char *const faults[]={"project_id","policy_revision","authorization_version","host_instance_id","deadline_ms","operation_sha256","sequence","actor","approved"};
                    static const char probe_json[]="{\"name\":\"fs_list\",\"arguments\":{\"workspace_id\":\"work\",\"identity\":\"administrator\"}}";
                    yyjson_doc *probe=yyjson_read(probe_json,sizeof(probe_json)-1,0);
                    /* Use a parsed object; no target is resolved for invalid bindings. */
                    unsigned passed=0;
                    for (size_t i=0; probe && i<sizeof(faults)/sizeof(faults[0]); ++i) {
                        char *reply=administrator_exchange_locked(m,w,"call","manager-owner",yyjson_doc_get_root(probe),faults[i]);
                        yyjson_doc *response=reply ? yyjson_read(reply,strlen(reply),0) : NULL;
                        bool rejected=response && sb_string(yyjson_obj_get(yyjson_doc_get_root(response),"error")) && sb_win_shared_alive(m->administrator);
                        yyjson_doc_free(response); free(reply); if (!rejected) break; ++passed;
                    }
                    yyjson_doc_free(probe);
                    yyjson_mut_obj_add_uint(doc,result,"bindings_rejected",passed);
                    if (passed!=sizeof(faults)/sizeof(faults[0])) failure="Administrator operation binding probe failed.";
                }
                sb_platform_mutex_unlock(m->administrator_lock);
            } else if ((!strcmp(command,"project-administrator-allow") || !strcmp(command,"project-administrator-revoke")) && !payload) {
                bool allow=!strcmp(command,"project-administrator-allow");
                sb_platform_mutex_lock(m->administrator_lock);
                if (allow && !administrator_eligible(w)) failure="Project isolation or legacy configuration is incompatible with administrator permission.";
                else if (!sb_audit_project(m->audit,allow?"administrator_permission_granted":"administrator_permission_revoked",w->project->id,w->project->policy_revision,w->pid))
                    failure="Manager audit unavailable; administrator authorization update blocked.";
                else {
                    bool old=w->administrator_allowed; w->administrator_allowed=false;
                    if (w->administrator_generation == m->administrator_generation && w->administrator_generation && sb_win_shared_alive(m->administrator)) {
                        yyjson_mut_doc *update=yyjson_mut_doc_new(NULL);
                        yyjson_mut_val *value=update ? yyjson_mut_obj(update) : NULL;
                        if (value) { yyjson_mut_doc_set_root(update,value); yyjson_mut_obj_add_bool(update,value,"allowed",allow); yyjson_mut_obj_add_uint(update,value,"new_version",w->authorization_version+1); }
                        char *wire=update ? yyjson_mut_write(update,0,NULL) : NULL;
                        yyjson_doc *operation=wire ? yyjson_read(wire,strlen(wire),0) : NULL;
                        char *reply=operation ? administrator_operation_locked(m,w,"authorization","manager-owner",yyjson_doc_get_root(operation)) : NULL;
                        if (!administrator_ok(reply)) { sb_execution_free(m->administrator); m->administrator=NULL; failure="Administrator authorization update failed; agent stopped and permission revoked."; }
                        free(reply); free(wire); yyjson_doc_free(operation); yyjson_mut_doc_free(update);
                    }
                    ++w->authorization_version;
                    if (!failure) w->administrator_allowed=allow;
                    yyjson_mut_obj_add_bool(doc,result,"previous_administrator_allowed",old); yyjson_mut_obj_add_bool(doc,result,"administrator_allowed",w->administrator_allowed);
                    yyjson_mut_obj_add_uint(doc,result,"authorization_version",w->authorization_version); yyjson_mut_obj_add_bool(doc,result,"running_configuration_changed",true);
                    yyjson_mut_obj_add_bool(doc,result,"persisted_to_registry",false);
                }
                sb_platform_mutex_unlock(m->administrator_lock);
            } else if (!strcmp(command,"project-administrator-owner") && payload) {
                if (!w->process) failure="Project is not running; explicit owner start is required.";
                else {
                    char *response=administrator_call(m,w,"owner","manager-owner",payload);
                    yyjson_doc *reply=response ? yyjson_read(response,strlen(response),0) : NULL;
                    if (reply) yyjson_mut_obj_add_val(doc,result,"response",yyjson_val_mut_copy(doc,yyjson_doc_get_root(reply)));
                    else failure="Administrator approval channel unavailable.";
                    yyjson_doc_free(reply); free(response);
                }
            } else if ((!strcmp(command, "project-request") || !strcmp(command, "project-owner")) && payload) {
                if (!w->process) failure = "Project is not running; explicit owner start is required.";
                else {
                    yyjson_mut_val *envelope = yyjson_mut_obj(doc);
                    yyjson_mut_obj_add_str(doc, envelope, "kind", !strcmp(command, "project-owner") ? "owner" : "mcp");
                    yyjson_mut_obj_add_val(doc, envelope, "request", yyjson_val_mut_copy(doc, payload));
                    char *wire = yyjson_mut_val_write(envelope, 0, NULL);
                    uint64_t deadline = sb_platform_millis() + WORKER_IO_MS;
                    char *response = wire ? worker_exchange(m,w,wire,payload,"manager-owner",deadline,SB_RESPONSE_LIMIT - 1024) : NULL;
                    free(wire);
                    yyjson_doc *reply = response ? yyjson_read(response, strlen(response), 0) : NULL;
                    if (reply) {
                        yyjson_mut_obj_add_str(doc, result, "project_id", w->project->id);
                        yyjson_mut_obj_add_val(doc, result, "response", yyjson_val_mut_copy(doc, yyjson_doc_get_root(reply)));
                    } else {
                        reap(m, w, "failed"); failure = "Worker disconnected or exceeded its deadline; review possible side effects before any new request. No automatic replay.";
                        sb_audit_project(m->audit, "project_failed", w->project->id, w->project->policy_revision, w->pid);
                    }
                    yyjson_doc_free(reply); free(response);
                }
            } else failure = "Unknown manager command or invalid arguments.";
            if (failure) yyjson_mut_obj_add_strcpy(doc, result, "error", failure);
            LeaveCriticalSection(&w->lock);
        }
        if (!w && failure) yyjson_mut_obj_add_strcpy(doc, result, "error", failure);
    }
    json = yyjson_mut_write(doc, 0, NULL); yyjson_mut_doc_free(doc);
done:
    yyjson_doc_free(input); return json;
}
static bool preflight(manager *m, const char *data_dir, const char *bytes, size_t length, sb_error *error) {
    if (!ordinary()) { sb_fail(error, "Manager requires a normal, unelevated owner token; it never requests UAC."); return false; }
    if (!sb_project_path_valid(data_dir) || !sb_projects_parse(&m->registry, bytes, length, error)) return false;
    m->lease = sb_platform_lease(data_dir, error); if (!m->lease) return false;
    m->data_root = sb_platform_root(data_dir, error); if (!m->data_root) return false;
    m->audit = sb_audit_new(m->lease, SB_AUDIT_LIMIT, error); if (!m->audit) return false;
    yyjson_doc *source = yyjson_read(bytes, length, 0);
    yyjson_val *array = yyjson_obj_get(yyjson_doc_get_root(source), "projects");
    bool ok = false;
    for (size_t i = 0; i < m->registry.project_count; ++i) {
        worker *w = &m->workers[i]; w->project = &m->registry.projects[i]; w->state = "stopped";
        w->administrator_allowed=w->project->policy.administrator_allowed && !w->project->legacy_compatibility; w->authorization_version=1;
        InitializeCriticalSection(&w->lock); ++m->initialized;
        /* Physical Data comparison includes junction/UNC aliases; roots stay pinned. */
        sb_lease *probe = sb_platform_lease(w->project->data_dir, error);
        if (!probe) goto done;
        w->data_root = sb_platform_root(w->project->data_dir, error); sb_platform_lease_close(probe);
        if (!w->data_root) goto done;
        if (!sb_platform_root_identity(w->data_root, w->data_identity, error)) goto done;
        if (sb_platform_roots_overlap(m->data_root, w->data_root)) goto overlap;
        for (size_t j = 0; j < i; ++j) if (sb_platform_roots_overlap(m->workers[j].data_root, w->data_root)) goto overlap;
        yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL); if (!doc) goto done;
        yyjson_mut_val *root = yyjson_mut_obj(doc), *projects = yyjson_mut_arr(doc); yyjson_mut_doc_set_root(doc, root);
        yyjson_mut_obj_add_uint(doc, root, "format_version", SB_PROJECT_FORMAT);
        yyjson_mut_arr_add_val(projects, yyjson_val_mut_copy(doc, yyjson_arr_get(array, i)));
        yyjson_mut_obj_add_val(doc, root, "projects", projects);
        w->snapshot = yyjson_mut_write(doc, 0, NULL); yyjson_mut_doc_free(doc);
        if (!w->snapshot) goto done;
    }
    /* Configured-directory projects must not expose manager/project state,
       including aliases. Account-trust projects retain their declared scope. */
    for (size_t i = 0; i < m->registry.project_count; ++i) {
        const sb_project *project = &m->registry.projects[i];
        for(size_t j=0;j<project->policy.runtime_file_count;++j) {
            char *relative=NULL;sb_root *parent=sb_platform_account_parent(project->policy.runtime_files[j],&relative,error);
            if(!parent){free(relative);goto done;}
            bool conflict=sb_platform_target_within(m->data_root,parent,relative,error);
            for(size_t k=0;!conflict && k<m->registry.project_count;++k)
                conflict=sb_platform_target_within(m->workers[k].data_root,parent,relative,error);
            sb_platform_root_close(parent);free(relative);
            if(*error->text)goto done;
            if(conflict){sb_fail(error,"Restricted runtime resources overlap protected manager/project Data.");goto done;}
        }
        if (project->policy.file_scope != SB_FILES_CONFIGURED) continue;
        for (size_t j = 0; j < project->directory_count; ++j) {
            sb_root *directory = sb_platform_root(project->directories[j].path, error);
            if (!directory) goto done;
            bool conflict = sb_platform_roots_overlap(directory, m->data_root);
            for (size_t k = 0; !conflict && k < m->registry.project_count; ++k)
                conflict = sb_platform_roots_overlap(directory, m->workers[k].data_root);
            sb_platform_root_close(directory);
            if (conflict) { sb_fail(error, "Configured project directories overlap protected manager/project Data."); goto done; }
        }
    }
    ok = true; goto done;
overlap:
    sb_fail(error, "Physical manager/project Data directories overlap; no workers started.");
done:
    yyjson_doc_free(source); return ok;
}
static int run_manager(const char *source, const char *data, bool http) {
    sb_error error = {0};size_t length=0;wchar_t *source_name=wide(source);
    HANDLE source_pin=source_name?CreateFileW(source_name,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL):INVALID_HANDLE_VALUE;
    free(source_name);
    char *bytes=source_pin!=INVALID_HANDLE_VALUE?sb_platform_load_config(source,&length,&error):NULL;
    manager *m = calloc(1, sizeof(*m)); int code = 2;
    if (m) m->administrator_lock=sb_platform_mutex_new();
    if (!m || !m->administrator_lock || !bytes || !preflight(m, data, bytes, length, &error)) goto done;
    m->stop = CreateEventW(NULL, TRUE, FALSE, NULL); if (!m->stop) goto done;
    m->desktop_session=sb_win_desktop_session_new();
    if (http) {
        if (!gateway_credentials(m, &error)) goto done;
        sb_config_defaults(&m->gateway_config); m->gateway_config.port = m->registry.port;
        if (*m->registry.public_origin && !sb_config_public_url(&m->gateway_config, m->registry.public_origin, &error)) goto done;
        const sb_http_routes routes = {gateway_open, gateway_call, gateway_close, owner_enabled, owner_authorized, owner_call};
        m->gateway = sb_http_start(&m->gateway_config, &routes, m, &error); if (!m->gateway) goto done;
    }
    m->owner = sb_owner_start_handler(data, manager_request, m, &error); if (!m->owner) goto done;
    printf("{\"manager_ready\":true,\"elevated\":false,\"http_gateway\":%s}\n", http ? "true" : "false"); fflush(stdout);
    /* Explicit startup per project. The manager never replays failed work. */
    while (WaitForSingleObject(m->stop, 100) == WAIT_TIMEOUT && !sb_owner_stopping(m->owner) && (!m->gateway || !sb_http_stopping())) {
        sb_platform_mutex_lock(m->administrator_lock); administrator_refresh_locked(m); sb_platform_mutex_unlock(m->administrator_lock);
        for (size_t i = 0; i < m->registry.project_count; ++i) {
            worker *w = &m->workers[i];
            if (TryEnterCriticalSection(&w->lock)) { refresh(m, w); LeaveCriticalSection(&w->lock); }
        }
    }
    SetEvent(m->stop); code = 0;
done:
    free(bytes);
    if (m) {
        if (m->stop) SetEvent(m->stop);
        if (m->administrator_lock) {
            sb_platform_mutex_lock(m->administrator_lock);
            if (m->administrator_starting) InterlockedExchange(&m->administrator_starting->cancelled,1);
            sb_platform_mutex_unlock(m->administrator_lock);
        }
        sb_http_stop(m->gateway);
        sb_owner_stop(m->owner);
        for (size_t i = 0; i < m->initialized; ++i) {
            if (m->workers[i].process)
                sb_audit_project(m->audit, "project_stopped", m->workers[i].project->id, m->workers[i].project->policy_revision, m->workers[i].pid);
            reap(m, &m->workers[i], "stopped"); free(m->workers[i].snapshot);
            SecureZeroMemory(m->workers[i].token, sizeof(m->workers[i].token));
            sb_platform_root_close(m->workers[i].data_root); DeleteCriticalSection(&m->workers[i].lock);
        }
        if (m->stop) CloseHandle(m->stop);
        sb_win_desktop_session_free(m->desktop_session);
        sb_audit_free(m->audit); sb_platform_root_close(m->data_root); sb_platform_lease_close(m->lease);
        sb_config_free(&m->gateway_config);
        if (m->administrator_starting) {
            WaitForSingleObject(m->administrator_starting->thread,500);
            administrator_refresh_locked(m);
            /* Pending UAC context owns no manager memory. run_manager returns
               directly to process exit; Windows reclaims this thread/context.
               A late agent cannot bind to a dead manager or receive projects. */
        }
        sb_execution_free(m->administrator); sb_platform_mutex_free(m->administrator_lock);
        sb_projects_free(&m->registry); SecureZeroMemory(m->owner_token,sizeof(m->owner_token)); free(m);
    }
    if(source_pin!=INVALID_HANDLE_VALUE)CloseHandle(source_pin);
    if (code) fprintf(stderr, "ShellBot: %s\n", *error.text ? error.text : "Manager startup failed.");
    return code;
}

static char *worker_administrator_call(void *context, const char *name, yyjson_val *args) {
    HANDLE pipe=context;
    yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL); if (!doc) return NULL;
    yyjson_mut_val *root=yyjson_mut_obj(doc),*request=yyjson_mut_obj(doc); yyjson_mut_doc_set_root(doc,root);
    yyjson_mut_obj_add_str(doc,request,"name",name); yyjson_mut_obj_add_val(doc,request,"arguments",yyjson_val_mut_copy(doc,args));
    yyjson_mut_obj_add_val(doc,root,"administrator_request",request);
    char *wire=yyjson_mut_write(doc,0,NULL); yyjson_mut_doc_free(doc);
    char *reply=wire && send_frame(pipe,wire,false,0) ? receive_frame(pipe,false,0,SB_RESPONSE_LIMIT) : NULL;
    free(wire); return reply;
}
int sb_project_worker(int argc, wchar_t **argv) {
    if (argc != 4 || !ordinary()) return 2;
    wchar_t *end = NULL; unsigned long long value = _wcstoui64(argv[2], &end, 10);
    if (!value || !end || *end) return 2;
    HANDLE pipe = (HANDLE)(uintptr_t)value;
    unsigned long expected = wcstoul(argv[3], &end, 10); ULONG server = 0;
    if (!expected || !end || *end || !GetNamedPipeServerProcessId(pipe, &server) || server != expected) return 2;
    HANDLE parent = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, server);
    if (!parent || WaitForSingleObject(parent, 0) != WAIT_TIMEOUT || !same_executable(parent)) { if (parent) CloseHandle(parent); return 2; }
    CloseHandle(parent);
    if (!SetHandleInformation(pipe, HANDLE_FLAG_INHERIT, 0)) return 2;
    sb_error error = {0}; sb_project_registry registry = {0}; sb_host host = {0}; int code = 2;
    char *bytes = receive_frame(pipe, false, 0, SB_TEXT_LIMIT);
    char *data_identity = bytes ? receive_frame(pipe, false, 0, 64) : NULL;
    if (!bytes || !sb_projects_parse(&registry, bytes, strlen(bytes), &error) || registry.project_count != 1) goto done;
    host.project = &registry.projects[0]; const sb_project *project = host.project;
    host.administrator_call=worker_administrator_call; host.administrator_context=pipe;
    sb_config_defaults(&host.config); host.started_ms = sb_platform_millis();
    host.config.data_dir = sb_strdup(project->data_dir); strcpy(host.config.token_env, project->token_env);
    host.config.limits = project->limits; host.config.approval_ttl_seconds = project->approval_ttl_seconds; host.config.audit_max_bytes = project->audit_max_bytes;
    host.config.execution_profile = SB_EXEC_USER;
    /* Legacy administrator stays unavailable until shared, project-authorized M5. */
    bool command_candidate = project->enabled && project->policy.execution_enabled && project->policy.default_identity == SB_ID_CURRENT_USER;
    if (command_candidate) {
        sb_request_context ctx = {.actor = "manager-owner", .project = project, .policy_revision = project->policy_revision};
        const sb_project_directory *cwd = sb_project_directory_find(project, project->default_workspace_id);
        sb_project_operation op = {.tool = "task_start", .directory = cwd, .identity = SB_ID_CURRENT_USER};
        host.config.execution_enabled = sb_project_evaluate(&ctx, &op).decision != SB_DECISION_DENY;
    }
    host.lease = host.config.data_dir && data_identity ? sb_platform_lease_bound(host.config.data_dir, data_identity, &error) : NULL;
    if (!host.lease) goto done;
    for (size_t i = 0; i < project->directory_count; ++i) {
        sb_workspace *workspace = &host.config.workspaces[host.config.workspace_count];
        workspace->root = sb_platform_root(project->directories[i].path, &error);
        if (!workspace->root) goto done;
        strcpy(workspace->id, project->directories[i].id); workspace->read_only = project->directories[i].read_only; ++host.config.workspace_count;
        for (size_t j = 0; j < i; ++j) if (sb_platform_roots_overlap(host.config.workspaces[j].root, workspace->root)) {
            sb_fail(&error, "Physical project directory aliases overlap."); goto done;
        }
    }
    host.execution = sb_execution_new(&host.config, &error); if (!host.execution) goto done;
    sb_execution_prepare_project(host.execution,&host);
    host.audit = sb_audit_new(host.lease, host.config.audit_max_bytes, &error); if (!host.audit) goto done;
    if (!project->legacy_compatibility) {
        host.store=sb_store_new(host.lease,project->id,project->policy_revision,sb_execution_instance(host.execution),"ordinary",&error);
        if (!host.store) goto done;
        sb_win_execution_store(host.execution,host.store,host.audit);
    }
    host.policy = sb_policy_new(host.config.approval_ttl_seconds, host.audit, &error); if (!host.policy) goto done;
    sb_policy_limit(host.policy, host.config.limits.approval_records);
    sb_policy_store(host.policy,host.store);
    host.transfers=sb_transfers_new(&error);if(!host.transfers)goto done;
    host.desktop=sb_win_desktop_new(&host,&error);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL); if (!doc) goto done;
    yyjson_mut_val *ready = yyjson_mut_obj(doc); yyjson_mut_doc_set_root(doc, ready);
    yyjson_mut_obj_add_bool(doc, ready, "ready", true); yyjson_mut_obj_add_str(doc, ready, "project_id", project->id);
    yyjson_mut_obj_add_uint(doc, ready, "policy_revision", project->policy_revision); yyjson_mut_obj_add_uint(doc, ready, "pid", GetCurrentProcessId());
    yyjson_mut_obj_add_str(doc, ready, "host_instance_id", sb_execution_instance(host.execution));
    char *greeting = yyjson_mut_write(doc, 0, NULL); yyjson_mut_doc_free(doc);
    bool sent = send_frame(pipe, greeting, false, 0); free(greeting); if (!sent) goto done;
    sb_protocol protocol = {0}; code = 0;
    for (;;) {
        char *envelope = receive_frame(pipe, false, 0, WORKER_REQUEST_LIMIT); if (!envelope) break;
        yyjson_doc *input = yyjson_read(envelope, strlen(envelope), 0);
        yyjson_val *root = yyjson_doc_get_root(input), *request = yyjson_obj_get(root, "request");
        static const char *const keys[] = {"kind", "request", "administrator_backend", "owner_url"};
        static const char *const http_keys[] = {"kind", "request", "version", "method", "name", "administrator_backend", "owner_url"};
        const char *kind = sb_string(yyjson_obj_get(root, "kind"));
        char *wire = yyjson_val_write(request, 0, NULL), *response = NULL;
        bool is_http = kind && !strcmp(kind, "http");
        bool valid = wire && kind && yyjson_is_bool(yyjson_obj_get(root,"administrator_backend")) &&
            (is_http ? sb_config_keys(root, http_keys, 7) : yyjson_is_obj(request) && sb_config_keys(root, keys, 4));
        const char *owner_url=sb_string(yyjson_obj_get(root,"owner_url"));
        if (!owner_url || strlen(owner_url)>=sizeof(host.owner_url)) valid=false;
        if (valid) strcpy(host.owner_url,owner_url);
        host.administrator_backend=valid && yyjson_get_bool(yyjson_obj_get(root,"administrator_backend"));
        const char *headers[3] = {0}; const char *names[] = {"version", "method", "name"};
        for (size_t i = 0; valid && is_http && i < 3; ++i) {
            yyjson_val *header = yyjson_obj_get(root, names[i]); headers[i] = sb_string(header);
            if (header && (!headers[i] || strlen(headers[i]) > 8192)) valid = false;
        }
        if (valid && !strcmp(kind,"shutdown")) { free(wire); yyjson_doc_free(input); free(envelope); break; }
        if (!valid) response = error_json("Invalid worker envelope.");
        else if (!strcmp(kind, "owner")) response = sb_policy_owner_request(host.policy, wire, strlen(wire));
        else if (!strcmp(kind,"remote-owner")) response=sb_policy_owner_request_actor(host.policy,wire,strlen(wire),"remote-owner");
        else if (!strcmp(kind, "mcp") || is_http) {
            sb_request_context ctx = {.actor = is_http ? "project-mcp" : "manager-owner", .version = headers[0], .method = headers[1], .name = headers[2],
                .project = project, .policy_revision = project->policy_revision};
            size_t response_length = 0; int http_status = 0;
            host.http_mode = is_http;
            response = sb_dispatch(&host, is_http ? NULL : &protocol, wire, strlen(wire), &ctx, &response_length, &http_status);
            if (is_http) {
                yyjson_doc *body = response ? yyjson_read(response, response_length, 0) : NULL;
                yyjson_mut_doc *wrapped = yyjson_mut_doc_new(NULL);
                char *encoded = NULL;
                if (wrapped && (body || (!response && http_status == 202))) {
                    yyjson_mut_val *result = yyjson_mut_obj(wrapped); yyjson_mut_doc_set_root(wrapped, result);
                    yyjson_mut_obj_add_uint(wrapped, result, "status", (unsigned)http_status);
                    yyjson_mut_obj_add_val(wrapped, result, "body", body ? yyjson_val_mut_copy(wrapped, yyjson_doc_get_root(body)) : yyjson_mut_null(wrapped));
                    encoded = yyjson_mut_write(wrapped, 0, NULL);
                }
                yyjson_doc_free(body); yyjson_mut_doc_free(wrapped); free(response); response = encoded;
            } else if (!response && http_status == 202) response = sb_strdup("{\"notification\":true}");
        } else response = error_json("Unknown worker envelope kind.");
        free(wire); yyjson_doc_free(input); free(envelope);
        sent = send_frame(pipe, response, false, 0); free(response); if (!sent) break;
    }
done:
    if (code && *error.text) { char *failure=error_json(error.text); if (failure) send_frame(pipe,failure,false,0); free(failure); }
    free(bytes); free(data_identity); sb_win_desktop_free(host.desktop); sb_transfers_free(host.transfers); sb_policy_free(host.policy); sb_execution_free(host.execution); sb_store_free(host.store); sb_audit_free(host.audit);
    sb_config_free(&host.config); sb_platform_lease_close(host.lease); sb_projects_free(&registry); CloseHandle(pipe);
    return code;
}

bool sb_manager_option(const char *option) {
    static const char *const names[] = {"--manager", "--manager-stop", "--project-create", "--project-configure", "--project-start",
        "--project-stop", "--project-pause", "--project-resume", "--project-status", "--management-status", "--manage", "--project-request", "--project-owner", "--project-links", "--administrator-start", "--administrator-stop", "--administrator-status",
        "--project-administrator-allow", "--project-administrator-revoke", "--project-administrator-owner", "--project-administrator-boundary-probe",
        "--desktop-status", "--desktop-pause", "--desktop-resume"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) if (!strcmp(option, names[i])) return true;
    return false;
}
int sb_manager_cli(int argc, char **argv) {
    const char *mode = argv[1], *id = NULL, *registry = NULL, *data = NULL, *definition = NULL, *output = NULL, *request_file = NULL;
    bool editing = !strcmp(mode, "--project-create") || !strcmp(mode, "--project-configure");
    bool payload = !strcmp(mode, "--project-request") || !strcmp(mode, "--project-owner") || !strcmp(mode,"--project-administrator-owner");
    bool global=!strcmp(mode,"--administrator-start") || !strcmp(mode,"--administrator-stop") || !strcmp(mode,"--administrator-status") ||
        !strcmp(mode,"--management-status") || !strcmp(mode,"--manage") ||
        !strcmp(mode,"--desktop-status") || !strcmp(mode,"--desktop-pause") || !strcmp(mode,"--desktop-resume");
    bool http = false;
    int index = 2;
    if (strcmp(mode, "--manager") && strcmp(mode, "--manager-stop") && !global) {
        if (index < argc && strncmp(argv[index], "--", 2)) id = argv[index++];
        if ((!id && strcmp(mode, "--project-status")) || (id && !sb_identifier(id))) goto invalid;
    }
    for (; index < argc; ++index) {
        if (!strcmp(argv[index], "--http") && !strcmp(mode, "--manager") && !http) { http = true; continue; }
        const char **slot = !strcmp(argv[index], "--projects") ? &registry : !strcmp(argv[index], "--manager-data-dir") ? &data :
            !strcmp(argv[index], "--project-config") ? &definition : !strcmp(argv[index], "--output") ? &output :
            !strcmp(argv[index], "--request") ? &request_file : NULL;
        if (!slot || *slot || index + 1 >= argc || !*argv[index + 1]) goto invalid;
        *slot = argv[++index];
    }
    sb_error error = {0}; int code = 2; char *bytes = NULL, *project = NULL, *json = NULL, *backup = NULL;
    size_t length = 0, project_length = 0;
    if (editing) {
        if (!definition || !output || data || request_file || !sb_project_path_valid(output) || (!strcmp(mode, "--project-configure") && !registry)) goto invalid;
        if (registry) { bytes = sb_platform_load_config(registry, &length, &error); if (!bytes) goto done; }
        project = sb_platform_load_config(definition, &project_length, &error); if (!project) goto done;
        json = sb_projects_edit(bytes, length, project, project_length, id, !strcmp(mode, "--project-configure"), &error); if (!json) goto done;
        size_t capacity = strlen(output) + sizeof(".source-backup.json"); backup = malloc(capacity); if (!backup) goto done;
        snprintf(backup, capacity, "%s.source-backup.json", output);
        if (!sb_platform_export_config(output, backup, json, strlen(json), bytes ? bytes : project, bytes ? length : project_length, &error)) goto done;
        puts("{\"exported\":true,\"source_backup_saved\":true,\"running_configuration_changed\":false}"); code = 0;
    } else if (!strcmp(mode, "--manager")) {
        if (!registry || !data || definition || output || request_file) goto invalid;
        code = run_manager(registry, data, http);
    } else if(!strcmp(mode,"--manage")) {
        if(!data || !sb_project_path_valid(data) || registry || definition || output || request_file)goto invalid;
        code=sb_manager_ui(data);
    } else {
        if (!data || !sb_project_path_valid(data) || registry || definition || output || (payload != (request_file != NULL))) goto invalid;
        yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL); if (!doc) goto done;
        yyjson_mut_val *root = yyjson_mut_obj(doc); yyjson_mut_doc_set_root(doc, root);
        yyjson_mut_obj_add_str(doc, root, "command", mode + 2); if (id) yyjson_mut_obj_add_str(doc, root, "id", id);
        if (request_file) {
            project = sb_platform_load_config(request_file, &project_length, &error);
            yyjson_doc *input = project ? yyjson_read(project, project_length, 0) : NULL;
            if (!input || !yyjson_is_obj(yyjson_doc_get_root(input))) { yyjson_doc_free(input); yyjson_mut_doc_free(doc); sb_fail(&error, "Request file must contain a bounded JSON object."); goto done; }
            yyjson_mut_obj_add_val(doc, root, "request", yyjson_val_mut_copy(doc, yyjson_doc_get_root(input))); yyjson_doc_free(input);
        }
        json = yyjson_mut_write(doc, 0, NULL); yyjson_mut_doc_free(doc); if (!json) goto done;
        bytes = sb_owner_exchange(data, json, strlen(json), &error); if (!bytes) goto done;
        yyjson_doc *reply = yyjson_read(bytes, strlen(bytes), 0);
        if (reply) { puts(bytes); code = yyjson_obj_get(yyjson_doc_get_root(reply), "error") ? 2 : 0; }
        yyjson_doc_free(reply);
    }
done:
    if (code && strcmp(mode, "--manager")) fprintf(stderr, "ShellBot: %s\n", *error.text ? error.text : "Command failed.");
    free(bytes); free(project); free(json); free(backup); return code;
invalid:
    fputs("ShellBot: Invalid project management arguments. Use --help.\n", stderr); return 2;
}
