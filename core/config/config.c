#include "sb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sb_fail(sb_error *error, const char *message) {
    if (error) snprintf(error->text, sizeof(error->text), "%s", message);
}

char *sb_strdup(const char *value) {
    size_t size = strlen(value) + 1;
    char *result = malloc(size);
    if (result) memcpy(result, value, size);
    return result;
}

const char *sb_string(yyjson_val *value) {
    const char *s = yyjson_get_str(value);
    return s && strlen(s) == yyjson_get_len(value) ? s : NULL;
}

bool sb_integer(yyjson_val *value, uint64_t *number) {
    if (!yyjson_is_int(value) || (yyjson_is_sint(value) && yyjson_get_sint(value) < 0)) return false;
    *number = yyjson_get_uint(value);
    return true;
}

bool sb_identifier(const char *id) {
    if (!id) return false;
    size_t size = strlen(id);
    if (!size || size > 63) return false;
    for (size_t i = 0; i < size; ++i) {
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return true;
}

void sb_config_defaults(sb_config *config) {
    memset(config, 0, sizeof(*config));
    config->port = 8765;
    config->approval_ttl_seconds = 300;
    config->audit_max_bytes = SB_AUDIT_LIMIT;
    strcpy(config->token_env, "SHELLBOT_TOKEN");
    config->execution_profile = SB_EXEC_CONTROLLED;
    config->execution_approval = SB_APPROVAL_ALWAYS;
    config->limits = (sb_resource_limits){8, 32, 262144, 3600000, 64, 512};
}

const char *sb_execution_profile_name(sb_execution_profile profile) {
    return profile == SB_EXEC_USER ? "user" : profile == SB_EXEC_ADMINISTRATOR ? "administrator" : "controlled";
}

sb_workspace *sb_workspace_find(sb_config *config, const char *id) {
    for (size_t i = 0; i < config->workspace_count; ++i)
        if (strcmp(config->workspaces[i].id, id) == 0) return &config->workspaces[i];
    return NULL;
}

bool sb_config_root(sb_config *config, const char *id, const char *path, sb_error *error) {
    if (!sb_identifier(id) || config->workspace_count >= SB_MAX_ROOTS ||
        sb_workspace_find(config, id)) {
        sb_fail(error, "Invalid, duplicate, or excessive workspace id."); return false;
    }
    sb_root *root = sb_platform_root(path, error);
    if (!root) return false;
    sb_workspace *workspace = &config->workspaces[config->workspace_count++];
    strcpy(workspace->id, id);
    workspace->root = root;
    workspace->read_only = true;
    return true;
}

/* Reject unknown settings instead of silently ignoring an intended permission. */
bool sb_config_keys(yyjson_val *object, const char *const *allowed, size_t count) {
    if (!yyjson_is_obj(object)) return false;
    size_t idx, max;
    yyjson_val *key, *value;
    yyjson_obj_foreach(object, idx, max, key, value) {
        (void)value;
        const char *name = sb_string(key);
        bool found = false;
        for (size_t i = 0; name && i < count; ++i) if (!strcmp(name, allowed[i])) found = true;
        if (!found) return false;
        /* Duplicate keys are ambiguous, including in nested configuration. */
        for (size_t j = 0; j < idx; ++j) {
            yyjson_obj_iter iterator = yyjson_obj_iter_with(object);
            yyjson_val *earlier = NULL;
            for (size_t k = 0; k <= j; ++k) earlier = yyjson_obj_iter_next(&iterator);
            if (!strcmp(name, yyjson_get_str(earlier))) return false;
        }
    }
    return true;
}

static bool url_prefix(const char *value, const char *prefix) {
    for (; *prefix; ++value, ++prefix) {
        unsigned char c = (unsigned char)*value;
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c != (unsigned char)*prefix) return false;
    }
    return true;
}

