#include "posix.h"
#ifdef SB_NATIVE_DESKTOP
#include "native.h"
#endif
#include "sb_projects.h"
#include "credentials.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
volatile sig_atomic_t sb_posix_signal=0;
static void interrupt(int signal_number){sb_posix_signal=signal_number;}
static void usage(void){puts("ShellBot POSIX phase 4 (C17)\n"
    "  ShellBot --desktop [--data-dir PRIVATE_DESKTOP_DIR] [--credential-key-file KEY]\n"
    "  ShellBot --stdio|--http --config FILE [--data-dir ABSOLUTE_DIR] [--port 1..65535]\n"
    "  ShellBot --stdio|--http --projects REGISTRY --project ID [--port 1..65535]\n"
    "  ShellBot --stdio --root ABSOLUTE_DIR --data-dir ABSOLUTE_DIR (read-only)\n"
    "  ShellBot --validate-projects REGISTRY | --generate-token | --version | --help\n"
    "  ShellBot --list-approvals|--stop --data-dir ABSOLUTE_DIR\n"
    "  ShellBot --inspect|--approve|--deny|--revoke ID --data-dir ABSOLUTE_DIR\n"
    "  ShellBot --owner-request FILE --data-dir ABSOLUTE_DIR\n"
    "One fixed project per process. HTTP binds only 127.0.0.1; token_env supplies bearer.\n"
    "Data must be a private user-owned 0700 directory, outside all configured roots.\n"
    "Owner IPC requires the same native uid. Native management UI is optional; desktop control remains unavailable.\n"
    "  ShellBot --manager [--http] --projects REGISTRY --manager-data-dir ABS_DIR [--start-project ID ...]\n"
    "  ShellBot --management-status|--manager-stop --manager-data-dir ABS_DIR\n"
    "  ShellBot --project-status|--project-start|--project-stop|--project-pause|--project-resume ID --manager-data-dir ABS_DIR\n"
    "  ShellBot --project-owner ID --request FILE --manager-data-dir ABS_DIR\n"
    "  ShellBot --project-list REGISTRY\n"
    "  ShellBot --project-create DEFINITION --projects REGISTRY --output NEW --backup NEW_BACKUP\n"
    "  ShellBot --project-configure ID --project-definition FILE --projects REGISTRY --output NEW --backup NEW_BACKUP\n"
    "  ShellBot --project-remove ID --projects REGISTRY --output NEW --backup NEW_BACKUP\n"
    "  ShellBot --manager --http --oauth --projects REGISTRY --manager-data-dir ABS_DIR [--credential-key-file ABS_KEY]\n"
    "  ShellBot --credential-key-create ABS_KEY (new 64-byte key in a separate private 0700 directory)\n"
    "  ShellBot --credential-set|--credential-generate ID --token-env NAME --manager-data-dir ABS_DIR [--credential-key-file ABS_KEY]\n"
    "  ShellBot --credential-status|--credential-remove ID|--credential-export ID --output NEW --manager-data-dir ABS_DIR [--credential-key-file ABS_KEY]\n"
    "  ShellBot --oauth-status|--oauth-approve ID [--persistent]|--oauth-deny ID|--oauth-revoke ID|--oauth-remove-client ID --manager-data-dir ABS_DIR\n"
    "Native credential storage uses an unlocked macOS Keychain or Linux Secret Service. No automatic plaintext fallback.\n"
    "macOS may select an existing private --credential-keychain FILE. Credential import reads one token from stdin.\n"
    "OAuth requires an explicit HTTPS public_url; TLS/tunnel configuration remains with the owner.\n"
    "--persistent approves ongoing access under the project's current and future permissions until explicit revocation.\n"
    "Ordinary commands require explicit current-user writable scope. No command sandbox is claimed.");}
