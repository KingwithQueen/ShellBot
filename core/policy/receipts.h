/* 仅供 approval.c 使用。完整回执可回收；有副作用的请求键只压缩，不遗忘或自动重放。 */
static bool receipt_reclaimable(const record *r){
    if(!*r->id || r->status==RUNNING || r->status==PENDING || r->status==APPROVED)return false;
    if(r->logical && sb_transfer_tool(r->tool) && r->status==COMPLETED && sb_platform_millis()<r->deadline)return false;
    return true;
}
static unsigned receipt_domain(const char *tool){return sb_desktop_tool(tool)?1u:sb_file_manage_tool(tool)?2u:0u;}
static bool receipt_hash(const char *text,char out[65]){sb_error error={0};return sb_platform_sha256(text?text:"",text?strlen(text):0,out,&error);}
static bool receipt_key(const char *parameters,char out[65]){
    *out=0;if(!parameters)return true;yyjson_doc *doc=yyjson_read(parameters,strlen(parameters),0);const char *key=sb_string(yyjson_obj_get(yyjson_doc_get_root(doc),"request_key"));bool ok=!key || receipt_hash(key,out);yyjson_doc_free(doc);return ok;
}
static yyjson_mut_val *receipt_archived(yyjson_mut_doc *doc,const replay_guard *g,bool conflict){
    yyjson_mut_val *r=error_data(doc,conflict?"request_key already belongs to different arguments, tool or target.":"The completed operation receipt was reclaimed. No operation was repeated. Inspect the original outcome before submitting a new action; observations may use a new request_key.");
    yyjson_mut_obj_add_str(doc,r,"code",conflict?"request_key_conflict":"operation_receipt_reclaimed");yyjson_mut_obj_add_str(doc,r,"status","archived");yyjson_mut_obj_add_strcpy(doc,r,"approval_id",g->id);yyjson_mut_obj_add_strcpy(doc,r,"tool",g->tool);yyjson_mut_obj_add_str(doc,r,"original_status",state_name(g->status));
    yyjson_mut_obj_add_bool(doc,r,"retryable",false);yyjson_mut_obj_add_bool(doc,r,"automatic_replay",false);yyjson_mut_obj_add_bool(doc,r,"receipt_available",false);return r;
}
static yyjson_mut_val *receipt_retired(sb_policy *p,const char *actor,const char *tool,const char *digest,const char *target,const char *parameters,const char *id,yyjson_mut_doc *doc){
    char key[65],hash[65];if(!receipt_key(parameters,key) || !receipt_hash(target,hash))return error_data(doc,"Cannot verify reclaimed operation binding.");unsigned domain=receipt_domain(tool);
    for(size_t i=0;i<p->guard_count;++i){replay_guard *g=&p->guards[i];if(strcmp(g->actor,actor))continue;
        /* 无请求键的新读取不是旧许可重试；旧 approval_id 仍必须拒绝。 */
        if(g->reading && !id && !*key)continue;
        bool same_id=id && !strcmp(g->id,id),same_key=*key && domain && domain==g->domain && !strcmp(key,g->key),same_operation=!strcmp(g->tool,tool) && !strcmp(g->digest,digest) && (!target || !strcmp(g->target,hash));
        if(same_id || same_key || (!id && same_operation))return receipt_archived(doc,g,!same_operation);
    }return NULL;
}
static bool receipt_retire(sb_policy *p,record *r){
    replay_guard g={0};strcpy(g.id,r->id);strcpy(g.actor,r->actor);strcpy(g.tool,r->tool);strcpy(g.digest,r->digest);g.status=r->status;g.domain=receipt_domain(r->tool);g.reading=logical_read(r) && !sb_transfer_tool(r->tool);
    if(!receipt_hash(r->target,g.target) || !receipt_key(r->logical?r->text:NULL,g.key))return false;
    size_t slot=p->guard_count;
    if(slot==REPLAY_GUARD_LIMIT){
        /* 只允许淘汰无副作用读取的键摘要；本实例内的写入/输入键一直保留。 */
        for(slot=0;slot<p->guard_count && !p->guards[slot].reading;++slot){}
        if(slot==p->guard_count){if(!g.reading)return false;clear(r);++p->reclaimed;return true;}
    }else if(slot==p->guard_capacity){size_t n=p->guard_capacity?p->guard_capacity*2:256;if(n>REPLAY_GUARD_LIMIT)n=REPLAY_GUARD_LIMIT;replay_guard *guards=realloc(p->guards,n*sizeof(*guards));if(!guards)return false;p->guards=guards;p->guard_capacity=n;}
    p->guards[slot]=g;if(slot==p->guard_count)++p->guard_count;clear(r);++p->reclaimed;return true;
}
static record *receipt_oldest(sb_policy *p,const record *except){record *oldest=NULL;for(size_t i=0;i<p->record_limit;++i){record *r=&p->records[i];if(r!=except && receipt_reclaimable(r) && (!oldest || r->deadline<oldest->deadline))oldest=r;}return oldest;}
static record *receipt_reserve(sb_policy *p){
    for(size_t i=0;i<p->record_limit;++i)if(!*p->records[i].id)return &p->records[i];
    for(size_t i=0;i<p->record_limit;++i)expire(p,&p->records[i]);
    record *r=receipt_oldest(p,NULL);return r && receipt_retire(p,r)?r:NULL;
}
static uint64_t receipt_cached_bytes(sb_policy *p){uint64_t bytes=0;for(size_t i=0;i<p->record_limit;++i)if(p->records[i].cached)bytes+=strlen(p->records[i].cached);return bytes;}
static void receipt_trim_cache(sb_policy *p,size_t incoming,const record *except){
    if(incoming>RECEIPT_CACHE_BYTES)return;
    while(receipt_cached_bytes(p)>RECEIPT_CACHE_BYTES-incoming){record *r=receipt_oldest(p,except);if(!r || !receipt_retire(p,r))break;++p->cache_reclaimed;}
}
