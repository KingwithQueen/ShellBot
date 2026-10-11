#include "sb.h"
#include "sb_projects.h"
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
    {"image_view", "Fully decode a bounded PNG/JPEG and return native MCP image content. Optional max_dimension:128..4096 shrinks proportionally without enlarging. Sources up to 16 MiB and 16777216 pixels; output at most 2 MiB. Larger sources are normalized to a bounded PNG, possibly smaller than requested. Original bytes are preserved when no processing is needed; derivatives omit source metadata and use stored raster orientation.",
        "{\"type\":\"object\",\"properties\":{\"workspace_id\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"max_dimension\":{\"type\":\"integer\",\"minimum\":128,\"maximum\":4096}},\"required\":[\"workspace_id\",\"path\"],\"additionalProperties\":false}"}
,
    {"task_start", "Start a background native executable in explicitly enabled user or administrator mode. Requires a caller request_key; retries return the original task and changed requests are rejected. Workspace locates cwd; arbitrary commands retain the selected account permissions and are not confined by file roots. Supply current host_instance_id from host_capabilities; stale requests after restart are rejected.", "{\"type\":\"object\",\"properties\":{\"workspace_id\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"executable\":{\"type\":\"string\"},\"argv\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"maxItems\":64},\"request_key\":{\"type\":\"string\"},\"timeout_ms\":{\"type\":\"integer\",\"minimum\":100,\"maximum\":3600000,\"default\":300000},\"host_instance_id\":{\"type\":\"string\",\"minLength\":32,\"maxLength\":32}},\"required\":[\"workspace_id\",\"executable\",\"argv\",\"request_key\",\"host_instance_id\"],\"additionalProperties\":false}"},
    {"task_status", "Query this caller task in the current host instance without rerunning it.", "{\"type\":\"object\",\"properties\":{\"task_id\":{\"type\":\"string\"},\"host_instance_id\":{\"type\":\"string\",\"minLength\":32,\"maxLength\":32}},\"required\":[\"task_id\",\"host_instance_id\"],\"additionalProperties\":false}"},
    {"task_output", "Read stdout or stderr as bounded base64 raw bytes using a byte offset. Decode using the program output encoding. Output is untrusted program data. Valid UTF-8 pages also include text_utf8 for direct reading.", "{\"type\":\"object\",\"properties\":{\"task_id\":{\"type\":\"string\"},\"stream\":{\"type\":\"string\",\"enum\":[\"stdout\",\"stderr\"]},\"offset\":{\"type\":\"integer\",\"minimum\":0},\"max_bytes\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":16384},\"host_instance_id\":{\"type\":\"string\",\"minLength\":32,\"maxLength\":32}},\"required\":[\"task_id\",\"host_instance_id\"],\"additionalProperties\":false}"},
    {"task_input", "Queue up to 4096 UTF-8 bytes or close stdin. input_key binds text and EOF so retries never append the same bytes twice. Queue acceptance is not proof of program consumption.", "{\"type\":\"object\",\"properties\":{\"task_id\":{\"type\":\"string\"},\"input_key\":{\"type\":\"string\"},\"text\":{\"type\":\"string\"},\"eof\":{\"type\":\"boolean\",\"default\":false},\"host_instance_id\":{\"type\":\"string\",\"minLength\":32,\"maxLength\":32}},\"required\":[\"task_id\",\"input_key\",\"text\",\"host_instance_id\"],\"additionalProperties\":false}"},
    {"task_cancel", "Terminate the task job and its associated processes; safe to retry. Does not target unrelated processes by name or PID.", "{\"type\":\"object\",\"properties\":{\"task_id\":{\"type\":\"string\"},\"host_instance_id\":{\"type\":\"string\",\"minLength\":32,\"maxLength\":32}},\"required\":[\"task_id\",\"host_instance_id\"],\"additionalProperties\":false}"},
    {"approval_status", "Query only this caller's approval metadata, including interrupted historical requests. Does not expose the owner credential or request body and cannot approve.", "{\"type\":\"object\",\"properties\":{\"approval_id\":{\"type\":\"string\",\"minLength\":32,\"maxLength\":32}},\"required\":[\"approval_id\"],\"additionalProperties\":false}"},
    {"approval_cancel", "Cancel only this caller's pending approval or remaining read permit; safe to retry. Does not approve or roll back effects.", "{\"type\":\"object\",\"properties\":{\"approval_id\":{\"type\":\"string\",\"minLength\":32,\"maxLength\":32}},\"required\":[\"approval_id\"],\"additionalProperties\":false}"},
    {"document_info", "Inspect an approved PDF/DOCX/XLSX/PPTX and list up to 128 document units. PDF units are pages, DOCX has one text document, XLSX units are worksheets and PPTX units are slides. Does not edit the source or execute macros. Legacy DOC/XLS/PPT must be converted first.", "{\"type\":\"object\",\"properties\":{\"workspace_id\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}},\"required\":[\"workspace_id\",\"path\"],\"additionalProperties\":false}"},
    {"document_read", "Read bounded extracted UTF-8 text from one approved document unit, numbered from 1. Continue at next_offset with identical arguments and approval_id. PDF text follows object order, with no OCR. XLSX returns cell references and stored values, never recalculates formulas. Office XML text is not original page layout. Source maximum 32 MiB, extracted unit maximum 1 MiB.", "{\"type\":\"object\",\"properties\":{\"workspace_id\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"unit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":4096,\"default\":1},\"offset\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":1048576},\"max_bytes\":{\"type\":\"integer\",\"minimum\":4,\"maximum\":65536}},\"required\":[\"workspace_id\",\"path\"],\"additionalProperties\":false}"},
    {"document_preview", "Return a bounded native PNG image of one approved PDF page or an explicitly labelled Office structured text preview. Office previews do not reproduce Word pagination, spreadsheet formatting, slide graphics or original layout. Unit numbers start at 1. Source is unchanged; no macros, external resources or formula execution.", "{\"type\":\"object\",\"properties\":{\"workspace_id\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"unit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":4096,\"default\":1},\"max_dimension\":{\"type\":\"integer\",\"minimum\":128,\"maximum\":4096,\"default\":1600}},\"required\":[\"workspace_id\",\"path\"],\"additionalProperties\":false}"}
};

