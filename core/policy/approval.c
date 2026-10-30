#include "sb.h"
#include "sb_projects.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RECORD_LIMIT 64
typedef enum { PENDING, APPROVED, RUNNING, COMPLETED, DENIED, EXPIRED, FAILED, CANCELLED, INTERRUPTED } state;
typedef struct {
    char id[33], digest[65], hash[65], actor[64], workspace[64];
    char *target, *identity, *text, *original;
    char tool[32], operation[16], original_hash[65], file_identity[96], backup_id[33];
    size_t bytes;
    uint64_t deadline;
    state status, previous_status;
    bool audit_warning;
    char error[256];
    char project[64], instance[33], version[65];
    char *cached;
    uint64_t revision, next_offset, last_offset, budget, used, charge;
    bool logical, pageable;
} record;
struct sb_policy { sb_mutex *mutex; sb_audit *audit; sb_store *store; unsigned ttl, record_limit; record records[RECORD_LIMIT]; };

static const char *state_name(state status) {
    static const char *const names[] = {"pending", "approved", "running", "completed", "denied", "expired", "failed", "cancelled", "interrupted"};
    return names[status];
}
static void clear(record *r) { free(r->target); free(r->identity); free(r->text); free(r->original); free(r->cached); memset(r, 0, sizeof(*r)); }
static yyjson_mut_val *capacity(sb_policy *policy,yyjson_mut_doc *doc) {
    yyjson_mut_val *value=yyjson_mut_obj(doc);yyjson_mut_obj_add_bool(doc,value,"enabled",policy!=NULL);if(!policy)return value;
    unsigned count=0,running=0,task_receipts=0;
    for(size_t i=0;i<policy->record_limit;++i){record *r=&policy->records[i];if(!*r->id)continue;++count;
        if(r->status==RUNNING)++running;
        if(r->logical && (!strcmp(r->tool,"task_start") || !strcmp(r->tool,"task_input")) && (r->status==COMPLETED || r->status==FAILED))++task_receipts;
    }
    yyjson_mut_obj_add_uint(doc,value,"record_count",count);yyjson_mut_obj_add_uint(doc,value,"record_limit",policy->record_limit);
    yyjson_mut_obj_add_uint(doc,value,"running_count",running);yyjson_mut_obj_add_uint(doc,value,"cached_task_receipt_count",task_receipts);return value;
}
yyjson_mut_val *sb_policy_capacity(sb_policy *policy,yyjson_mut_doc *doc) {
    if(!policy)return capacity(NULL,doc);sb_platform_mutex_lock(policy->mutex);yyjson_mut_val *value=capacity(policy,doc);sb_platform_mutex_unlock(policy->mutex);return value;
}
unsigned sb_policy_task_archive(sb_policy *policy,const char *actor,const char *task_id) {
    if(!policy || !actor || !task_id)return 0;unsigned released=0;sb_platform_mutex_lock(policy->mutex);
    for(size_t i=0;i<policy->record_limit;++i){record *r=&policy->records[i];
        if(!r->logical || strcmp(r->actor,actor) || (r->status!=COMPLETED && r->status!=FAILED))continue;
        bool start=!strcmp(r->tool,"task_start"),input=!strcmp(r->tool,"task_input");if(!start && !input)continue;
        yyjson_doc *saved=r->cached?yyjson_read(r->cached,strlen(r->cached),0):NULL;
        const char *bound=sb_string(yyjson_obj_get(yyjson_obj_get(yyjson_doc_get_root(saved),"structuredContent"),"task_id"));
        bool match=bound && !strcmp(bound,task_id);yyjson_doc_free(saved);
        if(!match && input){yyjson_doc *args=r->text?yyjson_read(r->text,strlen(r->text),0):NULL;bound=sb_string(yyjson_obj_get(yyjson_doc_get_root(args),"task_id"));match=bound && !strcmp(bound,task_id);yyjson_doc_free(args);}
        /* 后端先持久保存精简回执与请求键，再清理本人已结束的操作回执。审批中的请求与其他任务保持不变。 */
        if(match){clear(r);++released;}
    }
    sb_platform_mutex_unlock(policy->mutex);return released;
}
static yyjson_mut_val *describe(yyjson_mut_doc *doc, record *r, bool full);
#ifndef SB_POSIX
/* 大文件审批仍在内存中保留完整内容；持久历史只存预览和摘要，避免复制整份源码/图像耗尽日志。 */
static void history_preview(yyjson_mut_doc *doc, yyjson_mut_val *body, const char *field) {
    const char *text=yyjson_mut_get_str(yyjson_mut_obj_get(body,field));
    if (!text || strlen(text)<=SB_TEXT_LIMIT) return;
    size_t size=strlen(text),count=SB_TEXT_LIMIT;
    while (count && ((unsigned char)text[count]&0xc0)==0x80) --count;
    char *preview=malloc(count+1);
    if (!preview) { yyjson_mut_obj_remove_str(body,field); return; }
    memcpy(preview,text,count); preview[count]=0;
    yyjson_mut_obj_remove_str(body,field); yyjson_mut_obj_add_strcpy(doc,body,field,preview); free(preview);
    char key[64]; snprintf(key,sizeof(key),"%s_truncated",field);
    yyjson_mut_obj_add(body,yyjson_mut_strcpy(doc,key),yyjson_mut_bool(doc,true));
    snprintf(key,sizeof(key),"%s_original_bytes",field);
    yyjson_mut_obj_add(body,yyjson_mut_strcpy(doc,key),yyjson_mut_uint(doc,size));
}
#endif
static bool audit_record(sb_policy *policy, record *r, const char *event, const char *actor) {
    const char *status=state_name(r->status);
    if (!strcmp(event,"approval_approved")) status="approved";
    else if (!strcmp(event,"operation_started") || !strcmp(event,"write_started") || !strcmp(event,"modify_started")) status="running";
    yyjson_mut_doc *doc=policy->store ? yyjson_mut_doc_new(NULL) : NULL;
    bool desktop=sb_desktop_tool(r->tool);
    yyjson_mut_val *body=doc ? describe(doc,r,!desktop) : NULL;
#ifndef SB_POSIX
    if (body) { history_preview(doc,body,"text"); history_preview(doc,body,"original_text"); }
#endif
    if (body && r->cached) {
        yyjson_doc *receipt=yyjson_read(r->cached,strlen(r->cached),0);
        if (receipt) {
            yyjson_mut_val *saved=yyjson_val_mut_copy(doc,yyjson_doc_get_root(receipt));
            /* Desktop payloads and approval arguments remain in memory only.
               Recovery retains operation metadata without pixels, titles or text. */
            if (desktop) {
                yyjson_mut_obj_remove_str(saved,"content");
                yyjson_mut_val *details=yyjson_mut_obj_get(saved,"structuredContent");
                if (details) { yyjson_mut_obj_remove_str(details,"text"); yyjson_mut_obj_remove_str(details,"windows"); }
            }
#ifndef SB_POSIX
            /* 完整响应仅供当前实例的幂等重试，历史不复制 Base64 图像或产物块。 */
            yyjson_mut_obj_remove_str(saved,"content");
            yyjson_mut_val *details=yyjson_mut_obj_get(saved,"structuredContent");
            if (details) {
                yyjson_mut_obj_remove_str(details,"data_base64");
                history_preview(doc,details,"text"); history_preview(doc,details,"text_utf8");
            }
#endif
            yyjson_mut_obj_add_val(doc,body,"receipt",saved);
        }
        yyjson_doc_free(receipt);
    }
    bool ok=(!policy->store || (body && sb_store_append(policy->store,"approval",r->id,r->actor,status,event,body))) &&
        sb_audit_append(policy->audit, event, actor, r->id, r->workspace, r->target, r->hash, r->bytes,
        *r->tool ? r->tool : NULL, *r->operation ? r->operation : NULL,
        *r->original_hash ? r->original_hash : NULL, *r->backup_id ? r->backup_id : NULL);
    yyjson_mut_doc_free(doc);
    if (!ok) r->audit_warning = true;
    return ok;
}
static bool logical_read(const record *r) {
    return r->logical && ((!strncmp(r->tool,"fs_",3) && strcmp(r->tool,"fs_write") && strcmp(r->tool,"fs_patch") &&
        (!sb_file_manage_tool(r->tool) || !strcmp(r->tool,"fs_stat"))) || !strcmp(r->tool,"image_view") || sb_document_tool(r->tool) || sb_transfer_tool(r->tool) ||
        !strcmp(r->tool,"desktop_capture") || !strcmp(r->tool,"desktop_state") || !strcmp(r->tool,"window_list") || !strcmp(r->tool,"clipboard_read"));
}
static void expire(sb_policy *policy, record *r) {
    if ((r->status == PENDING || r->status == APPROVED || (r->status==COMPLETED && logical_read(r))) && sb_platform_millis() >= r->deadline) {
        r->status = EXPIRED;
        audit_record(policy, r, "approval_expired", "host");
    }
}
static record *find(sb_policy *policy, const char *id) {
    for (size_t i = 0; i < policy->record_limit; ++i) if (*policy->records[i].id && !strcmp(policy->records[i].id, id)) return &policy->records[i];
    return NULL;
}
bool sb_policy_transfer_valid(sb_policy *policy,const char *ticket,const char *actor) {
    if(!policy || !ticket || !*ticket)return false;
    sb_platform_mutex_lock(policy->mutex);record *r=find(policy,ticket);
    if(r)expire(policy,r);
    bool ok=r && r->logical && sb_transfer_tool(r->tool) && !strcmp(r->actor,actor) && r->status==COMPLETED && sb_platform_millis()<r->deadline;
    sb_platform_mutex_unlock(policy->mutex);return ok;
}
static yyjson_mut_val *error_data(yyjson_mut_doc *doc, const char *message) {
    yyjson_mut_val *data = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, data, "error", message);
    return data;
}
static yyjson_mut_val *describe(yyjson_mut_doc *doc, record *r, bool full) {
    yyjson_mut_val *data = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, data, "approval_id", r->id);
    yyjson_mut_obj_add_str(doc, data, "status", state_name(r->status));
    if (r->status==INTERRUPTED) yyjson_mut_obj_add_str(doc,data,"previous_status",state_name(r->previous_status));
    yyjson_mut_obj_add_strcpy(doc, data, "actor", r->actor);
    yyjson_mut_obj_add_strcpy(doc, data, "workspace_id", r->workspace);
    yyjson_mut_obj_add_strcpy(doc, data, "target", r->target);
    yyjson_mut_obj_add_strcpy(doc, data, r->logical ? "operation_sha256" : "sha256", r->hash);
    yyjson_mut_obj_add_uint(doc, data, "bytes", r->bytes);
    if (r->logical) {
        yyjson_mut_obj_add_strcpy(doc, data, "project_id", r->project);
        yyjson_mut_obj_add_uint(doc, data, "policy_revision", r->revision);
        yyjson_mut_obj_add_strcpy(doc, data, "host_instance_id", r->instance);
        yyjson_mut_obj_add_strcpy(doc, data, "target_version", r->version);
        yyjson_mut_obj_add_uint(doc, data, "maximum_units", r->budget);
        yyjson_mut_obj_add_uint(doc, data, "consumed_units", r->used);
    }
    if (*r->tool) {
        yyjson_mut_obj_add_strcpy(doc, data, "tool", r->tool);
        yyjson_mut_obj_add_strcpy(doc, data, "operation", r->operation);
        if(!r->logical){yyjson_mut_obj_add_strcpy(doc, data, "original_sha256", r->original_hash);yyjson_mut_obj_add_bool(doc, data, "atomic_save", false);}
        if (*r->backup_id) yyjson_mut_obj_add_strcpy(doc, data, "backup_id", r->backup_id);
    }
    uint64_t now = sb_platform_millis();
    yyjson_mut_obj_add_uint(doc, data, "expires_in_ms", r->deadline > now ? r->deadline - now : 0);
    if (full) {
        yyjson_mut_obj_add_strcpy(doc, data, "parent_identity", r->identity);
        yyjson_mut_obj_add_strcpy(doc, data, "text", r->text);
        if (r->original) {
            yyjson_mut_obj_add_strcpy(doc, data, "original_text", r->original);
            yyjson_mut_obj_add_strcpy(doc, data, "file_identity", r->file_identity);
        }
#ifndef SB_POSIX
        history_preview(doc,data,"text"); history_preview(doc,data,"original_text");
        if (yyjson_mut_obj_get(data,"text_truncated") || yyjson_mut_obj_get(data,"original_text_truncated"))
            yyjson_mut_obj_add_str(doc,data,"preview_notice","正文较长，仅显示前 64 KiB；请结合完整长度、摘要和请求来源核对，预览不是完整文件备份。");
#endif
    }
    if (*r->error) yyjson_mut_obj_add_strcpy(doc, data, "error", r->error);
    if (r->audit_warning) yyjson_mut_obj_add_str(doc, data, "audit_warning", "Audit recording is incomplete; the reported operation status remains authoritative.");
    return data;
}
sb_policy *sb_policy_new(unsigned ttl, sb_audit *audit, sb_error *error) {
    sb_policy *policy = calloc(1, sizeof(*policy));
    if (policy) policy->mutex = sb_platform_mutex_new();
    if (!policy || !policy->mutex) { free(policy); sb_fail(error, "Cannot allocate approval policy."); return NULL; }
    policy->ttl = ttl; policy->audit = audit; policy->record_limit = RECORD_LIMIT;
    return policy;
}
void sb_policy_limit(sb_policy *policy, unsigned records) {
    if (policy && records && records <= RECORD_LIMIT) policy->record_limit = records;
}
void sb_policy_store(sb_policy *policy, sb_store *store) { if (policy) policy->store=store; }
void sb_policy_free(sb_policy *policy) {
    if (policy) {
        for (size_t i = 0; i < policy->record_limit; ++i) {
            record *r=&policy->records[i];
            if (policy->store && *r->id && (r->status==PENDING || r->status==APPROVED || r->status==RUNNING)) {
                r->previous_status=r->status; r->status=INTERRUPTED; audit_record(policy,r,"approval_interrupted","host");
            }
            clear(r);
        }
        sb_platform_mutex_free(policy->mutex); free(policy);
    }
}

