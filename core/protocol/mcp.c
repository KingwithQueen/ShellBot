#include "sb.h"
#include "sb_projects.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

bool sb_protocol_version(const char *version) {
    return version && (!strcmp(version, "2025-03-26") || !strcmp(version, "2025-06-18") ||
        !strcmp(version, "2025-11-25") || !strcmp(version, "2026-07-28"));
}

static yyjson_mut_val *versions(yyjson_mut_doc *doc) {
    yyjson_mut_val *array = yyjson_mut_arr(doc);
    yyjson_mut_arr_add_str(doc, array, "2025-03-26"); yyjson_mut_arr_add_str(doc, array, "2025-06-18");
    yyjson_mut_arr_add_str(doc, array, "2025-11-25"); yyjson_mut_arr_add_str(doc, array, "2026-07-28");
    return array;
}
static yyjson_mut_val *capabilities(yyjson_mut_doc *doc) {
    yyjson_mut_val *caps = yyjson_mut_obj(doc), *tools = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_bool(doc, tools, "listChanged", false);
    yyjson_mut_obj_add_val(doc, caps, "tools", tools); return caps;
}
static yyjson_mut_val *server_info(yyjson_mut_doc *doc) {
    yyjson_mut_val *info = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, info, "name", "ShellBot"); yyjson_mut_obj_add_str(doc, info, "version", SB_VERSION); return info;
}

/* 2026 transport header encoding: =?base64?<UTF-8 in base64>?=.
   Require canonical padding and ASCII for headers without the sentinel. */
static bool header_matches(const char *header, const char *body) {
    if (!header || !body || strlen(header) > 8192 || strlen(body) > 4096) return false;
    if (strncmp(header, "=?base64?", 9)) {
        for (const unsigned char *p = (const unsigned char *)header; *p; ++p) if (*p < 32 || *p > 126) return false;
        return !strcmp(header, body);
    }
    size_t size = strlen(header);
    if (size < 11 || strcmp(header + size - 2, "?=")) return false;
    size_t length = size - 11;
    if (length % 4 || length / 4 * 3 > 6144) return false;
    char decoded[6145]; size_t written = 0;
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (size_t i = 0; i < length; i += 4) {
        unsigned v[4]; size_t padding = 0;
        for (size_t j = 0; j < 4; ++j) {
            char c = header[9 + i + j]; const char *at = c ? strchr(alphabet, c) : NULL;
            if (c == '=') { v[j] = 0; ++padding; }
            else { if (!at || padding) return false; v[j] = (unsigned)(at - alphabet); }
        }
        if (padding > 2 || (padding && i + 4 != length) ||
            (padding == 2 && (v[1] & 15)) || (padding == 1 && (v[2] & 3))) return false;
        decoded[written++] = (char)((v[0] << 2) | (v[1] >> 4));
        if (padding < 2) decoded[written++] = (char)((v[1] << 4) | (v[2] >> 2));
        if (!padding) decoded[written++] = (char)((v[2] << 6) | v[3]);
    }
    return written == strlen(body) && !memcmp(decoded, body, written);
}

static void rpc_error(yyjson_mut_doc *doc, yyjson_mut_val *response, int code, const char *message) {
    yyjson_mut_val *error = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_int(doc, error, "code", code);
    yyjson_mut_obj_add_str(doc, error, "message", message);
    yyjson_mut_obj_add_val(doc, response, "error", error);
}

/* Reject duplicate fields throughout a bounded request. yyjson validates UTF-8. */
static bool unambiguous(yyjson_val *value, unsigned depth) {
    if (depth > 32) return false;
    size_t idx, max; yyjson_val *key, *child;
    if (yyjson_is_obj(value)) {
        if (yyjson_obj_size(value) > 64) return false;
        yyjson_obj_foreach(value, idx, max, key, child) {
            const char *name = sb_string(key);
            if (!name || !unambiguous(child, depth + 1)) return false;
            yyjson_obj_iter it = yyjson_obj_iter_with(value);
            for (size_t j = 0; j < idx; ++j) if (!strcmp(name, yyjson_get_str(yyjson_obj_iter_next(&it)))) return false;
        }
    } else if (yyjson_is_arr(value)) {
        if (yyjson_arr_size(value) > 1024) return false;
        yyjson_arr_foreach(value, idx, max, child) if (!unambiguous(child, depth + 1)) return false;
    }
    return true;
}