static size_t tool_count(void) { return sizeof(definitions) / sizeof(definitions[0]); }

bool sb_tool_exists(const char *name) {
    if (sb_desktop_tool(name)) return true;
    for (size_t i = 0; i < tool_count(); ++i) if (!strcmp(name, definitions[i].name)) return true;
    return false;
}

static yyjson_mut_val *tools_list(yyjson_mut_doc *doc, bool approvals) {
    yyjson_mut_val *result = yyjson_mut_obj(doc), *array = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, result, "tools", array);
    for (size_t i = 0; i < tool_count(); ++i) {
        if (!approvals && !strncmp(definitions[i].name,"approval_",9)) continue;
        yyjson_mut_val *tool = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, tool, "name", definitions[i].name);
        yyjson_mut_obj_add_str(doc, tool, "description", definitions[i].description);
        yyjson_doc *schema = yyjson_read(definitions[i].schema, strlen(definitions[i].schema), 0);
        if (!schema) return NULL;
        yyjson_mut_obj_add_val(doc, tool, "inputSchema", yyjson_val_mut_copy(doc, yyjson_doc_get_root(schema)));
        yyjson_doc_free(schema);
        yyjson_mut_val *annotations = yyjson_mut_obj(doc);
        bool task_tool=!strncmp(definitions[i].name,"task_",5);
        bool modifying = !strcmp(definitions[i].name, "fs_write") || !strcmp(definitions[i].name, "fs_patch") || !strcmp(definitions[i].name,"task_start") || !strcmp(definitions[i].name,"task_input") || !strcmp(definitions[i].name,"task_cancel") || !strcmp(definitions[i].name,"approval_cancel");
        yyjson_mut_obj_add_bool(doc, annotations, "readOnlyHint", !modifying);
        yyjson_mut_obj_add_bool(doc, annotations, "destructiveHint", modifying);
        yyjson_mut_obj_add_bool(doc, annotations, "idempotentHint", task_tool || !modifying);
        yyjson_mut_obj_add_bool(doc, annotations, "openWorldHint", task_tool);
        yyjson_mut_obj_add_val(doc, tool, "annotations", annotations);
        yyjson_mut_arr_add_val(array, tool);
    }
    return result;
}
yyjson_mut_val *sb_tools_list(yyjson_mut_doc *doc) { return tools_list(doc,false); }

