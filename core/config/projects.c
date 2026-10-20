#include "sb_projects.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define KEYS(object, names) sb_config_keys(object, names, sizeof(names) / sizeof((names)[0]))

char *sb_projects_edit(const char *bytes, size_t length, const char *project_json,
                       size_t project_length, const char *id, bool configure, sb_error *error) {
    sb_project_registry original = {0}, candidate = {0};
    yyjson_doc *source = NULL, *project = NULL;
    yyjson_mut_doc *doc = NULL;
    char *json = NULL;
    if (!sb_identifier(id) || (configure && !bytes)) goto invalid;
    if (bytes && !sb_projects_parse(&original, bytes, length, error)) goto done;
    bool exists = sb_project_find(&original, id) != NULL;
    if (configure != exists) { sb_fail(error, configure ? "Unknown project id." : "Project id already exists."); goto done; }
    project = project_json && project_length <= SB_TEXT_LIMIT ? yyjson_read(project_json, project_length, 0) : NULL;
    yyjson_val *item = yyjson_doc_get_root(project);
    const char *new_id = sb_string(yyjson_obj_get(item, "id"));
    if (!yyjson_is_obj(item) || !new_id || strcmp(new_id, id)) goto invalid;
    source = bytes ? yyjson_read(bytes, length, 0) : NULL;
    if (bytes && !source) goto invalid;
    doc = yyjson_mut_doc_new(NULL);
    if (!doc) goto invalid;
    yyjson_mut_val *root = source ? yyjson_val_mut_copy(doc, yyjson_doc_get_root(source)) : yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_val *projects = source ? yyjson_mut_obj_get(root, "projects") : yyjson_mut_arr(doc);
    if (!source) {
        yyjson_mut_obj_add_uint(doc, root, "format_version", SB_PROJECT_FORMAT);
        yyjson_mut_obj_add_val(doc, root, "projects", projects);
    }
    yyjson_mut_val *replacement = yyjson_val_mut_copy(doc, item);
    if (configure) {
        const sb_project *old = sb_project_find(&original, id);
        if (old->policy_revision == UINT64_MAX) { sb_fail(error, "Policy revision is exhausted."); goto done; }
        /* Validate the source including duplicates before replacing its revision. */
        yyjson_mut_val *probe = yyjson_mut_obj(doc), *array = yyjson_mut_arr(doc);
        yyjson_mut_obj_add_uint(doc, probe, "format_version", SB_PROJECT_FORMAT);
        yyjson_mut_arr_add_val(array, yyjson_val_mut_copy(doc, item));
        yyjson_mut_obj_add_val(doc, probe, "projects", array);
        char *check = yyjson_mut_val_write(probe, 0, NULL);
        bool valid = check && sb_projects_parse(&candidate, check, strlen(check), error);
        free(check); sb_projects_free(&candidate);
        if (!valid) goto done;
        yyjson_mut_obj_remove_key(replacement, "policy_revision");
        yyjson_mut_obj_add_uint(doc, replacement, "policy_revision", old->policy_revision + 1);
        size_t idx = 0;
        for (; idx < original.project_count; ++idx) if (!strcmp(original.projects[idx].id, id)) break;
        yyjson_mut_arr_replace(projects, idx, replacement);
    } else yyjson_mut_arr_add_val(projects, replacement);
    json = yyjson_mut_write(doc, 0, NULL);
    if (!json || !sb_projects_parse(&candidate, json, strlen(json), error)) { free(json); json = NULL; }
    goto done;
invalid:
    sb_fail(error, "Invalid project definition or configuration command.");
done:
    sb_projects_free(&original); sb_projects_free(&candidate);
    yyjson_doc_free(source); yyjson_doc_free(project); yyjson_mut_doc_free(doc);
    return json;
}