/* Both the manager and the worker derive the same operation. A client response
   never grants permission: approval_id is still checked against the owner pool. */
char *sb_mcp_operation(yyjson_val *params, sb_error *error) {
    const char *name=sb_string(yyjson_obj_get(params,"name"));
    yyjson_val *args=yyjson_obj_get(params,"arguments"), *state=yyjson_obj_get(params,"requestState"), *responses=yyjson_obj_get(params,"inputResponses");
    const char *id=sb_string(state), *action=NULL;
    if (!name || !unambiguous(params,0) || (args && !yyjson_is_obj(args))) goto invalid;
    if (state && (!id || strlen(id)!=32 || strspn(id,"0123456789abcdef")!=32)) goto invalid;
    if (responses) {
        if (!id || !yyjson_is_obj(responses) || yyjson_obj_size(responses)!=1) goto invalid;
        yyjson_val *answer=yyjson_obj_get(responses,id);
        static const char *const keys[]={"action"};
        action=sb_string(yyjson_obj_get(answer,"action"));
        if (!sb_config_keys(answer,keys,1) || !action || (strcmp(action,"accept") && strcmp(action,"decline") && strcmp(action,"cancel"))) goto invalid;
    }
    const char *existing=sb_string(yyjson_obj_get(args,"approval_id"));
    if (id && yyjson_obj_get(args,"approval_id") && (!existing || strcmp(existing,id))) goto invalid;
    yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL);
    if (!doc) return NULL;
    yyjson_mut_val *root=yyjson_mut_obj(doc), *arguments=args ? yyjson_val_mut_copy(doc,args) : yyjson_mut_obj(doc);
    bool cancelled=action && strcmp(action,"accept");
    if (cancelled) {
        arguments=yyjson_mut_obj(doc);
        yyjson_val *identity=yyjson_obj_get(args,"identity");
        if (identity) yyjson_mut_obj_add_val(doc,arguments,"identity",yyjson_val_mut_copy(doc,identity));
    }
    if (id) { yyjson_mut_obj_remove_str(arguments,"approval_id"); yyjson_mut_obj_add_strcpy(doc,arguments,"approval_id",id); }
    yyjson_mut_doc_set_root(doc,root); yyjson_mut_obj_add_strcpy(doc,root,"name",cancelled ? "approval_cancel" : name);
    yyjson_mut_obj_add_val(doc,root,"arguments",arguments);
    char *wire=yyjson_mut_write(doc,0,NULL); yyjson_mut_doc_free(doc); return wire;
invalid:
    sb_fail(error,"Invalid tool-call parameters or approval continuation."); return NULL;
}

