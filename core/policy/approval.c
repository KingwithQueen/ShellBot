#include "sb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RECORD_LIMIT 64
typedef enum { PENDING, APPROVED, RUNNING, COMPLETED, DENIED, EXPIRED, FAILED } state;
typedef struct {
    char id[33], digest[65], hash[65], actor[32], workspace[64];
    char *target, *identity, *text, *original;
    char tool[16], operation[16], original_hash[65], file_identity[96], backup_id[33];
    size_t bytes;
    uint64_t deadline;
    state status;
    bool audit_warning;
    char error[256];
} record;
struct sb_policy { sb_mutex *mutex; sb_audit *audit; unsigned ttl; record records[RECORD_LIMIT]; };

static const char *state_name(state status) {
    static const char *const names[] = {"pending", "approved", "running", "completed", "denied", "expired", "failed"};
    return names[status];
}
static void clear(record *r) { free(r->target); free(r->identity); free(r->text); free(r->original); memset(r, 0, sizeof(*r)); }
static bool audit_record(sb_policy *policy, record *r, const char *event, const char *actor) {
    bool ok = sb_audit_append(policy->audit, event, actor, r->id, r->workspace, r->target, r->hash, r->bytes,
        *r->tool ? r->tool : NULL, *r->operation ? r->operation : NULL,
        *r->original_hash ? r->original_hash : NULL, *r->backup_id ? r->backup_id : NULL);
    if (!ok) r->audit_warning = true;
    return ok;
}
static void expire(sb_policy *policy, record *r) {
    if ((r->status == PENDING || r->status == APPROVED) && sb_platform_millis() >= r->deadline) {
        r->status = EXPIRED;
        audit_record(policy, r, "approval_expired", "host");
    }
}
static record *find(sb_policy *policy, const char *id) {
    for (size_t i = 0; i < RECORD_LIMIT; ++i) if (*policy->records[i].id && !strcmp(policy->records[i].id, id)) return &policy->records[i];
    return NULL;
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
    yyjson_mut_obj_add_strcpy(doc, data, "actor", r->actor);
    yyjson_mut_obj_add_strcpy(doc, data, "workspace_id", r->workspace);
    yyjson_mut_obj_add_strcpy(doc, data, "target", r->target);
    yyjson_mut_obj_add_strcpy(doc, data, "sha256", r->hash);
    yyjson_mut_obj_add_uint(doc, data, "bytes", r->bytes);
    if (*r->tool) {
        yyjson_mut_obj_add_strcpy(doc, data, "tool", r->tool);
        yyjson_mut_obj_add_strcpy(doc, data, "operation", r->operation);
        yyjson_mut_obj_add_strcpy(doc, data, "original_sha256", r->original_hash);
        yyjson_mut_obj_add_bool(doc, data, "atomic_save", false);
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
    }
    if (*r->error) yyjson_mut_obj_add_strcpy(doc, data, "error", r->error);
    if (r->audit_warning) yyjson_mut_obj_add_str(doc, data, "audit_warning", "Audit recording is incomplete; the reported operation status remains authoritative.");
    return data;
}
sb_policy *sb_policy_new(unsigned ttl, sb_audit *audit, sb_error *error) {
    sb_policy *policy = calloc(1, sizeof(*policy));
    if (policy) policy->mutex = sb_platform_mutex_new();
    if (!policy || !policy->mutex) { free(policy); sb_fail(error, "Cannot allocate approval policy."); return NULL; }
    policy->ttl = ttl; policy->audit = audit;
    return policy;
}
void sb_policy_free(sb_policy *policy) {
    if (policy) {
        for (size_t i = 0; i < RECORD_LIMIT; ++i) clear(&policy->records[i]);
        sb_platform_mutex_free(policy->mutex); free(policy);
    }
}

