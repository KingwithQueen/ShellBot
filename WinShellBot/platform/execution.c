#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sddl.h>
#include <shellapi.h>
#include <objbase.h>
#include "sb.h"
#include "execution.h"
#include "task_process.h"
#include "sandbox.h"


/* A 16 KiB UTF-8 page can expand to six JSON bytes per control byte,
   in addition to the lossless base64 representation and task metadata. */
#define SB_AGENT_TASK_REPLY_LIMIT 262144u
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define SB_AGENT_MAGIC 0x53424536u
#define SB_AGENT_QUERY 1u
#define SB_AGENT_STOP 2u
#define SB_AGENT_TASK 3u

typedef struct {
    DWORD magic, version, process_id, session_id, integrity;
    DWORD elevated, restricted, appcontainer, administrator_enabled;
    char user_sid[192];
} identity;

struct sb_execution {
    const sb_project *project;
    sb_win_sandbox *sandbox;
    char sandbox_error[256];
    bool configured, restricted_prepared, restricted_owner_rejected;
    bool agent_started, agent_healthy;
    sb_execution_profile profile;
    sb_execution_approval approval;
    identity host, agent;
    HANDLE restricted_token, pipe, process;
    sb_mutex *mutex;
    sb_win_tasks *tasks;
    char instance[33];
};

static bool token_dword(HANDLE token, TOKEN_INFORMATION_CLASS kind, DWORD *value) {
    DWORD size = 0;
    return GetTokenInformation(token, kind, value, sizeof(*value), &size) != FALSE;
}

static void *token_buffer(HANDLE token, TOKEN_INFORMATION_CLASS kind) {
    DWORD size = 0;
    GetTokenInformation(token, kind, NULL, 0, &size);
    void *buffer = size ? malloc(size) : NULL;
    if (buffer && !GetTokenInformation(token, kind, buffer, size, &size)) { free(buffer); buffer = NULL; }
    return buffer;
}

static bool read_identity(HANDLE token, identity *result) {
    memset(result, 0, sizeof(*result));
    result->magic = SB_AGENT_MAGIC; result->version = 1;
    result->process_id = GetCurrentProcessId();
    void *user = token_buffer(token, TokenUser), *label = token_buffer(token, TokenIntegrityLevel);
    TOKEN_GROUPS *groups = token_buffer(token, TokenGroups);
    LPSTR sid = NULL;
    bool ok = user && label && groups &&
        ConvertSidToStringSidA(((TOKEN_USER *)user)->User.Sid, &sid) &&
        token_dword(token, TokenSessionId, &result->session_id) &&
        token_dword(token, TokenElevation, &result->elevated) &&
        token_dword(token, TokenIsAppContainer, &result->appcontainer);
    if (ok) {
        if (strlen(sid) >= sizeof(result->user_sid)) ok = false;
        else strcpy(result->user_sid, sid);
        PSID integrity_sid = ((TOKEN_MANDATORY_LABEL *)label)->Label.Sid;
        UCHAR count = *GetSidSubAuthorityCount(integrity_sid);
        if (!count) ok = false;
        else result->integrity = *GetSidSubAuthority(integrity_sid, (DWORD)count - 1);
        result->restricted = IsTokenRestricted(token) != FALSE;
        BYTE admin_sid[SECURITY_MAX_SID_SIZE]; DWORD admin_size = sizeof(admin_sid);
        if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, NULL, admin_sid, &admin_size)) ok = false;
        else for (DWORD i = 0; i < groups->GroupCount; ++i) {
            DWORD attributes = groups->Groups[i].Attributes;
            if (EqualSid(groups->Groups[i].Sid, admin_sid) && (attributes & SE_GROUP_ENABLED) &&
                !(attributes & SE_GROUP_USE_FOR_DENY_ONLY)) result->administrator_enabled = 1;
        }
    }
    if (sid) LocalFree(sid);
    free(user); free(label); free(groups);
    return ok;
}

