#include "posix.h"
#include "desktop.h"
#include "sb_projects.h"
#include "local_socket.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#define ENDPOINT "owner.endpoint.json"
struct sb_owner {
    int fd; pthread_t thread; atomic_bool quit,stopping; bool threaded,bound,published;
    sb_owner_handler handler; void *context; sb_root *root; sb_local_socket *local;
    struct stat socket_identity,record_identity;
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
};
static bool same_inode(const struct stat *a,const struct stat *b){return a->st_dev==b->st_dev&&a->st_ino==b->st_ino&&a->st_mode==b->st_mode&&a->st_uid==b->st_uid&&a->st_nlink==b->st_nlink;}
static bool same_record(const struct stat *a,const struct stat *b){
    if(!same_inode(a,b)||a->st_size!=b->st_size)return false;
#ifdef __APPLE__
    return a->st_mtimespec.tv_sec==b->st_mtimespec.tv_sec&&a->st_mtimespec.tv_nsec==b->st_mtimespec.tv_nsec&&a->st_ctimespec.tv_sec==b->st_ctimespec.tv_sec&&a->st_ctimespec.tv_nsec==b->st_ctimespec.tv_nsec;
#else
    return a->st_mtim.tv_sec==b->st_mtim.tv_sec&&a->st_mtim.tv_nsec==b->st_mtim.tv_nsec&&a->st_ctim.tv_sec==b->st_ctim.tv_sec&&a->st_ctim.tv_nsec==b->st_ctim.tv_nsec;
#endif
}
static bool entry_is(sb_root *r,const char *name,const struct stat *expected){struct stat now;return !fstatat(r->fd,name,&now,AT_SYMLINK_NOFOLLOW)&&same_inode(&now,expected);}
static bool record_is(sb_root *r,const struct stat *expected){struct stat now;return !fstatat(r->fd,ENDPOINT,&now,AT_SYMLINK_NOFOLLOW)&&same_record(&now,expected);}
static bool data_named(sb_root *r,sb_error *e){
    sb_root *named=sb_platform_root(r->path,e);struct stat a,b;
    bool ok=named&&!fstat(r->fd,&a)&&!fstat(named->fd,&b)&&a.st_dev==b.st_dev&&a.st_ino==b.st_ino&&sb_posix_private(named,e);
    sb_platform_root_close(named);if(!ok)sb_fail(e,"Data directory identity changed during owner setup or connection.");return ok;
}
static bool short_path(const char *dir,char *out,size_t capacity){int n=snprintf(out,capacity,"%s/owner.sock",dir);return n>=0&&(size_t)n<capacity;}
bool sb_posix_socket_preflight(const char *dir,sb_error *e){
    if(!sb_project_path_valid(dir)){sb_fail(e,"Data requires a canonical absolute path without dot, parent or empty components.");return false;}
    /* Reserve room for generated private state filenames as well as the directory. */
    if(strlen(dir)>=(size_t)PATH_MAX-64){sb_fail(e,"Data path leaves insufficient room for private files within the native filesystem path limit.");return false;}
    return true;
}
/* A record can select only an unpredictable token in the fixed private runtime base.
   It cannot name an arbitrary path, UID, or a different Data directory. */
