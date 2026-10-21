#include "sb.h"
#include <stdlib.h>
#include <string.h>

static bool task_key_valid(const char *key) {
    return key && *key && strlen(key)<=64 && strspn(key,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.")==strlen(key);
}
static yyjson_mut_val *task_error(yyjson_mut_doc *doc, bool *failed, const char *message) {
    *failed = true; yyjson_mut_val *result = yyjson_mut_obj(doc); yyjson_mut_obj_add_strcpy(doc,result,"error",message); return result;
}
yyjson_mut_val *sb_tasks_call(sb_host *host,const char *actor,const char *name,yyjson_val *args,yyjson_mut_doc *doc,bool *failed) {
    yyjson_mut_doc *request=yyjson_mut_doc_new(NULL); if(!request) return task_error(doc,failed,"Cannot allocate task request.");
    yyjson_mut_val *parameters=yyjson_mut_obj(request); sb_file *directory=NULL,*program_file=NULL;sb_root *program_root=NULL;char *program_relative=NULL; char *cwd=NULL,*canonical=NULL,*reply=NULL; sb_error error={0};
    const char *workspace_id=NULL; char request_hash[65]={0},program_hash[65]={0}; const char *event_key=NULL;
    if(!yyjson_is_obj(args)) { sb_fail(&error,"Task arguments must be an object."); goto done; }
    const char *instance=sb_string(yyjson_obj_get(args,"host_instance_id"));
    if (!instance || !sb_execution_instance(host->execution) || strcmp(instance,sb_execution_instance(host->execution))) {
        sb_fail(&error,"Host instance changed or is missing; refresh host_capabilities and review the prior task before submitting another request.");goto done;
    }
    if(!strcmp(name,"task_start")) {
        workspace_id=sb_string(yyjson_obj_get(args,"workspace_id"));
        const char *path=sb_string(yyjson_obj_get(args,"path")),*exe=sb_string(yyjson_obj_get(args,"executable")),*key=sb_string(yyjson_obj_get(args,"request_key"));
        yyjson_val *argv=yyjson_obj_get(args,"argv"),*timeout_value=yyjson_obj_get(args,"timeout_ms"); uint64_t timeout=host->config.limits.task_timeout_ms < 300000 ? host->config.limits.task_timeout_ms : 300000;
        if(!workspace_id || !exe || !task_key_valid(key) || !yyjson_is_arr(argv) || yyjson_arr_size(argv)>64 || (yyjson_obj_get(args,"path") && !path) ||
            (timeout_value && (!sb_integer(timeout_value,&timeout) || timeout<100 || timeout>host->config.limits.task_timeout_ms))) { sb_fail(&error,"Invalid task program, argv, request key, working directory or timeout."); goto done; }
        sb_workspace *workspace=sb_workspace_find(&host->config,workspace_id);
        if(!workspace) { sb_fail(&error,"Unknown workspace_id."); goto done; }
        directory=sb_platform_open(workspace->root,path?path:".",true,&error);
        if(!directory) goto done;
        cwd=sb_platform_directory_path(directory,&error); if(!cwd) goto done;
        if(host->approved_program_version) {
            char cwd_version[65],program_version[65];
            program_root=sb_platform_account_parent(exe,&program_relative,&error);
            program_file=program_root?sb_platform_open(program_root,program_relative,false,&error):NULL;
            bool same=program_file && sb_platform_file_version(program_file,program_version,&error) && !strcmp(program_version,host->approved_program_version) &&
                sb_platform_file_version(directory,cwd_version,&error) && !strcmp(cwd_version,host->approved_target_version);
            if(!same){sb_fail(&error,"Approved program or working-directory snapshot changed before task startup.");goto done;}
            if(!sb_platform_file_sha256(program_file,program_hash,&error))goto done;
            yyjson_mut_obj_add_strcpy(request,parameters,"executable_sha256",program_hash);
        }
        yyjson_mut_obj_add_strcpy(request,parameters,"executable",exe);
        yyjson_mut_obj_add_val(request,parameters,"argv",yyjson_val_mut_copy(request,argv));
        yyjson_mut_obj_add_strcpy(request,parameters,"cwd",cwd);
        yyjson_mut_obj_add_strcpy(request,parameters,"workspace_id",workspace_id);
        yyjson_mut_obj_add_uint(request,parameters,"timeout_ms",timeout);
        canonical=yyjson_mut_val_write(parameters,0,NULL);
        if(!canonical || !sb_platform_sha256(canonical,strlen(canonical),request_hash,&error)) goto done;
        yyjson_mut_obj_add_strcpy(request,parameters,"request_key",key);
        yyjson_mut_obj_add_strcpy(request,parameters,"request_sha256",request_hash);
        yyjson_mut_obj_add_strcpy(request,parameters,"token_env",host->config.token_env);
        yyjson_mut_obj_add_strcpy(request,parameters,"runtime_directory",host->config.data_dir);
        event_key=key;
    } else {
        const char *id=sb_string(yyjson_obj_get(args,"task_id"));
        if(!id || strlen(id)!=32 || strspn(id,"0123456789abcdef")!=32) { sb_fail(&error,"Invalid task_id.");goto done; }
        yyjson_mut_obj_add_strcpy(request,parameters,"task_id",id); event_key=id;
        if(!strcmp(name,"task_output")) {
            const char *stream=sb_string(yyjson_obj_get(args,"stream")); uint64_t offset=0,maximum=16384;
            yyjson_val *offset_value=yyjson_obj_get(args,"offset"),*limit_value=yyjson_obj_get(args,"max_bytes");
            if((yyjson_obj_get(args,"stream") && (!stream || (strcmp(stream,"stdout") && strcmp(stream,"stderr")))) ||
                (offset_value && !sb_integer(offset_value,&offset)) || (limit_value && (!sb_integer(limit_value,&maximum) || !maximum || maximum>16384))) { sb_fail(&error,"Invalid output stream or page.");goto done; }
            yyjson_mut_obj_add_str(request,parameters,"stream",stream?stream:"stdout");
            yyjson_mut_obj_add_uint(request,parameters,"offset",offset);yyjson_mut_obj_add_uint(request,parameters,"max_bytes",maximum);
        } else if(!strcmp(name,"task_input")) {
            const char *key=sb_string(yyjson_obj_get(args,"input_key")),*text=sb_string(yyjson_obj_get(args,"text")); yyjson_val *eof=yyjson_obj_get(args,"eof");
            if(!task_key_valid(key) || !text || strlen(text)>4096 || (eof && !yyjson_is_bool(eof))) { sb_fail(&error,"Invalid stdin key, text or EOF.");goto done; }
            yyjson_mut_obj_add_strcpy(request,parameters,"input_key",key); yyjson_mut_obj_add_strcpy(request,parameters,"text",text);
            yyjson_mut_obj_add_bool(request,parameters,"eof",yyjson_get_bool(eof));
            if(!sb_platform_sha256(text,strlen(text),request_hash,&error)) goto done;
        }
    }
    yyjson_mut_obj_add_strcpy(request,parameters,"actor",actor);
    yyjson_mut_doc_set_root(request,parameters);
    free(canonical); canonical=yyjson_mut_write(request,0,NULL);
    yyjson_doc *parsed=canonical?yyjson_read(canonical,strlen(canonical),0):NULL;
    if(!parsed) { sb_fail(&error,"Cannot serialize normalized task arguments.");goto done; }
    bool changing=!strcmp(name,"task_start") || !strcmp(name,"task_input") || !strcmp(name,"task_cancel");
    if(changing) {
        bool recorded=sb_audit_task(host->audit,"task_operation_requested",actor,event_key,NULL,host->config.execution_profile,*request_hash?request_hash:NULL,name,0);
        if(!recorded && strcmp(name,"task_cancel")) { yyjson_doc_free(parsed);sb_fail(&error,"Audit unavailable; task start and stdin writes are blocked.");goto done; }
    }
    reply=sb_execution_task(host->execution,name,yyjson_doc_get_root(parsed),&error); yyjson_doc_free(parsed);
done:
    free(canonical);free(cwd);sb_platform_close(directory);sb_platform_close(program_file);sb_platform_root_close(program_root);free(program_relative);yyjson_mut_doc_free(request);
    if(!reply) return task_error(doc,failed,*error.text?error.text:"Task backend unavailable.");
    yyjson_doc *response=yyjson_read(reply,strlen(reply),0);free(reply);
    if(!response) return task_error(doc,failed,"Task backend returned an invalid response; retry using the original request key.");
    yyjson_val *root=yyjson_doc_get_root(response); *failed=yyjson_get_bool(yyjson_obj_get(root,"_error"));
    yyjson_mut_val *result=yyjson_val_mut_copy(doc,root);
    yyjson_mut_obj_add_strcpy(doc,result,"host_instance_id",sb_execution_instance(host->execution));
    if(*program_hash)yyjson_mut_obj_add_strcpy(doc,result,"executable_sha256",program_hash);
    if(!*failed && !strcmp(name,"task_start") && yyjson_get_bool(yyjson_obj_get(root,"created_new"))) {
        const char *id=sb_string(yyjson_obj_get(root,"task_id"));uint64_t pid=0;sb_integer(yyjson_obj_get(root,"process_id"),&pid);
        if(!sb_audit_task(host->audit,"task_started",actor,event_key,id,host->config.execution_profile,request_hash,name,pid)) yyjson_mut_obj_add_str(doc,result,"audit_warning","Task started but its result record could not be flushed; query the original task, do not rerun it.");
    }
    yyjson_doc_free(response);return result;
}