char *sb_projects_remove(const char *bytes, size_t length, const char *id, sb_error *error) {
    sb_project_registry registry={0}, candidate={0}; char *json=NULL;
    yyjson_doc *source=NULL; yyjson_mut_doc *doc=NULL;
    if (!sb_identifier(id) || !sb_projects_parse(&registry,bytes,length,error)) goto done;
    if (!sb_project_find(&registry,id)) { sb_fail(error,"Unknown project id."); goto done; }
    if (registry.project_count==1) { sb_fail(error,"Keep at least one project in the registry."); goto done; }
    source=yyjson_read(bytes,length,0); doc=yyjson_mut_doc_new(NULL);
    if (!source || !doc) goto done;
    yyjson_mut_val *root=yyjson_val_mut_copy(doc,yyjson_doc_get_root(source)); yyjson_mut_doc_set_root(doc,root);
    yyjson_mut_val *projects=yyjson_mut_obj_get(root,"projects");
    for (size_t i=0;i<registry.project_count;++i) if (!strcmp(registry.projects[i].id,id)) { yyjson_mut_arr_remove(projects,i); break; }
    if (!strcmp(registry.default_project_id,id)) yyjson_mut_obj_remove_key(yyjson_mut_obj_get(root,"gateway"),"default_project_id");
    json=yyjson_mut_write(doc,0,NULL);
    if (!json || !sb_projects_parse(&candidate,json,strlen(json),error)) { free(json); json=NULL; }
done:
    if (!json && !*error->text) sb_fail(error,"Cannot export the project removal.");
    sb_projects_free(&registry); sb_projects_free(&candidate); yyjson_doc_free(source); yyjson_mut_doc_free(doc); return json;
}

