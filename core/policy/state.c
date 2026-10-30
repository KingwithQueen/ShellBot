#include "sb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef SB_POSIX
#define STATE_BYTES (32u * 1024u * 1024u)
#define STATE_RECORDS 256
#else
#define STATE_BYTES (256u * 1024u * 1024u)
#define STATE_RECORDS 8192
#endif
#define STATE_LINE (2u * 1024u * 1024u)
typedef struct { char id[33], instance[33], actor[64], kind[16]; char *json; } saved_record;
struct sb_store {
    sb_mutex *mutex;
    sb_journal *journal;
    char project[64], instance[33], writer[33], backend[24];
    uint64_t revision, sequence, bytes;
    size_t count;
    bool healthy;
    saved_record records[STATE_RECORDS];
};
static bool hex_id(const char *id) { return id && strlen(id)==32 && strspn(id,"0123456789abcdef")==32; }
static bool active(const char *status) {
    return status && (!strcmp(status,"pending") || !strcmp(status,"approved") || !strcmp(status,"running") || !strcmp(status,"starting"));
}
static saved_record *find(sb_store *store, const char *kind, const char *id, const char *instance) {
    for (size_t i=0;i<store->count;++i) {
        saved_record *r=&store->records[i];
        if (!strcmp(r->kind,kind) && !strcmp(r->id,id) && !strcmp(r->instance,instance)) return r;
    }
    return NULL;
}
static bool cache(sb_store *store, const char *kind, const char *id, const char *actor, yyjson_mut_val *body) {
    if (strcmp(kind,"approval") && strcmp(kind,"task")) return true;
    const char *instance=yyjson_mut_get_str(yyjson_mut_obj_get(body,"state_instance_id"));
    if (!hex_id(id) || !hex_id(instance) || !actor || strlen(actor)>=64) return false;
    saved_record *r=find(store,kind,id,instance);
    if (!r && store->count==STATE_RECORDS) return false;
    char *json=yyjson_mut_val_write(body,0,NULL); if (!json) return false;
    if (!r) { r=&store->records[store->count++]; strcpy(r->id,id); strcpy(r->instance,instance); strcpy(r->kind,kind); strcpy(r->actor,actor); }
    if (strcmp(r->actor,actor)) { free(json); return false; }
    free(r->json); r->json=json; return true;
}
static bool load(void *context, const char *name, const char *bytes, size_t length, sb_error *error) {
    sb_store *store=context; uint64_t sequence=0;
    if (length>STATE_BYTES-store->bytes) { sb_fail_limit(error,"persistent_state.bytes",store->bytes+length,STATE_BYTES,"bytes"); return false; }
    store->bytes+=length;
    size_t offset=0;
    while (offset<length) {
        const char *end=memchr(bytes+offset,'\n',length-offset);
        /* A power loss may leave the final append incomplete. Preserve it and
           recover from earlier flushed records; never infer success from it. */
        if (!end) break;
        size_t line=(size_t)(end-(bytes+offset));
        yyjson_doc *input=line && line<=STATE_LINE ? yyjson_read(bytes+offset,line,0) : NULL;
        yyjson_val *root=yyjson_doc_get_root(input);
        static const char *const keys[]={"schema_version","project_id","policy_revision","writer_instance_id","backend","sequence","time_unix_ms","kind","record_id","actor","status","event","body","sha256"};
        const char *project=sb_string(yyjson_obj_get(root,"project_id")), *writer=sb_string(yyjson_obj_get(root,"writer_instance_id"));
        const char *backend=sb_string(yyjson_obj_get(root,"backend")), *kind=sb_string(yyjson_obj_get(root,"kind"));
        const char *id=sb_string(yyjson_obj_get(root,"record_id")), *actor=sb_string(yyjson_obj_get(root,"actor")), *hash=sb_string(yyjson_obj_get(root,"sha256"));
        uint64_t seq=0,format=0,revision=0;
        const char *body_instance=sb_string(yyjson_obj_get(yyjson_obj_get(root,"body"),"state_instance_id"));
        const char *host_instance=sb_string(yyjson_obj_get(yyjson_obj_get(root,"body"),"host_instance_id"));
        const char *status=sb_string(yyjson_obj_get(root,"status")), *body_status=sb_string(yyjson_obj_get(yyjson_obj_get(root,"body"),"status"));
        bool valid=sb_config_keys(root,keys,14) && sb_integer(yyjson_obj_get(root,"schema_version"),&format) && format==1 &&
            project && !strcmp(project,store->project) && backend && !strcmp(backend,store->backend) &&
            hex_id(writer) && !strncmp(name+6,writer,32) && sb_integer(yyjson_obj_get(root,"policy_revision"),&revision) && revision &&
            sb_integer(yyjson_obj_get(root,"sequence"),&seq) && seq==sequence+1 && kind && id && actor && hash && strlen(hash)==64 &&
            hex_id(id) && (!strcmp(kind,"approval") || !strcmp(kind,"task") || !strcmp(kind,"recovery")) &&
            body_instance && !strcmp(body_instance,writer) && hex_id(host_instance) && status && body_status && !strcmp(status,body_status) &&
            sb_string(yyjson_obj_get(root,"event")) && yyjson_is_obj(yyjson_obj_get(root,"body"));
        yyjson_mut_doc *doc=valid ? yyjson_doc_mut_copy(input,NULL) : NULL;
        yyjson_mut_val *copy=yyjson_mut_doc_get_root(doc); char actual[65]={0},*encoded=NULL;
        if (doc) { yyjson_mut_obj_remove_str(copy,"sha256"); encoded=yyjson_mut_write(doc,0,NULL); }
        valid=valid && encoded && sb_platform_sha256(encoded,strlen(encoded),actual,error) && !strcmp(actual,hash);
        yyjson_mut_val *body=yyjson_mut_obj_get(copy,"body");
        if (valid) valid=cache(store,kind,id,actor,body);
        free(encoded); yyjson_mut_doc_free(doc); yyjson_doc_free(input);
        if (!valid) { sb_fail(error,"Persistent state is corrupt, foreign, ambiguous or over its record budget; no operation was recovered."); return false; }
        sequence=seq; offset+=line+1;
    }
    return true;
}
bool sb_store_append(sb_store *store, const char *kind, const char *id, const char *actor,
                     const char *status, const char *event, yyjson_mut_val *body) {
    if (!store) return true; /* Legacy single-host operation is unchanged. */
    sb_platform_mutex_lock(store->mutex);
    yyjson_mut_doc *doc=store->healthy ? yyjson_mut_doc_new(NULL) : NULL;
    yyjson_mut_val *root=doc ? yyjson_mut_obj(doc) : NULL, *copy=doc ? yyjson_mut_val_mut_copy(doc,body) : NULL;
    bool ok=root && copy;
    if (ok) {
        yyjson_mut_doc_set_root(doc,root);
        if (!yyjson_mut_obj_get(copy,"host_instance_id")) yyjson_mut_obj_add_str(doc,copy,"host_instance_id",store->instance);
        yyjson_mut_obj_remove_str(copy,"state_instance_id"); yyjson_mut_obj_add_str(doc,copy,"state_instance_id",store->writer);
        yyjson_mut_obj_remove_str(copy,"status"); yyjson_mut_obj_add_str(doc,copy,"status",status);
        yyjson_mut_obj_remove_str(copy,"last_event"); yyjson_mut_obj_add_str(doc,copy,"last_event",event);
        yyjson_mut_obj_remove_str(copy,"updated_at_unix_ms"); yyjson_mut_obj_add_uint(doc,copy,"updated_at_unix_ms",sb_platform_unix_ms());
        yyjson_mut_obj_add_uint(doc,root,"schema_version",1); yyjson_mut_obj_add_str(doc,root,"project_id",store->project);
        yyjson_mut_obj_add_uint(doc,root,"policy_revision",store->revision); yyjson_mut_obj_add_str(doc,root,"writer_instance_id",store->writer);
        yyjson_mut_obj_add_str(doc,root,"backend",store->backend); yyjson_mut_obj_add_uint(doc,root,"sequence",store->sequence+1);
        yyjson_mut_obj_add_uint(doc,root,"time_unix_ms",sb_platform_unix_ms()); yyjson_mut_obj_add_str(doc,root,"kind",kind);
        yyjson_mut_obj_add_str(doc,root,"record_id",id); yyjson_mut_obj_add_str(doc,root,"actor",actor);
        yyjson_mut_obj_add_str(doc,root,"status",status); yyjson_mut_obj_add_str(doc,root,"event",event); yyjson_mut_obj_add_val(doc,root,"body",copy);
        if ((!strcmp(kind,"approval") || !strcmp(kind,"task")) && !find(store,kind,id,store->writer) && store->count==STATE_RECORDS) ok=false;
    }
    sb_error error={0}; char hash[65], *encoded=ok ? yyjson_mut_write(doc,0,NULL) : NULL;
    ok=encoded && sb_platform_sha256(encoded,strlen(encoded),hash,&error); free(encoded);
    if (ok) yyjson_mut_obj_add_str(doc,root,"sha256",hash);
    size_t length=0; char *json=ok ? yyjson_mut_write(doc,0,&length) : NULL, *line=json ? malloc(length+1) : NULL;
    ok=ok && line && length<=STATE_LINE && length+1<=STATE_BYTES-store->bytes;
    if (ok) {
        memcpy(line,json,length); line[length]='\n';
        ok=sb_platform_journal_append(store->journal,line,length+1,&error);
        if (ok) { store->bytes+=length+1; ++store->sequence; ok=cache(store,kind,id,actor,copy); }
    }
    if (!ok) store->healthy=false;
    free(line); free(json); yyjson_mut_doc_free(doc); sb_platform_mutex_unlock(store->mutex); return ok;
}
sb_store *sb_store_new(sb_lease *lease, const char *project, uint64_t revision,
                       const char *instance, const char *backend, sb_error *error) {
    sb_store *store=calloc(1,sizeof(*store));
    if (!store || !sb_identifier(project) || !hex_id(instance) || !revision || !backend || strlen(backend)>=24) goto failed;
    store->mutex=sb_platform_mutex_new(); strcpy(store->project,project); strcpy(store->instance,instance); strcpy(store->backend,backend); store->revision=revision;
    unsigned char nonce[16];
    if (!sb_platform_random(nonce,sizeof(nonce),error)) goto failed;
    for (size_t i=0;i<sizeof(nonce);++i) snprintf(store->writer+i*2,3,"%02x",nonce[i]);
    if (!store->mutex || !sb_platform_state_visit(lease,load,store,error)) goto failed;
    char name[64]; snprintf(name,sizeof(name),"state-%s.jsonl",store->writer);
    store->journal=sb_platform_journal_open(lease,name,error); store->healthy=store->journal!=NULL;
    yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL); yyjson_mut_val *body=doc ? yyjson_mut_obj(doc) : NULL;
    yyjson_mut_val *interrupted=doc ? yyjson_mut_arr(doc) : NULL;
    if (body) {
        for (size_t i=0;i<store->count;++i) {
            saved_record *r=&store->records[i]; yyjson_doc *saved=yyjson_read(r->json,strlen(r->json),0);
            const char *status=sb_string(yyjson_obj_get(yyjson_doc_get_root(saved),"status"));
            if (active(status)) {
                yyjson_mut_val *item=yyjson_mut_obj(doc); yyjson_mut_obj_add_str(doc,item,"kind",r->kind); yyjson_mut_obj_add_str(doc,item,"id",r->id);
                yyjson_mut_obj_add_str(doc,item,"state_instance_id",r->instance);
                yyjson_mut_obj_add_strcpy(doc,item,"host_instance_id",sb_string(yyjson_obj_get(yyjson_doc_get_root(saved),"host_instance_id")));
                yyjson_mut_obj_add_strcpy(doc,item,"previous_status",status); yyjson_mut_arr_add_val(interrupted,item);
            }
            yyjson_doc_free(saved);
        }
        yyjson_mut_obj_add_val(doc,body,"interrupted",interrupted); yyjson_mut_obj_add_bool(doc,body,"automatic_replay",false);
    }
    bool ok=body && sb_store_append(store,"recovery",store->writer,"host","interrupted","instance_recovered",body);
    yyjson_mut_doc_free(doc); if (!ok) goto failed;
    return store;