static bool project_tool_visible(sb_host *host, const char *name) {
    if (!host->project) return true;
    int index = sb_project_tool_index(name);
    if (index >= SB_DESKTOP_TOOL_FIRST && index < SB_DESKTOP_TOOL_END) return sb_desktop_tool_visible(host,name);
    /* Result polling and cancellation remain visible for already owned tasks. */
    return index >= 0 && ((index == 9 || index == 10 || index == 12 || index==13 || index==14) ||
        (host->project->enabled && (host->project->policy.tools & (UINT32_C(1) << index))));
}
yyjson_mut_val *sb_project_tools_list(sb_host *host, yyjson_mut_doc *doc) {
    yyjson_mut_val *result = tools_list(doc,host->project && !host->project->legacy_compatibility);
    if (!host->project || !result) return result;
    yyjson_mut_val *tools = yyjson_mut_obj_get(result, "tools");
    sb_desktop_tools_list(host,doc,tools);
    for (size_t i = yyjson_mut_arr_size(tools); i; --i) {
        yyjson_mut_val *tool = yyjson_mut_arr_get(tools,i-1);
        const char *name = yyjson_mut_get_str(yyjson_mut_obj_get(tool, "name"));
        if (!project_tool_visible(host, name)) yyjson_mut_arr_remove(tools, i - 1);
        else if (!host->project->legacy_compatibility && (strncmp(name,"fs_",3) == 0 || !strcmp(name,"image_view") || sb_document_tool(name) || !strncmp(name,"task_",5) || !strncmp(name,"approval_",9))) {
            yyjson_mut_val *schema = yyjson_mut_obj_get(tool,"inputSchema"), *properties = yyjson_mut_obj_get(schema,"properties");
            yyjson_mut_val *identity = yyjson_mut_obj(doc), *choices = yyjson_mut_arr(doc);
            yyjson_mut_obj_add_str(doc,identity,"type","string");
            yyjson_mut_arr_add_str(doc,choices,"current_user"); yyjson_mut_arr_add_str(doc,choices,"administrator");
            if (!strncmp(name,"task_",5)) yyjson_mut_arr_add_str(doc,choices,"restricted");
            yyjson_mut_obj_add_val(doc,identity,"enum",choices); yyjson_mut_obj_add_val(doc,properties,"identity",identity);
            if (!yyjson_mut_obj_get(properties,"approval_id") && strncmp(name,"approval_",9) && (strncmp(name,"task_",5) || !strcmp(name,"task_start") || !strcmp(name,"task_input"))) {
                yyjson_mut_val *approval = yyjson_mut_obj(doc); yyjson_mut_obj_add_str(doc,approval,"type","string");
                yyjson_mut_obj_add_val(doc,properties,"approval_id",approval);
            }
            if (host->project->policy.file_scope == SB_FILES_CURRENT_USER && strncmp(name,"task_",5) && strncmp(name,"approval_",9)) {
                yyjson_mut_val *required = yyjson_mut_obj_get(schema,"required");
                for (size_t j = yyjson_mut_arr_size(required); j; --j)
                    if (!strcmp(yyjson_mut_get_str(yyjson_mut_arr_get(required,j-1)),"workspace_id")) yyjson_mut_arr_remove(required,j-1);
            }
            if (!strncmp(name,"approval_",9)) continue;
            const char *policy_description="Project policy controls this operation. Administrator operations require identity:administrator and a manager-owned shared agent with owner project permission. Keep this identity on task status/output/input/cancel too. Pending replies contain approval_id; the local owner uses the matching ordinary or administrator approval channel. Keep arguments identical; file pages use next_offset and the same approval_id. With current_user file scope, omit workspace_id for an absolute local path. Read-only grants, expected hashes and private backups still apply.";
            char document_description[2048];
            if (sb_document_tool(name)) {
                snprintf(document_description,sizeof(document_description),"%s %s",yyjson_mut_get_str(yyjson_mut_obj_get(tool,"description")),policy_description);
            }
            yyjson_mut_obj_remove_str(tool,"description");
            if (sb_document_tool(name)) yyjson_mut_obj_add_strcpy(doc,tool,"description",document_description);
            else yyjson_mut_obj_add_str(doc,tool,"description",policy_description);
        }
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

static bool arguments_valid(const tool_definition *definition, yyjson_val *args, const sb_host *host) {
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
        bool project_approval = host->project && !host->project->legacy_compatibility && name && !strcmp(name,"approval_id") &&
            (strncmp(definition->name,"fs_",3) == 0 || !strcmp(definition->name,"image_view") || sb_document_tool(definition->name) || !strcmp(definition->name,"task_start") || !strcmp(definition->name,"task_input"));
        bool project_identity = host->project && !host->project->legacy_compatibility && name && !strcmp(name,"identity") &&
            (strncmp(definition->name,"fs_",3) == 0 || !strcmp(definition->name,"image_view") || sb_document_tool(definition->name) || !strncmp(definition->name,"task_",5) || !strncmp(definition->name,"approval_",9));
        if (!name || (!yyjson_obj_get(properties, name) && !project_approval && !project_identity)) { valid = false; break; }
        yyjson_obj_iter it = yyjson_obj_iter_with(args);
        for (size_t j = 0; j < idx; ++j) if (!strcmp(name, yyjson_get_str(yyjson_obj_iter_next(&it)))) valid = false;
    }
    yyjson_doc_free(schema);
    if (!strcmp(definition->name,"image_view")) {
        uint64_t maximum=0;
        if (!number(args,"max_dimension",0,128,4096,&maximum)) valid=false;
    }
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

static yyjson_mut_val *tool_call_inner(sb_host *host, const char *actor, const char *name, yyjson_val *args,
    yyjson_mut_doc *doc, sb_workspace *override, const char *path_override, bool authorized) {
    const tool_definition *definition = NULL;
    for (size_t i = 0; i < tool_count(); ++i) if (!strcmp(name, definitions[i].name)) definition = &definitions[i];
    if (!definition) return failure(doc, "Unknown tool.");
    if (!arguments_valid(definition, args, host)) return failure(doc, "Invalid or unknown tool arguments.");
    if (host->project && !authorized) {
        const sb_project *project = host->project;
        const sb_project_directory *directory = sb_project_directory_find(project, sb_string(yyjson_obj_get(args, "workspace_id")));
        sb_request_context context = {.actor = actor, .project = project, .policy_revision = project->policy_revision};
        sb_project_operation operation = {.tool = name, .directory = directory,
            .identity = (!strcmp(name, "task_start") || !strcmp(name, "task_input")) ? project->policy.default_identity : SB_ID_CURRENT_USER,
            .owned_task = sb_execution_task_owned(host->execution, actor, sb_string(yyjson_obj_get(args, "task_id")))};
        sb_policy_result decision = sb_project_evaluate(&context, &operation);
        if (decision.decision == SB_DECISION_DENY) return failure(doc, decision.reason);
        bool write = !strcmp(name, "fs_write") || !strcmp(name, "fs_patch");
        if (decision.decision == SB_DECISION_PENDING && !(write && project->legacy_compatibility))
            return failure(doc, "project_approval_adapter_unavailable: this operation requires M4 approval; no content accessed or operation executed.");
        if (write && !project->legacy_compatibility)
            return failure(doc, "project_file_write_adapter_unavailable: automatic project writes require M4.");
    }
    yyjson_mut_val *data = yyjson_mut_obj(doc);
    if (host->project) {
        yyjson_mut_obj_add_str(doc, data, "project_id", host->project->id);
        yyjson_mut_obj_add_uint(doc, data, "policy_revision", host->project->policy_revision);
    }
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
        for (size_t i = 0; i < tool_count(); ++i)
            if (project_tool_visible(host, definitions[i].name)) yyjson_mut_arr_add_str(doc, tools, definitions[i].name);
        for (size_t i=SB_DESKTOP_TOOL_FIRST;i<SB_DESKTOP_TOOL_END;++i)
            if (sb_desktop_tool_visible(host,sb_project_tool_name(i))) yyjson_mut_arr_add_str(doc,tools,sb_project_tool_name(i));
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
        yyjson_mut_obj_add_uint(doc, data, "max_image_source_bytes", SB_IMAGE_INPUT_LIMIT);
        yyjson_mut_obj_add_uint(doc, data, "max_image_source_pixels", SB_IMAGE_PIXEL_LIMIT);
        yyjson_mut_obj_add_uint(doc, data, "max_image_source_dimension", 32768);
        yyjson_mut_obj_add_uint(doc, data, "max_image_dimension", 4096);
        yyjson_mut_obj_add_bool(doc, data, "image_full_decode", true);
        yyjson_mut_obj_add_bool(doc, data, "image_resize", true);
        yyjson_mut_obj_add_bool(doc, data, "file_write", true);
        yyjson_mut_obj_add_bool(doc, data, "file_overwrite", true);
        yyjson_mut_obj_add_bool(doc, data, "file_patch", true);
        yyjson_mut_obj_add_bool(doc, data, "atomic_overwrite", false);
        yyjson_mut_obj_add_uint(doc, data, "max_edit_bytes", SB_TEXT_LIMIT);
        yyjson_mut_obj_add_uint(doc, data, "max_backups_per_instance", 64);
        yyjson_mut_obj_add_bool(doc, data, "execution", sb_execution_available(host->execution));
        yyjson_mut_obj_add_val(doc, data, "execution_status", sb_execution_status(host->execution, doc));
        yyjson_mut_obj_add_bool(doc, data, "desktop_control", host->project && host->project->policy.desktop.control && sb_desktop_available(host->desktop));
        yyjson_mut_obj_add_val(doc,data,"desktop_status",sb_desktop_capabilities(host->desktop,doc));
        yyjson_mut_obj_add_bool(doc, data, "oauth", false);
        yyjson_mut_obj_add_bool(doc, data, "approval", true);
        yyjson_mut_obj_add_str(doc, data, "approval_mode", host->project && !host->project->legacy_compatibility ?
            (host->project->policy.approval_mode == SB_PROJECT_NEVER ? "never" : host->project->policy.approval_mode == SB_PROJECT_WORKSPACE_AUTO ? "workspace_auto" : "always") : "local-owner");
        if (host->project) {
            yyjson_mut_obj_add_bool(doc, data, "project_approval_adapter", !host->project->legacy_compatibility);
            yyjson_mut_obj_add_bool(doc, data, "unlisted_account_file_adapter", !host->project->legacy_compatibility && host->project->policy.file_scope == SB_FILES_CURRENT_USER);
            yyjson_mut_obj_add_bool(doc, data, "administrator_backend", host->administrator_backend);
            yyjson_mut_obj_add_bool(doc, data, "administrator_identity_explicit", true);
            yyjson_mut_obj_add_uint(doc, data, "max_active_tasks", host->config.limits.active_tasks);
            yyjson_mut_obj_add_uint(doc, data, "max_task_records", host->config.limits.task_records);
            yyjson_mut_obj_add_uint(doc, data, "max_output_bytes", host->config.limits.output_bytes);
            yyjson_mut_obj_add_uint(doc, data, "max_timeout_ms", host->config.limits.task_timeout_ms);
            yyjson_mut_obj_add_uint(doc, data, "max_approval_records", host->config.limits.approval_records);
            yyjson_mut_obj_add_uint(doc, data, "worker_memory_mb", host->config.limits.worker_memory_mb);
        }
        yyjson_mut_obj_add_bool(doc, data, "durable_approvals", host->store!=NULL);
        yyjson_mut_obj_add_bool(doc, data, "durable_tasks", host->store!=NULL);
        yyjson_mut_obj_add_bool(doc, data, "approval_restored_after_restart", false);
        yyjson_mut_obj_add_bool(doc, data, "state_healthy", sb_store_healthy(host->store));
        yyjson_mut_obj_add_bool(doc, data, "remote_owner_approval", *host->owner_url!=0);
        yyjson_mut_obj_add_bool(doc, data, "durable_audit", true);
        yyjson_mut_obj_add_bool(doc, data, "audit_healthy", sb_audit_healthy(host->audit));
        yyjson_mut_obj_add_str(doc, data, "audit_scope", host->store ? "approval decisions, file calls, task starts/completion/cancellation/timeouts and input delivery" : "approval transitions, file edits, task operation intents and new task startup results");
        yyjson_mut_obj_add_uint(doc, data, "audit_max_bytes", host->config.audit_max_bytes);
        yyjson_mut_obj_add_str(doc, data, "search_mode", "case-sensitive literal filenames or Unicode content");
        yyjson_mut_obj_add_bool(doc, data, "content_search", true);
        yyjson_mut_obj_add_bool(doc,data,"document_read",true);
        yyjson_mut_obj_add_bool(doc,data,"pdf_page_preview",true);
        yyjson_mut_obj_add_str(doc,data,"pdf_backend","pinned PDFium 157.0.8086.0; no V8");
        yyjson_mut_obj_add_str(doc,data,"office_formats","DOCX, XLSX, PPTX; legacy formats require conversion");
        yyjson_mut_obj_add_str(doc,data,"office_preview","structured text, not original layout");
        yyjson_mut_obj_add_uint(doc,data,"max_document_source_bytes",SB_DOCUMENT_INPUT_LIMIT);
        yyjson_mut_obj_add_uint(doc,data,"max_document_unit_text_bytes",SB_DOCUMENT_TEXT_LIMIT);
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
    if (!id && !override) return failure(doc, "workspace_id is required.");
    sb_workspace *workspace = override ? override : sb_workspace_find(&host->config, id);
    if (!workspace) return failure(doc, "Unknown workspace_id.");
    if (!path && yyjson_obj_get(args, "path")) return failure(doc, "path must be a string without NUL.");
    if (!path && (!strcmp(name, "fs_read") || !strcmp(name, "image_view") || sb_document_tool(name) || !strcmp(name, "fs_write") || !strcmp(name, "fs_patch"))) return failure(doc, "path is required.");
    if (!path) path = ".";
    if (path_override) path = path_override;
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
            yyjson_mut_val *written = authorized ? sb_policy_modify_authorized(host,actor,workspace,path,&edit,doc,&failed) :
                sb_policy_modify(host, actor, workspace, path, &edit, doc, &failed);
            return content_result(doc, written, failed);
        }
        if (yyjson_obj_get(args, "expected_sha256")) return failure(doc, "expected_sha256 requires overwrite:true.");
        yyjson_mut_val *written = authorized ? sb_policy_write_authorized(host,actor,workspace,path,text,doc,&failed) :
            sb_policy_write(host, actor, workspace, path, text, approval_id, doc, &failed);
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
    } else if (sb_document_tool(name)) {
        sb_image preview={0};
        yyjson_mut_val *document=sb_document_call(file,path,name,args,doc,&preview,&error);
        result=document ? content_result(doc,document,false) : failure(doc,error.text);
        if (document && preview.encoded) {
            char *encoded=sb_base64(preview.encoded,preview.size);free(preview.encoded);
            if(!encoded){result=failure(doc,"Out of memory.");goto done;}
            yyjson_mut_val *image=yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc,image,"type","image");yyjson_mut_obj_add_str(doc,image,"mimeType","image/png");
            yyjson_mut_obj_add_strcpy(doc,image,"data",encoded);free(encoded);
            yyjson_mut_arr_add_val(yyjson_mut_obj_get(result,"content"),image);
        }
    } else {
        uint64_t size = sb_platform_size(file);
        if (!size || size > SB_IMAGE_INPUT_LIMIT) { result = failure(doc, "Image exceeds 16 MiB or is empty."); goto done; }
        unsigned char *bytes = malloc((size_t)size);
        if (!bytes) { result = failure(doc, "Out of memory."); goto done; }
        size_t got = 0;
        bool ok = sb_platform_read(file, 0, bytes, (size_t)size, &got, &error);
        uint64_t maximum=0; sb_image processed={0}; char source_hash[65],output_hash[65];
        ok=ok && got==size && number(args,"max_dimension",0,128,4096,&maximum) &&
            sb_image_prepare(bytes,got,(unsigned)maximum,&processed,&error);
        const unsigned char *output=processed.encoded ? processed.encoded : bytes;
        if (ok) ok=sb_platform_sha256(bytes,got,source_hash,&error) && sb_platform_sha256(output,processed.size,output_hash,&error);
        if (!ok) { free(bytes); free(processed.encoded); result=failure(doc,*error.text ? error.text : "Image read or parameters invalid."); goto done; }
        char *encoded = sb_base64(output, processed.size);
        bool reencoded=processed.encoded!=NULL;
        free(bytes); free(processed.encoded);
        if (!encoded) { result = failure(doc, "Out of memory."); goto done; }
        yyjson_mut_obj_add_str(doc, data, "mime_type", processed.mime);
        yyjson_mut_obj_add_str(doc, data, "source_mime_type", processed.source_mime);
        yyjson_mut_obj_add_uint(doc, data, "width", processed.width);
        yyjson_mut_obj_add_uint(doc, data, "height", processed.height);
        yyjson_mut_obj_add_uint(doc, data, "source_width", processed.source_width);
        yyjson_mut_obj_add_uint(doc, data, "source_height", processed.source_height);
        yyjson_mut_obj_add_uint(doc, data, "size", processed.size);
        yyjson_mut_obj_add_uint(doc, data, "source_size", size);
        yyjson_mut_obj_add_bool(doc, data, "fully_decoded", true);
        yyjson_mut_obj_add_bool(doc, data, "reencoded", reencoded);
        yyjson_mut_obj_add_bool(doc, data, "resized", processed.width!=processed.source_width || processed.height!=processed.source_height);
        yyjson_mut_obj_add_bool(doc, data, "exif_orientation_applied", false);
        yyjson_mut_obj_add_strcpy(doc, data, "source_sha256", source_hash);
        yyjson_mut_obj_add_strcpy(doc, data, "sha256", output_hash);
        result = content_result(doc, data, false);
        yyjson_mut_val *image = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, image, "type", "image");
        yyjson_mut_obj_add_str(doc, image, "mimeType", processed.mime);
        yyjson_mut_obj_add_strcpy(doc, image, "data", encoded);
        free(encoded);
        if (result) yyjson_mut_arr_add_val(yyjson_mut_obj_get(result, "content"), image);
    }
