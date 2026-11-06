/* 账户、会话、进程创建身份与私有对象的共用校验。 */
#include "graphics.h"
#include <sddl.h>
#include <aclapi.h>
#include <bcrypt.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
static TOKEN_USER *user(HANDLE token){DWORD bytes=0;GetTokenInformation(token,TokenUser,NULL,0,&bytes);TOKEN_USER *value=bytes?malloc(bytes):NULL;if(value && !GetTokenInformation(token,TokenUser,value,bytes,&bytes)){free(value);value=NULL;}return value;}
bool sg_same_user(HANDLE process){HANDLE own=NULL,target=NULL;TOKEN_USER *a=NULL,*b=NULL;bool ok=OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&own) && OpenProcessToken(process,TOKEN_QUERY,&target);if(ok){a=user(own);b=user(target);ok=a && b && EqualSid(a->User.Sid,b->User.Sid);}free(a);free(b);if(own)CloseHandle(own);if(target)CloseHandle(target);return ok;}
bool sg_identity_read(HANDLE process,sg_identity *value,bool normal){
    FILETIME start,exit,kernel,time;HANDLE token=NULL;DWORD bytes=0,session=0,own_session=0,pid=GetProcessId(process);BYTE integrity[256];TOKEN_ELEVATION elevation={0};
    if(!pid || !GetProcessTimes(process,&start,&exit,&kernel,&time) || !ProcessIdToSessionId(pid,&session) || !ProcessIdToSessionId(GetCurrentProcessId(),&own_session) || session!=own_session || !sg_same_user(process) || !OpenProcessToken(process,TOKEN_QUERY,&token))return false;
    bool ok=GetTokenInformation(token,TokenIntegrityLevel,integrity,sizeof(integrity),&bytes) && GetTokenInformation(token,TokenElevation,&elevation,sizeof(elevation),&bytes);DWORD rid=0;
    if(ok){PSID sid=((TOKEN_MANDATORY_LABEL*)integrity)->Label.Sid;rid=*GetSidSubAuthority(sid,(DWORD)*GetSidSubAuthorityCount(sid)-1);if(normal)ok=rid==SECURITY_MANDATORY_MEDIUM_RID && !elevation.TokenIsElevated && !IsTokenRestricted(token);}
    CloseHandle(token);if(!ok)return false;value->pid=pid;value->session=session;value->integrity=rid;value->created=((uint64_t)start.dwHighDateTime<<32)|start.dwLowDateTime;return true;
}
bool sg_same_identity(HANDLE process,const sg_identity *expected,bool normal){sg_identity actual={0};return sg_identity_read(process,&actual,normal) && actual.pid==expected->pid && actual.session==expected->session && actual.created==expected->created && actual.integrity==expected->integrity && WaitForSingleObject(process,0)==WAIT_TIMEOUT;}
bool sg_default_desktop(void){wchar_t station[64],desktop[64];DWORD bytes=0;return GetUserObjectInformationW(GetProcessWindowStation(),UOI_NAME,station,sizeof(station),&bytes) && !_wcsicmp(station,L"WinSta0") && GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()),UOI_NAME,desktop,sizeof(desktop),&bytes) && !_wcsicmp(desktop,L"Default");}
bool sg_private_security(PSECURITY_DESCRIPTOR *descriptor){
    HANDLE token=NULL;TOKEN_USER *value=NULL;LPWSTR sid=NULL;wchar_t text[512];bool ok=false;*descriptor=NULL;
    if(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token) && (value=user(token)) && ConvertSidToStringSidW(value->User.Sid,&sid) && swprintf_s(text,512,L"D:P(A;OICI;GA;;;SY)(A;OICI;GA;;;%ls)",sid)>0)ok=ConvertStringSecurityDescriptorToSecurityDescriptorW(text,SDDL_REVISION_1,descriptor,NULL)!=FALSE;
    if(sid)LocalFree(sid);free(value);if(token)CloseHandle(token);return ok;
}
bool sg_private_file(HANDLE file){
    HANDLE token=NULL;TOKEN_USER *value=NULL;PSECURITY_DESCRIPTOR descriptor=NULL;PSID owner=NULL;PACL acl=NULL;BYTE system[SECURITY_MAX_SID_SIZE];DWORD size=sizeof(system);bool ok=false;
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token) || !(value=user(token)) || !CreateWellKnownSid(WinLocalSystemSid,NULL,system,&size) || GetSecurityInfo(file,SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,&owner,NULL,&acl,NULL,&descriptor)!=ERROR_SUCCESS || !owner || !EqualSid(owner,value->User.Sid) || !acl)goto done;
    ok=true;for(DWORD i=0;i<acl->AceCount;++i){ACE_HEADER *ace=NULL;if(!GetAce(acl,i,(void**)&ace)){ok=false;break;}if(ace->AceType==ACCESS_ALLOWED_ACE_TYPE){PSID sid=&((ACCESS_ALLOWED_ACE*)ace)->SidStart;if(!EqualSid(sid,value->User.Sid) && !EqualSid(sid,system)){ok=false;break;}}else if(ace->AceType!=ACCESS_DENIED_ACE_TYPE){ok=false;break;}}
done:
    if(descriptor)LocalFree(descriptor);free(value);if(token)CloseHandle(token);return ok;
}
bool sg_valid_nonce(const wchar_t *value){return value && wcslen(value)==32 && wcsspn(value,L"0123456789abcdefABCDEF")==32;}
bool sg_nonce(wchar_t value[33]){BYTE random[16];if(BCryptGenRandom(NULL,random,sizeof(random),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)return false;static const wchar_t hex[]=L"0123456789abcdef";for(unsigned i=0;i<16;++i){value[i*2]=hex[random[i]>>4];value[i*2+1]=hex[random[i]&15];}value[32]=0;return true;}
bool sg_named(wchar_t out[128],const wchar_t *kind,DWORD pid,const wchar_t *nonce){return sg_valid_nonce(nonce) && swprintf_s(out,128,L"Local\\ShellBotGraphics.%ls.%lu.%ls",kind,pid,nonce)>0;}
