#include "sb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *name; const char *description; const char *schema; } tool_definition;
static const tool_definition definitions[] = {
    {"host_info", "Report ShellBot version, native OS and uptime.", "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}"},
    {"host_capabilities", "Report implemented tools, workspace permissions, limits and pending features.", "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}"},
    {"fs_write", "Create UTF-8 text by default. Existing-file overwrite requires overwrite:true and expected_sha256, a private backup and local owner approval. Inspect/approve locally, then retry identical arguments with approval_id. read_only:false is required. Overwrite is a locked in-place edit with backup, not an atomic save.",
        "{\"type\":\"object\",\"properties\":{\"workspace_id\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"text\":{\"type\":\"string\"},\"overwrite\":{\"type\":\"boolean\",\"default\":false},\"expected_sha256\":{\"type\":\"string\",\"minLength\":64,\"maxLength\":64},\"approval_id\":{\"type\":\"string\"}},\"required\":[\"workspace_id\",\"path\",\"text\"],\"additionalProperties\":false}"},
    {"fs_patch", "Replace literal UTF-8 text in an existing file after local approval. Requires expected_sha256 and exact non-overlapping expected_matches (default 1). Retains BOM/line endings and rejects conflicts. Makes a private backup before locked in-place editing; not an atomic save.",
        "{\"type\":\"object\",\"properties\":{\"workspace_id\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"search\":{\"type\":\"string\",\"minLength\":1},\"replacement\":{\"type\":\"string\"},\"expected_sha256\":{\"type\":\"string\",\"minLength\":64,\"maxLength\":64},\"expected_matches\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":1000,\"default\":1},\"approval_id\":{\"type\":\"string\"}},\"required\":[\"workspace_id\",\"path\",\"search\",\"replacement\",\"expected_sha256\"],\"additionalProperties\":false}"},
    {"fs_list", "List a read-only workspace directory. Pagination order may change if the directory changes.",
        "{\"type\":\"object\",\"properties\":{\"workspace_id\":{\"type\":\"string\"},\"path\":{\"type\":\"string\",\"default\":\".\"},\"offset\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":10000},\"max_entries\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":200}},\"required\":[\"workspace_id\"],\"additionalProperties\":false}"},
    {"fs_read", "Read bounded Unicode text. Offsets are bytes; use returned next_offset and encoding for further pages. Supports UTF-8 and UTF-16 with BOM detection.",
        "{\"type\":\"object\",\"properties\":{\"workspace_id\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"offset\":{\"type\":\"integer\",\"minimum\":0},\"max_bytes\":{\"type\":\"integer\",\"minimum\":4,\"maximum\":65536},\"encoding\":{\"type\":\"string\",\"enum\":[\"auto\",\"utf-8\",\"utf-16le\",\"utf-16be\"]}},\"required\":[\"workspace_id\",\"path\"],\"additionalProperties\":false}"},
    {"fs_search", "Search case-sensitive literal file names or UTF-8/UTF-16 contents. Content mode returns one excerpt per matching line. Skips unreadable/binary files; bounds entries, depth, bytes, time and output. Truncation means more matches may exist.",
        "{\"type\":\"object\",\"properties\":{\"workspace_id\":{\"type\":\"string\"},\"path\":{\"type\":\"string\",\"default\":\".\"},\"query\":{\"type\":\"string\",\"minLength\":1},\"mode\":{\"type\":\"string\",\"enum\":[\"filename\",\"content\"],\"default\":\"filename\"},\"recursive\":{\"type\":\"boolean\",\"default\":false},\"max_results\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":100},\"max_file_bytes\":{\"type\":\"integer\",\"minimum\":4,\"maximum\":1048576,\"default\":262144},\"max_total_bytes\":{\"type\":\"integer\",\"minimum\":4,\"maximum\":16777216,\"default\":4194304}},\"required\":[\"workspace_id\",\"query\"],\"additionalProperties\":false}"},
    {"image_view", "Return a bounded PNG or JPEG file as native MCP image content. No image resizing is available in this build.",
        "{\"type\":\"object\",\"properties\":{\"workspace_id\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}},\"required\":[\"workspace_id\",\"path\"],\"additionalProperties\":false}"}