done:
    sb_platform_close(file);
    return result;
}

static char *operation_parameters(const char *name, yyjson_val *args) {
    const char *keys[32]; size_t count = 0, idx, max; yyjson_val *key,*value;
    yyjson_obj_foreach(args,idx,max,key,value) {
        (void)value; const char *text = sb_string(key);
        if (!strcmp(text,"approval_id") || ((!strcmp(name,"fs_read") || !strcmp(name,"fs_list") || !strcmp(name,"document_read")) && !strcmp(text,"offset")) ||
            (!strcmp(name,"fs_read") && !strcmp(text,"encoding"))) continue;
        if (count == 32) return NULL; keys[count++] = text;
    }
    for(size_t i=1;i<count;++i) for(size_t j=i;j && strcmp(keys[j-1],keys[j])>0;--j) { const char *swap=keys[j-1];keys[j-1]=keys[j];keys[j]=swap; }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL); if (!doc) return NULL;
    yyjson_mut_val *object = yyjson_mut_obj(doc);
    for (size_t i = 0; i < count; ++i) yyjson_mut_obj_add_val(doc,object,keys[i],yyjson_val_mut_copy(doc,yyjson_obj_get(args,keys[i])));
    yyjson_mut_doc_set_root(doc,object); char *text = yyjson_mut_write(doc,0,NULL); yyjson_mut_doc_free(doc); return text;
}

