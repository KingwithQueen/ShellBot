#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include "sandbox.h"
#include "execution.h"
#include <userenv.h>
#include <aclapi.h>
#include <sddl.h>
#include <stdlib.h>
#include <stdio.h>
#include <wchar.h>
#include <string.h>

#define SANDBOX_OBJECTS 1024
struct sb_win_sandbox {
    PSID sid; wchar_t profile[64]; char sid_text[192];
    HANDLE objects[SANDBOX_OBJECTS]; bool trees[SANDBOX_OBJECTS]; unsigned count;
    wchar_t *programs[17]; unsigned program_count;
    sb_journal *journal; bool profile_created;
    SECURITY_CAPABILITIES capabilities;
    DWORD packages_policy, children_policy;
    DWORD64 mitigation;
};
static wchar_t *wide(const char *text) {
    int n=text?MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text,-1,NULL,0):0;
    wchar_t *out=n?calloc((size_t)n,sizeof(*out)):NULL;
    if(out)MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text,-1,out,n);return out;
}
static char *utf8(const wchar_t *text) {
    int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text,-1,NULL,0,NULL,NULL);
    char *out=n?malloc((size_t)n):NULL;if(out)WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text,-1,out,n,NULL,NULL);return out;
}
static wchar_t *final_path(HANDLE file) {
    wchar_t *out=calloc(SB_PATH_CAP,sizeof(*out));
    DWORD n=out?GetFinalPathNameByHandleW(file,out,SB_PATH_CAP,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS):0;
    if(!n || n>=SB_PATH_CAP){free(out);return NULL;}return out;
}
static bool object_id(HANDLE file,char id[50]) {
    FILE_ID_INFO info={0};if(!GetFileInformationByHandleEx(file,FileIdInfo,&info,sizeof(info)))return false;
    snprintf(id,50,"%016llx:",(unsigned long long)info.VolumeSerialNumber);
    for(size_t i=0;i<16;++i)snprintf(id+17+i*2,3,"%02x",info.FileId.Identifier[i]);return true;
}
static bool journal_record(sb_win_sandbox *sandbox,const char *path,const char *id,bool tree,bool complete,sb_error *error) {
    yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL);if(!doc)return false;
    yyjson_mut_val *root=yyjson_mut_obj(doc);yyjson_mut_doc_set_root(doc,root);
    char *profile=utf8(sandbox->profile);
    yyjson_mut_obj_add_strcpy(doc,root,"profile",profile);yyjson_mut_obj_add_strcpy(doc,root,"sid",sandbox->sid_text);
    free(profile);
    if(path){yyjson_mut_obj_add_strcpy(doc,root,"path",path);yyjson_mut_obj_add_strcpy(doc,root,"object_id",id);}
    if(tree)yyjson_mut_obj_add_bool(doc,root,"tree",true);
    if(complete)yyjson_mut_obj_add_bool(doc,root,"complete",true);
    size_t size=0;char *line=yyjson_mut_write(doc,0,&size);yyjson_mut_doc_free(doc);
    bool ok=line && sb_platform_journal_append(sandbox->journal,line,size,error) && sb_platform_journal_append(sandbox->journal,"\n",1,error);
    free(line);return ok;
}
static bool set_dacl(HANDLE handle,PACL acl) {
    SECURITY_DESCRIPTOR descriptor;
    return InitializeSecurityDescriptor(&descriptor,SECURITY_DESCRIPTOR_REVISION) &&
        SetSecurityDescriptorDacl(&descriptor,TRUE,acl,FALSE) &&
        SetKernelObjectSecurity(handle,DACL_SECURITY_INFORMATION,&descriptor);
}
static bool remove_sid(HANDLE handle,PSID sid) {
    PACL acl=NULL;PSECURITY_DESCRIPTOR descriptor=NULL;
    DWORD result=GetSecurityInfo(handle,SE_FILE_OBJECT,DACL_SECURITY_INFORMATION,NULL,NULL,&acl,NULL,&descriptor);
    bool changed=false;
    if(result==ERROR_SUCCESS && acl) {
        for(DWORD i=acl->AceCount;i;--i) {
            ACE_HEADER *ace=NULL;if(!GetAce(acl,i-1,(void **)&ace)){result=ERROR_INVALID_ACL;break;}
            if(ace->AceType==ACCESS_ALLOWED_ACE_TYPE && EqualSid(sid,&((ACCESS_ALLOWED_ACE *)ace)->SidStart)) {
                if(!DeleteAce(acl,i-1)){result=GetLastError();break;}changed=true;
            }
        }
        if(result==ERROR_SUCCESS && changed && !set_dacl(handle,acl))result=GetLastError();
    }
    LocalFree(descriptor);return result==ERROR_SUCCESS;
}
static bool grant_tree(sb_win_sandbox *sandbox,const wchar_t *path,bool read_only,bool directory,unsigned depth,sb_error *error) {
    if(sandbox->count>=SANDBOX_OBJECTS || depth>16){sb_fail(error,"Sandbox exceeds 1024 objects or 16 directory levels.");return false;}
    HANDLE file=CreateFileW(path,READ_CONTROL|WRITE_DAC|FILE_READ_ATTRIBUTES|(directory?FILE_LIST_DIRECTORY:GENERIC_READ),
        FILE_SHARE_READ|(directory?FILE_SHARE_WRITE:0),NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    if(file==INVALID_HANDLE_VALUE){sb_fail(error,"Cannot pin sandbox resource and its permissions.");return false;}
    BY_HANDLE_FILE_INFORMATION info={0};
    bool ok=GetFileInformationByHandle(file,&info) && !(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) &&
        ((info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0)==directory && (directory || info.nNumberOfLinks==1);
    wchar_t *actual=ok?final_path(file):NULL;char id[50]={0};ok=ok && actual && object_id(file,id);
    if(!ok){CloseHandle(file);free(actual);sb_fail(error,"Sandbox resources require regular single-link physical objects.");return false;}
    /* Descendants are journaled before parent inheritance can alter their ACLs. */
    if(directory) {
        size_t n=wcslen(actual)+3;wchar_t *pattern=calloc(n,sizeof(*pattern));WIN32_FIND_DATAW entry;
        if(pattern)swprintf(pattern,n,L"%ls\\*",actual);
        HANDLE search=pattern?FindFirstFileW(pattern,&entry):INVALID_HANDLE_VALUE;free(pattern);
        if(search!=INVALID_HANDLE_VALUE) {
            do {
                if(!wcscmp(entry.cFileName,L".") || !wcscmp(entry.cFileName,L".."))continue;
                if(entry.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT){ok=false;sb_fail(error,"Restricted activation refuses reparse points in directory grants.");break;}
                size_t size=wcslen(actual)+wcslen(entry.cFileName)+2;wchar_t *child=calloc(size,sizeof(*child));
                if(child)swprintf(child,size,L"%ls\\%ls",actual,entry.cFileName);
                ok=child && grant_tree(sandbox,child,read_only,(entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0,depth+1,error);free(child);
                if(!ok)break;
            } while(FindNextFileW(search,&entry));
            DWORD last=GetLastError();FindClose(search);if(ok && last!=ERROR_NO_MORE_FILES)ok=false;
        } else if(GetLastError()!=ERROR_FILE_NOT_FOUND)ok=false;
    }
    PACL old=NULL,acl=NULL;PSECURITY_DESCRIPTOR descriptor=NULL;
    if(ok)ok=GetSecurityInfo(file,SE_FILE_OBJECT,DACL_SECURITY_INFORMATION,NULL,NULL,&old,NULL,&descriptor)==ERROR_SUCCESS && old;
    char *text=actual?utf8(actual):NULL;
    if(ok)ok=text && journal_record(sandbox,text,id,directory && depth==0,false,error);
    EXPLICIT_ACCESSW entry={0};entry.grfAccessPermissions=FILE_GENERIC_READ|FILE_GENERIC_EXECUTE|(read_only?0:FILE_GENERIC_WRITE|DELETE);
    entry.grfAccessMode=GRANT_ACCESS;entry.grfInheritance=directory?SUB_CONTAINERS_AND_OBJECTS_INHERIT:NO_INHERITANCE;
    entry.Trustee.TrusteeForm=TRUSTEE_IS_SID;entry.Trustee.ptstrName=(LPWSTR)sandbox->sid;
    if(ok)ok=SetEntriesInAclW(1,&entry,old,&acl)==ERROR_SUCCESS &&
        set_dacl(file,acl);
    LocalFree(descriptor);LocalFree(acl);free(text);
    if(ok) {
        /* Pin immutable runtime binaries; workspace handles must allow writes and deletion. */
        if(directory || !read_only || depth>0) { CloseHandle(file);file=CreateFileW(actual,READ_CONTROL|WRITE_DAC|FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL); }
        if(file!=INVALID_HANDLE_VALUE){sandbox->trees[sandbox->count]=directory && depth==0;sandbox->objects[sandbox->count++]=file;}else ok=false;
    }
    if(!ok && file!=INVALID_HANDLE_VALUE)CloseHandle(file);
    free(actual);if(!ok && !*error->text)sb_fail(error,"Cannot grant journaled sandbox resource permissions.");return ok;
}
bool sb_win_sandbox_attributes(sb_win_sandbox *sandbox,LPPROC_THREAD_ATTRIBUTE_LIST attributes) {
    return UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES,&sandbox->capabilities,sizeof(sandbox->capabilities),NULL,NULL) &&
        UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_ALL_APPLICATION_PACKAGES_POLICY,&sandbox->packages_policy,sizeof(sandbox->packages_policy),NULL,NULL) &&
        UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY,&sandbox->mitigation,sizeof(sandbox->mitigation),NULL,NULL) &&
        UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY,&sandbox->children_policy,sizeof(sandbox->children_policy),NULL,NULL);
}
static bool lpac_access_check(HANDLE token,PSID sid) {
    HANDLE duplicate=NULL;bool ok=DuplicateTokenEx(token,TOKEN_QUERY|TOKEN_IMPERSONATE,NULL,SecurityImpersonation,TokenImpersonation,&duplicate)!=FALSE;
    wchar_t *text_sid=NULL;PSECURITY_DESCRIPTOR descriptor=NULL;
    if(ok)ok=ConvertSidToStringSidW(sid,&text_sid)!=FALSE;
    for(unsigned pass=0;ok && pass<2;++pass) {
        wchar_t text[320];swprintf(text,320,L"O:SYG:SYD:(A;;FR;;;WD)(A;;FR;;;%ls)",pass?text_sid:L"AC");
        ok=ConvertStringSecurityDescriptorToSecurityDescriptorW(text,SDDL_REVISION_1,&descriptor,NULL)!=FALSE;
        GENERIC_MAPPING mapping={FILE_GENERIC_READ,FILE_GENERIC_WRITE,FILE_GENERIC_EXECUTE,FILE_ALL_ACCESS};
        unsigned char privileges[1024];DWORD size=sizeof(privileges),granted=0;BOOL allowed=FALSE;
        if(ok)ok=AccessCheck(descriptor,duplicate,FILE_GENERIC_READ,&mapping,(PRIVILEGE_SET *)privileges,&size,&granted,&allowed) && (allowed!=FALSE)==(pass!=0);
        LocalFree(descriptor);descriptor=NULL;
    }
    LocalFree(text_sid);if(duplicate)CloseHandle(duplicate);return ok;
}
bool sb_win_sandbox_token(sb_win_sandbox *sandbox,HANDLE process) {
    HANDLE token=NULL;DWORD bytes=0,ac=0;TOKEN_ELEVATION elevation={0};
    unsigned char buffer[512];TOKEN_APPCONTAINER_INFORMATION *container=(TOKEN_APPCONTAINER_INFORMATION *)buffer;
    bool ok=OpenProcessToken(process,TOKEN_QUERY|TOKEN_DUPLICATE,&token) &&
        GetTokenInformation(token,TokenIsAppContainer,&ac,sizeof(ac),&bytes) && ac &&
        GetTokenInformation(token,TokenElevation,&elevation,sizeof(elevation),&bytes) && !elevation.TokenIsElevated &&
        GetTokenInformation(token,TokenAppContainerSid,buffer,sizeof(buffer),&bytes) && EqualSid(container->TokenAppContainer,sandbox->sid) &&
        !sb_win_owner_token_allowed(token);
    TOKEN_GROUPS *caps=(TOKEN_GROUPS *)buffer;
    if(ok)ok=GetTokenInformation(token,TokenCapabilities,buffer,sizeof(buffer),&bytes) && caps->GroupCount==0 && lpac_access_check(token,sandbox->sid);
    if(token)CloseHandle(token);return ok;
}
bool sb_win_sandbox_program(sb_win_sandbox *sandbox,const wchar_t *path) {
    HANDLE file=CreateFileW(path,FILE_READ_ATTRIBUTES,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    if(file==INVALID_HANDLE_VALUE)return false;wchar_t *actual=final_path(file);CloseHandle(file);
    bool matched=false;for(unsigned i=1;actual && i<sandbox->program_count;++i)if(!wcscmp(actual,sandbox->programs[i]))matched=true;
    free(actual);return matched;
}
static bool clean_tree(const wchar_t *path,PSID sid,unsigned depth,unsigned *count) {
    if(++*count>10000 || depth>64)return false;
    HANDLE file=CreateFileW(path,READ_CONTROL|WRITE_DAC|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    if(file==INVALID_HANDLE_VALUE)return GetLastError()==ERROR_FILE_NOT_FOUND || GetLastError()==ERROR_PATH_NOT_FOUND;
    BY_HANDLE_FILE_INFORMATION info={0};bool ok=GetFileInformationByHandle(file,&info)!=FALSE;
    if(ok && (info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)){ok=remove_sid(file,sid);CloseHandle(file);return ok;}
    wchar_t *actual=ok?final_path(file):NULL;
    if(ok && actual && (info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)) {
        size_t n=wcslen(actual)+3;wchar_t *pattern=calloc(n,sizeof(*pattern));if(pattern)swprintf(pattern,n,L"%ls\\*",actual);WIN32_FIND_DATAW entry;
        HANDLE search=pattern?FindFirstFileW(pattern,&entry):INVALID_HANDLE_VALUE;free(pattern);
        if(search!=INVALID_HANDLE_VALUE) {
            do {
                if(!wcscmp(entry.cFileName,L".") || !wcscmp(entry.cFileName,L".."))continue;
                size_t capacity=wcslen(actual)+wcslen(entry.cFileName)+2;wchar_t *child=calloc(capacity,sizeof(*child));if(child)swprintf(child,capacity,L"%ls\\%ls",actual,entry.cFileName);
                bool removed=child && clean_tree(child,sid,depth+1,count);free(child);if(!removed){ok=false;break;}
            }while(FindNextFileW(search,&entry));
            DWORD last=GetLastError();FindClose(search);if(ok && last!=ERROR_NO_MORE_FILES)ok=false;
        }else if(GetLastError()!=ERROR_FILE_NOT_FOUND)ok=false;
    }
    if(ok)ok=remove_sid(file,sid);free(actual);CloseHandle(file);return ok;
}
void sb_win_sandbox_free(sb_win_sandbox *sandbox) {
    if(!sandbox)return;bool complete=true;
    for(unsigned i=sandbox->count;i;--i){
        if(sandbox->trees[i-1]){wchar_t *path=final_path(sandbox->objects[i-1]);unsigned count=0;if(!path || !clean_tree(path,sandbox->sid,0,&count))complete=false;free(path);}
        if(!remove_sid(sandbox->objects[i-1],sandbox->sid))complete=false;CloseHandle(sandbox->objects[i-1]);
    }
    if(sandbox->profile_created && FAILED(DeleteAppContainerProfile(sandbox->profile)))complete=false;
    if(complete && sandbox->journal){sb_error ignored={0};journal_record(sandbox,NULL,NULL,false,true,&ignored);}
    sb_platform_journal_close(sandbox->journal);
    for(unsigned i=0;i<sandbox->program_count;++i)free(sandbox->programs[i]);
    if(sandbox->sid)FreeSid(sandbox->sid);free(sandbox);
}
static bool verify(sb_win_sandbox *sandbox,sb_host *host,const wchar_t *probe,const char *instance,sb_error *error) {
    WSADATA network;if(WSAStartup(MAKEWORD(2,2),&network))return false;
    SOCKET listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);struct sockaddr_in address={0};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    int address_size=sizeof(address);bool ok=listener!=INVALID_SOCKET && !bind(listener,(struct sockaddr *)&address,sizeof(address)) && !listen(listener,1) && !getsockname(listener,(struct sockaddr *)&address,&address_size);
    sb_file *root=sb_platform_open(host->config.workspaces[0].root,".",true,error);char *root_text=root?sb_platform_directory_path(root,error):NULL;sb_platform_close(root);
    wchar_t *workspace=wide(root_text),*data=wide(host->config.data_dir);free(root_text);wchar_t *readonly=NULL;
    for(size_t i=0;i<host->config.workspace_count;++i)if(host->config.workspaces[i].read_only){root=sb_platform_open(host->config.workspaces[i].root,".",true,error);char *text=root?sb_platform_directory_path(root,error):NULL;readonly=wide(text);free(text);sb_platform_close(root);break;}
    size_t capacity=workspace && data ? wcslen(probe)+wcslen(workspace)+wcslen(data)+(readonly?wcslen(readonly):0)+256:0;
    wchar_t *line=capacity?calloc(capacity,sizeof(*line)):NULL;
    wchar_t journal_name[80];swprintf(journal_name,80,L"sandbox-%hs.jsonl",instance);
    if(line)swprintf(line,capacity,L"\"%ls\" \"%ls\" %u \"%ls\\%ls\" \"%ls\" %lu %u",probe,workspace,!host->config.workspaces[0].read_only,data,journal_name,readonly?readonly:L"",GetCurrentProcessId(),ntohs(address.sin_port));
    SIZE_T size=0;InitializeProcThreadAttributeList(NULL,4,0,&size);LPPROC_THREAD_ATTRIBUTE_LIST attributes=size?malloc(size):NULL;
    bool initialized=attributes && InitializeProcThreadAttributeList(attributes,4,0,&size);ok=ok && initialized && line && sb_win_sandbox_attributes(sandbox,attributes);
    STARTUPINFOEXW startup={0};startup.StartupInfo.cb=sizeof(startup);startup.lpAttributeList=attributes;PROCESS_INFORMATION process={0};
    HANDLE job=CreateJobObjectW(NULL,NULL);JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits={0};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE|JOB_OBJECT_LIMIT_ACTIVE_PROCESS;limits.BasicLimitInformation.ActiveProcessLimit=1;
    ok=ok && job && SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits)) &&
        CreateProcessW(probe,line,NULL,NULL,FALSE,CREATE_SUSPENDED|DETACHED_PROCESS|EXTENDED_STARTUPINFO_PRESENT,NULL,workspace,&startup.StartupInfo,&process);
    DWORD exit_code=999;unsigned phase=1;
    if(ok){phase=2;ok=AssignProcessToJobObject(job,process.hProcess)!=FALSE;}
    if(ok){phase=3;ok=sb_win_sandbox_token(sandbox,process.hProcess);}
    if(ok){phase=4;ok=ResumeThread(process.hThread)!=(DWORD)-1;}
    if(ok){phase=5;ok=WaitForSingleObject(process.hProcess,2500)==WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess,&exit_code) && exit_code==0;}
    DWORD native_error=GetLastError();
    if(job){TerminateJobObject(job,125);CloseHandle(job);}if(process.hProcess){WaitForSingleObject(process.hProcess,2500);CloseHandle(process.hProcess);}if(process.hThread)CloseHandle(process.hThread);
    if(initialized)DeleteProcThreadAttributeList(attributes);free(attributes);free(line);free(workspace);free(data);free(readonly);
    if(listener!=INVALID_SOCKET)closesocket(listener);WSACleanup();
    if(!ok){char reason[160];snprintf(reason,sizeof(reason),"Restricted activation probe failed (phase %u, exit %lu, Windows error %lu); backend stays closed.",phase,exit_code,native_error);sb_fail(error,reason);}return ok;
}
sb_win_sandbox *sb_win_sandbox_new(sb_host *host,const char *instance,sb_error *error) {
    if(!host->project->policy.runtime_file_count){sb_fail(error,"Restricted execution requires owner-configured exact runtime_files.");return NULL;}
    sb_win_sandbox *sandbox=calloc(1,sizeof(*sandbox));if(!sandbox)return NULL;
    swprintf(sandbox->profile,64,L"ShellBot.M4.%hs",instance);
    if(FAILED(DeriveAppContainerSidFromAppContainerName(sandbox->profile,&sandbox->sid)))goto failed;
    sandbox->capabilities.AppContainerSid=sandbox->sid;
    sandbox->packages_policy=PROCESS_CREATION_ALL_APPLICATION_PACKAGES_OPT_OUT;
    sandbox->children_policy=PROCESS_CREATION_CHILD_PROCESS_RESTRICTED;
    sandbox->mitigation=PROCESS_CREATION_MITIGATION_POLICY_WIN32K_SYSTEM_CALL_DISABLE_ALWAYS_ON;
    wchar_t *sid=NULL;if(!ConvertSidToStringSidW(sandbox->sid,&sid))goto failed;char *sid_utf8=utf8(sid);LocalFree(sid);
    if(!sid_utf8 || strlen(sid_utf8)>=sizeof(sandbox->sid_text)){free(sid_utf8);goto failed;}strcpy(sandbox->sid_text,sid_utf8);free(sid_utf8);
    char name[64];snprintf(name,sizeof(name),"sandbox-%s.jsonl",instance);sandbox->journal=sb_platform_journal_open(host->lease,name,error);
    if(!sandbox->journal || !journal_record(sandbox,NULL,NULL,false,false,error))goto failed;
    PSID created=NULL;HRESULT result=CreateAppContainerProfile(sandbox->profile,L"ShellBot restricted worker",L"Private project execution identity",NULL,0,&created);
    if(FAILED(result))goto failed;sandbox->profile_created=true;
    bool same=EqualSid(created,sandbox->sid)!=FALSE;FreeSid(created);if(!same)goto failed;
    for(size_t i=0;i<host->config.workspace_count;++i) {
        sb_file *directory=sb_platform_open(host->config.workspaces[i].root,".",true,error);
        char *text=directory?sb_platform_directory_path(directory,error):NULL;sb_platform_close(directory);wchar_t *path=wide(text);free(text);
        bool ok=path && grant_tree(sandbox,path,host->config.workspaces[i].read_only,true,0,error);free(path);if(!ok)goto failed;
    }
    wchar_t probe[SB_PATH_CAP];DWORD n=GetModuleFileNameW(NULL,probe,SB_PATH_CAP);wchar_t *separator=n && n<SB_PATH_CAP?wcsrchr(probe,L'\\'):NULL;
    if(!separator)goto failed;wcscpy(separator+1,L"ShellBot-SandboxProbe.exe");
    for(size_t i=0;i<=host->project->policy.runtime_file_count;++i) {
        wchar_t *path=i?wide(host->project->policy.runtime_files[i-1]):_wcsdup(probe);
        if(!path || !grant_tree(sandbox,path,true,false,0,error)){free(path);goto failed;}
        HANDLE file=CreateFileW(path,FILE_READ_ATTRIBUTES,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,NULL);free(path);
        wchar_t *actual=file!=INVALID_HANDLE_VALUE?final_path(file):NULL;if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);
        if(!actual)goto failed;sandbox->programs[sandbox->program_count++]=actual;
    }
    if(!verify(sandbox,host,probe,instance,error))goto failed;return sandbox;