static bool separator(char c) { return c == '/' || c == '\\'; }
static unsigned char folded(char c) {
    if (separator(c)) return '/';
    return (unsigned char)(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
}

bool sb_project_path_valid(const char *path) {
    if (!path || !*path || strlen(path) >= SB_PATH_CAP) return false;
    size_t size = strlen(path), start;
    bool drive = size >= 3 && ((path[0] >= 'a' && path[0] <= 'z') || (path[0] >= 'A' && path[0] <= 'Z')) &&
                 path[1] == ':' && separator(path[2]);
    bool unc = size >= 5 && separator(path[0]) && separator(path[1]);
    if (!drive && !unc) return false;
    start = drive ? 3 : 2;
    unsigned components = 0;
    for (size_t i = start; i <= size; ++i) {
        unsigned char c = (unsigned char)path[i];
        if (c && (c < 32 || strchr(":*?\"<>|", c))) return false;
        if (!c || separator((char)c)) {
            size_t length = i - start;
            if (!length) { if (i != size) return false; }
            else {
                if (path[i - 1] == '.' || path[i - 1] == ' ') return false;
                ++components;
            }
            start = i + 1;
        }
    }
    return drive || components >= 2;
}

/* Syntax-level conflict checks only. M2 must also compare physical directory
   identities and aliases; this function never grants runtime path access. */
static size_t path_length(const char *path) {
    size_t length = strlen(path);
    while (length && separator(path[length - 1])) --length;
    return length;
}

static bool paths_overlap(const char *a, const char *b) {
    size_t al = path_length(a), bl = path_length(b), minimum = al < bl ? al : bl;
    for (size_t i = 0; i < minimum; ++i) if (folded(a[i]) != folded(b[i])) return false;
    return al == bl || separator((al < bl ? b : a)[minimum]);
}

static bool environment_same(const char *a, const char *b) {
    for (; *a && *b; ++a, ++b) if (folded(*a) != folded(*b)) return false;
    return !*a && !*b;
}

static bool bool_field(yyjson_val *object, const char *key, bool fallback, bool *out) {
    yyjson_val *value = yyjson_obj_get(object, key);
    if (value && !yyjson_is_bool(value)) return false;
    *out = value ? yyjson_get_bool(value) : fallback;
    return true;
}

static bool number_field(yyjson_val *object, const char *key, uint64_t fallback, uint64_t min, uint64_t max, uint64_t *out) {
    yyjson_val *value = yyjson_obj_get(object, key);
    *out = fallback;
    return !value || (sb_integer(value, out) && *out >= min && *out <= max);
}

static bool copy_id(char out[64], yyjson_val *value) {
    const char *text = sb_string(value);
    if (!sb_identifier(text)) return false;
    strcpy(out, text);
    return true;
}

const sb_project *sb_project_find(const sb_project_registry *registry, const char *id) {
    for (size_t i = 0; registry && id && i < registry->project_count; ++i)
        if (!strcmp(registry->projects[i].id, id)) return &registry->projects[i];
    return NULL;
}

const sb_project_directory *sb_project_directory_find(const sb_project *project, const char *id) {
    for (size_t i = 0; project && id && i < project->directory_count; ++i)
        if (!strcmp(project->directories[i].id, id)) return &project->directories[i];
    return NULL;
}

static bool directories(sb_project *project, yyjson_val *array, bool workspace) {
    static const char *const allowed[] = {"id", "path", "read_only"};
    if (!array) return true;
    if (!yyjson_is_arr(array) || yyjson_arr_size(array) > SB_MAX_ROOTS) return false;
    size_t idx, max;
    yyjson_val *item;
    yyjson_arr_foreach(array, idx, max, item) {
        if (!KEYS(item, allowed) || project->directory_count >= SB_MAX_PROJECT_DIRECTORIES) return false;
        sb_project_directory *directory = &project->directories[project->directory_count];
        if (!copy_id(directory->id, yyjson_obj_get(item, "id")) || sb_project_directory_find(project, directory->id)) return false;
        const char *path = sb_string(yyjson_obj_get(item, "path"));
        if (!sb_project_path_valid(path) || !bool_field(item, "read_only", true, &directory->read_only)) return false;
        for (size_t j = 0; j < project->directory_count; ++j)
            if (paths_overlap(project->directories[j].path, path)) return false;
        directory->path = sb_strdup(path);
        if (!directory->path) return false;
        directory->workspace = workspace;
        ++project->directory_count;
    }
    return true;
}

static bool policy_parse(sb_project_policy *policy, yyjson_val *value) {
    static const char *const allowed[] = {"approval_mode", "files", "execution", "tools", "desktop"};
    static const char *const file_keys[] = {"scope", "current_user_read_only"};
    static const char *const execution_keys[] = {"enabled", "default_identity", "administrator_allowed", "runtime_files"};
    policy->approval_mode = SB_PROJECT_ALWAYS;
    policy->file_scope = SB_FILES_CONFIGURED;
    policy->current_user_read_only = true;
    policy->default_identity = SB_ID_RESTRICTED;
    policy->desktop.lease_seconds = 30;
    policy->desktop.emergency_function_key = 9;
    policy->tools = (UINT32_C(1) << 6) - 1; /* Metadata and bounded file reading. */
    if (!value) return true;
    if (!KEYS(value, allowed)) return false;
    yyjson_val *mode = yyjson_obj_get(value, "approval_mode");
    if (mode) {
        const char *text = sb_string(mode);
        if (!text) return false;
        if (!strcmp(text, "always")) policy->approval_mode = SB_PROJECT_ALWAYS;
        else if (!strcmp(text, "workspace_auto")) policy->approval_mode = SB_PROJECT_WORKSPACE_AUTO;
        else if (!strcmp(text, "never")) policy->approval_mode = SB_PROJECT_NEVER;
        else return false;
    }
    yyjson_val *files = yyjson_obj_get(value, "files");
    if (files) {
        if (!KEYS(files, file_keys) || !bool_field(files, "current_user_read_only", true, &policy->current_user_read_only)) return false;
        yyjson_val *scope = yyjson_obj_get(files, "scope");
        if (scope) {
            const char *text = sb_string(scope);
            if (!text) return false;
            if (!strcmp(text, "configured")) policy->file_scope = SB_FILES_CONFIGURED;
            else if (!strcmp(text, "current_user")) policy->file_scope = SB_FILES_CURRENT_USER;
            else return false;
        }
        if (policy->file_scope != SB_FILES_CURRENT_USER && yyjson_obj_get(files, "current_user_read_only")) return false;
    }
    yyjson_val *execution = yyjson_obj_get(value, "execution");
    if (execution) {
        if (!KEYS(execution, execution_keys) || !bool_field(execution, "enabled", false, &policy->execution_enabled) ||
            !bool_field(execution, "administrator_allowed", false, &policy->administrator_allowed)) return false;
        yyjson_val *identity = yyjson_obj_get(execution, "default_identity");
        if (identity) {
            const char *text = sb_string(identity);
            if (!text) return false;
            if (!strcmp(text, "restricted")) policy->default_identity = SB_ID_RESTRICTED;
            else if (!strcmp(text, "current_user")) policy->default_identity = SB_ID_CURRENT_USER;
            else return false; /* Administrator always requires explicit operation selection. */
        }
        if (policy->administrator_allowed && (policy->file_scope != SB_FILES_CURRENT_USER || policy->default_identity == SB_ID_RESTRICTED)) return false;
        yyjson_val *runtime = yyjson_obj_get(execution,"runtime_files");
        if (runtime) {
            if (policy->default_identity != SB_ID_RESTRICTED || !yyjson_is_arr(runtime) || yyjson_arr_size(runtime)>16) return false;
            size_t i,n; yyjson_val *file;
            yyjson_arr_foreach(runtime,i,n,file) {
                const char *path = sb_string(file);
                if (!sb_project_path_valid(path)) return false;
                for(size_t j=0;j<policy->runtime_file_count;++j) if(!strcmp(policy->runtime_files[j],path))return false;
                char *copy = sb_strdup(path); if(!copy)return false;
                policy->runtime_files[policy->runtime_file_count++]=copy;
            }
        }
    }
    yyjson_val *desktop = yyjson_obj_get(value, "desktop");
    if (desktop) {
        static const char *const desktop_keys[] = {"enabled", "control", "clipboard_read", "clipboard_write", "lease_seconds", "emergency_key"};
        uint64_t seconds;
        if (!KEYS(desktop, desktop_keys) || !bool_field(desktop,"enabled",false,&policy->desktop.enabled) ||
            !bool_field(desktop,"control",false,&policy->desktop.control) ||
            !bool_field(desktop,"clipboard_read",false,&policy->desktop.clipboard_read) ||
            !bool_field(desktop,"clipboard_write",false,&policy->desktop.clipboard_write) ||
            !number_field(desktop,"lease_seconds",30,1,120,&seconds)) return false;
        policy->desktop.lease_seconds = (unsigned)seconds;
        if (!policy->desktop.enabled && (policy->desktop.control || policy->desktop.clipboard_read || policy->desktop.clipboard_write)) return false;
        yyjson_val *key = yyjson_obj_get(desktop,"emergency_key");
        if (key) {
            const char *text = sb_string(key); bool valid = false;
            for (unsigned i=1; text && i<=11; ++i) {
                char expected[4]; snprintf(expected,sizeof(expected),"F%u",i);
                if (!strcmp(expected,text)) { policy->desktop.emergency_function_key=i; valid=true; }
            }
            if (!valid) return false;
        }
    }
    yyjson_val *tools = yyjson_obj_get(value, "tools");
    if (tools) {
        if (!yyjson_is_arr(tools) || yyjson_arr_size(tools) > SB_PROJECT_TOOL_COUNT) return false;
        policy->tools = 0;
        size_t idx, max; yyjson_val *item;
        yyjson_arr_foreach(tools, idx, max, item) {
            int index = sb_project_tool_index(sb_string(item));
            if (index < 0 || (policy->tools & (UINT64_C(1) << index))) return false;
            policy->tools |= UINT64_C(1) << index;
        }
    }
    return true;
}

static bool legacy_parse(sb_project *project, yyjson_val *value, sb_error *error) {
    char *json = yyjson_val_write(value, 0, NULL);
    sb_config config;
    sb_config_defaults(&config);
    bool ok = json && sb_config_parse(&config, json, strlen(json), error);
    free(json);
    if (!ok) { sb_config_free(&config); return false; }
    project->legacy_compatibility = true;
    project->legacy_profile = config.execution_profile;
    project->legacy_approval = config.execution_approval;
    strcpy(project->token_env, config.token_env);
    project->policy.file_scope = SB_FILES_CONFIGURED;
    project->policy.current_user_read_only = true;
    project->policy.approval_mode = SB_PROJECT_ALWAYS; /* Overridden only by explicit legacy contract. */
    project->policy.default_identity = config.execution_profile == SB_EXEC_CONTROLLED ? SB_ID_RESTRICTED :
        config.execution_profile == SB_EXEC_ADMINISTRATOR ? SB_ID_ADMINISTRATOR : SB_ID_CURRENT_USER;
    project->policy.execution_enabled = config.execution_enabled;
    project->policy.administrator_allowed = config.execution_enabled && config.execution_profile == SB_EXEC_ADMINISTRATOR;
    project->policy.tools = UINT32_MAX;
    project->approval_ttl_seconds = config.approval_ttl_seconds;
    project->audit_max_bytes = config.audit_max_bytes;
    yyjson_val *roots = yyjson_obj_get(value, "roots");
    for (size_t i = 0; i < config.workspace_count; ++i) {
        sb_project_directory *directory = &project->directories[i];
        strcpy(directory->id, config.workspaces[i].id);
        directory->read_only = config.workspaces[i].read_only;
        directory->workspace = true;
        directory->path = sb_strdup(sb_string(yyjson_obj_get(yyjson_arr_get(roots, i), "path")));
        if (!directory->path) { ok = false; break; }
        ++project->directory_count;
    }
    if (project->directory_count) strcpy(project->default_workspace_id, project->directories[0].id);
    sb_config_free(&config);
    return ok;
}

static bool project_parse(sb_project *project, yyjson_val *value, sb_error *error) {
    static const char *const allowed[] = {"id", "policy_revision", "enabled", "token_env", "data_dir", "default_workspace_id",
        "workspaces", "extra_directories", "policy", "approval_ttl_seconds", "audit_max_bytes", "legacy_config", "resources"};
    static const char *const legacy_keys[] = {"id", "policy_revision", "enabled", "data_dir", "legacy_config"};
    uint64_t number;
    project->limits = (sb_resource_limits){8, 32, 262144, 3600000, 64, 512};
    if (!KEYS(value, allowed) || !copy_id(project->id, yyjson_obj_get(value, "id")) ||
        !bool_field(value, "enabled", true, &project->enabled) ||
        !number_field(value, "policy_revision", 1, 1, UINT64_MAX, &project->policy_revision)) return false;
    const char *data = sb_string(yyjson_obj_get(value, "data_dir"));
    if (!sb_project_path_valid(data)) return false;
    project->data_dir = sb_strdup(data);
    if (!project->data_dir) return false;
    yyjson_val *legacy = yyjson_obj_get(value, "legacy_config");
    if (legacy) {
        if (!KEYS(value, legacy_keys) || !legacy_parse(project, legacy, error)) return false;
        const char *old_data = sb_string(yyjson_obj_get(legacy, "data_dir"));
        if (old_data && paths_overlap(old_data, data)) return false;
        /* No implicit reuse of the historical default Data directory. */
        if (!old_data) {
            char *old_default = sb_platform_default_data_dir(error);
            bool overlap = !old_default || paths_overlap(old_default, data);
            free(old_default);
            if (overlap) return false;
        }
        return true;
    }
    if (!copy_id(project->token_env, yyjson_obj_get(value, "token_env")) ||
        !directories(project, yyjson_obj_get(value, "workspaces"), true) ||
        !directories(project, yyjson_obj_get(value, "extra_directories"), false) ||
        !policy_parse(&project->policy, yyjson_obj_get(value, "policy")) ||
        !number_field(value, "approval_ttl_seconds", 300, 1, 600, &number)) return false;
    project->approval_ttl_seconds = (unsigned)number;
    if (!number_field(value, "audit_max_bytes", SB_AUDIT_LIMIT, 4096, 64u * 1024u * 1024u, &project->audit_max_bytes)) return false;
    yyjson_val *resources = yyjson_obj_get(value, "resources");
    if (resources) {
        static const char *const keys[] = {"max_active_tasks", "max_task_records", "max_output_bytes", "max_timeout_ms", "max_approval_records", "worker_memory_mb"};
        unsigned *fields[] = {&project->limits.active_tasks, &project->limits.task_records, &project->limits.output_bytes,
            &project->limits.task_timeout_ms, &project->limits.approval_records, &project->limits.worker_memory_mb};
        static const unsigned minimum[] = {1, 1, 1024, 100, 1, 16};
        static const unsigned maximum[] = {8, 32, 262144, 3600000, 64, 4096};
        if (!KEYS(resources, keys)) return false;
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
            if (!number_field(resources, keys[i], *fields[i], minimum[i], maximum[i], &number)) return false;
            *fields[i] = (unsigned)number;
        }
        if (project->limits.task_records < project->limits.active_tasks) return false;
    }
    yyjson_val *cwd = yyjson_obj_get(value, "default_workspace_id");
    if (cwd) {
        if (!copy_id(project->default_workspace_id, cwd)) return false;
        const sb_project_directory *directory = sb_project_directory_find(project, project->default_workspace_id);
        if (!directory || !directory->workspace) return false;
    } else {
        for (size_t i = 0; i < project->directory_count; ++i)
            if (project->directories[i].workspace) { strcpy(project->default_workspace_id, project->directories[i].id); break; }
    }
    return true;
}