failed:
    sb_store_free(store); if (!*error->text) sb_fail(error,"Cannot initialize protected persistent state."); return NULL;
}
bool sb_store_healthy(sb_store *store) {
    if (!store) return true;
    sb_platform_mutex_lock(store->mutex); bool ok=store->healthy; sb_platform_mutex_unlock(store->mutex); return ok;
}
yyjson_mut_val *sb_store_capacity(sb_store *store,yyjson_mut_doc *doc) {
    yyjson_mut_val *root=yyjson_mut_obj(doc);yyjson_mut_obj_add_bool(doc,root,"enabled",store!=NULL);if(!store)return root;
    sb_platform_mutex_lock(store->mutex);
    yyjson_mut_obj_add_bool(doc,root,"healthy",store->healthy);yyjson_mut_obj_add_uint(doc,root,"bytes",store->bytes);yyjson_mut_obj_add_uint(doc,root,"bytes_limit",STATE_BYTES);
    yyjson_mut_obj_add_uint(doc,root,"record_count",store->count);yyjson_mut_obj_add_uint(doc,root,"record_limit",STATE_RECORDS);
    sb_platform_mutex_unlock(store->mutex);return root;
}
bool sb_store_task_repeated(sb_store *store, const char *actor, const char *key) {
    if (!store || !actor || !key) return false;
    bool found=false; sb_platform_mutex_lock(store->mutex);
    for (size_t i=0;i<store->count && !found;++i) {
        saved_record *r=&store->records[i];
        if (strcmp(r->kind,"task") || strcmp(r->actor,actor) || !strcmp(r->instance,store->writer)) continue;
        yyjson_doc *doc=yyjson_read(r->json,strlen(r->json),0);
        const char *old=sb_string(yyjson_obj_get(yyjson_doc_get_root(doc),"request_key"));
        found=old && !strcmp(old,key); yyjson_doc_free(doc);
    }
    sb_platform_mutex_unlock(store->mutex); return found;
}
char *sb_store_history(sb_store *store, const char *actor, const char *id, bool full, unsigned offset, unsigned limit) {
    if (!store) return sb_strdup("{\"records\":[],\"durable\":false}");
    sb_platform_mutex_lock(store->mutex); yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root=doc ? yyjson_mut_obj(doc) : NULL, *items=doc ? yyjson_mut_arr(doc) : NULL;
    unsigned matched=0,returned=0; bool more=false;
    for (size_t i=0;doc && i<store->count;++i) {
        saved_record *r=&store->records[i]; if ((actor && strcmp(r->actor,actor)) || (id && strcmp(r->id,id))) continue;
        if (matched++<offset) continue; if (returned==limit) { more=true; break; }
        yyjson_doc *saved=yyjson_read(r->json,strlen(r->json),0); yyjson_mut_val *body=yyjson_val_mut_copy(doc,yyjson_doc_get_root(saved));
        if (body) {
            bool current=!strcmp(r->instance,store->writer); const char *status=yyjson_mut_get_str(yyjson_mut_obj_get(body,"status"));
            if (!current && active(status)) { yyjson_mut_obj_add_strcpy(doc,body,"previous_status",status); yyjson_mut_obj_remove_str(body,"status"); yyjson_mut_obj_add_str(doc,body,"status","interrupted");
                yyjson_mut_obj_remove_str(body,"expires_in_ms"); yyjson_mut_obj_add_uint(doc,body,"expires_in_ms",0); }
            yyjson_mut_obj_add_bool(doc,body,"active_instance",current); yyjson_mut_obj_add_str(doc,body,"record_kind",r->kind);
            yyjson_mut_obj_add_str(doc,body,"backend",store->backend); yyjson_mut_obj_add_str(doc,body,"project_id",store->project);
            if (!yyjson_mut_obj_get(body,"actor")) yyjson_mut_obj_add_str(doc,body,"actor",r->actor);
            if (!full) {
                static const char *const private_keys[]={"text","original_text","receipt","stdout_base64","stderr_base64","argv","executable","cwd","parent_identity","file_identity"};
                for (size_t j=0;j<sizeof(private_keys)/sizeof(private_keys[0]);++j) yyjson_mut_obj_remove_str(body,private_keys[j]);
            }
            yyjson_mut_arr_add_val(items,body); ++returned;
        }
        yyjson_doc_free(saved);
    }
    if (doc) { yyjson_mut_doc_set_root(doc,root); yyjson_mut_obj_add_val(doc,root,"records",items); yyjson_mut_obj_add_bool(doc,root,"durable",true);
        yyjson_mut_obj_add_bool(doc,root,"healthy",store->healthy); yyjson_mut_obj_add_uint(doc,root,"next_offset",offset+returned); yyjson_mut_obj_add_bool(doc,root,"has_more",more); }
    char *json=doc ? yyjson_mut_write(doc,0,NULL) : NULL; yyjson_mut_doc_free(doc); sb_platform_mutex_unlock(store->mutex); return json;
}
void sb_store_free(sb_store *store) {
    if (!store) return;
    sb_platform_journal_close(store->journal); for (size_t i=0;i<store->count;++i) free(store->records[i].json);
    sb_platform_mutex_free(store->mutex); free(store);
}