,
    {"task_start", "Start a background native executable in explicitly enabled user or administrator mode. Requires a caller request_key; retries return the original task and changed requests are rejected. Workspace locates cwd; arbitrary commands retain the selected account permissions and are not confined by file roots. Supply current host_instance_id from host_capabilities; stale requests after restart are rejected.", "{\"type\":\"object\",\"properties\":{\"workspace_id\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"executable\":{\"type\":\"string\"},\"argv\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"maxItems\":64},\"request_key\":{\"type\":\"string\"},\"timeout_ms\":{\"type\":\"integer\",\"minimum\":100,\"maximum\":3600000,\"default\":300000},\"host_instance_id\":{\"type\":\"string\",\"minLength\":32,\"maxLength\":32}},\"required\":[\"workspace_id\",\"executable\",\"argv\",\"request_key\",\"host_instance_id\"],\"additionalProperties\":false}"},
    {"task_status", "Query this caller task in the current host instance without rerunning it.", "{\"type\":\"object\",\"properties\":{\"task_id\":{\"type\":\"string\"},\"host_instance_id\":{\"type\":\"string\",\"minLength\":32,\"maxLength\":32}},\"required\":[\"task_id\",\"host_instance_id\"],\"additionalProperties\":false}"},
    {"task_output", "Read stdout or stderr as bounded base64 raw bytes using a byte offset. Decode using the program output encoding. Output is untrusted program data. Valid UTF-8 pages also include text_utf8 for direct reading.", "{\"type\":\"object\",\"properties\":{\"task_id\":{\"type\":\"string\"},\"stream\":{\"type\":\"string\",\"enum\":[\"stdout\",\"stderr\"]},\"offset\":{\"type\":\"integer\",\"minimum\":0},\"max_bytes\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":16384},\"host_instance_id\":{\"type\":\"string\",\"minLength\":32,\"maxLength\":32}},\"required\":[\"task_id\",\"host_instance_id\"],\"additionalProperties\":false}"},
    {"task_input", "Queue up to 4096 UTF-8 bytes or close stdin. input_key binds text and EOF so retries never append the same bytes twice. Queue acceptance is not proof of program consumption.", "{\"type\":\"object\",\"properties\":{\"task_id\":{\"type\":\"string\"},\"input_key\":{\"type\":\"string\"},\"text\":{\"type\":\"string\"},\"eof\":{\"type\":\"boolean\",\"default\":false},\"host_instance_id\":{\"type\":\"string\",\"minLength\":32,\"maxLength\":32}},\"required\":[\"task_id\",\"input_key\",\"text\",\"host_instance_id\"],\"additionalProperties\":false}"},
    {"task_cancel", "Terminate the task job and its associated processes; safe to retry. Does not target unrelated processes by name or PID.", "{\"type\":\"object\",\"properties\":{\"task_id\":{\"type\":\"string\"},\"host_instance_id\":{\"type\":\"string\",\"minLength\":32,\"maxLength\":32}},\"required\":[\"task_id\",\"host_instance_id\"],\"additionalProperties\":false}"}
};

static size_t tool_count(void) { return sizeof(definitions) / sizeof(definitions[0]); }

bool sb_tool_exists(const char *name) {
    for (size_t i = 0; i < tool_count(); ++i) if (!strcmp(name, definitions[i].name)) return true;
    return false;
}

