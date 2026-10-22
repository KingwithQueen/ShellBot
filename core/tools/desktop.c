#include "sb.h"
#include "sb_projects.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool sb_desktop_tool(const char *name) {
    int index=sb_project_tool_index(name);
    return index>=SB_DESKTOP_TOOL_FIRST && index<SB_DESKTOP_TOOL_END;
}
bool sb_desktop_tool_visible(sb_host *host, const char *name) {
    if (!host->project || host->project->legacy_compatibility || !host->desktop) return false;
    sb_request_context context={.actor="tool-discovery",.project=host->project,.policy_revision=host->project->policy_revision};
    sb_project_operation operation={.tool=name,.identity=SB_ID_CURRENT_USER};
    return sb_project_evaluate(&context,&operation).decision!=SB_DECISION_DENY && sb_desktop_available(host->desktop);
}
static void add_properties(yyjson_mut_doc *doc, yyjson_mut_val *properties, const char *json) {
    yyjson_doc *input=yyjson_read(json,strlen(json),0);
    size_t i,n; yyjson_val *key,*value;
    yyjson_obj_foreach(yyjson_doc_get_root(input),i,n,key,value)
        yyjson_mut_obj_add(properties,yyjson_mut_strcpy(doc,yyjson_get_str(key)),yyjson_val_mut_copy(doc,value));
    yyjson_doc_free(input);
}
void sb_desktop_tools_list(sb_host *host, yyjson_mut_doc *doc, yyjson_mut_val *tools) {
    for (size_t i=SB_DESKTOP_TOOL_FIRST;i<SB_DESKTOP_TOOL_END;++i) {
        const char *name=sb_project_tool_name(i);
        if (!sb_desktop_tool_visible(host,name)) continue;
        yyjson_mut_val *tool=yyjson_mut_obj(doc),*schema=yyjson_mut_obj(doc),*properties=yyjson_mut_obj(doc),*required=yyjson_mut_arr(doc);
        yyjson_mut_obj_add_str(doc,tool,"name",name);
        yyjson_mut_obj_add_str(doc,tool,"description",
            "Explicit ordinary-project desktop permission and caller-bound expiring lease required. "
            "Request owner approval when pending; retry unchanged parameters with approval_id. Use a fresh request_key for each new observation/action. "
#ifdef SB_POSIX
            "Actions require screen_id from a recent observation and coordinates in its reported coordinate_space; refresh after any action. "
            "Connect locally and grant system permissions first. Read host_capabilities.desktop_status for platform limits and human-input pause support. "
            "The local owner can pause all workers and is the only party allowed to resume. Wayland has no arbitrary window focus/list or global human-input detection. "
#else
            "Actions require screen_id from a recent observation and physical desktop coordinates; refresh after any action. "
            "Human/foreign input or Ctrl+Alt+configured function key pauses all ShellBot desktop control; only the local owner resumes it. "
#endif
            "Input submission is not proof of application success. No elevated, lock-screen or UAC desktop automation.");
        yyjson_mut_obj_add_str(doc,schema,"type","object");
        yyjson_mut_obj_add_bool(doc,schema,"additionalProperties",false);
        yyjson_mut_obj_add_val(doc,schema,"properties",properties);
        yyjson_mut_obj_add_val(doc,schema,"required",required);
        add_properties(doc,properties,"{\"lease_id\":{\"type\":\"string\",\"minLength\":32,\"maxLength\":32},\"request_key\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":64},\"approval_id\":{\"type\":\"string\",\"minLength\":32,\"maxLength\":32}}");
        if (i==15) {
            add_properties(doc,properties,"{\"action\":{\"type\":\"string\",\"enum\":[\"acquire\",\"renew\",\"release\",\"status\"]},\"duration_seconds\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":120}}");
            yyjson_mut_arr_add_str(doc,required,"action");
        } else {
            yyjson_mut_arr_add_str(doc,required,"lease_id"); yyjson_mut_arr_add_str(doc,required,"request_key");
        }
        if (i>=19 && i<=24) {
            add_properties(doc,properties,"{\"screen_id\":{\"type\":\"string\",\"minLength\":32,\"maxLength\":32}}");
            yyjson_mut_arr_add_str(doc,required,"screen_id");
        }
        if (i==17) {
            add_properties(doc,properties,"{\"monitor_id\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":15},\"max_dimension\":{\"type\":\"integer\",\"minimum\":200,\"maximum\":1600},\"x\":{\"type\":\"integer\"},\"y\":{\"type\":\"integer\"},\"width\":{\"type\":\"integer\",\"minimum\":1},\"height\":{\"type\":\"integer\",\"minimum\":1}}");
        }
        if (i==19) {
            add_properties(doc,properties,"{\"window_id\":{\"type\":\"string\",\"minLength\":32,\"maxLength\":32}}"); yyjson_mut_arr_add_str(doc,required,"window_id");
        }
        if (i>=20 && i<=22) {
            add_properties(doc,properties,"{\"x\":{\"type\":\"integer\"},\"y\":{\"type\":\"integer\"}}");
            yyjson_mut_arr_add_str(doc,required,"x"); yyjson_mut_arr_add_str(doc,required,"y");
        }
        if (i==20 || i==21) add_properties(doc,properties,"{\"button\":{\"type\":\"string\",\"enum\":[\"left\",\"right\",\"middle\"]}}");
        if (i==20) add_properties(doc,properties,"{\"clicks\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":2}}");
        if (i==21) {
            add_properties(doc,properties,"{\"to_x\":{\"type\":\"integer\"},\"to_y\":{\"type\":\"integer\"},\"duration_ms\":{\"type\":\"integer\",\"minimum\":50,\"maximum\":1000}}");
            yyjson_mut_arr_add_str(doc,required,"to_x"); yyjson_mut_arr_add_str(doc,required,"to_y");
        }
        if (i==22) {
            add_properties(doc,properties,"{\"delta\":{\"type\":\"integer\",\"minimum\":-1200,\"maximum\":1200},\"horizontal\":{\"type\":\"boolean\"}}"); yyjson_mut_arr_add_str(doc,required,"delta");
        }
        if (i==23 || i==26) {
            add_properties(doc,properties,"{\"text\":{\"type\":\"string\",\"maxLength\":4096}}"); yyjson_mut_arr_add_str(doc,required,"text");
        }
        if (i==24) {
            add_properties(doc,properties,"{\"keys\":{\"type\":\"array\",\"minItems\":1,\"maxItems\":4,\"items\":{\"type\":\"string\"}}}"); yyjson_mut_arr_add_str(doc,required,"keys");
        }
        if (i==25 || i==26) {
            add_properties(doc,properties,"{\"expected_sequence\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":4294967295}}"); yyjson_mut_arr_add_str(doc,required,"expected_sequence");
        }
        yyjson_mut_obj_add_val(doc,tool,"inputSchema",schema);
        yyjson_mut_val *annotations=yyjson_mut_obj(doc);
        bool changing=i==15 || (i>=19 && i<=24) || i==26;
        yyjson_mut_obj_add_bool(doc,annotations,"readOnlyHint",!changing);
        yyjson_mut_obj_add_bool(doc,annotations,"destructiveHint",changing);
        yyjson_mut_obj_add_bool(doc,annotations,"idempotentHint",true);
        yyjson_mut_obj_add_bool(doc,annotations,"openWorldHint",true);
        yyjson_mut_obj_add_val(doc,tool,"annotations",annotations); yyjson_mut_arr_add_val(tools,tool);
    }
}
static yyjson_mut_val *wrap(yyjson_mut_doc *doc, yyjson_mut_val *data, bool failed) {
    yyjson_mut_val *root=yyjson_mut_obj(doc),*content=yyjson_mut_arr(doc),*text=yyjson_mut_obj(doc);
    char *json=yyjson_mut_val_write(data,0,NULL);
    if (!json) return NULL;
    yyjson_mut_obj_add_bool(doc,root,"isError",failed); yyjson_mut_obj_add_val(doc,root,"structuredContent",data);
    yyjson_mut_obj_add_str(doc,text,"type","text"); yyjson_mut_obj_add_strcpy(doc,text,"text",json); free(json);
    yyjson_mut_arr_add_val(content,text); yyjson_mut_obj_add_val(doc,root,"content",content); return root;
}
static yyjson_mut_val *error_result(yyjson_mut_doc *doc, const char *text) {
    yyjson_mut_val *data=yyjson_mut_obj(doc); yyjson_mut_obj_add_strcpy(doc,data,"error",text); return wrap(doc,data,true);
}
yyjson_mut_val *sb_desktop_tools_call(sb_host *host, const char *actor, const char *name, yyjson_val *args, yyjson_mut_doc *doc) {
    if (!host->project || host->project->legacy_compatibility || host->administrator_host || !host->desktop)
        return error_result(doc,"desktop_requires_enabled_ordinary_project");
    const char *action=sb_string(yyjson_obj_get(args,"action"));
    bool cleanup=!strcmp(name,"desktop_lease") && action && (!strcmp(action,"release") || !strcmp(action,"status"));
    sb_request_context context={.actor=actor,.project=host->project,.policy_revision=host->project->policy_revision};
    sb_project_operation operation={.tool=name,.identity=SB_ID_CURRENT_USER,.desktop_cleanup=cleanup};
    sb_policy_result decision=sb_project_evaluate(&context,&operation);
    if (decision.decision==SB_DECISION_DENY) return error_result(doc,decision.reason);
    char *prepared=sb_desktop_request(host->desktop,actor,name,args,true);
    yyjson_doc *metadata=prepared ? yyjson_read(prepared,strlen(prepared),0) : NULL;
    yyjson_val *body=yyjson_doc_get_root(metadata);
    if (!body || yyjson_get_bool(yyjson_obj_get(body,"isError"))) {
        yyjson_mut_val *failed=body ? yyjson_val_mut_copy(doc,body) : error_result(doc,"Desktop preparation unavailable; no automatic retry.");
        yyjson_doc_free(metadata); free(prepared); return failed;
    }
    char ticket[33]={0}; yyjson_mut_val *reply=NULL;
    if (!cleanup) {
        /* Platform returns sorted normalized arguments, excluding approval_id. */
        const char *parameters=sb_string(yyjson_obj_get(body,"parameters")),*target=sb_string(yyjson_obj_get(body,"target")),*version=sb_string(yyjson_obj_get(body,"version"));
        const char *approval=sb_string(yyjson_obj_get(args,"approval_id"));
        if (!parameters || !target || !version) { yyjson_doc_free(metadata); free(prepared); return error_result(doc,"Invalid desktop authorization binding."); }
        bool execute=sb_policy_operation_begin(host,actor,name,parameters,target,version,approval,
            decision.decision==SB_DECISION_PENDING,0,1,doc,&reply,ticket);
        if (!execute) {
            yyjson_doc_free(metadata); free(prepared);
            if (reply && yyjson_mut_obj_get(reply,"structuredContent")) return reply;
            return reply ? wrap(doc,reply,true) : error_result(doc,"Desktop authorization unavailable.");
        }
    }
    yyjson_doc_free(metadata); free(prepared);
    char *wire=sb_desktop_request(host->desktop,actor,name,args,false);
    yyjson_doc *response=wire ? yyjson_read(wire,strlen(wire),0) : NULL;
    reply=response ? yyjson_val_mut_copy(doc,yyjson_doc_get_root(response)) : error_result(doc,"Desktop operation did not return a receipt. Review possible effects before any new request.");
    yyjson_doc_free(response); free(wire);
    if (*ticket && !sb_policy_operation_finish(host->policy,ticket,reply)) {
        yyjson_mut_val *data=yyjson_mut_obj_get(reply,"structuredContent");
        yyjson_mut_obj_add_str(doc,data,"audit_warning","Operation receipt could not be flushed. Do not replay input; inspect the result.");
    }
    return reply;
}
