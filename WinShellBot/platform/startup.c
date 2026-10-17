#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#include <sddl.h>
#include <aclapi.h>
#include "startup.h"
#include "manager.h"
#include "manager_ui.h"
#include "execution.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static void trace(const char *phase) {
    const char *enabled=getenv("SHELLBOT_STARTUP_TRACE");if(enabled && !strcmp(enabled,"1")){fprintf(stderr,"ShellBot startup phase: %s\n",phase);fflush(stderr);}
}

static wchar_t *wide(const char *s) {
    int n=s ? MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s,-1,NULL,0) : 0;
    wchar_t *out=n ? calloc((size_t)n,sizeof(wchar_t)) : NULL;
    if(out && !MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s,-1,out,n)){free(out);out=NULL;}return out;
}
static char *utf8(const wchar_t *s) {
    int n=s?WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s,-1,NULL,0,NULL,NULL):0;char *out=n?malloc((size_t)n):NULL;
    if(out && !WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s,-1,out,n,NULL,NULL)){free(out);out=NULL;}return out;
}
static char *canonical_path(const char *path,HANDLE pin) {
    wchar_t text[SB_PATH_CAP],*name=wide(path);DWORD n=pin && pin!=INVALID_HANDLE_VALUE?
        GetFinalPathNameByHandleW(pin,text,SB_PATH_CAP,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS):name?GetFullPathNameW(name,SB_PATH_CAP,text,NULL):0;
    free(name);return n && n<SB_PATH_CAP?utf8(!wcsncmp(text,L"\\\\?\\",4)?text+4:text):NULL;
}
static bool local(const char *path) { return path && strlen(path)>2 && path[1]==':' && sb_project_path_valid(path); }
static bool ordinary(void) {
    HANDLE token=NULL;TOKEN_ELEVATION elevation={0};DWORD n=0;
    bool ok=OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token) && sb_win_owner_token_allowed(token) &&
        GetTokenInformation(token,TokenElevation,&elevation,sizeof(elevation),&n) && !elevation.TokenIsElevated;
    if(token)CloseHandle(token);return ok;
}
bool sb_startup_service_sid(unsigned char sid[SECURITY_MAX_SID_SIZE],DWORD *size) {
    wchar_t domain[256];DWORD capacity=256;SID_NAME_USE use;
    *size=SECURITY_MAX_SID_SIZE;return LookupAccountNameW(NULL,SB_SERVICE_ACCOUNT,sid,size,domain,&capacity,&use)!=FALSE;
}
/* DPAPI machine scope depends on this private, protected ACL. Only the file
   owner, SYSTEM and the one installed virtual service identity may read it. */
