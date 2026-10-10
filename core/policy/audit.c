#include "sb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct sb_audit {
    sb_mutex *mutex;
    sb_journal *journal;
    char instance[33];
    uint64_t sequence, bytes, maximum;
    bool healthy;
};

bool sb_audit_append(sb_audit *audit, const char *event, const char *actor, const char *id,
                     const char *workspace, const char *target, const char *hash, size_t bytes,
                     const char *tool, const char *operation, const char *original_hash, const char *backup_id) {
    if (!audit) return false;
    sb_platform_mutex_lock(audit->mutex);
    bool ok = audit->healthy;
    yyjson_mut_doc *doc = ok ? yyjson_mut_doc_new(NULL) : NULL;
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
    if (ok) {
        ok = root && yyjson_mut_obj_add_uint(doc, root, "schema_version", 1) &&
            yyjson_mut_obj_add_str(doc, root, "instance_id", audit->instance) &&
            yyjson_mut_obj_add_str(doc, root, "version", SB_VERSION) &&
            yyjson_mut_obj_add_uint(doc, root, "sequence", audit->sequence + 1) &&
            yyjson_mut_obj_add_uint(doc, root, "time_unix_ms", sb_platform_unix_ms()) &&
            yyjson_mut_obj_add_str(doc, root, "event", event) &&
            yyjson_mut_obj_add_str(doc, root, "actor", actor);
        if (id) ok = ok && yyjson_mut_obj_add_str(doc, root, "approval_id", id) &&
            yyjson_mut_obj_add_str(doc, root, "tool", tool ? tool : "fs_write") &&
            yyjson_mut_obj_add_str(doc, root, "workspace_id", workspace) &&
            yyjson_mut_obj_add_str(doc, root, "target", target) &&
            yyjson_mut_obj_add_str(doc, root, "sha256", hash) &&
            yyjson_mut_obj_add_uint(doc, root, "bytes", bytes);
        if (id && operation) ok = ok && yyjson_mut_obj_add_str(doc, root, "operation", operation);
        if (id && original_hash) ok = ok && yyjson_mut_obj_add_str(doc, root, "original_sha256", original_hash);
        if (id && backup_id) ok = ok && yyjson_mut_obj_add_str(doc, root, "backup_id", backup_id);
    }
    if (doc) yyjson_mut_doc_set_root(doc, root);
    size_t length = 0;
    char *json = ok ? yyjson_mut_write(doc, 0, &length) : NULL;
    /* Bounded line size, plus an explicit per-instance storage budget. */
    char *line = json && length < 256u * 1024u ? malloc(length + 1) : NULL;
    ok = ok && line && length + 1 <= audit->maximum - audit->bytes;
    if (ok) {
        memcpy(line, json, length); line[length] = '\n';
        sb_error error = {0};
        ok = sb_platform_journal_append(audit->journal, line, length + 1, &error);
        if (ok) { audit->bytes += length + 1; ++audit->sequence; }
    }
    if (!ok && audit->healthy) {
        audit->healthy = false;
        fputs("ShellBot: Audit recording failed or reached its budget; new approvals and writes are blocked.\n", stderr);
    }
    free(line); free(json); yyjson_mut_doc_free(doc);
    sb_platform_mutex_unlock(audit->mutex);
    return ok;
}


bool sb_audit_task(sb_audit *audit, const char *event, const char *actor, const char *key,
                   const char *task_id, sb_execution_profile profile, const char *hash,
                   const char *operation, uint64_t pid) {
    if (!audit) return false;
    sb_platform_mutex_lock(audit->mutex);
    yyjson_mut_doc *doc = audit->healthy ? yyjson_mut_doc_new(NULL) : NULL;
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
    bool ok = root && yyjson_mut_obj_add_uint(doc,root,"schema_version",1) &&
        yyjson_mut_obj_add_str(doc,root,"instance_id",audit->instance) &&
        yyjson_mut_obj_add_str(doc,root,"version",SB_VERSION) &&
        yyjson_mut_obj_add_uint(doc,root,"sequence",audit->sequence+1) &&
        yyjson_mut_obj_add_uint(doc,root,"time_unix_ms",sb_platform_unix_ms()) &&
        yyjson_mut_obj_add_str(doc,root,"event",event) && yyjson_mut_obj_add_str(doc,root,"actor",actor) &&
        yyjson_mut_obj_add_str(doc,root,"execution_profile",sb_execution_profile_name(profile)) &&
        yyjson_mut_obj_add_str(doc,root,"operation",operation);
    if (ok && key) ok = yyjson_mut_obj_add_strcpy(doc,root,"request_key",key);
    if (ok && task_id) ok = yyjson_mut_obj_add_strcpy(doc,root,"task_id",task_id);
    if (ok && hash) ok = yyjson_mut_obj_add_strcpy(doc,root,"request_sha256",hash);
    if (ok && pid) ok = yyjson_mut_obj_add_uint(doc,root,"process_id",pid);
    if (doc) yyjson_mut_doc_set_root(doc,root);
    size_t length = 0;
    char *json = ok ? yyjson_mut_write(doc,0,&length) : NULL;
    char *line = json && length < 16384 ? malloc(length+1) : NULL;
    ok = ok && line && length+1 <= audit->maximum-audit->bytes;
    if (ok) {
        memcpy(line,json,length); line[length]='\n'; sb_error error={0};
        ok = sb_platform_journal_append(audit->journal,line,length+1,&error);
        if (ok) { audit->bytes+=length+1; ++audit->sequence; }
    }
    if (!ok && audit->healthy) {
        audit->healthy=false;
        fputs("ShellBot: Task audit recording failed; new task starts and stdin writes are blocked.\n",stderr);
    }
    free(line);free(json);yyjson_mut_doc_free(doc);sb_platform_mutex_unlock(audit->mutex);return ok;
}

