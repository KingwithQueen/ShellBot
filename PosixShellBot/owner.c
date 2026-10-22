#include "posix.h"
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
struct sb_owner {int fd;pthread_t thread;atomic_bool quit,stopping;sb_owner_handler handler;void *context;char path[sizeof(((struct sockaddr_un *)0)->sun_path)];};
static bool peer(int fd){
#ifdef __APPLE__
    uid_t uid;gid_t gid;return !getpeereid(fd,&uid,&gid)&&uid==geteuid();
#else
    struct ucred who;socklen_t size=sizeof(who);return !getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&who,&size)&&who.uid==geteuid();
#endif
}
bool sb_posix_socket_path(const char *dir,char *out,size_t capacity,sb_error *e){int n=snprintf(out,capacity,"%s/owner.sock",dir);if(n<0||(size_t)n>=capacity){sb_fail(e,"Data directory path exceeds this platform's local socket path limit.");return false;}return true;}
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
    sb_root *root=sb_platform_root(data,e);if(!root)return NULL;if(!sb_posix_private(root,e)){sb_platform_root_close(root);return NULL;}
    sb_owner *o=calloc(1,sizeof(*o));if(!o){sb_platform_root_close(root);return NULL;}o->fd=-1;o->handler=handler;o->context=context;
    atomic_init(&o->quit,false);atomic_init(&o->stopping,false);
    if(!sb_posix_socket_path(data,o->path,sizeof(o->path),e))goto fail;
    struct stat st;
    if(!fstatat(root->fd,"owner.sock",&st,AT_SYMLINK_NOFOLLOW)){
        if(!S_ISSOCK(st.st_mode)||st.st_uid!=geteuid()){sb_fail(e,"Unsafe existing owner socket path.");goto fail;}
        if(unlinkat(root->fd,"owner.sock",0)){sb_posix_error(e,"Remove stale owner socket");goto fail;}
    }else if(errno!=ENOENT){sb_posix_error(e,"Inspect owner socket");goto fail;}
    o->fd=socket(AF_UNIX,SOCK_STREAM,0);if(o->fd<0)goto fail;fcntl(o->fd,F_SETFD,FD_CLOEXEC);
    struct sockaddr_un a={.sun_family=AF_UNIX};memcpy(a.sun_path,o->path,strlen(o->path)+1);
    if(bind(o->fd,(struct sockaddr *)&a,sizeof(a))||chmod(o->path,0600)||listen(o->fd,4)){sb_posix_error(e,"Create private owner socket");goto fail;}
    if(pthread_create(&o->thread,NULL,serve,o)){sb_fail(e,"Cannot start local owner thread.");goto fail;}sb_platform_root_close(root);return o;
fail:sb_platform_root_close(root);if(o->fd>=0)close(o->fd);free(o);return NULL;
}
static char *host_request(void *context,const char *bytes,size_t n){sb_host *h=context;return sb_policy_owner_request(h->policy,bytes,n);}
sb_owner *sb_owner_start(sb_host *h,sb_error *e){return sb_owner_start_handler(h->config.data_dir,host_request,h,e);}
void sb_owner_stop(sb_owner *o){if(o){atomic_store(&o->quit,true);pthread_join(o->thread,NULL);close(o->fd);unlink(o->path);free(o);}}
bool sb_owner_stopping(sb_owner *o){return o&&atomic_load(&o->stopping);}
char *sb_owner_exchange(const char *dir,const char *request,size_t length,sb_error *e){
    sb_root *r=sb_platform_root(dir,e);if(!r)return NULL;
    bool ok=sb_posix_private(r,e);sb_platform_root_close(r);if(!ok)return NULL;
    struct sockaddr_un a={.sun_family=AF_UNIX};if(!sb_posix_socket_path(dir,a.sun_path,sizeof(a.sun_path),e)||length>4096)return NULL;
    int fd=socket(AF_UNIX,SOCK_STREAM,0);if(fd<0)return NULL;fcntl(fd,F_SETFD,FD_CLOEXEC);
    struct timeval timeout={.tv_sec=3};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    if(connect(fd,(struct sockaddr *)&a,sizeof(a))||!peer(fd)||!sb_posix_write(fd,request,length,e)||!sb_posix_write(fd,"\n",1,e)){sb_posix_error(e,"Connect local owner channel");close(fd);return NULL;}
    char *reply=malloc(SB_RESPONSE_LIMIT+1);if(!reply){close(fd);return NULL;}size_t at=0;bool complete=false;
    while(at<SB_RESPONSE_LIMIT){ssize_t n=read(fd,reply+at,SB_RESPONSE_LIMIT-at);if(n<0&&errno==EINTR)continue;if(n<=0)break;at+=(size_t)n;if(memchr(reply,'\n',at)){complete=true;break;}}
    close(fd);if(!complete){free(reply);sb_fail(e,"Incomplete owner response.");return NULL;}reply[at]=0;return reply;
}
int sb_owner_command(const char *dir,const char *command,const char *id,sb_error *e){yyjson_mut_doc *d=yyjson_mut_doc_new(NULL);if(!d)return 2;yyjson_mut_val *v=yyjson_mut_obj(d);yyjson_mut_doc_set_root(d,v);yyjson_mut_obj_add_str(d,v,"command",command);if(id)yyjson_mut_obj_add_str(d,v,"id",id);char *request=yyjson_mut_write(d,0,NULL),*reply=request?sb_owner_exchange(dir,request,strlen(request),e):NULL;yyjson_mut_doc_free(d);free(request);if(!reply)return 2;puts(reply);yyjson_doc *parsed=yyjson_read(reply,strlen(reply),0);bool failed=!parsed||yyjson_obj_get(yyjson_doc_get_root(parsed),"error");yyjson_doc_free(parsed);free(reply);return failed?2:0;}
