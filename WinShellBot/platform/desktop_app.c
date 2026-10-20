#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include "desktop_app.h"
#include "manager.h"
#include "manager_ui.h"
#include "execution.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
typedef struct { sb_desktop_draft value;char *file; } saved_project;
struct sb_desktop_app {
    char *directory,*manager_data,*index,*configuration;
    char origin[320],owner[257];unsigned port;size_t count;
    saved_project projects[SB_MAX_PROJECTS];
    sb_lease *lease;HANDLE stop,manager_stop,ready,thread,mutex;
    volatile LONG busy;int code;sb_error runtime_error;
    wchar_t instance_key[96];
};
static const char *str(yyjson_val *o,const char *key) { const char *v=sb_string(yyjson_obj_get(o,key));return v ? v : ""; }
static bool nonce(char out[65],sb_error *error) {
    unsigned char bytes[32];if (!sb_platform_random(bytes,sizeof(bytes),error)) return false;
    for (size_t i=0;i<32;++i) snprintf(out+i*2,3,"%02x",bytes[i]);SecureZeroMemory(bytes,sizeof(bytes));return true;
}
void sb_desktop_draft_free(sb_desktop_draft *draft) {
    if (!draft) return;free(draft->name);free(draft->directory);free(draft->definition);SecureZeroMemory(draft,sizeof(*draft));
}
static bool copy_draft(sb_desktop_draft *out,const sb_desktop_draft *in) {
    *out=*in;out->name=sb_strdup(in->name);out->directory=sb_strdup(in->directory);out->definition=sb_strdup(in->definition);
    if (out->name && out->directory && out->definition) return true;sb_desktop_draft_free(out);return false;
}
static yyjson_mut_val *project_value(yyjson_mut_doc *doc,const sb_desktop_draft *p) {
    yyjson_doc *input=yyjson_read(p->definition,strlen(p->definition),0);
    yyjson_mut_val *value=input ? yyjson_val_mut_copy(doc,yyjson_doc_get_root(input)) : NULL;yyjson_doc_free(input);return value;
}
static char *configuration(sb_desktop_app *app,const sb_desktop_draft *replacement,size_t index) {
    yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL);if (!doc) return NULL;
    yyjson_mut_val *root=yyjson_mut_obj(doc),*projects=yyjson_mut_arr(doc),*gateway=yyjson_mut_obj(doc);yyjson_mut_doc_set_root(doc,root);
    yyjson_mut_obj_add_uint(doc,root,"format_version",1);yyjson_mut_obj_add_val(doc,root,"projects",projects);
    size_t count=app->count+(replacement && index==app->count ? 1 : 0);
    for (size_t i=0;i<count;++i) yyjson_mut_arr_add_val(projects,project_value(doc,replacement && i==index ? replacement : &app->projects[i].value));
    yyjson_mut_obj_add_uint(doc,gateway,"port",app->port);yyjson_mut_obj_add_str(doc,gateway,"owner_token_env","SHELLBOT_DESKTOP_OWNER");
    if (*app->origin) yyjson_mut_obj_add_str(doc,gateway,"public_url",app->origin);
    yyjson_mut_obj_add_val(doc,root,"gateway",gateway);char *json=yyjson_mut_write(doc,0,NULL);yyjson_mut_doc_free(doc);return json;
}
static char *catalog(sb_desktop_app *app,const sb_desktop_draft *replacement,const char *file,size_t index) {
    yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL);if (!doc) return NULL;
    yyjson_mut_val *root=yyjson_mut_obj(doc),*projects=yyjson_mut_arr(doc);yyjson_mut_doc_set_root(doc,root);
    yyjson_mut_obj_add_uint(doc,root,"format_version",1);yyjson_mut_obj_add_uint(doc,root,"port",app->port);
    yyjson_mut_obj_add_str(doc,root,"public_url",app->origin);yyjson_mut_obj_add_val(doc,root,"projects",projects);
    size_t count=app->count+(replacement && index==app->count ? 1 : 0);
    for (size_t i=0;i<count;++i) {
        const sb_desktop_draft *p=replacement && index==i ? replacement : &app->projects[i].value;
        yyjson_mut_val *item=yyjson_mut_obj(doc);yyjson_mut_obj_add_str(doc,item,"id",p->id);yyjson_mut_obj_add_str(doc,item,"directory",p->directory);
        yyjson_mut_obj_add_str(doc,item,"file",replacement && index==i ? file : app->projects[i].file);yyjson_mut_arr_add_val(projects,item);
    }
    char *json=yyjson_mut_write(doc,0,NULL);yyjson_mut_doc_free(doc);return json;
}
static bool exists(const char *path) { wchar_t *p=sb_desktop_wide(path);DWORD attr=p ? GetFileAttributesW(p) : INVALID_FILE_ATTRIBUTES;free(p);return attr!=INVALID_FILE_ATTRIBUTES; }
static bool load_project(sb_desktop_app *app,yyjson_val *index,sb_error *error) {
    saved_project *entry=&app->projects[app->count];sb_desktop_draft *p=&entry->value;
    const char *id=str(index,"id"),*directory=str(index,"directory"),*file=str(index,"file");
    if (!sb_identifier(id) || !sb_project_path_valid(directory) || !sb_identifier(file) || strlen(file)>50 || app->count>=SB_MAX_PROJECTS) goto invalid;
    char leaf[80];snprintf(leaf,sizeof(leaf),"%s.sbproj",file);char *path=sb_desktop_join(directory,leaf),*plain=path ? sb_desktop_unseal(path,error) : NULL;
    yyjson_doc *doc=plain ? yyjson_read(plain,strlen(plain),0) : NULL;yyjson_val *root=yyjson_doc_get_root(doc),*definition=yyjson_obj_get(root,"definition");
    bool ok=doc && yyjson_obj_size(root)==5 && yyjson_get_uint(yyjson_obj_get(root,"format_version"))==1 && yyjson_is_bool(yyjson_obj_get(root,"automatic")) &&
        yyjson_is_obj(definition) && !strcmp(str(definition,"id"),id) && sb_identifier(str(definition,"token_env")) && strlen(str(root,"token"))==64 &&
        strspn(str(root,"token"),"0123456789abcdef")==64 && *str(root,"name") && strlen(str(root,"name"))<256;
    char *expected=sb_desktop_join(directory,"State");ok=ok && expected && !_stricmp(expected,str(definition,"data_dir"));free(expected);
    if (ok) {
        strcpy(p->id,id);strcpy(p->token_env,str(definition,"token_env"));strcpy(p->token,str(root,"token"));p->name=sb_strdup(str(root,"name"));
        p->directory=sb_strdup(directory);p->definition=yyjson_val_write(definition,0,NULL);p->automatic=yyjson_get_bool(yyjson_obj_get(root,"automatic"));entry->file=sb_strdup(file);
        ok=p->name && p->directory && p->definition && entry->file;
    }
    yyjson_doc_free(doc);if (plain) { SecureZeroMemory(plain,strlen(plain));free(plain); }free(path);
    if (!ok) goto invalid;++app->count;return true;
invalid:
    if (!*error->text) sb_fail(error,"项目索引或加密配置损坏；原文件保留，请使用原账户或备份恢复。");return false;
}
static DWORD WINAPI manager_thread(void *context) {
    sb_desktop_app *app=context;const char *ids[SB_MAX_PROJECTS];size_t count=0;
    for (size_t i=0;i<app->count;++i) if (app->projects[i].value.automatic) ids[count++]=app->projects[i].value.id;
    app->code=sb_manager_desktop_startup(app->configuration,app->manager_data,ids,count,app->manager_stop,app->ready);return (DWORD)app->code;
}
static bool runtime_alive(sb_desktop_app *app) { return app->thread && WaitForSingleObject(app->thread,0)==WAIT_TIMEOUT && WaitForSingleObject(app->ready,0)==WAIT_OBJECT_0; }
static bool stop_runtime(sb_desktop_app *app,sb_error *error) {
    if (!app->thread) return true;SetEvent(app->manager_stop);
    if (WaitForSingleObject(app->thread,30000)!=WAIT_OBJECT_0) { sb_fail(error,"项目尚未完全停止，管理界面保持运行，请等待活动请求结束。");return false; }
    CloseHandle(app->thread);app->thread=NULL;return true;
}
static bool start_runtime(sb_desktop_app *app,sb_error *error) {
    if(WaitForSingleObject(app->stop,0)==WAIT_OBJECT_0){sb_fail(error,"正在退出，不再启动项目。");return false;}
    if (runtime_alive(app)) return true;
    memset(&app->runtime_error,0,sizeof(app->runtime_error));
    if (!stop_runtime(app,error)) return false;
    free(app->configuration);app->configuration=configuration(app,NULL,0);
    if (!app->configuration || _putenv_s("SHELLBOT_DESKTOP_OWNER",app->owner)) goto failed;
    for (size_t i=0;i<app->count;++i) if (_putenv_s(app->projects[i].value.token_env,app->projects[i].value.token)) goto failed;
    if(WaitForSingleObject(app->stop,0)==WAIT_OBJECT_0)goto failed;
    ResetEvent(app->manager_stop);ResetEvent(app->ready);app->code=2;
    app->thread=CreateThread(NULL,0,manager_thread,app,0,NULL);if (!app->thread) goto failed;
    if(WaitForSingleObject(app->stop,0)==WAIT_OBJECT_0){SetEvent(app->manager_stop);goto failed;}
    HANDLE handles[]={app->ready,app->thread};DWORD result=WaitForMultipleObjects(2,handles,FALSE,15000);
    if (result==WAIT_OBJECT_0) return true;
failed:
    sb_fail(error,"网关未启动。请在“连接设置”检查端口是否已被其他程序占用；已保存的项目仍保留在列表中。");return false;
}
static char *manager_command(sb_desktop_app *app,const char *command,const char *id,const char *payload,sb_error *error) {
    yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL);if (!doc) return NULL;
    yyjson_mut_val *root=yyjson_mut_obj(doc);yyjson_mut_doc_set_root(doc,root);yyjson_mut_obj_add_str(doc,root,"command",command);
    if (id) yyjson_mut_obj_add_str(doc,root,"id",id);
    yyjson_doc *input=payload ? yyjson_read(payload,strlen(payload),0) : NULL;
    if (input) yyjson_mut_obj_add_val(doc,root,"request",yyjson_val_mut_copy(doc,yyjson_doc_get_root(input)));
    char *wire=yyjson_mut_write(doc,0,NULL),*reply=wire ? sb_owner_exchange(app->manager_data,wire,strlen(wire),error) : NULL;
    yyjson_doc_free(input);yyjson_mut_doc_free(doc);free(wire);
    yyjson_doc *result=reply ? yyjson_read(reply,strlen(reply),0) : NULL;const char *failure=str(yyjson_doc_get_root(result),"error");
    if (!result || *failure) { if (*failure) sb_fail(error,failure);free(reply);reply=NULL; }
    yyjson_doc_free(result);return reply;
}
static void augment(sb_desktop_app *app,yyjson_mut_doc *doc) {
    yyjson_mut_val *root=yyjson_mut_doc_get_root(doc),*projects=yyjson_mut_obj_get(root,"projects");
    if (*app->runtime_error.text) yyjson_mut_obj_add_strcpy(doc,root,"runtime_notice",app->runtime_error.text);
    for (size_t i=0;i<yyjson_mut_arr_size(projects);++i) {
        yyjson_mut_val *p=yyjson_mut_arr_get(projects,i);const char *id=yyjson_mut_get_str(yyjson_mut_obj_get(p,"project_id"));
        for (size_t j=0;id && j<app->count;++j) if (!strcmp(id,app->projects[j].value.id)) {
            yyjson_mut_obj_add_strcpy(doc,p,"display_name",app->projects[j].value.name);yyjson_mut_obj_add_strcpy(doc,p,"project_directory",app->projects[j].value.directory);
            yyjson_mut_obj_add_bool(doc,p,"automatic_start",app->projects[j].value.automatic);
        }
    }
}
char *sb_desktop_exchange(sb_desktop_app *app,const char *request,sb_error *error) {
    yyjson_doc *input=yyjson_read(request,strlen(request),0);const char *command=str(yyjson_doc_get_root(input),"command");char *reply=NULL;
    if (WaitForSingleObject(app->stop,0)==WAIT_OBJECT_0) {yyjson_doc_free(input);sb_fail(error,"正在退出，不接受新的项目操作。");return NULL;}
    if (InterlockedCompareExchange(&app->busy,0,0)) { yyjson_doc_free(input);sb_fail(error,"项目配置正在保存，请稍后刷新。");return NULL; }
    if (!runtime_alive(app) && !strcmp(command,"management-status")) {
        yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL);yyjson_mut_val *root=yyjson_mut_obj(doc),*array=yyjson_mut_arr(doc);yyjson_mut_doc_set_root(doc,root);
        yyjson_mut_obj_add_val(doc,root,"projects",array);yyjson_mut_obj_add_bool(doc,root,"http_gateway",false);
        yyjson_mut_obj_add_bool(doc,root,"remote_owner_enabled",true);yyjson_mut_obj_add_str(doc,root,"version",SB_VERSION);
        for (size_t i=0;i<app->count;++i) {
            yyjson_mut_val *p=yyjson_mut_obj(doc);yyjson_mut_obj_add_str(doc,p,"project_id",app->projects[i].value.id);yyjson_mut_obj_add_str(doc,p,"state","stopped");
            yyjson_mut_obj_add_bool(doc,p,"enabled",true);yyjson_mut_arr_add_val(array,p);
        }
        augment(app,doc);reply=yyjson_mut_write(doc,0,NULL);yyjson_mut_doc_free(doc);
    } else {
        if (!runtime_alive(app) && !start_runtime(app,error)) goto done;
        reply=sb_owner_exchange(app->manager_data,request,strlen(request),error);
        if (reply && !strcmp(command,"management-status")) {
            yyjson_doc *doc=yyjson_read(reply,strlen(reply),0);yyjson_mut_doc *copy=doc ? yyjson_doc_mut_copy(doc,NULL) : NULL;
            if (copy) { augment(app,copy);char *next=yyjson_mut_write(copy,0,NULL);if (next) { free(reply);reply=next; } }
            yyjson_mut_doc_free(copy);yyjson_doc_free(doc);
        }
    }