sb_audit *sb_audit_new(sb_lease *lease, uint64_t maximum, sb_error *error) {
    sb_audit *audit = calloc(1, sizeof(*audit));
    if (audit) audit->mutex = sb_platform_mutex_new();
    unsigned char random[16];
    if (!audit || !audit->mutex || !sb_platform_random(random, sizeof(random), error)) {
        if (audit) sb_platform_mutex_free(audit->mutex);
        free(audit); sb_fail(error, "Cannot initialize audit recording."); return NULL;
    }
    for (size_t i = 0; i < sizeof(random); ++i) snprintf(audit->instance + i * 2, 3, "%02x", random[i]);
    char name[64]; snprintf(name, sizeof(name), "audit-%s.jsonl", audit->instance);
    audit->journal = sb_platform_journal_open(lease, name, error);
    audit->maximum = maximum; audit->healthy = audit->journal != NULL;
    if (!audit->healthy || !sb_audit_append(audit, "instance_opened", "host", NULL, NULL, NULL, NULL, 0, NULL, NULL, NULL, NULL)) {
        sb_platform_journal_close(audit->journal); sb_platform_mutex_free(audit->mutex); free(audit);
        sb_fail(error, "Cannot open and flush a protected audit journal."); return NULL;
    }
    return audit;
}

bool sb_audit_healthy(sb_audit *audit) {
    if (!audit) return false;
    sb_platform_mutex_lock(audit->mutex);
    bool healthy = audit->healthy;
    sb_platform_mutex_unlock(audit->mutex);
    return healthy;
}

bool sb_audit_project(sb_audit *audit, const char *event, const char *project, uint64_t revision, uint64_t pid) {
    if (!audit || !event || !project) return false;
    sb_platform_mutex_lock(audit->mutex);
    yyjson_mut_doc *doc = audit->healthy ? yyjson_mut_doc_new(NULL) : NULL;
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
    bool ok = root && yyjson_mut_obj_add_uint(doc, root, "schema_version", 1) &&
        yyjson_mut_obj_add_str(doc, root, "instance_id", audit->instance) &&
        yyjson_mut_obj_add_str(doc, root, "version", SB_VERSION) &&
        yyjson_mut_obj_add_uint(doc, root, "sequence", audit->sequence + 1) &&
        yyjson_mut_obj_add_uint(doc, root, "time_unix_ms", sb_platform_unix_ms()) &&
        yyjson_mut_obj_add_str(doc, root, "event", event) &&
        yyjson_mut_obj_add_str(doc, root, "actor", "owner") &&
        yyjson_mut_obj_add_str(doc, root, "project_id", project) &&
        yyjson_mut_obj_add_uint(doc, root, "policy_revision", revision) &&
        yyjson_mut_obj_add_uint(doc, root, "worker_pid", pid);
    if (doc) yyjson_mut_doc_set_root(doc, root);
    size_t length = 0; char *json = ok ? yyjson_mut_write(doc, 0, &length) : NULL;
    char *line = json && length < 4096 ? malloc(length + 1) : NULL;
    ok = ok && line && length + 1 <= audit->maximum - audit->bytes;
    if (ok) {
        memcpy(line, json, length); line[length] = '\n'; sb_error error = {0};
        ok = sb_platform_journal_append(audit->journal, line, length + 1, &error);
        if (ok) { audit->bytes += length + 1; ++audit->sequence; }
    }
    if (!ok) audit->healthy = false;
    free(line); free(json); yyjson_mut_doc_free(doc);
    sb_platform_mutex_unlock(audit->mutex); return ok;
}

void sb_audit_free(sb_audit *audit) {
    if (!audit) return;
    sb_audit_append(audit, "instance_closed", "host", NULL, NULL, NULL, NULL, 0, NULL, NULL, NULL, NULL);
    sb_platform_journal_close(audit->journal);
    sb_platform_mutex_free(audit->mutex); free(audit);
}
