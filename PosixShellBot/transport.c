#include "posix.h"
#include "sb_projects.h"
#include "civetweb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
int sb_stdio_run(sb_host *h){
    char *line=malloc(SB_REQUEST_LIMIT+1);if(!line)return 2;
    sb_protocol protocol={0};int result=0;
    while(!sb_posix_signal){size_t size=0;bool overflow=false;int c;
        while((c=fgetc(stdin))!=EOF&&c!='\n'){if(size<SB_REQUEST_LIMIT)line[size++]=(char)c;else overflow=true;}
        if(c==EOF&&!size&&!overflow)break;
        if(size&&line[size-1]=='\r')size--;if(!size&&!overflow)continue;
        sb_request_context ctx={.actor="stdio",.project=h->project,.policy_revision=h->project?h->project->policy_revision:0};size_t length;int status;
        char *reply=sb_dispatch(h,&protocol,line,overflow?SB_REQUEST_LIMIT+1:size,&ctx,&length,&status);
        if(reply){bool ok=fwrite(reply,1,length,stdout)==length&&fputc('\n',stdout)!=EOF&&!fflush(stdout);free(reply);if(!ok){result=1;break;}}
        else if(status!=202){result=1;break;}if(c==EOF)break;
    }
    if(ferror(stdin)&&!sb_posix_signal)result=1;free(line);return result;
}
typedef struct{sb_host *host;char *authorization;pthread_mutex_t serial;char error[256];} http_context;
static int send_reply(struct mg_connection *c,int status,const char *body){
    mg_disable_keep_alive(c);size_t n=body?strlen(body):0;
    mg_printf(c,"HTTP/1.1 %d %s\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: %zu\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n",status,status==200?"OK":status==202?"Accepted":"Request rejected",n);
    if(n)mg_write(c,body,n);return status;
}
static bool equal_secret(const char *a,const char *b){if(!a||strlen(a)!=strlen(b))return false;unsigned char diff=0;for(size_t i=0;b[i];i++)diff|=(unsigned char)(a[i]^b[i]);return !diff;}
static int request(struct mg_connection *c){
    const struct mg_request_info *r=mg_get_request_info(c);http_context *s=mg_get_user_data(mg_get_context(c));sb_host *h=s->host;
    const char *host=mg_get_header(c,"Host");char local[64],name[64];snprintf(local,sizeof(local),"127.0.0.1:%u",h->config.port);snprintf(name,sizeof(name),"localhost:%u",h->config.port);
    if(!host||(strcmp(host,local)&&strcmp(host,name)&&(!*h->config.public_authority||strcmp(host,h->config.public_authority)))||mg_get_header(c,"Origin")||r->query_string)return send_reply(c,403,"{\"error\":\"Invalid host, origin or query\"}");
    if(!strcmp(r->local_uri,"/healthz")&&!strcmp(r->request_method,"GET"))return send_reply(c,200,"{\"status\":\"ok\",\"platform_backend\":\"posix-phase1\"}");
    char endpoint[100];snprintf(endpoint,sizeof(endpoint),"/projects/%s/mcp",h->project?h->project->id:"");
    if(strcmp(r->local_uri,"/mcp")&&(!h->project||strcmp(r->local_uri,endpoint)))return send_reply(c,404,"{\"error\":\"Unknown endpoint\"}");
    unsigned authCount=0;for(int i=0;i<r->num_headers;i++)if(!strcasecmp(r->http_headers[i].name,"Authorization"))authCount++;
    if(authCount!=1||!equal_secret(mg_get_header(c,"Authorization"),s->authorization))return send_reply(c,401,"{\"error\":\"Bearer authentication required\"}");
    if(strcmp(r->request_method,"POST"))return send_reply(c,405,"{\"error\":\"POST required\"}");
    const char *type=mg_get_header(c,"Content-Type");if(!type||strncasecmp(type,"application/json",16)||(type[16]&&type[16]!=';'))return send_reply(c,415,"{\"error\":\"application/json required\"}");
    if(r->content_length<0||r->content_length>SB_REQUEST_LIMIT||mg_get_header(c,"Transfer-Encoding"))return send_reply(c,413,"{\"error\":\"A bounded Content-Length is required\"}");
    size_t n=(size_t)r->content_length,at=0;char *bytes=malloc(n+1);if(!bytes)return send_reply(c,500,NULL);
    while(at<n){int got=mg_read(c,bytes+at,n-at);if(got<=0){free(bytes);return send_reply(c,400,"{\"error\":\"Incomplete request\"}");}at+=(size_t)got;}bytes[n]=0;
    sb_request_context ctx={.actor="http-bearer",.project=h->project,.policy_revision=h->project?h->project->policy_revision:0,
        .version=mg_get_header(c,"MCP-Protocol-Version"),.method=mg_get_header(c,"MCP-Method"),.name=mg_get_header(c,"MCP-Name")};
    size_t length;int status;pthread_mutex_lock(&s->serial);char *reply=sb_dispatch(h,NULL,bytes,n,&ctx,&length,&status);pthread_mutex_unlock(&s->serial);free(bytes);
    int result=send_reply(c,status,reply);free(reply);return result;
}
static int log_message(const struct mg_connection *c,const char *message){if(c){http_context *s=mg_get_user_data(mg_get_context(c));if(s)snprintf(s->error,sizeof(s->error),"%s",message);}return 1;}
int sb_http_run(sb_host *h,sb_error *e){
    const char *token=getenv(h->config.token_env);if(!token||strlen(token)<32||strlen(token)>128||strspn(token,"0123456789abcdefABCDEF")!=strlen(token)){sb_fail(e,"HTTP requires a 32..128 character hexadecimal bearer token in the configured token_env.");return 2;}
    http_context s={.host=h};size_t auth_size=strlen(token)+8;s.authorization=malloc(auth_size);if(!s.authorization)return 2;snprintf(s.authorization,auth_size,"Bearer %s",token);pthread_mutex_init(&s.serial,NULL);
    char address[64];snprintf(address,sizeof(address),"127.0.0.1:%u",h->config.port);
    const char *options[]={"listening_ports",address,"num_threads","4","request_timeout_ms","5000","enable_keep_alive","no","max_request_size","16384",NULL};
    struct mg_callbacks callbacks={.begin_request=request,.log_message=log_message};mg_init_library(0);
    struct mg_context *server=mg_start(&callbacks,&s,options);
    if(!server){snprintf(e->text,sizeof(e->text),"Cannot bind loopback HTTP on port %u: %.160s",h->config.port,*s.error?s.error:"native listener initialization failed");}
    else{fprintf(stderr,"ShellBot: listening on %s\n",address);while(!sb_posix_signal&&!sb_owner_stopping(h->owner)){struct timespec t={.tv_nsec=100000000};nanosleep(&t,NULL);}mg_stop(server);}
    mg_exit_library();pthread_mutex_destroy(&s.serial);memset(s.authorization,0,strlen(s.authorization));free(s.authorization);return server?0:2;
}
