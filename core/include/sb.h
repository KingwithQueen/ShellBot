#ifndef SHELLBOT_H
#define SHELLBOT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "yyjson.h"

#define SB_VERSION "0.1.0-dev.8"
#define SB_MAX_ROOTS 16
#define SB_PATH_CAP 32768
#define SB_REQUEST_LIMIT (1024u * 1024u)
#define SB_TEXT_LIMIT 65536u
#define SB_IMAGE_LIMIT (2u * 1024u * 1024u)
#define SB_RESPONSE_LIMIT (4u * 1024u * 1024u)
#define SB_AUDIT_LIMIT (8u * 1024u * 1024u)

typedef struct { char text[256]; } sb_error;
typedef struct sb_root sb_root;
typedef struct sb_file sb_file;
typedef struct sb_lease sb_lease;
typedef struct sb_mutex sb_mutex;
typedef struct sb_policy sb_policy;
typedef struct sb_owner sb_owner;
typedef struct sb_create sb_create;
typedef struct sb_audit sb_audit;
typedef struct sb_journal sb_journal;
typedef struct sb_modify sb_modify;
typedef struct sb_execution sb_execution;
typedef struct sb_project sb_project;
typedef enum { SB_EXEC_CONTROLLED, SB_EXEC_USER, SB_EXEC_ADMINISTRATOR } sb_execution_profile;
typedef enum { SB_APPROVAL_ALWAYS, SB_APPROVAL_RULES } sb_execution_approval;
typedef struct {
    const char *tool, *text, *search, *replacement, *expected_hash, *approval_id;
    size_t expected_matches;
} sb_edit_request;
typedef struct {
    char id[64];
    sb_root *root;
    bool read_only;
} sb_workspace;
typedef struct {
    sb_workspace workspaces[SB_MAX_ROOTS];
    size_t workspace_count;
    unsigned port;
    char token_env[64];
    char public_origin[320];
    char public_authority[264];
    char public_default_port[6];
    char *data_dir;
    unsigned approval_ttl_seconds;
    uint64_t audit_max_bytes;
    bool execution_enabled;
    sb_execution_profile execution_profile;
    sb_execution_approval execution_approval;
} sb_config;
typedef struct {
    sb_config config;
    const sb_project *project; /* Trusted manager binding; M1 does not start project hosts. */
    sb_lease *lease;
    sb_policy *policy;
    sb_owner *owner;
    sb_audit *audit;
    bool http_mode;
    uint64_t started_ms;
    sb_execution *execution;
} sb_host;
typedef struct {
    char *name;
    uint64_t size;
    bool directory;
    bool reparse;
} sb_entry;
typedef struct {
    bool initialized;
    bool ready;
    bool modern;
} sb_protocol;

typedef struct {
    const char *actor; /* Set by transport, never taken from request parameters. */
    const char *version;
    const char *method;
    const char *name;
    const sb_project *project; /* Set by authenticated transport, never by JSON parameters. */
    uint64_t policy_revision;
} sb_request_context;

void sb_fail(sb_error *error, const char *message);
char *sb_strdup(const char *value);
const char *sb_string(yyjson_val *value); /* Reject embedded NUL. */
bool sb_integer(yyjson_val *value, uint64_t *number);
bool sb_config_load(sb_config *config, const char *path, sb_error *error);
bool sb_config_parse(sb_config *config, const char *bytes, size_t length, sb_error *error);
bool sb_identifier(const char *id);
bool sb_config_keys(yyjson_val *object, const char *const *allowed, size_t count);
bool sb_config_root(sb_config *config, const char *id, const char *path, sb_error *error);
void sb_config_free(sb_config *config);
void sb_config_defaults(sb_config *config);
bool sb_config_public_url(sb_config *config, const char *value, sb_error *error);
sb_workspace *sb_workspace_find(sb_config *config, const char *id);
const char *sb_execution_profile_name(sb_execution_profile profile);
sb_execution *sb_execution_new(const sb_config *config, sb_error *error);
void sb_execution_free(sb_execution *execution);
yyjson_mut_val *sb_execution_status(sb_execution *execution, yyjson_mut_doc *document);
bool sb_execution_available(sb_execution *execution);
const char *sb_execution_instance(sb_execution *execution);
char *sb_execution_task(sb_execution *execution, const char *name, yyjson_val *arguments, sb_error *error);
yyjson_mut_val *sb_tasks_call(sb_host *host, const char *actor, const char *name, yyjson_val *arguments, yyjson_mut_doc *document, bool *failed);

/* Platform boundary: no native handles or Windows headers in the core. */
uint64_t sb_platform_millis(void);
uint64_t sb_platform_unix_ms(void);
void sb_platform_info(char *system, size_t capacity, char *arch, size_t arch_capacity);
sb_root *sb_platform_root(const char *path, sb_error *error);
void sb_platform_root_close(sb_root *root);
sb_file *sb_platform_open(sb_root *root, const char *relative, bool directory, sb_error *error);
void sb_platform_close(sb_file *file);
char *sb_platform_directory_path(sb_file *directory, sb_error *error);
uint64_t sb_platform_size(sb_file *file);
bool sb_platform_read(sb_file *file, uint64_t offset, unsigned char *buffer,
                      size_t capacity, size_t *length, sb_error *error);