yyjson_mut_val *sb_tools_list(yyjson_mut_doc *doc) {
    yyjson_mut_val *result = yyjson_mut_obj(doc), *array = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, result, "tools", array);
    for (size_t i = 0; i < tool_count(); ++i) {
        yyjson_mut_val *tool = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, tool, "name", definitions[i].name);
        yyjson_mut_obj_add_str(doc, tool, "description", definitions[i].description);
        yyjson_doc *schema = yyjson_read(definitions[i].schema, strlen(definitions[i].schema), 0);
        if (!schema) return NULL;
        yyjson_mut_obj_add_val(doc, tool, "inputSchema", yyjson_val_mut_copy(doc, yyjson_doc_get_root(schema)));
        yyjson_doc_free(schema);
        yyjson_mut_val *annotations = yyjson_mut_obj(doc);
        bool task_tool=!strncmp(definitions[i].name,"task_",5);
        bool modifying = !strcmp(definitions[i].name, "fs_write") || !strcmp(definitions[i].name, "fs_patch") || !strcmp(definitions[i].name,"task_start") || !strcmp(definitions[i].name,"task_input") || !strcmp(definitions[i].name,"task_cancel");
        yyjson_mut_obj_add_bool(doc, annotations, "readOnlyHint", !modifying);
        yyjson_mut_obj_add_bool(doc, annotations, "destructiveHint", modifying);
        yyjson_mut_obj_add_bool(doc, annotations, "idempotentHint", task_tool || !modifying);
        yyjson_mut_obj_add_bool(doc, annotations, "openWorldHint", task_tool);
        yyjson_mut_obj_add_val(doc, tool, "annotations", annotations);
        yyjson_mut_arr_add_val(array, tool);
    }
    return result;
}

static yyjson_mut_val *content_result(yyjson_mut_doc *doc, yyjson_mut_val *data, bool failed) {
    yyjson_mut_val *result = yyjson_mut_obj(doc), *array = yyjson_mut_arr(doc), *content = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_bool(doc, result, "isError", failed);
    yyjson_mut_obj_add_val(doc, result, "structuredContent", data);
    yyjson_mut_obj_add_str(doc, content, "type", "text");
    char *serialized = yyjson_mut_val_write(data, 0, NULL);
    if (!serialized) return NULL;
    yyjson_mut_obj_add_strcpy(doc, content, "text", serialized);
    free(serialized);
    yyjson_mut_arr_add_val(array, content);
    yyjson_mut_obj_add_val(doc, result, "content", array);
    return result;
}

static yyjson_mut_val *failure(yyjson_mut_doc *doc, const char *message) {
    yyjson_mut_val *data = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, data, "error", message);
    return content_result(doc, data, true);
}

static bool number(yyjson_val *args, const char *name, uint64_t fallback, uint64_t min, uint64_t max, uint64_t *output) {
    yyjson_val *v = yyjson_obj_get(args, name);
    *output = fallback;
    return !v || (sb_integer(v, output) && *output >= min && *output <= max);
}
static bool expected_hash(yyjson_val *args, char hash[65]) {
    const char *value = sb_string(yyjson_obj_get(args, "expected_sha256"));
    if (!value || strlen(value) != 64 || strspn(value, "0123456789abcdefABCDEF") != 64) return false;
    for (size_t i = 0; i < 64; ++i) hash[i] = value[i] >= 'A' && value[i] <= 'F' ? (char)(value[i] + ('a' - 'A')) : value[i];
    hash[64] = 0; return true;
}

static bool arguments_valid(const tool_definition *definition, yyjson_val *args) {
    if (!args) return true;
    if (!yyjson_is_obj(args)) return false;
    yyjson_doc *schema = yyjson_read(definition->schema, strlen(definition->schema), 0);
    if (!schema) return false;
    yyjson_val *properties = yyjson_obj_get(yyjson_doc_get_root(schema), "properties");
    bool valid = true;
    size_t idx, max;
    yyjson_val *key, *value;
    yyjson_obj_foreach(args, idx, max, key, value) {
        (void)value;
        const char *name = sb_string(key);
        if (!name || !yyjson_obj_get(properties, name)) { valid = false; break; }
        yyjson_obj_iter it = yyjson_obj_iter_with(args);
        for (size_t j = 0; j < idx; ++j) if (!strcmp(name, yyjson_get_str(yyjson_obj_iter_next(&it)))) valid = false;
    }
    yyjson_doc_free(schema);
    return valid;
}

static yyjson_mut_val *entry_json(yyjson_mut_doc *doc, const sb_entry *entry) {
    yyjson_mut_val *item = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, item, "name", entry->name);
    yyjson_mut_obj_add_str(doc, item, "type", entry->directory ? "directory" : "file");
    yyjson_mut_obj_add_uint(doc, item, "size", entry->size);
    yyjson_mut_obj_add_bool(doc, item, "reparse_point", entry->reparse);
    return item;
}