failed:
    if(!*error->text)sb_fail(error,"Cannot initialize restricted sandbox resources; no command backend opened.");
    sb_win_sandbox_free(sandbox);return NULL;
}

bool sb_win_sandbox_recover(const sb_project *project,sb_error *error) {
    static const char recovery_receipt[]="sandbox-recovered\n";
    sb_root *data=sb_platform_root(project->data_dir,error);if(!data)return false;
    sb_file *directory=sb_platform_open(data,".",true,error);sb_entry *entries=NULL;size_t count=0;bool more=false;
    bool ok=directory && sb_platform_list(directory,0,4096,&entries,&count,&more,error) && !more;sb_platform_close(directory);
    for(size_t i=0;ok && i<count;++i) {
        const char *name=entries[i].name;
        if(strlen(name)!=46 || strncmp(name,"sandbox-",8) || strcmp(name+40,".jsonl") || entries[i].directory || entries[i].reparse)continue;
        char instance[33];memcpy(instance,name+8,32);instance[32]=0;if(strspn(instance,"0123456789abcdef")!=32)continue;
        char marker[64];snprintf(marker,sizeof(marker),"sandbox-%s.done",instance);
        sb_error marker_error={0};sb_file *done=sb_platform_open(data,marker,false,&marker_error);
        if(done){
            char receipt[sizeof(recovery_receipt)]={0};size_t read=0;
            bool saved=sb_platform_size(done)==sizeof(recovery_receipt)-1 &&
                sb_platform_read(done,0,(unsigned char *)receipt,sizeof(recovery_receipt)-1,&read,&marker_error) &&
                read==sizeof(recovery_receipt)-1 && !strcmp(receipt,recovery_receipt);
            sb_platform_close(done);if(saved)continue;
            sb_fail(error,"Sandbox recovery receipt is incomplete; inspect it before restarting.");ok=false;break;
        }
        sb_file *file=sb_platform_open(data,name,false,error);uint64_t size=file?sb_platform_size(file):0;
        char *bytes=size && size<=16777216?calloc((size_t)size+1,1):NULL;size_t got=0;
        bool loaded=bytes && sb_platform_read(file,0,(unsigned char *)bytes,(size_t)size,&got,error) && got==size;sb_platform_close(file);
        if(!loaded){free(bytes);ok=false;break;}
        if(strstr(bytes,"\"complete\":true")){free(bytes);continue;}
        wchar_t profile[64];swprintf(profile,64,L"ShellBot.M4.%hs",instance);PSID sid=NULL;
        if(FAILED(DeriveAppContainerSidFromAppContainerName(profile,&sid))){free(bytes);ok=false;break;}
        char *lines[1026];size_t line_count=0;char *position=bytes;
        while(*position && line_count<1026){lines[line_count++]=position;char *end=strchr(position,'\n');if(!end)break;*end=0;position=end+1;}
        bool restored=true;
        for(size_t j=line_count;j;--j) {
            if(!*lines[j-1])continue;
            yyjson_doc *doc=yyjson_read(lines[j-1],strlen(lines[j-1]),0);yyjson_val *root=yyjson_doc_get_root(doc);
            const char *path=sb_string(yyjson_obj_get(root,"path")),*expected=sb_string(yyjson_obj_get(root,"object_id"));
            if(path && expected) {
                wchar_t *wide_path=wide(path);HANDLE object=wide_path?CreateFileW(wide_path,READ_CONTROL|WRITE_DAC|FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL):INVALID_HANDLE_VALUE;free(wide_path);
                if(object!=INVALID_HANDLE_VALUE){char actual[50];bool valid=object_id(object,actual) && !strcmp(actual,expected);
                    if(valid && yyjson_get_bool(yyjson_obj_get(root,"tree"))){wchar_t *actual_path=final_path(object);unsigned seen=0;if(!actual_path || !clean_tree(actual_path,sid,0,&seen))restored=false;free(actual_path);}
                    if(!valid || !remove_sid(object,sid))restored=false;CloseHandle(object);}
                else if(GetLastError()!=ERROR_FILE_NOT_FOUND && GetLastError()!=ERROR_PATH_NOT_FOUND)restored=false;
            } else if(!doc && j!=line_count)restored=false; /* A torn final line precedes any permission mutation. */
            yyjson_doc_free(doc);
        }
        HRESULT deleted=DeleteAppContainerProfile(profile);
        if(FAILED(deleted) && deleted!=HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) && deleted!=HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND))restored=false;
        FreeSid(sid);free(bytes);
        if(!restored){ok=false;sb_fail(error,"Restricted resource cleanup incomplete; worker restart is blocked.");}
        if(restored) {
            char identity[50];bool bound=sb_platform_root_identity(data,identity,error);
            sb_lease *lease=bound?sb_platform_lease_bound(project->data_dir,identity,error):NULL;
            sb_journal *receipt=lease?sb_platform_journal_open(lease,marker,error):NULL;
            bool saved=receipt && sb_platform_journal_append(receipt,recovery_receipt,sizeof(recovery_receipt)-1,error);
            sb_platform_journal_close(receipt);sb_platform_lease_close(lease);
            if(!saved){ok=false;sb_fail(error,"Sandbox resources recovered but recovery receipt could not be saved; restart remains blocked.");}
        }
    }
    sb_platform_entries_free(entries,count);sb_platform_root_close(data);return ok;
}