bool sb_policy_operation_begin(sb_host *host, const char *actor, const char *tool,
    const char *parameters, const char *target, const char *version, const char *approval_id,
    bool approval_required, uint64_t offset, uint64_t budget, yyjson_mut_doc *doc,
    yyjson_mut_val **reply, char ticket[33]) {
    sb_policy *policy = host->policy; record *r = NULL; sb_error error = {0};
    *reply = NULL; *ticket = 0;
    char digest[65];
    if (!tool || strlen(tool)>=sizeof(((record*)0)->tool) || !parameters || strlen(parameters) > SB_REQUEST_LIMIT || !actor || strlen(actor) >= 64 ||
        !sb_platform_sha256(parameters, strlen(parameters), digest, &error)) {
        *reply = error_data(doc, "Logical operation arguments exceed the approval budget."); return false;
    }
    sb_platform_mutex_lock(policy->mutex);
    if (approval_id) {
        r = find(policy, approval_id);
        if (!r || !r->logical || strcmp(r->actor, actor) || strcmp(r->project, host->project->id) ||
            r->revision != host->project->policy_revision || strcmp(r->instance, sb_execution_instance(host->execution)) ||
            strcmp(r->tool, tool) || strcmp(r->digest, digest) || strcmp(r->target, target)) {
            *reply = error_data(doc, "Approval does not match project, caller, instance or operation arguments."); goto blocked;
        }
    } else {
        if (sb_desktop_tool(tool) || sb_file_manage_tool(tool)) {
            yyjson_doc *input=yyjson_read(parameters,strlen(parameters),0);
            const char *request_key=sb_string(yyjson_obj_get(yyjson_doc_get_root(input),"request_key"));
            bool conflict=false;
            for (size_t i=0;request_key && i<policy->record_limit;++i) {
                record *candidate=&policy->records[i];
                if (!candidate->logical || (sb_desktop_tool(tool) ? !sb_desktop_tool(candidate->tool) : !sb_file_manage_tool(candidate->tool)) || strcmp(candidate->actor,actor)) continue;
                yyjson_doc *prior=yyjson_read(candidate->text,strlen(candidate->text),0);
                const char *old_key=sb_string(yyjson_obj_get(yyjson_doc_get_root(prior),"request_key"));
                bool same=old_key && !strcmp(old_key,request_key); yyjson_doc_free(prior);
                if (!same) continue;
                conflict=strcmp(candidate->tool,tool) || strcmp(candidate->digest,digest) || strcmp(candidate->target,target);
                if (!conflict) r=candidate;
                break;
            }
            yyjson_doc_free(input);
            if (conflict) { *reply=error_data(doc,"request_key already belongs to different arguments, tool or target."); goto blocked; }
        }
        for (size_t i = 0; i < policy->record_limit; ++i) {
            record *candidate = &policy->records[i];
            if (candidate->logical && candidate->status!=DENIED && candidate->status!=EXPIRED && candidate->status!=CANCELLED && candidate->status!=INTERRUPTED && !strcmp(candidate->actor, actor) && !strcmp(candidate->tool, tool) &&
                !strcmp(candidate->digest, digest) && !strcmp(candidate->target, target) &&
                (!strcmp(candidate->version, version) || ((candidate->status==COMPLETED || candidate->status==FAILED) &&
                    (!strcmp(tool,"fs_write") || !strcmp(tool,"fs_patch") || !strcmp(tool,"task_start"))))) {
                r = candidate; break;
            }
        }
    }
    if (r) {
        expire(policy, r);
        bool reading=logical_read(r);
        if(reading && sb_platform_millis()>=r->deadline){r->status=EXPIRED;*reply=describe(doc,r,false);goto blocked;}
        if (reading && strcmp(r->version, version)) {
            r->status = DENIED; audit_record(policy,r,"approval_target_changed",actor);
            *reply = error_data(doc,"Approval target object or version changed; request a new approval."); goto blocked;
        }
        if ((r->status == COMPLETED || r->status == FAILED || r->status == APPROVED) && r->cached &&
            (!r->pageable || offset == r->last_offset)) {
            yyjson_doc *cached = yyjson_read(r->cached, strlen(r->cached), 0);
            *reply = cached ? yyjson_val_mut_copy(doc, yyjson_doc_get_root(cached)) : error_data(doc, "Cached receipt is unavailable.");
            if(cached && !strcmp(tool,"task_start")) {
                yyjson_mut_val *data=yyjson_mut_obj_get(*reply,"structuredContent");
                yyjson_mut_obj_remove_str(data,"created_new");yyjson_mut_obj_add_bool(doc,data,"created_new",false);
                yyjson_mut_obj_add_bool(doc,data,"receipt_replayed",true);
            }
            yyjson_doc_free(cached); goto blocked;
        }
        if(reading && !r->pageable && r->status==COMPLETED && !r->cached) {
            /* Large immutable image receipts are reproduced inside the original permit. */
            if(!audit_record(policy,r,"operation_replayed",actor)){*reply=error_data(doc,"Audit unavailable; operation was not started.");goto blocked;}
            r->status=RUNNING;r->charge=0;strcpy(ticket,r->id);
            sb_platform_mutex_unlock(policy->mutex);return true;
        }
        if (r->status != APPROVED || sb_platform_millis() >= r->deadline) { *reply = describe(doc, r, false); goto blocked; }
        if (strcmp(r->version, version)) { r->status = DENIED; audit_record(policy,r,"approval_target_changed",actor); *reply = error_data(doc, "Approval target object or version changed; request a new approval."); goto blocked; }
        if (offset != r->next_offset || budget > r->budget - r->used) {
            *reply = error_data(doc, "Approval page must continue at next_offset within the logical budget."); goto blocked;
        }
    } else {
        if (!sb_audit_healthy(policy->audit)) { *reply = error_data(doc, "Audit unavailable; operation was not started."); goto blocked; }
        for (size_t i = 0; i < policy->record_limit; ++i) if (!*policy->records[i].id) { r = &policy->records[i]; break; }
        if (!r) {
            *reply=error_data(doc,"Operation record budget reached; archive ended tasks to release their task receipts. Other approvals and operation receipts remain reserved for review.");
            yyjson_mut_obj_add_str(doc,*reply,"code","operation_record_budget_exceeded");yyjson_mut_obj_add_val(doc,*reply,"operation_budget",capacity(policy,doc));goto blocked;
        }
        unsigned char nonce[16];
        r->target = sb_strdup(target); r->identity = sb_strdup(version); r->text = sb_strdup(parameters);
        if (!r->target || !r->identity || !r->text || !sb_platform_random(nonce, sizeof(nonce), &error)) {
            clear(r); *reply = error_data(doc, "Cannot allocate logical approval."); goto blocked;
        }
        for (size_t i = 0; i < sizeof(nonce); ++i) snprintf(r->id + i * 2, 3, "%02x", nonce[i]);
        strcpy(r->actor, actor); strcpy(r->project, host->project->id); strcpy(r->instance, sb_execution_instance(host->execution));
        strcpy(r->tool, tool); strcpy(r->operation, "logical"); strcpy(r->digest, digest); strcpy(r->hash, digest); strcpy(r->version, version);
        yyjson_doc *arguments=yyjson_read(parameters,strlen(parameters),0);
        const char *workspace=sb_string(yyjson_obj_get(yyjson_doc_get_root(arguments),"workspace_id"));
        snprintf(r->workspace,sizeof(r->workspace),"%s",workspace?workspace:!strncmp(tool,"task_",5)?"task":"current-user");yyjson_doc_free(arguments);
        r->revision = host->project->policy_revision; r->logical = true;
        r->pageable = !strcmp(tool,"fs_read") || !strcmp(tool,"fs_list") || !strcmp(tool,"document_read");
        r->budget = (!strcmp(tool,"fs_read") || !strcmp(tool,"document_read")) ? SB_READ_PERMIT_LIMIT : !strcmp(tool,"fs_list") ? SB_LIST_PERMIT_LIMIT : budget;
        r->next_offset = offset; r->deadline = sb_platform_millis() + (uint64_t)policy->ttl * 1000;
        r->status = approval_required ? PENDING : APPROVED;
        if (!audit_record(policy,r,approval_required ? "approval_requested" : "operation_authorized",actor)) {
            clear(r); *reply = error_data(doc, "Cannot record operation authorization."); goto blocked;
        }
        if (approval_required) { *reply = describe(doc, r, false); yyjson_mut_obj_add_str(doc,*reply,"action","Inspect and approve with the local project owner, then retry with approval_id."); goto blocked; }
    }
    if (!audit_record(policy,r,"operation_started",actor)) { *reply = error_data(doc,"Audit unavailable; operation was not started."); goto blocked; }
    r->status = RUNNING; r->last_offset = offset; r->charge = budget; strcpy(ticket,r->id);
    sb_platform_mutex_unlock(policy->mutex); return true;
blocked:
    sb_platform_mutex_unlock(policy->mutex); return false;
}