bool sb_win_owner_token_allowed(HANDLE token) {
    DWORD appcontainer = 0;
    TOKEN_MANDATORY_LABEL *label = token_buffer(token, TokenIntegrityLevel);
    bool ok = label && token_dword(token, TokenIsAppContainer, &appcontainer) &&
        !appcontainer && !IsTokenRestricted(token);
    if (ok) {
        UCHAR count = *GetSidSubAuthorityCount(label->Label.Sid);
        ok = count && *GetSidSubAuthority(label->Label.Sid, (DWORD)count - 1) >= SECURITY_MANDATORY_MEDIUM_RID;
    }
    free(label);
    return ok;
}

/* Preparation only. This token does not yet grant any command capability.
   Public Users resources remain accessible; this is not a directory sandbox. */
static HANDLE prepare_restricted_token(void) {
    HANDLE source = NULL, restricted = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ADJUST_DEFAULT, &source)) return NULL;
    DWORD elevated = 0;
    if (!token_dword(source, TokenElevation, &elevated) || elevated) { CloseHandle(source); return NULL; }
    BYTE public_sid[SECURITY_MAX_SID_SIZE]; DWORD public_size = sizeof(public_sid);
    SID_AND_ATTRIBUTES restricting = {public_sid, 0};
    bool ok = CreateWellKnownSid(WinAuthenticatedUserSid, NULL, public_sid, &public_size) &&
        CreateRestrictedToken(source, DISABLE_MAX_PRIVILEGE, 0, NULL, 0, NULL, 1, &restricting, &restricted);
    CloseHandle(source);
    if (!ok) return NULL;
    SID_IDENTIFIER_AUTHORITY mandatory = SECURITY_MANDATORY_LABEL_AUTHORITY;
    PSID low = NULL;
    TOKEN_MANDATORY_LABEL label = {0};
    ok = AllocateAndInitializeSid(&mandatory, 1, SECURITY_MANDATORY_LOW_RID, 0, 0, 0, 0, 0, 0, 0, &low) != FALSE;
    if (ok) {
        label.Label.Sid = low; label.Label.Attributes = SE_GROUP_INTEGRITY;
        ok = SetTokenInformation(restricted, TokenIntegrityLevel, &label,
            (DWORD)sizeof(label) + GetLengthSid(low)) != FALSE;
    }
    if (low) FreeSid(low);
    if (!ok || !IsTokenRestricted(restricted) || sb_win_owner_token_allowed(restricted)) {
        CloseHandle(restricted); return NULL;
    }
    return restricted;
}

static bool transfer_some(HANDLE pipe, void *data, DWORD size, bool writing, DWORD *received) {
    OVERLAPPED overlapped = {0};
    overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!overlapped.hEvent) return false;
    DWORD transferred = 0;
    BOOL ok = writing ? WriteFile(pipe, data, size, &transferred, &overlapped) :
        ReadFile(pipe, data, size, &transferred, &overlapped);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(overlapped.hEvent, 3000) != WAIT_OBJECT_0) {
            CancelIoEx(pipe, &overlapped);
            GetOverlappedResult(pipe, &overlapped, &transferred, TRUE);
            ok = FALSE;
        } else ok = GetOverlappedResult(pipe, &overlapped, &transferred, FALSE);
    }
    CloseHandle(overlapped.hEvent);
    *received = transferred;
    return ok && (!writing || transferred == size);
}

static bool transfer(HANDLE pipe, void *data, DWORD size, bool writing) {
    DWORD received = 0;
    return transfer_some(pipe,data,size,writing,&received) && received == size;
}