static bool private_file(HANDLE handle) {
    PSECURITY_DESCRIPTOR descriptor=NULL;PSID owner=NULL;PACL acl=NULL;
    BYTE system[SECURITY_MAX_SID_SIZE],service[SECURITY_MAX_SID_SIZE];DWORD size=sizeof(system),service_size=0;
    trace("private_acl_service_sid");bool has_service=sb_startup_service_sid(service,&service_size);trace("private_acl_security");
    SECURITY_DESCRIPTOR_CONTROL control=0;DWORD revision=0;BY_HANDLE_FILE_INFORMATION info={0};
    bool ok=GetFileType(handle)==FILE_TYPE_DISK && GetFileInformationByHandle(handle,&info) && info.nNumberOfLinks==1 &&
        !(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)) &&
        GetSecurityInfo(handle,SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,&owner,NULL,&acl,NULL,&descriptor)==ERROR_SUCCESS &&
        owner && acl && acl->AceCount && GetSecurityDescriptorControl(descriptor,&control,&revision) &&
        (control&SE_DACL_PROTECTED) && CreateWellKnownSid(WinLocalSystemSid,NULL,system,&size);
    for(DWORD i=0;ok && i<acl->AceCount;++i) {
        ACCESS_ALLOWED_ACE *ace=NULL;ok=GetAce(acl,i,(void **)&ace) && ace->Header.AceType==ACCESS_ALLOWED_ACE_TYPE &&
            !(ace->Header.AceFlags&INHERITED_ACE) && (EqualSid(&ace->SidStart,owner) || EqualSid(&ace->SidStart,system) ||
            (has_service && EqualSid(&ace->SidStart,service)));
    }
    if(descriptor)LocalFree(descriptor);return ok;
}
static PSECURITY_DESCRIPTOR private_descriptor(void) {
    HANDLE token=NULL;DWORD n=0;TOKEN_USER *user=NULL;LPWSTR sid=NULL;PSECURITY_DESCRIPTOR out=NULL;
    if(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token)) {
        GetTokenInformation(token,TokenUser,NULL,0,&n);user=n?malloc(n):NULL;
        if(user && GetTokenInformation(token,TokenUser,user,n,&n) && ConvertSidToStringSidW(user->User.Sid,&sid)) {
            wchar_t text[512];swprintf(text,512,L"O:%lsD:P(A;;FA;;;SY)(A;;FA;;;%ls)",sid,sid);
            ConvertStringSecurityDescriptorToSecurityDescriptorW(text,SDDL_REVISION_1,&out,NULL);
        }
    }
    if(sid)LocalFree(sid);free(user);if(token)CloseHandle(token);return out;
}
static bool create_profile(const char *path,const char *text,sb_error *error) {
    wchar_t *name=wide(path);PSECURITY_DESCRIPTOR descriptor=private_descriptor();
    SECURITY_ATTRIBUTES attributes={sizeof(attributes),descriptor,FALSE};
    HANDLE file=name && descriptor ? CreateFileW(name,GENERIC_WRITE|DELETE,0,&attributes,CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,NULL) : INVALID_HANDLE_VALUE;
    free(name);if(descriptor)LocalFree(descriptor);DWORD written=0;size_t n=strlen(text);
    bool ok=file!=INVALID_HANDLE_VALUE && n<=SB_TEXT_LIMIT && WriteFile(file,text,(DWORD)n,&written,NULL) && written==n && FlushFileBuffers(file);
    if(file!=INVALID_HANDLE_VALUE){if(!ok){FILE_DISPOSITION_INFO dispose={TRUE};SetFileInformationByHandle(file,FileDispositionInfo,&dispose,sizeof(dispose));}CloseHandle(file);}
    if(!ok)sb_fail(error,"Startup profile must be a new private file in an existing directory.");return ok;
}
static bool overlaps(const char *a,const char *b) {
    char *left=sb_strdup(a),*right=sb_strdup(b);if(!left || !right){free(left);free(right);return true;}
    for(char *s=left;*s;++s)if(*s=='\\')*s='/';for(char *s=right;*s;++s)if(*s=='\\')*s='/';
    size_t n=strlen(left),m=strlen(right);while(n>3 && left[n-1]=='/')left[--n]=0;while(m>3 && right[m-1]=='/')right[--m]=0;
    size_t prefix_length=n<m?n:m;bool match=!_strnicmp(left,right,prefix_length) && (n==m || (n<m?right[n]:left[m])=='/' || prefix_length==3);
    free(left);free(right);return match;
}
static bool boundaries(sb_startup *p,sb_error *error) {
    if(!local(p->path) || !local(p->data)){sb_fail(error,"Startup profile and manager Data require absolute local paths.");return false;}
    char *leaf=NULL;sb_root *parent=sb_platform_account_parent(p->path,&leaf,error);if(!parent)return false;
    bool ok=!overlaps(p->path,p->data) && (!p->service || *p->registry.owner_token_env);
    for(size_t i=0;ok && i<p->registry.project_count;++i) {
        const sb_project *project=&p->registry.projects[i];ok=!overlaps(p->path,project->data_dir) && !overlaps(p->data,project->data_dir);
        if(p->service && (project->legacy_compatibility || project->policy.file_scope!=SB_FILES_CONFIGURED ||
            project->policy.administrator_allowed || project->policy.desktop.enabled || project->policy.desktop.control ||
            project->policy.desktop.clipboard_read || project->policy.desktop.clipboard_write ||
            (project->policy.execution_enabled && project->policy.default_identity!=SB_ID_RESTRICTED) || !local(project->data_dir)))ok=false;
        for(size_t j=0;ok && j<project->directory_count;++j) {
            if(p->service && !local(project->directories[j].path)){ok=false;break;}
            sb_root *directory=sb_platform_root(project->directories[j].path,error);
            bool within=directory && sb_platform_target_within(directory,parent,leaf,error);
            ok=directory && !*error->text && !within;sb_platform_root_close(directory);
        }
    }
    sb_platform_root_close(parent);free(leaf);
    if(!ok && !*error->text)sb_fail(error,"Startup profile/Data overlap or unsupported service policy. Service requires configured local scope, no desktop/admin, and restricted commands.");
    return ok;
}
static bool credentials(sb_startup *p,yyjson_val *object,bool capture,sb_error *error) {
    const char *names[SB_MAX_PROJECTS+1];size_t count=0;
    for(size_t i=0;i<p->registry.project_count;++i)if(p->registry.projects[i].enabled)names[count++]=p->registry.projects[i].token_env;
    if(*p->registry.owner_token_env)names[count++]=p->registry.owner_token_env;
    if(!capture && (!yyjson_is_obj(object) || yyjson_obj_size(object)!=count))goto invalid;
    for(size_t i=0;i<count;++i) {
        const char *value=capture?getenv(names[i]):sb_string(yyjson_obj_get(object,names[i]));size_t n=value?strlen(value):0;
        if(n<32 || n>256)goto invalid;
        for(size_t j=0;j<n;++j)if((unsigned char)value[j]<=32 || (unsigned char)value[j]>=127)goto invalid;
        for(size_t j=0;j<i;++j)if(!strcmp(value,p->credentials[j].value))goto invalid;
        strcpy(p->credentials[i].name,names[i]);strcpy(p->credentials[i].value,value);
    }
    p->credential_count=count;return true;
invalid:sb_fail(error,"Enabled project/owner credentials must be distinct 32..256 byte visible-ASCII values.");return false;
}
void sb_startup_free(sb_startup *p) {
    if(!p)return;if(p->pin && p->pin!=INVALID_HANDLE_VALUE)CloseHandle(p->pin);
    free(p->path);free(p->data);free(p->configuration);sb_projects_free(&p->registry);SecureZeroMemory(p,sizeof(*p));free(p);
}
sb_startup *sb_startup_load(const char *path,sb_error *error) {
    if(!local(path)){sb_fail(error,"Startup profile requires an absolute local file path.");return NULL;}
    sb_startup *p=calloc(1,sizeof(*p));wchar_t *name=wide(path);char *bytes=NULL,*mapped=NULL;yyjson_doc *outer=NULL,*inner=NULL;DATA_BLOB encrypted={0},plain={0};
    if(!p)goto done;p->path=sb_strdup(path);
    trace("profile_open");p->pin=name?CreateFileW(name,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,NULL):INVALID_HANDLE_VALUE;
    LARGE_INTEGER size={0};DWORD got=0;
    if(p->pin==INVALID_HANDLE_VALUE || !private_file(p->pin) || !GetFileSizeEx(p->pin,&size) || size.QuadPart<1 || size.QuadPart>SB_TEXT_LIMIT)goto done;
    free(p->path);p->path=canonical_path(path,p->pin);if(!p->path)goto done;
    bytes=calloc((size_t)size.QuadPart+1,1);
    if(!bytes || !ReadFile(p->pin,bytes,(DWORD)size.QuadPart,&got,NULL) || got!=size.QuadPart)goto done;
    outer=yyjson_read(bytes,got,0);yyjson_val *root=yyjson_doc_get_root(outer);
    static const char *const keys[]={"format_version","dpapi_scope","protected_settings"};uint64_t version;
    const char *scope=sb_string(yyjson_obj_get(root,"dpapi_scope")),*encoded=sb_string(yyjson_obj_get(root,"protected_settings"));
    if(!sb_config_keys(root,keys,3) || !sb_integer(yyjson_obj_get(root,"format_version"),&version) || version!=1 || !scope ||
        (strcmp(scope,"user") && strcmp(scope,"machine")) || !encoded || !*encoded || strlen(encoded)>60000)goto done;
    p->service=!strcmp(scope,"machine");
    trace("profile_decrypt");if(!CryptStringToBinaryA(encoded,0,CRYPT_STRING_BASE64,NULL,&encrypted.cbData,NULL,NULL) || encrypted.cbData>45000)goto done;
    encrypted.pbData=malloc(encrypted.cbData);if(!encrypted.pbData || !CryptStringToBinaryA(encoded,0,CRYPT_STRING_BASE64,encrypted.pbData,&encrypted.cbData,NULL,NULL) ||
        !CryptUnprotectData(&encrypted,NULL,NULL,NULL,NULL,CRYPTPROTECT_UI_FORBIDDEN,&plain) || !plain.cbData ||
        plain.cbData>32768 || plain.pbData[plain.cbData-1])goto done;
    mapped=calloc((size_t)plain.cbData+YYJSON_PADDING_SIZE,1);if(!mapped)goto done;memcpy(mapped,plain.pbData,plain.cbData);
    inner=yyjson_read(mapped,plain.cbData-1,YYJSON_READ_INSITU);root=yyjson_doc_get_root(inner);
    static const char *const settings[]={"mode","manager_data_dir","projects","start_projects","credentials"};
    const char *mode=sb_string(yyjson_obj_get(root,"mode")),*data=sb_string(yyjson_obj_get(root,"manager_data_dir"));
    yyjson_val *ids=yyjson_obj_get(root,"start_projects"),*item;size_t i,max;
    if(!sb_config_keys(root,settings,5) || !mode || strcmp(mode,p->service?"service":"tray") || !data ||
        !yyjson_is_arr(ids) || yyjson_arr_size(ids)>SB_MAX_PROJECTS)goto done;
    p->data=sb_strdup(data);p->configuration=yyjson_val_write(yyjson_obj_get(root,"projects"),0,NULL);
    if(!p->data || !p->configuration || !sb_projects_parse(&p->registry,p->configuration,strlen(p->configuration),error))goto done;
    yyjson_arr_foreach(ids,i,max,item) {
        const char *id=sb_string(item);const sb_project *project=id?sb_project_find(&p->registry,id):NULL;
        if(!project || !project->enabled)goto done;
        for(size_t j=0;j<p->count;++j)if(!strcmp(p->ids[j],id))goto done;
        strcpy(p->ids[p->count++],id);
    }
    trace("profile_boundaries");if(!credentials(p,yyjson_obj_get(root,"credentials"),false,error) || !boundaries(p,error))goto done;trace("profile_ready");
    yyjson_doc_free(outer);yyjson_doc_free(inner);if(mapped){SecureZeroMemory(mapped,(size_t)plain.cbData+YYJSON_PADDING_SIZE);free(mapped);}if(plain.pbData){SecureZeroMemory(plain.pbData,plain.cbData);LocalFree(plain.pbData);}free(encrypted.pbData);free(bytes);free(name);return p;
done:
    yyjson_doc_free(outer);yyjson_doc_free(inner);if(mapped){SecureZeroMemory(mapped,(size_t)plain.cbData+YYJSON_PADDING_SIZE);free(mapped);}if(plain.pbData){SecureZeroMemory(plain.pbData,plain.cbData);LocalFree(plain.pbData);}free(encrypted.pbData);free(bytes);free(name);sb_startup_free(p);
    if(!*error->text)sb_fail(error,"Startup profile is invalid, inaccessible, not private, or cannot be decrypted on this account/machine.");return NULL;
}
bool sb_startup_grant_profile(sb_startup *p,sb_error *error) {
    BYTE sid[SECURITY_MAX_SID_SIZE];DWORD size;wchar_t *name=wide(p->path);PACL old=NULL,updated=NULL;PSECURITY_DESCRIPTOR descriptor=NULL;
    bool ok=name && sb_startup_service_sid(sid,&size) && GetNamedSecurityInfoW(name,SE_FILE_OBJECT,DACL_SECURITY_INFORMATION,NULL,NULL,&old,NULL,&descriptor)==ERROR_SUCCESS;
    EXPLICIT_ACCESSW access={0};access.grfAccessPermissions=FILE_GENERIC_READ;access.grfAccessMode=GRANT_ACCESS;
    access.Trustee.TrusteeForm=TRUSTEE_IS_SID;access.Trustee.TrusteeType=TRUSTEE_IS_USER;access.Trustee.ptstrName=(LPWSTR)sid;
    ok=ok && SetEntriesInAclW(1,&access,old,&updated)==ERROR_SUCCESS &&
        SetNamedSecurityInfoW(name,SE_FILE_OBJECT,DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,NULL,NULL,updated,NULL)==ERROR_SUCCESS;
    if(updated)LocalFree(updated);if(descriptor)LocalFree(descriptor);free(name);
    if(!ok)sb_fail(error,"Cannot grant the installed virtual service read access to this private profile.");return ok;
}
int sb_startup_manager(sb_startup *p,HANDLE stop,HANDLE ready) {
    char *previous[SB_MAX_PROJECTS+1]={0};bool changed[SB_MAX_PROJECTS+1]={0},ok=true;int code=2;
    for(size_t i=0;i<p->credential_count && ok;++i) {
        const char *old=getenv(p->credentials[i].name);previous[i]=old?sb_strdup(old):NULL;
        ok=(!old || previous[i]) && !_putenv_s(p->credentials[i].name,p->credentials[i].value);changed[i]=ok;
    }
    const char *ids[SB_MAX_PROJECTS];for(size_t i=0;i<p->count;++i)ids[i]=p->ids[i];
    if(ok)code=sb_manager_startup(p->configuration,p->data,ids,p->count,stop,ready,p->service);
    for(size_t i=0;i<p->credential_count;++i) {
        if(changed[i])_putenv_s(p->credentials[i].name,previous[i]?previous[i]:"");
        if(previous[i]){SecureZeroMemory(previous[i],strlen(previous[i]));free(previous[i]);}
    }return code;
}
static bool prepare(const char *mode,const char *source,const char *data,const char *output,const char *const *ids,size_t count,sb_error *error) {
    sb_startup *p=calloc(1,sizeof(*p));size_t length=0;char *bytes=NULL,*plain=NULL,*encoded=NULL,*wire=NULL;yyjson_doc *config=NULL;yyjson_mut_doc *doc=NULL,*outer=NULL;DATA_BLOB sealed={0};bool ok=false;
    if(!p || !mode || (strcmp(mode,"tray") && strcmp(mode,"service")))goto done;
    p->service=!strcmp(mode,"service");p->path=sb_strdup(output);p->data=sb_strdup(data);
    if(!p->path || !p->data || !local(source) || !(bytes=sb_platform_load_config(source,&length,error)) || !sb_projects_parse(&p->registry,bytes,length,error) || !boundaries(p,error) || !credentials(p,NULL,true,error))goto done;
    for(size_t i=0;i<count;++i) {
        const sb_project *project=sb_project_find(&p->registry,ids[i]);if(!project || !project->enabled)goto done;
        for(size_t j=0;j<i;++j)if(!strcmp(ids[i],ids[j]))goto done;
    }
    config=yyjson_read(bytes,length,0);doc=yyjson_mut_doc_new(NULL);outer=yyjson_mut_doc_new(NULL);if(!config || !doc || !outer)goto done;
    yyjson_mut_val *root=yyjson_mut_obj(doc);yyjson_mut_doc_set_root(doc,root);
    yyjson_mut_obj_add_str(doc,root,"mode",mode);yyjson_mut_obj_add_str(doc,root,"manager_data_dir",data);
    yyjson_mut_obj_add_val(doc,root,"projects",yyjson_val_mut_copy(doc,yyjson_doc_get_root(config)));
    yyjson_mut_val *selected=yyjson_mut_arr(doc),*tokens=yyjson_mut_obj(doc);
    for(size_t i=0;i<count;++i)yyjson_mut_arr_add_str(doc,selected,ids[i]);
    for(size_t i=0;i<p->credential_count;++i)yyjson_mut_obj_add_str(doc,tokens,p->credentials[i].name,p->credentials[i].value);
    yyjson_mut_obj_add_val(doc,root,"start_projects",selected);yyjson_mut_obj_add_val(doc,root,"credentials",tokens);
    plain=yyjson_mut_write(doc,0,NULL);if(!plain || strlen(plain)+1>32768)goto done;
    DATA_BLOB input={(DWORD)strlen(plain)+1,(BYTE *)plain};
    if(!CryptProtectData(&input,L"ShellBot startup settings",NULL,NULL,NULL,CRYPTPROTECT_UI_FORBIDDEN|(p->service?CRYPTPROTECT_LOCAL_MACHINE:0),&sealed))goto done;
    encoded=sb_base64(sealed.pbData,sealed.cbData);if(!encoded)goto done;
    root=yyjson_mut_obj(outer);yyjson_mut_doc_set_root(outer,root);yyjson_mut_obj_add_uint(outer,root,"format_version",1);
    yyjson_mut_obj_add_str(outer,root,"dpapi_scope",p->service?"machine":"user");yyjson_mut_obj_add_str(outer,root,"protected_settings",encoded);
    wire=yyjson_mut_write(outer,0,NULL);ok=wire && create_profile(output,wire,error);
done:
    if(plain){SecureZeroMemory(plain,strlen(plain));free(plain);}if(sealed.pbData)LocalFree(sealed.pbData);free(encoded);free(wire);free(bytes);
    yyjson_doc_free(config);yyjson_mut_doc_free(doc);yyjson_mut_doc_free(outer);sb_startup_free(p);
    if(!ok && !*error->text)sb_fail(error,"Cannot prepare startup profile; validate mode, selected project IDs and bounded settings.");return ok;
}
static int logon(const char *mode,sb_startup *p,sb_error *error) {
    if(p->service || (!ordinary() && !sb_win_elevated_owner())){sb_fail(error,"Logon startup requires a tray profile and an interactive owner token.");return 2;}
    if(!strcmp(mode,"--logon-enable") && sb_startup_admin_logon("ordinary-logon-guard",p,error))return 2;
    wchar_t image[SB_PATH_CAP],*profile=wide(p->path),*command=NULL;char digest[65];wchar_t value[64];HKEY key=NULL;int code=2;
    DWORD n=GetModuleFileNameW(NULL,image,SB_PATH_CAP);wchar_t *leaf=n && n<SB_PATH_CAP?wcsrchr(image,L'\\'):NULL;
    if(!leaf || !profile || !sb_platform_sha256(p->path,strlen(p->path),digest,error))goto done;
    wcscpy(leaf+1,L"ShellBot-Launcher.exe");
    size_t capacity=wcslen(image)+wcslen(profile)+48;command=calloc(capacity,sizeof(wchar_t));if(!command)goto done;
    swprintf(command,capacity,L"\"%ls\" --startup-profile \"%ls\"",image,profile);swprintf(value,64,L"ShellBot-%.24hs",digest);
    if(wcslen(command)>260 || (!strcmp(mode,"--logon-enable") && GetFileAttributesW(image)==INVALID_FILE_ATTRIBUTES)){sb_fail(error,"Logon command exceeds 260 characters or ShellBot-Launcher.exe is missing.");goto done;}
    DWORD access=!strcmp(mode,"--logon-status")?KEY_QUERY_VALUE:KEY_QUERY_VALUE|KEY_SET_VALUE;
    trace("logon_registry_open");LONG result=RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",0,access,&key);trace("logon_registry_opened");
    if(result==ERROR_FILE_NOT_FOUND && !strcmp(mode,"--logon-enable"))result=RegCreateKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",0,NULL,0,access,NULL,&key,NULL);
    if(result==ERROR_FILE_NOT_FOUND){puts("{\"logon_enabled\":false,\"registered\":false}");code=0;goto done;}
    if(result!=ERROR_SUCCESS)goto done;
    wchar_t current[512]={0};DWORD bytes=sizeof(current),type=0;result=RegQueryValueExW(key,value,NULL,&type,(BYTE *)current,&bytes);
    bool exists=result==ERROR_SUCCESS,owned=exists && type==REG_SZ && bytes>=sizeof(wchar_t) && bytes<=sizeof(current) && current[bytes/sizeof(wchar_t)-1]==0 && !wcscmp(current,command);
    if(result!=ERROR_SUCCESS && result!=ERROR_FILE_NOT_FOUND)goto done;
    if(!strcmp(mode,"--logon-status")) {printf("{\"logon_enabled\":%s,\"registered\":%s,\"owned_entry\":%s}\n",owned?"true":"false",exists?"true":"false",owned?"true":"false");code=0;goto done;}
    if(exists && !owned){sb_fail(error,"Logon entry changed or belongs to another command; it was not overwritten or removed.");goto done;}
    trace("logon_registry_write");result=!strcmp(mode,"--logon-enable")?RegSetValueExW(key,value,0,REG_SZ,(BYTE *)command,(DWORD)((wcslen(command)+1)*sizeof(wchar_t))):!exists?ERROR_SUCCESS:RegDeleteValueW(key,value);trace("logon_registry_finished");
    if(result==ERROR_SUCCESS){printf("{\"logon_enabled\":%s,\"running_configuration_changed\":false}\n",!strcmp(mode,"--logon-enable")?"true":"false");code=0;}
done:
    if(key)RegCloseKey(key);free(profile);free(command);if(code && !*error->text)sb_fail(error,"Cannot inspect or modify this profile's own logon entry.");return code;
}
/* Registry protection/notifications may wait on another program. Isolate that
   wait in an owned, bounded child instead of leaving a CLI or tray thread hung.
   A timeout is explicitly an unconfirmed result, never an automatic retry. */