yyjson_mut_val *sb_policy_operation_receipt(sb_host *host, const char *actor, const char *tool,
    const char *parameters, const char *target, const char *approval_id, yyjson_mut_doc *doc) {
    char digest[65]; sb_error error={0}; yyjson_mut_val *result=NULL;
    if (!parameters || !sb_platform_sha256(parameters,strlen(parameters),digest,&error)) return NULL;
    sb_policy *policy=host->policy; sb_platform_mutex_lock(policy->mutex);
    for (size_t i=0;i<policy->record_limit;++i) {
        record *r=&policy->records[i];
        if (!r->logical || !r->cached || (r->status!=COMPLETED && r->status!=FAILED) ||
            (approval_id && strcmp(r->id,approval_id)) || strcmp(r->actor,actor) || strcmp(r->tool,tool) ||
            strcmp(r->digest,digest) || strcmp(r->target,target) || strcmp(r->project,host->project->id) ||
            r->revision!=host->project->policy_revision || strcmp(r->instance,sb_execution_instance(host->execution))) continue;
        yyjson_doc *cached=yyjson_read(r->cached,strlen(r->cached),0);
        if (cached) result=yyjson_val_mut_copy(doc,yyjson_doc_get_root(cached));
        yyjson_doc_free(cached); break;
    }
    sb_platform_mutex_unlock(policy->mutex); return result;
}