static bool agent_query(sb_execution *execution) {
    if (!execution->agent_started || !execution->agent_healthy ||
        WaitForSingleObject(execution->process, 0) != WAIT_TIMEOUT) return false;
    DWORD command = SB_AGENT_QUERY;
    identity reply = {0};
    bool ok = transfer(execution->pipe, &command, sizeof(command), true) &&
        transfer(execution->pipe, &reply, sizeof(reply), false) &&
        reply.magic == SB_AGENT_MAGIC && reply.version == 1 && reply.elevated &&
        !reply.restricted && !reply.appcontainer && reply.administrator_enabled &&
        reply.integrity >= SECURITY_MANDATORY_HIGH_RID &&
        reply.process_id == GetProcessId(execution->process) &&
        memchr(reply.user_sid, 0, sizeof(reply.user_sid));
    if (ok) execution->agent = reply;
    else execution->agent_healthy = false;
    return ok;
}

static bool start_agent(sb_execution *execution, sb_error *error) {
    unsigned char random[16]; char nonce[33];
    if (!sb_platform_random(random, sizeof(random), error)) return false;
    for (size_t i = 0; i < sizeof(random); ++i) snprintf(nonce + i * 2, 3, "%02x", random[i]);
    wchar_t name[128], arguments[128], image[SB_PATH_CAP];
    swprintf(name, 128, L"\\\\.\\pipe\\ShellBot-execution-%lu-%hs", GetCurrentProcessId(), nonce);
    swprintf(arguments, 128, L"--execution-agent %lu %hs", GetCurrentProcessId(), nonce);
    DWORD image_size = GetModuleFileNameW(NULL, image, SB_PATH_CAP);
    if (!image_size || image_size >= SB_PATH_CAP) { sb_fail(error, "Cannot identify the administrator agent executable."); return false; }
    wchar_t sddl[384]; PSECURITY_DESCRIPTOR descriptor = NULL;
    swprintf(sddl, 384, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;%hs)", execution->host.user_sid);
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &descriptor, NULL)) {
        sb_fail(error, "Cannot secure the administrator agent channel."); return false;
    }
    SECURITY_ATTRIBUTES attributes = {sizeof(attributes), descriptor, FALSE};
    execution->pipe = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, &attributes);
    LocalFree(descriptor);
    if (execution->pipe == INVALID_HANDLE_VALUE) { execution->pipe = NULL; sb_fail(error, "Cannot create the administrator agent channel."); return false; }
    OVERLAPPED connect = {0}; connect.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!connect.hEvent) { sb_fail(error, "Cannot allocate the administrator handshake event."); return false; }
    BOOL connected = ConnectNamedPipe(execution->pipe, &connect);
    DWORD connect_error = connected ? ERROR_SUCCESS : GetLastError();
    bool pending = !connected && connect_error == ERROR_IO_PENDING;
    if (!connected && connect_error != ERROR_IO_PENDING && connect_error != ERROR_PIPE_CONNECTED) {
        CloseHandle(connect.hEvent); sb_fail(error, "Cannot start the administrator handshake."); return false;
    }
    SHELLEXECUTEINFOW launch = {0}; launch.cbSize = sizeof(launch);
    launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    launch.lpVerb = L"runas"; launch.lpFile = image; launch.lpParameters = arguments; launch.nShow = SW_HIDE;
    HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool launched = SUCCEEDED(com) && ShellExecuteExW(&launch);
    DWORD launch_error = launched ? ERROR_SUCCESS : FAILED(com) ? ERROR_NOT_READY : GetLastError();
    if (SUCCEEDED(com)) CoUninitialize();
    execution->process = launch.hProcess;
    if (launched && execution->process) {
        HANDLE events[] = {connect.hEvent, execution->process};
        if (pending && WaitForMultipleObjects(2, events, FALSE, 25000) == WAIT_OBJECT_0) {
            DWORD transferred = 0;
            connected = GetOverlappedResult(execution->pipe, &connect, &transferred, FALSE);
            pending = false;
        } else if (!pending) connected = TRUE;
    }
    if (pending) { CancelIoEx(execution->pipe, &connect); DWORD bytes = 0; GetOverlappedResult(execution->pipe, &connect, &bytes, TRUE); }
    CloseHandle(connect.hEvent);
    if (!launched || !execution->process || !connected) {
        sb_fail(error, launch_error == ERROR_CANCELLED ? "Administrator startup authorization was cancelled." : "Administrator agent startup or handshake failed."); return false;
    }
    ULONG peer = 0;
    if (!GetNamedPipeClientProcessId(execution->pipe, &peer) || peer != GetProcessId(execution->process)) {
        sb_fail(error, "Administrator agent process identity did not match."); return false;
    }
    execution->agent_started = execution->agent_healthy = true;
    /* A pipe server can impersonate only after it has read a client message. */
    if (!agent_query(execution)) { sb_fail(error, "Administrator agent handshake reply was invalid."); return false; }
    /* SECURITY_IDENTIFICATION on the client prevents this lower-privilege
       server from borrowing the administrator's impersonation token. */
    HANDLE peer_token = NULL; identity actual = {0};
    bool verified = ImpersonateNamedPipeClient(execution->pipe) != FALSE;
    if (verified) {
        verified = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &peer_token) && read_identity(peer_token, &actual) &&
            actual.elevated && actual.administrator_enabled && actual.integrity >= SECURITY_MANDATORY_HIGH_RID &&
            !actual.restricted && !actual.appcontainer;
        if (peer_token) CloseHandle(peer_token);
        if (!RevertToSelf()) TerminateProcess(GetCurrentProcess(), 3);
    }
    if (!verified) { sb_fail(error, "Administrator agent token verification failed."); return false; }
    if (strcmp(actual.user_sid, execution->agent.user_sid)) {
        sb_fail(error, "Administrator agent identity reply did not match the operating system."); return false;
    }
    return true;
}