done:
    yyjson_doc_free(input);return reply;
}
bool sb_desktop_draft_load(sb_desktop_app *app,const char *id,sb_desktop_draft *draft,sb_error *error) {
    memset(draft,0,sizeof(*draft));
    if (id) { for (size_t i=0;i<app->count;++i) if (!strcmp(id,app->projects[i].value.id)) return copy_draft(draft,&app->projects[i].value);return false; }
    if (app->count>=SB_MAX_PROJECTS) { sb_fail(error,"最多可创建 16 个项目。");return false; }
    char random[65];if (!nonce(random,error) || !nonce(draft->token,error)) return false;
    snprintf(draft->id,sizeof(draft->id),"project-%.8s",random);snprintf(draft->token_env,sizeof(draft->token_env),"SB_DESKTOP_%.16s",random);
    PWSTR documents=NULL;char *base=NULL;
    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_Documents,0,NULL,&documents))) {
        int size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,documents,-1,NULL,0,NULL,NULL);base=size ? malloc((size_t)size) : NULL;
        if (base) WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,documents,-1,base,size,NULL,NULL);CoTaskMemFree(documents);
    }
    char *parent=base ? sb_desktop_join(base,"ShellBotProjects") : NULL;draft->directory=parent ? sb_desktop_join(parent,draft->id) : NULL;
    free(base);free(parent);draft->name=sb_strdup("新项目");draft->automatic=true;
    char *data=draft->directory ? sb_desktop_join(draft->directory,"State") : NULL,*work=draft->directory ? sb_desktop_join(draft->directory,"Workspace") : NULL;
    yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL);if (!doc || !data || !work) { yyjson_mut_doc_free(doc);free(data);free(work);sb_desktop_draft_free(draft);return false; }
    yyjson_mut_val *root=yyjson_mut_obj(doc),*array=yyjson_mut_arr(doc),*directory=yyjson_mut_obj(doc),*policy=yyjson_mut_obj(doc),*tools=yyjson_mut_arr(doc);yyjson_mut_doc_set_root(doc,root);
    yyjson_mut_obj_add_str(doc,root,"id",draft->id);yyjson_mut_obj_add_str(doc,root,"token_env",draft->token_env);yyjson_mut_obj_add_str(doc,root,"data_dir",data);
    yyjson_mut_obj_add_str(doc,root,"default_workspace_id","workspace");yyjson_mut_obj_add_uint(doc,root,"policy_revision",1);
    yyjson_mut_obj_add_str(doc,directory,"id","workspace");yyjson_mut_obj_add_str(doc,directory,"path",work);yyjson_mut_obj_add_bool(doc,directory,"read_only",false);
    yyjson_mut_arr_add_val(array,directory);yyjson_mut_obj_add_val(doc,root,"workspaces",array);yyjson_mut_obj_add_str(doc,policy,"approval_mode","workspace_auto");
    for (size_t i=0;i<SB_PROJECT_TOOL_COUNT;++i) { const char *name=sb_project_tool_name(i);if (strncmp(name,"task_",5) && (i<SB_DESKTOP_TOOL_FIRST || i>=SB_DESKTOP_TOOL_END)) yyjson_mut_arr_add_str(doc,tools,name); }
    yyjson_mut_obj_add_val(doc,policy,"tools",tools);yyjson_mut_obj_add_val(doc,root,"policy",policy);draft->definition=yyjson_mut_write(doc,0,NULL);yyjson_mut_doc_free(doc);free(data);free(work);
    return draft->definition && draft->name && draft->directory;
}
static char *remap(const char *path,const char *from,const char *to) {
    size_t n=strlen(from);if (strlen(path)<n || !sb_desktop_overlap(path,from)) return sb_strdup(path);
    for (size_t i=0;i<n;++i) { char a=path[i]=='/' ? '\\' : path[i],b=from[i]=='/' ? '\\' : from[i];if (a>='A' && a<='Z') a+=32;if (b>='A' && b<='Z') b+=32;if (a!=b) return sb_strdup(path); }
    size_t size=strlen(to)+strlen(path+n)+1;char *out=malloc(size);if (out) snprintf(out,size,"%s%s",to,path+n);return out;
}
static char *draft_definition(sb_desktop_draft *draft,const char *old_directory,uint64_t revision,sb_error *error) {
    yyjson_doc *source=yyjson_read(draft->definition,strlen(draft->definition),0);yyjson_mut_doc *doc=source ? yyjson_doc_mut_copy(source,NULL) : NULL;yyjson_doc_free(source);
    if (!doc) return NULL;yyjson_mut_val *root=yyjson_mut_doc_get_root(doc);char *data=sb_desktop_join(draft->directory,"State");
    yyjson_mut_obj_remove_key(root,"id");yyjson_mut_obj_add_strcpy(doc,root,"id",draft->id);yyjson_mut_obj_remove_key(root,"token_env");yyjson_mut_obj_add_strcpy(doc,root,"token_env",draft->token_env);
    yyjson_mut_obj_remove_key(root,"data_dir");yyjson_mut_obj_add_strcpy(doc,root,"data_dir",data);free(data);
    yyjson_mut_obj_remove_key(root,"policy_revision");yyjson_mut_obj_add_uint(doc,root,"policy_revision",revision);
    if (old_directory && _stricmp(old_directory,draft->directory)) {
        const char *keys[]={"workspaces","extra_directories"};
        for (size_t k=0;k<2;++k) { yyjson_mut_val *array=yyjson_mut_obj_get(root,keys[k]);for (size_t i=0;i<yyjson_mut_arr_size(array);++i) {
            yyjson_mut_val *item=yyjson_mut_arr_get(array,i);const char *path=yyjson_mut_get_str(yyjson_mut_obj_get(item,"path"));char *next=path ? remap(path,old_directory,draft->directory) : NULL;
            if (next) { yyjson_mut_obj_remove_key(item,"path");yyjson_mut_obj_add_strcpy(doc,item,"path",next);free(next); }
        } }
        yyjson_mut_val *runtime=yyjson_mut_obj_get(yyjson_mut_obj_get(yyjson_mut_obj_get(root,"policy"),"execution"),"runtime_files");
        for (size_t i=0;i<yyjson_mut_arr_size(runtime);++i) {
            const char *path=yyjson_mut_get_str(yyjson_mut_arr_get(runtime,i));char *next=path ? remap(path,old_directory,draft->directory) : NULL;
            if (next) {yyjson_mut_arr_replace(runtime,i,yyjson_mut_strcpy(doc,next));free(next);}
        }
    }
    char *out=yyjson_mut_write(doc,0,NULL);yyjson_mut_doc_free(doc);if (!out) sb_fail(error,"无法准备项目配置。");return out;
}
static bool project_boundaries(sb_desktop_app *app,const sb_desktop_draft *draft,const sb_project_registry *registry,size_t index,sb_error *error) {
    if (sb_desktop_overlap(draft->directory,app->directory)) goto conflict;
    for (size_t i=0;i<app->count;++i) if (i!=index && sb_desktop_overlap(draft->directory,app->projects[i].value.directory)) goto conflict;
    for (size_t i=0;i<registry->project_count;++i) if (registry->projects[i].policy.file_scope==SB_FILES_CONFIGURED) {
        const sb_project *p=&registry->projects[i];
        for (size_t d=0;d<p->directory_count;++d) {
            const char *path=p->directories[d].path;
            if (sb_desktop_overlap(path,app->directory)) goto conflict;
            for (size_t j=0;j<registry->project_count;++j) {
                const char *base=j==index ? draft->directory : app->projects[j].value.directory;
                if (strlen(path)<=strlen(base) && sb_desktop_overlap(path,base)) goto conflict;
                if (sb_desktop_overlap(path,registry->projects[j].data_dir)) goto conflict;
            }
        }
    }
    return true;
conflict:
    sb_fail(error,"项目存储目录须独立于管理器和其他项目；工作目录不能包含项目配置、凭据或 State 内部数据。请使用存储目录下的 Workspace 子目录。");return false;
}
static bool prepare_new_directory(const sb_desktop_draft *draft,sb_error *error) {
    char *path=sb_desktop_join(draft->directory,".shellbot-project.json"),hash[65];bool ok=false;
    if (!path || !sb_platform_sha256(draft->token,strlen(draft->token),hash,error)) goto done;
    if (exists(path)) {
        size_t length=0;char *bytes=sb_platform_load_config(path,&length,error);yyjson_doc *doc=bytes ? yyjson_read(bytes,length,0) : NULL;
        yyjson_val *root=yyjson_doc_get_root(doc);ok=doc && !strcmp(str(root,"id"),draft->id) && !strcmp(str(root,"token_sha256"),hash);
        yyjson_doc_free(doc);free(bytes);if (!ok) sb_fail(error,"该存储目录属于其他项目，请选择新的空目录。");
    } else if (sb_desktop_make_directory(draft->directory,true,error)) {
        char json[256];snprintf(json,sizeof(json),"{\"id\":\"%s\",\"token_sha256\":\"%s\"}",draft->id,hash);
        ok=sb_desktop_private_write(path,json,strlen(json),false,error);
    }
done:
    free(path);return ok;
}
bool sb_desktop_save(sb_desktop_app *app,const char *original_id,sb_desktop_draft *draft,sb_error *error) {
    if (InterlockedCompareExchange(&app->busy,1,0)) { sb_fail(error,"其他项目设置正在保存。");return false; }
    bool ok=false,was_running=false,was_alive=runtime_alive(app),catalog_written=false,unconfirmed=false;size_t index=app->count;
    char *definition=NULL,*next=NULL,*old_catalog=NULL,*next_catalog=NULL,*sealed=NULL,*plain=NULL,*file_path=NULL,*saved_file=NULL;
    sb_desktop_draft replacement={0};sb_project_registry *registry=calloc(1,sizeof(*registry));
    if(WaitForSingleObject(app->stop,0)==WAIT_OBJECT_0){sb_fail(error,"正在退出，未保存新的项目设置。");goto done;}
    if (original_id) { for (size_t i=0;i<app->count;++i) if (!strcmp(original_id,app->projects[i].value.id)) index=i;if (index==app->count) goto done; }
    if (!sb_identifier(draft->id) || (original_id && strcmp(original_id,draft->id)) || !draft->name || !*draft->name || strlen(draft->name)>255 ||
        !sb_project_path_valid(draft->directory) || draft->directory[1]!=':' || (index==app->count && app->count>=SB_MAX_PROJECTS)) {
        sb_fail(error,"请填写项目名称、有效的英数标识及本地绝对存储路径。已有项目标识保持固定。");goto done;
    }
    size_t directory_length=strlen(draft->directory);
    while(directory_length>3 && (draft->directory[directory_length-1]=='/' || draft->directory[directory_length-1]=='\\'))draft->directory[--directory_length]=0;
    for(char *c=draft->directory;*c;++c)if(*c=='/')*c='\\';
    for (size_t i=0;i<app->count;++i) if(i!=index && !strcmp(draft->id,app->projects[i].value.id)) {sb_fail(error,"项目标识已存在，请为新项目填写不同的地址标识。");goto done;}
    uint64_t revision=1;yyjson_doc *prior=index<app->count ? yyjson_read(app->projects[index].value.definition,strlen(app->projects[index].value.definition),0) : NULL;
    if (prior) revision=yyjson_get_uint(yyjson_obj_get(yyjson_doc_get_root(prior),"policy_revision"))+1;
    yyjson_doc_free(prior);if (!revision) { sb_fail(error,"项目策略版本已耗尽。");goto done; }
    const char *previous=index<app->count ? app->projects[index].value.directory : NULL;
    definition=draft_definition(draft,previous,revision,error);if (!definition) goto done;free(draft->definition);draft->definition=definition;definition=NULL;
    next=configuration(app,draft,index);
    if (!registry || !next || !sb_projects_parse(registry,next,strlen(next),error) || !project_boundaries(app,draft,registry,index,error)) goto done;
    if (was_alive && index<app->count) {
        char *state=manager_command(app,"project-status",original_id,NULL,error);yyjson_doc *doc=state ? yyjson_read(state,strlen(state),0) : NULL;
        was_running=!strcmp(str(yyjson_obj_get(yyjson_doc_get_root(doc),"project"),"state"),"running");yyjson_doc_free(doc);free(state);
        char *stopped=manager_command(app,"project-stop",original_id,NULL,error);if (!stopped) goto done;free(stopped);
    }
    bool relocated=previous && _stricmp(previous,draft->directory);
    if (relocated) { if (!sb_desktop_copy_tree(previous,draft->directory,app->stop,error)) goto done; }
    else if (index==app->count && !prepare_new_directory(draft,error)) goto done;
    for (size_t d=0;d<registry->projects[index].directory_count;++d) {
        const char *path=registry->projects[index].directories[d].path;
        if (strlen(path)>strlen(draft->directory) && sb_desktop_overlap(path,draft->directory)) {
            if (!sb_desktop_make_directory(path,false,error)) goto done;
        } else if (!exists(path)) { sb_fail(error,"配置的外部工作目录不存在，请先创建该目录。");goto done; }
    }
    yyjson_mut_doc *project=yyjson_mut_doc_new(NULL);yyjson_mut_val *root=project ? yyjson_mut_obj(project) : NULL;
    if (!project) goto done;yyjson_mut_doc_set_root(project,root);yyjson_mut_obj_add_uint(project,root,"format_version",1);
    yyjson_mut_obj_add_str(project,root,"name",draft->name);yyjson_mut_obj_add_bool(project,root,"automatic",draft->automatic);yyjson_mut_obj_add_str(project,root,"token",draft->token);
    yyjson_mut_obj_add_val(project,root,"definition",project_value(project,draft));plain=yyjson_mut_write(project,0,NULL);yyjson_mut_doc_free(project);
    sealed=plain ? sb_desktop_seal(plain,error) : NULL;char random[65],file[48];if (!sealed || !nonce(random,error)) goto done;
    snprintf(file,sizeof(file),"project-%.32s",random);char leaf[64];snprintf(leaf,sizeof(leaf),"%s.sbproj",file);file_path=sb_desktop_join(draft->directory,leaf);
    if (!file_path || !sb_desktop_private_write(file_path,sealed,strlen(sealed),false,error)) goto done;
    saved_file=sb_strdup(file);if (!saved_file || !copy_draft(&replacement,draft)) { sb_fail(error,"无法准备项目内存，请重试；原配置保留。");goto done; }
    if(WaitForSingleObject(app->stop,0)==WAIT_OBJECT_0){sb_fail(error,"已退出，原项目注册与目录保留。");goto done;}
    old_catalog=catalog(app,NULL,NULL,0);next_catalog=catalog(app,draft,file,index);
    if (!old_catalog || !next_catalog || !sb_desktop_private_write(app->index,next_catalog,strlen(next_catalog),true,error)) goto done;catalog_written=true;
    if (_putenv_s(draft->token_env,draft->token)) { sb_fail(error,"无法准备内部项目凭据。");goto done; }
    if (was_alive) {
        char *applied=manager_command(app,"configuration-apply",NULL,next,error);
        if (!applied) {
            sb_error verification={0};char *actual=manager_command(app,"configuration-snapshot",NULL,NULL,&verification);
            yyjson_doc *observed=actual ? yyjson_read(actual,strlen(actual),0) : NULL,*expected=yyjson_read(next,strlen(next),0);
            bool matches=observed && expected && yyjson_equals(yyjson_obj_get(yyjson_doc_get_root(observed),"configuration"),yyjson_doc_get_root(expected));
            bool known=observed!=NULL;yyjson_doc_free(observed);yyjson_doc_free(expected);free(actual);
            if (known && !matches) goto done;
            unconfirmed=!known;memset(error,0,sizeof(*error));
        }
        free(applied);
    }
    if (index<app->count) { sb_desktop_draft_free(&app->projects[index].value);free(app->projects[index].file); }
    app->projects[index].value=replacement;memset(&replacement,0,sizeof(replacement));app->projects[index].file=saved_file;saved_file=NULL;if (index==app->count) ++app->count;ok=true;memset(&app->runtime_error,0,sizeof(app->runtime_error));
    if (unconfirmed) sb_fail(&app->runtime_error,"配置已保存，但运行配置尚未确认。请刷新诊断；退出后重新打开会加载已保存配置，不重放旧任务。");
    if (!unconfirmed && !was_alive) { sb_error runtime={0};start_runtime(app,&runtime);app->runtime_error=runtime; }
    if (!unconfirmed && runtime_alive(app) && (was_running || (!original_id && draft->automatic))) { sb_error runtime={0};char *started=manager_command(app,"project-start",draft->id,NULL,&runtime);free(started);app->runtime_error=runtime; }
done:
    if (!ok && catalog_written && old_catalog) { sb_error rollback={0};if (!sb_desktop_private_write(app->index,old_catalog,strlen(old_catalog),true,&rollback)) sb_fail(error,"运行配置未切换，索引回退失败；两份项目配置均保留，请退出后核对索引备份。"); }
    if (!ok && was_running && original_id && was_alive && WaitForSingleObject(app->stop,0)!=WAIT_OBJECT_0) { sb_error resume={0};char *resumed=manager_command(app,"project-start",original_id,NULL,&resume);free(resumed); }
    if (!ok && !*error->text) sb_fail(error,"项目未保存，原配置及目录保留。");
    if (plain) { SecureZeroMemory(plain,strlen(plain));free(plain); }free(sealed);free(file_path);free(definition);free(next);free(old_catalog);free(next_catalog);free(saved_file);sb_desktop_draft_free(&replacement);sb_projects_free(registry);free(registry);
    InterlockedExchange(&app->busy,0);return ok;
}
bool sb_desktop_connection(sb_desktop_app *app,unsigned port,const char *origin,sb_error *error) {
    if (port<1024 || port>65535 || strlen(origin)>=sizeof(app->origin)) { sb_fail(error,"端口须为 1024～65535；公共地址请填写域名或 HTTP(S) 来源地址。");return false; }
    sb_config config;sb_config_defaults(&config);bool valid=!*origin || sb_config_public_url(&config,origin,error);sb_config_free(&config);if (!valid) return false;
    if (InterlockedCompareExchange(&app->busy,1,0)) return false;
    if (!stop_runtime(app,error)) { InterlockedExchange(&app->busy,0);return false; }
    unsigned old=app->port;char previous[320];strcpy(previous,app->origin);app->port=port;strcpy(app->origin,origin);
    char *json=catalog(app,NULL,NULL,0);bool ok=json && sb_desktop_private_write(app->index,json,strlen(json),true,error);free(json);
    if (!ok) { app->port=old;strcpy(app->origin,previous); }
    sb_error runtime={0};start_runtime(app,&runtime);app->runtime_error=runtime;InterlockedExchange(&app->busy,0);return ok;
}
unsigned sb_desktop_port(sb_desktop_app *app) {return app->port;}
const char *sb_desktop_origin(sb_desktop_app *app) {return app->origin;}
const char *sb_desktop_data(sb_desktop_app *app) {return app->manager_data;}
const char *sb_desktop_owner_token(sb_desktop_app *app) {return app->owner;}
HANDLE sb_desktop_stop(sb_desktop_app *app) {return app->stop;}
void sb_desktop_request_exit(sb_desktop_app *app) {SetEvent(app->stop);SetEvent(app->manager_stop);}
const wchar_t *sb_desktop_instance_key(sb_desktop_app *app) {return app->instance_key;}
bool sb_desktop_is_busy(sb_desktop_app *app) {return InterlockedCompareExchange(&app->busy,0,0)!=0;}
static void close_app(sb_desktop_app *app) {
    if (!app) return;sb_error error={0};if (!stop_runtime(app,&error)) ExitProcess(3);
    for (size_t i=0;i<SB_MAX_PROJECTS;++i) { if (*app->projects[i].value.token_env) _putenv_s(app->projects[i].value.token_env,"");sb_desktop_draft_free(&app->projects[i].value);free(app->projects[i].file); }
    _putenv_s("SHELLBOT_DESKTOP_OWNER","");SecureZeroMemory(app->owner,sizeof(app->owner));
    if (app->stop) CloseHandle(app->stop);if (app->manager_stop) CloseHandle(app->manager_stop);if (app->ready) CloseHandle(app->ready);
    sb_platform_lease_close(app->lease);if (app->mutex) CloseHandle(app->mutex);free(app->directory);free(app->manager_data);free(app->index);free(app->configuration);free(app);
}
static BOOL CALLBACK activate_instance(HWND window,LPARAM parameter) {
    const wchar_t *key=(const wchar_t *)parameter;
    if (GetPropW(window,key)) { PostMessageW(window,WM_APP+19,0,0);return FALSE; }return TRUE;
}
int sb_desktop_main(const char *directory) {
    sb_error error={0};sb_desktop_app *app=calloc(1,sizeof(*app));int code=2;
    if (!app) return 2;
    HANDLE token=NULL;bool allowed=OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token) && sb_win_owner_token_allowed(token);if (token) CloseHandle(token);
    if (!allowed) { sb_fail(&error,"管理界面需要交互式 Windows 所有者账户。");goto done; }
    char *base=directory ? sb_strdup(directory) : sb_platform_default_data_dir(&error);
    app->directory=base ? sb_desktop_join(base,"Desktop") : NULL;free(base);if (!app->directory) goto done;
    if (!sb_desktop_make_directory(app->directory,false,&error)) goto done;
    char hash[65],identity[50];sb_root *directory_root=sb_platform_root(app->directory,&error);
    bool identified=directory_root && sb_platform_root_identity(directory_root,identity,&error);sb_platform_root_close(directory_root);
    if (!identified || !sb_platform_sha256(identity,strlen(identity),hash,&error)) goto done;
    swprintf(app->instance_key,96,L"ShellBotDesktop-%.32hs",hash);wchar_t mutex_name[112];swprintf(mutex_name,112,L"Local\\%ls",app->instance_key);app->mutex=CreateMutexW(NULL,FALSE,mutex_name);
    if (!app->mutex) goto done;if (GetLastError()==ERROR_ALREADY_EXISTS) {
        EnumWindows(activate_instance,(LPARAM)app->instance_key);code=0;goto done;
    }
    app->lease=sb_platform_lease(app->directory,&error);if (!app->lease) goto done;
    app->manager_data=sb_desktop_join(app->directory,"Manager");app->index=sb_desktop_join(app->directory,"catalog.json");app->port=8765;
    app->stop=CreateEventW(NULL,TRUE,FALSE,NULL);app->manager_stop=CreateEventW(NULL,TRUE,FALSE,NULL);app->ready=CreateEventW(NULL,TRUE,FALSE,NULL);
    if (!app->manager_data || !app->index || !app->stop || !app->manager_stop || !app->ready) goto done;
    if (exists(app->index)) {
        size_t length=0;char *bytes=sb_platform_load_config(app->index,&length,&error);yyjson_doc *doc=bytes ? yyjson_read(bytes,length,0) : NULL;yyjson_val *root=yyjson_doc_get_root(doc),*projects=yyjson_obj_get(root,"projects");
        bool valid=doc && yyjson_get_uint(yyjson_obj_get(root,"format_version"))==1 && yyjson_obj_size(root)==4 && yyjson_is_arr(projects) && yyjson_arr_size(projects)<=SB_MAX_PROJECTS &&
            yyjson_is_uint(yyjson_obj_get(root,"port")) && yyjson_get_uint(yyjson_obj_get(root,"port"))>=1024 && yyjson_get_uint(yyjson_obj_get(root,"port"))<=65535 && strlen(str(root,"public_url"))<sizeof(app->origin);
        if (valid) { app->port=(unsigned)yyjson_get_uint(yyjson_obj_get(root,"port"));strcpy(app->origin,str(root,"public_url"));for (size_t i=0;i<yyjson_arr_size(projects) && valid;++i) valid=load_project(app,yyjson_arr_get(projects,i),&error); }
        yyjson_doc_free(doc);free(bytes);if (!valid) goto done;
    }
    char *owner_path=sb_desktop_join(app->directory,"owner.sbkey"),*owner=owner_path && exists(owner_path) ? sb_desktop_unseal(owner_path,&error) : NULL;
    if (owner_path && !exists(owner_path)) { if (!nonce(app->owner,&error)) { free(owner_path);goto done; }char *sealed=sb_desktop_seal(app->owner,&error);bool saved=sealed && sb_desktop_private_write(owner_path,sealed,strlen(sealed),false,&error);free(sealed);if (!saved) { free(owner_path);goto done; } }
    else if (owner && strlen(owner)==64 && strspn(owner,"0123456789abcdef")==64) strcpy(app->owner,owner);
    if (owner) { SecureZeroMemory(owner,strlen(owner));free(owner); }free(owner_path);if (!*app->owner) goto done;
    app->configuration=configuration(app,NULL,0);sb_project_registry *registry=calloc(1,sizeof(*registry));
    bool valid=registry && app->configuration && sb_projects_parse(registry,app->configuration,strlen(app->configuration),&error);sb_projects_free(registry);free(registry);if (!valid) goto done;
    start_runtime(app,&app->runtime_error);FreeConsole();code=sb_manager_desktop_ui(app);
done:
    if (code) { wchar_t *message=sb_desktop_wide(*error.text ? error.text : "无法打开项目管理；原配置保持完整。");MessageBoxW(NULL,message,L"ShellBot",MB_OK|MB_ICONERROR);free(message); }
    close_app(app);return code;
}
