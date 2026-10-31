#include "sb.h"
#include "sb_projects.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

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
    yyjson_mut_obj_add_strcpy(doc, error, "message", message);
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
        if (yyjson_arr_size(value) > 65536) return false;
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
        char *text=NULL;
#ifndef SB_POSIX
        const char *mode=sb_string(yyjson_obj_get(yyjson_obj_get(params,"arguments"),"response_mode"));
        if(mode && !strcmp(mode,"compact")){
            char summary[1024];snprintf(summary,sizeof(summary),"Owner approval required: inspect %s, then retry unchanged arguments with approval_id %s; opening the link does not approve execution.",url,id);text=sb_strdup(summary);
        }else
#endif
        text=yyjson_mut_val_write(details,0,NULL);
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

bool sb_rpc_reply_valid(yyjson_val *reply,yyjson_val *request) {
    const char *version=sb_string(yyjson_obj_get(reply,"jsonrpc"));
    yyjson_val *id=yyjson_obj_get(reply,"id"),*expected=yyjson_obj_get(request,"id"),*result=yyjson_obj_get(reply,"result"),*error=yyjson_obj_get(reply,"error");
    if(!version || strcmp(version,"2.0") || !id || (!!result==!!error))return false;
    bool expected_valid=yyjson_is_int(expected) || sb_string(expected)!=NULL;
    if(expected_valid ? !yyjson_equals(id,expected) : (!yyjson_is_null(id) || !error))return false;
    if(result)return yyjson_is_obj(result);
    return yyjson_is_int(yyjson_obj_get(error,"code")) && sb_string(yyjson_obj_get(error,"message"))!=NULL;
}

void sb_request_id(char id[33]) {
    unsigned char bytes[16]; sb_error error={0};
    if (!sb_platform_random(bytes,sizeof(bytes),&error)) {
        /* Correlation only, never an authorization secret. */
        static volatile long sequence;
#ifdef _WIN32
        uint64_t stamp=sb_platform_millis() ^ ((uint64_t)GetCurrentProcessId()<<32), serial=(uint64_t)InterlockedIncrement(&sequence);
#else
        uint64_t stamp=sb_platform_millis(), serial=(uint64_t)__sync_add_and_fetch(&sequence,1);
#endif
        memcpy(bytes,&stamp,8);memcpy(bytes+8,&serial,8);
    }
    for(size_t i=0;i<16;++i)snprintf(id+i*2,3,"%02x",bytes[i]);
}

char *sb_rpc_internal_error(sb_host *host,yyjson_val *request,const char *request_id,
    const char *instance,const char *stage,bool invoked,const char *reason) {
    yyjson_val *id=yyjson_obj_get(request,"id"),*params=yyjson_obj_get(request,"params"),*args=yyjson_obj_get(params,"arguments");
    const char *tool=sb_string(yyjson_obj_get(params,"name"));if(!tool || !sb_tool_exists(tool))tool="unknown";
    sb_audit_diagnostic(host?host->audit:NULL,request_id,instance,stage,tool,args,reason);
    yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL);if(!doc)return NULL;
    yyjson_mut_val *root=yyjson_mut_obj(doc),*error=yyjson_mut_obj(doc),*data=yyjson_mut_obj(doc);
    const char *guidance=!invoked?"retry_original_request":"inspect_state_before_retry";
    if(invoked && (!strcmp(tool,"task_start") || !strcmp(tool,"task_input")))guidance="same_key_only";
    if(invoked && (!strcmp(tool,"desktop_click") || !strcmp(tool,"desktop_drag") || !strcmp(tool,"desktop_scroll") ||
        !strcmp(tool,"desktop_type") || !strcmp(tool,"desktop_key") || !strcmp(tool,"window_focus") || !strcmp(tool,"clipboard_write")))guidance="observe_before_retry_no_automatic_replay";
    bool ok=root && error && data && yyjson_mut_obj_add_str(doc,root,"jsonrpc","2.0") &&
        yyjson_mut_obj_add_val(doc,root,"id",(yyjson_is_int(id) || sb_string(id))?yyjson_val_mut_copy(doc,id):yyjson_mut_null(doc)) &&
        yyjson_mut_obj_add_int(doc,error,"code",-32603) && yyjson_mut_obj_add_str(doc,error,"message","Internal error; inspect error.data and correlate the server diagnostic.") &&
        yyjson_mut_obj_add_str(doc,data,"error_code","INTERNAL_ERROR") && yyjson_mut_obj_add_strcpy(doc,data,"request_id",request_id) &&
        yyjson_mut_obj_add_strcpy(doc,data,"host_instance_id",instance?instance:"") && yyjson_mut_obj_add_strcpy(doc,data,"stage",stage) &&
        yyjson_mut_obj_add_str(doc,data,"execution_state",invoked?"unknown":"not_started") &&
        yyjson_mut_obj_add_str(doc,data,"retry_guidance",guidance) && yyjson_mut_obj_add_strcpy(doc,data,"reason",reason) &&
        yyjson_mut_obj_add_val(doc,error,"data",data) && yyjson_mut_obj_add_val(doc,root,"error",error);
    yyjson_mut_doc_set_root(doc,root);char *wire=ok?yyjson_mut_write(doc,0,NULL):NULL;
    yyjson_mut_doc_free(doc);return wire;
}

