#include "sb_projects.h"
#include <string.h>

static const char *const tool_names[SB_PROJECT_TOOL_COUNT] = {
    "host_info", "host_capabilities", "fs_list", "fs_read", "fs_search", "image_view",
    "fs_write", "fs_patch", "task_start", "task_status", "task_output", "task_input", "task_cancel"
};

const char *sb_project_tool_name(size_t index) {
    return index < SB_PROJECT_TOOL_COUNT ? tool_names[index] : NULL;
}

int sb_project_tool_index(const char *name) {
    for (int i = 0; name && i < SB_PROJECT_TOOL_COUNT; ++i)
        if (!strcmp(name, tool_names[i])) return i;
    return -1;
}

static sb_policy_result result(sb_decision decision, const char *reason) {
    sb_policy_result value = {decision, reason};
    return value;
}

sb_policy_result sb_project_evaluate(const sb_request_context *context, const sb_project_operation *operation) {
    if (!context || !context->project || !context->actor || !*context->actor || !operation)
        return result(SB_DECISION_DENY, "unbound_project");
    const sb_project *project = context->project;
    if (!context->policy_revision || context->policy_revision != project->policy_revision)
        return result(SB_DECISION_DENY, "stale_policy");
    if (operation->requested_project_id && strcmp(operation->requested_project_id, project->id))
        return result(SB_DECISION_DENY, "project_mismatch");
    int tool = sb_project_tool_index(operation->tool);
    if (tool < 0) return result(SB_DECISION_DENY, "unknown_tool");
    bool task_result = tool == 9 || tool == 10 || tool == 12;
    if ((task_result || tool == 11) && !operation->owned_task)
        return result(SB_DECISION_DENY, "task_not_owned");
    /* Revocation never prevents the owner from cancelling an existing task or
       obtaining its already-authorized result. Backend ownership stays mandatory. */
    if (task_result) return result(SB_DECISION_ALLOW, "owned_task_result");
    if (!project->enabled) return result(SB_DECISION_DENY, "project_disabled");
    if (!(project->policy.tools & (UINT32_C(1) << tool))) return result(SB_DECISION_DENY, "tool_disabled");
    if (tool < 2) return result(SB_DECISION_ALLOW, "project_metadata");
    if (operation->identity < SB_ID_RESTRICTED || operation->identity > SB_ID_ADMINISTRATOR)
        return result(SB_DECISION_DENY, "invalid_identity");
    bool command = tool == 8 || tool == 11;
    bool write = tool == 6 || tool == 7;
    bool directory_owned = false;
    for (size_t i = 0; i < project->directory_count; ++i)
        if (operation->directory == &project->directories[i]) directory_owned = true;
    if (operation->directory && (!directory_owned || operation->current_user_target))
        return result(SB_DECISION_DENY, "invalid_directory_binding");
    if (tool != 11) {
        if (!directory_owned && (!operation->current_user_target || project->policy.file_scope != SB_FILES_CURRENT_USER))
            return result(SB_DECISION_DENY, "outside_file_scope");
        if (write && (directory_owned ? operation->directory->read_only : project->policy.current_user_read_only))
            return result(SB_DECISION_DENY, "read_only");
    }
    if (operation->identity == SB_ID_ADMINISTRATOR) {
        if (!project->policy.administrator_allowed) return result(SB_DECISION_DENY, "administrator_not_allowed");
        if (project->policy.file_scope != SB_FILES_CURRENT_USER && !project->legacy_compatibility)
            return result(SB_DECISION_DENY, "administrator_incompatible_with_isolation");
        /* The dev.7 single-host agent cannot establish a trusted project binding. */
        return result(SB_DECISION_DENY, "project_administrator_backend_unavailable");
    }
    if (command) {
        if (!project->policy.execution_enabled) return result(SB_DECISION_DENY, "execution_disabled");
        if (operation->identity != project->policy.default_identity)
            return result(SB_DECISION_DENY, "execution_identity_exceeds_policy");
        if (operation->identity == SB_ID_RESTRICTED)
            return result(SB_DECISION_DENY, "restricted_backend_unverified");
        if (project->policy.file_scope != SB_FILES_CURRENT_USER && !project->legacy_compatibility)
            return result(SB_DECISION_DENY, "directory_isolation_unavailable");
        if (!project->legacy_compatibility) {
            if (project->policy.current_user_read_only)
                return result(SB_DECISION_DENY, "read_only_command_isolation_unavailable");
            for (size_t i = 0; i < project->directory_count; ++i)
                if (project->directories[i].read_only)
                    return result(SB_DECISION_DENY, "read_only_command_isolation_unavailable");
        }
        if (tool == 8 && (!directory_owned || !operation->directory->workspace))
            return result(SB_DECISION_DENY, "cwd_requires_workspace");
    }
    if (project->legacy_compatibility) {
        /* Old behavior is deliberately not called never: reads automatic,
           writes approved, enabled account commands preauthorized. */
        return result(write ? SB_DECISION_PENDING : SB_DECISION_ALLOW, write ? "owner_approval_required" : "legacy_authorization");
    }
    if (project->policy.approval_mode == SB_PROJECT_NEVER)
        return result(SB_DECISION_ALLOW, "within_policy");
    if (project->policy.approval_mode == SB_PROJECT_WORKSPACE_AUTO && !command && directory_owned && operation->directory->workspace)
        return result(SB_DECISION_ALLOW, "workspace_file_operation");
    if (project->policy.approval_mode != SB_PROJECT_ALWAYS && project->policy.approval_mode != SB_PROJECT_WORKSPACE_AUTO)
        return result(SB_DECISION_DENY, "invalid_approval_mode");
    return result(SB_DECISION_PENDING, "owner_approval_required");
}