void sb_projects_free(sb_project_registry *registry) {
    if (!registry) return;
    for (size_t i = 0; i < registry->project_count; ++i) {
        sb_project *project = &registry->projects[i];
        for (size_t j = 0; j < project->directory_count; ++j) free(project->directories[j].path);
        free(project->data_dir);
        for(size_t j=0;j<project->policy.runtime_file_count;++j) free(project->policy.runtime_files[j]);
    }
    memset(registry, 0, sizeof(*registry));
}

bool sb_projects_parse(sb_project_registry *registry, const char *bytes, size_t length, sb_error *error) {
    static const char *const allowed[] = {"format_version", "projects", "gateway"};
    static const char *const gateway_keys[] = {"port", "public_url", "default_project_id", "owner_token_env"};
    if (!registry || registry->project_count) { sb_fail(error, "Registry must be initialized and empty."); return false; }
    yyjson_doc *doc = bytes && length <= SB_TEXT_LIMIT ? yyjson_read(bytes, length, 0) : NULL;
    sb_project_registry *candidate = calloc(1, sizeof(*candidate));
    bool ok = false;
    if (!doc || !candidate) goto done;
    yyjson_val *root = yyjson_doc_get_root(doc);
    uint64_t number;
    if (!KEYS(root, allowed) || !sb_integer(yyjson_obj_get(root, "format_version"), &number) || number != SB_PROJECT_FORMAT) goto done;
    candidate->format_version = (unsigned)number;
    candidate->port = 8765;
    yyjson_val *projects = yyjson_obj_get(root, "projects");
    if (!yyjson_is_arr(projects) || yyjson_arr_size(projects) > SB_MAX_PROJECTS) goto done;
    size_t idx, max; yyjson_val *item;
    yyjson_arr_foreach(projects, idx, max, item) {
        sb_project *project = &candidate->projects[candidate->project_count++];
        if (!project_parse(project, item, error)) goto done;
        for (size_t j = 0; j + 1 < candidate->project_count; ++j) {
            const sb_project *prior = &candidate->projects[j];
            if (!strcmp(project->id, prior->id) || environment_same(project->token_env, prior->token_env) || paths_overlap(project->data_dir, prior->data_dir)) goto done;
        }
    }
    yyjson_val *gateway = yyjson_obj_get(root, "gateway");
    if (gateway) {
        if (!KEYS(gateway, gateway_keys) || !number_field(gateway, "port", 8765, 1024, 65535, &number)) goto done;
        candidate->port = (unsigned)number;
        yyjson_val *owner=yyjson_obj_get(gateway,"owner_token_env");
        if (owner) {
            if (!copy_id(candidate->owner_token_env,owner)) goto done;
            for (size_t i=0;i<candidate->project_count;++i) if (environment_same(candidate->owner_token_env,candidate->projects[i].token_env)) goto done;
        }
        yyjson_val *url = yyjson_obj_get(gateway, "public_url");
        if (url) {
            sb_config config; sb_config_defaults(&config);
            if (!sb_config_public_url(&config, sb_string(url), error)) goto done;
            strcpy(candidate->public_origin, config.public_origin);
        }
        yyjson_val *default_id = yyjson_obj_get(gateway, "default_project_id");
        if (default_id && (!copy_id(candidate->default_project_id, default_id) || !sb_project_find(candidate, candidate->default_project_id))) goto done;
    }
    *registry = *candidate;
    memset(candidate, 0, sizeof(*candidate));
    ok = true;
done:
    if (!ok && error && !*error->text) sb_fail(error, "Invalid project registry: version, duplicate/unknown fields, scope, identity, directories or project references.");
    sb_projects_free(candidate); free(candidate); yyjson_doc_free(doc);
    return ok;
}