static bool port_number(const char *text,unsigned *out){if(!text||!*text||strspn(text,"0123456789")!=strlen(text))return false;unsigned long v=strtoul(text,NULL,10);if(v<1||v>65535)return false;*out=(unsigned)v;return true;}
int main(int argc,char **argv){
    umask(077);struct sigaction action={0};action.sa_handler=interrupt;sigemptyset(&action.sa_mask);sigaction(SIGINT,&action,NULL);sigaction(SIGTERM,&action,NULL);signal(SIGPIPE,SIG_IGN);
    sb_posix_set_program(argv[0]);
#ifdef SB_NATIVE_DESKTOP
    if(argc==1||!strcmp(argv[1],"--desktop"))return sb_native_main(argc,argv);
#ifdef SHELLBOT_TESTS
    if(argc>=3&&!strcmp(argv[1],"--desktop-driver")){
        sb_error e={0};sb_desktop *g=sb_desktop_open(argv[2],argc==4?argv[3]:NULL,&e);
        if(!g){fprintf(stderr,"%s\n",e.text);return 2;}
        char *line=NULL;size_t capacity=0;
        while(getline(&line,&capacity,stdin)>0){char *reply=sb_desktop_work(g,line);if(!reply)break;puts(reply);fflush(stdout);free(reply);}
        free(line);sb_desktop_close(g);return 0;
    }
#endif
#endif
    for(int i=1;i<argc;i++)if(!strcmp(argv[i],"--project-worker")){if(!sb_posix_peer(3,true))return 2;fcntl(3,F_SETFD,FD_CLOEXEC);sb_posix_close_inherited();break;}
    if(argc==2&&!strcmp(argv[1],"--task-agent"))return sb_posix_task_agent(3);
    for(int i=1;i<argc;i++)if(sb_credential_option(argv[i]))return sb_credential_cli(argc,argv);
    for(int i=1;i<argc;i++)if(sb_posix_manager_option(argv[i]))return sb_posix_manager_cli(argc,argv);
    sb_error error={0};
    if(argc==2&&!strcmp(argv[1],"--help")){usage();return 0;}
    if(argc==2&&!strcmp(argv[1],"--version")){puts("ShellBot " SB_VERSION " (C17; POSIX phase 4)");return 0;}
    if(argc==2&&!strcmp(argv[1],"--generate-token")){unsigned char b[32];if(!sb_platform_random(b,sizeof(b),&error))return 2;for(unsigned i=0;i<32;i++)printf("%02x",b[i]);puts("");memset(b,0,sizeof(b));return 0;}
    sb_host host={0};sb_config_defaults(&host.config);host.started_ms=sb_platform_millis();sb_project_registry registry={0};
    const char *config=NULL,*root=NULL,*data=NULL,*projects=NULL,*project_id=NULL,*port=NULL,*command=NULL,*id=NULL,*requestfile=NULL;
    bool http=false,transport=false,validate=false,worker=false;int result=2;
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"--project-worker")){if(worker)goto bad;worker=true;}
        else if(!strcmp(argv[i],"--oauth-capable")){if(host.oauth_backend)goto bad;host.oauth_backend=true;}
        else if(!strcmp(argv[i],"--http")||!strcmp(argv[i],"--stdio")){if(transport)goto bad;transport=true;http=!strcmp(argv[i],"--http");}
        else if(!strcmp(argv[i],"--list-approvals")||!strcmp(argv[i],"--stop")){if(command)goto bad;command=!strcmp(argv[i],"--stop")?"stop":"list";}
        else if(!strcmp(argv[i],"--inspect")||!strcmp(argv[i],"--approve")||!strcmp(argv[i],"--deny")||!strcmp(argv[i],"--revoke")){
            if(command||i+1>=argc)goto bad;command=argv[i]+2;id=argv[++i];if(strlen(id)!=32||strspn(id,"0123456789abcdef")!=32)goto bad;
        }else{
            const char **slot=!strcmp(argv[i],"--config")?&config:!strcmp(argv[i],"--root")?&root:!strcmp(argv[i],"--data-dir")?&data:
                !strcmp(argv[i],"--projects")?&projects:!strcmp(argv[i],"--project")?&project_id:!strcmp(argv[i],"--port")?&port:
                !strcmp(argv[i],"--owner-request")?&requestfile:!strcmp(argv[i],"--validate-projects")?&projects:NULL;
            if(!slot||*slot||i+1>=argc||!*argv[i+1])goto bad;if(!strcmp(argv[i],"--validate-projects"))validate=true;*slot=argv[++i];
        }
    }
    if(validate){if(argc!=3)goto bad;if(!sb_projects_load(&registry,projects,&error))goto done;char *summary=sb_projects_summary(&registry);if(summary){puts(summary);free(summary);result=0;}goto done;}
    if(command||requestfile){if(!data||config||root||projects||project_id||port||transport||(command&&requestfile))goto bad;
        if(command)result=sb_owner_command(data,command,id,&error);
        else{size_t n;char *bytes=sb_platform_load_config(requestfile,&n,&error),*reply=bytes?sb_owner_exchange(data,bytes,n,&error):NULL;free(bytes);if(reply){puts(reply);yyjson_doc *d=yyjson_read(reply,strlen(reply),0);result=d&&!yyjson_obj_get(yyjson_doc_get_root(d),"error")?0:2;yyjson_doc_free(d);free(reply);}}
        goto done;
    }
    if((projects&&!project_id)||(!projects&&project_id)||(config&&(root||projects))||(root&&projects))goto bad;
    if(host.oauth_backend&&!worker)goto bad;
    if(projects){
        if(!sb_projects_load(&registry,projects,&error))goto done;host.project=sb_project_find(&registry,project_id);
        if(!host.project||!host.project->enabled){sb_fail(&error,"The selected fixed project is missing or disabled.");goto done;}
        if(host.project->legacy_compatibility||host.project->policy.administrator_allowed||host.project->policy.desktop.enabled||(host.project->policy.execution_enabled&&host.project->policy.default_identity!=SB_ID_CURRENT_USER)){sb_fail(&error,"POSIX projects support ordinary current-user commands; legacy projects, restricted commands, administrator and desktop are unavailable.");goto done;}
        host.config.execution_enabled=host.project->policy.execution_enabled;host.config.execution_profile=SB_EXEC_USER;
        host.config.port=registry.port;host.config.approval_ttl_seconds=host.project->approval_ttl_seconds;host.config.audit_max_bytes=host.project->audit_max_bytes;host.config.limits=host.project->limits;
        strcpy(host.config.token_env,host.project->token_env);strcpy(host.config.public_origin,registry.public_origin);
        if(*registry.public_origin&&!sb_config_public_url(&host.config,registry.public_origin,&error))goto done;
        host.config.data_dir=sb_strdup(host.project->data_dir);
        for(size_t i=0;i<host.project->directory_count;i++){const sb_project_directory *d=&host.project->directories[i];if(!sb_config_root(&host.config,d->id,d->path,&error))goto done;host.config.workspaces[host.config.workspace_count-1].read_only=d->read_only;}
    }else if(config){if(!sb_config_load(&host.config,config,&error))goto done;}
    else if(root&&!sb_config_root(&host.config,"workspace",root,&error))goto done;
    if(data){if(projects){sb_fail(&error,"Project data_dir is fixed by its registry; do not override it.");goto done;}free(host.config.data_dir);host.config.data_dir=sb_strdup(data);}
    if(!host.config.data_dir||!sb_platform_storage_path(host.config.data_dir,&error)){sb_fail(&error,"Set an absolute private data_dir explicitly.");goto done;}
    if(port&&!port_number(port,&host.config.port))goto bad;
    host.http_mode=http;host.lease=sb_platform_lease(host.config.data_dir,&error);if(!host.lease)goto done;
    for(size_t i=0;i<host.config.workspace_count;i++)if(sb_platform_roots_overlap(host.lease->root,host.config.workspaces[i].root)){sb_fail(&error,"Private data directory must not overlap an exposed file root.");goto done;}
    for(size_t i=0;i<host.config.workspace_count;i++)for(size_t j=0;j<i;j++)if(sb_platform_roots_overlap(host.config.workspaces[i].root,host.config.workspaces[j].root)){sb_fail(&error,"Configured roots physically overlap or alias one another.");goto done;}
    host.execution=sb_execution_new(&host.config,&error);if(!host.execution)goto done;
    host.audit=sb_audit_new(host.lease,host.config.audit_max_bytes,&error);if(!host.audit)goto done;
    host.policy=sb_policy_new(host.config.approval_ttl_seconds,host.audit,&error);if(!host.policy)goto done;
    if(host.project){host.store=sb_store_new(host.lease,host.project->id,host.project->policy_revision,sb_execution_instance(host.execution),"ordinary",&error);if(!host.store)goto done;sb_policy_store(host.policy,host.store);sb_policy_limit(host.policy,host.config.limits.approval_records);}
    if(!sb_posix_execution_bind(host.execution,&host,&error))goto done;
    host.owner=sb_owner_start(&host,&error);if(!host.owner)goto done;
    if(worker&&(!projects||transport||port)){sb_fail(&error,"Invalid private worker invocation.");goto done;}
    result=worker?sb_posix_worker_run(&host,3):http?sb_http_run(&host,&error):sb_stdio_run(&host);goto done;
bad:sb_fail(&error,"Invalid, duplicate or incompatible arguments. Use --help.");
done:
    if(result&&*error.text)fprintf(stderr,"ShellBot: %s\n",error.text);
    sb_owner_stop(host.owner);sb_execution_free(host.execution);sb_policy_free(host.policy);sb_store_free(host.store);sb_audit_free(host.audit);sb_platform_lease_close(host.lease);sb_config_free(&host.config);sb_projects_free(&registry);return result;
}