static int logon_apply(const char *mode,sb_startup *p,sb_error *error) {
    if(p->service || (!ordinary() && !sb_win_elevated_owner())){sb_fail(error,"Logon startup requires a tray profile and an interactive owner token.");return 2;}
    wchar_t image[SB_PATH_CAP],*path=wide(p->path),*line=NULL;DWORD n=GetModuleFileNameW(NULL,image,SB_PATH_CAP);
    size_t capacity=n+(path?wcslen(path):0)+64;HANDLE reader=NULL,writer=NULL,null_file=INVALID_HANDLE_VALUE,job=NULL;PROCESS_INFORMATION process={0};int code=2;
    STARTUPINFOEXW startup={0};startup.StartupInfo.cb=sizeof(startup);SIZE_T attribute_size=0;bool attributes_initialized=false;
    if(!n || n>=SB_PATH_CAP || !path || capacity>=32767)goto done;
    line=calloc(capacity,sizeof(wchar_t));if(!line)goto done;
    swprintf(line,capacity,L"\"%ls\" %ls --startup-profile \"%ls\"",image,!strcmp(mode,"--logon-enable")?L"--logon-write-enable":L"--logon-write-disable",path);
    SECURITY_ATTRIBUTES security={sizeof(security),NULL,TRUE};
    if(!CreatePipe(&reader,&writer,&security,0) || !SetHandleInformation(reader,HANDLE_FLAG_INHERIT,0))goto done;
    null_file=CreateFileW(L"NUL",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,0,NULL);if(null_file==INVALID_HANDLE_VALUE)goto done;
    InitializeProcThreadAttributeList(NULL,1,0,&attribute_size);startup.lpAttributeList=malloc(attribute_size);
    if(!startup.lpAttributeList || !InitializeProcThreadAttributeList(startup.lpAttributeList,1,0,&attribute_size))goto done;
    attributes_initialized=true;
    HANDLE inherited[]={writer,null_file};
    if(!UpdateProcThreadAttribute(startup.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof(inherited),NULL,NULL))goto done;
    startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES;startup.StartupInfo.hStdInput=null_file;startup.StartupInfo.hStdOutput=writer;startup.StartupInfo.hStdError=null_file;
    job=CreateJobObjectW(NULL,NULL);JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits={0};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!job || !SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits)) ||
        !CreateProcessW(image,line,NULL,NULL,TRUE,CREATE_NO_WINDOW|CREATE_SUSPENDED|EXTENDED_STARTUPINFO_PRESENT,NULL,NULL,&startup.StartupInfo,&process))goto done;
    if(!AssignProcessToJobObject(job,process.hProcess) || ResumeThread(process.hThread)==(DWORD)-1){TerminateProcess(process.hProcess,2);goto done;}
    CloseHandle(writer);writer=NULL;trace("logon_helper_wait");
    if(WaitForSingleObject(process.hProcess,20000)!=WAIT_OBJECT_0){sb_fail(error,"Windows did not finish the logon change. Its result is unconfirmed; inspect --logon-status before retrying.");goto done;}
    DWORD exit_code=2,available=0,got=0;char reply[4097]={0};
    if(!GetExitCodeProcess(process.hProcess,&exit_code) || exit_code || !PeekNamedPipe(reader,NULL,0,NULL,&available,NULL) || !available || available>4096 ||
        !ReadFile(reader,reply,available,&got,NULL) || got!=available)goto done;
    yyjson_doc *doc=yyjson_read(reply,got,0);yyjson_val *value=yyjson_obj_get(yyjson_doc_get_root(doc),"logon_enabled");
    if(yyjson_is_bool(value) && yyjson_get_bool(value)==!strcmp(mode,"--logon-enable")){puts(reply);code=0;}yyjson_doc_free(doc);