bool sb_config_public_url(sb_config *config, const char *value, sb_error *error) {
    /* Explicit ASCII DNS authority only; URLs never carry paths or credentials.
       Empty configuration leaves the domain to an existing local reverse proxy. */
    if (!value || strlen(value) >= 320) goto invalid;
    if (!*value) {
        *config->public_origin = *config->public_authority = *config->public_default_port = 0;
        return true;
    }
    const char *authority = value, *scheme = "https", *default_port = "443";
    if (url_prefix(value, "https://")) authority += 8;
    else if (url_prefix(value, "http://")) { authority += 7; scheme = "http"; default_port = "80"; }
    size_t length = strlen(authority);
    if (length && authority[length - 1] == '/') --length;
    if (!length || length >= sizeof(config->public_authority)) goto invalid;
    char host[264]; memcpy(host, authority, length); host[length] = 0;
    char *colon = strchr(host, ':');
    if (colon) {
        char *end = NULL;
        const char *digits = colon + 1;
        for (const char *p = digits; *p; ++p) if (*p < '0' || *p > '9') goto invalid;
        unsigned long port = strtoul(digits, &end, 10);
        if (!*digits || !end || *end || strlen(digits) > 5 || port < 1 || port > 65535) goto invalid;
        *colon = 0;
        /* Canonicalize ports, including an explicit default port. */
        if (port != strtoul(default_port, NULL, 10)) snprintf(colon + 1, 6, "%lu", port);
    }
    size_t dns_length = strlen(host);
    if (!dns_length || dns_length > 253) goto invalid;
    size_t label = 0;
    for (size_t i = 0; i <= dns_length; ++i) {
        unsigned char c = (unsigned char)host[i];
        if (c == '.' || !c) {
            if (!label || label > 63 || host[i - 1] == '-') goto invalid;
            label = 0;
        } else {
            if (c >= 'A' && c <= 'Z') host[i] = (char)(c += 'a' - 'A');
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (c == '-' && label))) goto invalid;
            ++label;
        }
    }
    if (colon && strtoul(colon + 1, NULL, 10) != strtoul(default_port, NULL, 10)) *colon = ':';
    int written = snprintf(config->public_origin, sizeof(config->public_origin), "%s://%s", scheme, host);
    if (written < 0 || (size_t)written >= sizeof(config->public_origin)) goto invalid;
    strcpy(config->public_authority, host); strcpy(config->public_default_port, default_port);
    return true;
invalid:
    sb_fail(error, "public_url must be an ASCII domain or http(s) origin, optionally with a port; no path, credentials, query, wildcard, or fragment.");
    return false;
}

bool sb_config_load(sb_config *config, const char *path, sb_error *error) {
    size_t length = 0;
    char *bytes = sb_platform_load_config(path, &length, error);
    if (!bytes) return false;
    bool ok = sb_config_parse(config, bytes, length, error);
    free(bytes);
    return ok;
}