static yyjson_mut_val *approval_prompt(sb_host *host, yyjson_mut_doc *doc, yyjson_mut_val *result, yyjson_val *params, bool modern) {
    yyjson_mut_val *details=yyjson_mut_obj_get(result,"structuredContent");
    const char *status=yyjson_mut_get_str(yyjson_mut_obj_get(details,"status"));
    const char *id=yyjson_mut_get_str(yyjson_mut_obj_get(details,"approval_id"));
    if (!host->project || !*host->owner_url || !status || strcmp(status,"pending") || !id) return result;
    const char *identity=sb_string(yyjson_obj_get(yyjson_obj_get(params,"arguments"),"identity"));
    char url[640]; snprintf(url,sizeof(url),"%s#project=%s&backend=%s&approval=%s",host->owner_url,host->project->id,
        identity && !strcmp(identity,"administrator") ? "administrator" : "ordinary",id);
    yyjson_mut_obj_add_strcpy(doc,details,"approval_url",url);
    yyjson_mut_obj_remove_str(details,"action");
    yyjson_mut_obj_add_str(doc,details,"action","Ask the project owner to inspect approval_url and approve this exact request, then retry unchanged arguments with approval_id or requestState. Opening the link or accepting the prompt does not approve execution.");
    yyjson_val *caps=yyjson_obj_get(yyjson_obj_get(params,"_meta"),"io.modelcontextprotocol/clientCapabilities");
    if (!modern || yyjson_obj_get(params,"requestState") || !yyjson_is_obj(yyjson_obj_get(yyjson_obj_get(caps,"elicitation"),"url"))) {
        /* Older clients may display only text content, ignoring structuredContent. */
        yyjson_mut_val *entry=yyjson_mut_arr_get(yyjson_mut_obj_get(result,"content"),0);
        char *text=yyjson_mut_val_write(details,0,NULL);
        if (entry && text) { yyjson_mut_obj_remove_str(entry,"text"); yyjson_mut_obj_add_strcpy(doc,entry,"text",text); }
        free(text); return result;
    }
    yyjson_mut_val *required=yyjson_mut_obj(doc), *requests=yyjson_mut_obj(doc), *request=yyjson_mut_obj(doc), *prompt=yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc,required,"resultType","input_required");
    yyjson_mut_obj_add_strcpy(doc,required,"requestState",id);
    yyjson_mut_obj_add_str(doc,request,"method","elicitation/create");
    yyjson_mut_obj_add_str(doc,prompt,"mode","url"); yyjson_mut_obj_add_strcpy(doc,prompt,"url",url);
    yyjson_mut_obj_add_str(doc,prompt,"message","Ask the project owner to review this exact request in ShellBot. Opening the page or accepting this prompt does not approve execution. Owner credentials stay on that page.");
    yyjson_mut_obj_add_val(doc,request,"params",prompt); yyjson_mut_obj_add_val(doc,requests,id,request);
    yyjson_mut_obj_add_val(doc,required,"inputRequests",requests); return required;
}