sb_execution *sb_execution_new(const sb_config *config, sb_error *error) {
    sb_execution *execution = calloc(1, sizeof(*execution));
    if (!execution) { sb_fail(error, "Cannot allocate execution state."); return NULL; }
    execution->profile = config->execution_profile;
    execution->approval = config->execution_approval;
    execution->configured = config->execution_enabled;
    execution->mutex = sb_platform_mutex_new();
    unsigned char nonce[16];
    if (!sb_platform_random(nonce,sizeof(nonce),error)) goto failed;
    for (size_t i=0;i<sizeof(nonce);++i) snprintf(execution->instance+i*2,3,"%02x",nonce[i]);
    HANDLE token = NULL;
    bool ok = execution->mutex && OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) && read_identity(token, &execution->host);
    if (token) CloseHandle(token);
    if (!ok) { sb_fail(error, "Cannot inspect the actual Windows execution identity."); goto failed; }
    if (execution->configured && execution->profile == SB_EXEC_USER &&
        (execution->host.elevated || execution->host.administrator_enabled || execution->host.restricted || execution->host.appcontainer)) {
        sb_fail(error, "The user profile requires an ordinary unelevated host. Restart ShellBot without administrator elevation."); goto failed;
    }
    if (execution->configured && execution->profile == SB_EXEC_CONTROLLED) {
        execution->restricted_token = prepare_restricted_token();
        execution->restricted_prepared = execution->restricted_token != NULL;
        execution->restricted_owner_rejected = execution->restricted_prepared && !sb_win_owner_token_allowed(execution->restricted_token);
    }
    if (execution->configured && execution->profile == SB_EXEC_USER) {
        execution->tasks=sb_win_tasks_new_limited(SB_EXEC_USER, &config->limits);
        if (!execution->tasks) { sb_fail(error,"Cannot initialize ordinary user task execution.");goto failed; }
    }
    if (execution->configured && execution->profile == SB_EXEC_ADMINISTRATOR && !start_agent(execution, error)) goto failed;
    return execution;
failed:
    sb_execution_free(execution); return NULL;
}

