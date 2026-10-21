#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "desktop_app.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static bool exists(const char *path) {
    wchar_t *name=sb_desktop_wide(path);DWORD flags=name?GetFileAttributesW(name):INVALID_FILE_ATTRIBUTES;free(name);return flags!=INVALID_FILE_ATTRIBUTES;
}
char *sb_desktop_absolute(const char *path){
    wchar_t *name=sb_desktop_wide(path),full[SB_PATH_CAP];DWORD n=name?GetFullPathNameW(name,SB_PATH_CAP,full,NULL):0;free(name);
    if(!n || n>=SB_PATH_CAP)return NULL;while(n>3 && (full[n-1]==L'\\' || full[n-1]==L'/'))full[--n]=0;
    int size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,full,-1,NULL,0,NULL,NULL);char *out=size?malloc((size_t)size):NULL;
    if(out)WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,full,-1,out,size,NULL,NULL);return out;
}
bool sb_desktop_publish_storage(const char *locator,const char *directory,sb_error *error) {
    if(!locator || !sb_platform_storage_path(locator,error) || !sb_platform_storage_path(directory,error))return false;
    yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL);if(!doc)return false;
    yyjson_mut_val *root=yyjson_mut_obj(doc);yyjson_mut_doc_set_root(doc,root);
    yyjson_mut_obj_add_uint(doc,root,"format_version",1);yyjson_mut_obj_add_str(doc,root,"directory",directory);
    char *json=yyjson_mut_write(doc,0,NULL);yyjson_mut_doc_free(doc);
    bool ok=json && sb_desktop_private_write(locator,json,strlen(json),true,error);free(json);return ok;
}
char *sb_desktop_resolve_storage(const char *override,char **locator,sb_error *error) {
    char *program=override?sb_desktop_absolute(override):sb_platform_program_directory(error);
    if(!program)return NULL;
    if(override){size_t n=strlen(program)+14;*locator=malloc(n);if(*locator)snprintf(*locator,n,"%s.storage.json",program);}
    else *locator=sb_desktop_join(program,"ShellBot.storage.json");char *base=NULL;
    bool configured=*locator && exists(*locator);
    if(configured){
        size_t length=0;char *bytes=sb_platform_load_config(*locator,&length,error);
        yyjson_doc *doc=bytes?yyjson_read(bytes,length,0):NULL;yyjson_val *root=yyjson_doc_get_root(doc);
        const char *value=sb_string(yyjson_obj_get(root,"directory"));
        if(doc && yyjson_obj_size(root)==2 && yyjson_get_uint(yyjson_obj_get(root,"format_version"))==1 && value)
            base=!strcmp(value,"Data")?sb_desktop_join(program,"Data"):sb_strdup(value);
        yyjson_doc_free(doc);free(bytes);
    }else base=override?sb_strdup(override):sb_desktop_join(program,"Data");
    bool ready=base && sb_platform_storage_path(base,error) && (!configured || exists(base)) && sb_desktop_make_directory(base,false,error);
    if(!ready){
        if(override){if(!*error->text)sb_fail(error,"指定的数据目录不可用；不会回退到用户目录或 C 盘。");free(base);base=NULL;}
        else {
            char *chosen=sb_desktop_choose_storage(base?base:"","数据位置不可用、位于 C 盘或程序目录不可写。请选择非 C 盘可写目录；取消则退出，不会自动改用其他位置。");free(base);base=chosen;
            if(base){
                memset(error,0,sizeof(*error));
                if(!sb_desktop_publish_storage(*locator,base,error)){
                    MessageBoxW(NULL,L"已选择数据目录，但程序目录不可写或位于 C 盘，无法保存位置设置。此次仍使用所选目录；下次启动需要重新选择。可将整个程序移到可写的非 C 盘目录。",L"ShellBot 数据位置",MB_OK|MB_ICONINFORMATION);
                    memset(error,0,sizeof(*error));free(*locator);*locator=NULL;
                }
            }else sb_fail(error,"未选择数据位置，程序已退出；没有使用 C 盘。");
        }
    }
    char *absolute=base?sb_desktop_absolute(base):NULL;free(base);free(program);return absolute;
}