bool sb_platform_list(sb_file *directory, size_t offset, size_t limit,
                      sb_entry **entries, size_t *count, bool *more, sb_error *error);
void sb_platform_entries_free(sb_entry *entries, size_t count);
char *sb_platform_default_data_dir(sb_error *error);
sb_lease *sb_platform_lease(const char *directory, sb_error *error);
void sb_platform_lease_close(sb_lease *lease);
bool sb_platform_random(unsigned char *buffer, size_t size, sb_error *error);
char *sb_platform_load_config(const char *path, size_t *length, sb_error *error);
bool sb_platform_export_config(const char *output, const char *backup,
                               const char *json, size_t json_length,
                               const char *original, size_t original_length, sb_error *error);
bool sb_platform_sha256(const void *bytes, size_t length, char hex[65], sb_error *error);
sb_mutex *sb_platform_mutex_new(void);
void sb_platform_mutex_free(sb_mutex *mutex);
void sb_platform_mutex_lock(sb_mutex *mutex);
void sb_platform_mutex_unlock(sb_mutex *mutex);
sb_create *sb_platform_prepare_create(sb_root *root, const char *relative, sb_error *error);
const char *sb_platform_create_target(sb_create *plan);
const char *sb_platform_create_identity(sb_create *plan);
bool sb_platform_create_text(sb_create *plan, const char *text, size_t length, sb_error *error);
void sb_platform_create_close(sb_create *plan);
sb_modify *sb_platform_prepare_modify(sb_root *root, const char *relative, sb_error *error);
const char *sb_platform_modify_target(sb_modify *plan);
const char *sb_platform_modify_parent(sb_modify *plan);
const char *sb_platform_modify_identity(sb_modify *plan);
const char *sb_platform_modify_original(sb_modify *plan, size_t *length);
bool sb_platform_modify_backup(sb_modify *plan, sb_lease *lease, char backup_id[33], sb_error *error);
bool sb_platform_modify_text(sb_modify *plan, const char *text, size_t length, sb_error *error);
void sb_platform_modify_close(sb_modify *plan);
sb_journal *sb_platform_journal_open(sb_lease *lease, const char *name, sb_error *error);
bool sb_platform_journal_append(sb_journal *journal, const char *line, size_t length, sb_error *error);
void sb_platform_journal_close(sb_journal *journal);

/* Only fixed metadata is accepted here. Never pass tool text or credentials. */
sb_audit *sb_audit_new(sb_lease *lease, uint64_t maximum, sb_error *error);
bool sb_audit_healthy(sb_audit *audit);
bool sb_audit_append(sb_audit *audit, const char *event, const char *actor, const char *id,
                     const char *workspace, const char *target, const char *hash, size_t bytes,
                     const char *tool, const char *operation, const char *original_hash, const char *backup_id);
void sb_audit_free(sb_audit *audit);
bool sb_audit_task(sb_audit *audit, const char *event, const char *actor, const char *key, const char *task_id, sb_execution_profile profile, const char *hash, const char *operation, uint64_t pid);

sb_policy *sb_policy_new(unsigned ttl, sb_audit *audit, sb_error *error);
void sb_policy_free(sb_policy *policy);
yyjson_mut_val *sb_policy_write(sb_host *host, const char *actor, sb_workspace *workspace,
                               const char *path, const char *text, const char *approval_id,
                               yyjson_mut_doc *document, bool *failed);
yyjson_mut_val *sb_policy_modify(sb_host *host, const char *actor, sb_workspace *workspace,
                                const char *path, const sb_edit_request *request,
                                yyjson_mut_doc *document, bool *failed);
char *sb_policy_owner_request(sb_policy *policy, const char *request, size_t length);
sb_owner *sb_owner_start(sb_host *host, sb_error *error);
void sb_owner_stop(sb_owner *owner);
bool sb_owner_stopping(sb_owner *owner);
int sb_owner_command(const char *data_dir, const char *command, const char *id, sb_error *error);

char *sb_base64(const unsigned char *data, size_t length);
bool sb_decode(const unsigned char *data, size_t length, bool end_of_file,
               const char *encoding, bool at_start, char **text, size_t *text_length,
               size_t *consumed, const char **detected, sb_error *error);
yyjson_mut_val *sb_tools_list(yyjson_mut_doc *document);
yyjson_mut_val *sb_search(sb_workspace *workspace, const char *path, const char *query,
                         bool recursive, bool content, size_t maximum, size_t per_file,
                         size_t total_bytes, yyjson_mut_doc *document, sb_error *error);
bool sb_tool_exists(const char *name);
yyjson_mut_val *sb_tool_call(sb_host *host, const char *actor, const char *name, yyjson_val *arguments,
                            yyjson_mut_doc *document);
bool sb_protocol_version(const char *version);
/* A NULL protocol means stateless HTTP. Context contains trusted transport data. */
char *sb_dispatch(sb_host *host, sb_protocol *protocol, const char *request,
                  size_t length, const sb_request_context *context,
                  size_t *response_length, int *http_status);
int sb_stdio_run(sb_host *host);
int sb_http_run(sb_host *host, sb_error *error);

#endif