yyjson_mut_val *sb_policy_write(sb_host *host, const char *actor, sb_workspace *workspace,
                               const char *path, const char *text, const char *approval_id,
                               yyjson_mut_doc *doc, bool *failed) {
    *failed = true;
    if (workspace->read_only) return error_data(doc, "Workspace is read-only.");
    size_t length = strlen(text);
    if (length > SB_TEXT_LIMIT) return error_data(doc, "Text exceeds 64 KiB.");
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
        for (size_t i = 0; i < RECORD_LIMIT; ++i) {
            record *candidate = &policy->records[i];
            if (!*candidate->id) continue;
            expire(policy, candidate);
            if ((candidate->status == PENDING || candidate->status == APPROVED) &&
                !strcmp(candidate->actor, actor) && !strcmp(candidate->digest, digest)) { r = candidate; break; }
        }
        if (!r) {
            if (!sb_audit_healthy(policy->audit)) { data = error_data(doc, "Audit recording unavailable; new approvals are blocked."); goto done; }
            for (size_t i = 0; i < RECORD_LIMIT; ++i) {
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
    bool ok = length <= SB_TEXT_LIMIT && sb_decode((const unsigned char *)text, length, true,
        "utf-8", true, &decoded, &size, &consumed, &encoding, error);
    free(decoded);
    if (length > SB_TEXT_LIMIT) sb_fail(error, "Editing supports UTF-8 text within 64 KiB.");
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
    if (matches && replace_size > (SB_TEXT_LIMIT - output_size) / matches) {
        sb_fail(error, "Patched text exceeds 64 KiB."); return NULL;
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
    for (size_t i = 0; i < RECORD_LIMIT; ++i) {
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
        for (size_t i = 0; i < RECORD_LIMIT; ++i) {
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

char *sb_policy_owner_request(sb_policy *policy, const char *request, size_t length) {
    yyjson_doc *input = length <= 4096 ? yyjson_read(request, length, 0) : NULL;
    yyjson_val *root = yyjson_doc_get_root(input);
    const char *command = sb_string(yyjson_obj_get(root, "command"));
    const char *id = sb_string(yyjson_obj_get(root, "id"));
    bool valid = yyjson_is_obj(root) && command && yyjson_obj_size(root) == (id ? 2u : 1u);
    size_t idx, max; yyjson_val *key, *value;
    yyjson_obj_foreach(root, idx, max, key, value) {
        (void)value;
        const char *name = sb_string(key);
        if (!name || (strcmp(name, "command") && strcmp(name, "id"))) valid = false;
        yyjson_obj_iter it = yyjson_obj_iter_with(root);
        for (size_t j = 0; j < idx; ++j) if (!strcmp(yyjson_get_str(key), yyjson_get_str(yyjson_obj_iter_next(&it)))) valid = false;
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) { yyjson_doc_free(input); return NULL; }
    yyjson_mut_val *result;
    sb_platform_mutex_lock(policy->mutex);
    if (!valid) result = error_data(doc, "Invalid owner request.");
    else if (!strcmp(command, "list") && !id) {
        result = yyjson_mut_obj(doc); yyjson_mut_val *items = yyjson_mut_arr(doc);
        for (size_t i = 0; i < RECORD_LIMIT; ++i) if (*policy->records[i].id) {
            expire(policy, &policy->records[i]); yyjson_mut_arr_add_val(items, describe(doc, &policy->records[i], false));
        }
        yyjson_mut_obj_add_val(doc, result, "approvals", items);
    } else {
        record *r = id ? find(policy, id) : NULL;
        if (!r) result = error_data(doc, "Unknown approval id in this running instance.");
        else {
            expire(policy, r);
            bool ok = true;
            if (!strcmp(command, "inspect")) { /* Full content is returned only on the owner channel. */ }
            else if (!strcmp(command, "approve") && r->status == PENDING) {
                if (audit_record(policy, r, "approval_approved", "local-owner")) r->status = APPROVED;
                else { result = error_data(doc, "Audit recording unavailable; approval was not granted."); goto serialize; }
            }
            else if ((!strcmp(command, "deny") || !strcmp(command, "revoke")) &&
                     (r->status == PENDING || r->status == APPROVED)) {
                r->status = DENIED;
                audit_record(policy, r, !strcmp(command, "deny") ? "approval_denied" : "approval_revoked", "local-owner");
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
