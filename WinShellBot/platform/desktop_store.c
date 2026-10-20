#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sddl.h>
#include <aclapi.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include "desktop_app.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
wchar_t *sb_desktop_wide(const char *s) {
    int n=s ? MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s,-1,NULL,0) : 0;
    wchar_t *out=n ? calloc((size_t)n,sizeof(*out)) : NULL;
    if (out) MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s,-1,out,n); return out;
}
char *sb_desktop_join(const char *directory,const char *leaf) {
    size_t n=strlen(directory)+strlen(leaf)+2;char *out=n<SB_PATH_CAP ? malloc(n) : NULL;
    size_t length=strlen(directory);const char *separator=length && (directory[length-1]=='\\' || directory[length-1]=='/') ? "" : "\\";
    if (out) snprintf(out,n,"%s%s%s",directory,separator,leaf);return out;
}
static unsigned char folded(char c) { return c=='\\' ? '/' : (unsigned char)(c>='A' && c<='Z' ? c+32 : c); }
bool sb_desktop_overlap(const char *a,const char *b) {
    if (!a || !b || !*a || !*b) return false;
    size_t n=strlen(a),m=strlen(b);while (n>2 && folded(a[n-1])=='/') --n;while (m>2 && folded(b[m-1])=='/') --m;
    size_t size=n<m ? n : m;for (size_t i=0;i<size;++i) if (folded(a[i])!=folded(b[i])) return false;
    return n==m || folded((n<m ? b : a)[size])=='/';
}
static PSECURITY_DESCRIPTOR descriptor(void) {
    HANDLE token=NULL;DWORD size=0;TOKEN_USER *user=NULL;LPWSTR sid=NULL;PSECURITY_DESCRIPTOR out=NULL;
    if (OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token)) {
        GetTokenInformation(token,TokenUser,NULL,0,&size);user=size ? malloc(size) : NULL;
        if (user && GetTokenInformation(token,TokenUser,user,size,&size) && ConvertSidToStringSidW(user->User.Sid,&sid)) {
            wchar_t sddl[512];swprintf(sddl,512,L"O:%lsD:P(A;;FA;;;SY)(A;;FA;;;%ls)",sid,sid);
            ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl,SDDL_REVISION_1,&out,NULL);
        }
    }
    if (token) CloseHandle(token);if (sid) LocalFree(sid);free(user);return out;
}
bool sb_desktop_private_write(const char *path,const char *bytes,size_t length,bool replace,sb_error *error) {
    unsigned char nonce[16];char suffix[48]=".pending-";
    if (!sb_project_path_valid(path) || !bytes || length>SB_TEXT_LIMIT || !sb_platform_random(nonce,sizeof(nonce),error)) return false;
    for (size_t i=0;i<16;++i) snprintf(suffix+9+i*2,3,"%02x",nonce[i]);
    if (replace) {
        wchar_t *existing=sb_desktop_wide(path);DWORD flags=existing ? GetFileAttributesW(existing) : INVALID_FILE_ATTRIBUTES;free(existing);
        if (flags!=INVALID_FILE_ATTRIBUTES) {
            if (flags&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)) {sb_fail(error,"配置文件被替换为目录或链接，已拒绝覆盖。");return false;}
            size_t original_size=0;char *original=sb_platform_load_config(path,&original_size,error);
            size_t capacity=strlen(path)+sizeof(suffix)+9;char *backup=malloc(capacity);
            if (backup) snprintf(backup,capacity,"%s.backup%s",path,suffix);
            bool retained=original && backup && sb_desktop_private_write(backup,original,original_size,false,error);free(original);free(backup);
            if (!retained) return false;
        }
    }
    size_t size=strlen(path)+sizeof(suffix);char *temporary=malloc(size);if (temporary) snprintf(temporary,size,"%s%s",path,suffix);
    wchar_t *target=sb_desktop_wide(path),*name=temporary ? sb_desktop_wide(temporary) : NULL;
    PSECURITY_DESCRIPTOR sd=descriptor();SECURITY_ATTRIBUTES sa={sizeof(sa),sd,FALSE};
    HANDLE file=sd && name ? CreateFileW(name,GENERIC_WRITE|DELETE,0,&sa,CREATE_NEW,FILE_FLAG_OPEN_REPARSE_POINT,NULL) : INVALID_HANDLE_VALUE;
    DWORD written=0;bool ok=file!=INVALID_HANDLE_VALUE && WriteFile(file,bytes,(DWORD)length,&written,NULL) && written==length && FlushFileBuffers(file);
    if (file!=INVALID_HANDLE_VALUE) { if (!ok) { FILE_DISPOSITION_INFO d={TRUE};SetFileInformationByHandle(file,FileDispositionInfo,&d,sizeof(d)); }CloseHandle(file); }
    if (ok) ok=MoveFileExW(name,target,(replace ? MOVEFILE_REPLACE_EXISTING : 0)|MOVEFILE_WRITE_THROUGH)!=FALSE;
    /* A completed unpublished temporary file is retained on publication failure. */
    if (!ok) sb_fail(error,"无法保存项目配置；原文件保持完整，请检查目录权限、剩余空间或被占用的文件。");
    if (sd) LocalFree(sd);free(target);free(name);free(temporary);return ok;
}
char *sb_desktop_seal(const char *text,sb_error *error) {
    DATA_BLOB input={(DWORD)strlen(text)+1,(BYTE *)text},output={0};char *base64=NULL,*json=NULL;DWORD size=0;
    if (input.cbData>40000 || !CryptProtectData(&input,L"ShellBot desktop project",NULL,NULL,NULL,CRYPTPROTECT_UI_FORBIDDEN,&output)) goto done;
    if (!CryptBinaryToStringA(output.pbData,output.cbData,CRYPT_STRING_BASE64|CRYPT_STRING_NOCRLF,NULL,&size)) goto done;
    base64=malloc(size);if (!base64 || !CryptBinaryToStringA(output.pbData,output.cbData,CRYPT_STRING_BASE64|CRYPT_STRING_NOCRLF,base64,&size)) goto done;
    yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL);if (!doc) goto done;
    yyjson_mut_val *root=yyjson_mut_obj(doc);yyjson_mut_doc_set_root(doc,root);
    yyjson_mut_obj_add_uint(doc,root,"format_version",1);yyjson_mut_obj_add_str(doc,root,"protected_project",base64);
    json=yyjson_mut_write(doc,0,NULL);yyjson_mut_doc_free(doc);