bool sb_execution_restricted_verified(sb_execution *execution) { return execution && execution->sandbox != NULL && execution->tasks != NULL; }
bool sb_execution_program_permitted(sb_execution *execution,const char *executable) {
    if(!execution || !execution->sandbox)return true;
    int n=executable?MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,executable,-1,NULL,0):0;
    wchar_t *path=n?calloc((size_t)n,sizeof(*path)):NULL;
    if(path)MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,executable,-1,path,n);
    bool allowed=path && sb_win_sandbox_program(execution->sandbox,path);free(path);return allowed;
}
void sb_execution_prepare_project(sb_execution *execution,sb_host *host) {
    if(!execution || !host->project || host->project->legacy_compatibility)return;
    execution->project=host->project;
    if(!host->project->enabled ||
        !host->project->policy.execution_enabled || host->project->policy.default_identity!=SB_ID_RESTRICTED)return;
    execution->configured=true;execution->profile=SB_EXEC_CONTROLLED;host->config.execution_profile=SB_EXEC_CONTROLLED;
    sb_error error={0};
    execution->sandbox=sb_win_sandbox_new(host,execution->instance,&error);
    if(execution->sandbox) {
        execution->tasks=sb_win_tasks_new_limited(SB_EXEC_USER,&host->config.limits);
        if(execution->tasks){sb_win_tasks_restrict(execution->tasks,execution->sandbox);execution->restricted_prepared=true;execution->restricted_owner_rejected=true;}
        else {sb_win_sandbox_free(execution->sandbox);execution->sandbox=NULL;sb_fail(&error,"Cannot initialize sandbox task receipts.");}
    }
    if(!execution->sandbox)snprintf(execution->sandbox_error,sizeof(execution->sandbox_error),"%s",*error.text?error.text:"Restricted backend initialization failed.");
}

void sb_execution_free(sb_execution *execution) {
    if (!execution) return;
    if (execution->pipe) {
        if (execution->agent_started && execution->agent_healthy) {
            DWORD command = SB_AGENT_STOP; identity ignored;
            transfer(execution->pipe, &command, sizeof(command), true);
            transfer(execution->pipe, &ignored, sizeof(ignored), false);
        }
        CloseHandle(execution->pipe);
    }
    if (execution->process) { WaitForSingleObject(execution->process, 3500); CloseHandle(execution->process); }
    sb_win_tasks_free(execution->tasks);
    sb_win_sandbox_free(execution->sandbox);
    if (execution->restricted_token) CloseHandle(execution->restricted_token);
    sb_platform_mutex_free(execution->mutex);
    free(execution);
}


const char *sb_execution_instance(sb_execution *execution) { return execution ? execution->instance : NULL; }

bool sb_execution_task_owned(sb_execution *execution, const char *actor, const char *id) {
    if (!execution) return false;
    sb_platform_mutex_lock(execution->mutex);
    bool owned = sb_win_tasks_owned(execution->tasks, actor, id);
    sb_platform_mutex_unlock(execution->mutex);
    return owned;
}

