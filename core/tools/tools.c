#include "sb.h"
#include "sb_projects.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *name; const char *description; const char *schema; } tool_definition;
#define FS_LOCATION "\"workspace_id\":{\"type\":\"string\"},\"path\":{\"type\":\"string\",\"minLength\":1},\"directory\":{\"type\":\"boolean\",\"default\":false}"
#define FS_CHANGE ",\"request_key\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":64}"
#define FS_VERSION ",\"expected_version\":{\"type\":\"string\",\"minLength\":64,\"maxLength\":64}"
#define FS_DEST ",\"destination_workspace_id\":{\"type\":\"string\"},\"destination_path\":{\"type\":\"string\",\"minLength\":1}"
static const tool_definition definitions[] = {
    {"fs_stat","Inspect a file or directory and return its physical version for subsequent native file operations. Set directory:true for directories. No file content is read.","{\"type\":\"object\",\"properties\":{" FS_LOCATION "},\"required\":[\"workspace_id\",\"path\"],\"additionalProperties\":false}"},
    {"fs_mkdir","Create one new directory in an existing authorized parent. No overwrite or recursive parent creation. Keep request_key and approval_id on retries.","{\"type\":\"object\",\"properties\":{" FS_LOCATION FS_CHANGE "},\"required\":[\"workspace_id\",\"path\",\"request_key\"],\"additionalProperties\":false}"},
    {"fs_copy","Copy a binary file within max_transfer_bytes from host_capabilities to a new destination. Source may be read-only; destination must be writable. Supply expected_version from fs_stat. Destination workspace defaults to source workspace. No directory-tree copy or overwrite.","{\"type\":\"object\",\"properties\":{" FS_LOCATION FS_CHANGE FS_VERSION FS_DEST "},\"required\":[\"workspace_id\",\"path\",\"destination_path\",\"expected_version\",\"request_key\"],\"additionalProperties\":false}"},
    {"fs_move","Rename/move a file or directory on the same volume to a new destination. Both locations must permit writes. Supply expected_version from fs_stat; set directory:true for a directory. No overwrite; cross-volume files require explicit copy then delete.","{\"type\":\"object\",\"properties\":{" FS_LOCATION FS_CHANGE FS_VERSION FS_DEST "},\"required\":[\"workspace_id\",\"path\",\"destination_path\",\"expected_version\",\"request_key\"],\"additionalProperties\":false}"},
    {"fs_delete","Delete one file within max_transfer_bytes from host_capabilities after retaining a private binary backup, or remove an empty directory with directory:true. Supply expected_version from fs_stat. No recursive deletion. backup_id identifies the retained original bytes.","{\"type\":\"object\",\"properties\":{" FS_LOCATION FS_CHANGE FS_VERSION "},\"required\":[\"workspace_id\",\"path\",\"expected_version\",\"request_key\"],\"additionalProperties\":false}"},
    {"artifact_download", "Transfer an authorized binary file within max_transfer_bytes from host_capabilities in bounded Base64 chunks. Versioned projects only. open requires workspace_id/path and request_key; optional expected_sha256/ttl_seconds. Approval precedes all source bytes. read requires transfer_id and offset, optionally max_bytes up to max_transfer_chunk_bytes; continue at next_offset, retry only the last chunk. status/cancel require transfer_id. Bound to caller, project and backend; no anonymous URL or client attachment integration.", "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"open\",\"read\",\"status\",\"cancel\"]},\"workspace_id\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"request_key\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":64},\"transfer_id\":{\"type\":\"string\",\"minLength\":32,\"maxLength\":32},\"offset\":{\"type\":\"integer\",\"minimum\":0},\"expected_sha256\":{\"type\":\"string\",\"minLength\":64,\"maxLength\":64},\"ttl_seconds\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":900},\"max_bytes\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":32768}},\"required\":[\"action\"],\"additionalProperties\":false}"},
    {"artifact_upload", "Upload a binary file within max_transfer_bytes from host_capabilities to a NEW authorized destination. Versioned projects only, write permission mandatory. open requires workspace_id/path, request_key, exact size and expected_sha256 before approval/staging. write requires transfer_id, offset, canonical data_base64 up to max_transfer_chunk_bytes decoded bytes. Append at next_offset; retry only identical last bytes. commit verifies complete size/SHA-256 then creates the file, never overwrites. status/cancel require transfer_id. Expiry/revoke/restart cannot resume staging.", "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"open\",\"write\",\"commit\",\"status\",\"cancel\"]},\"workspace_id\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"request_key\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":64},\"transfer_id\":{\"type\":\"string\",\"minLength\":32,\"maxLength\":32},\"offset\":{\"type\":\"integer\",\"minimum\":0},\"expected_sha256\":{\"type\":\"string\",\"minLength\":64,\"maxLength\":64},\"ttl_seconds\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":900},\"size\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":67108864},\"data_base64\":{\"type\":\"string\",\"maxLength\":43692}},\"required\":[\"action\"],\"additionalProperties\":false}"},
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
    {"image_view", "Fully decode a bounded PNG/JPEG and return native MCP image content. Optional max_dimension from 128 to max_image_dimension shrinks proportionally without enlarging. Read source/output byte and pixel budgets from host_capabilities. Larger sources are normalized to a bounded PNG, possibly smaller than requested. Original bytes are preserved when no processing is needed; derivatives omit source metadata and use stored raster orientation.",
        "{\"type\":\"object\",\"properties\":{\"workspace_id\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"max_dimension\":{\"type\":\"integer\",\"minimum\":128,\"maximum\":4096}},\"required\":[\"workspace_id\",\"path\"],\"additionalProperties\":false}"}