done:
    if(process.hProcess && WaitForSingleObject(process.hProcess,0)==WAIT_TIMEOUT){if(job)TerminateJobObject(job,2);else TerminateProcess(process.hProcess,2);WaitForSingleObject(process.hProcess,3000);}
    if(process.hThread)CloseHandle(process.hThread);if(process.hProcess)CloseHandle(process.hProcess);if(job)CloseHandle(job);
    if(reader)CloseHandle(reader);if(writer)CloseHandle(writer);if(null_file!=INVALID_HANDLE_VALUE)CloseHandle(null_file);
    if(startup.lpAttributeList){if(attributes_initialized)DeleteProcThreadAttributeList(startup.lpAttributeList);free(startup.lpAttributeList);}free(path);free(line);
    if(code && !*error->text)sb_fail(error,"Logon change could not be confirmed; inspect the profile and current --logon-status.");return code;
}
typedef struct {sb_startup *profile;HANDLE stop,ready;int code;} launch;
static DWORD WINAPI manager_thread(void *argument) {launch *job=argument;job->code=sb_startup_manager(job->profile,job->stop,job->ready);return (DWORD)job->code;}
static HANDLE console_stop;
static BOOL WINAPI console_handler(DWORD event) {
    if(event==CTRL_C_EVENT || event==CTRL_BREAK_EVENT || event==CTRL_CLOSE_EVENT || event==CTRL_LOGOFF_EVENT || event==CTRL_SHUTDOWN_EVENT){if(console_stop)SetEvent(console_stop);return TRUE;}return FALSE;
}
static int resident(sb_startup *p,bool console,sb_error *error) {
    if((!ordinary() && !sb_win_elevated_owner()) || (!console && p->service)){sb_fail(error,"Resident tray mode requires an interactive owner and a tray profile.");return 2;}
    launch job={p,CreateEventW(NULL,TRUE,FALSE,NULL),CreateEventW(NULL,TRUE,FALSE,NULL),2};HANDLE thread=NULL;int code=2,ui_code=0;bool became_ready=false;
    if(!job.stop || !job.ready)goto done;
    if(!console)FreeConsole();else {console_stop=job.stop;SetConsoleCtrlHandler(console_handler,TRUE);}
    thread=CreateThread(NULL,0,manager_thread,&job,0,NULL);if(!thread)goto done;
    HANDLE startup[]={job.ready,thread};DWORD result=WaitForMultipleObjects(2,startup,FALSE,180000);
    if(result!=WAIT_OBJECT_0){SetEvent(job.stop);sb_fail(error,"Startup manager failed or did not become ready; no automatic retry.");goto done;}
    became_ready=true;
    if(console)WaitForSingleObject(thread,INFINITE);
    else ui_code=sb_manager_resident_ui(p->data,job.stop,thread,p->path);
    SetEvent(job.stop);code=job.code;
done:
    if(thread){SetEvent(job.stop);if(WaitForSingleObject(thread,30000)!=WAIT_OBJECT_0)ExitProcess(3);code=job.code?job.code:became_ready?ui_code:2;CloseHandle(thread);}
    if(console){SetConsoleCtrlHandler(console_handler,FALSE);console_stop=NULL;}
    if(job.stop)CloseHandle(job.stop);if(job.ready)CloseHandle(job.ready);return code;
}
bool sb_startup_option(const char *option) {
    static const char *const names[]={"--startup-prepare","--startup-inspect","--startup-console","--resident","--logon-enable","--logon-disable","--logon-status","--logon-write-enable","--logon-write-disable","--admin-logon-enable","--admin-logon-disable","--admin-logon-status",
        "--service-install","--service-uninstall","--service-start","--service-stop","--service-status","--service-run"};
    for(size_t i=0;i<sizeof(names)/sizeof(*names);++i)if(!strcmp(option,names[i]))return true;return false;
}
int sb_startup_cli(int argc,char **argv) {
    const char *mode=argv[1],*profile=NULL,*source=NULL,*data=NULL,*output=NULL,*kind=NULL,*ids[SB_MAX_PROJECTS];size_t count=0;
    for(int i=2;i<argc;++i) {
        if(!strcmp(argv[i],"--start-project")){if(count>=SB_MAX_PROJECTS || i+1>=argc || !sb_identifier(argv[i+1]))goto invalid;ids[count++]=argv[++i];continue;}
        const char **slot=!strcmp(argv[i],"--startup-profile")?&profile:!strcmp(argv[i],"--projects")?&source:!strcmp(argv[i],"--manager-data-dir")?&data:
            !strcmp(argv[i],"--output")?&output:!strcmp(argv[i],"--mode")?&kind:NULL;
        if(!slot || *slot || i+1>=argc || !*argv[i+1])goto invalid;*slot=argv[++i];
    }
    sb_error error={0};int code=2;sb_startup *loaded=NULL;
    if(!strcmp(mode,"--startup-prepare")) {
        if(profile || !source || !data || !output || !kind)goto invalid;
        if(prepare(kind,source,data,output,ids,count,&error)){puts("{\"startup_profile_saved\":true,\"credentials_encrypted\":true,\"configuration_snapshot\":true,\"registered\":false,\"runtime_started\":false}");code=0;}goto done;
    }
    if(source || data || output || kind || count || (!profile && strcmp(mode,"--service-status")))goto invalid;
    if(profile) {
        if(!strcmp(mode,"--logon-disable") || !strcmp(mode,"--logon-write-disable") || !strcmp(mode,"--logon-status") || !strcmp(mode,"--admin-logon-disable") || !strcmp(mode,"--admin-logon-status")) {
            if(!local(profile))goto invalid;loaded=calloc(1,sizeof(*loaded));if(!loaded)goto done;
            wchar_t *name=wide(profile);HANDLE pin=name?CreateFileW(name,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,NULL):INVALID_HANDLE_VALUE;
            loaded->path=canonical_path(profile,pin);if(pin!=INVALID_HANDLE_VALUE)CloseHandle(pin);free(name);if(!loaded->path)goto done;
        } else if(!(loaded=sb_startup_load(profile,&error)))goto done;
    }
    if(!strcmp(mode,"--startup-inspect")) {
        yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL);yyjson_mut_val *root=doc?yyjson_mut_obj(doc):NULL;if(!root)goto done;yyjson_mut_doc_set_root(doc,root);
        yyjson_mut_obj_add_str(doc,root,"mode",loaded->service?"service":"tray");yyjson_mut_obj_add_str(doc,root,"manager_data_dir",loaded->data);
        yyjson_mut_obj_add_str(doc,root,"profile_path",loaded->path);
        yyjson_mut_obj_add_uint(doc,root,"credential_count",loaded->credential_count);yyjson_mut_obj_add_bool(doc,root,"credentials_encrypted",true);
        yyjson_mut_obj_add_bool(doc,root,"configuration_snapshot",true);yyjson_mut_val *selected=yyjson_mut_arr(doc);
        for(size_t i=0;i<loaded->count;++i)yyjson_mut_arr_add_str(doc,selected,loaded->ids[i]);yyjson_mut_obj_add_val(doc,root,"start_projects",selected);
        /* Registry schema stores only credential references, never values. The
           owner can review the exact frozen permissions before registration. */
        yyjson_doc *configuration=yyjson_read(loaded->configuration,strlen(loaded->configuration),0);
        yyjson_mut_obj_add_val(doc,root,"projects_configuration",yyjson_val_mut_copy(doc,yyjson_doc_get_root(configuration)));yyjson_doc_free(configuration);
        char *wire=yyjson_mut_write(doc,0,NULL);if(wire){puts(wire);code=0;}free(wire);yyjson_mut_doc_free(doc);
    } else if(!strncmp(mode,"--admin-logon-",14))code=sb_startup_admin_logon(mode,loaded,&error);
    else if(!strcmp(mode,"--logon-write-enable"))code=logon("--logon-enable",loaded,&error);
    else if(!strcmp(mode,"--logon-write-disable"))code=logon("--logon-disable",loaded,&error);
    else if(!strcmp(mode,"--logon-enable") || !strcmp(mode,"--logon-disable"))code=logon_apply(mode,loaded,&error);
    else if(!strcmp(mode,"--logon-status"))code=logon(mode,loaded,&error);
    else if(!strcmp(mode,"--resident") || !strcmp(mode,"--startup-console"))code=resident(loaded,!strcmp(mode,"--startup-console"),&error);
    else code=sb_service_cli(mode,loaded,&error);
done:
    sb_startup_free(loaded);if(code)fprintf(stderr,"ShellBot: %s\n",*error.text?error.text:"Startup command failed.");return code;
invalid:fputs("ShellBot: Invalid startup arguments. Use --help.\n",stderr);return 2;
}