bool sb_projects_load(sb_project_registry *registry, const char *path, sb_error *error) {
    size_t length = 0;
    char *bytes = sb_platform_load_config(path, &length, error);
    if (!bytes) return false;
    bool ok = sb_projects_parse(registry, bytes, length, error);
    free(bytes);
    return ok;
}

char *sb_projects_summary(const sb_project_registry *registry) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return NULL;
    yyjson_mut_val *root = yyjson_mut_obj(doc), *projects = yyjson_mut_arr(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_uint(doc, root, "format_version", registry->format_version);
    yyjson_mut_obj_add_bool(doc, root, "runtime_available", true);
    yyjson_mut_obj_add_bool(doc, root, "runtime_started", false);
    yyjson_mut_obj_add_bool(doc, root, "http_gateway_available", true);
    yyjson_mut_obj_add_bool(doc, root, "remote_owner_configured", *registry->owner_token_env!=0);
    yyjson_mut_obj_add_val(doc, root, "projects", projects);
    for (size_t i = 0; i < registry->project_count; ++i) {
        const sb_project *project = &registry->projects[i];
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, item, "project_id", project->id);
        yyjson_mut_obj_add_uint(doc, item, "policy_revision", project->policy_revision);
        yyjson_mut_obj_add_bool(doc, item, "enabled", project->enabled);
        yyjson_mut_obj_add_bool(doc, item, "legacy_compatibility", project->legacy_compatibility);
        yyjson_mut_obj_add_str(doc, item, "approval_mode", project->legacy_compatibility ? "dev7_compatibility" :
            project->policy.approval_mode == SB_PROJECT_ALWAYS ? "always" : project->policy.approval_mode == SB_PROJECT_WORKSPACE_AUTO ? "workspace_auto" : "never");
        yyjson_mut_obj_add_uint(doc, item, "directory_count", project->directory_count);
        yyjson_mut_obj_add_str(doc, item, "file_scope", project->policy.file_scope == SB_FILES_CONFIGURED ? "configured" : "current_user");
        yyjson_mut_obj_add_bool(doc, item, "administrator_allowed", project->policy.administrator_allowed);
        yyjson_mut_obj_add_bool(doc, item, "desktop_enabled", project->policy.desktop.enabled);
        yyjson_mut_obj_add_bool(doc, item, "desktop_control", project->policy.desktop.control);
        yyjson_mut_obj_add_bool(doc, item, "clipboard_read", project->policy.desktop.clipboard_read);
        yyjson_mut_obj_add_bool(doc, item, "clipboard_write", project->policy.desktop.clipboard_write);
        yyjson_mut_obj_add_str(doc, item, "default_execution_identity", project->policy.default_identity == SB_ID_RESTRICTED ? "restricted" :
            project->policy.default_identity == SB_ID_CURRENT_USER ? "current_user" : "administrator");
        const sb_request_context context = {.actor = "configuration-status", .project = project, .policy_revision = project->policy_revision};
        const sb_project_operation operation = {.tool = "task_start", .directory = sb_project_directory_find(project, project->default_workspace_id),
            .identity = project->policy.default_identity};
        sb_policy_result command = sb_project_evaluate(&context, &operation);
        yyjson_mut_obj_add_str(doc, item, "default_command_policy_decision", command.decision == SB_DECISION_DENY ? "deny" :
            command.decision == SB_DECISION_ALLOW ? "allow" : "pending");
        yyjson_mut_obj_add_str(doc, item, "default_command_policy_reason", command.reason);
        yyjson_mut_obj_add_bool(doc, item, "restricted_backend_verified", false);
        yyjson_mut_obj_add_bool(doc, item, "project_administrator_backend_available", false);
        yyjson_mut_arr_add_val(projects, item);
    }
    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    return json;
}

