#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "sb_projects.h"
#include "execution.h"
#include "shared_admin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* This dispatcher has one caller: the authenticated manager process on the
   private, single-connection elevation channel. Workers have no channel handle.
   Approvals live here and are evaluated here; no approved=true protocol exists. */
typedef struct {
    sb_project_registry registry;
    sb_host host;
    HANDLE worker;
    uint64_t authorization_version;
    bool allowed;
} admin_project;
struct sb_shared_admin { admin_project projects[SB_MAX_PROJECTS]; uint64_t sequence; bool initialized; };

static char *failure(const char *message) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL); if (!doc) return NULL;
    yyjson_mut_val *root = yyjson_mut_obj(doc); yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_strcpy(doc, root, "error", message);
    char *text = yyjson_mut_write(doc, 0, NULL); yyjson_mut_doc_free(doc); return text;
}
static void clear(admin_project *project) {
    sb_policy_free(project->host.policy); sb_execution_free(project->host.execution); sb_audit_free(project->host.audit);
    sb_store_free(project->host.store);
    sb_config_free(&project->host.config); sb_platform_lease_close(project->host.lease);
    sb_projects_free(&project->registry); if (project->worker) CloseHandle(project->worker);
    memset(project, 0, sizeof(*project));
}
sb_shared_admin *sb_shared_admin_new(void) { return calloc(1, sizeof(sb_shared_admin)); }
void sb_shared_admin_free(sb_shared_admin *state) {
    if (!state) return;
    for (size_t i = 0; i < SB_MAX_PROJECTS; ++i) clear(&state->projects[i]); free(state);
}
static bool eligible(const sb_project *p) {
    return p && p->enabled && !p->legacy_compatibility && p->policy.file_scope == SB_FILES_CURRENT_USER &&
        p->policy.default_identity == SB_ID_CURRENT_USER;
}
static admin_project *find(sb_shared_admin *state, const char *id) {
    for (size_t i = 0; id && i < SB_MAX_PROJECTS; ++i)
        if (state->projects[i].host.project && !strcmp(id, state->projects[i].host.project->id)) return &state->projects[i];
    return NULL;
}
static char *register_project(sb_shared_admin *state, yyjson_val *root) {
    static const char *const keys[] = {"command", "snapshot", "worker_pid", "host_instance_id", "authorization_version", "allowed", "data_identity"};
    const char *snapshot = sb_string(yyjson_obj_get(root, "snapshot")), *instance = sb_string(yyjson_obj_get(root, "host_instance_id"));
    const char *data_identity = sb_string(yyjson_obj_get(root, "data_identity"));
    uint64_t pid = 0, version = 0;
    if (!sb_config_keys(root, keys, 7) || !snapshot || !data_identity || !instance || strlen(instance) != 32 || strspn(instance,"0123456789abcdef") != 32 ||
        !sb_integer(yyjson_obj_get(root,"worker_pid"),&pid) || !pid || pid > MAXDWORD || !sb_integer(yyjson_obj_get(root,"authorization_version"),&version) || !version ||
        !yyjson_is_bool(yyjson_obj_get(root,"allowed"))) return failure("Invalid trusted project registration.");
    sb_error error = {0}; admin_project candidate = {0};
    if (!sb_projects_parse(&candidate.registry,snapshot,strlen(snapshot),&error) || candidate.registry.project_count != 1 ||
        !eligible(&candidate.registry.projects[0]) || find(state,candidate.registry.projects[0].id) ||
        !sb_win_shared_worker((DWORD)pid,&candidate.worker)) { clear(&candidate); return failure("Project registration or worker identity rejected."); }
    admin_project *slot = NULL;
    for (size_t i = 0; i < SB_MAX_PROJECTS; ++i) if (!state->projects[i].host.project) { slot = &state->projects[i]; break; }
    if (!slot) { clear(&candidate); return failure("Shared administrator project capacity exhausted."); }
    candidate.host.project = &candidate.registry.projects[0];
    candidate.authorization_version = version; candidate.allowed = yyjson_get_bool(yyjson_obj_get(root,"allowed"));
    candidate.registry.projects[0].policy.administrator_allowed = candidate.allowed;
    sb_config_defaults(&candidate.host.config);
    const sb_project *p = candidate.host.project;
    size_t size = strlen(p->data_dir) + 64;
    candidate.host.config.data_dir = malloc(size);
    if (candidate.host.config.data_dir) snprintf(candidate.host.config.data_dir,size,"%s\\administrator-state",p->data_dir);
    candidate.host.config.limits = p->limits; candidate.host.config.approval_ttl_seconds = p->approval_ttl_seconds;
    candidate.host.config.audit_max_bytes = p->audit_max_bytes; candidate.host.config.execution_enabled = p->policy.execution_enabled;
    candidate.host.config.execution_profile = SB_EXEC_ADMINISTRATOR; strcpy(candidate.host.config.token_env,p->token_env);
    candidate.host.started_ms = sb_platform_millis(); candidate.host.administrator_host = true; candidate.host.administrator_backend = true;
    candidate.host.lease = candidate.host.config.data_dir ? sb_platform_lease_bound(candidate.host.config.data_dir,data_identity,&error) : NULL;
    if (!candidate.host.lease) goto failed;
    for (size_t i = 0; i < p->directory_count; ++i) {
        sb_workspace *w = &candidate.host.config.workspaces[candidate.host.config.workspace_count];
        w->root = sb_platform_root(p->directories[i].path,&error); if (!w->root) goto failed;
        strcpy(w->id,p->directories[i].id); w->read_only = p->directories[i].read_only; ++candidate.host.config.workspace_count;
        for (size_t j = 0; j < i; ++j) if (sb_platform_roots_overlap(w->root,candidate.host.config.workspaces[j].root)) goto failed;
    }
    candidate.host.audit = sb_audit_new(candidate.host.lease,p->audit_max_bytes,&error); if (!candidate.host.audit) goto failed;
    candidate.host.store=sb_store_new(candidate.host.lease,p->id,p->policy_revision,instance,"administrator",&error);
    if (!candidate.host.store) goto failed;
    candidate.host.policy = sb_policy_new(p->approval_ttl_seconds,candidate.host.audit,&error); if (!candidate.host.policy) goto failed;
    sb_policy_limit(candidate.host.policy,p->limits.approval_records);
    sb_policy_store(candidate.host.policy,candidate.host.store);
    /* Move before initializing execution: it keeps a pointer to the bound project. */
    *slot = candidate; slot->host.project = &slot->registry.projects[0];
    slot->host.execution = sb_win_administrator_host(&slot->host.config,slot->host.project,instance,&error);
    if (!slot->host.execution || !sb_win_execution_watch_worker(slot->host.execution,slot->worker)) { clear(slot); return failure(*error.text ? error.text : "Cannot bind administrator jobs to the worker lifetime."); }
    sb_win_execution_store(slot->host.execution,slot->host.store,slot->host.audit);
    if (!sb_audit_project(slot->host.audit,"administrator_project_registered",p->id,p->policy_revision,pid)) { clear(slot); return failure("Administrator registration audit unavailable."); }
    return sb_strdup("{\"registered\":true}");
failed:
    clear(&candidate); return failure(*error.text ? error.text : "Administrator project initialization failed.");
}
char *sb_shared_admin_request(sb_shared_admin *state, yyjson_val *root) {
    if (!state) return failure("Shared administrator state unavailable.");
    const char *command = sb_string(yyjson_obj_get(root,"command"));
    if (command && !strcmp(command,"initialize")) {
        static const char *const init_keys[]={"command","token_envs"}; yyjson_val *envs=yyjson_obj_get(root,"token_envs");
        if (state->initialized || !sb_config_keys(root,init_keys,2) || !yyjson_is_arr(envs) || yyjson_arr_size(envs)>SB_MAX_PROJECTS+1)
            return failure("Shared administrator initialization rejected.");
        size_t i,n; yyjson_val *value;
        yyjson_arr_foreach(envs,i,n,value) {
            const char *key=sb_string(value); if (!sb_identifier(key)) return failure("Invalid credential environment key.");
            wchar_t wide_key[65]; if (!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,key,-1,wide_key,65) ||
                (!SetEnvironmentVariableW(wide_key,NULL) && GetLastError()!=ERROR_ENVVAR_NOT_FOUND))
                return failure("Cannot remove project credentials from the administrator environment.");
        }
        state->initialized=true; return sb_strdup("{\"initialized\":true}");
    }
    if (!state->initialized) return failure("Trusted administrator initialization is required.");
    if (command && !strcmp(command,"register")) return register_project(state,root);
    static const char *const keys[] = {"command", "project_id", "policy_revision", "authorization_version", "host_instance_id", "sequence", "deadline_ms", "operation_sha256", "actor", "operation"};
    const char *id = sb_string(yyjson_obj_get(root,"project_id")), *instance = sb_string(yyjson_obj_get(root,"host_instance_id"));
    const char *actor = sb_string(yyjson_obj_get(root,"actor")), *digest = sb_string(yyjson_obj_get(root,"operation_sha256"));
    uint64_t revision=0, version=0, sequence=0, deadline=0; sb_error error={0};
    yyjson_val *operation=yyjson_obj_get(root,"operation");
    char *serialized=yyjson_val_write(operation,0,NULL); char actual[65]={0};
    bool valid=command && id && instance && actor && (!strcmp(actor,"manager-owner") || !strcmp(actor,"project-mcp")) && digest && strlen(digest)==64 &&
        sb_config_keys(root,keys,10) && yyjson_is_obj(operation) && sb_integer(yyjson_obj_get(root,"policy_revision"),&revision) &&
        sb_integer(yyjson_obj_get(root,"authorization_version"),&version) && sb_integer(yyjson_obj_get(root,"sequence"),&sequence) && sequence > state->sequence &&
        sb_integer(yyjson_obj_get(root,"deadline_ms"),&deadline) && deadline >= sb_platform_millis() && deadline - sb_platform_millis() <= 8000 &&
        serialized && sb_platform_sha256(serialized,strlen(serialized),actual,&error) && !strcmp(actual,digest);
    free(serialized);
    if (!valid) return failure("Administrator operation binding, digest, sequence or deadline rejected.");
    state->sequence=sequence;
    admin_project *project=find(state,id);
    if (!project || revision != project->host.project->policy_revision || version != project->authorization_version ||
        strcmp(instance,sb_execution_instance(project->host.execution))) return failure("Administrator project authorization or worker instance changed.");
    if (!strcmp(command,"detach")) { clear(project); return sb_strdup("{\"detached\":true}"); }
    if (WaitForSingleObject(project->worker,0) != WAIT_TIMEOUT) { clear(project); return failure("Registered worker exited; administrator tasks cancelled."); }
    if (!strcmp(command,"authorization")) {
        static const char *const auth_keys[]={"allowed", "new_version"}; uint64_t next=0;
        if (strcmp(actor,"manager-owner") || !sb_config_keys(operation,auth_keys,2) || !yyjson_is_bool(yyjson_obj_get(operation,"allowed")) ||
            !sb_integer(yyjson_obj_get(operation,"new_version"),&next) || next != version+1) return failure("Owner authorization update rejected.");
        bool allowed=yyjson_get_bool(yyjson_obj_get(operation,"allowed"));
        project->allowed=false; project->registry.projects[0].policy.administrator_allowed=false;
        if (!sb_win_execution_cancel_all(project->host.execution)) return failure("Administrator tasks could not all be cancelled; permission remains revoked.");
        sb_policy_free(project->host.policy); project->host.policy=sb_policy_new(project->host.config.approval_ttl_seconds,project->host.audit,&error);
        if (!project->host.policy) return failure("Administrator approvals unavailable; permission remains revoked.");
        sb_policy_limit(project->host.policy,project->host.config.limits.approval_records);
        sb_policy_store(project->host.policy,project->host.store);
        if (!sb_audit_project(project->host.audit,allowed?"administrator_permission_granted":"administrator_permission_revoked",id,revision,GetProcessId(project->worker)))
            return failure("Administrator authorization audit unavailable; permission remains revoked.");
        project->authorization_version=next; project->allowed=allowed; project->registry.projects[0].policy.administrator_allowed=allowed;
        return sb_strdup("{\"authorization_updated\":true,\"old_approvals_invalidated\":true,\"active_tasks_cancelled\":true}");
    }
    if (!strcmp(command,"owner") || !strcmp(command,"remote-owner")) {
        if (strcmp(actor,"manager-owner")) return failure("Administrator approval management requires the owner channel.");
        char *wire=yyjson_val_write(operation,0,NULL); char *reply=wire ? sb_policy_owner_request_actor(project->host.policy,wire,strlen(wire),!strcmp(command,"remote-owner") ? "remote-owner" : "local-owner") : NULL;
        free(wire); return reply;
    }
    if (strcmp(command,"call")) return failure("Unknown shared administrator command.");
    static const char *const call_keys[]={"name", "arguments"};
    const char *name=sb_string(yyjson_obj_get(operation,"name")); yyjson_val *args=yyjson_obj_get(operation,"arguments");
    const char *identity=sb_string(yyjson_obj_get(args,"identity"));
    if (!sb_config_keys(operation,call_keys,2) || !name || !yyjson_is_obj(args) || !identity || strcmp(identity,"administrator"))
        return failure("Explicit administrator identity is required.");
    bool result_tool=!strcmp(name,"task_status") || !strcmp(name,"task_output") || !strcmp(name,"task_cancel") || !strcmp(name,"approval_status") || !strcmp(name,"approval_cancel");
    if (!project->allowed && !result_tool) return failure("administrator_not_allowed");
    yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL); if (!doc) return NULL;
    yyjson_mut_val *reply=sb_tool_call(&project->host,actor,name,args,doc); yyjson_mut_doc_set_root(doc,reply);
    yyjson_mut_val *metadata=yyjson_mut_obj_get(reply,"structuredContent");
    if (metadata) {
        yyjson_mut_obj_add_str(doc,metadata,"execution_identity","administrator"); yyjson_mut_obj_add_uint(doc,metadata,"authorization_version",version);
        yyjson_mut_obj_add_uint(doc,metadata,"administrator_agent_pid",GetCurrentProcessId());
        yyjson_mut_val *content=yyjson_mut_arr_get(yyjson_mut_obj_get(reply,"content"),0);
        char *description=yyjson_mut_val_write(metadata,0,NULL);
        if (content && description) { yyjson_mut_obj_remove_str(content,"text"); yyjson_mut_obj_add_strcpy(doc,content,"text",description); }
        free(description);
    }
    char *text=yyjson_mut_write(doc,0,NULL); yyjson_mut_doc_free(doc); return text;
}
