#include "sb.h"
#include "sb_projects.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TRANSFER_RECORDS 32
#define TRANSFER_ACTIVE 8
#define TRANSFER_TOTAL SB_TRANSFER_TOTAL
typedef enum { ACTIVE, COMPLETE, CANCELLED, EXPIRED_TRANSFER, FAILED_TRANSFER } transfer_state;
typedef struct {
    char id[33],actor[64],key[65],arguments[65],hash[65],version[65],ticket[33];
    char *filename;
    bool upload;
    transfer_state state;
    uint64_t size,next,deadline,last_offset,last_budget;
    size_t last_size;
    unsigned char *last;
    sb_file *file;
    sb_create *destination;
} transfer;
struct sb_transfers { sb_mutex *mutex; transfer records[TRANSFER_RECORDS]; };
bool sb_transfer_tool(const char *name) { return name && (!strcmp(name,"artifact_download") || !strcmp(name,"artifact_upload")); }
sb_transfers *sb_transfers_new(sb_error *error) {
    sb_transfers *value=calloc(1,sizeof(*value));if(value)value->mutex=sb_platform_mutex_new();
    if(value && value->mutex)return value;free(value);sb_fail(error,"Transfer registry unavailable.");return NULL;
}
static void release(transfer *r) { sb_platform_close(r->file);r->file=NULL;sb_platform_create_close(r->destination);r->destination=NULL; }
void sb_transfers_free(sb_transfers *value) {
    if(!value)return;for(size_t i=0;i<TRANSFER_RECORDS;++i){release(&value->records[i]);free(value->records[i].filename);free(value->records[i].last);}
    sb_platform_mutex_free(value->mutex);free(value);
}
static yyjson_mut_val *metadata(yyjson_mut_doc *doc,const transfer *r) {
    static const char *const states[]={"active","completed","cancelled","expired","failed"};
    yyjson_mut_val *out=yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc,out,"transfer_id",r->id);yyjson_mut_obj_add_str(doc,out,"direction",r->upload?"upload":"download");
    yyjson_mut_obj_add_str(doc,out,"status",states[r->state]);yyjson_mut_obj_add_strcpy(doc,out,"filename",r->filename);
    yyjson_mut_obj_add_uint(doc,out,"size",r->size);yyjson_mut_obj_add_strcpy(doc,out,"sha256",r->hash);
    yyjson_mut_obj_add_uint(doc,out,"next_offset",r->next);yyjson_mut_obj_add_uint(doc,out,"max_chunk_bytes",SB_TRANSFER_CHUNK);
    yyjson_mut_obj_add_uint(doc,out,"default_chunk_bytes",SB_TRANSFER_DEFAULT_CHUNK);
    yyjson_mut_obj_add_bool(doc,out,"source_modified",false);yyjson_mut_obj_add_bool(doc,out,"overwrite",false);
    yyjson_mut_obj_add_bool(doc,out,"committed",r->upload && r->state==COMPLETE);
    yyjson_mut_obj_add_bool(doc,out,"restart_resumable",false);
    if(*r->ticket)yyjson_mut_obj_add_strcpy(doc,out,"approval_id",r->ticket);
    return out;
}
static bool hash_argument(yyjson_val *args,char out[65]) {
    const char *s=sb_string(yyjson_obj_get(args,"expected_sha256"));
    if(!s || strlen(s)!=64 || strspn(s,"0123456789abcdefABCDEF")!=64)return false;
    for(size_t i=0;i<64;++i)out[i]=s[i]>='A'&&s[i]<='F' ? (char)(s[i]+32) : s[i];out[64]=0;return true;
}
static bool number(yyjson_val *args,const char *key,uint64_t fallback,uint64_t max,uint64_t *out) {
    yyjson_val *v=yyjson_obj_get(args,key);*out=fallback;return !v || (sb_integer(v,out) && *out<=max);
}
/* Standard, padded, canonical Base64 only; no whitespace or alternate alphabet. */
static bool decode(const char *s,unsigned char *out,size_t *size) {
    static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t n=s ? strlen(s) : 0;*size=0;if(!n || n%4 || n>4*((SB_TRANSFER_CHUNK+2)/3))return false;
    for(size_t i=0;i<n;i+=4){unsigned bits=0;unsigned padding=0;
        for(size_t j=0;j<4;++j){const char *at=strchr(alphabet,s[i+j]);
            if(s[i+j]=='='){if(i+4!=n || j<2)return false;++padding;bits<<=6;}
            else {if(!at || padding)return false;bits=(bits<<6)|(unsigned)(at-alphabet);}}
        if(padding>2 || (padding==1 && (bits&255)) || (padding==2 && (bits&65535)))return false;
        size_t count=3-padding;if(count>SB_TRANSFER_CHUNK-*size)return false;
        for(size_t j=0;j<count;++j)out[(*size)++]=(unsigned char)(bits>>(16-j*8));
    }return true;
}
static bool digest_open(yyjson_val *args,char out[65],sb_error *error) {
    static const char *const fields[]={"action","workspace_id","path","request_key","size","expected_sha256","ttl_seconds","identity"};
    yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL);yyjson_mut_val *object=doc?yyjson_mut_obj(doc):NULL;
    if(!object){yyjson_mut_doc_free(doc);return false;}
    for(size_t i=0;i<sizeof(fields)/sizeof(*fields);++i){yyjson_val *v=yyjson_obj_get(args,fields[i]);
        if(v)yyjson_mut_obj_add_val(doc,object,fields[i],yyjson_val_mut_copy(doc,v));}
    char *wire=yyjson_mut_val_write(object,0,NULL);bool ok=wire && sb_platform_sha256(wire,strlen(wire),out,error);
    free(wire);yyjson_mut_doc_free(doc);return ok;
}
yyjson_mut_val *sb_transfer_call(sb_host *host,const char *actor,const char *name,yyjson_val *args,
    yyjson_mut_doc *doc,sb_workspace *workspace,const char *path,const char *ticket,bool *failed) {
    sb_error error={0};yyjson_mut_val *out=NULL;unsigned char *bytes=NULL;*failed=true;
    sb_transfers *value=host->transfers;const char *action=sb_string(yyjson_obj_get(args,"action"));bool upload=!strcmp(name,"artifact_upload");
    if(!value || !host->project || host->project->legacy_compatibility || !action || !actor || strlen(actor)>=64){sb_fail(&error,"Transfers require a running versioned project.");goto finish;}
    sb_platform_mutex_lock(value->mutex);
    for(size_t i=0;i<TRANSFER_RECORDS;++i){transfer *r=&value->records[i];if(*r->id && r->state==ACTIVE && sb_platform_millis()>=r->deadline){release(r);r->state=EXPIRED_TRANSFER;}}
    transfer *r=NULL;
    if(!strcmp(action,"open")) {
        const char *key=sb_string(yyjson_obj_get(args,"request_key"));char digest[65];uint64_t ttl,size=0;
        if(!workspace || !path || !key || !*key || strlen(key)>64 || !number(args,"ttl_seconds",SB_TRANSFER_TTL,SB_TRANSFER_MAX_TTL,&ttl) || !ttl || !digest_open(args,digest,&error)) { sb_fail(&error,"Open requires a target, request_key and ttl_seconds within max_transfer_ttl_seconds from host_capabilities.");goto unlock; }
        for(size_t i=0;i<TRANSFER_RECORDS;++i)if(*value->records[i].id && !strcmp(value->records[i].actor,actor) && !strcmp(value->records[i].key,key)) {
            r=&value->records[i];if(r->upload!=upload || strcmp(r->arguments,digest) || strcmp(r->ticket,ticket?ticket:"")){sb_fail(&error,"request_key belongs to another transfer or permit.");goto unlock;}
            out=metadata(doc,r);*failed=false;goto unlock;
        }
        if(upload && (!sb_integer(yyjson_obj_get(args,"size"),&size) || size>SB_TRANSFER_LIMIT)) {sb_fail(&error,"Upload size is required and must fit max_transfer_bytes from host_capabilities.");goto unlock;}
        unsigned active=0;uint64_t total=0;
        for(size_t i=0;i<TRANSFER_RECORDS;++i){transfer *current=&value->records[i];if(!*current->id && !r)r=current;
            if(current->state==ACTIVE && *current->id){++active;total+=current->size;}}
        if(!r || active>=TRANSFER_ACTIVE){sb_fail(&error,"Transfer record or active-transfer budget exhausted; cancel active transfers or review receipts before restarting.");goto unlock;}
        transfer candidate={0};candidate.upload=upload;candidate.size=size;candidate.last_offset=UINT64_MAX;
        bool ok=true;const char *leaf=strrchr(path,'/'),*back=strrchr(path,'\\');if(back && (!leaf || back>leaf))leaf=back;
        candidate.filename=sb_strdup(leaf?leaf+1:path);
        if(upload) {
            ok=!workspace->read_only && hash_argument(args,candidate.hash);
            candidate.destination=ok?sb_platform_prepare_create(workspace->root,path,&error):NULL;
            ok=ok && candidate.destination && sb_platform_create_version(candidate.destination,candidate.version,&error);
            candidate.file=ok ? sb_platform_transfer_spool(host->lease,&error):NULL;ok=ok && candidate.file;
        } else {
            candidate.file=sb_platform_open(workspace->root,path,false,&error);candidate.size=candidate.file?sb_platform_size(candidate.file):0;
            ok=candidate.file && candidate.size<=SB_TRANSFER_LIMIT && sb_platform_file_version(candidate.file,candidate.version,&error);
            char after[65];ok=ok && sb_platform_file_sha256(candidate.file,candidate.hash,&error) && sb_platform_file_version(candidate.file,after,&error) && !strcmp(after,candidate.version);
            if(yyjson_obj_get(args,"expected_sha256")){char expected[65];ok=ok && hash_argument(args,expected) && !strcmp(expected,candidate.hash);}
        }
        if(ok && host->approved_target_version)ok=!strcmp(host->approved_target_version,candidate.version);
        if(!ok || !candidate.filename || candidate.size>TRANSFER_TOTAL-total){release(&candidate);free(candidate.filename);
            if(!*error.text)sb_fail(&error,"Transfer source, destination, checksum or total-byte budget is invalid.");goto unlock;}
        unsigned char nonce[16];if(!sb_platform_random(nonce,sizeof(nonce),&error)){release(&candidate);free(candidate.filename);goto unlock;}
        for(size_t i=0;i<sizeof(nonce);++i)snprintf(candidate.id+i*2,3,"%02x",nonce[i]);
        strcpy(candidate.actor,actor);strcpy(candidate.key,key);strcpy(candidate.arguments,digest);if(ticket)strcpy(candidate.ticket,ticket);
        candidate.deadline=sb_platform_millis()+ttl*1000;*r=candidate;out=metadata(doc,r);*failed=false;goto unlock;
    }
    const char *id=sb_string(yyjson_obj_get(args,"transfer_id"));
    for(size_t i=0;id && i<TRANSFER_RECORDS;++i)if(!strcmp(value->records[i].id,id) && !strcmp(value->records[i].actor,actor) && value->records[i].upload==upload){r=&value->records[i];break;}
    if(!r){sb_fail(&error,"Unknown transfer for this caller, direction and running backend.");goto unlock;}
    if(!strcmp(action,"status")){out=metadata(doc,r);*failed=false;goto unlock;}
    if(!strcmp(action,"cancel")){
        if(r->state!=COMPLETE || !r->upload){release(r);r->state=CANCELLED;}
        free(r->last);r->last=NULL;
        if(*r->ticket){char *reply=sb_policy_caller_request(host->policy,actor,"cancel",r->ticket);free(reply);}
        out=metadata(doc,r);*failed=false;goto unlock;
    }
    if(upload && !strcmp(action,"commit") && r->state==COMPLETE){out=metadata(doc,r);*failed=false;goto unlock;}
    if(r->state!=ACTIVE && !(r->state==COMPLETE && !upload && !strcmp(action,"read"))){sb_fail(&error,"Transfer is cancelled, expired, failed or completed.");goto unlock;}
    if(sb_platform_millis()>=r->deadline || (*r->ticket && !sb_policy_transfer_valid(host->policy,r->ticket,actor))){release(r);r->state=EXPIRED_TRANSFER;sb_fail(&error,"Transfer permit expired or was revoked.");goto unlock;}
    uint64_t offset;size_t count=0;
    /* 分块缓冲只在传输数据时分配，避免扩大分块后耗尽线程栈。 */
    bytes=malloc(SB_TRANSFER_CHUNK);if(!bytes){sb_fail(&error,"Cannot allocate transfer chunk.");goto unlock;}
    if(!upload && !strcmp(action,"read")) {
        uint64_t max;char current[65];
        if(!number(args,"offset",0,r->size,&offset) || !number(args,"max_bytes",SB_TRANSFER_DEFAULT_CHUNK,SB_TRANSFER_CHUNK,&max) || !max){sb_fail(&error,"Invalid download offset or chunk size.");goto unlock;}
        if(offset==r->last_offset){if(max!=r->last_budget){sb_fail(&error,"Retry chunk size changed.");goto unlock;}count=r->last_size;memcpy(bytes,r->last,count);}
        else {
            if(r->state!=ACTIVE || offset!=r->next){sb_fail(&error,"Download must continue at next_offset; only the last chunk may be retried.");goto unlock;}
            if(!sb_platform_file_version(r->file,current,&error) || strcmp(current,r->version) || !sb_platform_read(r->file,offset,bytes,(size_t)max,&count,&error))goto unlock;
            if(!r->last)r->last=malloc(SB_TRANSFER_CHUNK);if(!r->last){sb_fail(&error,"Chunk receipt allocation failed.");goto unlock;}
            memcpy(r->last,bytes,count);r->last_offset=offset;r->last_size=count;r->last_budget=max;r->next+=count;
            if(r->next==r->size){release(r);r->state=COMPLETE;}
        }
        char *encoded=sb_base64(bytes,count),hash[65];if(!encoded || !sb_platform_sha256(bytes,count,hash,&error)){free(encoded);goto unlock;}
        out=metadata(doc,r);yyjson_mut_obj_add_strcpy(doc,out,"data_base64",encoded);free(encoded);
        yyjson_mut_obj_add_strcpy(doc,out,"chunk_sha256",hash);yyjson_mut_obj_add_uint(doc,out,"offset",offset);yyjson_mut_obj_add_uint(doc,out,"bytes",count);
        yyjson_mut_obj_add_bool(doc,out,"has_more",r->next<r->size);*failed=false;
    } else if(upload && !strcmp(action,"write")) {
        if(!number(args,"offset",0,r->size,&offset) || !decode(sb_string(yyjson_obj_get(args,"data_base64")),bytes,&count) || !count || count>r->size-offset){sb_fail(&error,"Invalid upload offset, canonical Base64 or chunk size.");goto unlock;}
        if(offset==r->last_offset){if(count!=r->last_size || memcmp(bytes,r->last,count)){sb_fail(&error,"Retry chunk differs from its original bytes.");goto unlock;}}
        else {
            if(offset!=r->next){sb_fail(&error,"Upload must append at next_offset; only the last chunk may be retried.");goto unlock;}
            if(!r->last)r->last=malloc(SB_TRANSFER_CHUNK);if(!r->last){sb_fail(&error,"Chunk receipt allocation failed.");goto unlock;}
            if(!sb_platform_spool_append(r->file,bytes,count,&error)){release(r);r->state=FAILED_TRANSFER;goto unlock;}
            memcpy(r->last,bytes,count);r->last_offset=offset;r->last_size=count;r->next+=count;
        }
        out=metadata(doc,r);*failed=false;
    } else if(upload && !strcmp(action,"commit")) {
        char actual[65],version[65];
        if(r->next!=r->size){sb_fail(&error,"Upload is incomplete; destination was not created.");goto unlock;}
        if(!sb_platform_file_sha256(r->file,actual,&error) || strcmp(actual,r->hash)){release(r);r->state=FAILED_TRANSFER;sb_fail(&error,"Complete upload SHA-256 mismatch; destination was not created.");goto unlock;}
        if(!sb_platform_create_version(r->destination,version,&error) || strcmp(version,r->version) || !sb_platform_create_file(r->destination,r->file,&error)){release(r);r->state=FAILED_TRANSFER;goto unlock;}
        release(r);r->state=COMPLETE;out=metadata(doc,r);*failed=false;
    } else sb_fail(&error,"Action is unsupported for this transfer direction.");
unlock:
    free(bytes);sb_platform_mutex_unlock(value->mutex);
finish:
    if(!out){out=yyjson_mut_obj(doc);yyjson_mut_obj_add_strcpy(doc,out,"error",*error.text?error.text:"Transfer failed; no content returned.");}
    if(host->project){yyjson_mut_obj_add_str(doc,out,"project_id",host->project->id);yyjson_mut_obj_add_uint(doc,out,"policy_revision",host->project->policy_revision);}
    const char *instance=sb_execution_instance(host->execution);if(instance)yyjson_mut_obj_add_str(doc,out,"host_instance_id",instance);
    return out;
}