static int endpoint_read(sb_root *r,char token[33],struct stat *identity,sb_error *e){
    int fd=openat(r->fd,ENDPOINT,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
    if(fd<0){if(errno==ENOENT)return 0;sb_posix_error(e,"Open owner endpoint record");return -1;}
    struct stat after,entry;char bytes[513],root_id[50];size_t at=0;bool ok=false;
    if(fstat(fd,identity)||!S_ISREG(identity->st_mode)||identity->st_uid!=geteuid()||(identity->st_mode&07777)!=0600||identity->st_nlink!=1||identity->st_size<=0||identity->st_size>512)goto done;
    while(at<(size_t)identity->st_size){ssize_t n=read(fd,bytes+at,(size_t)identity->st_size-at);if(n<0&&errno==EINTR)continue;if(n<=0)goto done;at+=(size_t)n;}
    if(fstat(fd,&after)||!same_record(identity,&after)||fstatat(r->fd,ENDPOINT,&entry,AT_SYMLINK_NOFOLLOW)||!same_record(identity,&entry)||!sb_platform_root_identity(r,root_id,e))goto done;
    bytes[at]=0;yyjson_doc *d=yyjson_read(bytes,at,0);yyjson_val *v=yyjson_doc_get_root(d);
    const char *keys[]={"version","identity","token"};const char *bound=sb_string(yyjson_obj_get(v,"identity")),*value=sb_string(yyjson_obj_get(v,"token"));yyjson_val *version=yyjson_obj_get(v,"version");
    ok=yyjson_obj_size(v)==3&&sb_config_keys(v,keys,3)&&yyjson_is_uint(version)&&yyjson_get_uint(version)==1&&bound&&!strcmp(bound,root_id)&&value&&yyjson_get_len(yyjson_obj_get(v,"identity"))==strlen(bound)&&yyjson_get_len(yyjson_obj_get(v,"token"))==32&&strlen(value)==32&&strspn(value,"0123456789abcdef")==32;
    if(ok)memcpy(token,value,33);yyjson_doc_free(d);
done:close(fd);if(!ok)sb_fail(e,"Unsafe, changed or mismatched owner endpoint record.");return ok?1:-1;
}
static bool endpoint_publish(sb_owner *o,sb_error *e){
    char name[64],bytes[256],identity[50];const char *token=sb_local_socket_token(o->local);
    if(!data_named(o->root,e)||!sb_platform_root_identity(o->root,identity,e))return false;
    int n=snprintf(bytes,sizeof(bytes),"{\"version\":1,\"identity\":\"%s\",\"token\":\"%s\"}\n",identity,token);
    snprintf(name,sizeof(name),".owner-endpoint-%s",token);
    int fd=openat(o->root->fd,name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0){sb_posix_error(e,"Create owner endpoint record");return false;}
    struct stat st={0};bool ok=!fstat(fd,&st)&&n>0&&(size_t)n<sizeof(bytes)&&sb_posix_write(fd,bytes,(size_t)n,e)&&!fsync(fd);
    /* Atomic no-replace rename avoids both overwrites and a crash window with two links. */
    if(ok){
#ifdef __APPLE__
        ok=!renameatx_np(o->root->fd,name,o->root->fd,ENDPOINT,RENAME_EXCL);
#else
        ok=!syscall(SYS_renameat2,o->root->fd,name,o->root->fd,ENDPOINT,1u);
#endif
        if(ok){o->record_identity=st;o->published=true;}
    }
    close(fd);if(!o->published&&entry_is(o->root,name,&st))unlinkat(o->root->fd,name,0);
    if(ok)ok=!fsync(o->root->fd);
    if(!ok)sb_posix_error(e,"Publish owner endpoint record");return ok;
}
static bool stale_short(sb_root *r,const char *path,sb_error *e){
    struct stat st;if(fstatat(r->fd,"owner.sock",&st,AT_SYMLINK_NOFOLLOW)){if(errno==ENOENT)return true;sb_posix_error(e,"Inspect owner socket");return false;}
    if(!S_ISSOCK(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&07777)!=0600||st.st_nlink!=1){sb_fail(e,"Unsafe existing owner socket path.");return false;}
    int fd=socket(AF_UNIX,SOCK_STREAM,0);if(fd<0){sb_posix_error(e,"Inspect old owner channel");return false;}
    fcntl(fd,F_SETFD,FD_CLOEXEC);fcntl(fd,F_SETFL,O_NONBLOCK);
    struct sockaddr_un a={.sun_family=AF_UNIX};memcpy(a.sun_path,path,strlen(path)+1);
    int rc=connect(fd,(struct sockaddr *)&a,sizeof(a)),saved=errno;close(fd);
    if(!rc||(saved!=ECONNREFUSED&&saved!=ENOENT)){sb_fail(e,"Existing owner channel is active or unavailable; it was preserved.");return false;}
    if(!entry_is(r,"owner.sock",&st)||unlinkat(r->fd,"owner.sock",0)){sb_fail(e,"Existing owner socket changed during recovery.");return false;}return true;
}
static bool peer(int fd){
#ifdef __APPLE__
    uid_t uid;gid_t gid;return !getpeereid(fd,&uid,&gid)&&uid==geteuid();
#else
    struct ucred who;socklen_t size=sizeof(who);return !getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&who,&size)&&who.uid==geteuid();
#endif
}
static void *serve(void *ctx){
    sb_owner *o=ctx;
    while(!atomic_load(&o->quit)) {
        struct pollfd p={.fd=o->fd,.events=POLLIN};int n=poll(&p,1,200);
        if(n<=0)continue;
        int fd=accept(o->fd,NULL,NULL);if(fd<0)continue;fcntl(fd,F_SETFD,FD_CLOEXEC);
        struct timeval timeout={.tv_sec=2};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
        if(peer(fd)) {
            char bytes[4097];size_t at=0;bool complete=false;
            while(at<sizeof(bytes)-1){ssize_t got=read(fd,bytes+at,sizeof(bytes)-1-at);if(got<0&&errno==EINTR)continue;if(got<=0)break;at+=(size_t)got;if(memchr(bytes,'\n',at)){complete=true;break;}}
            char *reply=NULL;
            if(complete){bytes[at]=0;yyjson_doc *d=yyjson_read(bytes,at,0);const char *cmd=sb_string(yyjson_obj_get(yyjson_doc_get_root(d),"command"));
                if(cmd&&!strcmp(cmd,"stop")){atomic_store(&o->stopping,true);reply=sb_strdup("{\"stopping\":true}");}
                else reply=o->handler(o->context,bytes,at);yyjson_doc_free(d);
            }
            if(reply){sb_error e={0};sb_posix_write(fd,reply,strlen(reply),&e);sb_posix_write(fd,"\n",1,&e);free(reply);}
        }
        close(fd);
    }
    return NULL;
}
sb_owner *sb_owner_start_handler(const char *data,sb_owner_handler handler,void *context,sb_error *e){
    if(!sb_posix_socket_preflight(data,e))return NULL;
    sb_root *root=sb_platform_root(data,e);if(!root)return NULL;if(!sb_posix_private(root,e)){sb_platform_root_close(root);return NULL;}
    sb_owner *o=calloc(1,sizeof(*o));if(!o){sb_platform_root_close(root);return NULL;}o->fd=-1;o->handler=handler;o->context=context;o->root=root;
    atomic_init(&o->quit,false);atomic_init(&o->stopping,false);
    bool legacy=short_path(data,o->path,sizeof(o->path));
    if(legacy){if(!stale_short(root,o->path,e))goto fail;}
    else{
        char old_token[33];struct stat old_record;int found=endpoint_read(root,old_token,&old_record,e);
        if(found<0)goto fail;
        if(found&&(!sb_local_socket_reclaim(old_token,e)||!record_is(root,&old_record)||unlinkat(root->fd,ENDPOINT,0))){if(!e->text[0])sb_fail(e,"Owner endpoint changed during recovery.");goto fail;}
        o->local=sb_local_socket_create(e);if(!o->local)goto fail;
        snprintf(o->path,sizeof(o->path),"%s",sb_local_socket_path(o->local));
    }
    o->fd=socket(AF_UNIX,SOCK_STREAM,0);if(o->fd<0)goto fail;fcntl(o->fd,F_SETFD,FD_CLOEXEC);
    if(o->local){if(!sb_local_socket_bind(o->local,o->fd,4,e))goto fail;}
    else{
        struct sockaddr_un a={.sun_family=AF_UNIX};memcpy(a.sun_path,o->path,strlen(o->path)+1);
        if(!data_named(root,e))goto fail;
        if(bind(o->fd,(struct sockaddr *)&a,sizeof(a))){sb_posix_error(e,"Create private owner socket");goto fail;}
        if(fstatat(root->fd,"owner.sock",&o->socket_identity,AT_SYMLINK_NOFOLLOW)){sb_posix_error(e,"Inspect new owner socket");goto fail;}o->bound=true;
        if(fchmodat(root->fd,"owner.sock",0600,0)||listen(o->fd,4)){sb_posix_error(e,"Secure private owner socket");goto fail;}
        if(fstatat(root->fd,"owner.sock",&o->socket_identity,AT_SYMLINK_NOFOLLOW))goto fail;
    }
    if(!data_named(root,e))goto fail;
    if(pthread_create(&o->thread,NULL,serve,o)){sb_fail(e,"Cannot start local owner thread.");goto fail;}o->threaded=true;
    if(o->local&&!endpoint_publish(o,e))goto fail;
    return o;
fail:sb_owner_stop(o);return NULL;
}
char *sb_posix_owner_request(sb_host *h, const char *bytes, size_t n) {
    yyjson_doc *doc = yyjson_read(bytes, n, 0);
    yyjson_val *v = yyjson_doc_get_root(doc);
    const char *command = sb_string(yyjson_obj_get(v, "command"));
    char *reply = NULL;
    if (command && !strncmp(command, "desktop-", 8)) {
        const char *keys[] = {"command"};
        reply = sb_config_keys(v, keys, 1) ? sb_posix_desktop_owner(h, command)
                                           : sb_posix_json_error("Unknown desktop owner fields.");
    }
    yyjson_doc_free(doc);
    return reply ? reply : sb_policy_owner_request(h->policy, bytes, n);
}
static char *host_request(void *context, const char *bytes, size_t n) {
    return sb_posix_owner_request(context, bytes, n);
}
sb_owner *sb_owner_start(sb_host *h,sb_error *e){return sb_owner_start_handler(h->config.data_dir,host_request,h,e);}
void sb_owner_stop(sb_owner *o){
    if(!o)return;atomic_store(&o->quit,true);if(o->threaded)pthread_join(o->thread,NULL);if(o->fd>=0)close(o->fd);
    if(o->published&&entry_is(o->root,ENDPOINT,&o->record_identity))unlinkat(o->root->fd,ENDPOINT,0);
    if(o->bound&&entry_is(o->root,"owner.sock",&o->socket_identity))unlinkat(o->root->fd,"owner.sock",0);
    sb_local_socket_close(o->local);sb_platform_root_close(o->root);free(o);
}
bool sb_owner_stopping(sb_owner *o){return o&&atomic_load(&o->stopping);}
char *sb_owner_exchange(const char *dir,const char *request,size_t length,sb_error *e){
    if(!sb_posix_socket_preflight(dir,e)||length>4096)return NULL;
    sb_root *r=sb_platform_root(dir,e);if(!r)return NULL;
    sb_local_socket *local=NULL;struct stat identity={0};int fd=-1;char *reply=NULL;bool complete=false;
    if(!sb_posix_private(r,e))goto done;
    struct sockaddr_un a={.sun_family=AF_UNIX};bool legacy=short_path(dir,a.sun_path,sizeof(a.sun_path));
    if(legacy){
        if(fstatat(r->fd,"owner.sock",&identity,AT_SYMLINK_NOFOLLOW)||!S_ISSOCK(identity.st_mode)||identity.st_uid!=geteuid()||(identity.st_mode&07777)!=0600||identity.st_nlink!=1){sb_fail(e,"Missing or unsafe owner socket.");goto done;}
    }else{
        char token[33];if(endpoint_read(r,token,&identity,e)!=1){if(!e->text[0])sb_fail(e,"Owner endpoint record is missing.");goto done;}
        local=sb_local_socket_open(token,e);if(!local)goto done;
        snprintf(a.sun_path,sizeof(a.sun_path),"%s",sb_local_socket_path(local));
    }
    fd=socket(AF_UNIX,SOCK_STREAM,0);if(fd<0)goto done;fcntl(fd,F_SETFD,FD_CLOEXEC);
    struct timeval timeout={.tv_sec=3};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    if(connect(fd,(struct sockaddr *)&a,sizeof(a))||!peer(fd)){sb_posix_error(e,"Connect local owner channel");goto done;}
    if(!(legacy?entry_is(r,"owner.sock",&identity):record_is(r,&identity))||!data_named(r,e)||(local&&!sb_local_socket_check(local,e))){sb_fail(e,"Local owner endpoint changed during connection.");goto done;}
    /* Exactly one connection and one send: approval/stop requests are never replayed. */
    if(!sb_posix_write(fd,request,length,e)||!sb_posix_write(fd,"\n",1,e))goto done;
    reply=malloc(SB_RESPONSE_LIMIT+1);if(!reply)goto done;size_t at=0;
    while(at<SB_RESPONSE_LIMIT){ssize_t n=read(fd,reply+at,SB_RESPONSE_LIMIT-at);if(n<0&&errno==EINTR)continue;if(n<=0)break;at+=(size_t)n;if(memchr(reply,'\n',at)){complete=true;break;}}
    if(complete)reply[at]=0;else sb_fail(e,"Incomplete owner response.");
done:if(fd>=0)close(fd);sb_local_socket_close(local);sb_platform_root_close(r);if(!complete){free(reply);reply=NULL;}return reply;
}
int sb_owner_command(const char *dir,const char *command,const char *id,sb_error *e){yyjson_mut_doc *d=yyjson_mut_doc_new(NULL);if(!d)return 2;yyjson_mut_val *v=yyjson_mut_obj(d);yyjson_mut_doc_set_root(d,v);yyjson_mut_obj_add_str(d,v,"command",command);if(id)yyjson_mut_obj_add_str(d,v,"id",id);char *request=yyjson_mut_write(d,0,NULL),*reply=request?sb_owner_exchange(dir,request,strlen(request),e):NULL;yyjson_mut_doc_free(d);free(request);if(!reply)return 2;puts(reply);yyjson_doc *parsed=yyjson_read(reply,strlen(reply),0);bool failed=!parsed||yyjson_obj_get(yyjson_doc_get_root(parsed),"error");yyjson_doc_free(parsed);free(reply);return failed?2:0;}