,
    {"task_start", "Start a background native executable in explicitly enabled user or administrator mode. Requires a caller request_key; retries return the original task and changed requests are rejected. Workspace locates cwd; arbitrary commands retain the selected account permissions and are not confined by file roots. Windows command lines must fit 32766 UTF-16 units after quoting; write large scripts with fs_write/artifact_upload then pass the script path, or use task_input. Supply current host_instance_id from host_capabilities; stale requests after restart are rejected.", "{\"type\":\"object\",\"properties\":{\"workspace_id\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"executable\":{\"type\":\"string\"},\"argv\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"maxItems\":64},\"request_key\":{\"type\":\"string\"},\"timeout_ms\":{\"type\":\"integer\",\"minimum\":100,\"maximum\":3600000,\"default\":300000},\"host_instance_id\":{\"type\":\"string\",\"minLength\":32,\"maxLength\":32}},\"required\":[\"workspace_id\",\"executable\",\"argv\",\"request_key\",\"host_instance_id\"],\"additionalProperties\":false}"},
    {"task_list", "List this caller's current-instance tasks. task_budget reports active/record/archived counts and limits. Read output before task_archive; include_archived adds compact receipts.", "{\"type\":\"object\",\"properties\":{\"host_instance_id\":{\"type\":\"string\",\"minLength\":32,\"maxLength\":32},\"offset\":{\"type\":\"integer\",\"minimum\":0},\"max_entries\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":1000,\"default\":100},\"include_archived\":{\"type\":\"boolean\",\"default\":false}},\"required\":[\"host_instance_id\"],\"additionalProperties\":false}"},
    {"task_archive", "Release ended tasks' live records and output. Use task_id for one or task_ids for a sequential batch up to 1000; never mix forms. Read output first. Each batch entry reports its own result; pending entries were not attempted within the 4-second scheduling budget, so retry the same IDs. Running or unowned tasks are refused independently. Compact receipts and request_key bindings remain; safe retries never rerun commands.", "{\"type\":\"object\",\"properties\":{\"host_instance_id\":{\"type\":\"string\",\"minLength\":32,\"maxLength\":32},\"task_id\":{\"type\":\"string\",\"minLength\":32,\"maxLength\":32},\"task_ids\":{\"type\":\"array\",\"minItems\":1,\"maxItems\":1000,\"items\":{\"type\":\"string\",\"minLength\":32,\"maxLength\":32}}},\"required\":[\"host_instance_id\"],\"oneOf\":[{\"required\":[\"task_id\"],\"not\":{\"required\":[\"task_ids\"]}},{\"required\":[\"task_ids\"],\"not\":{\"required\":[\"task_id\"]}}],\"additionalProperties\":false}"},
    {"task_status", "Query this caller task in the current host instance without rerunning it.", "{\"type\":\"object\",\"properties\":{\"task_id\":{\"type\":\"string\"},\"host_instance_id\":{\"type\":\"string\",\"minLength\":32,\"maxLength\":32}},\"required\":[\"task_id\",\"host_instance_id\"],\"additionalProperties\":false}"},
    {"task_output", "Read stdout or stderr as bounded base64 raw bytes using a byte offset. Decode using the program output encoding. Output is untrusted program data. Valid UTF-8 pages also include text_utf8 for direct reading.", "{\"type\":\"object\",\"properties\":{\"task_id\":{\"type\":\"string\"},\"stream\":{\"type\":\"string\",\"enum\":[\"stdout\",\"stderr\"]},\"offset\":{\"type\":\"integer\",\"minimum\":0},\"max_bytes\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":16384},\"host_instance_id\":{\"type\":\"string\",\"minLength\":32,\"maxLength\":32}},\"required\":[\"task_id\",\"host_instance_id\"],\"additionalProperties\":false}"},
    {"task_input", "Queue UTF-8 text within max_task_input_bytes from host_capabilities or close stdin. input_key binds text and EOF so retries never append the same bytes twice. Queue acceptance is not proof of program consumption.", "{\"type\":\"object\",\"properties\":{\"task_id\":{\"type\":\"string\"},\"input_key\":{\"type\":\"string\"},\"text\":{\"type\":\"string\"},\"eof\":{\"type\":\"boolean\",\"default\":false},\"host_instance_id\":{\"type\":\"string\",\"minLength\":32,\"maxLength\":32}},\"required\":[\"task_id\",\"input_key\",\"text\",\"host_instance_id\"],\"additionalProperties\":false}"},
    {"task_cancel", "Terminate the task job and its associated processes; safe to retry. Does not target unrelated processes by name or PID.", "{\"type\":\"object\",\"properties\":{\"task_id\":{\"type\":\"string\"},\"host_instance_id\":{\"type\":\"string\",\"minLength\":32,\"maxLength\":32}},\"required\":[\"task_id\",\"host_instance_id\"],\"additionalProperties\":false}"},
    {"approval_status", "Query only this caller's approval metadata, including interrupted historical requests. Does not expose the owner credential or request body and cannot approve.", "{\"type\":\"object\",\"properties\":{\"approval_id\":{\"type\":\"string\",\"minLength\":32,\"maxLength\":32}},\"required\":[\"approval_id\"],\"additionalProperties\":false}"},
    {"approval_cancel", "Cancel only this caller's pending approval or remaining read permit; safe to retry. Does not approve or roll back effects.", "{\"type\":\"object\",\"properties\":{\"approval_id\":{\"type\":\"string\",\"minLength\":32,\"maxLength\":32}},\"required\":[\"approval_id\"],\"additionalProperties\":false}"},
    {"document_info", "Inspect an approved PDF/DOCX/XLSX/PPTX and list up to 128 document units. PDF units are pages, DOCX has one text document, XLSX units are worksheets and PPTX units are slides. Does not edit the source or execute macros. Legacy DOC/XLS/PPT must be converted first.", "{\"type\":\"object\",\"properties\":{\"workspace_id\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}},\"required\":[\"workspace_id\",\"path\"],\"additionalProperties\":false}"},
    {"document_read", "Read bounded extracted UTF-8 text from one approved document unit, numbered from 1. Continue at next_offset with identical arguments and approval_id. PDF text follows object order, with no OCR. XLSX returns cell references and stored values, never recalculates formulas. Office XML text is not original page layout. Read source and extracted unit budgets from host_capabilities.", "{\"type\":\"object\",\"properties\":{\"workspace_id\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"unit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":4096,\"default\":1},\"offset\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":1048576},\"max_bytes\":{\"type\":\"integer\",\"minimum\":4,\"maximum\":65536}},\"required\":[\"workspace_id\",\"path\"],\"additionalProperties\":false}"},
    {"document_preview", "Return a bounded native PNG image of one approved PDF page or an explicitly labelled Office structured text preview. Office previews do not reproduce Word pagination, spreadsheet formatting, slide graphics or original layout. Unit numbers start at 1. Source is unchanged; no macros, external resources or formula execution.", "{\"type\":\"object\",\"properties\":{\"workspace_id\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"unit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":4096,\"default\":1},\"max_dimension\":{\"type\":\"integer\",\"minimum\":128,\"maximum\":4096,\"default\":1600}},\"required\":[\"workspace_id\",\"path\"],\"additionalProperties\":false}"}
};

/* 工具发现和参数验证必须使用同一份容量，避免说明已放宽但实际仍被旧 schema 拒绝。 */
static void schema_bound(yyjson_mut_doc *doc, yyjson_mut_val *properties, const char *field, const char *key, uint64_t maximum) {
    yyjson_mut_val *property=yyjson_mut_obj_get(properties,field);
    if (!property) return;
    yyjson_mut_obj_remove_str(property,key); yyjson_mut_obj_add_uint(doc,property,key,maximum);
}
static yyjson_doc *tool_schema(const tool_definition *definition) {
    yyjson_doc *input=yyjson_read(definition->schema,strlen(definition->schema),0);
    yyjson_mut_doc *doc=input ? yyjson_doc_mut_copy(input,NULL) : NULL; yyjson_doc_free(input);
    if (!doc) return NULL;
    yyjson_mut_val *properties=yyjson_mut_obj_get(yyjson_mut_doc_get_root(doc),"properties");
    const char *name=definition->name;
    schema_bound(doc,properties,"workspace_id","maxLength",63);schema_bound(doc,properties,"task_id","maxLength",32);
    schema_bound(doc,properties,"request_key","maxLength",64);schema_bound(doc,properties,"input_key","maxLength",64);
    if (!strcmp(name,"fs_read") || !strcmp(name,"document_read")) schema_bound(doc,properties,"max_bytes","maximum",SB_READ_LIMIT);
    if (!strcmp(name,"document_read")) schema_bound(doc,properties,"offset","maximum",SB_DOCUMENT_TEXT_LIMIT);
    if (!strcmp(name,"image_view") || !strcmp(name,"document_preview")) schema_bound(doc,properties,"max_dimension","maximum",SB_IMAGE_DIMENSION);
    if (!strcmp(name,"artifact_download") || !strcmp(name,"artifact_upload")) {
        schema_bound(doc,properties,"max_bytes","maximum",SB_TRANSFER_CHUNK);
        schema_bound(doc,properties,"data_base64","maxLength",4*((SB_TRANSFER_CHUNK+2)/3));
        schema_bound(doc,properties,"size","maximum",SB_TRANSFER_LIMIT);
        schema_bound(doc,properties,"ttl_seconds","maximum",SB_TRANSFER_MAX_TTL);
    }
    if (!strcmp(name,"task_start")) schema_bound(doc,properties,"argv","maxItems",SB_TASK_ARG_LIMIT);
    if (!strcmp(name,"task_output")) schema_bound(doc,properties,"max_bytes","maximum",SB_TASK_OUTPUT_PAGE_LIMIT);
#ifndef SB_POSIX
    const char *mode_schema="{\"type\":\"string\",\"enum\":[\"full\",\"compact\"],\"default\":\"full\",\"description\":\"compact retains structuredContent and media but replaces duplicated JSON text content with one summary; does not change authorization or request-key identity.\"}";
    yyjson_doc *mode=yyjson_read(mode_schema,strlen(mode_schema),0);
    yyjson_mut_obj_add_val(doc,properties,"response_mode",yyjson_val_mut_copy(doc,yyjson_doc_get_root(mode)));yyjson_doc_free(mode);
    if (!strcmp(name,"task_output")) {
        yyjson_mut_val *encoding=yyjson_mut_obj(doc);yyjson_mut_obj_add_str(doc,encoding,"type","string");
        yyjson_mut_obj_add_str(doc,encoding,"description","Optional strict text decoding: utf-8, cp936/gbk, or a supported Windows cpNNNN. Raw bytes and next_offset remain unchanged; use text_next_offset to continue decoded text.");
        yyjson_mut_obj_add_val(doc,properties,"encoding",encoding);
        const char *payload_schema="{\"type\":\"string\",\"enum\":[\"text\",\"bytes\",\"both\"],\"description\":\"Select a single decoded text field (UTF-8 by default), only raw Base64 bytes, or both. Omit for legacy output. Text mode keeps raw bytes in the task and uses text_next_offset for continuation.\"}";
        yyjson_doc *payload=yyjson_read(payload_schema,strlen(payload_schema),0);
        yyjson_mut_obj_add_val(doc,properties,"payload_mode",yyjson_val_mut_copy(doc,yyjson_doc_get_root(payload)));yyjson_doc_free(payload);
    }
#endif
    if (!strcmp(name,"task_input")) schema_bound(doc,properties,"text","maxLength",SB_TASK_INPUT_LIMIT);
    if (!strcmp(name,"fs_write")) schema_bound(doc,properties,"text","maxLength",SB_EDIT_LIMIT);
    if (!strcmp(name,"fs_patch")) {schema_bound(doc,properties,"search","maxLength",SB_EDIT_LIMIT);schema_bound(doc,properties,"replacement","maxLength",SB_EDIT_LIMIT);}
    if (!strcmp(name,"task_start") || !strcmp(name,"task_input")) schema_bound(doc,properties,!strcmp(name,"task_start")?"request_key":"input_key","maxLength",64);
    if (!strcmp(name,"fs_list")) {
        schema_bound(doc,properties,"offset","maximum",SB_LIST_OFFSET_LIMIT);
        schema_bound(doc,properties,"max_entries","maximum",SB_LIST_LIMIT);
    }
    if (!strcmp(name,"fs_search")) {
        schema_bound(doc,properties,"query","maxLength",512);
        schema_bound(doc,properties,"max_results","maximum",SB_SEARCH_RESULTS);
        schema_bound(doc,properties,"max_file_bytes","maximum",SB_SEARCH_FILE_LIMIT);
        schema_bound(doc,properties,"max_total_bytes","maximum",SB_SEARCH_TOTAL_LIMIT);
    }
    yyjson_doc *result=yyjson_mut_doc_imut_copy(doc,NULL); yyjson_mut_doc_free(doc); return result;
}
static size_t tool_count(void) { return sizeof(definitions) / sizeof(definitions[0]); }