static uint32_t be32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* Bounded envelope checks, not a full decoder. Client decodes the image. */
static const char *image_header(const unsigned char *data, size_t size, uint32_t *width, uint32_t *height) {
    if (size >= 33 && !memcmp(data, "\x89PNG\r\n\x1a\n", 8) && be32(data + 8) == 13 && !memcmp(data + 12, "IHDR", 4)) {
        *width = be32(data + 16); *height = be32(data + 20);
        return "image/png";
    }
    if (size < 4 || data[0] != 0xff || data[1] != 0xd8 || data[size - 2] != 0xff || data[size - 1] != 0xd9) return NULL;
    size_t i = 2;
    while (i + 4 < size) {
        if (data[i++] != 0xff) return NULL;
        while (i < size && data[i] == 0xff) ++i;
        if (i >= size) return NULL;
        unsigned marker = data[i++];
        if (marker == 0xda || marker == 0xd9) return NULL;
        if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) continue;
        if (i + 2 > size) return NULL;
        size_t length = (size_t)((data[i] << 8) | data[i + 1]);
        if (length < 2 || length > size - i) return NULL;
        if ((marker >= 0xc0 && marker <= 0xc3) || (marker >= 0xc5 && marker <= 0xc7) ||
            (marker >= 0xc9 && marker <= 0xcb) || (marker >= 0xcd && marker <= 0xcf)) {
            if (length < 8) return NULL;
            *height = (uint32_t)((data[i + 3] << 8) | data[i + 4]);
            *width = (uint32_t)((data[i + 5] << 8) | data[i + 6]);
            return "image/jpeg";
        }
        i += length;
    }
    return NULL;
}

