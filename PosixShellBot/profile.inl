static bool profile_name(const char *s) {
    return s && *s && strlen(s) < 96 && !strchr(s, '/') && !strstr(s, "..") && strspn(s, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.") == strlen(s);
}
static bool follow_location(const char *initial, char *out, size_t capacity, sb_error *e) {
    char *visited[8]={0};bool ok=false;
    if(!sb_posix_socket_preflight(initial,e)||!native_path(out,capacity,e,"%s",initial))return false;
    for(unsigned hop=0;hop<8;hop++){
        for(unsigned i=0;i<hop;i++)if(!strcmp(visited[i],out)){sb_fail(e,"数据位置引用出现循环。");goto done;}
        visited[hop]=sb_strdup(out);if(!visited[hop]){sb_fail(e,"Out of memory.");goto done;}
        sb_root *r=sb_platform_root(out,e);if(!r||!sb_posix_private(r,e)){sb_platform_root_close(r);goto done;}
        struct stat st;int found=fstatat(r->fd,"relocation.json",&st,AT_SYMLINK_NOFOLLOW),found_error=errno;sb_platform_root_close(r);
        if(found){ok=found_error==ENOENT;goto done;}
        char path[SB_PATH_CAP];if(!native_path(path,sizeof(path),e,"%s/relocation.json",out))goto done;
        yyjson_doc *d=parse(load(path));yyjson_val *v=yyjson_doc_get_root(d);const char *next=sb_gui_string(v,"target");
        bool valid=d&&yyjson_obj_size(v)==1&&S_ISREG(st.st_mode)&&st.st_nlink==1&&st.st_uid==geteuid()&&!(st.st_mode&077)&&
            sb_project_path_valid(next)&&strlen(next)<capacity;
        if(valid)valid=sb_posix_socket_preflight(next,e)&&native_path(out,capacity,e,"%s",next);yyjson_doc_free(d);
        if(!valid){if(!*e->text)sb_fail(e,"数据位置引用不可用；原文件保留，没有创建空配置。");goto done;}
    }
    sb_fail(e,"数据位置引用过多，请直接打开最新数据目录。");
done:
    for(unsigned i=0;i<8;i++)free(visited[i]);return ok;
}
static bool active_profile(sb_native_manager *g, sb_error *e) {
    char path[SB_PATH_CAP];if(!native_path(path,sizeof(path),e,"%s/active.json",g->directory))return false;
    struct stat st;
    if (lstat(path, &st)) return errno == ENOENT;
    yyjson_doc *d = parse(load(path)); yyjson_val *v = yyjson_doc_get_root(d);
    const char *r = sb_gui_string(v, "registry"), *m = sb_gui_string(v, "manager");
    bool ok = d && S_ISREG(st.st_mode) && st.st_nlink == 1 && st.st_uid == geteuid() && !(st.st_mode & 077) &&
        yyjson_obj_size(v) == 2 && profile_name(r) && profile_name(m);
    if (ok) {
        char registry[SB_PATH_CAP],data[SB_PATH_CAP];
        ok=native_path(registry,sizeof(registry),e,"%s/%s",g->directory,r)&&
            native_path(data,sizeof(data),e,"%s/%s",g->directory,m)&&sb_posix_socket_preflight(data,e);
        if(ok){memcpy(g->registry,registry,strlen(registry)+1);memcpy(g->data,data,strlen(data)+1);}
    }
    else sb_fail(e,"活动配置位置损坏；没有创建替代数据。");
    yyjson_doc_free(d); return ok;
}
static bool configure_project(sb_native_manager *g, yyjson_val *q, sb_error *e) {
    if (!stopped(g,e)) return false;
    char *old = load(g->registry), hash[65];
    if (!old || !sb_platform_sha256(old,strlen(old),hash,e)) { free(old); return false; }
    if (strcmp(hash,sb_gui_string(q,"expected_registry_sha256"))) { free(old); sb_fail(e,"配置已变化，请重新打开项目设置。"); return false; }
    yyjson_doc *source = yyjson_read(old,strlen(old),0);
    yyjson_val *definition = yyjson_obj_get(q,"definition");
    const char *id = sb_gui_string(definition,"id"), *original = sb_gui_string(q,"original_id");
    yyjson_val *existing = find(source,"projects","id",original);
    bool ok = true;
    if (*original && (!existing || strcmp(id,original) ||
        strcmp(sb_gui_string(existing,"data_dir"),sb_gui_string(definition,"data_dir")) ||
        strcmp(sb_gui_string(existing,"token_env"),sb_gui_string(definition,"token_env")))) {
        sb_fail(e,"已有项目的地址标识和凭据引用保持固定；变更数据目录请使用迁移入口。"); ok = false;
    }
    yyjson_mut_doc *copy = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *p = yyjson_val_mut_copy(copy,definition);
    sb_form_normalize(copy,p);
    char *json = sb_posix_json(copy,p);
    char *next = ok && json ? sb_projects_edit(old,strlen(old),json,strlen(json),id,*original!=0,e) : NULL;
    ok = next && save_registry(g,next,e);
    if (ok) {
        yyjson_doc *s = settings(g); yyjson_mut_doc *d = yyjson_doc_mut_copy(s,NULL);
        yyjson_mut_val *root = yyjson_mut_doc_get_root(d), *opts = yyjson_mut_obj_get(root,"project_options");
        if (!opts) { opts = yyjson_mut_obj(d); yyjson_mut_obj_add_val(d,root,"project_options",opts); }
        yyjson_mut_val *item = yyjson_mut_obj(d);
        yyjson_mut_obj_add_bool(d,item,"auto_start",sb_gui_bool(q,"auto_start"));
        yyjson_mut_obj_add_strcpy(d,item,"display_name",sb_gui_string(q,"display_name"));
        yyjson_mut_obj_remove_key(opts,id); yyjson_mut_obj_add(opts,yyjson_mut_strcpy(d,id),item);
        char *text = sb_posix_json(d,root); ok = text && save(g,"desktop.json",text,e);
        free(text); yyjson_doc_free(s);
        snprintf(g->selected,sizeof(g->selected),"%s",id);
    }
    free(json); free(next); free(old); yyjson_doc_free(source); return ok;
}
static bool owner_credential(sb_native_manager *g, sb_error *e) {
    if(online(g)){
        yyjson_doc *d=exchange(g,"owner-credential",NULL,NULL);
        const char *token=sb_gui_string(yyjson_doc_get_root(d),"token");
        if(*token)g->clipboard=sb_strdup(token);
        yyjson_doc_free(d);
        if(!g->clipboard)sb_fail(e,"远程所有者审批未启用。");return g->clipboard!=NULL;
    }
    if (!stopped(g,e)) return false;
    sb_lease *lease = sb_platform_lease(g->data,e);
    sb_secret_options options = {.key_file=*g->key?g->key:NULL};
    sb_secrets *s = lease ? sb_secrets_open(lease,&options,e) : NULL;
    yyjson_doc *cfg = registry(g);
    const char *env = sb_gui_string(yyjson_obj_get(yyjson_doc_get_root(cfg),"gateway"),"owner_token_env");
    char *bearer = *env && s ? sb_owner_bearer(s,env,true,e) : NULL;
    if (bearer) { g->clipboard=sb_strdup(bearer+7); sb_posix_wipe(bearer,strlen(bearer)); free(bearer); }
    else if (!*e->text) sb_fail(e,"请先在连接设置中启用远程所有者审批。");
    yyjson_doc_free(cfg); sb_secrets_close(s); sb_platform_lease_close(lease); return g->clipboard!=NULL;
}
static bool relocate_path(yyjson_mut_doc *d, yyjson_mut_val *object, const char *key, const char *from, const char *to, sb_error *e) {
    const char *path = yyjson_mut_get_str(yyjson_mut_obj_get(object,key)); size_t n = strlen(from);
    if (path && !strncmp(path,from,n) && (!path[n] || path[n]=='/')) {
        char result[SB_PATH_CAP];if(!native_path(result,sizeof(result),e,"%s%s",to,path+n))return false;
        yyjson_mut_obj_remove_key(object,key); yyjson_mut_obj_add_strcpy(d,object,key,result);
    }
    return true;
}
static size_t migration_bindings(sb_project_registry *r, sb_secrets *s, sb_oauth_project out[SB_MAX_PROJECTS]) {
    for (size_t i=0;i<r->project_count;i++) {
        sb_project *p=&r->projects[i]; out[i]=(sb_oauth_project){.id=p->id,.revision=p->policy_revision};
        sb_error e={0}; char identity[50], binding[256]; sb_root *root=sb_platform_root(p->data_dir,&e);
        char *bearer=root?sb_credential_bearer(s,p->id,p->token_env,&e):NULL;
        if (bearer && sb_platform_root_identity(root,identity,&e)) {
            snprintf(binding,sizeof(binding),"%s:%s:%s",bearer+7,p->id,identity);
            out[i].enabled=sb_platform_sha256(binding,strlen(binding),out[i].binding,&e);
            sb_posix_wipe(binding,sizeof(binding));
        }
        if(bearer){sb_posix_wipe(bearer,strlen(bearer));free(bearer);} sb_platform_root_close(root);
    }
    return r->project_count;
}
static bool migration_guard(sb_lease *lease,const char *text,struct stat *identity,sb_error *e){
    int fd=openat(lease->root->fd,"migration-guard.json",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0){sb_fail(e,"原位置已有迁移保护记录；请先核对上次迁移，不覆盖它。");return false;}
    bool ok=!fstat(fd,identity)&&sb_posix_write(fd,text,strlen(text),e)&&!fsync(fd)&&!fsync(lease->root->fd);close(fd);return ok;
}
static bool publication_matches(sb_root *root,const char *name,const char *text){
    char path[SB_PATH_CAP];if(!native_path(path,sizeof(path),NULL,"%s/%s",root->path,name))return false;char *actual=load(path);
    bool same=actual&&!strcmp(actual,text);free(actual);return same;
}
static yyjson_doc *pending_migration(sb_native_manager *g){
    char path[SB_PATH_CAP];if(!native_path(path,sizeof(path),NULL,"%s/migration-guard.json",g->data))return NULL;return parse(load(path));
}
static bool recover_migration(sb_native_manager *g,yyjson_val *q,sb_error *e){
    if(!stopped(g,e))return false;
    char active_path[SB_PATH_CAP];if(!native_path(active_path,sizeof(active_path),e,"%s/relocation.json",g->directory))return false;
    struct stat st;if(!lstat(active_path,&st)){sb_fail(e,"此位置已发布迁移，请关闭窗口后从原入口重新打开。");return false;}
    yyjson_doc *pending=pending_migration(g);yyjson_val *v=yyjson_doc_get_root(pending);
    const char *id=sb_gui_string(v,"migration_id");
    bool valid=strlen(id)==32&&strspn(id,"0123456789abcdef")==32&&!strcmp(id,sb_gui_string(q,"migration_id"))&&
        strcmp(g->data,sb_gui_string(v,"manager"))&&strcmp(g->registry,sb_gui_string(v,"registry"));
    if(!valid){yyjson_doc_free(pending);sb_fail(e,"没有匹配的未完成迁移；请刷新并核对记录。");return false;}
    if(!native_path(active_path,sizeof(active_path),e,"%s/active.json",g->directory)){yyjson_doc_free(pending);return false;}yyjson_doc *active=parse(load(active_path));
    if(active&&strcmp(sb_gui_string(yyjson_doc_get_root(active),"manager"),strrchr(g->data,'/')+1)){
        yyjson_doc_free(active);yyjson_doc_free(pending);sb_fail(e,"活动配置已切换，请重新打开管理窗口。");return false;
    }yyjson_doc_free(active);
    sb_project_registry r={0};sb_lease *leases[SB_MAX_PROJECTS+1]={0};bool ok=sb_projects_load(&r,g->registry,e);
    if(ok){leases[0]=sb_platform_lease(g->data,e);ok=leases[0]!=NULL;}
    for(size_t i=0;ok&&i<r.project_count;i++){
        if(lstat(r.projects[i].data_dir,&st)){if(errno==ENOENT)continue;ok=false;break;}
        leases[i+1]=sb_platform_lease(r.projects[i].data_dir,e);ok=leases[i+1]!=NULL;
    }
    struct stat identities[SB_MAX_PROJECTS+1]={0};
    for(size_t i=0;ok&&i<=r.project_count;i++)if(leases[i]){
        if(fstatat(leases[i]->root->fd,"migration-guard.json",&identities[i],AT_SYMLINK_NOFOLLOW)){
            if(i && errno==ENOENT){identities[i].st_ino=0;continue;}ok=false;break;
        }
        struct stat *identity=&identities[i];
        char path[SB_PATH_CAP];if(!native_path(path,sizeof(path),e,"%s/migration-guard.json",leases[i]->root->path)){ok=false;break;}
        yyjson_doc *record=parse(load(path));
        ok=record&&S_ISREG(identity->st_mode)&&identity->st_nlink==1&&identity->st_uid==geteuid()&&!(identity->st_mode&077)&&
            !strcmp(sb_gui_string(yyjson_doc_get_root(record),"migration_id"),id);
        yyjson_doc_free(record);
    }
    for(size_t j=1;ok&&j<=r.project_count+1;j++){
        size_t i=j<=r.project_count?j:0;
        if(identities[i].st_ino){
            struct stat actual;
            ok=!fstatat(leases[i]->root->fd,"migration-guard.json",&actual,AT_SYMLINK_NOFOLLOW)&&actual.st_dev==identities[i].st_dev&&actual.st_ino==identities[i].st_ino&&
                !unlinkat(leases[i]->root->fd,"migration-guard.json",0)&&!fsync(leases[i]->root->fd);
        }
    }
    if(!ok&&!*e->text)sb_fail(e,"迁移保护记录不匹配、已变化或无法同步，未恢复启动。");
    for(size_t i=0;i<=SB_MAX_PROJECTS;i++)sb_platform_lease_close(leases[i]);
    sb_projects_free(&r);yyjson_doc_free(pending);return ok;
}
static bool migrate_profile(sb_native_manager *g, yyjson_val *q, bool application, sb_error *e) {
    if (!stopped(g,e)) return false;
    const char *destination=sb_gui_string(q,"path");
    if(!sb_posix_socket_preflight(destination,e))return false;
    sb_error startup_error={0}; yyjson_doc *autostart=startup(g,"--startup-status",&startup_error);
    bool installed=sb_gui_bool(yyjson_doc_get_root(autostart),"installed"); yyjson_doc_free(autostart);
    if(application && (installed || *startup_error.text)){sb_fail(e,"迁移应用数据前请先取消登录自启；迁移后可在新位置重新启用。");return false;}
    sb_project_registry before={0},after={0}; char *original=load(g->registry),*next=NULL;
    yyjson_doc *cfg=original?yyjson_read(original,strlen(original),0):NULL;
    sb_lease *manager_lease=NULL,*project_leases[SB_MAX_PROJECTS]={0},*target_lease=NULL,*new_manager_lease=NULL;
    sb_root *target=NULL; sb_secrets *old_secrets=NULL,*new_secrets=NULL; bool ok=false,committed=false,durable=true;
    struct stat guards[SB_MAX_PROJECTS+1]={0};
    yyjson_mut_doc *d=NULL; char new_manager[SB_PATH_CAP],new_registry[SB_PATH_CAP],nonce[33];
    unsigned char random[16]; sb_migration state={.cancel=&g->cancel};
    if(!original||!sb_projects_parse(&before,original,strlen(original),e))goto done;
    const sb_project *selected=sb_project_find(&before,g->selected);
    if(!application&&!selected){sb_fail(e,"请先选择需要迁移的项目。");goto done;}
    manager_lease=sb_platform_lease(g->data,e); if(!manager_lease||!sb_migration_can_start(manager_lease,e))goto done;
    if(application&&*g->key){
        char *leaf=NULL;sb_root *r=sb_platform_account_parent(g->key,&leaf,e);free(leaf);
        bool overlap=!r||sb_platform_roots_overlap(r,g->lease->root);sb_platform_root_close(r);
        if(overlap){sb_fail(e,"应用迁移要求独立密钥文件位于应用数据目录之外；请用原密钥从独立位置重新打开。");goto done;}
    }
    for(size_t i=0;i<before.project_count;i++){
        struct stat st;
        if(lstat(before.projects[i].data_dir,&st)){if(errno==ENOENT)continue;goto done;}
        project_leases[i]=sb_platform_lease(before.projects[i].data_dir,e);if(!project_leases[i]||!sb_migration_can_start(project_leases[i],e))goto done;
    }
    d=yyjson_doc_mut_copy(cfg,NULL);
    if(!d){sb_fail(e,"Out of memory.");goto done;}
    yyjson_mut_val *root=yyjson_mut_doc_get_root(d),*projects=yyjson_mut_obj_get(root,"projects");
    size_t i,n;yyjson_mut_val *p;
    yyjson_mut_arr_foreach(projects,i,n,p){
        if(application){
            if(!relocate_path(d,p,"data_dir",g->directory,destination,e))goto done;
            const char *arrays[]={"workspaces","extra_directories"};
            for(unsigned a=0;a<2;a++){size_t j,m;yyjson_mut_val *item;yyjson_mut_arr_foreach(yyjson_mut_obj_get(p,arrays[a]),j,m,item)if(!relocate_path(d,item,"path",g->directory,destination,e))goto done;}
            yyjson_mut_val *execution=yyjson_mut_obj_get(yyjson_mut_obj_get(p,"policy"),"execution"),*runtime=yyjson_mut_obj_get(execution,"runtime_files");
            for(size_t j=0;j<yyjson_mut_arr_size(runtime);j++){
                const char *path=yyjson_mut_get_str(yyjson_mut_arr_get(runtime,j));size_t prefix=strlen(g->directory);
                if(path&&!strncmp(path,g->directory,prefix)&&path[prefix]=='/'){
                    char moved_path[SB_PATH_CAP];if(!native_path(moved_path,sizeof(moved_path),e,"%s%s",destination,path+prefix))goto done;
                    yyjson_mut_arr_replace(runtime,j,yyjson_mut_strcpy(d,moved_path));
                }
            }
        }else if(!strcmp(yyjson_mut_get_str(yyjson_mut_obj_get(p,"id")),g->selected)){
            yyjson_mut_obj_remove_key(p,"data_dir");yyjson_mut_obj_add_strcpy(d,p,"data_dir",destination);
        }
        /* New locations always start a fresh policy/worker instance. */
        uint64_t revision=yyjson_mut_get_uint(yyjson_mut_obj_get(p,"policy_revision"));if(!revision)revision=1;
        if(revision==UINT64_MAX){sb_fail(e,"策略版本已耗尽。");goto done;}
        yyjson_mut_obj_remove_key(p,"policy_revision");yyjson_mut_obj_add_uint(d,p,"policy_revision",revision+1);
    }
    next=yyjson_mut_write(d,0,NULL);if(!next||!sb_projects_parse(&after,next,strlen(next),e))goto done;
    if(!sb_platform_random(random,sizeof(random),e))goto done;
    for(unsigned k=0;k<16;k++)snprintf(nonce+k*2,3,"%02x",random[k]);
    if(application){
        if(!native_path(new_manager,sizeof(new_manager),e,"%s/%s",destination,strrchr(g->data,'/')+1)||
            !native_path(new_registry,sizeof(new_registry),e,"%s/%s",destination,strrchr(g->registry,'/')+1))goto done;
    }else{
        if(!native_path(new_manager,sizeof(new_manager),e,"%s/manager-%.16s",g->directory,nonce)||
            !native_path(new_registry,sizeof(new_registry),e,"%s/projects-%.16s.json",g->directory,nonce))goto done;
    }
    /* Validate every new Data path before creating or copying the target. */
    if(!sb_posix_socket_preflight(new_manager,e))goto done;
    for(size_t k=0;k<after.project_count;k++)if(!sb_posix_socket_preflight(after.projects[k].data_dir,e))goto done;
    target=sb_migration_destination(destination,e);if(!target)goto done;
    if(sb_platform_roots_overlap(g->lease->root,target) || sb_platform_roots_overlap(manager_lease->root,target)){
        sb_fail(e,"迁移目标不能与应用原数据重叠。");goto done;
    }
    for(size_t k=0;k<before.project_count;k++){
        for(size_t j=0;j<before.projects[k].directory_count;j++){
            sb_root *r=sb_platform_root(before.projects[k].directories[j].path,e);
            bool bad=!r||sb_platform_roots_overlap(r,target);sb_platform_root_close(r);
            if(bad){sb_fail(e,"迁移目标不能位于已授权文件目录内或与其重叠。");goto done;}
        }
        if(project_leases[k]&&sb_platform_roots_overlap(project_leases[k]->root,target)){
            sb_fail(e,"迁移目标不能与其他项目的数据目录重叠。");goto done;
        }
    }
    target_lease=sb_platform_lease(destination,e);if(!target_lease)goto done;
    sb_root *source=application?g->lease->root:NULL;
    for(size_t k=0;!source&&k<before.project_count;k++)if(&before.projects[k]==selected&&project_leases[k])source=project_leases[k]->root;
    if(source&&!sb_migration_copy(source,target,&state,e))goto done;
    if(!application){
        if(mkdir(new_manager,0700))goto done;
        sb_root *r=sb_platform_root(new_manager,e);bool copied=r&&sb_migration_copy(manager_lease->root,r,&state,e);sb_platform_root_close(r);if(!copied)goto done;
    }
    new_manager_lease=sb_platform_lease(new_manager,e);if(!new_manager_lease)goto done;
    const char *sealed[]={"credentials.sealed","oauth.sealed","owner.sealed"};bool has_secrets=false;
    DIR *entries=fdopendir(openat(manager_lease->root->fd,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC));if(!entries)goto done;struct dirent *entry;
    bool scan_ok=true;
    for(;;){errno=0;entry=readdir(entries);if(!entry){scan_ok=errno==0;break;}for(unsigned k=0;k<3;k++){size_t length=strlen(sealed[k]);has_secrets|=!strncmp(entry->d_name,sealed[k],length)&&(!entry->d_name[length]||entry->d_name[length]=='.');}}closedir(entries);if(!scan_ok)goto done;
    if(has_secrets){
        sb_secret_options options={.key_file=*g->key?g->key:NULL};
        if(!sb_secrets_relocate(manager_lease,new_manager_lease,&options,&old_secrets,&new_secrets,e))goto done;
        sb_oauth_project old_bindings[SB_MAX_PROJECTS],new_bindings[SB_MAX_PROJECTS];
        size_t count=migration_bindings(&before,old_secrets,old_bindings);migration_bindings(&after,new_secrets,new_bindings);
        if(!sb_oauth_relocate(old_secrets,new_secrets,before.public_origin,old_bindings,new_bindings,count,e))goto done;
    }
    if(atomic_load(&g->cancel)||sb_posix_signal){sb_fail(e,"迁移已取消；未切换位置，原数据保留。");goto done;}
    yyjson_mut_doc *receipt=yyjson_mut_doc_new(NULL);yyjson_mut_val *record=yyjson_mut_obj(receipt);
    char old_hash[65],new_hash[65],receipt_name[80];
    if(!sb_platform_sha256(original,strlen(original),old_hash,e)||!sb_platform_sha256(next,strlen(next),new_hash,e)){yyjson_mut_doc_free(receipt);goto done;}
    yyjson_mut_obj_add_strcpy(receipt,record,"migration_id",nonce);yyjson_mut_obj_add_strcpy(receipt,record,"source",application?g->directory:selected->data_dir);
    yyjson_mut_obj_add_strcpy(receipt,record,"target",destination);yyjson_mut_obj_add_strcpy(receipt,record,"source_registry_sha256",old_hash);yyjson_mut_obj_add_strcpy(receipt,record,"target_registry_sha256",new_hash);
    yyjson_mut_obj_add_uint(receipt,record,"verified_files",state.files);yyjson_mut_obj_add_uint(receipt,record,"verified_bytes",state.bytes);yyjson_mut_obj_add_bool(receipt,record,"source_retained",true);
    yyjson_mut_obj_add_bool(receipt,record,"credentials_reencrypted",has_secrets);yyjson_mut_obj_add_bool(receipt,record,"automatic_task_replay",false);
    char *receipt_text=sb_posix_json(receipt,record);snprintf(receipt_name,sizeof(receipt_name),"migration-%s.json",nonce);
    bool saved_receipt=receipt_text&&sb_migration_write(application?target:g->lease->root,receipt_name,receipt_text,strlen(receipt_text),e);free(receipt_text);if(!saved_receipt)goto done;
    yyjson_mut_doc *guard_doc=yyjson_mut_doc_new(NULL);yyjson_mut_val *guard_value=yyjson_mut_obj(guard_doc);
    yyjson_mut_obj_add_strcpy(guard_doc,guard_value,"registry",new_registry);yyjson_mut_obj_add_strcpy(guard_doc,guard_value,"manager",new_manager);
    yyjson_mut_obj_add_strcpy(guard_doc,guard_value,"migration_id",nonce);char *guard_text=sb_posix_json(guard_doc,guard_value);
    bool guarded=guard_text&&migration_guard(manager_lease,guard_text,&guards[0],e);
    for(size_t k=0;guarded&&k<before.project_count;k++)if(project_leases[k]&&strcmp(before.projects[k].data_dir,after.projects[k].data_dir))
        guarded=migration_guard(project_leases[k],guard_text,&guards[k+1],e);
    free(guard_text);if(!guarded)goto done;
    sb_root *publication=application?target:g->lease->root;
    if(!sb_migration_write(publication,strrchr(new_registry,'/')+1,next,strlen(next),e))goto done;
    yyjson_mut_doc *manifest=yyjson_mut_doc_new(NULL);yyjson_mut_val *v=yyjson_mut_obj(manifest);
    yyjson_mut_obj_add_strcpy(manifest,v,"registry",strrchr(new_registry,'/')+1);yyjson_mut_obj_add_strcpy(manifest,v,"manager",strrchr(new_manager,'/')+1);
    char *text=sb_posix_json(manifest,v);bool published=text&&sb_migration_write(publication,"active.json",text,strlen(text),e);
    if(!application&&!published&&text&&publication_matches(publication,"active.json",text)){published=true;durable=false;}
    free(text);if(!published)goto done;
    if(!application)committed=true;
    if(application){
        manifest=yyjson_mut_doc_new(NULL);v=yyjson_mut_obj(manifest);yyjson_mut_obj_add_strcpy(manifest,v,"target",destination);
        text=sb_posix_json(manifest,v);published=text&&sb_migration_write(g->lease->root,"relocation.json",text,strlen(text),e);
        if(!published&&text&&publication_matches(g->lease->root,"relocation.json",text)){published=true;durable=false;}
        free(text);if(!published)goto done;committed=true;
        pthread_mutex_lock(&g->location_mutex);
        sb_platform_lease_close(g->lease);g->lease=target_lease;target_lease=NULL;
        memcpy(g->directory,destination,strlen(destination)+1);
        pthread_mutex_unlock(&g->location_mutex);
    }
    memcpy(g->registry,new_registry,strlen(new_registry)+1);memcpy(g->data,new_manager,strlen(new_manager)+1);
    yyjson_doc_free(g->inspected);g->inspected=NULL;
    snprintf(g->message,sizeof(g->message),"迁移完成：%llu 个文件，%llu 字节已逐文件校验。原目录保留；后台和项目需重新启动。",(unsigned long long)state.files,(unsigned long long)state.bytes);
    ok=durable;
done:
    if(!committed){
        for(size_t k=0;k<=SB_MAX_PROJECTS;k++)if(guards[k].st_ino){
            sb_lease *l=k?project_leases[k-1]:manager_lease;struct stat st;
            if(l&&!fstatat(l->root->fd,"migration-guard.json",&st,AT_SYMLINK_NOFOLLOW)&&st.st_dev==guards[k].st_dev&&st.st_ino==guards[k].st_ino){unlinkat(l->root->fd,"migration-guard.json",0);fsync(l->root->fd);}
        }
    }
    sb_secrets_close(old_secrets);sb_secrets_close(new_secrets);sb_platform_lease_close(new_manager_lease);
    sb_platform_lease_close(target_lease);sb_platform_root_close(target);sb_platform_lease_close(manager_lease);
    for(size_t k=0;k<SB_MAX_PROJECTS;k++)sb_platform_lease_close(project_leases[k]);
    yyjson_mut_doc_free(d);yyjson_doc_free(cfg);free(original);free(next);sb_projects_free(&before);sb_projects_free(&after);
    if(!ok&&!*e->text)sb_fail(e,"迁移未完成；原目录及目标准备文件均已保留，没有自动重试。");return ok;
}