static bool platform_tool_available(const char *name) {
#ifdef SB_POSIX
    if (name && (!strcmp(name,"task_list") || !strcmp(name,"task_archive"))) return false;
    return name && (!strncmp(name,"fs_",3) || !strncmp(name,"host_",5) || !strncmp(name,"approval_",9) || !strncmp(name,"task_",5) || sb_desktop_tool(name) || sb_document_tool(name) || !strcmp(name,"image_view") || !strcmp(name,"artifact_download") || !strcmp(name,"artifact_upload"));
#else
    (void)name; return true;
#endif
}

bool sb_tool_exists(const char *name) {
    if (!platform_tool_available(name)) return false;
    if (sb_desktop_tool(name)) return true;
    for (size_t i = 0; i < tool_count(); ++i) if (!strcmp(name, definitions[i].name)) return true;
    return false;
}
bool sb_file_manage_tool(const char *name) {
    return name && (!strcmp(name,"fs_stat") || !strcmp(name,"fs_mkdir") || !strcmp(name,"fs_copy") || !strcmp(name,"fs_move") || !strcmp(name,"fs_delete"));
}

static yyjson_mut_val *tools_list(yyjson_mut_doc *doc, bool approvals) {
    yyjson_mut_val *result = yyjson_mut_obj(doc), *array = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, result, "tools", array);
    for (size_t i = 0; i < tool_count(); ++i) {
        if (!platform_tool_available(definitions[i].name)) continue;
        if (!approvals && !strncmp(definitions[i].name,"approval_",9)) continue;
        if (!approvals && sb_transfer_tool(definitions[i].name)) continue;
        if (!approvals && sb_file_manage_tool(definitions[i].name)) continue;
        yyjson_mut_val *tool = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, tool, "name", definitions[i].name);
        yyjson_mut_obj_add_str(doc, tool, "description", definitions[i].description);
        yyjson_doc *schema = tool_schema(&definitions[i]);
        if (!schema) return NULL;
        yyjson_mut_obj_add_val(doc, tool, "inputSchema", yyjson_val_mut_copy(doc, yyjson_doc_get_root(schema)));
        yyjson_doc_free(schema);
        yyjson_mut_val *annotations = yyjson_mut_obj(doc);
        bool task_tool=!strncmp(definitions[i].name,"task_",5);
        bool modifying = (sb_file_manage_tool(definitions[i].name) && strcmp(definitions[i].name,"fs_stat")) || !strcmp(definitions[i].name,"artifact_upload") || !strcmp(definitions[i].name, "fs_write") || !strcmp(definitions[i].name, "fs_patch") || !strcmp(definitions[i].name,"task_start") || !strcmp(definitions[i].name,"task_input") || !strcmp(definitions[i].name,"task_cancel") || !strcmp(definitions[i].name,"task_archive") || !strcmp(definitions[i].name,"approval_cancel");
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
    if (!platform_tool_available(name)) return false;
    if (sb_transfer_tool(name) && (!host->project || host->project->legacy_compatibility)) return false;
    if (sb_file_manage_tool(name) && (!host->project || host->project->legacy_compatibility)) return false;
    if (!host->project) return true;
    int index = sb_project_tool_index(name);
    if (index >= SB_DESKTOP_TOOL_FIRST && index < SB_DESKTOP_TOOL_END) return sb_desktop_tool_visible(host,name);
    /* Result polling and cancellation remain visible for already owned tasks. */
    return index >= 0 && ((index == 9 || index == 10 || index == 12 || index==13 || index==14 || index==37 || index==38) ||
        (host->project->enabled && (host->project->policy.tools & (UINT64_C(1) << index))));
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
        else if (!host->project->legacy_compatibility && (strncmp(name,"fs_",3) == 0 || !strcmp(name,"image_view") || sb_document_tool(name) || sb_transfer_tool(name) || !strncmp(name,"task_",5) || !strncmp(name,"approval_",9))) {
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
            {
                const char *description=yyjson_mut_get_str(yyjson_mut_obj_get(tool,"description"));
                if (!description) return NULL;
                snprintf(document_description,sizeof(document_description),"%s %s",description,policy_description);
            }
            yyjson_mut_obj_remove_str(tool,"description");
            yyjson_mut_obj_add_strcpy(doc,tool,"description",document_description);
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

/* 在具体类型/策略检查之前定位超限字段，同样覆盖旧 stdio 入口。 */
static yyjson_mut_val *argument_limit(const tool_definition *definition,yyjson_val *args,yyjson_mut_doc *doc,const sb_host *host) {
    yyjson_doc *schema=tool_schema(definition);if(!schema)return NULL;
    yyjson_val *properties=yyjson_obj_get(yyjson_doc_get_root(schema),"properties");
    yyjson_mut_val *error=NULL;size_t i,n;yyjson_val *key,*value;
    yyjson_obj_foreach(args,i,n,key,value) {
        const char *field=sb_string(key);if(!field)continue;
        yyjson_val *property=yyjson_obj_get(properties,field);uint64_t actual=0,limit=0;const char *unit=NULL;
        if(yyjson_is_str(value) && sb_integer(yyjson_obj_get(property,"maxLength"),&limit)){actual=yyjson_get_len(value);unit="UTF-8 bytes";}
        else if(yyjson_is_arr(value) && sb_integer(yyjson_obj_get(property,"maxItems"),&limit)){actual=yyjson_arr_size(value);unit="items";}
        else if(sb_integer(value,&actual) && sb_integer(yyjson_obj_get(property,"maximum"),&limit)){
            unit=!strcmp(field,"timeout_ms")?"ms":!strcmp(field,"ttl_seconds")?"seconds":!strcmp(field,"max_dimension")?"pixels":
                strstr(field,"bytes") || !strcmp(field,"size") || (!strcmp(field,"offset") && strcmp(definition->name,"fs_list")) ? "bytes" : "items";
            if(!strcmp(field,"timeout_ms") && host->config.limits.task_timeout_ms<limit)limit=host->config.limits.task_timeout_ms;
        }
        if(unit && actual>limit){error=sb_limit_error(doc,field,actual,limit,unit);break;}
    }
    yyjson_doc_free(schema);return error ? content_result(doc,error,true) : NULL;
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
    yyjson_doc *schema = tool_schema(definition);
    if (!schema) return false;
    yyjson_val *properties = yyjson_obj_get(yyjson_doc_get_root(schema), "properties");
    bool valid = true;
    size_t idx, max;
    yyjson_val *key, *value;
    yyjson_obj_foreach(args, idx, max, key, value) {
        (void)value;
        const char *name = sb_string(key);
        bool project_approval = host->project && !host->project->legacy_compatibility && name && !strcmp(name,"approval_id") &&
            (strncmp(definition->name,"fs_",3) == 0 || !strcmp(definition->name,"image_view") || sb_document_tool(definition->name) || sb_transfer_tool(definition->name) || !strcmp(definition->name,"task_start") || !strcmp(definition->name,"task_input"));
        bool project_identity = host->project && !host->project->legacy_compatibility && name && !strcmp(name,"identity") &&
            (strncmp(definition->name,"fs_",3) == 0 || !strcmp(definition->name,"image_view") || sb_document_tool(definition->name) || sb_transfer_tool(definition->name) || !strncmp(definition->name,"task_",5) || !strncmp(definition->name,"approval_",9));
        if (!name || (!yyjson_obj_get(properties, name) && !project_approval && !project_identity)) { valid = false; break; }
        yyjson_obj_iter it = yyjson_obj_iter_with(args);
        for (size_t j = 0; j < idx; ++j) if (!strcmp(name, yyjson_get_str(yyjson_obj_iter_next(&it)))) valid = false;
    }
    yyjson_doc_free(schema);
    if (!strcmp(definition->name,"image_view")) {
        uint64_t maximum=0;
        if (!number(args,"max_dimension",0,128,SB_IMAGE_DIMENSION,&maximum)) valid=false;
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
    yyjson_mut_val *limit_reply=argument_limit(definition,args,doc,host);if(limit_reply)return limit_reply;
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
#ifdef SB_POSIX
        const bool media_available = true;
        const bool account_files_available = true;
#else
        const bool media_available = true;
        const bool account_files_available = true;
#endif
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
        yyjson_mut_obj_add_uint(doc, data, "max_text_bytes", SB_READ_LIMIT);
        yyjson_mut_obj_add_uint(doc, data, "max_image_bytes", SB_IMAGE_LIMIT);
        yyjson_mut_obj_add_uint(doc, data, "max_image_source_bytes", SB_IMAGE_INPUT_LIMIT);
        yyjson_mut_obj_add_uint(doc, data, "max_image_source_pixels", SB_IMAGE_PIXEL_LIMIT);
        yyjson_mut_obj_add_uint(doc, data, "max_image_source_dimension", 32768);
        yyjson_mut_obj_add_uint(doc, data, "max_image_dimension", SB_IMAGE_DIMENSION);
        yyjson_mut_obj_add_bool(doc, data, "image_full_decode", media_available);
        yyjson_mut_obj_add_bool(doc, data, "image_resize", media_available);
        yyjson_mut_obj_add_bool(doc, data, "file_write", true);
        yyjson_mut_obj_add_bool(doc, data, "file_overwrite", true);
        yyjson_mut_obj_add_bool(doc, data, "file_patch", true);
        yyjson_mut_obj_add_bool(doc, data, "atomic_overwrite", false);
        yyjson_mut_obj_add_uint(doc, data, "max_edit_bytes", SB_EDIT_LIMIT);
        yyjson_mut_obj_add_uint(doc,data,"max_request_bytes",SB_REQUEST_LIMIT);
        yyjson_mut_obj_add_uint(doc,data,"max_response_bytes",SB_RESPONSE_LIMIT);
        yyjson_mut_obj_add_uint(doc,data,"default_text_page_bytes",SB_READ_DEFAULT);
        yyjson_mut_obj_add_uint(doc,data,"max_read_permit_bytes",SB_READ_PERMIT_LIMIT);
        yyjson_mut_obj_add_uint(doc,data,"max_task_input_bytes",SB_TASK_INPUT_LIMIT);
        yyjson_mut_obj_add_uint(doc,data,"max_task_output_page_bytes",SB_TASK_OUTPUT_PAGE_LIMIT);
        yyjson_mut_obj_add_uint(doc,data,"max_task_arguments",SB_TASK_ARG_LIMIT);
#ifndef SB_POSIX
        yyjson_mut_obj_add_uint(doc,data,"max_command_utf16_units",SB_WINDOWS_COMMAND_CAP-1u);
#endif
        yyjson_mut_obj_add_uint(doc,data,"max_transfer_ttl_seconds",SB_TRANSFER_MAX_TTL);
        yyjson_mut_obj_add_uint(doc,data,"default_transfer_chunk_bytes",SB_TRANSFER_DEFAULT_CHUNK);
        yyjson_mut_obj_add_uint(doc,data,"max_transfer_total_bytes",SB_TRANSFER_TOTAL);
        yyjson_mut_obj_add_uint(doc, data, "max_backups_per_instance", 64);
        yyjson_mut_obj_add_bool(doc, data, "execution", sb_execution_available(host->execution));
        yyjson_mut_obj_add_val(doc, data, "execution_status", sb_execution_status(host->execution, doc));
        yyjson_mut_obj_add_bool(doc, data, "desktop_control", host->project && host->project->policy.desktop.control && sb_desktop_available(host->desktop));
        yyjson_mut_val *desktop_status=sb_desktop_capabilities(host->desktop,doc);
#ifndef SB_POSIX
        bool key_visible=sb_desktop_tool_visible(host,"desktop_key");
        yyjson_mut_obj_remove_str(desktop_status,"explicit_key_chords");
        yyjson_mut_obj_add_bool(doc,desktop_status,"explicit_key_chords_implemented",true);
        yyjson_mut_obj_add_bool(doc,desktop_status,"explicit_key_chords",key_visible);
        yyjson_mut_obj_add_bool(doc,desktop_status,"desktop_key_available",key_visible);
        if(!key_visible){
            const char *reason="desktop_backend_unavailable";
            if(!host->project || host->project->legacy_compatibility)reason="desktop_requires_ordinary_project";
            else {
                sb_request_context context={.actor=actor,.project=host->project,.policy_revision=host->project->policy_revision};
                sb_project_operation operation={.tool="desktop_key",.identity=SB_ID_CURRENT_USER};
                sb_policy_result decision=sb_project_evaluate(&context,&operation);
                if(decision.decision==SB_DECISION_DENY)reason=decision.reason;
            }
            yyjson_mut_obj_add_str(doc,desktop_status,"desktop_key_unavailable_reason",reason);
        }
#endif
        yyjson_mut_obj_add_val(doc,data,"desktop_status",desktop_status);
#ifdef SB_POSIX
        yyjson_mut_obj_add_bool(doc, data, "oauth", host->oauth_backend);
#else
        yyjson_mut_obj_add_bool(doc, data, "oauth", false);
#endif
        yyjson_mut_obj_add_bool(doc, data, "approval", true);
        yyjson_mut_obj_add_str(doc, data, "approval_mode", host->project && !host->project->legacy_compatibility ?
            (host->project->policy.approval_mode == SB_PROJECT_NEVER ? "never" : host->project->policy.approval_mode == SB_PROJECT_WORKSPACE_AUTO ? "workspace_auto" : "always") : "local-owner");
        if (host->project) {
            yyjson_mut_obj_add_bool(doc, data, "project_approval_adapter", !host->project->legacy_compatibility);
            yyjson_mut_obj_add_bool(doc, data, "unlisted_account_file_adapter", account_files_available && !host->project->legacy_compatibility && host->project->policy.file_scope == SB_FILES_CURRENT_USER);
#ifdef SB_POSIX
            yyjson_mut_obj_add_bool(doc, data, "administrator_backend", host->administrator_host || sb_posix_administrator_available(host));
#else
            yyjson_mut_obj_add_bool(doc, data, "administrator_backend", host->administrator_backend);
#endif
            yyjson_mut_obj_add_bool(doc, data, "administrator_identity_explicit", true);
            yyjson_mut_obj_add_uint(doc, data, "max_active_tasks", host->config.limits.active_tasks);
            yyjson_mut_obj_add_uint(doc, data, "max_task_records", host->config.limits.task_records);
            yyjson_mut_obj_add_uint(doc, data, "max_output_bytes", host->config.limits.output_bytes);
            yyjson_mut_obj_add_uint(doc, data, "max_timeout_ms", host->config.limits.task_timeout_ms);
            yyjson_mut_obj_add_uint(doc, data, "max_approval_records", host->config.limits.approval_records);
            yyjson_mut_obj_add_uint(doc, data, "worker_memory_mb", host->config.limits.worker_memory_mb);
        }
        yyjson_mut_obj_add_bool(doc, data, "durable_approvals", host->store!=NULL);
        yyjson_mut_obj_add_bool(doc, data, "durable_tasks", host->store!=NULL && platform_tool_available("task_start"));
        yyjson_mut_obj_add_bool(doc, data, "approval_restored_after_restart", false);
        yyjson_mut_obj_add_bool(doc, data, "state_healthy", sb_store_healthy(host->store));
        yyjson_mut_obj_add_bool(doc, data, "remote_owner_approval", *host->owner_url!=0);
        yyjson_mut_obj_add_bool(doc, data, "durable_audit", true);
        yyjson_mut_obj_add_bool(doc, data, "audit_healthy", sb_audit_healthy(host->audit));
        yyjson_mut_obj_add_str(doc, data, "audit_scope", host->store ? "approval decisions, file calls, task starts/completion/cancellation/timeouts and input delivery" : "approval transitions, file edits, task operation intents and new task startup results");
        yyjson_mut_obj_add_uint(doc, data, "audit_max_bytes", host->config.audit_max_bytes);
        yyjson_mut_obj_add_str(doc, data, "search_mode", "case-sensitive literal filenames or Unicode content");
        yyjson_mut_obj_add_bool(doc, data, "content_search", true);
        yyjson_mut_obj_add_bool(doc,data,"artifact_transfer",host->project && !host->project->legacy_compatibility && platform_tool_available("artifact_download"));
        yyjson_mut_obj_add_uint(doc,data,"max_transfer_bytes",SB_TRANSFER_LIMIT);
        yyjson_mut_obj_add_uint(doc,data,"max_transfer_chunk_bytes",SB_TRANSFER_CHUNK);
        yyjson_mut_obj_add_bool(doc,data,"document_read",media_available);
        yyjson_mut_obj_add_bool(doc,data,"pdf_page_preview",media_available);
        yyjson_mut_obj_add_str(doc,data,"pdf_backend",media_available ? "pinned PDFium 157.0.8086.0; no V8" : "unavailable");
        yyjson_mut_obj_add_str(doc,data,"office_formats",media_available ? "DOCX, XLSX, PPTX; legacy formats require conversion" : "unavailable");
        yyjson_mut_obj_add_str(doc,data,"office_preview",media_available ? "structured text, not original layout" : "unavailable");
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
    if(sb_transfer_tool(name)) {
        bool failed;yyjson_mut_val *value=sb_transfer_call(host,actor,name,args,doc,workspace,path_override?path_override:path,sb_string(yyjson_obj_get(args,"approval_id")),&failed);
        return content_result(doc,value,failed);
    }
    if (!path && yyjson_obj_get(args, "path")) return failure(doc, "path must be a string without NUL.");
    if (!path && (!strcmp(name, "fs_read") || !strcmp(name, "image_view") || sb_document_tool(name) || sb_transfer_tool(name) || !strcmp(name, "fs_write") || !strcmp(name, "fs_patch"))) return failure(doc, "path is required.");
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
            !number(args, "max_results", 50, 1, SB_SEARCH_RESULTS, &maximum) ||
            !number(args, "max_file_bytes", 262144, 4, SB_SEARCH_FILE_LIMIT, &per_file) ||
            !number(args, "max_total_bytes", 4194304, 4, SB_SEARCH_TOTAL_LIMIT, &total_bytes)) return failure(doc, "Invalid search parameters.");
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
        if (!number(args, "offset", 0, 0, SB_LIST_OFFSET_LIMIT, &offset) || !number(args, "max_entries", 100, 1, SB_LIST_LIMIT, &limit)) {
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
            !number(args, "max_bytes", SB_READ_DEFAULT, 4, SB_READ_LIMIT, &limit)) {
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
        if(size>SB_IMAGE_INPUT_LIMIT){result=content_result(doc,sb_limit_error(doc,"image.source_size",size,SB_IMAGE_INPUT_LIMIT,"bytes"),true);goto done;}
        if (!size) { result = failure(doc, "Image is empty."); goto done; }
        unsigned char *bytes = malloc((size_t)size);
        if (!bytes) { result = failure(doc, "Out of memory."); goto done; }
        size_t got = 0;
        bool ok = sb_platform_read(file, 0, bytes, (size_t)size, &got, &error);
        uint64_t maximum=0; sb_image processed={0}; char source_hash[65],output_hash[65];
        ok=ok && got==size && number(args,"max_dimension",0,128,SB_IMAGE_DIMENSION,&maximum) &&
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
    yyjson_doc *schema = tool_schema(definition);
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

typedef struct {
    sb_workspace account, *workspace;
    const sb_project_directory *directory;
    const char *path;
    char *relative;
} fs_location;
static void fs_location_close(fs_location *location) { free(location->relative); sb_platform_root_close(location->account.root); }
static bool fs_location_open(sb_host *host, const char *id, const char *path, fs_location *location, sb_error *error) {
    location->path=path; location->directory=sb_project_directory_find(host->project,id);
    location->workspace=id ? sb_workspace_find(&host->config,id) : NULL;
    if (!id && host->project->policy.file_scope==SB_FILES_CURRENT_USER) {
        location->account.root=path ? sb_platform_account_parent(path,&location->relative,error) : NULL;
        if (!location->account.root) return false;
        location->path=location->relative; strcpy(location->account.id,"current-user");
        location->account.read_only=host->project->policy.current_user_read_only; location->workspace=&location->account;
        for (size_t i=0;i<host->config.workspace_count;++i) {
            bool inside=sb_platform_target_within(host->config.workspaces[i].root,location->account.root,location->path,error);
            if (*error->text) return false;
            if (inside) {
                location->directory=sb_project_directory_find(host->project,host->config.workspaces[i].id);
                location->account.read_only=host->config.workspaces[i].read_only; break;
            }
        }
    }
    if (!location->workspace || !location->path) { sb_fail(error,"Unknown workspace or missing file path."); return false; } return true;
}
typedef struct { sb_host *host; const char *actor; } file_manage_context;
static bool file_manage_backup(void *context,const char *path,uint64_t bytes,const char *backup_id) {
    file_manage_context *call=context;
    return sb_audit_append(call->host->audit,"backup_saved",call->actor,NULL,NULL,path,NULL,(size_t)bytes,"fs_delete","delete",NULL,backup_id);
}
static yyjson_mut_val *file_manage_call(sb_host *host, const char *actor, const char *name, yyjson_val *args, yyjson_mut_doc *doc) {
    bool reading=!strcmp(name,"fs_stat"), pair=!strcmp(name,"fs_copy") || !strcmp(name,"fs_move");
    bool mkdir=!strcmp(name,"fs_mkdir"), directory=mkdir || yyjson_get_bool(yyjson_obj_get(args,"directory"));
    const char *id=sb_string(yyjson_obj_get(args,"workspace_id")), *path=sb_string(yyjson_obj_get(args,"path"));
    const char *expected=sb_string(yyjson_obj_get(args,"expected_version"));
    fs_location locations[2]={0}; sb_fs_plan *plan=NULL; sb_fs_info info={0}; sb_error error={0};
    yyjson_mut_val *result=NULL; char *parameters=NULL, version[65],target[65],ticket[33]={0};
    if (!reading) {
        const char *key=sb_string(yyjson_obj_get(args,"request_key"));
        if (!key || !*key || strlen(key)>64 || strspn(key,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-")!=strlen(key) ||
            (!mkdir && (!expected || strlen(expected)!=64 || strspn(expected,"0123456789abcdef")!=64))) return failure(doc,"Supply a valid request_key and the expected_version returned by fs_stat.");
    }
    if (!fs_location_open(host,id,path,&locations[0],&error)) goto failed;
    if (pair) {
        const char *destination_id=sb_string(yyjson_obj_get(args,"destination_workspace_id"));
        if (!fs_location_open(host,destination_id ? destination_id : id,sb_string(yyjson_obj_get(args,"destination_path")),&locations[1],&error)) goto failed;
    }
    sb_request_context context={.actor=actor,.project=host->project,.policy_revision=host->project->policy_revision};
    bool pending=false; char roots[2][50]={{0}};
    for (unsigned i=0;i<(pair?2u:1u);++i) {
        fs_location *location=&locations[i];
        sb_project_operation operation={.tool=name,.directory=location->directory,
            .current_user_target=location->workspace==&location->account && !location->directory,
            .identity=host->administrator_host ? SB_ID_ADMINISTRATOR : SB_ID_CURRENT_USER,
            .administrator_backend_verified=host->administrator_host,.file_source_read=i==0 && !strcmp(name,"fs_copy")};
        sb_policy_result decision=sb_project_evaluate(&context,&operation);
        if (decision.decision==SB_DECISION_DENY) { sb_fail(&error,decision.reason); goto failed; }
        pending|=decision.decision==SB_DECISION_PENDING;
        if (!sb_platform_root_identity(location->workspace->root,roots[i],&error)) goto failed;
    }
    parameters=operation_parameters(name,args);
    size_t capacity=parameters ? strlen(parameters)+sizeof(roots)+8 : 0;
    char *tuple=capacity ? malloc(capacity) : NULL;
    if (!tuple) goto failed;
    snprintf(tuple,capacity,"%s|%s|%s",roots[0],roots[1],parameters);
    bool hashed=sb_platform_sha256(tuple,strlen(tuple),target,&error); free(tuple); if (!hashed) goto failed;
    const char *approval=sb_string(yyjson_obj_get(args,"approval_id"));
    if (!reading) {
        result=sb_policy_operation_receipt(host,actor,name,parameters,target,approval,doc);
        if (result) goto done;
    }
    plan=sb_platform_fs_prepare(name,locations[0].workspace->root,locations[0].path,directory,
        pair ? locations[1].workspace->root : NULL,pair ? locations[1].path : NULL,&info,version,&error);
    if (!plan) goto failed;
    if (expected && strcmp(expected,info.version)) { sb_fail(&error,"Physical source changed; inspect it again with fs_stat before using a new request."); goto failed; }
    if (!reading || pending || approval) {
        yyjson_mut_val *reply=NULL;
        if (!sb_policy_operation_begin(host,actor,name,parameters,target,version,approval,pending,0,1,doc,&reply,ticket)) {
            result=yyjson_mut_obj_get(reply,"isError") ? reply : content_result(doc,reply,true); goto done;
        }
    }
    file_manage_context call={host,actor}; char backup_id[33]={0};
    bool ok=sb_platform_fs_execute(plan,host->lease,file_manage_backup,&call,backup_id,&error);
    yyjson_mut_val *data=yyjson_mut_obj(doc);
    if (ok) {
        yyjson_mut_obj_add_str(doc,data,"status","completed");
        yyjson_mut_obj_add_str(doc,data,"type",info.directory ? "directory" : "file");
        yyjson_mut_obj_add_uint(doc,data,"size",info.size);
        if (*info.version) yyjson_mut_obj_add_strcpy(doc,data,"version",info.version);
        yyjson_mut_obj_add_bool(doc,data,"overwrote_existing",false);
    } else yyjson_mut_obj_add_strcpy(doc,data,"error",*error.text ? error.text : "Native file operation failed.");
    if (*backup_id) yyjson_mut_obj_add_strcpy(doc,data,"backup_id",backup_id);
    if (*ticket) yyjson_mut_obj_add_strcpy(doc,data,"approval_id",ticket);
    yyjson_mut_obj_add_strcpy(doc,data,"project_id",host->project->id);
    yyjson_mut_obj_add_uint(doc,data,"policy_revision",host->project->policy_revision);
    result=content_result(doc,data,!ok);
    if (*ticket && !sb_policy_operation_finish(host->policy,ticket,result)) yyjson_mut_obj_add_str(doc,data,"audit_warning","Result is authoritative; final persistent recording is incomplete.");
    goto done;
failed:
    result=failure(doc,*error.text ? error.text : "Cannot prepare the native file operation.");
done:
    sb_platform_fs_close(plan); free(parameters); fs_location_close(&locations[0]); fs_location_close(&locations[1]); return result;
}

static yyjson_mut_val *project_tool_call(sb_host *host, const char *actor, const char *name, yyjson_val *args, yyjson_mut_doc *doc) {
    if (sb_desktop_tool(name)) return sb_desktop_tools_call(host,actor,name,args,doc);
    if (!host->project || host->project->legacy_compatibility) return sb_file_manage_tool(name) ? failure(doc,"Native file management requires a normal versioned project.") : tool_call_inner(host,actor,name,args,doc,NULL,NULL,false);
    const tool_definition *definition = NULL;
    for (size_t i=0;i<tool_count();++i) if(!strcmp(name,definitions[i].name)) definition=&definitions[i];
    if(definition){yyjson_mut_val *limit=argument_limit(definition,args,doc,host);if(limit)return limit;}
    if (!definition || !project_arguments_valid(host,definition,args)) return failure(doc,"Invalid or unknown tool arguments.");
    const sb_project *project = host->project;
    const char *selected_identity=sb_string(yyjson_obj_get(args,"identity"));
    bool administrator=selected_identity && !strcmp(selected_identity,"administrator");
    if (administrator && !host->administrator_host) {
#ifdef SB_POSIX
        char *wire=sb_posix_administrator_call(host,actor,name,args);
#else
        char *wire=host->administrator_call ? host->administrator_call(host->administrator_context,name,args) : NULL;
#endif
        yyjson_doc *reply=wire ? yyjson_read(wire,strlen(wire),0) : NULL;
        yyjson_val *root=yyjson_doc_get_root(reply);
        yyjson_mut_val *result=yyjson_is_bool(yyjson_obj_get(root,"isError")) ? yyjson_val_mut_copy(doc,root) :
            failure(doc,sb_string(yyjson_obj_get(root,"error")) ? sb_string(yyjson_obj_get(root,"error")) : "project_administrator_backend_unavailable");
        yyjson_doc_free(reply); free(wire); return result;
    }
    if (host->administrator_host && !administrator) return failure(doc,"Administrator identity must be explicitly selected.");
    if (sb_file_manage_tool(name)) return file_manage_call(host,actor,name,args,doc);
    if(sb_transfer_tool(name)) {
        const char *action=sb_string(yyjson_obj_get(args,"action"));
        static const char *const download_open[]={"action","workspace_id","path","request_key","expected_sha256","ttl_seconds","identity","approval_id"};
        static const char *const upload_open[]={"action","workspace_id","path","request_key","expected_sha256","ttl_seconds","identity","approval_id","size"};
        static const char *const read_keys[]={"action","transfer_id","offset","max_bytes","identity"};
        static const char *const write_keys[]={"action","transfer_id","offset","data_base64","identity"};
        static const char *const control_keys[]={"action","transfer_id","identity"};
        bool upload=!strcmp(name,"artifact_upload"),opening=action && !strcmp(action,"open");
        const char *const *keys=opening ? (upload?upload_open:download_open) : action && !strcmp(action,"read") ? read_keys : action && !strcmp(action,"write") ? write_keys : control_keys;
        size_t count=opening ? (upload?9u:8u) : action && !strcmp(action,"read") ? 5u : action && !strcmp(action,"write") ? 5u : 3u;
        if(!action || !sb_config_keys(args,keys,count))return failure(doc,"Invalid arguments for transfer action.");
        if(!opening) {
            int index=sb_project_tool_index(name);
            bool cleanup=!strcmp(action,"status") || !strcmp(action,"cancel");
            if(!cleanup && (!project->enabled || !(project->policy.tools&(UINT64_C(1)<<index))))return failure(doc,"Transfer permission was revoked.");
            bool failed;yyjson_mut_val *value=sb_transfer_call(host,actor,name,args,doc,NULL,NULL,NULL,&failed);
            return content_result(doc,value,failed);
        }
        if(!sb_string(yyjson_obj_get(args,"path")))return failure(doc,"Transfer open requires a path.");
        const char *key=sb_string(yyjson_obj_get(args,"request_key"));char hash[65];uint64_t ttl,size;
        if(!key || !*key || strlen(key)>64 || !number(args,"ttl_seconds",SB_TRANSFER_TTL,1,SB_TRANSFER_MAX_TTL,&ttl) ||
            (upload && (!sb_integer(yyjson_obj_get(args,"size"),&size) || size>SB_TRANSFER_LIMIT || !expected_hash(args,hash))) ||
            (!upload && yyjson_obj_get(args,"expected_sha256") && !expected_hash(args,hash)))return failure(doc,"Transfer open requires a request_key, bounded expiry and valid size/checksum metadata.");
    }
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
    bool file_tool = !strncmp(name,"fs_",3) || !strcmp(name,"image_view") || sb_document_tool(name) || sb_transfer_tool(name);
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
    bool create=(!strcmp(name,"fs_write") && !yyjson_get_bool(yyjson_obj_get(args,"overwrite"))) || !strcmp(name,"artifact_upload");
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
    if(!strcmp(name,"fs_read") || !strcmp(name,"document_read")) { if(!number(args,"offset",0,0,INT64_MAX,&offset)||!number(args,"max_bytes",SB_READ_DEFAULT,4,SB_READ_LIMIT,&budget)){result=failure(doc,"Invalid file page.");goto done;} }
    else if(!strcmp(name,"fs_list")) { if(!number(args,"offset",0,0,SB_LIST_OFFSET_LIMIT,&offset)||!number(args,"max_entries",100,1,SB_LIST_LIMIT,&budget)){result=failure(doc,"Invalid directory page.");goto done;} }
    else if(!strcmp(name,"fs_search")) { if(!number(args,"max_total_bytes",4194304,4,SB_SEARCH_TOTAL_LIMIT,&budget)){result=failure(doc,"Invalid search budget.");goto done;} }
    else if(!strcmp(name,"image_view"))budget=SB_IMAGE_INPUT_LIMIT;
    else if(sb_document_tool(name))budget=SB_DOCUMENT_INPUT_LIMIT;
    else if(sb_transfer_tool(name))budget=SB_TRANSFER_LIMIT;
    else if(!strcmp(name,"fs_write") || !strcmp(name,"fs_patch"))budget=SB_EDIT_LIMIT;
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
    if(file_tool && strncmp(name,"fs_write",8) && strcmp(name,"fs_patch") && strcmp(name,"artifact_upload") && !yyjson_mut_get_bool(yyjson_mut_obj_get(result,"isError"))) {
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

static yyjson_mut_val *tool_call_default(sb_host *host, const char *actor, const char *name, yyjson_val *args, yyjson_mut_doc *doc) {
#ifndef SB_POSIX
    if(!strcmp(name,"task_archive") && yyjson_obj_get(args,"task_ids")){
        const char *selected=sb_string(yyjson_obj_get(args,"identity"));
        if(host->project && !host->project->legacy_compatibility && selected && !strcmp(selected,"administrator") && !host->administrator_host)
            return project_tool_call(host,actor,name,args,doc);
        const tool_definition *definition=NULL;for(size_t i=0;i<tool_count();++i)if(!strcmp(name,definitions[i].name))definition=&definitions[i];
        yyjson_mut_val *limited=argument_limit(definition,args,doc,host);if(limited)return limited;
        yyjson_val *ids=yyjson_obj_get(args,"task_ids");const char *instance=sb_string(yyjson_obj_get(args,"host_instance_id"));
        if(!arguments_valid(definition,args,host) || yyjson_obj_get(args,"task_id") || !yyjson_is_arr(ids) || !yyjson_arr_size(ids) || !instance ||
            !sb_execution_instance(host->execution) || strcmp(instance,sb_execution_instance(host->execution)))return failure(doc,"Invalid archive batch or host instance; supply task_ids only and refresh host_capabilities.");
        for(size_t i=0;i<yyjson_arr_size(ids);++i){const char *id=sb_string(yyjson_arr_get(ids,i));if(!id || strlen(id)!=32 || strspn(id,"0123456789abcdef")!=32)return failure(doc,"Every task_ids entry must be a 32-character lowercase hexadecimal task ID; no archive was attempted.");}
        yyjson_mut_val *data=yyjson_mut_obj(doc),*results=yyjson_mut_arr(doc);size_t completed=0,errors=0,pending=0;uint64_t until=sb_platform_millis()+4000;
        for(size_t i=0;i<yyjson_arr_size(ids);++i){
            const char *id=sb_string(yyjson_arr_get(ids,i));yyjson_mut_val *entry=yyjson_mut_obj(doc);yyjson_mut_obj_add_strcpy(doc,entry,"task_id",id);
            if(sb_platform_millis()>=until){++pending;yyjson_mut_obj_add_str(doc,entry,"status","pending");}
            else {
                yyjson_mut_doc *single=yyjson_mut_doc_new(NULL);yyjson_mut_val *parameters=single?yyjson_val_mut_copy(single,args):NULL;
                if(!parameters){if(single)yyjson_mut_doc_free(single);return failure(doc,"Cannot allocate archive batch continuation; inspect task_list before retrying the same IDs.");}
                yyjson_mut_obj_remove_str(parameters,"task_ids");yyjson_mut_obj_add_strcpy(single,parameters,"task_id",id);
                yyjson_doc *input=yyjson_mut_val_imut_copy(parameters,NULL);yyjson_mut_doc_free(single);
                yyjson_mut_val *reply=input?sb_tool_call(host,actor,name,yyjson_doc_get_root(input),doc):NULL;yyjson_doc_free(input);
                bool failed=!reply || yyjson_mut_get_bool(yyjson_mut_obj_get(reply,"isError"));if(failed)++errors;else ++completed;
                yyjson_mut_obj_add_str(doc,entry,"status",failed?"failed":"archived");if(reply)yyjson_mut_obj_add_val(doc,entry,"result",reply);
            }
            yyjson_mut_arr_add_val(results,entry);
        }
        yyjson_mut_obj_add_val(doc,data,"results",results);yyjson_mut_obj_add_uint(doc,data,"archived_count",completed);yyjson_mut_obj_add_uint(doc,data,"failed_count",errors);
        yyjson_mut_obj_add_uint(doc,data,"pending_count",pending);yyjson_mut_obj_add_bool(doc,data,"complete",pending==0);yyjson_mut_obj_add_strcpy(doc,data,"host_instance_id",instance);
        yyjson_mut_obj_add_str(doc,data,"retry","Retry identical task_ids safely; archived IDs replay receipts. Pending entries were not attempted. Scheduling deadline cannot interrupt an in-flight durable write.");
        return content_result(doc,data,errors!=0);
    }
#endif
    bool file=host->project && !host->project->legacy_compatibility && (!strncmp(name,"fs_",3) || !strcmp(name,"image_view") || sb_document_tool(name) || sb_transfer_tool(name));
    if(sb_transfer_tool(name)) {const char *action=sb_string(yyjson_obj_get(args,"action"));if(action && (!strcmp(action,"cancel") || !strcmp(action,"status")))file=false;}
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

#ifndef SB_POSIX
static void copy_field(yyjson_mut_doc *doc,yyjson_mut_val *dest,yyjson_mut_val *source,const char *key) {
    yyjson_mut_val *value=yyjson_mut_obj_get(source,key);
    if(value)yyjson_mut_obj_add_val(doc,dest,key,yyjson_mut_val_mut_copy(doc,value));
}
static yyjson_mut_val *compact_archive(sb_host *host,const char *actor,yyjson_val *args,yyjson_mut_doc *doc,yyjson_mut_val *original) {
    yyjson_mut_val *old=yyjson_mut_obj_get(original,"structuredContent"),*rows=yyjson_mut_obj_get(old,"results");
    if(!yyjson_mut_is_arr(rows))return original;
    yyjson_mut_val *data=yyjson_mut_obj(doc),*results=yyjson_mut_arr(doc),*pending=yyjson_mut_arr(doc),*counts=yyjson_mut_obj(doc);
    copy_field(doc,data,old,"complete");copy_field(doc,data,old,"host_instance_id");
    const char *old_counts[]={"archived_count","failed_count","pending_count"},*new_counts[]={"archived","failed","pending"};
    for(unsigned i=0;i<3;++i)yyjson_mut_obj_add_uint(doc,counts,new_counts[i],yyjson_mut_get_uint(yyjson_mut_obj_get(old,old_counts[i])));
    yyjson_mut_obj_add_val(doc,data,"counts",counts);
    for(size_t i=0;i<yyjson_mut_arr_size(rows);++i){
        yyjson_mut_val *row=yyjson_mut_arr_get(rows,i);const char *status=yyjson_mut_get_str(yyjson_mut_obj_get(row,"status"));
        if(status && !strcmp(status,"pending")){yyjson_mut_arr_add_val(pending,yyjson_mut_val_mut_copy(doc,yyjson_mut_obj_get(row,"task_id")));continue;}
        yyjson_mut_val *item=yyjson_mut_obj(doc),*reply=yyjson_mut_obj_get(row,"result"),*body=yyjson_mut_obj_get(reply,"structuredContent");
        copy_field(doc,item,row,"task_id");copy_field(doc,item,row,"status");
        if(status && !strcmp(status,"failed")){
            yyjson_mut_val *error=yyjson_mut_obj(doc);const char *code=yyjson_mut_get_str(yyjson_mut_obj_get(body,"code")),*message=yyjson_mut_get_str(yyjson_mut_obj_get(body,"error"));
            if(!code)code=message && !strcmp(message,"task_not_owned")?"TASK_NOT_OWNED":"OPERATION_FAILED";
            yyjson_mut_obj_add_strcpy(doc,error,"code",code);yyjson_mut_obj_add_bool(doc,error,"retryable",yyjson_mut_get_bool(yyjson_mut_obj_get(body,"retryable")));
            if(message)yyjson_mut_obj_add_strcpy(doc,error,"message",message);yyjson_mut_obj_add_val(doc,item,"error",error);
        }
        copy_field(doc,item,body,"audit_warning");
        if(yyjson_mut_get_bool(yyjson_mut_obj_get(body,"durable_warning")))yyjson_mut_obj_add_bool(doc,item,"durable_warning",true);
        yyjson_mut_arr_add_val(results,item);
    }
    yyjson_mut_obj_add_val(doc,data,"results",results);yyjson_mut_obj_add_val(doc,data,"pending_task_ids",pending);
    /* 一次只读快照返回整批结束后的预算；不会重新执行归档。 */
    yyjson_mut_doc *query=yyjson_mut_doc_new(NULL);yyjson_mut_val *parameters=query?yyjson_mut_obj(query):NULL;
    if(parameters){
        const char *fields[]={"host_instance_id","identity"};for(unsigned i=0;i<2;++i){yyjson_val *v=yyjson_obj_get(args,fields[i]);if(v)yyjson_mut_obj_add_val(query,parameters,fields[i],yyjson_val_mut_copy(query,v));}
        yyjson_mut_obj_add_uint(query,parameters,"max_entries",1);yyjson_doc *input=yyjson_mut_val_imut_copy(parameters,NULL);
        yyjson_mut_val *reply=input?tool_call_default(host,actor,"task_list",yyjson_doc_get_root(input),doc):NULL;yyjson_doc_free(input);
        yyjson_mut_val *body=yyjson_mut_obj_get(reply,"structuredContent");const char *shared[]={"task_budget","operation_budget","persistent_state","project_id","policy_revision"};
        for(unsigned i=0;i<5;++i)copy_field(doc,data,body,shared[i]);
    }
    yyjson_mut_doc_free(query);
    yyjson_mut_obj_add_str(doc,data,"pending_semantics","Only pending_task_ids were not attempted within the scheduling budget; failed items were attempted and may require action before retry.");
    yyjson_mut_obj_remove_str(original,"structuredContent");yyjson_mut_obj_add_val(doc,original,"structuredContent",data);return original;
}
#endif

yyjson_mut_val *sb_tool_call(sb_host *host,const char *actor,const char *name,yyjson_val *args,yyjson_mut_doc *doc) {
#ifndef SB_POSIX
    yyjson_val *mode=yyjson_obj_get(args,"response_mode");const char *value=sb_string(mode);
    if(mode && (!value || (strcmp(value,"full") && strcmp(value,"compact"))))return failure(doc,"response_mode must be full or compact; no operation was attempted.");
    bool compact=value && !strcmp(value,"compact");yyjson_mut_doc *normalized=NULL;yyjson_doc *clean=NULL;
    if(mode){
        normalized=yyjson_mut_doc_new(NULL);yyjson_mut_val *parameters=normalized?yyjson_val_mut_copy(normalized,args):NULL;
        if(parameters){yyjson_mut_obj_remove_str(parameters,"response_mode");clean=yyjson_mut_val_imut_copy(parameters,NULL);}yyjson_mut_doc_free(normalized);
        if(!clean)return failure(doc,"Cannot allocate response presentation arguments; no operation was attempted.");args=yyjson_doc_get_root(clean);
    }
    yyjson_mut_val *result=tool_call_default(host,actor,name,args,doc);
    if(compact && result){
        if(!strcmp(name,"task_archive") && yyjson_obj_get(args,"task_ids"))result=compact_archive(host,actor,args,doc,result);
        yyjson_mut_val *content=yyjson_mut_arr(doc),*entry=yyjson_mut_obj(doc),*old=yyjson_mut_obj_get(result,"content"),*data=yyjson_mut_obj_get(result,"structuredContent");char summary[192];
        yyjson_mut_val *counts=yyjson_mut_obj_get(data,"counts");
        if(counts)snprintf(summary,sizeof(summary),"task_archive: %llu archived, %llu failed, %llu pending; details in structuredContent.",(unsigned long long)yyjson_mut_get_uint(yyjson_mut_obj_get(counts,"archived")),(unsigned long long)yyjson_mut_get_uint(yyjson_mut_obj_get(counts,"failed")),(unsigned long long)yyjson_mut_get_uint(yyjson_mut_obj_get(counts,"pending")));
        else snprintf(summary,sizeof(summary),"%s: %s; details in structuredContent.",name,yyjson_mut_get_bool(yyjson_mut_obj_get(result,"isError"))?"failed":"receipt available");
        yyjson_mut_obj_add_str(doc,entry,"type","text");yyjson_mut_obj_add_strcpy(doc,entry,"text",summary);yyjson_mut_arr_add_val(content,entry);
        for(size_t i=0;i<yyjson_mut_arr_size(old);++i){yyjson_mut_val *item=yyjson_mut_arr_get(old,i);const char *type=yyjson_mut_get_str(yyjson_mut_obj_get(item,"type"));if(type && strcmp(type,"text"))yyjson_mut_arr_add_val(content,yyjson_mut_val_mut_copy(doc,item));}
        yyjson_mut_obj_remove_str(result,"content");yyjson_mut_obj_add_val(doc,result,"content",content);
    }
    yyjson_doc_free(clean);return result;
#else
    return tool_call_default(host,actor,name,args,doc);
#endif
}