bool sb_policy_operation_finish(sb_policy *policy, const char *ticket, yyjson_mut_val *result) {
    bool recorded=false;
    sb_platform_mutex_lock(policy->mutex);
    record *r = find(policy, ticket);
    if (r && r->logical && r->status == RUNNING) {
        bool failed = !result || yyjson_mut_get_bool(yyjson_mut_obj_get(result,"isError"));
        yyjson_mut_val *data = yyjson_mut_obj_get(result,"structuredContent");
        const char *backup_id=yyjson_mut_get_str(yyjson_mut_obj_get(data,"backup_id"));
        if (backup_id && strlen(backup_id)==32 && strspn(backup_id,"0123456789abcdef")==32) strcpy(r->backup_id,backup_id);
        uint64_t next = yyjson_mut_get_uint(yyjson_mut_obj_get(data,"next_offset"));
        bool more = yyjson_mut_get_bool(yyjson_mut_obj_get(data,"truncated")) || yyjson_mut_get_bool(yyjson_mut_obj_get(data,"has_more"));
        if (r->pageable && !failed && next >= r->last_offset) { r->used += next - r->last_offset; r->next_offset = next; }
        else if (!failed) r->used += r->charge;
        r->status = failed ? FAILED : r->pageable && more && r->used < r->budget ? APPROVED : COMPLETED;
        size_t size = 0; char *cached = result ? yyjson_mut_val_write(result,0,&size) : NULL;
        free(r->cached); r->cached = size <= (!strcmp(r->tool,"desktop_capture") ? SB_RESPONSE_LIMIT : SB_POLICY_CACHE_LIMIT) ? cached : NULL;
        if (!r->cached) free(cached);
        recorded=audit_record(policy,r,failed ? "operation_failed" : "operation_completed",r->actor);
    }
    sb_platform_mutex_unlock(policy->mutex);
    return recorded;
}