char *sb_dispatch(sb_host *host, sb_protocol *protocol, const char *request,
                  size_t length, const sb_request_context *context, size_t *response_length, int *http_status) {
    *response_length = 0; *http_status = 200;
    yyjson_doc *input = length <= SB_REQUEST_LIMIT ? yyjson_read(request, length, 0) : NULL;
    yyjson_mut_doc *output = yyjson_mut_doc_new(NULL);
    if (!output) { yyjson_doc_free(input); *http_status = 500; return NULL; }
    yyjson_mut_val *response = yyjson_mut_obj(output);
    yyjson_mut_doc_set_root(output, response);
    yyjson_mut_obj_add_str(output, response, "jsonrpc", "2.0");
    yyjson_val *root = input ? yyjson_doc_get_root(input) : NULL;
    yyjson_val *id = yyjson_obj_get(root, "id");
    bool id_valid = yyjson_is_str(id) ? sb_string(id) != NULL : yyjson_is_int(id);
    yyjson_mut_obj_add_val(output, response, "id", id_valid ? yyjson_val_mut_copy(output, id) : yyjson_mut_null(output));
    const char *version = sb_string(yyjson_obj_get(root, "jsonrpc"));
    const char *method = sb_string(yyjson_obj_get(root, "method"));
    yyjson_val *params = yyjson_obj_get(root, "params");
    yyjson_val *meta = yyjson_obj_get(params, "_meta");
    yyjson_val *meta_version = yyjson_obj_get(meta, "io.modelcontextprotocol/protocolVersion");
    const char *requested = sb_string(meta_version);
    const char *http_version = context ? context->version : NULL;
    bool modern = meta_version != NULL || (http_version && !strcmp(http_version, "2026-07-28"));
    if (!input) {
        rpc_error(output, response, -32700, "Parse error or request limit exceeded."); *http_status = 400;
    } else if (host->project && (!context || context->project != host->project || !context->actor ||
        context->policy_revision != host->project->policy_revision)) {
        rpc_error(output, response, -32003, "Project context binding mismatch."); *http_status = 403;
    } else if (!yyjson_is_obj(root) || !unambiguous(root, 0) || !version || strcmp(version, "2.0") ||
        !method || !*method || (id && !id_valid) || (params && !yyjson_is_obj(params))) {
        rpc_error(output, response, -32600, "Invalid Request."); *http_status = 400;
    } else if ((http_version && !sb_protocol_version(http_version)) || (requested && strcmp(requested, "2026-07-28"))) {
        rpc_error(output, response, -32022, "Unsupported protocol version."); *http_status = 400;
        yyjson_mut_val *error = yyjson_mut_obj_get(response, "error"), *data = yyjson_mut_obj(output);
        yyjson_mut_obj_add_val(output, data, "supported", versions(output));
        yyjson_mut_obj_add_str(output, data, "requested", http_version && !sb_protocol_version(http_version) ? http_version : requested);
        yyjson_mut_obj_add_val(output, error, "data", data);
    } else if (modern && (!requested || !yyjson_is_obj(yyjson_obj_get(meta, "io.modelcontextprotocol/clientCapabilities")) ||
        (yyjson_obj_get(meta, "io.modelcontextprotocol/clientInfo") &&
         (!sb_string(yyjson_obj_get(yyjson_obj_get(meta, "io.modelcontextprotocol/clientInfo"), "name")) ||
          !sb_string(yyjson_obj_get(yyjson_obj_get(meta, "io.modelcontextprotocol/clientInfo"), "version")))))) {
        rpc_error(output, response, -32602, "2026 requests require protocolVersion and clientCapabilities in params._meta."); *http_status = 400;
    } else if (modern && !protocol && (!http_version || strcmp(http_version, requested) || !context ||
        !header_matches(context->method, method) ||
        (!strcmp(method, "tools/call") && !header_matches(context->name, sb_string(yyjson_obj_get(params, "name")))))) {
        rpc_error(output, response, -32020, "Required MCP headers are missing or do not match the request body."); *http_status = 400;
    } else if (protocol && ((modern && protocol->initialized) || (!modern && protocol->modern))) {
        rpc_error(output, response, -32600, "Cannot mix protocol eras on a stdio connection.");
    } else if (!id) {
        /* Notifications cannot initiate tools, initialization, or side effects. */
        if (!modern && !strcmp(method, "notifications/initialized") && protocol && protocol->initialized) protocol->ready = true;
        *http_status = 202;
        yyjson_doc_free(input); yyjson_mut_doc_free(output); return NULL;
    } else if (!modern && !strcmp(method, "initialize")) {
        const char *legacy_requested = sb_string(yyjson_obj_get(params, "protocolVersion"));
        yyjson_val *client = yyjson_obj_get(params, "clientInfo");
        if (!legacy_requested || !yyjson_is_obj(yyjson_obj_get(params, "capabilities")) ||
            !sb_string(yyjson_obj_get(client, "name")) || !sb_string(yyjson_obj_get(client, "version"))) {
            rpc_error(output, response, -32602, "Invalid initialize parameters.");
        } else if (protocol && protocol->initialized) {
            rpc_error(output, response, -32600, "Already initialized.");
        } else {
            yyjson_mut_val *result = yyjson_mut_obj(output);
            yyjson_mut_obj_add_str(output, result, "protocolVersion", sb_protocol_version(legacy_requested) && strcmp(legacy_requested, "2026-07-28") ? legacy_requested : "2025-11-25");
            yyjson_mut_obj_add_val(output, result, "capabilities", capabilities(output));
            yyjson_mut_obj_add_val(output, result, "serverInfo", server_info(output));
            yyjson_mut_obj_add_str(output, result, "instructions", "Use host_capabilities to inspect workspace ids, project policy, and available backends. Approval and execution depend on owner configuration. Overwrite and patch require expected_sha256 and a private backup. OAuth and tunnel management are unavailable.");
            yyjson_mut_obj_add_val(output, response, "result", result);
            if (protocol) protocol->initialized = true;
        }
    } else if (modern && !strcmp(method, "server/discover")) {
        yyjson_mut_val *result = yyjson_mut_obj(output);
        yyjson_mut_obj_add_val(output, result, "supportedVersions", versions(output));
        yyjson_mut_obj_add_val(output, result, "capabilities", capabilities(output));
        yyjson_mut_obj_add_uint(output, result, "ttlMs", 0);
        yyjson_mut_obj_add_str(output, result, "cacheScope", "private");
        yyjson_mut_obj_add_val(output, response, "result", result);
    } else if (!strcmp(method, "ping")) {
        yyjson_mut_obj_add_val(output, response, "result", yyjson_mut_obj(output));
    } else if (!modern && protocol && !protocol->ready) {
        rpc_error(output, response, -32600, "Initialize and send notifications/initialized first.");
    } else if (!strcmp(method, "tools/list")) {
        if (yyjson_obj_get(params, "cursor")) rpc_error(output, response, -32602, "No tool-list cursor is supported.");
        else yyjson_mut_obj_add_val(output, response, "result", sb_project_tools_list(host, output));
    } else if (!strcmp(method, "tools/call")) {
        sb_error error={0}; char *wire=sb_mcp_operation(params,&error);
        yyjson_doc *operation=wire ? yyjson_read(wire,strlen(wire),0) : NULL;
        yyjson_val *call=yyjson_doc_get_root(operation); const char *name=sb_string(yyjson_obj_get(call,"name"));
        if (!name) rpc_error(output,response,-32602,*error.text ? error.text : "Invalid tool-call parameters.");
        else if (!sb_tool_exists(name)) rpc_error(output,response,-32602,"Unknown tool.");
        else {
            yyjson_mut_val *result=sb_tool_call(host,context && context->actor ? context->actor :
                (protocol ? "stdio" : "http-development"),name,yyjson_obj_get(call,"arguments"),output);
            yyjson_mut_obj_add_val(output,response,"result",approval_prompt(host,output,result,params,modern));
        }
        yyjson_doc_free(operation); free(wire);
    } else {
        rpc_error(output, response, -32601, "Method not found.");
        if (modern && !protocol) *http_status = 404;
    }
    yyjson_mut_val *result = yyjson_mut_obj_get(response, "result");
    if (modern && result) {
        if (protocol) protocol->modern = true;
        if (!yyjson_mut_obj_get(result,"resultType")) yyjson_mut_obj_add_str(output, result, "resultType", "complete");
        yyjson_mut_val *result_meta = yyjson_mut_obj(output);
        yyjson_mut_obj_add_val(output, result_meta, "io.modelcontextprotocol/serverInfo", server_info(output));
        yyjson_mut_obj_add_val(output, result, "_meta", result_meta);
        if (!strcmp(method, "tools/list")) {
            yyjson_mut_obj_add_uint(output, result, "ttlMs", 0);
            yyjson_mut_obj_add_str(output, result, "cacheScope", "private");
        }
    }
    char *serialized = yyjson_mut_write(output, 0, response_length);
    yyjson_doc_free(input); yyjson_mut_doc_free(output);
    if (!serialized || *response_length > SB_RESPONSE_LIMIT) {
        free(serialized);
        const char fallback[] = "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32603,\"message\":\"Response allocation or size limit exceeded.\"}}";
        serialized = sb_strdup(fallback); *response_length = sizeof(fallback) - 1; *http_status = 500;
    }
    return serialized;
}
