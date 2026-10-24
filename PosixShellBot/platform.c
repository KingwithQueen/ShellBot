#include "posix.h"
#include <sys/file.h>
#include <sys/utsname.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif

void sb_posix_error(sb_error *e,const char *operation) {
    if(e)snprintf(e->text,sizeof(e->text),"%s: %s",operation,strerror(errno));
}
static bool valid_relative(const char *p,bool dot) {
    if(!p || !*p || *p=='/' || strlen(p)>=SB_PATH_CAP)return false;
    if(dot && !strcmp(p,"."))return true;
    const char *start=p;
    for(;;p++) {
        if(*p && (unsigned char)*p<32)return false;
        if(*p=='/' || !*p) {
            size_t n=(size_t)(p-start);
            if(!n || (n==1 && *start=='.') || (n==2 && !memcmp(start,"..",2)))return false;
            if(!*p)return true;
            start=p+1;
        }
    }
}
/* Walk each component through directory descriptors, never through symlinks. */
static int walk(int anchor,const char *path) {
    int fd=openat(anchor,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if(fd<0)return -1;
    if(!strcmp(path,"."))return fd;
    char *copy=sb_strdup(path),*save=NULL;
    if(!copy){close(fd);errno=ENOMEM;return -1;}
    for(char *part=strtok_r(copy,"/",&save);part;part=strtok_r(NULL,"/",&save)) {
        int next=openat(fd,part,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        int saved=errno;close(fd);fd=next;errno=saved;
        if(fd<0)break;
    }
    free(copy);return fd;
}
static int parent(sb_root *root,const char *path,char **leaf,sb_error *e) {
    if(!valid_relative(path,false)){sb_fail(e,"A relative path without empty, dot or parent components is required.");return -1;}
    char *copy=sb_strdup(path);
    if(!copy){sb_fail(e,"Out of memory.");return -1;}
    char *slash=strrchr(copy,'/');
    *leaf=sb_strdup(slash?slash+1:copy);
    if(slash)*slash=0;
    int fd=*leaf?walk(root->fd,slash?copy:"."):-1;
    free(copy);
    if(fd<0){free(*leaf);*leaf=NULL;sb_posix_error(e,"Open anchored parent");}
    return fd;
}
static bool checked(int fd,bool directory,struct stat *st,sb_error *e) {
    if(fstat(fd,st)){sb_posix_error(e,"Inspect file");return false;}
    if(directory? !S_ISDIR(st->st_mode):(!S_ISREG(st->st_mode)||st->st_nlink!=1)) {
        sb_fail(e,"Expected a directory or a regular file with exactly one hard link; special files are unavailable.");return false;
    }
    return true;
}
static bool same(const struct stat *a,const struct stat *b) {return a->st_dev==b->st_dev && a->st_ino==b->st_ino;}
static void identity(const struct stat *st,char out[50]) {snprintf(out,50,"%llx:%llx",(unsigned long long)st->st_dev,(unsigned long long)st->st_ino);}
static bool version(const struct stat *st,char out[65],sb_error *e) {
    char text[320];
#ifdef __APPLE__
    struct timespec mt=st->st_mtimespec,ct=st->st_ctimespec;
#else
    struct timespec mt=st->st_mtim,ct=st->st_ctim;
#endif
    int n=snprintf(text,sizeof(text),"%llu:%llu:%llu:%llu:%llu:%lld:%ld:%lld:%ld:%llu",
        (unsigned long long)st->st_dev,(unsigned long long)st->st_ino,(unsigned long long)st->st_mode,
        (unsigned long long)st->st_size,(unsigned long long)st->st_nlink,
        (long long)mt.tv_sec,mt.tv_nsec,(long long)ct.tv_sec,ct.tv_nsec,(unsigned long long)st->st_uid);
    return n>0 && (size_t)n<sizeof(text) && sb_platform_sha256(text,(size_t)n,out,e);
}
static bool unchanged(int fd,const struct stat *initial,sb_error *e) {
    struct stat now;char a[65],b[65];
    if(fstat(fd,&now) || !version(initial,a,e) || !version(&now,b,e) || strcmp(a,b)) {
        sb_fail(e,"File changed during the operation; no stale approval can be used.");return false;
    }
    return true;
}
uint64_t sb_platform_millis(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;}
uint64_t sb_platform_unix_ms(void){struct timespec t;clock_gettime(CLOCK_REALTIME,&t);return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;}
void sb_platform_info(char *os,size_t cap,char *arch,size_t acap){struct utsname u;if(uname(&u)){snprintf(os,cap,"POSIX");snprintf(arch,acap,"unknown");}else{snprintf(os,cap,"%s %s",u.sysname,u.release);snprintf(arch,acap,"%s",u.machine);}}
sb_root *sb_platform_root(const char *path,sb_error *e) {
    if(!path || *path!='/' || strlen(path)>=SB_PATH_CAP){sb_fail(e,"An absolute POSIX directory is required.");return NULL;}
    const char *relative=path+1;
    if(*relative && !valid_relative(relative,false)){sb_fail(e,"Noncanonical absolute directory path.");return NULL;}
    int base=open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    int fd=base<0?-1:walk(base,*relative?relative:".");if(base>=0)close(base);
    if(fd<0){sb_posix_error(e,"Open root without symlinks");return NULL;}
    sb_root *r=calloc(1,sizeof(*r));
    if(!r || !(r->path=sb_strdup(path))){free(r);close(fd);sb_fail(e,"Out of memory.");return NULL;}
    r->fd=fd;return r;
}
void sb_platform_root_close(sb_root *r){if(r){close(r->fd);free(r->path);free(r);}}
static bool beneath(int ancestor,int descendant) {
    struct stat a,here,up;
    if(fstat(ancestor,&a))return true; /* Fail closed on uncertainty. */
    int fd=dup(descendant);if(fd<0)return true;
    for(unsigned i=0;i<1024;i++) {
        if(fstat(fd,&here)){close(fd);return true;}
        if(same(&a,&here)){close(fd);return true;}
        int next=openat(fd,"..",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
        if(next<0 || fstat(next,&up)){if(next>=0)close(next);close(fd);return true;}
        close(fd);fd=next;
        if(same(&here,&up)){close(fd);return false;}
    }
    close(fd);return true;
}
bool sb_platform_roots_overlap(const sb_root *a,const sb_root *b){return beneath(a->fd,b->fd)||beneath(b->fd,a->fd);}
bool sb_platform_root_identity(const sb_root *r,char out[50],sb_error *e){struct stat st;if(fstat(r->fd,&st)){sb_posix_error(e,"Root identity");return false;}identity(&st,out);return true;}
sb_root *sb_platform_account_parent(const char *absolute,char **relative,sb_error *e){
    *relative=NULL;if(!absolute||*absolute!='/'||!valid_relative(absolute+1,false)){sb_fail(e,"A canonical absolute account path is required.");return NULL;}
    char *copy=sb_strdup(absolute);if(!copy)return NULL;char *slash=strrchr(copy,'/');*relative=sb_strdup(slash+1);if(slash==copy)slash[1]=0;else *slash=0;
    sb_root *r=*relative?sb_platform_root(copy,e):NULL;free(copy);if(!r){free(*relative);*relative=NULL;}return r;
}
bool sb_platform_target_within(sb_root *root,sb_root *p,const char *rel,sb_error *e){
    if(beneath(root->fd,p->fd))return true;
    /* Account paths may name the configured root itself. Compare its handle too. */
    sb_file *f=sb_platform_open(p,rel,true,e);if(!f){if(e)e->text[0]=0;return false;}
    bool inside=beneath(root->fd,f->fd);sb_platform_close(f);return inside;
}
sb_file *sb_platform_open(sb_root *root,const char *path,bool directory,sb_error *e) {
    int fd=-1;char *leaf=NULL;
    if(directory && path && !strcmp(path,"."))fd=walk(root->fd,".");
    else {
        int p=parent(root,path,&leaf,e);if(p<0)return NULL;
        fd=openat(p,leaf,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK|(directory?O_DIRECTORY:0));
        int saved=errno;close(p);free(leaf);errno=saved;
    }
    if(fd<0){sb_posix_error(e,"Open authorized file");return NULL;}
    sb_file *f=calloc(1,sizeof(*f));
    if(!f){close(fd);sb_fail(e,"Out of memory.");return NULL;}
    f->fd=fd;f->directory=directory;
    if(!checked(fd,directory,&f->initial,e)||(!directory&&flock(fd,LOCK_SH|LOCK_NB))){if(!e||!*e->text)sb_fail(e,"File is locked by another cooperating operation.");sb_platform_close(f);return NULL;}
    return f;
}
void sb_platform_close(sb_file *f){if(f){close(f->fd);free(f);}}
char *sb_platform_directory_path(sb_file *f,sb_error *e){
    char path[SB_PATH_CAP];if(!f||!f->directory){sb_fail(e,"A directory handle is required.");return NULL;}
#ifdef __APPLE__
    if(fcntl(f->fd,F_GETPATH,path)){sb_posix_error(e,"Resolve directory handle");return NULL;}
#else
    char link[64];snprintf(link,sizeof(link),"/proc/self/fd/%d",f->fd);ssize_t n=readlink(link,path,sizeof(path)-1);if(n<=0||(size_t)n>=sizeof(path)-1){sb_posix_error(e,"Resolve directory handle");return NULL;}path[n]=0;
#endif
    sb_root *again=sb_platform_root(path,e);struct stat a,b;bool ok=again&&!fstat(f->fd,&a)&&!fstat(again->fd,&b)&&same(&a,&b);sb_platform_root_close(again);
    if(!ok){sb_fail(e,"Working directory moved or is no longer a canonical path.");return NULL;}return sb_strdup(path);
}
uint64_t sb_platform_size(sb_file *f){return (uint64_t)f->initial.st_size;}
bool sb_platform_file_version(sb_file *f,char out[65],sb_error *e){struct stat st;return checked(f->fd,f->directory,&st,e)&&version(&st,out,e);}
bool sb_platform_read(sb_file *f,uint64_t offset,unsigned char *out,size_t cap,size_t *length,sb_error *e) {
    *length=0;
    if(f->directory || offset>INT64_MAX || !unchanged(f->fd,&f->initial,e))return false;
    ssize_t n;do{n=pread(f->fd,out,cap,(off_t)offset);}while(n<0 && errno==EINTR);
    if(n<0){sb_posix_error(e,"Read file");return false;}
    if(!unchanged(f->fd,&f->initial,e))return false;
    *length=(size_t)n;return true;
}
bool sb_platform_file_sha256(sb_file *f,char hex[65],sb_error *e) {
    sb_sha c;sb_sha_init(&c);unsigned char bytes[32768];uint64_t offset=0;
    for(;;){size_t n;if(!sb_platform_read(f,offset,bytes,sizeof(bytes),&n,e))return false;if(!n)break;sb_sha_update(&c,bytes,n);offset+=n;}
    sb_sha_finish(&c,hex);return true;
}
static int entry_compare(const void *a,const void *b){return strcmp(((const sb_entry *)a)->name,((const sb_entry *)b)->name);}
bool sb_platform_list(sb_file *f,size_t offset,size_t limit,sb_entry **out,size_t *count,bool *more,sb_error *e) {
    *out=NULL;*count=0;*more=false;
    int duplicate=openat(f->fd,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC);DIR *d=duplicate<0?NULL:fdopendir(duplicate);
    if(!d){if(duplicate>=0)close(duplicate);sb_posix_error(e,"List directory");return false;}
    sb_entry *all=NULL;size_t n=0;bool ok=true;struct dirent *de;
    errno=0;
    while((de=readdir(d))) {
        if(!strcmp(de->d_name,".")||!strcmp(de->d_name,".."))continue;
        char *decoded=NULL;size_t decoded_length,consumed;const char *encoding;
        if(!sb_decode((const unsigned char *)de->d_name,strlen(de->d_name),true,"utf-8",false,&decoded,&decoded_length,&consumed,&encoding,e)){ok=false;break;}
        free(decoded);
        if(n==10000){sb_fail(e,"Directory exceeds the 10000-entry inspection limit.");ok=false;break;}
        struct stat st;if(fstatat(f->fd,de->d_name,&st,AT_SYMLINK_NOFOLLOW)){sb_posix_error(e,"Inspect directory entry");ok=false;break;}
        sb_entry *grown=realloc(all,(n+1)*sizeof(*all));if(!grown){sb_fail(e,"Out of memory.");ok=false;break;}all=grown;
        all[n]=(sb_entry){.name=sb_strdup(de->d_name),.size=(uint64_t)st.st_size,.directory=S_ISDIR(st.st_mode),.reparse=S_ISLNK(st.st_mode)||(!S_ISDIR(st.st_mode)&&(!S_ISREG(st.st_mode)||st.st_nlink!=1))};
        if(!all[n].name){sb_fail(e,"Out of memory.");ok=false;break;}n++;errno=0;
    }
    if(!de && errno){sb_posix_error(e,"Read directory");ok=false;}closedir(d);
    if(!ok){sb_platform_entries_free(all,n);return false;}
    if(n>1)qsort(all,n,sizeof(*all),entry_compare);
    size_t begin=offset<n?offset:n,end=n-begin>limit?begin+limit:n;
    *more=end<n;*count=end-begin;
    if(*count){*out=calloc(*count,sizeof(**out));if(!*out){sb_platform_entries_free(all,n);sb_fail(e,"Out of memory.");return false;}memcpy(*out,all+begin,*count*sizeof(**out));for(size_t i=begin;i<end;i++)all[i].name=NULL;}
    sb_platform_entries_free(all,n);return true;
}
void sb_platform_entries_free(sb_entry *entries,size_t count){for(size_t i=0;i<count;i++)free(entries[i].name);free(entries);}
bool sb_platform_random(unsigned char *bytes,size_t length,sb_error *e){int fd=open("/dev/urandom",O_RDONLY|O_CLOEXEC);if(fd<0){sb_posix_error(e,"Open random source");return false;}size_t at=0;while(at<length){ssize_t n=read(fd,bytes+at,length-at);if(n<0&&errno==EINTR)continue;if(n<=0){close(fd);sb_fail(e,"System random source failed.");return false;}at+=(size_t)n;}close(fd);return true;}
bool sb_posix_write(int fd,const void *bytes,size_t length,sb_error *e){const unsigned char *p=bytes;while(length){ssize_t n=write(fd,p,length);if(n<0&&errno==EINTR)continue;if(n<=0){sb_posix_error(e,"Write file");return false;}p+=n;length-=(size_t)n;}return true;}
static char *load_fd(int fd,size_t maximum,size_t *length,sb_error *e){struct stat st;if(!checked(fd,false,&st,e)||st.st_size<0||(uint64_t)st.st_size>maximum){sb_fail(e,"Private/configuration file is invalid or exceeds its budget.");return NULL;}char *bytes=malloc((size_t)st.st_size+1);if(!bytes){sb_fail(e,"Out of memory.");return NULL;}size_t n=0;while(n<(size_t)st.st_size){ssize_t got=pread(fd,bytes+n,(size_t)st.st_size-n,(off_t)n);if(got<0&&errno==EINTR)continue;if(got<=0){free(bytes);sb_posix_error(e,"Read complete file");return NULL;}n+=(size_t)got;}if(!unchanged(fd,&st,e)){free(bytes);return NULL;}bytes[n]=0;*length=n;return bytes;}
char *sb_platform_load_config(const char *path,size_t *length,sb_error *e){int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);if(fd<0){sb_posix_error(e,"Open configuration");return NULL;}char *s=load_fd(fd,SB_REQUEST_LIMIT,length,e);close(fd);return s;}
sb_mutex *sb_platform_mutex_new(void){sb_mutex *m=malloc(sizeof(*m));if(m&&pthread_mutex_init(&m->value,NULL)){free(m);return NULL;}return m;}
void sb_platform_mutex_free(sb_mutex *m){if(m){pthread_mutex_destroy(&m->value);free(m);}}
void sb_platform_mutex_lock(sb_mutex *m){pthread_mutex_lock(&m->value);}
void sb_platform_mutex_unlock(sb_mutex *m){pthread_mutex_unlock(&m->value);}
char *sb_platform_default_data_dir(sb_error *e){(void)e;return sb_strdup("Data");}
char *sb_platform_program_directory(sb_error *e){(void)e;return getcwd(NULL,0);}
bool sb_platform_storage_path(const char *path,sb_error *e){return sb_posix_socket_preflight(path,e);}
bool sb_platform_prepare_runtime(const char *directory,sb_error *e){(void)directory;(void)e;return true;}
bool sb_posix_private(sb_root *r,sb_error *e){struct stat st;if(fstat(r->fd,&st)||st.st_uid!=geteuid()||(st.st_mode&077)){sb_fail(e,"Data directory must belong to this user with mode 0700.");return false;}return true;}
sb_lease *sb_platform_lease(const char *directory,sb_error *e){return sb_platform_lease_bound(directory,NULL,e);}
sb_lease *sb_platform_lease_bound(const char *directory,const char *expected,sb_error *e) {
    if(!sb_posix_socket_preflight(directory,e))return NULL;
    if(mkdir(directory,0700)&&errno!=EEXIST){sb_posix_error(e,"Create data directory");return NULL;}
    sb_root *r=sb_platform_root(directory,e);if(!r)return NULL;
    char actual[50];
    if(!sb_posix_private(r,e)||(expected&&(!sb_platform_root_identity(r,actual,e)||strcmp(expected,actual)))){sb_platform_root_close(r);return NULL;}
    int fd=openat(r->fd,"host.lock",O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK,0600);struct stat st;
    if(fd<0||!checked(fd,false,&st,e)||st.st_uid!=geteuid()||(st.st_mode&077)||flock(fd,LOCK_EX|LOCK_NB)) {
        if(fd>=0)close(fd);sb_platform_root_close(r);sb_fail(e,"Private data directory is unsafe or already owned by a running host.");return NULL;
    }
    sb_lease *l=calloc(1,sizeof(*l));if(!l){close(fd);sb_platform_root_close(r);sb_fail(e,"Out of memory.");return NULL;}
    l->root=r;l->lock=fd;
    if(pthread_mutex_init(&l->mutex,NULL)){close(fd);sb_platform_root_close(r);free(l);return NULL;}return l;
}
void sb_platform_lease_close(sb_lease *l){if(l){close(l->lock);sb_platform_root_close(l->root);pthread_mutex_destroy(&l->mutex);free(l);}}
sb_journal *sb_platform_journal_open(sb_lease *l,const char *name,sb_error *e){if(!name||strchr(name,'/')||!valid_relative(name,false)){sb_fail(e,"Invalid private journal name.");return NULL;}int fd=openat(l->root->fd,name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);if(fd<0){sb_posix_error(e,"Create private journal");return NULL;}sb_journal *j=malloc(sizeof(*j));if(!j){close(fd);sb_fail(e,"Out of memory.");return NULL;}j->fd=fd;return j;}
bool sb_platform_journal_append(sb_journal *j,const char *line,size_t length,sb_error *e){struct stat st;return checked(j->fd,false,&st,e)&&st.st_uid==geteuid()&&!(st.st_mode&077)&&sb_posix_write(j->fd,line,length,e)&&!fsync(j->fd);}
void sb_platform_journal_close(sb_journal *j){if(j){close(j->fd);free(j);}}
bool sb_platform_state_visit(sb_lease *l,sb_state_visitor visitor,void *ctx,sb_error *e) {
    sb_file dir={.fd=l->root->fd,.directory=true};sb_entry *entries=NULL;size_t n;bool more;
    if(!sb_platform_list(&dir,0,10000,&entries,&n,&more,e))return false;
    bool ok=true;size_t total=0,count=0;
    for(size_t i=0;i<n&&ok;i++) {
        const char *name=entries[i].name;size_t len=strlen(name);
        if(len!=44||strncmp(name,"state-",6)||strcmp(name+38,".jsonl"))continue;
        if(++count>256){sb_fail(e,"Retained state journal count exceeds 256.");ok=false;break;}
        int fd=openat(l->root->fd,name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);struct stat st;size_t length=0;char *bytes=NULL;
        if(fd<0||!checked(fd,false,&st,e)||st.st_uid!=geteuid()||(st.st_mode&077)){sb_fail(e,"Unsafe retained state journal.");ok=false;}
        else bytes=load_fd(fd,32u*1024u*1024u-total,&length,e);
        if(fd>=0)close(fd);
        if(ok){ok=bytes&&visitor(ctx,name,bytes,length,e);total+=length;}free(bytes);
    }
    sb_platform_entries_free(entries,n);return ok;
}

struct sb_create { int parent;char *leaf,*target;char identity[50],version[65];struct stat initial; };
sb_create *sb_platform_prepare_create(sb_root *root,const char *path,sb_error *e) {
    sb_create *p=calloc(1,sizeof(*p));if(!p){sb_fail(e,"Out of memory.");return NULL;}
    p->parent=parent(root,path,&p->leaf,e);p->target=sb_strdup(path);
    struct stat existing;
    if(p->parent<0||!p->target||fstat(p->parent,&p->initial)||fstatat(p->parent,p->leaf,&existing,AT_SYMLINK_NOFOLLOW)==0||errno!=ENOENT) {
        if(p->parent>=0)sb_fail(e,"Create destination must not exist.");sb_platform_create_close(p);return NULL;
    }
    identity(&p->initial,p->identity);version(&p->initial,p->version,e);return p;
}
const char *sb_platform_create_target(sb_create *p){return p->target;}
const char *sb_platform_create_identity(sb_create *p){return p->identity;}
bool sb_platform_create_version(sb_create *p,char out[65],sb_error *e){(void)e;memcpy(out,p->version,65);return true;}
static void discard_created(int parentfd,const char *leaf,int fd){struct stat a,b;if(!fstat(fd,&a)&&!fstatat(parentfd,leaf,&b,AT_SYMLINK_NOFOLLOW)&&same(&a,&b))unlinkat(parentfd,leaf,0);}
bool sb_platform_create_text(sb_create *p,const char *text,size_t length,sb_error *e) {
    if(!unchanged(p->parent,&p->initial,e))return false;
    int fd=openat(p->parent,p->leaf,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0){sb_posix_error(e,"Create new file");return false;}
    bool ok=!flock(fd,LOCK_EX|LOCK_NB)&&sb_posix_write(fd,text,length,e)&&!fsync(fd);if(!ok)discard_created(p->parent,p->leaf,fd);close(fd);
    if(ok&&fsync(p->parent)){sb_posix_error(e,"Flush created directory entry");return false;}return ok;
}
bool sb_platform_create_file(sb_create *p,sb_file *source,sb_error *e) {
    if(source->initial.st_size<0||(uint64_t)source->initial.st_size>SB_TRANSFER_LIMIT){sb_fail(e,"Copy source exceeds 64 MiB.");return false;}
    if(!unchanged(p->parent,&p->initial,e))return false;
    int fd=openat(p->parent,p->leaf,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0){sb_posix_error(e,"Create copy destination");return false;}
    bool ok=!flock(fd,LOCK_EX|LOCK_NB);uint64_t offset=0;unsigned char bytes[32768];
    while(ok&&offset<(uint64_t)source->initial.st_size){size_t n=0;ok=sb_platform_read(source,offset,bytes,sizeof(bytes),&n,e)&&n>0&&sb_posix_write(fd,bytes,n,e);offset+=n;}
    ok=ok&&unchanged(source->fd,&source->initial,e)&&!fsync(fd);
    if(!ok)discard_created(p->parent,p->leaf,fd);close(fd);if(ok)ok=!fsync(p->parent);return ok;
}
void sb_platform_create_close(sb_create *p){if(p){if(p->parent>=0)close(p->parent);free(p->leaf);free(p->target);free(p);}}
static bool backup_fd(sb_lease *l,sb_file *f,char id[33],sb_error *e) {
    pthread_mutex_lock(&l->mutex);
    if(l->backups>=64){pthread_mutex_unlock(&l->mutex);sb_fail(e,"Backup count limit reached for this host instance.");return false;}
    l->backups++;pthread_mutex_unlock(&l->mutex);
    unsigned char nonce[16];if(!sb_platform_random(nonce,sizeof(nonce),e))return false;
    for(unsigned i=0;i<16;i++)snprintf(id+i*2,3,"%02x",nonce[i]);
    char name[64];snprintf(name,sizeof(name),"backup-%s.bin",id);sb_journal *j=sb_platform_journal_open(l,name,e);if(!j)return false;
    bool ok=true;uint64_t offset=0;unsigned char bytes[32768];
    while(ok&&offset<(uint64_t)f->initial.st_size){size_t n=0;ok=sb_platform_read(f,offset,bytes,sizeof(bytes),&n,e)&&n>0&&sb_posix_write(j->fd,bytes,n,e);offset+=n;}
    ok=ok&&unchanged(f->fd,&f->initial,e)&&!fsync(j->fd)&&!fsync(l->root->fd);sb_platform_journal_close(j);return ok;
}
struct sb_modify { sb_file file;int parent;char *leaf,*target,*original;char parent_identity[50],identity[50];size_t length; };
sb_modify *sb_platform_prepare_modify(sb_root *root,const char *path,sb_error *e){return sb_platform_prepare_modify_guarded(root,path,NULL,e);}
sb_modify *sb_platform_prepare_modify_guarded(sb_root *root,const char *path,const char *expected,sb_error *e) {
    sb_modify *p=calloc(1,sizeof(*p));if(!p)return NULL;p->parent=-1;p->file.fd=-1;
    p->parent=parent(root,path,&p->leaf,e);p->target=sb_strdup(path);
    if(p->parent<0||!p->target)goto fail;
    p->file.fd=openat(p->parent,p->leaf,O_RDWR|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
    if(p->file.fd<0||!checked(p->file.fd,false,&p->file.initial,e)||flock(p->file.fd,LOCK_EX|LOCK_NB)){sb_fail(e,"Cannot lock a single-link regular file for editing.");goto fail;}
    struct stat ps;if(fstat(p->parent,&ps))goto fail;identity(&ps,p->parent_identity);identity(&p->file.initial,p->identity);
    char actual[65];if(expected&&(!version(&p->file.initial,actual,e)||strcmp(expected,actual))){sb_fail(e,"Approved file version changed.");goto fail;}
    p->original=load_fd(p->file.fd,SB_TEXT_LIMIT,&p->length,e);if(!p->original)goto fail;return p;
fail:sb_platform_modify_close(p);return NULL;
}
const char *sb_platform_modify_target(sb_modify *p){return p->target;}
const char *sb_platform_modify_parent(sb_modify *p){return p->parent_identity;}
const char *sb_platform_modify_identity(sb_modify *p){return p->identity;}
bool sb_platform_modify_version(sb_modify *p,char out[65],sb_error *e){return version(&p->file.initial,out,e);}
const char *sb_platform_modify_original(sb_modify *p,size_t *length){*length=p->length;return p->original;}
bool sb_platform_modify_backup(sb_modify *p,sb_lease *l,char id[33],sb_error *e){return backup_fd(l,&p->file,id,e);}
bool sb_platform_modify_text(sb_modify *p,const char *text,size_t length,sb_error *e) {
    struct stat named;
    if(!unchanged(p->file.fd,&p->file.initial,e)||fstatat(p->parent,p->leaf,&named,AT_SYMLINK_NOFOLLOW)||!same(&named,&p->file.initial)||named.st_nlink!=1){sb_fail(e,"Edit target changed after approval.");return false;}
    if(lseek(p->file.fd,0,SEEK_SET)<0||!sb_posix_write(p->file.fd,text,length,e)||ftruncate(p->file.fd,(off_t)length)||fsync(p->file.fd)){sb_posix_error(e,"Complete backed-up in-place edit");return false;}return true;
}
void sb_platform_modify_close(sb_modify *p){if(p){if(p->file.fd>=0)close(p->file.fd);if(p->parent>=0)close(p->parent);free(p->leaf);free(p->target);free(p->original);free(p);}}
bool sb_platform_target_version(sb_root *r,const char *path,bool directory,bool create,bool recursive,char out[65],sb_error *e){(void)recursive;if(create){sb_create *p=sb_platform_prepare_create(r,path,e);if(!p)return false;bool ok=sb_platform_create_version(p,out,e);sb_platform_create_close(p);return ok;}sb_file *f=sb_platform_open(r,path,directory,e);if(!f)return false;bool ok=sb_platform_file_version(f,out,e);sb_platform_close(f);return ok;}
struct sb_fs_plan {char operation[16];sb_root *root;char *path,*leaf;int parent;sb_file *source;sb_create *destination;sb_fs_info info;};
sb_fs_plan *sb_platform_fs_prepare(const char *op,sb_root *r,const char *path,bool directory,sb_root *dest,const char *destpath,sb_fs_info *info,char out[65],sb_error *e) {
    sb_fs_plan *p=calloc(1,sizeof(*p));if(!p)return NULL;p->parent=-1;p->root=r;p->path=sb_strdup(path);snprintf(p->operation,sizeof(p->operation),"%s",op);
    if(!p->path)goto fail;
    if(!strcmp(op,"fs_mkdir")){p->destination=sb_platform_prepare_create(r,path,e);p->info.directory=true;}
    else {
        p->source=sb_platform_open(r,path,directory,e);if(!p->source)goto fail;
        if(!sb_platform_file_version(p->source,p->info.version,e))goto fail;
        p->info.directory=directory;p->info.size=sb_platform_size(p->source);
        if(strcmp(op,"fs_stat")) {
            p->parent=parent(r,path,&p->leaf,e);if(p->parent<0)goto fail;
            if(!directory&&flock(p->source->fd,(!strcmp(op,"fs_copy")?LOCK_SH:LOCK_EX)|LOCK_NB)){sb_fail(e,"File is locked by another cooperating operation.");goto fail;}
            if(!directory&&p->info.size>SB_TRANSFER_LIMIT){sb_fail(e,"File operation exceeds 64 MiB.");goto fail;}
        }
        if(!strcmp(op,"fs_copy")||!strcmp(op,"fs_move")) {
            if(!strcmp(op,"fs_copy")&&directory){sb_fail(e,"Directory copies are unavailable.");goto fail;}
            p->destination=sb_platform_prepare_create(dest,destpath,e);
        }
    }
    if((!strcmp(op,"fs_mkdir")||!strcmp(op,"fs_copy")||!strcmp(op,"fs_move"))&&!p->destination)goto fail;
    if(!strcmp(op,"fs_mkdir")){if(!sb_platform_create_version(p->destination,out,e))goto fail;}
    else memcpy(out,p->info.version,65);
    /* Mutations bind destination parent identity/version as well as source. */
    if(p->source&&p->destination){char tuple[140];snprintf(tuple,sizeof(tuple),"%s|%s",out,p->destination->version);sb_platform_sha256(tuple,strlen(tuple),out,e);}
    *info=p->info;return p;
fail:sb_platform_fs_close(p);return NULL;
}
bool sb_platform_fs_execute(sb_fs_plan *p,sb_lease *l,sb_fs_backup_receipt receipt,void *context,char backup[33],sb_error *e) {
    if(!strcmp(p->operation,"fs_stat"))return true;
    if(p->source) {
        struct stat named;
        if(!unchanged(p->source->fd,&p->source->initial,e)||fstatat(p->parent,p->leaf,&named,AT_SYMLINK_NOFOLLOW)||!same(&named,&p->source->initial)){sb_fail(e,"File management target changed.");return false;}
    }
    if(p->destination&&!unchanged(p->destination->parent,&p->destination->initial,e))return false;
    if(!strcmp(p->operation,"fs_mkdir")) {
        if(mkdirat(p->destination->parent,p->destination->leaf,0700)||fsync(p->destination->parent)){sb_posix_error(e,"Create directory");return false;}return true;
    }
    if(!strcmp(p->operation,"fs_copy"))return sb_platform_create_file(p->destination,p->source,e);
    if(!strcmp(p->operation,"fs_delete")) {
        if(!p->info.directory&&(!backup_fd(l,p->source,backup,e)||(receipt&&!receipt(context,p->path,p->info.size,backup))))return false;
        if(unlinkat(p->parent,p->leaf,p->info.directory?AT_REMOVEDIR:0)||fsync(p->parent)){sb_posix_error(e,"Delete backed-up file or empty directory");return false;}return true;
    }
    if(!strcmp(p->operation,"fs_move")) {
#ifdef __APPLE__
        int rc=renameatx_np(p->parent,p->leaf,p->destination->parent,p->destination->leaf,RENAME_EXCL);
#else
        int rc=(int)syscall(SYS_renameat2,p->parent,p->leaf,p->destination->parent,p->destination->leaf,1u);
#endif
        if(rc||fsync(p->parent)||fsync(p->destination->parent)){sb_posix_error(e,"Move without replacement (same filesystem only)");return false;}return true;
    }
    sb_fail(e,"Unsupported file management operation.");return false;
}
void sb_platform_fs_close(sb_fs_plan *p){if(p){sb_platform_close(p->source);sb_platform_create_close(p->destination);if(p->parent>=0)close(p->parent);free(p->path);free(p->leaf);free(p);}}
sb_file *sb_platform_transfer_spool(sb_lease *l,sb_error *e){
    unsigned char nonce[16];char name[48]="transfer-";
    if(!l || !sb_platform_random(nonce,sizeof(nonce),e))return NULL;
    for(unsigned i=0;i<16;i++)snprintf(name+9+i*2,3,"%02x",nonce[i]);
    int fd=openat(l->root->fd,name,O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0){sb_posix_error(e,"Create private upload spool");return NULL;}
    /* Unlink immediately: no retained partial uploads, even after a crash. */
    if(unlinkat(l->root->fd,name,0)){sb_posix_error(e,"Unlink upload spool");close(fd);return NULL;}
    sb_file *f=calloc(1,sizeof(*f));
    if(!f){close(fd);sb_fail(e,"Out of memory.");return NULL;}
    f->fd=fd;
    if(fstat(fd,&f->initial)){sb_posix_error(e,"Inspect upload spool");sb_platform_close(f);return NULL;}
    return f;
}
bool sb_platform_spool_append(sb_file *f,const unsigned char *bytes,size_t length,sb_error *e){
    if(!f || !unchanged(f->fd,&f->initial,e) || f->initial.st_nlink ||
       f->initial.st_uid!=geteuid() || (f->initial.st_mode&077) ||
       (uint64_t)f->initial.st_size+length>SB_TRANSFER_LIMIT){sb_fail(e,"Invalid private upload spool or size.");return false;}
    return sb_posix_write(f->fd,bytes,length,e) && !fsync(f->fd) && !fstat(f->fd,&f->initial);
}
static bool export_one(const char *path,const char *bytes,size_t n,sb_error *e){
    char *leaf=NULL;sb_root *r=sb_platform_account_parent(path,&leaf,e);if(!r)return false;
    int fd=openat(r->fd,leaf,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);bool ok=fd>=0&&sb_posix_write(fd,bytes,n,e)&&!fsync(fd);
    if(fd>=0)close(fd);if(ok)ok=!fsync(r->fd);if(!ok&&!*e->text)sb_posix_error(e,"Export new private configuration");free(leaf);sb_platform_root_close(r);return ok;
}
bool sb_platform_export_config(const char *out,const char *backup,const char *json,size_t n,const char *original,size_t on,sb_error *e){
    if(!out||!backup||!strcmp(out,backup)){sb_fail(e,"Distinct new output and backup paths are required.");return false;}
    /* Keep a successfully flushed backup even if creating the new output fails. */
    return export_one(backup,original,on,e)&&export_one(out,json,n,e);
}
