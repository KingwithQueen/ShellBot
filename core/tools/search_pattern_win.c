#define PCRE2_CODE_UNIT_WIDTH 8
#define PCRE2_STATIC
#include "pcre2.h"
#include "sb_search_pattern.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct sb_search_pattern {
    pcre2_code *query,*filter;
    pcre2_match_context *context;
    pcre2_match_data *matches;
    const char *literal,*failure;
    bool glob;
    uint64_t deadline;
};
static int deadline_callout(pcre2_callout_block *block,void *parameter){
    (void)block;sb_search_pattern *p=parameter;
    return sb_platform_millis()>=p->deadline?PCRE2_ERROR_CALLOUT:0;
}
static pcre2_code *compile(const char *text,bool glob,bool literal,bool insensitive,sb_error *error){
    PCRE2_UCHAR *converted=NULL;PCRE2_SIZE length=0,offset=0;int code=0;
    const char *input=text;char *escaped=NULL;
    if(glob){
        pcre2_convert_context *context=pcre2_convert_context_create(NULL);
        if(!context){sb_fail(error,"Cannot allocate glob converter.");return NULL;}
        pcre2_set_glob_separator(context,'/');
        code=pcre2_pattern_convert((PCRE2_SPTR)text,PCRE2_ZERO_TERMINATED,PCRE2_CONVERT_GLOB|PCRE2_CONVERT_UTF,&converted,&length,context);
        pcre2_convert_context_free(context);
        if(code){snprintf(error->text,sizeof(error->text),"Invalid glob: PCRE2 error %d at byte %zu.",code,(size_t)length);return NULL;}
        input=(const char*)converted;
    }else if(literal){
        escaped=malloc(strlen(text)*2+1);if(!escaped){sb_fail(error,"Cannot allocate literal pattern.");return NULL;}
        char *next=escaped;for(const char *c=text;*c;++c){if(strchr("\\^$.|?*+()[]{}",*c))*next++='\\';*next++=*c;}*next=0;input=escaped;
    }
    uint32_t flags=PCRE2_UTF|PCRE2_UCP|PCRE2_AUTO_CALLOUT|PCRE2_NEVER_BACKSLASH_C|(insensitive?PCRE2_CASELESS:0);
    pcre2_compile_context *context=pcre2_compile_context_create(NULL);
    if(!context){pcre2_converted_pattern_free(converted);free(escaped);sb_fail(error,"Cannot allocate regex compiler.");return NULL;}
    pcre2_set_max_pattern_length(context,262144);pcre2_set_max_pattern_compiled_length(context,8388608);
    pcre2_code *result=pcre2_compile((PCRE2_SPTR)input,PCRE2_ZERO_TERMINATED,flags,&code,&offset,context);pcre2_compile_context_free(context);
    if(!result){PCRE2_UCHAR message[192];pcre2_get_error_message(code,message,sizeof(message));snprintf(error->text,sizeof(error->text),"Invalid %s at UTF-8 byte %zu: %s",glob?"glob":"regex",(size_t)offset,(char*)message);}
    pcre2_converted_pattern_free(converted);free(escaped);return result;
}
sb_search_pattern *sb_search_pattern_new(const char *query,yyjson_val *args,uint64_t deadline,sb_error *error){
    const char *type=sb_string(yyjson_obj_get(args,"pattern_type")),*filter=sb_string(yyjson_obj_get(args,"file_glob"));yyjson_val *ic=yyjson_obj_get(args,"ignore_case");
    if((yyjson_obj_get(args,"pattern_type") && (!type || (strcmp(type,"literal") && strcmp(type,"regex") && strcmp(type,"glob")))) ||
       (ic && !yyjson_is_bool(ic)) || (yyjson_obj_get(args,"file_glob") && (!filter || !*filter || strlen(filter)>16384))){sb_fail(error,"Invalid pattern_type, ignore_case or file_glob (1..16384 UTF-8 bytes).");return NULL;}
    sb_search_pattern *p=calloc(1,sizeof(*p));if(!p){sb_fail(error,"Cannot allocate search pattern.");return NULL;}
    p->deadline=deadline;p->glob=type && !strcmp(type,"glob");p->literal=query;
    bool insensitive=yyjson_get_bool(ic);
    p->context=pcre2_match_context_create(NULL);p->matches=pcre2_match_data_create(1,NULL);
    if(!p->context || !p->matches)goto failed;
    pcre2_set_match_limit(p->context,1000000);pcre2_set_depth_limit(p->context,1000);pcre2_set_heap_limit(p->context,8192);pcre2_set_callout(p->context,deadline_callout,p);
    if((type && strcmp(type,"literal")) || insensitive){p->query=compile(query,p->glob,!type || !strcmp(type,"literal"),insensitive,error);if(!p->query)goto failed;}
    if(filter){p->filter=compile(filter,true,false,insensitive,error);if(!p->filter)goto failed;}
    return p;
failed:
    if(!*error->text)sb_fail(error,"Cannot allocate search pattern.");sb_search_pattern_free(p);return NULL;
}
void sb_search_pattern_free(sb_search_pattern *p){if(p){pcre2_code_free(p->query);pcre2_code_free(p->filter);pcre2_match_context_free(p->context);pcre2_match_data_free(p->matches);free(p);}}
static int run(sb_search_pattern *p,pcre2_code *code,const char *text,size_t length,size_t *offset){
    if(p->failure)return -1;
    int result=pcre2_match(code,(PCRE2_SPTR)text,length,0,0,p->matches,p->context);
    if(result>=0){if(offset)*offset=(size_t)pcre2_get_ovector_pointer(p->matches)[0];return 1;}
    if(result==PCRE2_ERROR_NOMATCH)return 0;
    p->failure=result==PCRE2_ERROR_CALLOUT?"regex_time_budget":result==PCRE2_ERROR_MATCHLIMIT?"regex_match_budget":result==PCRE2_ERROR_DEPTHLIMIT?"regex_depth_budget":result==PCRE2_ERROR_HEAPLIMIT?"regex_heap_budget":"regex_match_error";return -1;
}
int sb_search_pattern_match(sb_search_pattern *p,const char *text,size_t length,bool path,size_t *offset){
    (void)path;
    if(p->query)return run(p,p->query,text,length,offset);
    const char *found=strstr(text,p->literal);if(found && offset)*offset=(size_t)(found-text);return found?1:0;
}
int sb_search_pattern_include(sb_search_pattern *p,const char *path){return p->filter?run(p,p->filter,path,strlen(path),NULL):1;}
const char *sb_search_pattern_error(sb_search_pattern *p){return p->failure;}