bool sb_config_parse(sb_config *config, const char *bytes, size_t length, sb_error *error) {
    static const char *const top_keys[] = {"roots", "http", "data_dir", "approval_ttl_seconds", "audit_max_bytes", "execution"};
    static const char *const execution_keys[] = {"enabled", "profile", "approval_mode"};
    static const char *const root_keys[] = {"id", "path", "read_only"};
    static const char *const http_keys[] = {"port", "token_env", "public_url"};
    yyjson_doc *document = yyjson_read(bytes, length, 0);
    if (!document) { sb_fail(error, "Cannot read configuration as strict UTF-8 JSON."); return false; }
    yyjson_val *root = yyjson_doc_get_root(document);
    bool ok = false;
    if (yyjson_obj_get(root, "format_version")) {
        sb_fail(error, "Project registry runtime is unavailable in M1; use --validate-projects. Legacy runtime requires an unversioned configuration.");
        goto done;
    }
    if (!sb_config_keys(root, top_keys, 6)) goto invalid;
    yyjson_val *execution = yyjson_obj_get(root, "execution");
    if (execution) {
        if (!sb_config_keys(execution, execution_keys, 3)) goto invalid;
        yyjson_val *enabled = yyjson_obj_get(execution, "enabled");
        if (enabled && !yyjson_is_bool(enabled)) goto invalid;
        config->execution_enabled = yyjson_get_bool(enabled);
        yyjson_val *profile_value = yyjson_obj_get(execution, "profile");
        if (profile_value) {
            const char *profile = sb_string(profile_value);
            if (!profile) goto invalid;
            if (!strcmp(profile, "controlled")) config->execution_profile = SB_EXEC_CONTROLLED;
            else if (!strcmp(profile, "user")) config->execution_profile = SB_EXEC_USER;
            else if (!strcmp(profile, "administrator")) config->execution_profile = SB_EXEC_ADMINISTRATOR;
            else goto invalid;
        }
        yyjson_val *approval_value = yyjson_obj_get(execution, "approval_mode");
        if (approval_value) {
            const char *approval = sb_string(approval_value);
            if (config->execution_profile != SB_EXEC_CONTROLLED || !approval) goto invalid;
            if (!strcmp(approval, "always")) config->execution_approval = SB_APPROVAL_ALWAYS;
            else if (!strcmp(approval, "rules")) config->execution_approval = SB_APPROVAL_RULES;
            else goto invalid;
        }
    }
    yyjson_val *audit_max = yyjson_obj_get(root, "audit_max_bytes");
    if (audit_max && (!sb_integer(audit_max, &config->audit_max_bytes) ||
        config->audit_max_bytes < 4096 || config->audit_max_bytes > 64u * 1024u * 1024u)) goto invalid;
    yyjson_val *ttl = yyjson_obj_get(root, "approval_ttl_seconds");
    uint64_t ttl_number;
    if (ttl) {
        if (!sb_integer(ttl, &ttl_number) || ttl_number < 1 || ttl_number > 600) goto invalid;
        config->approval_ttl_seconds = (unsigned)ttl_number;
    }
    yyjson_val *roots = yyjson_obj_get(root, "roots");
    if (roots && (!yyjson_is_arr(roots) || yyjson_arr_size(roots) > SB_MAX_ROOTS)) goto invalid;
    size_t idx, max;
    yyjson_val *item;
    yyjson_arr_foreach(roots, idx, max, item) {
        if (!sb_config_keys(item, root_keys, 3)) goto invalid;
        const char *id = sb_string(yyjson_obj_get(item, "id"));
        const char *directory = sb_string(yyjson_obj_get(item, "path"));
        yyjson_val *ro = yyjson_obj_get(item, "read_only");
        if (!id || !directory || (ro && !yyjson_is_bool(ro))) goto invalid;
        if (!sb_config_root(config, id, directory, error)) goto done;
        config->workspaces[config->workspace_count - 1].read_only = !ro || yyjson_get_bool(ro);
    }
    yyjson_val *http = yyjson_obj_get(root, "http");
    if (http) {
        if (!sb_config_keys(http, http_keys, 3)) goto invalid;
        yyjson_val *public_url = yyjson_obj_get(http, "public_url");
        if (public_url) {
            const char *url = sb_string(public_url);
            if (!url) goto invalid;
            if (!sb_config_public_url(config, url, error)) goto done;
        }
        yyjson_val *port = yyjson_obj_get(http, "port");
        uint64_t number;
        if (port) {
            if (!sb_integer(port, &number) || number < 1024 || number > 65535) goto invalid;
            config->port = (unsigned)number;
        }
        yyjson_val *env = yyjson_obj_get(http, "token_env");
        if (env) {
            const char *name = sb_string(env);
            if (!name || !sb_identifier(name)) goto invalid;
            strcpy(config->token_env, name);
        }
    }
    yyjson_val *data = yyjson_obj_get(root, "data_dir");
    if (data) {
        const char *directory = sb_string(data);
        if (!directory || !*directory || strlen(directory) >= SB_PATH_CAP) goto invalid;
        config->data_dir = sb_strdup(directory);
        if (!config->data_dir) { sb_fail(error, "Out of memory."); goto done; }
    }
    ok = true;
    goto done;
invalid:
    sb_fail(error, "Invalid configuration or unsupported setting.");
done:
    yyjson_doc_free(document);
    return ok;
}

void sb_config_free(sb_config *config) {
    for (size_t i = 0; i < config->workspace_count; ++i) sb_platform_root_close(config->workspaces[i].root);
    free(config->data_dir);
    memset(config, 0, sizeof(*config));
}