char *sb_projects_migrate(const char *bytes, size_t length, const char *project_id, const char *new_data_dir, sb_error *error) {
    if (!sb_identifier(project_id) || !sb_project_path_valid(new_data_dir)) { sb_fail(error, "Migration requires a valid project id and a new absolute project data directory."); return NULL; }
    yyjson_doc *legacy = bytes && length <= SB_TEXT_LIMIT ? yyjson_read(bytes, length, 0) : NULL;
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    char *json = NULL;
    if (!legacy || !doc) goto done;
    yyjson_mut_val *root = yyjson_mut_obj(doc), *array = yyjson_mut_arr(doc), *project = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_uint(doc, root, "format_version", SB_PROJECT_FORMAT);
    yyjson_mut_obj_add_val(doc, root, "projects", array);
    yyjson_mut_arr_add_val(array, project);
    yyjson_mut_obj_add_strcpy(doc, project, "id", project_id);
    yyjson_mut_obj_add_uint(doc, project, "policy_revision", 1);
    yyjson_mut_obj_add_strcpy(doc, project, "data_dir", new_data_dir);
    yyjson_mut_obj_add_val(doc, project, "legacy_config", yyjson_val_mut_copy(doc, yyjson_doc_get_root(legacy)));
    yyjson_val *old_http = yyjson_obj_get(yyjson_doc_get_root(legacy), "http");
    yyjson_mut_val *gateway = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, gateway, "default_project_id", project_id);
    if (old_http) {
        yyjson_val *port = yyjson_obj_get(old_http, "port"), *url = yyjson_obj_get(old_http, "public_url");
        if (port) yyjson_mut_obj_add_val(doc, gateway, "port", yyjson_val_mut_copy(doc, port));
        if (url) yyjson_mut_obj_add_val(doc, gateway, "public_url", yyjson_val_mut_copy(doc, url));
    }
    yyjson_mut_obj_add_val(doc, root, "gateway", gateway);
    json = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY, NULL);
    sb_project_registry registry = {0};
    if (!json || !sb_projects_parse(&registry, json, strlen(json), error)) { free(json); json = NULL; }
    sb_projects_free(&registry);
done:
    yyjson_doc_free(legacy); yyjson_mut_doc_free(doc);
    if (!json && error && !*error->text) sb_fail(error, "Cannot migrate invalid legacy configuration.");
    return json;
}