yyjson_mut_val *sb_policy_write_authorized(sb_host *host, const char *actor, sb_workspace *workspace,
    const char *path, const char *text, yyjson_mut_doc *doc, bool *failed) {
    *failed = true; sb_error error = {0}; char hash[65];
    size_t size = strlen(text); char *decoded = NULL; size_t length, consumed; const char *encoding;
    if (workspace->read_only || size > SB_EDIT_LIMIT || !sb_decode((const unsigned char *)text,size,true,"utf-8",true,&decoded,&length,&consumed,&encoding,&error)) {
        free(decoded); return error_data(doc, workspace->read_only ? "Workspace is read-only." : *error.text ? error.text : "Text exceeds the edit byte limit reported by host_capabilities.");
    }
    free(decoded);
    sb_create *plan = sb_platform_prepare_create(workspace->root,path,&error);
    char actual_version[65];bool same=plan && host->approved_target_version && sb_platform_create_version(plan,actual_version,&error) && !strcmp(actual_version,host->approved_target_version);
    if(!same)sb_fail(&error,"Physical parent changed before file creation.");
    bool ok = same && sb_platform_sha256(text,size,hash,&error) &&
        sb_audit_append(host->audit,"write_started",actor,NULL,workspace->id,sb_platform_create_target(plan),hash,size,"fs_write","create",NULL,NULL);
    if (ok) ok = sb_platform_create_text(plan,text,size,&error);
    yyjson_mut_val *data = ok ? yyjson_mut_obj(doc) : error_data(doc,*error.text ? error.text : "Audit unavailable; creation was not started.");
    if (ok) { yyjson_mut_obj_add_str(doc,data,"status","completed"); yyjson_mut_obj_add_strcpy(doc,data,"sha256",hash); yyjson_mut_obj_add_uint(doc,data,"bytes",size); }
    sb_platform_create_close(plan); *failed = !ok; return data;
}

yyjson_mut_val *sb_policy_write(sb_host *host, const char *actor, sb_workspace *workspace,
                               const char *path, const char *text, const char *approval_id,
                               yyjson_mut_doc *doc, bool *failed) {
    *failed = true;
    if (workspace->read_only) return error_data(doc, "Workspace is read-only.");
    size_t length = strlen(text);
    if (length > SB_EDIT_LIMIT) return error_data(doc, "Text exceeds the edit byte limit reported by host_capabilities.");
    /* Validate text before creating an approval record. Preserve exact bytes, including BOM. */
    char *decoded = NULL; size_t decoded_size, consumed; const char *encoding;
    sb_error error = {0};
    if (!sb_decode((const unsigned char *)text, length, true, "utf-8", true,
                   &decoded, &decoded_size, &consumed, &encoding, &error)) return error_data(doc, error.text);
    free(decoded);
    sb_create *plan = sb_platform_prepare_create(workspace->root, path, &error);
    if (!plan) return error_data(doc, error.text);
    const char *target = sb_platform_create_target(plan), *identity = sb_platform_create_identity(plan);
    char hash[65], digest[65];
    if (!sb_platform_sha256(text, length, hash, &error)) { sb_platform_create_close(plan); return error_data(doc, error.text); }
    /* Length-prefixed fields prevent concatenation ambiguity. A single checked
       allocation makes approval binding fail closed even during allocation failure. */
    size_t tuple_capacity = strlen(workspace->id) + strlen(target) + strlen(identity) + 256;
    char *tuple_text = malloc(tuple_capacity);
    int tuple_length = tuple_text ? snprintf(tuple_text, tuple_capacity, "fs_write:create:utf-8|%zu:%s%zu:%s%zu:%s64:%s|%zu",
        strlen(workspace->id), workspace->id, strlen(target), target, strlen(identity), identity, hash, length) : -1;
    bool hashed = tuple_length >= 0 && (size_t)tuple_length < tuple_capacity &&
        sb_platform_sha256(tuple_text, (size_t)tuple_length, digest, &error);
    free(tuple_text);
    if (!hashed) { sb_platform_create_close(plan); return error_data(doc, "Cannot hash approval parameters."); }

    sb_policy *policy = host->policy;
    sb_platform_mutex_lock(policy->mutex);
    record *r = approval_id ? find(policy, approval_id) : NULL;
    yyjson_mut_val *data;
    if (approval_id) {
        if (!r || strcmp(r->actor, actor) || strcmp(r->digest, digest)) {
            data = error_data(doc, "Approval does not match this caller, target, parent directory, or content."); goto done;
        }
        expire(policy, r);
        if (r->status == APPROVED) {
            if (!audit_record(policy, r, "write_started", actor)) {
                data = error_data(doc, "Audit recording unavailable; file creation was not started."); goto done;
            }
            r->status = RUNNING;
            /* No record can be evicted while running. The parent remains pinned. */
            sb_platform_mutex_unlock(policy->mutex);
            bool ok = sb_platform_create_text(plan, text, length, &error);
            sb_platform_mutex_lock(policy->mutex);
            r->status = ok ? COMPLETED : FAILED;
            if (!ok) memcpy(r->error, error.text, sizeof(r->error));
            /* A failed result record cannot undo a committed create. Keep the
               actual result and cached retry behavior, with a separate warning. */
            audit_record(policy, r, ok ? "write_completed" : "write_failed", actor);
        }
    } else {
        for (size_t i = 0; i < policy->record_limit; ++i) {
            record *candidate = &policy->records[i];
            if (!*candidate->id) continue;
            expire(policy, candidate);
            if ((candidate->status == PENDING || candidate->status == APPROVED) &&
                !strcmp(candidate->actor, actor) && !strcmp(candidate->digest, digest)) { r = candidate; break; }
        }
        if (!r) {
            if (!sb_audit_healthy(policy->audit)) { data = error_data(doc, "Audit recording unavailable; new approvals are blocked."); goto done; }
            for (size_t i = 0; i < policy->record_limit; ++i) {
                record *candidate = &policy->records[i];
                if (!*candidate->id || (candidate->status != RUNNING && sb_platform_millis() >= candidate->deadline)) {
                    r = candidate; clear(r); break;
                }
            }
            if (!r) { data = error_data(doc, "Approval queue is full. Wait for records to expire."); goto done; }
            unsigned char random[16];
            r->target = sb_strdup(target); r->identity = sb_strdup(identity); r->text = sb_strdup(text);
            if (!r->target || !r->identity || !r->text || !sb_platform_random(random, sizeof(random), &error)) {
                clear(r); data = error_data(doc, "Cannot allocate approval request."); goto done;
            }
            static const char hex[] = "0123456789abcdef";
            for (size_t i = 0; i < sizeof(random); ++i) { r->id[i * 2] = hex[random[i] >> 4]; r->id[i * 2 + 1] = hex[random[i] & 15]; }
            memcpy(r->digest, digest, sizeof(digest)); memcpy(r->hash, hash, sizeof(hash));
            strcpy(r->actor, actor); strcpy(r->workspace, workspace->id);
            r->bytes = length; r->deadline = sb_platform_millis() + (uint64_t)policy->ttl * 1000;
            r->status = PENDING;
            if (!audit_record(policy, r, "approval_requested", actor)) {
                clear(r); data = error_data(doc, "Cannot record approval request."); goto done;
            }
        }
    }
    data = describe(doc, r, false);
    *failed = r->status != COMPLETED;
    if (r->status == PENDING || r->status == APPROVED) {
        yyjson_mut_obj_add_str(doc, data, "action", "Inspect and approve locally, then retry the identical tool call with approval_id.");
    }
done:
    sb_platform_mutex_unlock(policy->mutex);
    sb_platform_create_close(plan);
    return data;
}