static bool project_arguments_valid(sb_host *host, const tool_definition *definition, yyjson_val *args) {
    if (!arguments_valid(definition,args,host)) return false;
    yyjson_doc *schema = yyjson_read(definition->schema,strlen(definition->schema),0);
    if (!schema) return false;
    yyjson_val *root = yyjson_doc_get_root(schema), *required = yyjson_obj_get(root,"required"), *properties = yyjson_obj_get(root,"properties");
    bool valid = true; size_t idx,max; yyjson_val *key,*value;
    yyjson_arr_foreach(required,idx,max,value) {
        const char *name = sb_string(value);
        bool account_path = !strcmp(name,"workspace_id") && host->project->policy.file_scope == SB_FILES_CURRENT_USER &&
            strncmp(definition->name,"task_",5) && !yyjson_obj_get(args,"workspace_id");
        if (!account_path && !yyjson_obj_get(args,name)) valid = false;
    }
    yyjson_obj_foreach(args,idx,max,key,value) {
        const char *name = sb_string(key); yyjson_val *property = yyjson_obj_get(properties,name);
        const char *type = sb_string(yyjson_obj_get(property,"type"));
        if (!strcmp(name,"identity")) {
            const char *identity=sb_string(value);
            if (!identity || (strcmp(identity,"current_user") && strcmp(identity,"administrator") &&
                (strcmp(identity,"restricted") || strncmp(definition->name,"task_",5)))) valid=false;
        } else if (!strcmp(name,"approval_id")) {
            const char *id = sb_string(value); if (!id || strlen(id)!=32 || strspn(id,"0123456789abcdef")!=32) valid=false;
        } else if (type && !strcmp(type,"string")) {
            const char *text = sb_string(value); uint64_t lo=0,hi=UINT64_MAX;
            sb_integer(yyjson_obj_get(property,"minLength"),&lo); sb_integer(yyjson_obj_get(property,"maxLength"),&hi);
            if (!text || strlen(text)<lo || strlen(text)>hi) valid=false;
            yyjson_val *choices = yyjson_obj_get(property,"enum");
            if (choices) { bool matched=false; size_t i,n; yyjson_val *choice; yyjson_arr_foreach(choices,i,n,choice) if(text && !strcmp(text,sb_string(choice))) matched=true; if(!matched)valid=false; }
        } else if (type && !strcmp(type,"integer")) {
            uint64_t n=0,lo=0,hi=UINT64_MAX; sb_integer(yyjson_obj_get(property,"minimum"),&lo);sb_integer(yyjson_obj_get(property,"maximum"),&hi);
            if (!sb_integer(value,&n) || n<lo || n>hi)valid=false;
        } else if (type && !strcmp(type,"boolean") && !yyjson_is_bool(value)) valid=false;
        else if (type && !strcmp(type,"array") && !yyjson_is_arr(value)) valid=false;
    }
    yyjson_doc_free(schema); return valid;
}
static bool command_directory_version(sb_root *root, const char *path, char version[65], sb_error *error) {
    /* cwd authorizes the physical directory, not every file a native program
       might use. Dependencies are not a sandbox or an implicit script approval. */
    sb_file *directory=sb_platform_open(root,path,true,error);
    bool ok=directory && sb_platform_file_version(directory,version,error);
    sb_platform_close(directory); return ok;
}