bool sb_execution_available(sb_execution *execution) {
    if (!execution) return false;
    sb_platform_mutex_lock(execution->mutex);
    bool ok=execution->configured && (((execution->profile==SB_EXEC_USER || execution->sandbox) && execution->tasks) ||
        (execution->profile==SB_EXEC_ADMINISTRATOR && execution->agent_healthy && WaitForSingleObject(execution->process,0)==WAIT_TIMEOUT));
    sb_platform_mutex_unlock(execution->mutex);return ok;
}
char *sb_execution_task(sb_execution *execution,const char *name,yyjson_val *arguments,sb_error *error) {
    if (!execution) { sb_fail(error,"Execution state unavailable.");return NULL; }
    sb_platform_mutex_lock(execution->mutex); char *reply=NULL;
    if (!execution->configured) sb_fail(error,"Command execution is disabled in local configuration.");
    else if (execution->profile==SB_EXEC_CONTROLLED && !execution->sandbox) sb_fail(error,"Controlled command execution is unavailable until its isolation and approval backend is verified.");
    else if (execution->profile==SB_EXEC_USER || execution->sandbox) reply=sb_win_tasks_call(execution->tasks,name,arguments);
    else if (!execution->agent_healthy || WaitForSingleObject(execution->process,0)!=WAIT_TIMEOUT) sb_fail(error,"Administrator agent unavailable; no automatic restart or fallback.");
    else {
        yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL);
        yyjson_mut_val *root=doc?yyjson_mut_obj(doc):NULL;
        if (root) { yyjson_mut_obj_add_str(doc,root,"name",name);yyjson_mut_obj_add_val(doc,root,"arguments",yyjson_val_mut_copy(doc,arguments));yyjson_mut_doc_set_root(doc,root); }
        size_t size=0; char *request=root?yyjson_mut_write(doc,0,&size):NULL;
        DWORD command=SB_AGENT_TASK,received=0; char *buffer=malloc(SB_AGENT_TASK_REPLY_LIMIT+1u);
        bool ok=request && size && size<=65536 && buffer && transfer(execution->pipe,&command,sizeof(command),true) &&
            transfer(execution->pipe,request,(DWORD)size,true) && transfer_some(execution->pipe,buffer,SB_AGENT_TASK_REPLY_LIMIT,false,&received) && received;
        if (ok) { buffer[received]=0;reply=buffer;buffer=NULL; }
        else if (!request || !size || size>65536 || !buffer) sb_fail(error,"Cannot encode a task request within the administrator channel budget.");
        else {
            execution->agent_healthy=false;
            CloseHandle(execution->pipe);execution->pipe=NULL;
            sb_fail(error,"Administrator task exchange failed; execution status may be uncertain. The channel is closed to stop its jobs; do not start with a new request key.");
        }
        free(buffer);free(request);yyjson_mut_doc_free(doc);
    }
    sb_platform_mutex_unlock(execution->mutex);return reply;
}

static yyjson_mut_val *identity_json(yyjson_mut_doc *doc, const identity *value) {
    yyjson_mut_val *result = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, result, "user_sid", value->user_sid);
    yyjson_mut_obj_add_uint(doc, result, "process_id", value->process_id);
    yyjson_mut_obj_add_uint(doc, result, "session_id", value->session_id);
    yyjson_mut_obj_add_uint(doc, result, "integrity_rid", value->integrity);
    yyjson_mut_obj_add_bool(doc, result, "elevated", value->elevated != 0);
    yyjson_mut_obj_add_bool(doc, result, "administrator_group_enabled", value->administrator_enabled != 0);
    yyjson_mut_obj_add_bool(doc, result, "restricted", value->restricted != 0);
    yyjson_mut_obj_add_bool(doc, result, "appcontainer", value->appcontainer != 0);
    return result;
}