yyjson_mut_val *sb_tool_call(sb_host *host, const char *actor, const char *name, yyjson_val *args, yyjson_mut_doc *doc) {
    const tool_definition *definition = NULL;
    for (size_t i = 0; i < tool_count(); ++i) if (!strcmp(name, definitions[i].name)) definition = &definitions[i];
    if (!definition) return failure(doc, "Unknown tool.");
    if (!arguments_valid(definition, args)) return failure(doc, "Invalid or unknown tool arguments.");
    yyjson_mut_val *data = yyjson_mut_obj(doc);
    if (!strcmp(name, "host_info")) {
        char system[128], arch[16];
        sb_platform_info(system, sizeof(system), arch, sizeof(arch));
        yyjson_mut_obj_add_str(doc, data, "name", "ShellBot");
        yyjson_mut_obj_add_str(doc, data, "version", SB_VERSION);
        yyjson_mut_obj_add_strcpy(doc, data, "system", system);
        yyjson_mut_obj_add_strcpy(doc, data, "architecture", arch);
        yyjson_mut_obj_add_uint(doc, data, "uptime_ms", sb_platform_millis() - host->started_ms);
        yyjson_mut_obj_add_val(doc, data, "execution_status", sb_execution_status(host->execution, doc));
        return content_result(doc, data, false);
    }
    if (!strcmp(name, "host_capabilities")) {
        yyjson_mut_val *tools = yyjson_mut_arr(doc), *roots = yyjson_mut_arr(doc), *versions = yyjson_mut_arr(doc);
        for (size_t i = 0; i < tool_count(); ++i) yyjson_mut_arr_add_str(doc, tools, definitions[i].name);
        for (size_t i = 0; i < host->config.workspace_count; ++i) {
            yyjson_mut_val *root = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_strcpy(doc, root, "workspace_id", host->config.workspaces[i].id);
            yyjson_mut_obj_add_bool(doc, root, "read_only", host->config.workspaces[i].read_only);
            yyjson_mut_arr_add_val(roots, root);
        }
        yyjson_mut_arr_add_str(doc, versions, "2025-03-26");
        yyjson_mut_arr_add_str(doc, versions, "2025-06-18");
        yyjson_mut_arr_add_str(doc, versions, "2025-11-25");
        yyjson_mut_arr_add_str(doc, versions, "2026-07-28");
        yyjson_mut_obj_add_val(doc, data, "tools", tools);
        yyjson_mut_obj_add_val(doc, data, "workspaces", roots);
        yyjson_mut_obj_add_val(doc, data, "protocol_versions", versions);
        yyjson_mut_obj_add_uint(doc, data, "max_text_bytes", SB_TEXT_LIMIT);
        yyjson_mut_obj_add_uint(doc, data, "max_image_bytes", SB_IMAGE_LIMIT);
        yyjson_mut_obj_add_bool(doc, data, "file_write", true);
        yyjson_mut_obj_add_bool(doc, data, "file_overwrite", true);
        yyjson_mut_obj_add_bool(doc, data, "file_patch", true);
        yyjson_mut_obj_add_bool(doc, data, "atomic_overwrite", false);
        yyjson_mut_obj_add_uint(doc, data, "max_edit_bytes", SB_TEXT_LIMIT);
        yyjson_mut_obj_add_uint(doc, data, "max_backups_per_instance", 64);
        yyjson_mut_obj_add_bool(doc, data, "execution", sb_execution_available(host->execution));
        yyjson_mut_obj_add_val(doc, data, "execution_status", sb_execution_status(host->execution, doc));
        yyjson_mut_obj_add_bool(doc, data, "desktop_control", false);
        yyjson_mut_obj_add_bool(doc, data, "oauth", false);
        yyjson_mut_obj_add_bool(doc, data, "approval", true);
        yyjson_mut_obj_add_str(doc, data, "approval_mode", "local-owner");
        yyjson_mut_obj_add_bool(doc, data, "durable_approvals", false);
        yyjson_mut_obj_add_bool(doc, data, "durable_audit", true);
        yyjson_mut_obj_add_bool(doc, data, "audit_healthy", sb_audit_healthy(host->audit));
        yyjson_mut_obj_add_str(doc, data, "audit_scope", "approval transitions, file edits, task operation intents and new task startup results");
        yyjson_mut_obj_add_uint(doc, data, "audit_max_bytes", host->config.audit_max_bytes);
        yyjson_mut_obj_add_str(doc, data, "search_mode", "case-sensitive literal filenames or Unicode content");
        yyjson_mut_obj_add_bool(doc, data, "content_search", true);
        yyjson_mut_obj_add_bool(doc, data, "tunnel_management", false);
        yyjson_mut_obj_add_bool(doc, data, "domain_required", false);
        yyjson_mut_obj_add_str(doc, data, "http_binding", "127.0.0.1");
        if (*host->config.public_origin) {
            char endpoint[328]; snprintf(endpoint, sizeof(endpoint), "%s/mcp", host->config.public_origin);
            yyjson_mut_obj_add_strcpy(doc, data, "public_endpoint", endpoint);
        }
        return content_result(doc, data, false);
    }
    if (!strncmp(name,"task_",5)) {
        bool failed=false;
        yyjson_mut_val *result=sb_tasks_call(host,actor,name,args,doc,&failed);
        return content_result(doc,result,failed);
    }
    const char *id = sb_string(yyjson_obj_get(args, "workspace_id"));
    const char *path = sb_string(yyjson_obj_get(args, "path"));
    if (!id) return failure(doc, "workspace_id is required.");
    sb_workspace *workspace = sb_workspace_find(&host->config, id);
    if (!workspace) return failure(doc, "Unknown workspace_id.");
    if (!path && yyjson_obj_get(args, "path")) return failure(doc, "path must be a string without NUL.");
    if (!path && (!strcmp(name, "fs_read") || !strcmp(name, "image_view") || !strcmp(name, "fs_write") || !strcmp(name, "fs_patch"))) return failure(doc, "path is required.");
    if (!path) path = ".";
    sb_error error = {0};
    if (!strcmp(name, "fs_write") || !strcmp(name, "fs_patch")) {
        const char *text = sb_string(yyjson_obj_get(args, "text"));
        const char *approval_id = sb_string(yyjson_obj_get(args, "approval_id"));
        if ((!approval_id && yyjson_obj_get(args, "approval_id")) ||
            (approval_id && (strlen(approval_id) != 32 || strspn(approval_id, "0123456789abcdef") != 32)))
            return failure(doc, "approval_id must be a 32-character lowercase hex id.");
        yyjson_val *overwrite = yyjson_obj_get(args, "overwrite");
        if ((!strcmp(name, "fs_write") && !text) || (overwrite && !yyjson_is_bool(overwrite)))
            return failure(doc, "text must be a string without NUL and overwrite must be boolean.");
        bool editing = !strcmp(name, "fs_patch") || yyjson_get_bool(overwrite);
        bool failed;
        if (editing) {
            char hash[65]; uint64_t matches;
            const char *search = sb_string(yyjson_obj_get(args, "search")), *replacement = sb_string(yyjson_obj_get(args, "replacement"));
            if (!expected_hash(args, hash) || (!strcmp(name, "fs_patch") && (!search || !*search || !replacement)) ||
                !number(args, "expected_matches", 1, 1, 1000, &matches))
                return failure(doc, "Editing requires a 64-character expected_sha256; patch requires nonempty search, replacement and expected_matches within 1..1000.");
            sb_edit_request edit = {name, text, search, replacement, hash, approval_id, (size_t)matches};
            yyjson_mut_val *written = sb_policy_modify(host, actor, workspace, path, &edit, doc, &failed);
            return content_result(doc, written, failed);
        }
        if (yyjson_obj_get(args, "expected_sha256")) return failure(doc, "expected_sha256 requires overwrite:true.");
        yyjson_mut_val *written = sb_policy_write(host, actor, workspace, path, text, approval_id, doc, &failed);
        return content_result(doc, written, failed);
    }
    if (!strcmp(name, "fs_search")) {
        const char *query = sb_string(yyjson_obj_get(args, "query"));
        const char *mode = sb_string(yyjson_obj_get(args, "mode"));
        yyjson_val *recursive = yyjson_obj_get(args, "recursive");
        uint64_t maximum, per_file, total_bytes;
        if (!query || !*query || strlen(query) > 512 || (recursive && !yyjson_is_bool(recursive)) ||
            (!mode && yyjson_obj_get(args, "mode")) || (mode && strcmp(mode, "filename") && strcmp(mode, "content")) ||
            !number(args, "max_results", 50, 1, 100, &maximum) ||
            !number(args, "max_file_bytes", 262144, 4, 1048576, &per_file) ||
            !number(args, "max_total_bytes", 4194304, 4, 16777216, &total_bytes)) return failure(doc, "Invalid search parameters.");
        bool content = mode && !strcmp(mode, "content");
        if (content && (strchr(query, '\r') || strchr(query, '\n'))) return failure(doc, "Content queries must fit on one line.");
        char *decoded = NULL; size_t decoded_length, consumed; const char *detected;
        if (!sb_decode((const unsigned char *)query, strlen(query), true, "utf-8", true,
                       &decoded, &decoded_length, &consumed, &detected, &error)) return failure(doc, error.text);
        free(decoded);
        yyjson_mut_val *found = sb_search(workspace, path, query, yyjson_get_bool(recursive), content,
                                        (size_t)maximum, (size_t)per_file, (size_t)total_bytes, doc, &error);
        return found ? content_result(doc, found, false) : failure(doc, error.text);
    }
    bool directory = !strcmp(name, "fs_list");
    sb_file *file = sb_platform_open(workspace->root, path, directory, &error);
    if (!file) return failure(doc, error.text);
    yyjson_mut_val *result = NULL;
    if (directory) {
        uint64_t offset, limit;
        if (!number(args, "offset", 0, 0, 10000, &offset) || !number(args, "max_entries", 100, 1, 200, &limit)) {
            result = failure(doc, "Invalid directory range."); goto done;
        }
        sb_entry *entries = NULL;
        size_t count = 0; bool more = false;
        if (!sb_platform_list(file, (size_t)offset, (size_t)limit, &entries, &count, &more, &error)) {
            result = failure(doc, error.text); goto done;
        }
        yyjson_mut_val *items = yyjson_mut_arr(doc);
        for (size_t i = 0; i < count; ++i) yyjson_mut_arr_add_val(items, entry_json(doc, &entries[i]));
        sb_platform_entries_free(entries, count);
        yyjson_mut_obj_add_val(doc, data, "entries", items);
        yyjson_mut_obj_add_uint(doc, data, "next_offset", offset + count);
        yyjson_mut_obj_add_bool(doc, data, "has_more", more);
        result = content_result(doc, data, false);
    } else if (!strcmp(name, "fs_read")) {
        uint64_t offset, limit;
        const char *encoding = sb_string(yyjson_obj_get(args, "encoding"));
        if ((!encoding && yyjson_obj_get(args, "encoding")) || !number(args, "offset", 0, 0, INT64_MAX, &offset) ||
            !number(args, "max_bytes", SB_TEXT_LIMIT, 4, SB_TEXT_LIMIT, &limit)) {
            result = failure(doc, "Invalid text range or encoding."); goto done;
        }
        if (!encoding) encoding = "auto";
        if (offset % 2 && (!strcmp(encoding, "utf-16le") || !strcmp(encoding, "utf-16be"))) {
            result = failure(doc, "UTF-16 byte offsets must be even."); goto done;
        }
        unsigned char *bytes = malloc((size_t)limit);
        if (!bytes) { result = failure(doc, "Out of memory."); goto done; }
        size_t got;
        if (!sb_platform_read(file, offset, bytes, (size_t)limit, &got, &error)) {
            free(bytes); result = failure(doc, error.text); goto done;
        }
        char *text = NULL; size_t text_length, consumed; const char *detected;
        bool ok = sb_decode(bytes, got, offset + got >= sb_platform_size(file), encoding, offset == 0,
            &text, &text_length, &consumed, &detected, &error);
        char hash[65]; bool complete = offset == 0 && got == sb_platform_size(file);
        if (ok && complete) ok = sb_platform_sha256(bytes, got, hash, &error);
        free(bytes);
        if (!ok) { free(text); result = failure(doc, error.text); goto done; }
        if (complete) yyjson_mut_obj_add_strcpy(doc, data, "sha256", hash);
        yyjson_mut_obj_add_strncpy(doc, data, "text", text, text_length);
        free(text);
        yyjson_mut_obj_add_str(doc, data, "encoding", detected);
        yyjson_mut_obj_add_uint(doc, data, "size", sb_platform_size(file));
        yyjson_mut_obj_add_uint(doc, data, "offset", offset);
        yyjson_mut_obj_add_uint(doc, data, "next_offset", offset + consumed);
        yyjson_mut_obj_add_bool(doc, data, "truncated", offset + consumed < sb_platform_size(file));
        result = content_result(doc, data, false);
    } else {
        uint64_t size = sb_platform_size(file);
        if (!size || size > SB_IMAGE_LIMIT) { result = failure(doc, "Image exceeds 2 MiB or is empty. Resize it locally first."); goto done; }
        unsigned char *bytes = malloc((size_t)size);
        if (!bytes) { result = failure(doc, "Out of memory."); goto done; }
        size_t got = 0;
        bool ok = sb_platform_read(file, 0, bytes, (size_t)size, &got, &error);
        uint32_t width = 0, height = 0;
        const char *mime = ok && got == size ? image_header(bytes, got, &width, &height) : NULL;
        if (!mime || !width || !height || (uint64_t)width * height > 16777216) {
            free(bytes); result = failure(doc, "Unsupported or invalid image header, or more than 16 megapixels."); goto done;
        }
        char *encoded = sb_base64(bytes, got);
        free(bytes);
        if (!encoded) { result = failure(doc, "Out of memory."); goto done; }
        yyjson_mut_obj_add_str(doc, data, "mime_type", mime);
        yyjson_mut_obj_add_uint(doc, data, "width", width);
        yyjson_mut_obj_add_uint(doc, data, "height", height);
        yyjson_mut_obj_add_uint(doc, data, "size", size);
        result = content_result(doc, data, false);
        yyjson_mut_val *image = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, image, "type", "image");
        yyjson_mut_obj_add_str(doc, image, "mimeType", mime);
        yyjson_mut_obj_add_strcpy(doc, image, "data", encoded);
        free(encoded);
        if (result) yyjson_mut_arr_add_val(yyjson_mut_obj_get(result, "content"), image);
    }
done:
    sb_platform_close(file);
    return result;
}