static yyjson_mut_val *project_tool_call(sb_host *host, const char *actor, const char *name, yyjson_val *args, yyjson_mut_doc *doc) {
    if (sb_desktop_tool(name)) return sb_desktop_tools_call(host,actor,name,args,doc);
    if (!host->project || host->project->legacy_compatibility) return tool_call_inner(host,actor,name,args,doc,NULL,NULL,false);
    const tool_definition *definition = NULL;
    for (size_t i=0;i<tool_count();++i) if(!strcmp(name,definitions[i].name)) definition=&definitions[i];
    if (!definition || !project_arguments_valid(host,definition,args)) return failure(doc,"Invalid or unknown tool arguments.");
    const sb_project *project = host->project;
    const char *selected_identity=sb_string(yyjson_obj_get(args,"identity"));
    bool administrator=selected_identity && !strcmp(selected_identity,"administrator");
    if (administrator && !host->administrator_host) {
        char *wire=host->administrator_call ? host->administrator_call(host->administrator_context,name,args) : NULL;
        yyjson_doc *reply=wire ? yyjson_read(wire,strlen(wire),0) : NULL;
        yyjson_val *root=yyjson_doc_get_root(reply);
        yyjson_mut_val *result=yyjson_is_bool(yyjson_obj_get(root,"isError")) ? yyjson_val_mut_copy(doc,root) :
            failure(doc,sb_string(yyjson_obj_get(root,"error")) ? sb_string(yyjson_obj_get(root,"error")) : "project_administrator_backend_unavailable");
        yyjson_doc_free(reply); free(wire); return result;
    }
    if (host->administrator_host && !administrator) return failure(doc,"Administrator identity must be explicitly selected.");
    if (!strcmp(name,"approval_status") || !strcmp(name,"approval_cancel")) {
        const char *id=sb_string(yyjson_obj_get(args,"approval_id"));
        char *wire=sb_policy_caller_request(host->policy,actor,!strcmp(name,"approval_cancel") ? "cancel" : "status",id);
        yyjson_doc *saved=wire ? yyjson_read(wire,strlen(wire),0) : NULL;
        yyjson_val *body=yyjson_doc_get_root(saved);
        if (!strcmp(name,"approval_status") && yyjson_obj_get(body,"error")) {
            char *history=sb_store_history(host->store,actor,id,false,0,1);
            yyjson_doc *old=history ? yyjson_read(history,strlen(history),0) : NULL;
            yyjson_val *record=yyjson_arr_get(yyjson_obj_get(yyjson_doc_get_root(old),"records"),0);
            const char *kind=sb_string(yyjson_obj_get(record,"record_kind"));
            if (kind && !strcmp(kind,"approval")) {
                yyjson_mut_val *result=content_result(doc,yyjson_val_mut_copy(doc,record),false);
                yyjson_doc_free(old); free(history); yyjson_doc_free(saved); free(wire); return result;
            }
            yyjson_doc_free(old); free(history);
        }
        yyjson_mut_val *result=body ? content_result(doc,yyjson_val_mut_copy(doc,body),yyjson_obj_get(body,"error")!=NULL) : failure(doc,"Approval state unavailable.");
        yyjson_doc_free(saved); free(wire); return result;
    }
    const char *id = sb_string(yyjson_obj_get(args,"workspace_id")), *path = sb_string(yyjson_obj_get(args,"path"));
    if (!strcmp(name,"task_start") && sb_store_task_repeated(host->store,actor,sb_string(yyjson_obj_get(args,"request_key"))))
        return failure(doc,"This request_key belongs to a retained task from an earlier backend instance. Review owner history and actual effects before explicitly using a new key. No task was replayed.");
    bool file_tool = !strncmp(name,"fs_",3) || !strcmp(name,"image_view") || sb_document_tool(name);
    sb_workspace account = {0}, *workspace = id ? sb_workspace_find(&host->config,id) : NULL;
    const sb_project_directory *directory = sb_project_directory_find(project,id);
    char *relative = NULL; sb_error error = {0}; yyjson_mut_val *result = NULL;
    if (file_tool && !id && project->policy.file_scope == SB_FILES_CURRENT_USER) {
        account.root = path ? sb_platform_account_parent(path,&relative,&error) : NULL;
        if (!account.root) return failure(doc,*error.text ? error.text : "An absolute account path is required.");
        strcpy(account.id,"current-user"); account.read_only=project->policy.current_user_read_only; workspace=&account;
        for(size_t i=0;i<host->config.workspace_count;++i) {
            bool inside=sb_platform_target_within(host->config.workspaces[i].root,account.root,relative,&error);
            if(*error.text){result=failure(doc,error.text);goto done;}
            if(inside){directory=sb_project_directory_find(project,host->config.workspaces[i].id); account.read_only=host->config.workspaces[i].read_only;break;}
        }
        path=relative;
    }
    sb_request_context context = {.actor=actor,.project=project,.policy_revision=project->policy_revision};
    sb_project_operation operation = {.tool=name,.directory=directory,.current_user_target=file_tool && workspace==&account && !directory,
        .identity=administrator ? SB_ID_ADMINISTRATOR : selected_identity ? (!strcmp(selected_identity,"restricted") ? SB_ID_RESTRICTED : SB_ID_CURRENT_USER) :
            ((!strcmp(name,"task_start") || !strcmp(name,"task_input")) ? project->policy.default_identity : SB_ID_CURRENT_USER),
        .restricted_backend_verified=sb_execution_restricted_verified(host->execution),
        .administrator_backend_verified=host->administrator_host,
        .owned_task=sb_execution_task_owned(host->execution,actor,sb_string(yyjson_obj_get(args,"task_id")))};
    sb_policy_result decision=sb_project_evaluate(&context,&operation);
    if(decision.decision==SB_DECISION_DENY) { result=failure(doc,decision.reason);goto done; }
    if(!strcmp(name,"task_start") && !sb_execution_program_permitted(host->execution,sb_string(yyjson_obj_get(args,"executable")))) {
        result=failure(doc,"Restricted executable must be an owner-configured runtime_file.");goto done;
    }
    if(!file_tool && strcmp(name,"task_start") && strcmp(name,"task_input")) { result=tool_call_inner(host,actor,name,args,doc,NULL,NULL,true);goto done; }
    if(file_tool && !workspace) { result=failure(doc,"Unknown workspace_id.");goto done; }
    if(file_tool && decision.decision==SB_DECISION_ALLOW && strcmp(name,"fs_write") && strcmp(name,"fs_patch") && !yyjson_obj_get(args,"approval_id")) {
        result=tool_call_inner(host,actor,name,args,doc,workspace==&account ? &account : NULL,workspace==&account ? path : NULL,true);goto done;
    }
    if(!path) path=".";
    bool directory_target=!strcmp(name,"fs_list") || !strcmp(name,"fs_search") || !strcmp(name,"task_start");
    bool create=!strcmp(name,"fs_write") && !yyjson_get_bool(yyjson_obj_get(args,"overwrite"));
    char version[65]={0}, target_version[65]={0},program_version[65]={0}, target[256],ticket[33]={0};
    char parent_id[50]={0};
    if(file_tool || !strcmp(name,"task_start")) {
        if (!workspace || !sb_platform_root_identity(workspace->root,parent_id,&error) ||
            (!strcmp(name,"task_start") ? !command_directory_version(workspace->root,path,version,&error) :
                !sb_platform_target_version(workspace->root,path,directory_target,create,!strcmp(name,"fs_search") && yyjson_get_bool(yyjson_obj_get(args,"recursive")),version,&error))) {
            result=failure(doc,*error.text ? error.text : "Cannot resolve physical operation target.");goto done;
        }
        char path_hash[65]; if(!sb_platform_sha256(path,strlen(path),path_hash,&error)) { result=failure(doc,error.text);goto done; }
        snprintf(target,sizeof(target),"%s:%s",parent_id,path_hash);
    } else { snprintf(target,sizeof(target),"task:%s",sb_string(yyjson_obj_get(args,"task_id"))); strcpy(version,sb_execution_instance(host->execution)); }
    strcpy(target_version,version);
    if(!strcmp(name,"task_start")) {
        char *program_relative=NULL; sb_root *program=sb_platform_account_parent(sb_string(yyjson_obj_get(args,"executable")),&program_relative,&error);
        bool ok=program && sb_platform_target_version(program,program_relative,false,false,false,program_version,&error);
        sb_platform_root_close(program);free(program_relative);
        char pair[132];snprintf(pair,sizeof(pair),"%s|%s",version,ok?program_version:"");
        if(!ok || !sb_platform_sha256(pair,strlen(pair),version,&error)) { result=failure(doc,error.text);goto done; }
    }
    uint64_t offset=0,budget=1;
    if(!strcmp(name,"fs_read") || !strcmp(name,"document_read")) { if(!number(args,"offset",0,0,INT64_MAX,&offset)||!number(args,"max_bytes",SB_TEXT_LIMIT,4,SB_TEXT_LIMIT,&budget)){result=failure(doc,"Invalid file page.");goto done;} }
    else if(!strcmp(name,"fs_list")) { if(!number(args,"offset",0,0,10000,&offset)||!number(args,"max_entries",100,1,200,&budget)){result=failure(doc,"Invalid directory page.");goto done;} }
    else if(!strcmp(name,"fs_search")) { if(!number(args,"max_total_bytes",16777216,4,16777216,&budget)){result=failure(doc,"Invalid search budget.");goto done;} }
    else if(!strcmp(name,"image_view"))budget=SB_IMAGE_INPUT_LIMIT;
    else if(sb_document_tool(name))budget=SB_DOCUMENT_INPUT_LIMIT;
    else if(!strcmp(name,"fs_write") || !strcmp(name,"fs_patch"))budget=SB_TEXT_LIMIT;
    if(!file_tool && decision.decision==SB_DECISION_ALLOW && !yyjson_obj_get(args,"approval_id")) {
        host->approved_target_version=target_version;host->approved_program_version=*program_version?program_version:NULL;
        result=tool_call_inner(host,actor,name,args,doc,NULL,NULL,true);
        host->approved_target_version=NULL;host->approved_program_version=NULL;goto done;
    }
    char *parameters=operation_parameters(name,args); yyjson_mut_val *reply=NULL;
    bool allowed=sb_policy_operation_begin(host,actor,name,parameters,target,version,sb_string(yyjson_obj_get(args,"approval_id")),decision.decision==SB_DECISION_PENDING,offset,budget,doc,&reply,ticket);
    free(parameters);
    if(!allowed) { result=yyjson_mut_obj_get(reply,"isError") ? reply : content_result(doc,reply,true);goto done; }
    host->approved_target_version=target_version;host->approved_program_version=*program_version?program_version:NULL;
    result=tool_call_inner(host,actor,name,args,doc,workspace==&account ? &account : NULL,workspace==&account ? path : NULL,true);
    host->approved_target_version=NULL;host->approved_program_version=NULL;
    /* Content is kept in the worker until the physical snapshot is checked again. */
    if(file_tool && strncmp(name,"fs_write",8) && strcmp(name,"fs_patch") && !yyjson_mut_get_bool(yyjson_mut_obj_get(result,"isError"))) {
        char after[65]; if(!sb_platform_target_version(workspace->root,path,directory_target,false,!strcmp(name,"fs_search") && yyjson_get_bool(yyjson_obj_get(args,"recursive")),after,&error) || strcmp(version,after))
            result=failure(doc,"Physical target changed while accessing it; no content returned.");
    }
    yyjson_mut_val *data=yyjson_mut_obj_get(result,"structuredContent");
    yyjson_mut_obj_add_strcpy(doc,data,"approval_id",ticket);
    if (!sb_policy_operation_finish(host->policy,ticket,result)) yyjson_mut_obj_add_str(doc,data,"audit_warning","The result is authoritative; final persistent recording is incomplete.");
done:
    if(result) {
        yyjson_mut_val *metadata=yyjson_mut_obj_get(result,"structuredContent");
        if(metadata && !yyjson_mut_obj_get(metadata,"project_id"))yyjson_mut_obj_add_str(doc,metadata,"project_id",project->id);
        if(metadata && !yyjson_mut_obj_get(metadata,"policy_revision"))yyjson_mut_obj_add_uint(doc,metadata,"policy_revision",project->policy_revision);
        yyjson_mut_val *text=yyjson_mut_arr_get(yyjson_mut_obj_get(result,"content"),0);
        char *serialized=metadata?yyjson_mut_val_write(metadata,0,NULL):NULL;
        if(serialized && text){yyjson_mut_obj_remove_str(text,"text");yyjson_mut_obj_add_strcpy(doc,text,"text",serialized);}free(serialized);
    }
    free(relative);sb_platform_root_close(account.root);return result;
}