yyjson_mut_val *sb_execution_status(sb_execution *execution, yyjson_mut_doc *doc) {
    yyjson_mut_val *result = yyjson_mut_obj(doc);
    if (!execution) { yyjson_mut_obj_add_str(doc, result, "error", "Execution state is unavailable."); return result; }
    sb_platform_mutex_lock(execution->mutex);
    if (execution->agent_started) execution->agent_healthy = agent_query(execution);
    yyjson_mut_obj_add_strcpy(doc,result,"host_instance_id",execution->instance);
    yyjson_mut_obj_add_str(doc, result, "profile", sb_execution_profile_name(execution->profile));
    yyjson_mut_obj_add_bool(doc, result, "configured_enabled", execution->configured);
    bool available=execution->configured && (execution->tasks || (execution->profile==SB_EXEC_ADMINISTRATOR && execution->agent_healthy));
    yyjson_mut_obj_add_bool(doc, result, "command_execution_available", available);
    yyjson_mut_obj_add_bool(doc, result, "task_tools_available", available);
    yyjson_mut_obj_add_bool(doc, result, "remote_approval_available", false);
    yyjson_mut_obj_add_bool(doc, result, "controlled_isolation_verified", execution->sandbox != NULL);
    if(*execution->sandbox_error)yyjson_mut_obj_add_strcpy(doc,result,"restricted_unavailable_reason",execution->sandbox_error);
    if(execution->sandbox) {
        yyjson_mut_obj_add_str(doc,result,"restricted_backend","Windows LPAC, exact readonly runtime resources, per-instance directory grants");
        yyjson_mut_obj_add_bool(doc,result,"network_available",false);yyjson_mut_obj_add_bool(doc,result,"desktop_available",false);
        yyjson_mut_obj_add_bool(doc,result,"child_processes_allowed",false);
    }
    yyjson_mut_obj_add_bool(doc, result, "restricted_token_prepared", execution->restricted_prepared);
    yyjson_mut_obj_add_bool(doc, result, "restricted_owner_rejected", execution->restricted_owner_rejected);
    yyjson_mut_obj_add_str(doc, result, "approval_mode", execution->project ?
        (execution->project->policy.approval_mode==SB_PROJECT_NEVER ? "never" : execution->project->policy.approval_mode==SB_PROJECT_WORKSPACE_AUTO ? "workspace_auto" : "always") : execution->profile == SB_EXEC_CONTROLLED ?
        (execution->approval == SB_APPROVAL_RULES ? "rules" : "always") : "preauthorized");
    yyjson_mut_obj_add_str(doc, result, "readiness", !execution->configured ? "disabled" : available ? "ready" : "unavailable");
    if (!available) yyjson_mut_obj_add_strcpy(doc,result,"unavailable_reason",!execution->configured ? "Command tasks are disabled in local configuration." :
        *execution->sandbox_error ? execution->sandbox_error : execution->profile==SB_EXEC_CONTROLLED ? "Complete controlled isolation and task approval are not implemented." : "Administrator agent unavailable.");
    yyjson_mut_obj_add_uint(doc,result,"max_task_records",execution->project?execution->project->limits.task_records:32);
    yyjson_mut_obj_add_uint(doc,result,"max_active_tasks",execution->project?execution->project->limits.active_tasks:8);
    yyjson_mut_obj_add_uint(doc,result,"max_output_bytes_per_stream",execution->project?execution->project->limits.output_bytes:262144);
    yyjson_mut_obj_add_val(doc, result, "host_identity", identity_json(doc, &execution->host));
    yyjson_mut_obj_add_bool(doc, result, "administrator_agent_started", execution->agent_started);
    yyjson_mut_obj_add_bool(doc, result, "administrator_agent_alive", execution->agent_healthy);
    yyjson_mut_obj_add_uint(doc, result, "administrator_agent_starts", execution->agent_started ? 1 : 0);
    if (execution->agent_started) yyjson_mut_obj_add_val(doc, result, "administrator_identity", identity_json(doc, &execution->agent));
    sb_platform_mutex_unlock(execution->mutex);
    return result;
}

