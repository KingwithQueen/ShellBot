#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <windowsx.h>
#include <sddl.h>
#include <shellapi.h>
#include <wtsapi32.h>
#include <wincodec.h>
#include <objbase.h>
#include "desktop.h"
#include "app_icon.h"
#include "execution.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define DESKTOP_JOB (WM_APP+51)
#define OBSERVATIONS 8
#define WINDOWS_LIMIT 128
#define MONITORS_LIMIT 16
#define OBSERVATION_MS 30000
#define PNG_LIMIT SB_IMAGE_LIMIT
#define CLIPBOARD_BYTES (1024u*1024u)
typedef struct { RECT rect; wchar_t name[32]; bool primary; unsigned dpi; } monitor;
typedef struct { char id[33]; HWND hwnd; DWORD pid,tid; FILETIME created; RECT rect; } window_ref;
typedef struct {
    char id[33],layout[65]; uint64_t at; HWND foreground; DWORD foreground_pid;
    LONG input_epoch; RECT foreground_rect; RECT captured; bool foreground_rect_known,capture,invalidated;
    window_ref windows[WINDOWS_LIMIT]; unsigned window_count;
} observation;
typedef struct {
    volatile LONG references,cancelled; HANDLE done; bool prepare;
    char actor[64],name[32]; char *arguments,*reply; uint64_t deadline;
} desktop_job;
struct sb_desktop {
    sb_host *host; sb_desktop_policy policy; HANDLE thread,ready,mutex,pause,epoch_map;
    volatile LONG *epoch;
    HWND window; HHOOK keyboard,mouse; HANDLE foreground_hook;
    volatile LONG available,input_epoch,physical_inputs,foreign_inputs,truncated_inputs; DWORD session;
    ULONG_PTR input_tag; bool leased,was_paused,hotkey;
    char lease_id[33],actor[64]; uint64_t deadline;
    unsigned next_observation,emergency_stops; observation observations[OBSERVATIONS];
    wchar_t sid[192]; DWORD integrity;
};
static sb_desktop *hook_context;
struct sb_win_desktop_session { HANDLE mutex,pause,mapping; volatile LONG *epoch; };