char *sb_dispatch(sb_host *host, sb_protocol *protocol, const char *request,
                  size_t length, const sb_request_context *context, size_t *response_length, int *http_status) {
    *response_length = 0; *http_status = 200;
    char generated_id[33];sb_request_id(generated_id);
    const char *request_id=context && context->request_id?context->request_id:generated_id;
    const char *instance=sb_execution_instance(host->execution),*stage="parse",*internal_reason=NULL;bool invoked=false;
    yyjson_doc *input = length <= SB_REQUEST_LIMIT ? yyjson_read(request, length, 0) : NULL;
    yyjson_mut_doc *output = yyjson_mut_doc_new(NULL);
    if (!output) {
        char *failure=sb_rpc_internal_error(host,yyjson_doc_get_root(input),request_id,instance,stage,false,"response_allocation_failed");
        yyjson_doc_free(input);*http_status=500;*response_length=failure?strlen(failure):0;return failure;
    }
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
        if(length>SB_REQUEST_LIMIT){yyjson_mut_val *data=sb_limit_error(output,"request.body",length,SB_REQUEST_LIMIT,"bytes");yyjson_mut_obj_add_val(output,yyjson_mut_obj_get(response,"error"),"data",data);}
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
        else {
            stage="worker_reply";yyjson_mut_val *listing=sb_project_tools_list(host,output);
            if(!yyjson_mut_is_obj(listing))internal_reason="invalid_tool_list_reply";
            else yyjson_mut_obj_add_val(output,response,"result",listing);
        }
    } else if (!strcmp(method, "tools/call")) {
        sb_error error={0}; char *wire=sb_mcp_operation(params,&error);
        yyjson_doc *operation=wire ? yyjson_read(wire,strlen(wire),0) : NULL;
        yyjson_val *call=yyjson_doc_get_root(operation); const char *name=sb_string(yyjson_obj_get(call,"name"));
        if (!name) {
            if(!*error.text)internal_reason="operation_allocation_failed";
            else rpc_error(output,response,-32602,error.text);
        }
        else if (!sb_tool_exists(name)) rpc_error(output,response,-32602,"Unknown tool.");
        else {
            stage="worker_invoke";invoked=true;
            yyjson_mut_val *result=sb_tool_call(host,context && context->actor ? context->actor :
                (protocol ? "stdio" : "http-development"),name,yyjson_obj_get(call,"arguments"),output);
            stage="worker_reply";
            /* Detach operation argument/name strings before freeing the operation. */
            result=yyjson_mut_is_obj(result)?yyjson_mut_val_mut_copy(output,result):NULL;
            if(!result)internal_reason="invalid_or_unallocatable_tool_reply";
            else if(!yyjson_mut_obj_add_val(output,response,"result",approval_prompt(host,output,result,params,modern)))internal_reason="response_allocation_failed";
        }
        yyjson_doc_free(operation); free(wire);
    } else {
        rpc_error(output, response, -32601, "Method not found.");
        if (modern && !protocol) *http_status = 404;
    }
    if(!strcmp(stage,"parse") && input)stage="authorize";
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
    if(result){
        yyjson_mut_val *trace=yyjson_mut_obj_get(result,"_meta");
        if(!trace){trace=yyjson_mut_obj(output);yyjson_mut_obj_add_val(output,result,"_meta",trace);}
        yyjson_mut_obj_add_strcpy(output,trace,"shellbot/request_id",request_id);
    }
    yyjson_mut_val *rpc_failure=yyjson_mut_obj_get(response,"error");
    if(rpc_failure){
        yyjson_mut_val *data=yyjson_mut_obj_get(rpc_failure,"data");
        if(!data){data=yyjson_mut_obj(output);yyjson_mut_obj_add_val(output,rpc_failure,"data",data);}
        yyjson_mut_obj_add_strcpy(output,data,"request_id",request_id);
        yyjson_mut_obj_add_str(output,data,"stage",stage);
    }
    if(!internal_reason && !yyjson_mut_is_obj(result) && !yyjson_mut_is_obj(rpc_failure))internal_reason="incomplete_rpc_reply";
    char *serialized=internal_reason?NULL:yyjson_mut_write(output,0,response_length);
    if(!internal_reason && (!serialized || *response_length>SB_RESPONSE_LIMIT)){
        stage="serialize";internal_reason=serialized?"response_size_limit_exceeded":"response_serialization_failed";
    }
    if(internal_reason){
        free(serialized);serialized=sb_rpc_internal_error(host,root,request_id,instance,stage,invoked,internal_reason);
        *response_length=serialized?strlen(serialized):0;*http_status=500;
    }
    yyjson_doc_free(input);yyjson_mut_doc_free(output);return serialized;
}