int sb_win_execution_agent(int argc, wchar_t **argv) {
    if (argc != 4 || !*argv[2] || wcslen(argv[3]) != 32 || wcsspn(argv[3], L"0123456789abcdef") != 32) return 2;
    wchar_t *end = NULL;
    unsigned long parent_id = wcstoul(argv[2], &end, 10);
    if (!parent_id || *end || wcsspn(argv[2], L"0123456789") != wcslen(argv[2])) return 2;
    HANDLE token = NULL; identity self = {0};
    bool ok = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) && read_identity(token, &self);
    if (token) CloseHandle(token);
    if (!ok || !self.elevated || !self.administrator_enabled || self.restricted || self.appcontainer ||
        self.integrity < SECURITY_MANDATORY_HIGH_RID) return 2;
    HANDLE parent = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)parent_id);
    wchar_t parent_image[SB_PATH_CAP], self_image[SB_PATH_CAP]; DWORD parent_size = SB_PATH_CAP;
    ok = parent && QueryFullProcessImageNameW(parent, 0, parent_image, &parent_size) &&
        GetModuleFileNameW(NULL, self_image, SB_PATH_CAP) && !_wcsicmp(parent_image, self_image) &&
        WaitForSingleObject(parent, 0) == WAIT_TIMEOUT;
    if (!ok) { if (parent) CloseHandle(parent); return 2; }
    wchar_t name[128]; swprintf(name, 128, L"\\\\.\\pipe\\ShellBot-execution-%lu-%ls", parent_id, argv[3]);
    HANDLE pipe = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
        SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, NULL);
    ULONG server = 0; DWORD message_mode = PIPE_READMODE_MESSAGE;
    ok = pipe != INVALID_HANDLE_VALUE && GetNamedPipeServerProcessId(pipe, &server) && server == parent_id &&
        SetNamedPipeHandleState(pipe, &message_mode, NULL, NULL);
    if (!ok) { if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe); CloseHandle(parent); return 2; }
    sb_win_tasks *tasks=sb_win_tasks_new(SB_EXEC_ADMINISTRATOR);
    if (!tasks) { CloseHandle(pipe);CloseHandle(parent);return 2; }
    int result = 0;
    while (WaitForSingleObject(parent, 0) == WAIT_TIMEOUT) {
        DWORD command = 0, bytes = 0;
        if (!ReadFile(pipe, &command, sizeof(command), &bytes, NULL)) break;
        if (bytes != sizeof(command)) { result=2;break; }
        if (command==SB_AGENT_TASK) {
            char *request=malloc(65537);DWORD length=0;
            bool read=request && ReadFile(pipe,request,65536,&length,NULL) && length;
            yyjson_doc *doc=read?yyjson_read(request,length,0):NULL;free(request);
            yyjson_val *root=doc?yyjson_doc_get_root(doc):NULL;
            const char *operation=sb_string(yyjson_obj_get(root,"name"));
            char *reply=operation?sb_win_tasks_call(tasks,operation,yyjson_obj_get(root,"arguments")):NULL;
            yyjson_doc_free(doc);
            size_t reply_length=reply?strlen(reply):0;
            bool wrote=reply_length && reply_length<=SB_AGENT_TASK_REPLY_LIMIT && WriteFile(pipe,reply,(DWORD)reply_length,&bytes,NULL) && bytes==reply_length;
            free(reply);if (!wrote) { result=2;break; }continue;
        }
        if (command!=SB_AGENT_QUERY && command!=SB_AGENT_STOP) { result=2;break; }
        if (!WriteFile(pipe, &self, sizeof(self), &bytes, NULL) || bytes != sizeof(self)) break;
        if (command == SB_AGENT_STOP) break;
    }
    sb_win_tasks_free(tasks);
    CloseHandle(pipe); CloseHandle(parent); return result;
}

int sb_win_owner_boundary_probe(const char *data_dir) {
    HANDLE restricted = prepare_restricted_token();
    if (!restricted) { fputs("ShellBot: Cannot prepare the restricted owner boundary probe.\n", stderr); return 2; }
    bool token_rejected = !sb_win_owner_token_allowed(restricted);
    bool impersonated = ImpersonateLoggedOnUser(restricted) != FALSE;
    int command_result = 0;
    sb_error error = {0};
    if (impersonated) {
        command_result = sb_owner_command(data_dir, "list", NULL, &error);
        if (!RevertToSelf()) TerminateProcess(GetCurrentProcess(), 3);
    }
    CloseHandle(restricted);
    bool passed = token_rejected && impersonated && command_result != 0;
    printf("{\"restricted_token_rejected\":%s,\"restricted_owner_command_denied\":%s}\n",
        token_rejected ? "true" : "false", passed ? "true" : "false");
    return passed ? 0 : 2;
}