static bool edit_text_valid(const char *text, size_t length, sb_error *error) {
    char *decoded = NULL; size_t size, consumed; const char *encoding;
    bool ok = length <= SB_EDIT_LIMIT && sb_decode((const unsigned char *)text, length, true,
        "utf-8", true, &decoded, &size, &consumed, &encoding, error);
    free(decoded);
    if (length > SB_EDIT_LIMIT) sb_fail_limit(error,"file.text",length,SB_EDIT_LIMIT,"UTF-8 bytes");
    return ok;
}
static char *edited_text(const char *original, size_t length, const sb_edit_request *request, sb_error *error) {
    if (!edit_text_valid(original, length, error)) return NULL;
    if (!strcmp(request->tool, "fs_write")) {
        if (!edit_text_valid(request->text, strlen(request->text), error)) return NULL;
        return sb_strdup(request->text);
    }
    size_t find_size = strlen(request->search), replace_size = strlen(request->replacement), matches = 0;
    if (!find_size || !edit_text_valid(request->search, find_size, error) ||
        !edit_text_valid(request->replacement, replace_size, error)) return NULL;
    const char *position = original, *match;
    while ((match = strstr(position, request->search)) != NULL) { ++matches; position = match + find_size; }
    if (matches != request->expected_matches) { sb_fail(error, "Search occurrence count does not match expected_matches."); return NULL; }
    size_t output_size = length - find_size * matches;
    if (matches && replace_size > (SB_EDIT_LIMIT - output_size) / matches) {
        sb_fail_limit(error,"patched_text",output_size+(uint64_t)replace_size*matches,SB_EDIT_LIMIT,"UTF-8 bytes"); return NULL;
    }
    output_size += replace_size * matches;
    char *output = malloc(output_size + 1);
    if (!output) { sb_fail(error, "Cannot allocate patched text."); return NULL; }
    position = original; char *destination = output;
    while ((match = strstr(position, request->search)) != NULL) {
        size_t prefix = (size_t)(match - position);
        memcpy(destination, position, prefix); destination += prefix;
        memcpy(destination, request->replacement, replace_size); destination += replace_size;
        position = match + find_size;
    }
    memcpy(destination, position, strlen(position) + 1);
    return output;
}
yyjson_mut_val *sb_policy_modify_authorized(sb_host *host, const char *actor, sb_workspace *workspace,
    const char *path, const sb_edit_request *request, yyjson_mut_doc *doc, bool *failed) {
    *failed = true; sb_error error = {0}; char original_hash[65], hash[65], backup[33] = {0};
    if (workspace->read_only) return error_data(doc,"Workspace is read-only.");
    sb_modify *plan = sb_platform_prepare_modify_guarded(workspace->root,path,host->approved_target_version,&error);
    size_t size = 0; const char *original = plan ? sb_platform_modify_original(plan,&size) : NULL;
    char *output = NULL;
    bool ok = original && sb_platform_sha256(original,size,original_hash,&error);
    if (ok && strcmp(original_hash,request->expected_hash)) { sb_fail(&error,"expected_sha256 does not match the original file."); ok = false; }
    if (ok) { output = edited_text(original,size,request,&error); ok = output != NULL; }
    if (ok && size == strlen(output) && !memcmp(original,output,size)) { sb_fail(&error,"Modification does not change the file."); ok = false; }
    if (ok) ok = sb_platform_sha256(output,strlen(output),hash,&error) && sb_platform_modify_backup(plan,host->lease,backup,&error);
    if (ok) ok = sb_audit_append(host->audit,"backup_saved",actor,NULL,workspace->id,sb_platform_modify_target(plan),hash,strlen(output),request->tool,"edit",original_hash,backup);
    if (ok) ok = sb_platform_modify_text(plan,output,strlen(output),&error);
    yyjson_mut_val *data = ok ? yyjson_mut_obj(doc) : error_data(doc,*error.text ? error.text : "Audit unavailable; modification was not started.");
    if (ok) {
        yyjson_mut_obj_add_str(doc,data,"status","completed"); yyjson_mut_obj_add_strcpy(doc,data,"sha256",hash);
        yyjson_mut_obj_add_strcpy(doc,data,"original_sha256",original_hash); yyjson_mut_obj_add_strcpy(doc,data,"backup_id",backup);
        yyjson_mut_obj_add_bool(doc,data,"atomic_save",false); yyjson_mut_obj_add_uint(doc,data,"bytes",strlen(output));
    }
    free(output); sb_platform_modify_close(plan); *failed = !ok; return data;
}
yyjson_mut_val *sb_policy_modify(sb_host *host, const char *actor, sb_workspace *workspace,
                                const char *path, const sb_edit_request *request,
                                yyjson_mut_doc *doc, bool *failed) {
    *failed = true;
    if (workspace->read_only) return error_data(doc, "Workspace is read-only.");
    sb_error error = {0}; char input_hash[65], replacement_hash[65], digest[65];
    const char *input = request->text ? request->text : request->search;
    const char *replacement = request->replacement ? request->replacement : "";
    if (!edit_text_valid(input, strlen(input), &error) || !edit_text_valid(replacement, strlen(replacement), &error) ||
        !sb_platform_sha256(input, strlen(input), input_hash, &error) ||
        !sb_platform_sha256(replacement, strlen(replacement), replacement_hash, &error)) return error_data(doc, error.text);
    size_t tuple_capacity = strlen(path) + strlen(workspace->id) + 512;
    char *tuple = malloc(tuple_capacity);
    int tuple_length = tuple ? snprintf(tuple, tuple_capacity, "%s|%zu:%s%zu:%s|%s|%s|%s|%zu",
        request->tool, strlen(workspace->id), workspace->id, strlen(path), path,
        request->expected_hash, input_hash, replacement_hash, request->expected_matches) : -1;
    bool hashed = tuple_length >= 0 && (size_t)tuple_length < tuple_capacity &&
        sb_platform_sha256(tuple, (size_t)tuple_length, digest, &error);
    free(tuple);
    if (!hashed) return error_data(doc, "Cannot hash modification parameters.");
    sb_policy *policy = host->policy; sb_modify *plan = NULL; char *output = NULL;
    yyjson_mut_val *data; record *r = NULL;
    sb_platform_mutex_lock(policy->mutex);
    if (request->approval_id) {
        r = find(policy, request->approval_id);
        if (!r || strcmp(r->actor, actor) || strcmp(r->digest, digest) || strcmp(r->tool, request->tool)) {
            data = error_data(doc, "Approval does not match the modification arguments or caller."); goto finish;
        }
        expire(policy, r);
        if (r->status != APPROVED) { data = describe(doc, r, false); *failed = r->status != COMPLETED; goto finish; }
        if (!audit_record(policy, r, "write_started", actor)) {
            data = error_data(doc, "Audit recording unavailable; modification was not started."); goto finish;
        }
        r->status = RUNNING;
        sb_platform_mutex_unlock(policy->mutex);
        plan = sb_platform_prepare_modify(workspace->root, path, &error);
        size_t original_size = 0; const char *original = plan ? sb_platform_modify_original(plan, &original_size) : NULL;
        char actual_hash[65];
        bool ok = plan && sb_platform_sha256(original, original_size, actual_hash, &error);
        if (ok && (strcmp(actual_hash, r->original_hash) || strcmp(sb_platform_modify_parent(plan), r->identity) ||
            strcmp(sb_platform_modify_identity(plan), r->file_identity) || strcmp(sb_platform_modify_target(plan), r->target))) {
            sb_fail(&error, "Original file or physical directory changed after approval; modification was not started."); ok = false;
        }
        char backup[33] = {0};
        if (ok) ok = sb_platform_modify_backup(plan, host->lease, backup, &error);
        sb_platform_mutex_lock(policy->mutex);
        if (*backup) memcpy(r->backup_id, backup, sizeof(backup));
        if (ok && !audit_record(policy, r, "backup_saved", actor)) {
            sb_fail(&error, "Cannot record the private backup; original file was not modified."); ok = false;
        }
        sb_platform_mutex_unlock(policy->mutex);
        if (ok) ok = sb_platform_modify_text(plan, r->text, r->bytes, &error);
        sb_platform_mutex_lock(policy->mutex);
        r->status = ok ? COMPLETED : FAILED;
        if (!ok) memcpy(r->error, error.text, sizeof(r->error));
        audit_record(policy, r, ok ? "write_completed" : "write_failed", actor);
        data = describe(doc, r, false); *failed = !ok;
        goto finish;
    }
    sb_platform_mutex_unlock(policy->mutex);
    plan = sb_platform_prepare_modify(workspace->root, path, &error);
    size_t original_size = 0; const char *original = plan ? sb_platform_modify_original(plan, &original_size) : NULL;
    char original_hash[65], output_hash[65];
    bool ready = plan && sb_platform_sha256(original, original_size, original_hash, &error);
    if (ready && strcmp(original_hash, request->expected_hash)) { sb_fail(&error, "expected_sha256 does not match the original file."); ready = false; }
    if (ready) { output = edited_text(original, original_size, request, &error); ready = output != NULL; }
    if (ready && strlen(output) == original_size && !memcmp(output, original, original_size)) {
        sb_fail(&error, "Modification does not change the file."); ready = false;
    }
    if (ready) ready = sb_platform_sha256(output, strlen(output), output_hash, &error);
    sb_platform_mutex_lock(policy->mutex);
    if (!ready) { data = error_data(doc, *error.text ? error.text : "Cannot prepare the modification."); goto finish; }
    for (size_t i = 0; i < policy->record_limit; ++i) {
        record *candidate = &policy->records[i];
        if (!*candidate->id) continue;
        expire(policy, candidate);
        if ((candidate->status == PENDING || candidate->status == APPROVED) && !strcmp(candidate->actor, actor) &&
            !strcmp(candidate->digest, digest) && !strcmp(candidate->tool, request->tool) &&
            !strcmp(candidate->file_identity, sb_platform_modify_identity(plan)) &&
            !strcmp(candidate->identity, sb_platform_modify_parent(plan))) { r = candidate; break; }
    }
    if (!r) {
        if (!sb_audit_healthy(policy->audit)) { data = error_data(doc, "Audit recording unavailable; new approvals are blocked."); goto finish; }
        for (size_t i = 0; i < policy->record_limit; ++i) {
            record *candidate = &policy->records[i];
            if (!*candidate->id || (candidate->status != RUNNING && sb_platform_millis() >= candidate->deadline)) {
                r = candidate; clear(r); break;
            }
        }
        if (!r) { data = error_data(doc, "Approval queue is full. Wait for records to expire."); goto finish; }
        r->target = sb_strdup(sb_platform_modify_target(plan)); r->identity = sb_strdup(sb_platform_modify_parent(plan));
        r->text = sb_strdup(output); r->original = sb_strdup(original);
        unsigned char random[16];
        if (!r->target || !r->identity || !r->text || !r->original || !sb_platform_random(random, sizeof(random), &error)) {
            clear(r); data = error_data(doc, "Cannot allocate modification approval."); goto finish;
        }
        for (size_t i = 0; i < sizeof(random); ++i) snprintf(r->id + i * 2, 3, "%02x", random[i]);
        memcpy(r->digest, digest, sizeof(digest)); memcpy(r->hash, output_hash, sizeof(output_hash));
        memcpy(r->original_hash, original_hash, sizeof(original_hash));
        strcpy(r->file_identity, sb_platform_modify_identity(plan));
        strcpy(r->actor, actor); strcpy(r->workspace, workspace->id); strcpy(r->tool, request->tool);
        strcpy(r->operation, !strcmp(request->tool, "fs_patch") ? "patch" : "overwrite");
        r->bytes = strlen(output); r->deadline = sb_platform_millis() + (uint64_t)policy->ttl * 1000;
        r->status = PENDING;
        if (!audit_record(policy, r, "approval_requested", actor)) {
            clear(r); data = error_data(doc, "Cannot record modification approval."); goto finish;
        }
    }
    data = describe(doc, r, false);
    yyjson_mut_obj_add_str(doc, data, "action", "Inspect original and replacement locally, approve, then retry identical arguments with approval_id.");
finish:
    sb_platform_mutex_unlock(policy->mutex);
    sb_platform_modify_close(plan); free(output);
    return data;
}

