#ifndef SHELLBOT_PROJECTS_H
#define SHELLBOT_PROJECTS_H

#include "sb.h"

#define SB_PROJECT_FORMAT 1
#define SB_MAX_PROJECTS 16
#define SB_MAX_PROJECT_DIRECTORIES (SB_MAX_ROOTS * 2)
#define SB_PROJECT_TOOL_COUNT 27

typedef enum { SB_PROJECT_ALWAYS, SB_PROJECT_WORKSPACE_AUTO, SB_PROJECT_NEVER } sb_project_approval;
typedef enum { SB_FILES_CONFIGURED, SB_FILES_CURRENT_USER } sb_file_scope;
typedef enum { SB_ID_RESTRICTED, SB_ID_CURRENT_USER, SB_ID_ADMINISTRATOR } sb_execution_identity;
typedef enum { SB_DECISION_DENY, SB_DECISION_ALLOW, SB_DECISION_PENDING } sb_decision;

typedef struct {
    bool enabled, control, clipboard_read, clipboard_write;
    unsigned lease_seconds, emergency_function_key;
} sb_desktop_policy;

typedef struct {
    char id[64];
    char *path; /* Configuration only. Runtime must resolve physical handles before evaluation. */
    bool read_only;
    bool workspace;
} sb_project_directory;

typedef struct {
    sb_project_approval approval_mode;
    sb_file_scope file_scope;
    bool current_user_read_only;
    bool execution_enabled;
    sb_execution_identity default_identity;
    bool administrator_allowed; /* Owner configuration, independent of agent availability. */
    uint32_t tools;
    sb_desktop_policy desktop; /* Independent session-wide disclosure/input permission. */
    char *runtime_files[16]; /* Owner-configured, exact readonly executable/DLL resources. */
    size_t runtime_file_count;
} sb_project_policy;

struct sb_project {
    char id[64], token_env[64], default_workspace_id[64];
    char *data_dir;
    uint64_t policy_revision;
    bool enabled;
    sb_project_directory directories[SB_MAX_PROJECT_DIRECTORIES];
    size_t directory_count;
    sb_project_policy policy;
    unsigned approval_ttl_seconds;
    uint64_t audit_max_bytes;
    sb_resource_limits limits;
    bool legacy_compatibility;
    sb_execution_profile legacy_profile;
    sb_execution_approval legacy_approval;
};

typedef struct {
    unsigned format_version, port;
    char public_origin[320], default_project_id[64];
    char owner_token_env[64]; /* Independent remote owner credential reference. */
    sb_project projects[SB_MAX_PROJECTS];
    size_t project_count;
} sb_project_registry;

typedef struct {
    const char *tool;
    const char *requested_project_id; /* Optional assertion only; cannot change context. */
    const sb_project_directory *directory; /* Trusted physical resolver result, never a client pointer/id. */
    bool current_user_target; /* Trusted resolver classified an unlisted, account-accessible file. */
    sb_execution_identity identity;
    bool owned_task; /* Backend verified both project and authenticated actor, not a JSON boolean. */
    bool owned_request;
    bool restricted_backend_verified; /* Actual local sandbox verification, never accepted from JSON. */
    bool administrator_backend_verified; /* Verified manager-owned agent, not a client assertion. */
    bool desktop_cleanup; /* Validated lease status/release; never grants new control. */
} sb_project_operation;

typedef struct { sb_decision decision; const char *reason; } sb_policy_result;

/* Future approval records bind these fields before any execution. Pending does
   not grant permission; pagination also requires the same version and budget. */
typedef struct {
    char project_id[64], actor[64], operation_sha256[65];
    uint64_t policy_revision, deadline_ms, maximum_bytes;
    const char *target_identity, *target_version;
} sb_project_approval_binding;

bool sb_projects_parse(sb_project_registry *registry, const char *bytes, size_t length, sb_error *error);
bool sb_projects_load(sb_project_registry *registry, const char *path, sb_error *error);
void sb_projects_free(sb_project_registry *registry);
const sb_project *sb_project_find(const sb_project_registry *registry, const char *id);
const sb_project_directory *sb_project_directory_find(const sb_project *project, const char *id);
bool sb_project_path_valid(const char *path);
int sb_project_tool_index(const char *name);
const char *sb_project_tool_name(size_t index);
sb_policy_result sb_project_evaluate(const sb_request_context *context, const sb_project_operation *operation);
char *sb_projects_summary(const sb_project_registry *registry);
/* Validates the same immutable legacy bytes that are later backed up. No Data migration. */
char *sb_projects_migrate(const char *bytes, size_t length, const char *project_id,
                          const char *new_data_dir, sb_error *error);
/* Owner CLI export to a new file. Existing registry is never modified in place. */
char *sb_projects_edit(const char *bytes, size_t length, const char *project_json,
                       size_t project_length, const char *id, bool configure, sb_error *error);

#endif