yyjson_mut_val *sb_tool_call(sb_host *host, const char *actor, const char *name, yyjson_val *args, yyjson_mut_doc *doc) {
    bool file=host->project && !host->project->legacy_compatibility && (!strncmp(name,"fs_",3) || !strcmp(name,"image_view") || sb_document_tool(name));
    const char *identity=sb_string(yyjson_obj_get(args,"identity"));
    /* The ordinary forwarding worker cannot claim it accessed admin content. */
    if (file && identity && !strcmp(identity,"administrator") && !host->administrator_host) file=false;
    char hash[65]={0}; sb_error error={0};
    char *parameters=file ? yyjson_val_write(args,0,NULL) : NULL;
    bool started=!file || (parameters && sb_platform_sha256(parameters,strlen(parameters),hash,&error) &&
        sb_store_healthy(host->store) && sb_audit_operation(host->audit,"file_call_started",actor,host->project->id,host->administrator_host ? "administrator" : "ordinary",name,hash));
    free(parameters);
    if (!started) return failure(doc,"Persistent state or audit unavailable; file access was not started.");
    yyjson_mut_val *result=project_tool_call(host,actor,name,args,doc);
    if (file && !sb_audit_operation(host->audit,yyjson_mut_get_bool(yyjson_mut_obj_get(result,"isError")) ? "file_call_blocked" : "file_call_completed",
        actor,host->project->id,host->administrator_host ? "administrator" : "ordinary",name,hash)) {
        yyjson_mut_val *data=yyjson_mut_obj_get(result,"structuredContent");
        yyjson_mut_obj_add_str(doc,data,"audit_warning","The result is authoritative; the final audit record could not be flushed.");
    }
    return result;
}