char *sb_policy_owner_request_actor(sb_policy *policy, const char *request, size_t length, const char *owner_actor) {
    yyjson_doc *input = length <= 4096 ? yyjson_read(request, length, 0) : NULL;
    yyjson_val *root = yyjson_doc_get_root(input);
    const char *command = sb_string(yyjson_obj_get(root, "command"));
    const char *id = sb_string(yyjson_obj_get(root, "id"));
    static const char *const keys[]={"command","id","expected_sha256","expected_host_instance_id","expected_policy_revision","offset","limit"};
    bool valid=command && sb_config_keys(root,keys,7);
    uint64_t offset=0,limit=64,revision=0;
    yyjson_val *offset_value=yyjson_obj_get(root,"offset"), *limit_value=yyjson_obj_get(root,"limit");
    if ((offset_value && (!sb_integer(offset_value,&offset) || offset>256)) || (limit_value && (!sb_integer(limit_value,&limit) || !limit || limit>64))) valid=false;
    size_t idx, max; yyjson_val *key, *value;
    yyjson_obj_foreach(root, idx, max, key, value) {
        (void)value;
        const char *name = sb_string(key);
        if (!name) valid = false;
        yyjson_obj_iter it = yyjson_obj_iter_with(root);
        for (size_t j = 0; j < idx; ++j) if (!strcmp(yyjson_get_str(key), yyjson_get_str(yyjson_obj_iter_next(&it)))) valid = false;
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) { yyjson_doc_free(input); return NULL; }
    yyjson_mut_val *result;
    sb_platform_mutex_lock(policy->mutex);
    if (!valid) result = error_data(doc, "Invalid owner request.");
    else if (!strcmp(command,"history")) {
        char *history=sb_store_history(policy->store,NULL,id,id!=NULL,(unsigned)offset,(unsigned)limit);
        yyjson_doc *saved=history ? yyjson_read(history,strlen(history),0) : NULL;
        result=saved ? yyjson_val_mut_copy(doc,yyjson_doc_get_root(saved)) : error_data(doc,"Persistent history unavailable.");
        yyjson_doc_free(saved); free(history);
    }
    else if (!strcmp(command, "list") && !id) {
        result = yyjson_mut_obj(doc); yyjson_mut_val *items = yyjson_mut_arr(doc);
        for (size_t i = 0; i < policy->record_limit; ++i) if (*policy->records[i].id) {
            expire(policy, &policy->records[i]); yyjson_mut_arr_add_val(items, describe(doc, &policy->records[i], false));
        }
        yyjson_mut_obj_add_val(doc, result, "approvals", items);
    } else {
        record *r = id ? find(policy, id) : NULL;
        if (!r) result = error_data(doc, "Unknown approval id in this running instance.");
        else {
            expire(policy, r);
            bool ok = true;
            const char *expected=sb_string(yyjson_obj_get(root,"expected_sha256")), *instance=sb_string(yyjson_obj_get(root,"expected_host_instance_id"));
            yyjson_val *version=yyjson_obj_get(root,"expected_policy_revision");
            if ((yyjson_obj_get(root,"expected_sha256") && (!expected || strcmp(expected,r->hash))) ||
                (yyjson_obj_get(root,"expected_host_instance_id") && (!instance || strcmp(instance,r->instance))) ||
                (version && (!sb_integer(version,&revision) || revision!=r->revision))) {
                result=error_data(doc,"Owner decision does not match the inspected operation, instance or policy."); goto serialize;
            }
            if (!strcmp(command, "inspect")) { /* Full content is returned only on the owner channel. */ }
            else if (!strcmp(command, "approve") && r->status == PENDING) {
                if (audit_record(policy, r, "approval_approved", owner_actor)) r->status = APPROVED;
                else { result = error_data(doc, "Audit recording unavailable; approval was not granted."); goto serialize; }
            }
            else if ((!strcmp(command, "deny") || !strcmp(command, "revoke")) &&
                     (r->status == PENDING || r->status == APPROVED ||
                      (!strcmp(command,"revoke") && r->status==COMPLETED && logical_read(r)))) {
                r->status = DENIED;
                audit_record(policy, r, !strcmp(command, "deny") ? "approval_denied" : "approval_revoked", owner_actor);
            }
            else ok = false;
            result = ok ? describe(doc, r, !strcmp(command, "inspect")) : error_data(doc, "Operation is not allowed in the current approval state.");
        }
    }
serialize:
    yyjson_mut_doc_set_root(doc, result);
    char *serialized = yyjson_mut_write(doc, 0, NULL);
    sb_platform_mutex_unlock(policy->mutex);
    yyjson_doc_free(input); yyjson_mut_doc_free(doc);
    return serialized;
}
char *sb_policy_owner_request(sb_policy *policy, const char *request, size_t length) {
    return sb_policy_owner_request_actor(policy,request,length,"local-owner");
}
char *sb_policy_caller_request(sb_policy *policy, const char *actor, const char *command, const char *id) {
    yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL); if (!doc) return NULL;
    sb_platform_mutex_lock(policy->mutex); record *r=id ? find(policy,id) : NULL; yyjson_mut_val *body=NULL;
    if (!r || strcmp(r->actor,actor)) body=error_data(doc,"Unknown approval id for this caller and running instance.");
    else {
        expire(policy,r);
        if (!strcmp(command,"cancel")) {
            if (r->status==PENDING || r->status==APPROVED || (r->status==COMPLETED && logical_read(r))) {
                r->status=CANCELLED; audit_record(policy,r,"approval_cancelled",actor);
            } else if (r->status!=CANCELLED) body=error_data(doc,"Approval can no longer be cancelled; side effects are not rolled back.");
        }
        if (!body) body=describe(doc,r,false);
    }
    yyjson_mut_doc_set_root(doc,body); char *json=yyjson_mut_write(doc,0,NULL);
    sb_platform_mutex_unlock(policy->mutex); yyjson_mut_doc_free(doc); return json;
}