done:
    if (output.pbData) LocalFree(output.pbData);free(base64);if (!json) sb_fail(error,"Windows 无法加密项目凭据，配置未保存。");return json;
}
char *sb_desktop_unseal(const char *path,sb_error *error) {
    size_t length=0;char *bytes=sb_platform_load_config(path,&length,error),*out=NULL;
    yyjson_doc *doc=bytes ? yyjson_read(bytes,length,0) : NULL;yyjson_val *root=yyjson_doc_get_root(doc);
    const char *value=sb_string(yyjson_obj_get(root,"protected_project"));DATA_BLOB encrypted={0},plain={0};
    if (!value || yyjson_get_uint(yyjson_obj_get(root,"format_version"))!=1 || yyjson_obj_size(root)!=2 ||
        !CryptStringToBinaryA(value,0,CRYPT_STRING_BASE64,NULL,&encrypted.cbData,NULL,NULL) || encrypted.cbData>50000) goto done;
    encrypted.pbData=malloc(encrypted.cbData);
    if (!encrypted.pbData || !CryptStringToBinaryA(value,0,CRYPT_STRING_BASE64,encrypted.pbData,&encrypted.cbData,NULL,NULL) ||
        !CryptUnprotectData(&encrypted,NULL,NULL,NULL,NULL,CRYPTPROTECT_UI_FORBIDDEN,&plain) || !plain.cbData || plain.cbData>40000 || plain.pbData[plain.cbData-1] || memchr(plain.pbData,0,plain.cbData-1)) goto done;
    out=calloc((size_t)plain.cbData+1,1);if (out) memcpy(out,plain.pbData,plain.cbData);
done:
    if (plain.pbData) { SecureZeroMemory(plain.pbData,plain.cbData);LocalFree(plain.pbData); }
    free(encrypted.pbData);yyjson_doc_free(doc);free(bytes);if (!out && !*error->text) sb_fail(error,"项目配置无法由当前 Windows 账户解密，请保留原文件。");return out;
}
bool sb_desktop_make_directory(const char *path,bool empty,sb_error *error) {
    wchar_t *name=sb_desktop_wide(path);bool ok=name && sb_project_path_valid(path);
    if (ok) {
        for (wchar_t *p=name+3;*p;++p) if (*p==L'\\' || *p==L'/') {
            wchar_t c=*p;*p=0;DWORD flags=GetFileAttributesW(name);
            if (flags==INVALID_FILE_ATTRIBUTES) ok=CreateDirectoryW(name,NULL)!=FALSE;
            else ok=(flags&FILE_ATTRIBUTE_DIRECTORY) && !(flags&FILE_ATTRIBUTE_REPARSE_POINT);
            *p=c;if (!ok) break;
        }
    }
    if (ok) {
        DWORD flags=GetFileAttributesW(name);
        ok=flags==INVALID_FILE_ATTRIBUTES ? CreateDirectoryW(name,NULL)!=FALSE : (flags&FILE_ATTRIBUTE_DIRECTORY) && !(flags&FILE_ATTRIBUTE_REPARSE_POINT);
    }
    if (ok && empty) {
        size_t n=wcslen(name)+3;wchar_t *pattern=calloc(n,sizeof(*pattern));if (pattern) swprintf(pattern,n,L"%ls\\*",name);
        WIN32_FIND_DATAW item;HANDLE find=pattern ? FindFirstFileW(pattern,&item) : INVALID_HANDLE_VALUE;free(pattern);
        ok=find!=INVALID_HANDLE_VALUE;
        if (ok) { do { if (wcscmp(item.cFileName,L".") && wcscmp(item.cFileName,L"..")) { ok=false;break; } } while (FindNextFileW(find,&item));FindClose(find); }
    }
    free(name);if (!ok) sb_fail(error,empty ? "项目存储位置必须是可写的本地空目录；不能使用联接、符号链接或已有文件的目录。" : "无法创建项目目录，请检查路径、权限及联接或符号链接。");return ok;
}
static bool cancelled(HANDLE event) {return event && WaitForSingleObject(event,0)==WAIT_OBJECT_0;}
static bool file_hash(const wchar_t *path,char hash[65],HANDLE cancel,sb_error *error) {
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,NULL);
    BCRYPT_ALG_HANDLE algorithm=NULL;BCRYPT_HASH_HANDLE digest=NULL;unsigned char bytes[65536],result[32];DWORD got=0;
    bool ok=file!=INVALID_HANDLE_VALUE && BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,NULL,0)==0 && BCryptCreateHash(algorithm,&digest,NULL,0,NULL,0,0)==0;
    while(ok && !cancelled(cancel)) {ok=ReadFile(file,bytes,sizeof(bytes),&got,NULL)!=FALSE;if(!ok || !got)break;ok=BCryptHashData(digest,bytes,got,0)==0;}
    ok=ok && !cancelled(cancel) && BCryptFinishHash(digest,result,sizeof(result),0)==0;
    if(ok)for(size_t i=0;i<sizeof(result);++i)snprintf(hash+i*2,3,"%02x",result[i]);
    if(digest)BCryptDestroyHash(digest);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);
    if(!ok)sb_fail(error,cancelled(cancel)?"迁移已取消，原项目目录保留。":"无法读取或核验迁移文件，原项目目录保留。");return ok;
}
static DWORD CALLBACK copy_progress(LARGE_INTEGER total,LARGE_INTEGER transferred,LARGE_INTEGER stream_size,LARGE_INTEGER stream_transferred,
                                    DWORD stream,DWORD reason,HANDLE source,HANDLE destination,LPVOID context) {
    (void)total;(void)transferred;(void)stream_size;(void)stream_transferred;(void)stream;(void)reason;(void)source;(void)destination;
    return cancelled((HANDLE)context) ? PROGRESS_CANCEL : PROGRESS_CONTINUE;
}
static bool copy_directory(const wchar_t *source,const wchar_t *destination,unsigned depth,size_t *objects,HANDLE cancel,sb_error *error) {
    if (depth>64 || *objects>100000) { sb_fail(error,"迁移超过目录层级或文件数量上限，原目录保留。");return false; }
    HANDLE directory_pin=CreateFileW(source,FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    BY_HANDLE_FILE_INFORMATION directory_info={0};
    if (directory_pin==INVALID_HANDLE_VALUE || !GetFileInformationByHandle(directory_pin,&directory_info) || (directory_info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)) {if(directory_pin!=INVALID_HANDLE_VALUE)CloseHandle(directory_pin);return false;}
    size_t n=wcslen(source)+3;wchar_t *pattern=calloc(n,sizeof(*pattern));if (pattern) swprintf(pattern,n,L"%ls\\*",source);
    WIN32_FIND_DATAW item;HANDLE find=pattern ? FindFirstFileW(pattern,&item) : INVALID_HANDLE_VALUE;free(pattern);if (find==INVALID_HANDLE_VALUE) {CloseHandle(directory_pin);return false;}
    bool ok=true;DWORD end=ERROR_NO_MORE_FILES;
    do {
        if(cancelled(cancel)){ok=false;break;}
        if (!wcscmp(item.cFileName,L".") || !wcscmp(item.cFileName,L"..")) {if (!FindNextFileW(find,&item)) {end=GetLastError();break;}continue;}
        if (++*objects>100000 || (item.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)) { ok=false;break; }
        size_t sl=wcslen(source)+wcslen(item.cFileName)+2,dl=wcslen(destination)+wcslen(item.cFileName)+2;
        wchar_t *src=calloc(sl,sizeof(*src)),*dst=calloc(dl,sizeof(*dst));
        if (src) swprintf(src,sl,L"%ls\\%ls",source,item.cFileName);if (dst) swprintf(dst,dl,L"%ls\\%ls",destination,item.cFileName);
        ok=src && dst;
        if (ok && (item.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)) ok=CreateDirectoryW(dst,NULL) && copy_directory(src,dst,depth+1,objects,cancel,error);
        else if (ok) {
            HANDLE pin=CreateFileW(src,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,NULL);BY_HANDLE_FILE_INFORMATION info={0};
            ok=pin!=INVALID_HANDLE_VALUE && GetFileInformationByHandle(pin,&info) && info.nNumberOfLinks==1 && !(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT);
            char before[65],after[65];if (ok) ok=file_hash(src,before,cancel,error) && CopyFileExW(src,dst,copy_progress,cancel,NULL,COPY_FILE_FAIL_IF_EXISTS) && file_hash(dst,after,cancel,error) && !strcmp(before,after);
            if (ok) {
                PSECURITY_DESCRIPTOR sd=NULL;PACL dacl=NULL;SECURITY_DESCRIPTOR_CONTROL control=0;DWORD revision=0;
                ok=GetNamedSecurityInfoW(src,SE_FILE_OBJECT,DACL_SECURITY_INFORMATION,NULL,NULL,&dacl,NULL,&sd)==ERROR_SUCCESS &&
                    GetSecurityDescriptorControl(sd,&control,&revision) && SetNamedSecurityInfoW(dst,SE_FILE_OBJECT,
                    DACL_SECURITY_INFORMATION|((control&SE_DACL_PROTECTED)?PROTECTED_DACL_SECURITY_INFORMATION:UNPROTECTED_DACL_SECURITY_INFORMATION),NULL,NULL,dacl,NULL)==ERROR_SUCCESS;
                if (sd) LocalFree(sd);
            }
            if (pin!=INVALID_HANDLE_VALUE) CloseHandle(pin);
        }
        free(src);free(dst);if (!ok) break;
        if (!FindNextFileW(find,&item)) { end=GetLastError();break; }
    } while (true);
    FindClose(find);CloseHandle(directory_pin);return ok && end==ERROR_NO_MORE_FILES;
}
bool sb_desktop_copy_tree(const char *source,const char *destination,HANDLE cancel,sb_error *error) {
    if (sb_desktop_overlap(source,destination) || !sb_desktop_make_directory(destination,true,error)) return false;
    wchar_t *src=sb_desktop_wide(source),*dst=sb_desktop_wide(destination);size_t objects=0;
    bool ok=src && dst && copy_directory(src,dst,0,&objects,cancel,error);free(src);free(dst);
    if (!ok && !*error->text) sb_fail(error,"项目迁移未完成。原目录保持完整；新目录中的已复制文件保留，未切换配置。请检查占用、联接或硬链接。");return ok;
}