static bool nonce(char id[33]) {
    unsigned char bytes[16]; sb_error error={0}; if (!sb_platform_random(bytes,sizeof(bytes),&error)) return false;
    for (unsigned i=0;i<16;++i) snprintf(id+i*2,3,"%02x",bytes[i]); return true;
}
static bool hex(const char *text) { return text && strlen(text)==32 && strspn(text,"0123456789abcdef")==32; }
static wchar_t *wide(const char *text) {
    int n=text ? MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text,-1,NULL,0) : 0;
    wchar_t *value=n ? malloc((size_t)n*sizeof(wchar_t)) : NULL;
    if (value && !MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text,-1,value,n)) { free(value); value=NULL; }
    return value;
}
static char *utf8(const wchar_t *value) {
    int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value,-1,NULL,0,NULL,NULL);
    char *text=n ? malloc((size_t)n) : NULL;
    if (text && !WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value,-1,text,n,NULL,NULL)) { free(text); text=NULL; }
    return text;
}
static yyjson_mut_val *object(yyjson_mut_doc *doc) { return yyjson_mut_obj(doc); }
static char *result(yyjson_mut_doc *doc, yyjson_mut_val *data, const char *error, const char *png) {
    yyjson_mut_val *root=object(doc),*content=yyjson_mut_arr(doc),*entry=object(doc);
    if (error) yyjson_mut_obj_add_strcpy(doc,data,"error",error);
    yyjson_mut_obj_add_bool(doc,root,"isError",error!=NULL); yyjson_mut_obj_add_val(doc,root,"structuredContent",data);
    char *text=yyjson_mut_val_write(data,0,NULL);
    yyjson_mut_obj_add_str(doc,entry,"type","text"); yyjson_mut_obj_add_strcpy(doc,entry,"text",text?text:"{}"); free(text);
    yyjson_mut_arr_add_val(content,entry);
    if (png) { yyjson_mut_val *image=object(doc); yyjson_mut_obj_add_str(doc,image,"type","image");
        yyjson_mut_obj_add_str(doc,image,"mimeType","image/png"); yyjson_mut_obj_add_strcpy(doc,image,"data",png); yyjson_mut_arr_add_val(content,image); }
    yyjson_mut_obj_add_val(doc,root,"content",content); return yyjson_mut_val_write(root,0,NULL);
}
static char *failed(const char *error) {
    yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL); if (!doc) return NULL;
    char *wire=result(doc,object(doc),error,NULL); yyjson_mut_doc_free(doc); return wire;
}
static void rect_json(yyjson_mut_doc *doc, yyjson_mut_val *data, const char *name, const RECT *rect) {
    yyjson_mut_val *value=object(doc); yyjson_mut_obj_add_int(doc,value,"x",rect->left); yyjson_mut_obj_add_int(doc,value,"y",rect->top);
    yyjson_mut_obj_add_int(doc,value,"width",(int64_t)rect->right-rect->left); yyjson_mut_obj_add_int(doc,value,"height",(int64_t)rect->bottom-rect->top);
    yyjson_mut_obj_add_val(doc,data,name,value);
}
static bool token_identity(HANDLE process, wchar_t sid[192], DWORD *integrity) {
    HANDLE token=NULL; DWORD bytes=0; bool ok=false;
    if (!OpenProcessToken(process,TOKEN_QUERY,&token)) return false;
    GetTokenInformation(token,TokenUser,NULL,0,&bytes); TOKEN_USER *user=malloc(bytes);
    if (user && GetTokenInformation(token,TokenUser,user,bytes,&bytes)) {
        LPWSTR value=NULL;
        if (ConvertSidToStringSidW(user->User.Sid,&value) && wcslen(value)<192) { wcscpy(sid,value); ok=true; } LocalFree(value);
    }
    free(user); GetTokenInformation(token,TokenIntegrityLevel,NULL,0,&bytes); TOKEN_MANDATORY_LABEL *label=malloc(bytes);
    if (!label || !GetTokenInformation(token,TokenIntegrityLevel,label,bytes,&bytes)) ok=false;
    else *integrity=*GetSidSubAuthority(label->Label.Sid,*GetSidSubAuthorityCount(label->Label.Sid)-1);
    free(label); CloseHandle(token); return ok;
}
static bool shared_objects(HANDLE *mutex, HANDLE *pause, HANDLE *epoch_map, volatile LONG **epoch, DWORD *session, wchar_t sid[192], DWORD *integrity,bool owner) {
    if (!ProcessIdToSessionId(GetCurrentProcessId(),session) || !*session || !token_identity(GetCurrentProcess(),sid,integrity) ||
        *integrity<SECURITY_MANDATORY_MEDIUM_RID || (*integrity>=SECURITY_MANDATORY_HIGH_RID && (!owner || !sb_win_elevated_owner()))) return false;
    wchar_t sddl[512],name[256]; PSECURITY_DESCRIPTOR sd=NULL;
    swprintf(sddl,512,L"D:P(A;;GA;;;%ls)(A;;GA;;;SY)S:(ML;;NWNR;;;ME)",sid);
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl,SDDL_REVISION_1,&sd,NULL)) return false;
    SECURITY_ATTRIBUTES attributes={sizeof(attributes),sd,FALSE};
    swprintf(name,256,L"Local\\ShellBot.Desktop.v1.%ls.%lu.Control",sid,*session); *mutex=CreateMutexW(&attributes,FALSE,name);
    swprintf(name,256,L"Local\\ShellBot.Desktop.v1.%ls.%lu.Paused",sid,*session); *pause=CreateEventW(&attributes,TRUE,FALSE,name);
    swprintf(name,256,L"Local\\ShellBot.Desktop.v1.%ls.%lu.Epoch",sid,*session);
    *epoch_map=CreateFileMappingW(INVALID_HANDLE_VALUE,&attributes,PAGE_READWRITE,0,sizeof(LONG),name);
    *epoch=*epoch_map ? MapViewOfFile(*epoch_map,FILE_MAP_WRITE,0,0,sizeof(LONG)) : NULL;
    LocalFree(sd); if (!*mutex || !*pause || !*epoch) {
        if (*mutex) CloseHandle(*mutex); if (*pause) CloseHandle(*pause); if (*epoch_map) CloseHandle(*epoch_map);
        *mutex=*pause=*epoch_map=NULL; return false;
    }
    return true;
}
sb_win_desktop_session *sb_win_desktop_session_new(void) {
    sb_win_desktop_session *state=calloc(1,sizeof(*state)); DWORD session=0,integrity=0; wchar_t sid[192]={0};
    if (state && !shared_objects(&state->mutex,&state->pause,&state->mapping,&state->epoch,&session,sid,&integrity,true)) { free(state); state=NULL; }
    return state;
}
void sb_win_desktop_session_free(sb_win_desktop_session *state) {
    if (state) { UnmapViewOfFile((const void *)state->epoch); CloseHandle(state->mapping); CloseHandle(state->mutex); CloseHandle(state->pause); free(state); }
}
static bool interactive(sb_desktop *desktop) {
    LPWSTR value=NULL; DWORD bytes=0; bool active=WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE,desktop->session,WTSConnectState,&value,&bytes) &&
        bytes>=sizeof(WTS_CONNECTSTATE_CLASS) && *(WTS_CONNECTSTATE_CLASS *)value==WTSActive;
    WTSFreeMemory(value); if (!active) return false;
    wchar_t station[64]={0},current[64]={0},input[64]={0};
    if (!GetUserObjectInformationW(GetProcessWindowStation(),UOI_NAME,station,sizeof(station),&bytes) || wcscmp(station,L"WinSta0") ||
        !GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()),UOI_NAME,current,sizeof(current),&bytes) || wcscmp(current,L"Default")) return false;
    HDESK desk=OpenInputDesktop(0,FALSE,DESKTOP_READOBJECTS);
    bool ok=desk && GetUserObjectInformationW(desk,UOI_NAME,input,sizeof(input),&bytes) && !wcscmp(input,current);
    if (desk) CloseDesktop(desk); return ok;
}
typedef struct { monitor items[MONITORS_LIMIT]; unsigned count; bool overflow; } topology;
static BOOL CALLBACK monitor_visit(HMONITOR handle,HDC dc,LPRECT rectangle,LPARAM parameter) {
    (void)dc; (void)rectangle; topology *list=(topology *)parameter;
    if (list->count>=MONITORS_LIMIT) { list->overflow=true; return FALSE; }
    MONITORINFOEXW info={0}; info.cbSize=sizeof(info);
    if (!GetMonitorInfoW(handle,(MONITORINFO *)&info)) return FALSE;
    monitor *item=&list->items[list->count++]; item->rect=info.rcMonitor; item->primary=(info.dwFlags&MONITORINFOF_PRIMARY)!=0; wcscpy(item->name,info.szDevice);
    typedef HRESULT (WINAPI *dpi_query)(HMONITOR,int,UINT *,UINT *);
    HMODULE library=LoadLibraryExW(L"shcore.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);
    dpi_query query=library ? (dpi_query)(void *)GetProcAddress(library,"GetDpiForMonitor") : NULL;
    UINT x=96,y=96; if (query) query(handle,0,&x,&y); item->dpi=x; if (library) FreeLibrary(library); return TRUE;
}
static bool layout(topology *list, char hash[65]) {
    memset(list,0,sizeof(*list));
    if (!EnumDisplayMonitors(NULL,NULL,monitor_visit,(LPARAM)list) || !list->count || list->overflow) return false;
    /* Sort by device name for stable indices, not EnumDisplayMonitors order. */
    for (unsigned i=1;i<list->count;++i) for (unsigned j=i;j && wcscmp(list->items[j-1].name,list->items[j].name)>0;--j) {
        monitor swap=list->items[j-1]; list->items[j-1]=list->items[j]; list->items[j]=swap;
    }
    sb_error error={0}; return sb_platform_sha256(list,sizeof(*list),hash,&error);
}
static void invalidate(sb_desktop *desktop) { for(unsigned i=0;i<OBSERVATIONS;++i)desktop->observations[i].invalidated=true; }
static void release_lease(sb_desktop *desktop) {
    if (desktop->hotkey) { UnregisterHotKey(desktop->window,1); desktop->hotkey=false; }
    if (desktop->keyboard) UnhookWindowsHookEx(desktop->keyboard); if (desktop->mouse) UnhookWindowsHookEx(desktop->mouse);
    desktop->keyboard=desktop->mouse=NULL;
    if (desktop->leased) ReleaseMutex(desktop->mutex);
    desktop->leased=false; *desktop->lease_id=*desktop->actor=0; desktop->deadline=0; invalidate(desktop);
}
static void pause_local(sb_desktop *desktop) {
    InterlockedIncrement(desktop->epoch); SetEvent(desktop->pause);
}
static LRESULT CALLBACK keyboard_hook(int code,WPARAM kind,LPARAM parameter) {
    if (code>=0 && hook_context) {
        KBDLLHOOKSTRUCT *key=(KBDLLHOOKSTRUCT *)parameter;
        if (!(key->flags&LLKHF_INJECTED) || key->dwExtraInfo!=hook_context->input_tag) {
            InterlockedIncrement(key->flags&LLKHF_INJECTED ? &hook_context->foreign_inputs : &hook_context->physical_inputs);
            pause_local(hook_context);
        }
    }
    return CallNextHookEx(NULL,code,kind,parameter);
}
static LRESULT CALLBACK mouse_hook(int code,WPARAM kind,LPARAM parameter) {
    if (code>=0 && hook_context) {
        MSLLHOOKSTRUCT *mouse=(MSLLHOOKSTRUCT *)parameter;
        if (!(mouse->flags&LLMHF_INJECTED) || mouse->dwExtraInfo!=hook_context->input_tag) {
            if ((mouse->flags&LLMHF_INJECTED) && (DWORD)mouse->dwExtraInfo==(DWORD)hook_context->input_tag && mouse->dwExtraInfo!=hook_context->input_tag) InterlockedIncrement(&hook_context->truncated_inputs);
            InterlockedIncrement(mouse->flags&LLMHF_INJECTED ? &hook_context->foreign_inputs : &hook_context->physical_inputs);
            pause_local(hook_context);
        }
    }
    return CallNextHookEx(NULL,code,kind,parameter);
}
static bool integer(yyjson_val *args,const char *name,int fallback,int low,int high,int *output) {
    yyjson_val *value=yyjson_obj_get(args,name); *output=fallback;
    if (!value) return true;
    if (!yyjson_is_int(value) || (yyjson_is_uint(value) && yyjson_get_uint(value)>INT_MAX)) return false;
    int64_t number=yyjson_get_sint(value); if (number<low || number>high) return false; *output=(int)number; return true;
}
static WORD key_code(const char *name) {
    if (!name) return 0;
    if (strlen(name)==1 && name[0]>='a' && name[0]<='z') return (WORD)(name[0]-'a'+'A');
    if (strlen(name)==1 && ((name[0]>='A' && name[0]<='Z') || (name[0]>='0' && name[0]<='9'))) return (WORD)name[0];
    static const struct { const char *name; WORD key; } keys[]={
        {"CTRL",VK_CONTROL},{"ALT",VK_MENU},{"SHIFT",VK_SHIFT},{"ENTER",VK_RETURN},{"TAB",VK_TAB},{"ESC",VK_ESCAPE},
        {"BACKSPACE",VK_BACK},{"DELETE",VK_DELETE},{"HOME",VK_HOME},{"END",VK_END},{"PAGEUP",VK_PRIOR},{"PAGEDOWN",VK_NEXT},
        {"UP",VK_UP},{"DOWN",VK_DOWN},{"LEFT",VK_LEFT},{"RIGHT",VK_RIGHT},{"SPACE",VK_SPACE}};
    for (size_t i=0;i<sizeof(keys)/sizeof(keys[0]);++i) if (!_stricmp(keys[i].name,name)) return keys[i].key;
    if(!_stricmp(name,"CONTROL"))return VK_CONTROL;if(!_stricmp(name,"RETURN"))return VK_RETURN;if(!_stricmp(name,"ESCAPE"))return VK_ESCAPE;
    for (unsigned i=1;i<=12;++i) { char text[4]; snprintf(text,sizeof(text),"F%u",i); if (!_stricmp(name,text)) return (WORD)(VK_F1+i-1); }
    return 0;
}
typedef struct { WORD codes[4]; unsigned count; } key_chord;
static bool modifier(WORD key) { return key==VK_CONTROL || key==VK_MENU || key==VK_SHIFT; }
static const char *chord_parse(yyjson_val *args,unsigned emergency,key_chord *chord) {
    memset(chord,0,sizeof(*chord));yyjson_val *keys=yyjson_obj_get(args,"keys"),*main=yyjson_obj_get(args,"key"),*mods=yyjson_obj_get(args,"modifiers");
    if(keys) {
        if(main || mods)return "Use key with optional modifiers, or legacy keys; do not mix the two formats.";
        if(!yyjson_is_arr(keys) || !yyjson_arr_size(keys) || yyjson_arr_size(keys)>4)return "keys must contain one main key and at most three modifiers.";
        chord->count=(unsigned)yyjson_arr_size(keys);
        for(unsigned i=0;i<chord->count;++i)chord->codes[i]=key_code(sb_string(yyjson_arr_get(keys,i)));
    } else {
        WORD code=key_code(sb_string(main));if(!code || modifier(code))return "key must name a supported non-modifier key, such as ENTER or V.";
        if(mods && (!yyjson_is_arr(mods) || yyjson_arr_size(mods)>3))return "modifiers must be an array of at most three CTRL, ALT or SHIFT names.";
        for(size_t i=0;i<yyjson_arr_size(mods);++i){WORD mod=key_code(sb_string(yyjson_arr_get(mods,i)));if(!modifier(mod))return "modifiers accepts only CTRL/CONTROL, ALT and SHIFT.";chord->codes[chord->count++]=mod;}
        chord->codes[chord->count++]=code;
    }
    bool control=false,alt=false;
    for(unsigned i=0;i<chord->count;++i){WORD code=chord->codes[i];
        if(!code || (i+1<chord->count && !modifier(code)) || (i+1==chord->count && modifier(code)))return "Use modifiers followed by one supported main key.";
        for(unsigned j=0;j<i;++j)if(code==chord->codes[j])return "Duplicate key or modifier names are not allowed.";
        if(code==VK_CONTROL)control=true;if(code==VK_MENU)alt=true;
    }
    WORD final=chord->codes[chord->count-1];
    if(control && alt && (final==VK_DELETE || final==(WORD)(VK_F1+emergency-1)))return "The secure-attention and configured emergency-stop combinations cannot be injected.";
    return NULL;
}
static void key_name(WORD key,char name[16]) {
    static const struct {WORD key;const char *name;} names[]={{VK_CONTROL,"CTRL"},{VK_MENU,"ALT"},{VK_SHIFT,"SHIFT"},{VK_RETURN,"ENTER"},{VK_TAB,"TAB"},{VK_ESCAPE,"ESC"},{VK_BACK,"BACKSPACE"},{VK_DELETE,"DELETE"},{VK_HOME,"HOME"},{VK_END,"END"},{VK_PRIOR,"PAGEUP"},{VK_NEXT,"PAGEDOWN"},{VK_UP,"UP"},{VK_DOWN,"DOWN"},{VK_LEFT,"LEFT"},{VK_RIGHT,"RIGHT"},{VK_SPACE,"SPACE"}};
    for(size_t i=0;i<sizeof(names)/sizeof(*names);++i)if(names[i].key==key){strcpy(name,names[i].name);return;}
    if(key>=VK_F1 && key<=VK_F12)snprintf(name,16,"F%u",(unsigned)(key-VK_F1+1));else {name[0]=(char)key;name[1]=0;}
}
static bool arguments(sb_desktop *desktop,const char *name,yyjson_val *args) {
    int tool=sb_project_tool_index(name),n; if (tool<SB_DESKTOP_TOOL_FIRST || tool>=SB_DESKTOP_TOOL_END || !yyjson_is_obj(args)) return false;
    const char *allowed[20]={"lease_id","request_key","approval_id"}; size_t count=3;
    if (tool==15) { allowed[count++]="action"; allowed[count++]="duration_seconds"; }
    if (tool>=19 && tool<=24) allowed[count++]="screen_id";
    if (tool==17) { const char *fields[]={"monitor_id","max_dimension","x","y","width","height"}; for (unsigned i=0;i<6;++i) allowed[count++]=fields[i]; }
    if (tool==19) allowed[count++]="window_id";
    if (tool>=20 && tool<=22) { allowed[count++]="x"; allowed[count++]="y"; }
    if (tool==20 || tool==21) allowed[count++]="button";
    if (tool==20) allowed[count++]="clicks";
    if (tool==21) { allowed[count++]="to_x"; allowed[count++]="to_y"; allowed[count++]="duration_ms"; }
    if (tool==22) { allowed[count++]="delta"; allowed[count++]="horizontal"; }
    if (tool==23 || tool==26) allowed[count++]="text";
    if (tool==24) {allowed[count++]="keys";allowed[count++]="key";allowed[count++]="modifiers";}
    if (tool==25 || tool==26) allowed[count++]="expected_sequence";
    if (!sb_config_keys(args,allowed,count)) return false;
    const char *action=sb_string(yyjson_obj_get(args,"action")),*lease=sb_string(yyjson_obj_get(args,"lease_id")),*key=sb_string(yyjson_obj_get(args,"request_key"));
    bool cleanup=tool==15 && action && (!strcmp(action,"status") || !strcmp(action,"release"));
    if (tool==15 && (!action || (strcmp(action,"acquire") && strcmp(action,"renew") && strcmp(action,"release") && strcmp(action,"status")))) return false;
    if ((!cleanup && (!key || !*key || strlen(key)>64 || strspn(key,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.")!=strlen(key))) ||
        (yyjson_obj_get(args,"request_key") && (!key || !*key || strlen(key)>64))) return false;
    if ((tool!=15 || (action && (!strcmp(action,"renew") || !strcmp(action,"release")))) && !hex(lease)) return false;
    if (yyjson_obj_get(args,"lease_id") && !hex(lease)) return false;
    if (yyjson_obj_get(args,"approval_id") && !hex(sb_string(yyjson_obj_get(args,"approval_id")))) return false;
    if (tool>=19 && tool<=24 && !hex(sb_string(yyjson_obj_get(args,"screen_id")))) return false;
    if (tool==19 && !hex(sb_string(yyjson_obj_get(args,"window_id")))) return false;
    if (!integer(args,"duration_seconds",(int)desktop->policy.lease_seconds,1,(int)desktop->policy.lease_seconds,&n) ||
        !integer(args,"monitor_id",0,0,15,&n) || !integer(args,"max_dimension",1600,200,SB_IMAGE_DIMENSION,&n) ||
        !integer(args,"clicks",1,1,2,&n) || !integer(args,"duration_ms",200,50,1000,&n)) return false;
    const char *coordinates[]={"x","y","to_x","to_y"};
    for (unsigned i=0;i<4;++i) if (!integer(args,coordinates[i],0,-32768,32767,&n)) return false;
    if ((tool>=20 && tool<=22) && (!yyjson_obj_get(args,"x") || !yyjson_obj_get(args,"y"))) return false;
    if (tool==21 && (!yyjson_obj_get(args,"to_x") || !yyjson_obj_get(args,"to_y"))) return false;
    if (!integer(args,"width",0,1,32768,&n) || !integer(args,"height",0,1,32768,&n)) return false;
    unsigned crop=0; const char *crop_keys[]={"x","y","width","height"};
    if (tool==17) { for (unsigned i=0;i<4;++i) if (yyjson_obj_get(args,crop_keys[i])) ++crop; if (crop && crop!=4) return false; }
    const char *button=sb_string(yyjson_obj_get(args,"button"));
    if (yyjson_obj_get(args,"button") && (!button || (strcmp(button,"left") && strcmp(button,"right") && strcmp(button,"middle")))) return false;
    yyjson_val *horizontal=yyjson_obj_get(args,"horizontal"); if (horizontal && !yyjson_is_bool(horizontal)) return false;
    if (tool==22 && (!yyjson_obj_get(args,"delta") || !integer(args,"delta",0,-1200,1200,&n) || !n)) return false;
    if (tool==23 || tool==26) {
        const char *text=sb_string(yyjson_obj_get(args,"text")); if (!text || strlen(text)>(tool==23 ? 4096u : CLIPBOARD_BYTES/2u-1u) || (tool==23 && !*text)) return false;
        wchar_t *value=wide(text); bool ok=value && (tool!=23 || wcslen(value)<=512); free(value); if (!ok) return false;
    }
    if (tool==24) {
        key_chord chord;if(chord_parse(args,desktop->policy.emergency_function_key,&chord))return false;
    }
    if (tool==25 || tool==26) { uint64_t sequence; if (!sb_integer(yyjson_obj_get(args,"expected_sequence"),&sequence) || sequence>UINT32_MAX) return false; }
    if (tool==15 && strcmp(action,"acquire") && yyjson_obj_get(args,"duration_seconds") && strcmp(action,"renew")) return false;
    return true;
}
static char *canonical(yyjson_val *args) {
    const char *keys[20]; size_t count=0,i,n; yyjson_val *key,*value;
    yyjson_obj_foreach(args,i,n,key,value) { (void)value; const char *name=sb_string(key); if (strcmp(name,"approval_id")) keys[count++]=name; }
    for (size_t a=1;a<count;++a) for (size_t b=a;b && strcmp(keys[b-1],keys[b])>0;--b) { const char *swap=keys[b-1]; keys[b-1]=keys[b]; keys[b]=swap; }
    yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL); if (!doc) return NULL; yyjson_mut_val *root=object(doc);
    for (i=0;i<count;++i) yyjson_mut_obj_add_val(doc,root,keys[i],yyjson_val_mut_copy(doc,yyjson_obj_get(args,keys[i])));
    char *text=yyjson_mut_val_write(root,0,NULL); yyjson_mut_doc_free(doc); return text;
}
static bool owns(sb_desktop *desktop,const char *actor,const char *lease) {
    return desktop->leased && sb_platform_millis()<desktop->deadline && actor && !strcmp(actor,desktop->actor) && lease && !strcmp(lease,desktop->lease_id);
}
static bool window_identity(window_ref *reference) {
    DWORD pid=0; DWORD tid=GetWindowThreadProcessId(reference->hwnd,&pid); RECT rect;
    if (!IsWindowVisible(reference->hwnd) || IsIconic(reference->hwnd) || pid!=reference->pid || tid!=reference->tid ||
        !GetWindowRect(reference->hwnd,&rect) || !EqualRect(&rect,&reference->rect)) return false;
    HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid); FILETIME created={0},exit,kernel,user;
    bool ok=process && GetProcessTimes(process,&created,&exit,&kernel,&user) && !memcmp(&created,&reference->created,sizeof(created));
    if (process) CloseHandle(process); return ok;
}
static bool input_permitted(sb_desktop *desktop,HWND window) {
    DWORD pid=0,session=0,integrity=0; GetWindowThreadProcessId(window,&pid); wchar_t sid[192]={0};
    if (!pid || !ProcessIdToSessionId(pid,&session) || session!=desktop->session) return false;
    HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    bool ok=process && token_identity(process,sid,&integrity) && !wcscmp(sid,desktop->sid) && integrity<=desktop->integrity;
    if (process) CloseHandle(process); return ok;
}
static yyjson_mut_val *change(yyjson_mut_doc *doc,yyjson_mut_val *changes,const char *field,window_ref *window) {
    yyjson_mut_val *value=object(doc);yyjson_mut_obj_add_str(doc,value,"field",field);
    if(window){yyjson_mut_obj_add_strcpy(doc,value,"window_id",window->id);yyjson_mut_obj_add_uint(doc,value,"pid",window->pid);}
    yyjson_mut_arr_add_val(changes,value);return value;
}
static void change_uint(yyjson_mut_doc *doc,yyjson_mut_val *changes,const char *field,uint64_t before,uint64_t after,window_ref *window) {
    if(before==after)return;yyjson_mut_val *v=change(doc,changes,field,window);yyjson_mut_obj_add_uint(doc,v,"expected",before);yyjson_mut_obj_add_uint(doc,v,"actual",after);
}
static void change_rect(yyjson_mut_doc *doc,yyjson_mut_val *changes,const char *field,const RECT *before,const RECT *after,window_ref *window) {
    if(EqualRect(before,after))return;yyjson_mut_val *v=change(doc,changes,field,window);rect_json(doc,v,"expected",before);rect_json(doc,v,"actual",after);
}
static const char *observe_guard(sb_desktop *desktop,observation *view,bool input,bool require_foreground,yyjson_mut_doc *doc,yyjson_mut_val *data) {
    if (!desktop->leased || sb_platform_millis()>=desktop->deadline) return "lease_expired";
    if (!interactive(desktop)) return "no_desktop_session: locked, disconnected or secure desktop";
    if (input && WaitForSingleObject(desktop->pause,0)==WAIT_OBJECT_0) return "desktop_paused: local owner must resume";
    yyjson_mut_val *changes=yyjson_mut_arr(doc);
    if (!view || !*view->id) {
        yyjson_mut_val *v=change(doc,changes,"screen_id",NULL);yyjson_mut_obj_add_str(doc,v,"reason","unknown_or_evicted: retain only the latest eight observations");
    } else {
        yyjson_mut_obj_add_strcpy(doc,data,"screen_id",view->id);uint64_t age=sb_platform_millis()-view->at;
        if(age>OBSERVATION_MS){yyjson_mut_val *v=change(doc,changes,"observation_age_ms",NULL);yyjson_mut_obj_add_uint(doc,v,"actual",age);yyjson_mut_obj_add_uint(doc,v,"maximum",OBSERVATION_MS);}
        if(view->invalidated){yyjson_mut_val *v=change(doc,changes,"observation_valid",NULL);yyjson_mut_obj_add_bool(doc,v,"expected",true);yyjson_mut_obj_add_bool(doc,v,"actual",false);yyjson_mut_obj_add_str(doc,v,"reason","previous_input_action_or_lease_change");}
        change_uint(doc,changes,"input_epoch",(uint32_t)view->input_epoch,(uint32_t)InterlockedCompareExchange(desktop->epoch,0,0),NULL);
        topology monitors;char hash[65]={0};RECT rect={0};DWORD pid=0;HWND foreground=GetForegroundWindow();GetWindowThreadProcessId(foreground,&pid);
        bool known=layout(&monitors,hash);
        if(!known || strcmp(hash,view->layout)){yyjson_mut_val *v=change(doc,changes,"monitor_layout",NULL);yyjson_mut_obj_add_strcpy(doc,v,"expected",view->layout);if(known)yyjson_mut_obj_add_strcpy(doc,v,"actual",hash);else yyjson_mut_obj_add_null(doc,v,"actual");}
        window_ref *previous=NULL;for(unsigned i=0;i<view->window_count;++i)if(view->windows[i].hwnd==view->foreground)previous=&view->windows[i];
        change_uint(doc,changes,"foreground.hwnd",(uintptr_t)view->foreground,(uintptr_t)foreground,previous);
        change_uint(doc,changes,"foreground.pid",view->foreground_pid,pid,previous);
        bool got=foreground && GetWindowRect(foreground,&rect)!=0;
        if(view->foreground_rect_known!=got){yyjson_mut_val *v=change(doc,changes,"foreground.bounds",previous);
            if(view->foreground_rect_known)rect_json(doc,v,"expected",&view->foreground_rect);else yyjson_mut_obj_add_null(doc,v,"expected");
            if(got)rect_json(doc,v,"actual",&rect);else yyjson_mut_obj_add_null(doc,v,"actual");}
        else if(got && foreground==view->foreground)change_rect(doc,changes,"foreground.bounds",&view->foreground_rect,&rect,previous);
    }
    if(yyjson_mut_arr_size(changes)){
        yyjson_mut_obj_add_str(doc,data,"code","stale_observation");yyjson_mut_obj_add_val(doc,data,"changes",changes);
        yyjson_mut_obj_add_str(doc,data,"recovery","Refresh desktop_state/window_list, inspect changes, then use the new screen_id and a new request_key.");return "stale_observation";
    }
    HWND foreground=GetForegroundWindow();
    if (input && require_foreground && !input_permitted(desktop,foreground)) return "input_not_permitted: unknown owner or higher integrity target";
    return NULL;
}
static observation *find_observation(sb_desktop *desktop,const char *id) {
    for (unsigned i=0;id && i<OBSERVATIONS;++i) if (!strcmp(desktop->observations[i].id,id)) return &desktop->observations[i]; return NULL;
}
typedef struct { sb_desktop *desktop; observation *view; yyjson_mut_doc *doc; yyjson_mut_val *windows; bool overflow; } windows_context;
static BOOL CALLBACK window_visit(HWND hwnd,LPARAM parameter) {
    windows_context *context=(windows_context *)parameter; observation *view=context->view;
    if (!IsWindowVisible(hwnd) || hwnd==context->desktop->window) return TRUE;
    DWORD pid=0; DWORD tid=GetWindowThreadProcessId(hwnd,&pid),session=0;
    RECT rect; if (!pid || !ProcessIdToSessionId(pid,&session) || session!=context->desktop->session ||
        !GetWindowRect(hwnd,&rect) || rect.right<=rect.left || rect.bottom<=rect.top || IsIconic(hwnd)) return TRUE;
    if (view->window_count>=WINDOWS_LIMIT) { context->overflow=true; return FALSE; }
    HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid); FILETIME created={0},exit,kernel,user;
    bool ok=process && GetProcessTimes(process,&created,&exit,&kernel,&user); if (process) CloseHandle(process); if (!ok) return TRUE;
    window_ref *item=&view->windows[view->window_count]; memset(item,0,sizeof(*item));
    if (!nonce(item->id)) return FALSE; item->hwnd=hwnd; item->pid=pid; item->tid=tid; item->created=created; item->rect=rect; ++view->window_count;
    wchar_t title[513]={0}; GetWindowTextW(hwnd,title,513); char *text=utf8(title);
    yyjson_mut_val *value=object(context->doc); yyjson_mut_obj_add_str(context->doc,value,"window_id",item->id);
    yyjson_mut_obj_add_uint(context->doc,value,"pid",pid); yyjson_mut_obj_add_strcpy(context->doc,value,"title",text?text:""); free(text);
    yyjson_mut_obj_add_bool(context->doc,value,"foreground",hwnd==view->foreground);
    yyjson_mut_obj_add_bool(context->doc,value,"input_permitted",input_permitted(context->desktop,hwnd));
    rect_json(context->doc,value,"bounds",&rect); yyjson_mut_arr_add_val(context->windows,value); return TRUE;
}
static bool observe(sb_desktop *desktop,yyjson_mut_doc *doc,yyjson_mut_val *data,observation **output) {
    topology monitors; char hash[65]; if (!layout(&monitors,hash)) return false;
    observation *view=&desktop->observations[desktop->next_observation++%OBSERVATIONS]; memset(view,0,sizeof(*view));
    if (!nonce(view->id)) return false;
    view->at=sb_platform_millis(); view->input_epoch=InterlockedCompareExchange(desktop->epoch,0,0); strcpy(view->layout,hash);
    view->foreground=GetForegroundWindow(); GetWindowThreadProcessId(view->foreground,&view->foreground_pid);
    view->foreground_rect_known=view->foreground && GetWindowRect(view->foreground,&view->foreground_rect)!=0;
    yyjson_mut_val *foreground=object(doc);yyjson_mut_obj_add_bool(doc,foreground,"present",view->foreground!=NULL);
    if(view->foreground)yyjson_mut_obj_add_uint(doc,foreground,"hwnd",(uintptr_t)view->foreground);else yyjson_mut_obj_add_null(doc,foreground,"hwnd");
    yyjson_mut_obj_add_uint(doc,foreground,"pid",view->foreground_pid);
    if(view->foreground_rect_known)rect_json(doc,foreground,"bounds",&view->foreground_rect);else yyjson_mut_obj_add_null(doc,foreground,"bounds");
    yyjson_mut_obj_add_val(doc,data,"foreground",foreground);
    yyjson_mut_obj_add_str(doc,data,"screen_id",view->id); yyjson_mut_obj_add_uint(doc,data,"session_id",desktop->session);
    yyjson_mut_obj_add_uint(doc,data,"captured_at_unix_ms",sb_platform_unix_ms()); yyjson_mut_obj_add_uint(doc,data,"observation_ttl_ms",OBSERVATION_MS);
    yyjson_mut_obj_add_bool(doc,data,"paused",WaitForSingleObject(desktop->pause,0)==WAIT_OBJECT_0);
    yyjson_mut_obj_add_uint(doc,data,"clipboard_sequence",GetClipboardSequenceNumber());
    yyjson_mut_val *array=yyjson_mut_arr(doc);
    for (unsigned i=0;i<monitors.count;++i) {
        yyjson_mut_val *value=object(doc); char *name=utf8(monitors.items[i].name);
        yyjson_mut_obj_add_uint(doc,value,"monitor_id",i); yyjson_mut_obj_add_strcpy(doc,value,"device",name?name:""); free(name);
        yyjson_mut_obj_add_bool(doc,value,"primary",monitors.items[i].primary); yyjson_mut_obj_add_uint(doc,value,"dpi",monitors.items[i].dpi);
        yyjson_mut_obj_add_real(doc,value,"scale",monitors.items[i].dpi/96.0); rect_json(doc,value,"bounds",&monitors.items[i].rect); yyjson_mut_arr_add_val(array,value);
    }
    yyjson_mut_obj_add_val(doc,data,"monitors",array);
    array=yyjson_mut_arr(doc); windows_context context={desktop,view,doc,array,false}; EnumWindows(window_visit,(LPARAM)&context);
    yyjson_mut_obj_add_val(doc,data,"windows",array); yyjson_mut_obj_add_bool(doc,data,"windows_truncated",context.overflow);
    *output=view; return true;
}
typedef struct { const char *code,*stage,*method,*first_stage; DWORD win32,first_win32; HRESULT hr; uint64_t bytes; bool captured,bytes_known,fallback; } capture_diagnostic;
static void capture_report(sb_desktop *desktop,yyjson_mut_doc *doc,yyjson_mut_val *data,const capture_diagnostic *d) {
    yyjson_mut_val *value=object(doc);bool available=interactive(desktop);
    yyjson_mut_obj_add_str(doc,value,"code",d->code?d->code:"not_attempted");yyjson_mut_obj_add_str(doc,value,"stage",d->stage?d->stage:"precondition");
    yyjson_mut_obj_add_uint(doc,value,"win32_error",d->win32);yyjson_mut_obj_add_bool(doc,value,"win32_error_available",d->win32!=ERROR_SUCCESS);
    if(d->captured)yyjson_mut_obj_add_uint(doc,value,"hresult",(uint32_t)d->hr);else yyjson_mut_obj_add_null(doc,value,"hresult");
    yyjson_mut_obj_add_bool(doc,value,"desktop_session_available",available);
    if(d->code || !available)yyjson_mut_obj_add_bool(doc,value,"desktop_capturable",available && d->captured);else yyjson_mut_obj_add_null(doc,value,"desktop_capturable");
    yyjson_mut_obj_add_bool(doc,value,"pixels_captured",d->captured);
    yyjson_mut_obj_add_str(doc,value,"capture_method",d->method?d->method:"none");yyjson_mut_obj_add_bool(doc,value,"fallback_attempted",d->fallback);
    if(d->first_stage){yyjson_mut_obj_add_str(doc,value,"initial_failure_stage",d->first_stage);yyjson_mut_obj_add_uint(doc,value,"initial_win32_error",d->first_win32);}
    yyjson_mut_obj_add_str(doc,value,"desktop_status",available?"interactive_default_desktop":"locked_disconnected_secure_or_unavailable");
    yyjson_mut_obj_add_bool(doc,value,"encoded_bytes_known",d->bytes_known);
    if(d->bytes_known)yyjson_mut_obj_add_uint(doc,value,"encoded_bytes",d->bytes);else yyjson_mut_obj_add_null(doc,value,"encoded_bytes");
    yyjson_mut_obj_add_uint(doc,value,"png_limit_bytes",PNG_LIMIT);yyjson_mut_obj_add_val(doc,data,"capture_diagnostic",value);
}
/* 某些显示驱动拒绝直接缩放屏幕 DC。先按原尺寸复制到同设备内存，再在内存中缩放。 */
static bool capture_fallback(HDC screen,HDC memory,const RECT *rect,unsigned width,unsigned height,capture_diagnostic *d) {
    int w=rect->right-rect->left,h=rect->bottom-rect->top;
    if((uint64_t)w*h>SB_IMAGE_PIXEL_LIMIT){d->stage="fallback_source_pixel_budget";d->win32=ERROR_FILE_TOO_LARGE;return false;}
    d->fallback=true;d->method="BitBlt+memory_StretchBlt";d->stage="fallback.CreateCompatibleDC";SetLastError(ERROR_SUCCESS);
    HDC source=CreateCompatibleDC(screen);HBITMAP bitmap=NULL;HGDIOBJ old=NULL;bool ok=false;
    if(source){d->stage="fallback.CreateCompatibleBitmap";bitmap=CreateCompatibleBitmap(screen,w,h);}
    if(bitmap){d->stage="fallback.SelectObject";old=SelectObject(source,bitmap);}
    if(old && old!=HGDI_ERROR){
        d->stage="fallback.BitBlt";SetLastError(ERROR_SUCCESS);ok=BitBlt(source,0,0,w,h,screen,rect->left,rect->top,SRCCOPY|CAPTUREBLT)!=0;
        if(ok){d->stage="fallback.memory_StretchBlt";SetLastError(ERROR_SUCCESS);ok=StretchBlt(memory,0,0,(int)width,(int)height,source,0,0,w,h,SRCCOPY)!=0;}
        if(ok){d->stage="fallback.GdiFlush";ok=GdiFlush()!=0;}
    }
    if(!ok)d->win32=GetLastError();
    if(old && old!=HGDI_ERROR)SelectObject(source,old);if(bitmap)DeleteObject(bitmap);if(source)DeleteDC(source);return ok;
}
static char *capture_png(const RECT *rectangle,unsigned maximum,unsigned *width,unsigned *height,capture_diagnostic *diagnostic) {
    diagnostic->code="capture_failed";diagnostic->stage="validate_region";
    int source_width=rectangle->right-rectangle->left,source_height=rectangle->bottom-rectangle->top;
    if (source_width<=0 || source_height<=0 || source_width>32768 || source_height>32768){diagnostic->win32=ERROR_INVALID_PARAMETER;return NULL;}
    double scale=(source_width>source_height ? source_width : source_height)/(double)maximum;
    if (scale<1) scale=1; *width=(unsigned)(source_width/scale); *height=(unsigned)(source_height/scale);
    if (!*width) *width=1; if (!*height) *height=1;
    UINT stride=(*width*3+3)&~3u,size=stride**height; unsigned char *pixels=NULL; char *encoded=NULL;
    HDC screen=NULL,memory=NULL; HBITMAP bitmap=NULL; HGDIOBJ old=NULL;
    SetLastError(ERROR_SUCCESS);diagnostic->stage="GetDC";screen=GetDC(NULL);
    if(screen){diagnostic->stage="CreateCompatibleDC";memory=CreateCompatibleDC(screen);}
    BITMAPINFO info={0}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth=(LONG)*width;
    info.bmiHeader.biHeight=-(LONG)*height; info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=24;
    if(memory){diagnostic->stage="CreateDIBSection";bitmap=CreateDIBSection(screen,&info,DIB_RGB_COLORS,(void **)&pixels,NULL,0);}
    if (bitmap){diagnostic->stage="SelectObject";old=SelectObject(memory,bitmap);}
    bool captured=false;
    if (bitmap && old && old!=HGDI_ERROR) {
        SetStretchBltMode(memory,HALFTONE); SetBrushOrgEx(memory,0,0,NULL);
        SetLastError(ERROR_SUCCESS);diagnostic->stage="StretchBlt";diagnostic->method="screen_StretchBlt";
        captured=StretchBlt(memory,0,0,(int)*width,(int)*height,screen,rectangle->left,rectangle->top,source_width,source_height,SRCCOPY|CAPTUREBLT)!=0;
        if(!captured){diagnostic->first_stage="StretchBlt";diagnostic->first_win32=GetLastError();captured=capture_fallback(screen,memory,rectangle,*width,*height,diagnostic);}
        if(captured && !GdiFlush()){captured=false;diagnostic->stage="GdiFlush";diagnostic->win32=GetLastError();}
    }
    diagnostic->captured=captured;if(!captured && !diagnostic->first_stage)diagnostic->win32=GetLastError();
    IWICImagingFactory *factory=NULL; IWICBitmapEncoder *encoder=NULL; IWICBitmapFrameEncode *frame=NULL; IPropertyBag2 *properties=NULL; IStream *stream=NULL;
    HRESULT hr=E_FAIL;
    if(captured){diagnostic->code="png_encoding_failed";diagnostic->stage="WIC.factory";hr=CoCreateInstance(&CLSID_WICImagingFactory,NULL,CLSCTX_INPROC_SERVER,&IID_IWICImagingFactory,(void **)&factory);}
#define CAPTURE_STEP(stage_name,expression) if(SUCCEEDED(hr)){diagnostic->stage=stage_name;hr=(expression);}
    CAPTURE_STEP("WIC.stream",CreateStreamOnHGlobal(NULL,TRUE,&stream));
    CAPTURE_STEP("WIC.encoder",IWICImagingFactory_CreateEncoder(factory,&GUID_ContainerFormatPng,NULL,&encoder));
    CAPTURE_STEP("WIC.initialize_encoder",IWICBitmapEncoder_Initialize(encoder,stream,WICBitmapEncoderNoCache));
    CAPTURE_STEP("WIC.frame",IWICBitmapEncoder_CreateNewFrame(encoder,&frame,&properties));
    CAPTURE_STEP("WIC.initialize_frame",IWICBitmapFrameEncode_Initialize(frame,properties));
    CAPTURE_STEP("WIC.size",IWICBitmapFrameEncode_SetSize(frame,*width,*height));
    WICPixelFormatGUID format=GUID_WICPixelFormat24bppBGR;
    CAPTURE_STEP("WIC.pixel_format",IWICBitmapFrameEncode_SetPixelFormat(frame,&format));
    if (SUCCEEDED(hr) && !IsEqualGUID(&format,&GUID_WICPixelFormat24bppBGR)) hr=E_FAIL;
    CAPTURE_STEP("WIC.write_pixels",IWICBitmapFrameEncode_WritePixels(frame,*height,stride,size,pixels));
    CAPTURE_STEP("WIC.commit_frame",IWICBitmapFrameEncode_Commit(frame));
    CAPTURE_STEP("WIC.commit_encoder",IWICBitmapEncoder_Commit(encoder));
    STATSTG stat={0}; HGLOBAL buffer=NULL;
    CAPTURE_STEP("WIC.encoded_size",IStream_Stat(stream,&stat,STATFLAG_NONAME));
    if(SUCCEEDED(hr)){diagnostic->bytes=stat.cbSize.QuadPart;diagnostic->bytes_known=true;}
    if (SUCCEEDED(hr) && (!stat.cbSize.QuadPart || stat.cbSize.QuadPart>PNG_LIMIT)){diagnostic->code=stat.cbSize.QuadPart?"png_budget_exceeded":"png_encoding_failed";hr=HRESULT_FROM_WIN32(stat.cbSize.QuadPart?ERROR_FILE_TOO_LARGE:ERROR_INVALID_DATA);}
    CAPTURE_STEP("WIC.buffer",GetHGlobalFromStream(stream,&buffer));
    if (SUCCEEDED(hr)) {
        diagnostic->stage="base64";diagnostic->code="image_serialization_failed";void *data=GlobalLock(buffer);
        if (data) { encoded=sb_base64(data,(size_t)diagnostic->bytes); GlobalUnlock(buffer); }
        if(!data)diagnostic->win32=GetLastError();
        if(encoded){diagnostic->code="ok";diagnostic->stage="complete";}
    }
    diagnostic->hr=hr;
#undef CAPTURE_STEP
    if (properties) IPropertyBag2_Release(properties); if (frame) IWICBitmapFrameEncode_Release(frame);
    if (encoder) IWICBitmapEncoder_Release(encoder); if (factory) IWICImagingFactory_Release(factory); if (stream) IStream_Release(stream);
    if (old && old!=HGDI_ERROR) SelectObject(memory,old); if (bitmap) DeleteObject(bitmap); if (memory) DeleteDC(memory); if (screen) ReleaseDC(NULL,screen);
    return encoded;
}
static void pump(void) {
    MSG message; unsigned count=0;
    while (count++<32 && PeekMessageW(&message,NULL,0,0,PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
}
static INPUT desktop_mouse_event(sb_desktop *desktop,DWORD flags,DWORD data,int x,int y) {
    INPUT input={0}; input.type=INPUT_MOUSE; input.mi.dwFlags=flags; input.mi.mouseData=data; input.mi.dwExtraInfo=desktop->input_tag;
    if (flags&MOUSEEVENTF_MOVE) {
        int left=GetSystemMetrics(SM_XVIRTUALSCREEN),top=GetSystemMetrics(SM_YVIRTUALSCREEN);
        int width=GetSystemMetrics(SM_CXVIRTUALSCREEN),height=GetSystemMetrics(SM_CYVIRTUALSCREEN);
        input.mi.dwFlags|=MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_VIRTUALDESK;
        input.mi.dx=width>1 ? (LONG)((int64_t)(x-left)*65535/(width-1)) : 0;
        input.mi.dy=height>1 ? (LONG)((int64_t)(y-top)*65535/(height-1)) : 0;
    }
    return input;
}
static INPUT key_event(sb_desktop *desktop,WORD key,DWORD flags,bool unicode) {
    INPUT input={0}; input.type=INPUT_KEYBOARD; input.ki.dwExtraInfo=desktop->input_tag;
    if (unicode) { input.ki.wScan=key; input.ki.dwFlags=flags|KEYEVENTF_UNICODE; }
    else { input.ki.wVk=key; input.ki.dwFlags=flags;
        if (key==VK_DELETE || key==VK_HOME || key==VK_END || key==VK_PRIOR || key==VK_NEXT || (key>=VK_LEFT && key<=VK_DOWN)) input.ki.dwFlags|=KEYEVENTF_EXTENDEDKEY; }
    return input;
}
static const char *submit_chord(sb_desktop *desktop,const key_chord *chord,yyjson_mut_doc *doc,yyjson_mut_val *data,unsigned *inserted) {
    INPUT events[8],cleanup[4];unsigned count=chord->count;char label[16];
    yyjson_mut_val *keys=yyjson_mut_arr(doc),*mods=yyjson_mut_arr(doc);
    for(unsigned i=0;i<count;++i){WORD key=chord->codes[i];key_name(key,label);yyjson_mut_arr_add_strcpy(doc,keys,label);
        if(i+1<count)yyjson_mut_arr_add_strcpy(doc,mods,label);else yyjson_mut_obj_add_strcpy(doc,data,"key",label);
        events[i]=key_event(desktop,key,0,false);events[count*2-i-1]=key_event(desktop,key,KEYEVENTF_KEYUP,false);
    }
    yyjson_mut_obj_add_val(doc,data,"keys",keys);yyjson_mut_obj_add_val(doc,data,"modifiers",mods);yyjson_mut_obj_add_uint(doc,data,"events_requested",count*2);
    SetLastError(ERROR_SUCCESS);*inserted=SendInput(count*2,events,sizeof(INPUT));DWORD native=*inserted==count*2?ERROR_SUCCESS:GetLastError();
    yyjson_mut_obj_add_uint(doc,data,"win32_error",native);yyjson_mut_obj_add_bool(doc,data,"win32_error_available",native!=ERROR_SUCCESS);
    yyjson_mut_obj_add_str(doc,data,"key_submission_status",*inserted==count*2?"complete":*inserted?"partial":"not_inserted");
    /* 只释放已经提交且尚未收到 key-up 的前缀，避免为未按下的键补发释放。 */
    unsigned held=*inserted<count?*inserted:count*2-*inserted,released=0;
    if(held){for(unsigned i=0;i<held;++i)cleanup[i]=key_event(desktop,chord->codes[held-i-1],KEYEVENTF_KEYUP,false);released=SendInput(held,cleanup,sizeof(INPUT));}
    yyjson_mut_obj_add_uint(doc,data,"cleanup_events_requested",held);yyjson_mut_obj_add_uint(doc,data,"cleanup_events_inserted",released);
    return *inserted==count*2?NULL:"partial_input_submission: inspect the receipt and application state; do not automatically repeat the key or paste.";
}
static const char *coordinate_guard(observation *view,int x,int y) {
    POINT point={x,y}; HWND at=WindowFromPoint(point);
    if (!PtInRect(&view->foreground_rect,point) || !at || GetAncestor(at,GA_ROOT)!=view->foreground ||
        (view->capture && !PtInRect(&view->captured,point))) return "coordinate_outside_observed_foreground_target";
    return NULL;
}
typedef struct { bool attempted; BOOL returned; HWND before; } focus_attempt;
static yyjson_mut_val *focus_identity(yyjson_mut_doc *doc,HWND hwnd,DWORD pid,DWORD tid) {
    yyjson_mut_val *value=object(doc);DWORD session=0;
    if(hwnd)yyjson_mut_obj_add_uint(doc,value,"hwnd",(uintptr_t)hwnd);else yyjson_mut_obj_add_null(doc,value,"hwnd");
    if(pid)yyjson_mut_obj_add_uint(doc,value,"pid",pid);else yyjson_mut_obj_add_null(doc,value,"pid");
    if(tid)yyjson_mut_obj_add_uint(doc,value,"tid",tid);else yyjson_mut_obj_add_null(doc,value,"tid");
    if(pid && ProcessIdToSessionId(pid,&session))yyjson_mut_obj_add_uint(doc,value,"session_id",session);else yyjson_mut_obj_add_null(doc,value,"session_id");
    HDESK handle=tid?GetThreadDesktop(tid):NULL;DWORD error=tid && !handle?GetLastError():ERROR_NOT_FOUND,bytes=0;char *name=NULL;
    if(handle){
        GetUserObjectInformationW(handle,UOI_NAME,NULL,0,&bytes);
        if(bytes){wchar_t *buffer=calloc(1,(size_t)bytes+sizeof(wchar_t));
            if(buffer){if(GetUserObjectInformationW(handle,UOI_NAME,buffer,bytes,&bytes)){name=utf8(buffer);error=name?0:ERROR_NO_UNICODE_TRANSLATION;}else error=GetLastError();free(buffer);}
            else error=ERROR_NOT_ENOUGH_MEMORY;
        }else error=GetLastError();
    }
    yyjson_mut_obj_add_bool(doc,value,"desktop_name_known",name!=NULL);
    if(name)yyjson_mut_obj_add_strcpy(doc,value,"desktop_name",name);else yyjson_mut_obj_add_null(doc,value,"desktop_name");free(name);
    yyjson_mut_obj_add_uint(doc,value,"desktop_query_win32_error",error);return value;
}
static yyjson_mut_val *focus_window_identity(yyjson_mut_doc *doc,HWND hwnd) {
    DWORD pid=0,tid=hwnd?GetWindowThreadProcessId(hwnd,&pid):0;return focus_identity(doc,hwnd,pid,tid);
}
static void focus_report(yyjson_mut_doc *doc,yyjson_mut_val *data,window_ref *target,const focus_attempt *attempt) {
    yyjson_mut_val *value=object(doc);
    yyjson_mut_obj_add_val(doc,value,"caller",focus_identity(doc,NULL,GetCurrentProcessId(),GetCurrentThreadId()));
    yyjson_mut_obj_add_val(doc,value,"target",focus_window_identity(doc,target?target->hwnd:NULL));
    if(target){yyjson_mut_obj_add_strcpy(doc,value,"target_window_id",target->id);yyjson_mut_obj_add_uint(doc,value,"observed_target_pid",target->pid);yyjson_mut_obj_add_uint(doc,value,"observed_target_tid",target->tid);}
    yyjson_mut_obj_add_val(doc,value,"foreground_before",focus_window_identity(doc,attempt->before));
    yyjson_mut_obj_add_val(doc,value,"foreground_after",focus_window_identity(doc,GetForegroundWindow()));
    yyjson_mut_obj_add_str(doc,value,"api","SetForegroundWindow");yyjson_mut_obj_add_bool(doc,value,"api_attempted",attempt->attempted);
    if(attempt->attempted)yyjson_mut_obj_add_int(doc,value,"api_return_value",attempt->returned);else yyjson_mut_obj_add_null(doc,value,"api_return_value");
    yyjson_mut_obj_add_null(doc,value,"api_win32_error");yyjson_mut_obj_add_bool(doc,value,"api_win32_error_authoritative",false);
    yyjson_mut_obj_add_str(doc,value,"reason","SetForegroundWindow documents BOOL success/failure, not GetLastError. No residual last-error value is used as the cause; unavailable identity fields remain null.");
    yyjson_mut_obj_add_val(doc,data,"focus_diagnostic",value);
}
static const char *input_action_inner(sb_desktop *desktop,const char *name,yyjson_val *args,observation *view,yyjson_mut_doc *doc,yyjson_mut_val *data,focus_attempt *focus) {
    if(!strcmp(name,"window_focus"))yyjson_mut_obj_add_strcpy(doc,data,"requested_window_id",sb_string(yyjson_obj_get(args,"window_id")));
    const char *error=observe_guard(desktop,view,true,strcmp(name,"window_focus")!=0,doc,data); if (error) return error;
    if (GetAsyncKeyState(VK_CONTROL)&0x8000 || GetAsyncKeyState(VK_MENU)&0x8000 || GetAsyncKeyState(VK_SHIFT)&0x8000 ||
        GetAsyncKeyState(VK_LWIN)&0x8000 || GetAsyncKeyState(VK_RWIN)&0x8000 || GetAsyncKeyState(VK_LBUTTON)&0x8000 ||
        GetAsyncKeyState(VK_RBUTTON)&0x8000 || GetAsyncKeyState(VK_MBUTTON)&0x8000) return "user_input_already_held";
    unsigned inserted=0;
    if (!strcmp(name,"window_focus")) {
        const char *id=sb_string(yyjson_obj_get(args,"window_id")); window_ref *target=NULL;
        for (unsigned i=0;i<view->window_count;++i) if (!strcmp(view->windows[i].id,id)) target=&view->windows[i];
        if (!target || !window_identity(target)) {
            yyjson_mut_val *changes=yyjson_mut_arr(doc);yyjson_mut_obj_add_strcpy(doc,data,"window_id",id);
            if(!target) { yyjson_mut_val *v=change(doc,changes,"window_id",NULL);yyjson_mut_obj_add_str(doc,v,"reason","not_in_this_observation"); }
            else {
                DWORD pid=0,tid=GetWindowThreadProcessId(target->hwnd,&pid);RECT rect={0};bool got=GetWindowRect(target->hwnd,&rect)!=0;
                change_uint(doc,changes,"window.exists",1,IsWindow(target->hwnd)!=0,target);
                change_uint(doc,changes,"window.pid",target->pid,pid,target);change_uint(doc,changes,"window.thread_id",target->tid,tid,target);
                change_uint(doc,changes,"window.visible",1,IsWindowVisible(target->hwnd)!=0,target);change_uint(doc,changes,"window.minimized",0,IsIconic(target->hwnd)!=0,target);
                if(got)change_rect(doc,changes,"window.bounds",&target->rect,&rect,target);
                else {yyjson_mut_val *v=change(doc,changes,"window.bounds",target);rect_json(doc,v,"expected",&target->rect);yyjson_mut_obj_add_null(doc,v,"actual");}
                FILETIME created={0},exit,kernel,user;HANDLE process=pid?OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid):NULL;
                bool identity=process && GetProcessTimes(process,&created,&exit,&kernel,&user);if(process)CloseHandle(process);
                if(!identity || memcmp(&created,&target->created,sizeof(created))){
                    yyjson_mut_val *v=change(doc,changes,"window.process_created_filetime",target);
                    yyjson_mut_obj_add_uint(doc,v,"expected",((uint64_t)target->created.dwHighDateTime<<32)|target->created.dwLowDateTime);
                    if(identity)yyjson_mut_obj_add_uint(doc,v,"actual",((uint64_t)created.dwHighDateTime<<32)|created.dwLowDateTime);else yyjson_mut_obj_add_null(doc,v,"actual");
                }
            }
            yyjson_mut_obj_add_str(doc,data,"code","stale_window_id");yyjson_mut_obj_add_val(doc,data,"changes",changes);return "stale_window_id";
        }
        if (!input_permitted(desktop,target->hwnd)) return "input_not_permitted: higher integrity or unknown window owner";
        if (target->hwnd!=GetForegroundWindow()) {focus->attempted=true;focus->returned=SetForegroundWindow(target->hwnd);if(!focus->returned)return "focus_refused_by_windows";}
        uint64_t until=sb_platform_millis()+250;
        while (target->hwnd!=GetForegroundWindow() && sb_platform_millis()<until) {
            Sleep(5); pump();
            if (!interactive(desktop) || !desktop->leased || sb_platform_millis()>=desktop->deadline ||
                WaitForSingleObject(desktop->pause,0)==WAIT_OBJECT_0 || view->input_epoch!=InterlockedCompareExchange(desktop->epoch,0,0)) return "focus_interrupted: obtain a fresh observation";
        }
        if (target->hwnd!=GetForegroundWindow()) return "focus_not_observed";
        yyjson_mut_obj_add_bool(doc,data,"foreground_observed",true);
    } else if (!strcmp(name,"desktop_type")) {
        wchar_t *text=wide(sb_string(yyjson_obj_get(args,"text"))); size_t length=text ? wcslen(text) : 0;
        if (!text) return "invalid_unicode_text";
        for (size_t offset=0;offset<length;) {
            pump(); error=observe_guard(desktop,view,true,true,doc,data); if (error) break;
            INPUT events[64]; unsigned count=0;
            while (offset<length && count<=60) {
                WORD key=(WORD)text[offset++]; events[count++]=key_event(desktop,key,0,true); events[count++]=key_event(desktop,key,KEYEVENTF_KEYUP,true);
                if (key>=0xd800 && key<=0xdbff && offset<length) { key=(WORD)text[offset++]; events[count++]=key_event(desktop,key,0,true); events[count++]=key_event(desktop,key,KEYEVENTF_KEYUP,true); }
            }
            UINT sent=SendInput(count,events,sizeof(INPUT)); inserted+=sent;
            if (sent!=count) { for (unsigned i=1;i<count;i+=2) SendInput(1,&events[i],sizeof(INPUT)); error="partial_input_submission"; break; }
        }
        free(text);
    } else if (!strcmp(name,"desktop_key")) {
        key_chord chord;error=chord_parse(args,desktop->policy.emergency_function_key,&chord);if(error)return error;
        for(unsigned i=0;i<chord.count;++i)if(GetAsyncKeyState(chord.codes[i])&0x8000)return "requested_key_already_held";
        error=submit_chord(desktop,&chord,doc,data,&inserted);
    } else {
        int x=0,y=0; integer(args,"x",0,-32768,32767,&x); integer(args,"y",0,-32768,32767,&y);
        error=coordinate_guard(view,x,y); if (error) return error;
        INPUT events[5]; events[0]=desktop_mouse_event(desktop,MOUSEEVENTF_MOVE,0,x,y);
        if (!strcmp(name,"desktop_scroll")) {
            int delta=0; integer(args,"delta",0,-1200,1200,&delta);
            events[1]=desktop_mouse_event(desktop,yyjson_get_bool(yyjson_obj_get(args,"horizontal")) ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL,(DWORD)delta,0,0);
            inserted=SendInput(2,events,sizeof(INPUT)); if (inserted!=2) error="partial_input_submission";
        } else {
            const char *button=sb_string(yyjson_obj_get(args,"button"));
            DWORD down=button && !strcmp(button,"right") ? MOUSEEVENTF_RIGHTDOWN : button && !strcmp(button,"middle") ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_LEFTDOWN;
            DWORD up=down==MOUSEEVENTF_RIGHTDOWN ? MOUSEEVENTF_RIGHTUP : down==MOUSEEVENTF_MIDDLEDOWN ? MOUSEEVENTF_MIDDLEUP : MOUSEEVENTF_LEFTUP;
            events[1]=desktop_mouse_event(desktop,down,0,0,0); events[2]=desktop_mouse_event(desktop,up,0,0,0);
            if (!strcmp(name,"desktop_click")) {
                int clicks=1; integer(args,"clicks",1,1,2,&clicks);
                if (clicks==2) { events[3]=events[1]; events[4]=events[2]; }
                unsigned count=clicks==2 ? 5 : 3; inserted=SendInput(count,events,sizeof(INPUT));
                if (inserted!=count) { SendInput(1,events+2,sizeof(INPUT)); error="partial_input_submission"; }
            } else {
                int end_x=0,end_y=0,duration=200;
                integer(args,"to_x",0,-32768,32767,&end_x); integer(args,"to_y",0,-32768,32767,&end_y); integer(args,"duration_ms",200,50,1000,&duration);
                error=coordinate_guard(view,end_x,end_y); if (error) return error;
                inserted=SendInput(2,events,sizeof(INPUT)); if (inserted!=2) error="partial_input_submission";
                for (int step=1;!error && step<=20;++step) {
                    Sleep((DWORD)duration/20); pump(); error=observe_guard(desktop,view,true,true,doc,data); if (error) break;
                    if (sb_platform_millis()>=desktop->deadline) { error="desktop_lease_expired"; break; }
                    INPUT move=desktop_mouse_event(desktop,MOUSEEVENTF_MOVE,0,x+(end_x-x)*step/20,y+(end_y-y)*step/20);
                    UINT sent=SendInput(1,&move,sizeof(INPUT)); inserted+=sent; if (!sent) error="partial_input_submission";
                }
                inserted+=SendInput(1,events+2,sizeof(INPUT));
            }
        }
    }
    yyjson_mut_obj_add_uint(doc,data,"events_inserted",inserted); yyjson_mut_obj_add_bool(doc,data,"application_success_confirmed",false);
    yyjson_mut_obj_add_str(doc,data,"next_action","Obtain a fresh observation and verify the application result; do not assume business completion.");
    invalidate(desktop); return error;
}
static const char *input_action(sb_desktop *desktop,const char *name,yyjson_val *args,observation *view,yyjson_mut_doc *doc,yyjson_mut_val *data) {
    bool focusing=!strcmp(name,"window_focus");focus_attempt attempt={0};window_ref *target=NULL;
    if(focusing){attempt.before=GetForegroundWindow();const char *id=sb_string(yyjson_obj_get(args,"window_id"));
        for(unsigned i=0;view && id && i<view->window_count;++i)if(!strcmp(view->windows[i].id,id))target=&view->windows[i];}
    const char *error=input_action_inner(desktop,name,args,view,doc,data,&attempt);
    if(focusing)focus_report(doc,data,target,&attempt);return error;
}
static const char *clipboard_action(sb_desktop *desktop,const char *name,yyjson_val *args,yyjson_mut_doc *doc,yyjson_mut_val *data) {
    uint64_t expected=0; sb_integer(yyjson_obj_get(args,"expected_sequence"),&expected);
    if (GetClipboardSequenceNumber()!=(DWORD)expected) return "clipboard_sequence_changed";
    bool writing=!strcmp(name,"clipboard_write");
    if (writing && WaitForSingleObject(desktop->pause,0)==WAIT_OBJECT_0) return "desktop_paused: local owner must resume";
    if (!OpenClipboard(desktop->window)) return "clipboard_busy";
    const char *error=NULL; HGLOBAL output=NULL;
    if (GetClipboardSequenceNumber()!=(DWORD)expected) { error="clipboard_sequence_changed"; goto done; }
    if (writing) {
        wchar_t *text=wide(sb_string(yyjson_obj_get(args,"text"))); size_t size=text ? (wcslen(text)+1)*sizeof(wchar_t) : 0;
        output=size ? GlobalAlloc(GMEM_MOVEABLE,size) : NULL; void *buffer=output ? GlobalLock(output) : NULL;
        if (!buffer) { free(text); error="clipboard_allocation_failed"; goto done; }
        memcpy(buffer,text,size); GlobalUnlock(output); free(text);
        if (!EmptyClipboard() || !SetClipboardData(CF_UNICODETEXT,output)) { error="clipboard_write_failed: prior contents may have been cleared"; goto done; }
        output=NULL; yyjson_mut_obj_add_bool(doc,data,"written",true);
    } else {
        if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) { error="clipboard_text_unavailable"; goto done; }
        HANDLE handle=GetClipboardData(CF_UNICODETEXT); SIZE_T size=handle ? GlobalSize(handle) : 0;
        if (size<sizeof(wchar_t) || size>CLIPBOARD_BYTES || size%sizeof(wchar_t)) { error="clipboard_text_budget_exceeded"; goto done; }
        const wchar_t *text=GlobalLock(handle); size_t characters=size/sizeof(wchar_t),length=0;
        if (!text) { error="clipboard_read_failed"; goto done; }
        while (length<characters && text[length]) ++length;
        char *encoded=length<characters ? utf8(text) : NULL; GlobalUnlock(handle);
        if (!encoded) { error="invalid_clipboard_text"; goto done; }
        yyjson_mut_obj_add_strcpy(doc,data,"text",encoded); free(encoded);
    }
done:
    if (output) GlobalFree(output); CloseClipboard(); yyjson_mut_obj_add_uint(doc,data,"clipboard_sequence",GetClipboardSequenceNumber()); return error;
}
static char *execute(sb_desktop *desktop,desktop_job *job,yyjson_val *args) {
    yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL); if (!doc) return NULL; yyjson_mut_val *data=object(doc);
    const char *error=NULL,*action=sb_string(yyjson_obj_get(args,"action")),*lease=sb_string(yyjson_obj_get(args,"lease_id"));
    capture_diagnostic diagnostic={0};
    yyjson_mut_obj_add_str(doc,data,"project_id",desktop->host->project->id);
    yyjson_mut_obj_add_str(doc,data,"host_instance_id",sb_execution_instance(desktop->host->execution));
    if (!strcmp(job->name,"desktop_lease")) {
        if (!strcmp(action,"status")) {
            yyjson_mut_obj_add_bool(doc,data,"leased",desktop->leased);
            bool owner=desktop->leased && !strcmp(job->actor,desktop->actor);
            yyjson_mut_obj_add_bool(doc,data,"owned_by_caller",owner);
            if (owner) { yyjson_mut_obj_add_str(doc,data,"lease_id",desktop->lease_id); yyjson_mut_obj_add_uint(doc,data,"expires_in_ms",desktop->deadline>sb_platform_millis()?desktop->deadline-sb_platform_millis():0); }
        } else if (!strcmp(action,"release")) {
            if (!desktop->leased) yyjson_mut_obj_add_bool(doc,data,"released",true);
            else if (!owns(desktop,job->actor,lease)) error="desktop_lease_not_owned";
            else { release_lease(desktop); yyjson_mut_obj_add_bool(doc,data,"released",true); }
        } else {
            int duration=0; integer(args,"duration_seconds",(int)desktop->policy.lease_seconds,1,(int)desktop->policy.lease_seconds,&duration);
            if (!interactive(desktop)) error="no_desktop_session";
            else if (WaitForSingleObject(desktop->pause,0)==WAIT_OBJECT_0) error="desktop_paused: only the local owner can resume";
            else if (!strcmp(action,"renew")) {
                if (!owns(desktop,job->actor,lease)) error="desktop_lease_not_owned";
                else desktop->deadline=sb_platform_millis()+(uint64_t)duration*1000;
            } else if (desktop->leased) error="desktop_busy: release the current lease before a new acquisition";
            else {
                DWORD wait=WaitForSingleObject(desktop->mutex,0);
                if (wait!=WAIT_OBJECT_0 && wait!=WAIT_ABANDONED) error="desktop_busy: another project or manager owns this session";
                else {
                    desktop->leased=true;
                    desktop->hotkey=RegisterHotKey(desktop->window,1,MOD_CONTROL|MOD_ALT|MOD_NOREPEAT,VK_F1+desktop->policy.emergency_function_key-1)!=0;
                    desktop->keyboard=SetWindowsHookExW(WH_KEYBOARD_LL,keyboard_hook,GetModuleHandleW(NULL),0);
                    desktop->mouse=SetWindowsHookExW(WH_MOUSE_LL,mouse_hook,GetModuleHandleW(NULL),0);
                    if (!desktop->hotkey || !desktop->keyboard || !desktop->mouse || !nonce(desktop->lease_id)) {
                        error=!desktop->hotkey ? "emergency_hotkey_unavailable: configure a different Ctrl+Alt+F1..F11 combination" :
                            !desktop->keyboard ? "desktop_keyboard_safety_hook_unavailable" : !desktop->mouse ? "desktop_mouse_safety_hook_unavailable" : "desktop_random_source_unavailable";
                        release_lease(desktop);
                    }
                    else { strcpy(desktop->actor,job->actor); desktop->deadline=sb_platform_millis()+(uint64_t)duration*1000; }
                }
            }
            if (!error) {
                yyjson_mut_obj_add_str(doc,data,"lease_id",desktop->lease_id); yyjson_mut_obj_add_uint(doc,data,"duration_seconds",duration);
                yyjson_mut_obj_add_uint(doc,data,"expires_at_unix_ms",sb_platform_unix_ms()+desktop->deadline-sb_platform_millis());
                yyjson_mut_obj_add_str(doc,data,"binding","project + authenticated caller + worker instance + Windows session");
            }
        }
        yyjson_mut_obj_add_bool(doc,data,"paused",WaitForSingleObject(desktop->pause,0)==WAIT_OBJECT_0);
    } else if (!owns(desktop,job->actor,lease)) error="desktop_lease_not_owned_or_expired";
    else if (!interactive(desktop)) error="no_desktop_session: locked, disconnected or secure desktop";
    else if (!strcmp(job->name,"desktop_state") || !strcmp(job->name,"window_list") || !strcmp(job->name,"desktop_capture")) {
        observation *view=NULL;
        if (!observe(desktop,doc,data,&view)) error="desktop_observation_unavailable";
        else if (!strcmp(job->name,"desktop_capture")) {
            topology monitors; char hash[65]; int index=0,maximum=1600,x=0,y=0,width=0,height=0;
            integer(args,"monitor_id",0,0,15,&index); integer(args,"max_dimension",1600,200,SB_IMAGE_DIMENSION,&maximum);
            if (!layout(&monitors,hash) || (unsigned)index>=monitors.count) error="unknown_monitor_id";
            else {
                RECT crop=monitors.items[index].rect;
                if (yyjson_obj_get(args,"x")) {
                    integer(args,"x",0,-32768,32767,&x); integer(args,"y",0,-32768,32767,&y);
                    integer(args,"width",0,1,32768,&width); integer(args,"height",0,1,32768,&height);
                    RECT requested={x,y,x+width,y+height},intersection;
                    if (!IntersectRect(&intersection,&crop,&requested) || !EqualRect(&intersection,&requested)) error="capture_crop_outside_monitor";
                    crop=requested;
                }
                if (!error) {
                    unsigned image_width=0,image_height=0;
                    char *png=capture_png(&crop,(unsigned)maximum,&image_width,&image_height,&diagnostic);
                    if (!png) error=diagnostic.code;
                    else {
                        error=observe_guard(desktop,view,false,false,doc,data);
                        if (!error) {
                            view->captured=crop; view->capture=true;
                            yyjson_mut_obj_add_uint(doc,data,"width",image_width); yyjson_mut_obj_add_uint(doc,data,"height",image_height);
                            yyjson_mut_obj_add_uint(doc,data,"bytes",diagnostic.bytes); yyjson_mut_obj_add_str(doc,data,"mime_type","image/png");
                            yyjson_mut_obj_add_bool(doc,data,"cursor_included",false); rect_json(doc,data,"physical_region",&crop);
                            yyjson_mut_obj_add_real(doc,data,"physical_pixels_per_image_x",(crop.right-crop.left)/(double)image_width);
                            yyjson_mut_obj_add_real(doc,data,"physical_pixels_per_image_y",(crop.bottom-crop.top)/(double)image_height);
                            capture_report(desktop,doc,data,&diagnostic);char *wire=result(doc,data,NULL,png); free(png); yyjson_mut_doc_free(doc); return wire;
                        }
                        free(png);
                    }
                }
            }
        }
    } else if (!strcmp(job->name,"clipboard_read") || !strcmp(job->name,"clipboard_write")) error=clipboard_action(desktop,job->name,args,doc,data);
    else {
        observation *view=find_observation(desktop,sb_string(yyjson_obj_get(args,"screen_id")));
        error=input_action(desktop,job->name,args,view,doc,data);
    }
    if(!strcmp(job->name,"desktop_capture"))capture_report(desktop,doc,data,&diagnostic);
    char *wire=result(doc,data,error,NULL); yyjson_mut_doc_free(doc); return wire;
}
static void job_release(desktop_job *job) {
    if (InterlockedDecrement(&job->references)==0) { CloseHandle(job->done); free(job->arguments); free(job->reply); free(job); }
}
static char *capabilities(sb_desktop *desktop) {
    yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL); if (!doc) return NULL; yyjson_mut_val *data=object(doc);
    bool available=interactive(desktop); InterlockedExchange(&desktop->available,available?1:0);
    yyjson_mut_obj_add_bool(doc,data,"available",available); yyjson_mut_obj_add_bool(doc,data,"control_enabled",desktop->policy.control);
    yyjson_mut_obj_add_bool(doc,data,"clipboard_read_enabled",desktop->policy.clipboard_read);
    yyjson_mut_obj_add_bool(doc,data,"clipboard_write_enabled",desktop->policy.clipboard_write);
    yyjson_mut_obj_add_uint(doc,data,"session_id",desktop->session);
    yyjson_mut_obj_add_bool(doc,data,"paused",WaitForSingleObject(desktop->pause,0)==WAIT_OBJECT_0);
    yyjson_mut_obj_add_uint(doc,data,"max_lease_seconds",desktop->policy.lease_seconds);
    yyjson_mut_obj_add_uint(doc,data,"observation_ttl_ms",OBSERVATION_MS); yyjson_mut_obj_add_uint(doc,data,"max_observations",OBSERVATIONS);
    yyjson_mut_obj_add_uint(doc,data,"max_png_bytes",PNG_LIMIT); yyjson_mut_obj_add_uint(doc,data,"max_image_dimension",SB_IMAGE_DIMENSION);
    yyjson_mut_obj_add_uint(doc,data,"max_type_utf16_units",512); yyjson_mut_obj_add_uint(doc,data,"max_clipboard_bytes",CLIPBOARD_BYTES);
    yyjson_mut_obj_add_bool(doc,data,"explicit_key_chords",true);yyjson_mut_obj_add_uint(doc,data,"max_key_modifiers",3);
    yyjson_mut_obj_add_bool(doc,data,"key_hold_supported",false);
    char key[24]; snprintf(key,sizeof(key),"Ctrl+Alt+F%u",desktop->policy.emergency_function_key); yyjson_mut_obj_add_strcpy(doc,data,"emergency_hotkey",key);
    yyjson_mut_obj_add_uint(doc,data,"emergency_stop_count",desktop->emergency_stops);
    yyjson_mut_obj_add_uint(doc,data,"physical_input_count",desktop->physical_inputs);
    yyjson_mut_obj_add_uint(doc,data,"foreign_input_count",desktop->foreign_inputs);
    yyjson_mut_obj_add_uint(doc,data,"truncated_input_tag_count",desktop->truncated_inputs);
    yyjson_mut_obj_add_bool(doc,data,"local_owner_resume_only",true); yyjson_mut_obj_add_bool(doc,data,"elevated_input",false);
    yyjson_mut_obj_add_bool(doc,data,"automatic_replay",false); yyjson_mut_obj_add_bool(doc,data,"screenshots_persisted",false);
    char *wire=yyjson_mut_val_write(data,0,NULL); yyjson_mut_doc_free(doc); return wire;
}
static void process_job(sb_desktop *desktop,desktop_job *job) {
    if (job->cancelled || sb_platform_millis()>=job->deadline) job->reply=failed("desktop_request_deadline_exceeded: no queued action replay");
    else if (!strcmp(job->name,"__capabilities")) job->reply=capabilities(desktop);
    else {
        yyjson_doc *input=yyjson_read(job->arguments,strlen(job->arguments),0); yyjson_val *args=yyjson_doc_get_root(input);
        key_chord chord;const char *key_error=!strcmp(job->name,"desktop_key") && yyjson_is_obj(args)?chord_parse(args,desktop->policy.emergency_function_key,&chord):NULL;
        if(key_error)job->reply=failed(key_error);
        else if (!arguments(desktop,job->name,args)) job->reply=failed("invalid_desktop_arguments");
        else if (job->prepare) {
            /* Binding uses the immutable worker/session and caller lease nonce.
               Live observation/clipboard guards run again immediately before execution.
               Preparation never reads pixels, titles or clipboard text. */
            const char *lease=sb_string(yyjson_obj_get(args,"lease_id")); char target[256],version[65]; sb_error error={0};
            snprintf(target,sizeof(target),"desktop-session:%lu/project:%s/worker:%s/lease:%s",desktop->session,
                desktop->host->project->id,sb_execution_instance(desktop->host->execution),lease?lease:"acquire");
            char *parameters=canonical(args);
            if (!parameters || !sb_platform_sha256(target,strlen(target),version,&error)) job->reply=failed("desktop_binding_unavailable");
            else {
                yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL); yyjson_mut_val *body=object(doc);
                yyjson_mut_obj_add_strcpy(doc,body,"target",target); yyjson_mut_obj_add_strcpy(doc,body,"version",version);
                yyjson_mut_obj_add_strcpy(doc,body,"parameters",parameters); job->reply=yyjson_mut_val_write(body,0,NULL); yyjson_mut_doc_free(doc);
            }
            free(parameters);
        } else job->reply=execute(desktop,job,args);
        yyjson_doc_free(input);
    }
    SetEvent(job->done); job_release(job);
}
static LRESULT CALLBACK desktop_window(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    sb_desktop *desktop=(sb_desktop *)GetWindowLongPtrW(window,GWLP_USERDATA);
    if (message==WM_NCCREATE) { desktop=((CREATESTRUCTW *)lparam)->lpCreateParams; desktop->window=window; SetWindowLongPtrW(window,GWLP_USERDATA,(LONG_PTR)desktop); }
    if (!desktop) return DefWindowProcW(window,message,wparam,lparam);
    if (message==DESKTOP_JOB) { process_job(desktop,(desktop_job *)lparam); return 0; }
    if (message==WM_HOTKEY && wparam==1) { ++desktop->emergency_stops; pause_local(desktop); return 0; }
    if (message==WM_TIMER) {
        bool available=interactive(desktop); InterlockedExchange(&desktop->available,available?1:0);
        if (desktop->leased && (!available || sb_platform_millis()>=desktop->deadline)) {
            sb_audit_operation(desktop->host->audit,available?"desktop_lease_expired":"desktop_session_unavailable",desktop->actor,
                desktop->host->project->id,"ordinary","desktop_lease",NULL); release_lease(desktop);
        }
        bool paused=WaitForSingleObject(desktop->pause,0)==WAIT_OBJECT_0;
        if (paused!=desktop->was_paused) {
            /* epoch 已在暂停/恢复瞬间更新；定时器不能再使恢复后刚取得的观察过期。 */ desktop->was_paused=paused;
            sb_audit_operation(desktop->host->audit,paused?"desktop_paused":"desktop_resumed","local-owner-or-input",desktop->host->project->id,"ordinary","desktop_lease",NULL);
        }
        return 0;
    }
    if (message==WM_CLOSE) { release_lease(desktop); DestroyWindow(window); return 0; }
    if (message==WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window,message,wparam,lparam);
}
static DWORD WINAPI desktop_thread(void *parameter) {
    sb_desktop *desktop=parameter; hook_context=desktop;
    HRESULT initialized=CoInitializeEx(NULL,COINIT_APARTMENTTHREADED);
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSW cls={0}; cls.lpfnWndProc=desktop_window; cls.hInstance=GetModuleHandleW(NULL); cls.hIcon=sb_app_icon(); cls.lpszClassName=L"ShellBotDesktopController.v1";
    if (!RegisterClassW(&cls) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) { SetEvent(desktop->ready); goto done; }
    desktop->window=CreateWindowExW(WS_EX_TOOLWINDOW,cls.lpszClassName,L"ShellBot 桌面控制",WS_OVERLAPPED,0,0,0,0,NULL,NULL,cls.hInstance,desktop);
    if (!desktop->window) { SetEvent(desktop->ready); goto done; }
    SetTimer(desktop->window,1,100,NULL); InterlockedExchange(&desktop->available,interactive(desktop)?1:0);
    desktop->was_paused=WaitForSingleObject(desktop->pause,0)==WAIT_OBJECT_0;
    SetEvent(desktop->ready);
    MSG message; while (GetMessageW(&message,NULL,0,0)>0) { TranslateMessage(&message); DispatchMessageW(&message); }
done:
    InterlockedExchange(&desktop->available,0); hook_context=NULL;
    if (SUCCEEDED(initialized)) CoUninitialize(); return 0;
}
sb_desktop *sb_win_desktop_new(sb_host *host,sb_error *error) {
    if (!host->project || host->project->legacy_compatibility || !host->project->policy.desktop.enabled || host->administrator_host) return NULL;
    sb_desktop *desktop=calloc(1,sizeof(*desktop)); if (!desktop) return NULL;
    desktop->host=host; desktop->policy=host->project->policy.desktop;
    if (!shared_objects(&desktop->mutex,&desktop->pause,&desktop->epoch_map,&desktop->epoch,&desktop->session,desktop->sid,&desktop->integrity,false)) goto failed;
    /* Mouse hooks can retain only the low 32 bits of dwExtraInfo on Windows.
       Use a random positive tag represented identically by both input paths. */
    uint32_t tag=0; if (!sb_platform_random((unsigned char *)&tag,sizeof(tag),error)) goto failed;
    desktop->input_tag=(ULONG_PTR)((tag&UINT32_C(0x7fffffff))|1u);
    desktop->ready=CreateEventW(NULL,TRUE,FALSE,NULL); desktop->thread=desktop->ready ? CreateThread(NULL,0,desktop_thread,desktop,0,NULL) : NULL;
    if (!desktop->thread || WaitForSingleObject(desktop->ready,5000)!=WAIT_OBJECT_0 || !desktop->window) goto failed;
    return desktop;
failed:
    sb_fail(error,"Desktop backend unavailable; file and command capabilities remain independent."); sb_win_desktop_free(desktop); return NULL;
}
void sb_win_desktop_free(sb_desktop *desktop) {
    if (!desktop) return;
    if (desktop->window) PostMessageW(desktop->window,WM_CLOSE,0,0);
    if (desktop->thread) { WaitForSingleObject(desktop->thread,INFINITE); CloseHandle(desktop->thread); }
    if (desktop->ready) CloseHandle(desktop->ready); if (desktop->mutex) CloseHandle(desktop->mutex); if (desktop->pause) CloseHandle(desktop->pause);
    if (desktop->epoch) UnmapViewOfFile((const void *)desktop->epoch); if (desktop->epoch_map) CloseHandle(desktop->epoch_map); free(desktop);
}
char *sb_desktop_request(sb_desktop *desktop,const char *actor,const char *name,yyjson_val *arguments_value,bool prepare) {
    if (!desktop || !desktop->window || !actor || strlen(actor)>=64 || !name || strlen(name)>=32) return failed("desktop_backend_unavailable");
    desktop_job *job=calloc(1,sizeof(*job)); if (!job) return NULL;
    job->references=2; job->prepare=prepare; job->deadline=sb_platform_millis()+5000; strcpy(job->actor,actor); strcpy(job->name,name);
    job->arguments=arguments_value ? yyjson_val_write(arguments_value,0,NULL) : sb_strdup("{}"); job->done=CreateEventW(NULL,TRUE,FALSE,NULL);
    if (!job->arguments || !job->done || !PostMessageW(desktop->window,DESKTOP_JOB,0,(LPARAM)job)) { job_release(job); job_release(job); return failed("desktop_request_not_queued"); }
    char *reply=NULL;
    if (WaitForSingleObject(job->done,5000)==WAIT_OBJECT_0) reply=job->reply ? sb_strdup(job->reply) : failed("desktop_receipt_unavailable");
    else { InterlockedExchange(&job->cancelled,1); reply=failed("desktop_request_timed_out: review possible effects; no automatic retry"); }
    job_release(job); return reply;
}
bool sb_desktop_available(sb_desktop *desktop) { return desktop && desktop->available!=0; }
yyjson_mut_val *sb_desktop_capabilities(sb_desktop *desktop,yyjson_mut_doc *doc) {
    char *wire=desktop ? sb_desktop_request(desktop,"capabilities","__capabilities",NULL,false) : NULL;
    yyjson_doc *input=wire ? yyjson_read(wire,strlen(wire),0) : NULL;
    yyjson_mut_val *data=input ? yyjson_val_mut_copy(doc,yyjson_doc_get_root(input)) : object(doc);
    if (!input) { yyjson_mut_obj_add_bool(doc,data,"available",false); yyjson_mut_obj_add_str(doc,data,"reason","not_enabled_or_backend_unavailable"); }
    yyjson_doc_free(input); free(wire); return data;
}
char *sb_win_desktop_owner(const char *command,sb_audit *audit) {
    if (!command || (strcmp(command,"desktop-status") && strcmp(command,"desktop-pause") && strcmp(command,"desktop-resume"))) return failed("unknown_desktop_owner_command");
    HANDLE mutex=NULL,pause=NULL,mapping=NULL; volatile LONG *epoch=NULL; DWORD session=0,integrity=0; wchar_t sid[192]={0};
    if (!shared_objects(&mutex,&pause,&mapping,&epoch,&session,sid,&integrity,true)) return failed("desktop_owner_requires_normal_interactive_user");
    bool changing=strcmp(command,"desktop-status")!=0; const char *error=NULL;
    if (changing && !sb_audit_operation(audit,"desktop_owner_control","local-owner","manager","ordinary",command,NULL)) error="Audit unavailable; desktop control change was not performed.";
    if (!error && changing) { InterlockedIncrement(epoch); if (!strcmp(command,"desktop-pause")) SetEvent(pause); else if (!strcmp(command,"desktop-resume")) ResetEvent(pause); }
    yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL); yyjson_mut_val *data=object(doc); DWORD wait=WaitForSingleObject(mutex,0);
    bool free_mutex=wait==WAIT_OBJECT_0 || wait==WAIT_ABANDONED; if (free_mutex) ReleaseMutex(mutex);
    yyjson_mut_obj_add_uint(doc,data,"session_id",session); yyjson_mut_obj_add_bool(doc,data,"paused",WaitForSingleObject(pause,0)==WAIT_OBJECT_0);
    yyjson_mut_obj_add_bool(doc,data,"leased",!free_mutex); yyjson_mut_obj_add_bool(doc,data,"automatic_replay",false);
    char *wire=result(doc,data,error,NULL); yyjson_mut_doc_free(doc); UnmapViewOfFile((const void *)epoch); CloseHandle(mapping); CloseHandle(mutex); CloseHandle(pause); return wire;
}
