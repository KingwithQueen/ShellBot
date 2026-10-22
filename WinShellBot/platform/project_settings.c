#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include "sb_projects.h"
#include "project_settings.h"
#include "execution.h"
#include "desktop_app.h"
#include "native_ui.h"
#include "folder_dialog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

enum { C_OPEN=4001,C_NEW,C_ADD,C_REMOVE,C_JSON,C_SAVE,C_DIR_ADD,C_DIR_EDIT,C_DIR_REMOVE,
    C_PROJECTS=4200,C_OUTPUT,C_STATUS,C_TABS,C_SOURCE,C_DIRECTORIES,C_TOOLS,C_PROJECT_HINT,C_DIRECTORY_HINT,C_OUTPUT_LABEL };
enum { F_ID,F_TOKEN,F_DATA,F_CWD,F_ENABLED,F_APPROVAL,F_SCOPE,F_READONLY,F_EXECUTION,F_IDENTITY,F_ADMIN,
    F_DESKTOP,F_CONTROL,F_CLIPREAD,F_CLIPWRITE,F_LEASE,F_HOTKEY,F_RUNTIME,F_PORT,F_PUBLIC,F_OWNER,F_DEFAULT,F_COUNT };
typedef struct {
    HWND window,owner,projects,tabs,directories,tools,source,output,status,fields[F_COUNT],labels[F_COUNT];
    HWND buttons[9]; HFONT font; yyjson_mut_doc *doc; char *original; int selected,page;
    bool loading,dirty,closed,embedded;
    char *result;UINT dpi;
} settings;
typedef struct { HWND window,owner,edit,id,workspace,readonly; HFONT font; bool done,accepted,json; char *text,*name; bool is_workspace,read_only;UINT dpi; } modal;
static wchar_t *wtext(const char *s) {
    int n=s ? MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s,-1,NULL,0) : 0;
    wchar_t *w=n ? calloc((size_t)n,sizeof(*w)) : NULL;
    if (w) MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s,-1,w,n); return w;
}
static char *text(HWND control) {
    int n=GetWindowTextLengthW(control); wchar_t *w=calloc((size_t)n+1,sizeof(*w));
    if (!w) return NULL; GetWindowTextW(control,w,n+1);
    int size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,w,-1,NULL,0,NULL,NULL);
    char *s=size ? malloc((size_t)size) : NULL;
    if (s) WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,w,-1,s,size,NULL,NULL); free(w); return s;
}
static void put(HWND control,const char *s) { wchar_t *w=wtext(s ? s : ""); SetWindowTextW(control,w ? w : L""); free(w); }
static const char *str(yyjson_mut_val *o,const char *key) { const char *s=yyjson_mut_get_str(yyjson_mut_obj_get(o,key)); return s ? s : ""; }
static yyjson_mut_val *object(settings *s,yyjson_mut_val *parent,const char *key) {
    yyjson_mut_val *o=yyjson_mut_obj_get(parent,key);
    if (!o) { o=yyjson_mut_obj(s->doc); yyjson_mut_obj_add_val(s->doc,parent,key,o); } return o;
}
static void string(settings *s,yyjson_mut_val *o,const char *key,const char *value,bool optional) {
    yyjson_mut_obj_remove_key(o,key); if (!optional || (value && *value)) yyjson_mut_obj_add_strcpy(s->doc,o,key,value ? value : "");
}
static void set_bool(settings *s,yyjson_mut_val *o,const char *key,bool value) { yyjson_mut_obj_remove_key(o,key); yyjson_mut_obj_add_bool(s->doc,o,key,value); }
static bool checked(HWND h) { return SendMessageW(h,BM_GETCHECK,0,0)==BST_CHECKED; }
static void check(HWND h,bool value) { SendMessageW(h,BM_SETCHECK,value ? BST_CHECKED : BST_UNCHECKED,0); }
static int px(HWND h,int x) { UINT dpi=GetDpiForWindow(h); return MulDiv(x,(int)(dpi ? dpi : 96),96); }
static HWND child(HWND parent,HFONT font,const wchar_t *klass,const wchar_t *label,DWORD style,int id,int x,int y,int w,int h) {
    HWND c=CreateWindowExW(!wcscmp(klass,L"EDIT") ? WS_EX_CLIENTEDGE : 0,klass,label,WS_CHILD|WS_VISIBLE|style,
        px(parent,x),px(parent,y),px(parent,w),px(parent,h),parent,(HMENU)(INT_PTR)id,GetModuleHandleW(NULL),NULL);
    SendMessageW(c,WM_SETFONT,(WPARAM)font,TRUE); return c;
}
static void error_box(HWND owner,const char *message) { wchar_t *w=wtext(message); MessageBoxW(owner,w ? w : L"操作失败。",L"ShellBot 配置",MB_OK|MB_ICONERROR); free(w); }
static bool file_dialog(HWND owner,bool save,wchar_t path[SB_PATH_CAP]) {
    OPENFILENAMEW ofn={0}; ofn.lStructSize=sizeof(ofn); ofn.hwndOwner=owner; ofn.lpstrFile=path; ofn.nMaxFile=SB_PATH_CAP;
    ofn.lpstrFilter=L"JSON 配置文件\0*.json\0所有文件\0*.*\0"; ofn.lpstrDefExt=L"json";
    ofn.Flags=OFN_EXPLORER|OFN_NOCHANGEDIR|OFN_PATHMUSTEXIST|(save ? 0 : OFN_FILEMUSTEXIST);
    return (save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn))!=FALSE;
}
static LRESULT CALLBACK modal_proc(HWND window,UINT message,WPARAM wp,LPARAM lp) {
    modal *m=(modal *)GetWindowLongPtrW(window,GWLP_USERDATA);
    if (message==WM_NCCREATE) { m=((CREATESTRUCTW *)lp)->lpCreateParams; m->window=window; SetWindowLongPtrW(window,GWLP_USERDATA,(LONG_PTR)m); }
    if (!m) return DefWindowProcW(window,message,wp,lp);
    if(message==DM_GETDEFID)return MAKELRESULT(IDOK,DC_HASDEFID);
    if (message==WM_CREATE) {
        m->font=sb_native_font(window);m->dpi=sb_native_dpi(window);
        if (m->json) {
            m->edit=child(window,m->font,L"EDIT",L"",WS_TABSTOP|ES_MULTILINE|ES_AUTOVSCROLL|ES_AUTOHSCROLL|WS_VSCROLL|WS_HSCROLL,4300,16,16,770,510);
            SendMessageW(m->edit,EM_SETLIMITTEXT,SB_TEXT_LIMIT,0); put(m->edit,m->text);
        } else {
            child(window,m->font,L"STATIC",L"目录标识",0,0,16,20,100,24);
            m->id=child(window,m->font,L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,4301,130,16,470,28); put(m->id,m->name);
            child(window,m->font,L"STATIC",L"绝对路径",0,0,16,64,100,24);
            m->edit=child(window,m->font,L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,4300,130,60,470,28); put(m->edit,m->text);
            child(window,m->font,L"BUTTON",L"选择目录…",WS_TABSTOP,4304,476,104,124,30);
            m->workspace=child(window,m->font,L"BUTTON",L"作为工作目录",WS_TABSTOP|BS_AUTOCHECKBOX,4302,130,112,180,26); check(m->workspace,m->is_workspace);
            m->readonly=child(window,m->font,L"BUTTON",L"只读",WS_TABSTOP|BS_AUTOCHECKBOX,4303,130,152,180,26); check(m->readonly,m->read_only);
        }
        int y=m->json ? 540 : 204;
        child(window,m->font,L"BUTTON",L"确定",WS_TABSTOP|BS_DEFPUSHBUTTON,IDOK,m->json ? 570 : 370,y,100,30);
        child(window,m->font,L"BUTTON",L"取消",WS_TABSTOP,IDCANCEL,m->json ? 684 : 484,y,100,30); return 0;
    }
    if (message==WM_COMMAND) {
        if (LOWORD(wp)==4304) {
            sb_choose_directory(window,m->edit,L"选择允许访问的目录");return 0;
        }
        if (LOWORD(wp)==IDOK) {
            char *value=text(m->edit),*name=m->json ? NULL : text(m->id);
            if (!value || (!m->json && (!sb_identifier(name) || !sb_project_path_valid(value)))) {
                bool bad_id=!m->json && !sb_identifier(name); free(value); free(name);
                error_box(window,bad_id ? "目录标识只能使用 1～63 个英文字母、数字、下划线或连字符，例如 workspace。" : "目录路径必须是有效的 Windows 绝对路径，例如 D:\\ShellBot\\Workspace；路径可以包含中文。");
                SetFocus(bad_id ? m->id : m->edit); return 0;
            }
            free(m->text); free(m->name); m->text=value; m->name=name;
            m->is_workspace=checked(m->workspace); m->read_only=checked(m->readonly); m->accepted=true; DestroyWindow(window); return 0;
        }
        if (LOWORD(wp)==IDCANCEL) { DestroyWindow(window); return 0; }
    }
    if(message==WM_DPICHANGED){sb_native_dpi_changed(window,&m->font,&m->dpi,wp,lp);return 0;}
    if (message==WM_DESTROY) { m->done=true; return 0; }
    return DefWindowProcW(window,message,wp,lp);
}
static bool show_modal(modal *m) {
    WNDCLASSW wc={0}; wc.lpfnWndProc=modal_proc; wc.hInstance=GetModuleHandleW(NULL); wc.lpszClassName=L"ShellBotSettingsDialog";
    wc.hIcon=sb_app_icon();wc.hCursor=LoadCursorW(NULL,IDC_ARROW); wc.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1); RegisterClassW(&wc);
    RECT r={0,0,px(m->owner,m->json ? 804 : 624),px(m->owner,m->json ? 588 : 254)};
    AdjustWindowRectExForDpi(&r,WS_CAPTION|WS_SYSMENU,FALSE,WS_EX_DLGMODALFRAME,sb_native_dpi(m->owner));
    RECT owner={0};GetWindowRect(m->owner,&owner);
    HWND h=CreateWindowExW(WS_EX_DLGMODALFRAME,wc.lpszClassName,m->json ? L"项目高级配置" : L"目录权限",WS_CAPTION|WS_SYSMENU|WS_CLIPCHILDREN,
        owner.left,owner.top,r.right-r.left,r.bottom-r.top,m->owner,NULL,wc.hInstance,m);
    if (!h) return false; EnableWindow(m->owner,FALSE); ShowWindow(h,SW_SHOW); SetFocus(m->edit);
    sb_native_modal_loop(h,&m->done);
    EnableWindow(m->owner,TRUE); SetActiveWindow(m->owner);DeleteObject(m->font);m->font=NULL;return m->accepted;
}
static yyjson_mut_val *project(settings *s) { return s->selected<0 ? NULL : yyjson_mut_arr_get(yyjson_mut_obj_get(yyjson_mut_doc_get_root(s->doc),"projects"),(size_t)s->selected); }
static void combo(HWND h,const wchar_t *const *labels,size_t count) { for (size_t i=0;i<count;++i) SendMessageW(h,CB_ADDSTRING,0,(LPARAM)labels[i]); }
static void field(settings *s,int index,const wchar_t *label,int x,int y,bool checkbox,bool dropdown,int width) {
    if (!checkbox) s->labels[index]=child(s->window,s->font,L"STATIC",label,0,0,x,y+4,174,24);
    s->fields[index]=child(s->window,s->font,checkbox ? L"BUTTON" : dropdown ? L"COMBOBOX" : L"EDIT",checkbox ? label : L"",
        WS_TABSTOP|(checkbox ? BS_AUTOCHECKBOX : dropdown ? CBS_DROPDOWNLIST|WS_VSCROLL : ES_AUTOHSCROLL),4100+index,
        checkbox ? x : x+180,y,checkbox ? width+180 : width,dropdown ? 200 : 28);
}
static int field_page(int i) { return i<=F_ENABLED ? 0 : i<=F_HOTKEY ? 1 : i==F_RUNTIME ? 3 : 4; }
static void show_page(settings *s) {
    for (int i=0;i<F_COUNT;++i) { int show=field_page(i)==s->page ? SW_SHOW : SW_HIDE; ShowWindow(s->fields[i],show); if (s->labels[i]) ShowWindow(s->labels[i],show); }
    ShowWindow(s->directories,s->page==2 ? SW_SHOW : SW_HIDE);
    for (int i=6;i<9;++i) ShowWindow(s->buttons[i],s->page==2 ? SW_SHOW : SW_HIDE);
    ShowWindow(s->tools,s->page==3 ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(s->window,C_PROJECT_HINT),s->page==0 ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(s->window,C_DIRECTORY_HINT),s->page==2 ? SW_SHOW : SW_HIDE);
}
static const char *first_workspace(yyjson_mut_val *p) {
    return str(yyjson_mut_arr_get(yyjson_mut_obj_get(p,"workspaces"),0),"id");
}
static void list_projects(settings *s) {
    SendMessageW(s->projects,LB_RESETCONTENT,0,0);
    yyjson_mut_val *projects=yyjson_mut_obj_get(yyjson_mut_doc_get_root(s->doc),"projects");
    for (size_t i=0;i<yyjson_mut_arr_size(projects);++i) { wchar_t *w=wtext(str(yyjson_mut_arr_get(projects,i),"id")); SendMessageW(s->projects,LB_ADDSTRING,0,(LPARAM)(w ? w : L"")); free(w); }
    SendMessageW(s->projects,LB_SETCURSEL,(WPARAM)s->selected,0);
}
static void render_directories(settings *s) {
    ListView_DeleteAllItems(s->directories); yyjson_mut_val *p=project(s);
    for (unsigned kind=0;kind<2;++kind) {
        yyjson_mut_val *items=yyjson_mut_obj_get(p,kind ? "extra_directories" : "workspaces");
        for (size_t i=0;i<yyjson_mut_arr_size(items);++i) {
            yyjson_mut_val *d=yyjson_mut_arr_get(items,i); wchar_t *id=wtext(str(d,"id")),*path=wtext(str(d,"path"));
            LVITEMW row={0}; row.mask=LVIF_TEXT|LVIF_PARAM; row.iItem=ListView_GetItemCount(s->directories); row.pszText=id;
            row.lParam=(LPARAM)(i|(kind<<16)); int index=(int)SendMessageW(s->directories,LVM_INSERTITEMW,0,(LPARAM)&row);
            ListView_SetItemText(s->directories,index,1,kind ? L"额外目录" : L"工作目录");
            yyjson_mut_val *ro=yyjson_mut_obj_get(d,"read_only"); ListView_SetItemText(s->directories,index,2,(!ro || yyjson_mut_get_bool(ro)) ? L"只读" : L"读写");
            ListView_SetItemText(s->directories,index,3,path); free(id); free(path);
        }
    }
}
static void load_project(settings *s) {
    s->loading=true; yyjson_mut_val *root=yyjson_mut_doc_get_root(s->doc),*p=project(s),*policy=yyjson_mut_obj_get(p,"policy"),*gateway=yyjson_mut_obj_get(root,"gateway");
    const char *keys[]={"id","token_env","data_dir","default_workspace_id"};
    for (int i=0;i<4;++i) put(s->fields[i],str(p,keys[i]));
    if (!*str(p,"default_workspace_id")) put(s->fields[F_CWD],first_workspace(p));
    yyjson_mut_val *enabled=yyjson_mut_obj_get(p,"enabled"); check(s->fields[F_ENABLED],!enabled || yyjson_mut_get_bool(enabled));
    const char *mode=str(policy,"approval_mode"); SendMessageW(s->fields[F_APPROVAL],CB_SETCURSEL,!strcmp(mode,"never") ? 2 : !strcmp(mode,"workspace_auto") ? 1 : 0,0);
    yyjson_mut_val *files=yyjson_mut_obj_get(policy,"files"),*execution=yyjson_mut_obj_get(policy,"execution"),*desktop=yyjson_mut_obj_get(policy,"desktop");
    SendMessageW(s->fields[F_SCOPE],CB_SETCURSEL,!strcmp(str(files,"scope"),"current_user") ? 1 : 0,0);
    yyjson_mut_val *account_read_only=yyjson_mut_obj_get(files,"current_user_read_only");
    check(s->fields[F_READONLY],!account_read_only || yyjson_mut_get_bool(account_read_only));
    check(s->fields[F_EXECUTION],yyjson_mut_get_bool(yyjson_mut_obj_get(execution,"enabled")));
    check(s->fields[F_ADMIN],yyjson_mut_get_bool(yyjson_mut_obj_get(execution,"administrator_allowed")));
    SendMessageW(s->fields[F_IDENTITY],CB_SETCURSEL,!strcmp(str(execution,"default_identity"),"current_user") ? 1 : 0,0);
    const char *flags[]={"enabled","control","clipboard_read","clipboard_write"};
    for (int i=0;i<4;++i) check(s->fields[F_DESKTOP+i],yyjson_mut_get_bool(yyjson_mut_obj_get(desktop,flags[i])));
    uint64_t lease=yyjson_mut_get_uint(yyjson_mut_obj_get(desktop,"lease_seconds")); char number[32]; snprintf(number,sizeof(number),"%llu",(unsigned long long)(lease ? lease : 30)); put(s->fields[F_LEASE],number);
    put(s->fields[F_HOTKEY],*str(desktop,"emergency_key") ? str(desktop,"emergency_key") : "F9");
    yyjson_mut_val *tools=yyjson_mut_obj_get(policy,"tools");
    for (int i=0;i<SB_PROJECT_TOOL_COUNT;++i) {
        bool found=false;
        for (size_t j=0;j<yyjson_mut_arr_size(tools);++j) { const char *name=yyjson_mut_get_str(yyjson_mut_arr_get(tools,j)); if (name && !strcmp(name,sb_project_tool_name((size_t)i))) found=true; }
        /* Missing tools authorizes only the original six read/metadata tools. */
        if (!tools) found=i<6;
        ListView_SetCheckState(s->tools,i,found);
    }
    char runtime[SB_TEXT_LIMIT]={0}; size_t used=0; yyjson_mut_val *resources=yyjson_mut_obj_get(execution,"runtime_files");
    for (size_t i=0;i<yyjson_mut_arr_size(resources);++i) { const char *v=yyjson_mut_get_str(yyjson_mut_arr_get(resources,i)); if (v && used+strlen(v)+3<sizeof(runtime)) { used+=(size_t)snprintf(runtime+used,sizeof(runtime)-used,"%s\r\n",v); } }
    put(s->fields[F_RUNTIME],runtime);
    uint64_t port=yyjson_mut_get_uint(yyjson_mut_obj_get(gateway,"port")); snprintf(number,sizeof(number),"%llu",(unsigned long long)(port ? port : 8765)); put(s->fields[F_PORT],number);
    put(s->fields[F_PUBLIC],str(gateway,"public_url")); put(s->fields[F_OWNER],str(gateway,"owner_token_env")); put(s->fields[F_DEFAULT],str(gateway,"default_project_id"));
    bool legacy=yyjson_mut_obj_get(p,"legacy_config")!=NULL;
    for (int i=0;i<=F_RUNTIME;++i) EnableWindow(s->fields[i],p && !legacy);
    EnableWindow(s->directories,p && !legacy); EnableWindow(s->tools,p && !legacy);
    for (int i=6;i<9;++i) EnableWindow(s->buttons[i],p && !legacy);
    render_directories(s); show_page(s); s->loading=false;
}
static bool read_number(settings *s,int field_id,yyjson_mut_val *parent,const char *key) {
    char *value=text(s->fields[field_id]),*end=NULL; unsigned long long number=value ? strtoull(value,&end,10) : 0;
    bool ok=value && *value && strspn(value,"0123456789")==strlen(value) && end && !*end;
    if (ok) { yyjson_mut_obj_remove_key(parent,key); yyjson_mut_obj_add_uint(s->doc,parent,key,(uint64_t)number); }
    free(value); return ok;
}
static bool commit(settings *s) {
    if (s->loading || !s->doc) return true;
    yyjson_mut_val *p=project(s);
    if (p && !yyjson_mut_obj_get(p,"legacy_config")) {
        yyjson_mut_val *gateway=object(s,yyjson_mut_doc_get_root(s->doc),"gateway");
        char *id=text(s->fields[F_ID]),*default_id=text(s->fields[F_DEFAULT]);
        if (id && default_id && strcmp(id,str(p,"id")) && !strcmp(str(p,"id"),str(gateway,"default_project_id")) && !strcmp(default_id,str(p,"id"))) {
            string(s,gateway,"default_project_id",id,true); bool loading=s->loading; s->loading=true;
            put(s->fields[F_DEFAULT],id); s->loading=loading;
        }
        free(id); free(default_id);
        const char *keys[]={"id","token_env","data_dir","default_workspace_id"};
        for (int i=0;i<4;++i) { char *v=text(s->fields[i]); string(s,p,keys[i],v,i==3); free(v); }
        set_bool(s,p,"enabled",checked(s->fields[F_ENABLED]));
        yyjson_mut_val *policy=object(s,p,"policy"),*files=object(s,policy,"files"),*execution=object(s,policy,"execution"),*desktop=object(s,policy,"desktop");
        const char *modes[]={"always","workspace_auto","never"}; int mode=(int)SendMessageW(s->fields[F_APPROVAL],CB_GETCURSEL,0,0);
        string(s,policy,"approval_mode",modes[mode>=0 && mode<3 ? mode : 0],false);
        bool account=SendMessageW(s->fields[F_SCOPE],CB_GETCURSEL,0,0)==1;
        string(s,files,"scope",account ? "current_user" : "configured",false);
        yyjson_mut_obj_remove_key(files,"current_user_read_only"); if (account) set_bool(s,files,"current_user_read_only",checked(s->fields[F_READONLY]));
        set_bool(s,execution,"enabled",checked(s->fields[F_EXECUTION])); set_bool(s,execution,"administrator_allowed",checked(s->fields[F_ADMIN]));
        bool current=SendMessageW(s->fields[F_IDENTITY],CB_GETCURSEL,0,0)==1;
        string(s,execution,"default_identity",current ? "current_user" : "restricted",false);
        const char *flags[]={"enabled","control","clipboard_read","clipboard_write"};
        for (int i=0;i<4;++i) set_bool(s,desktop,flags[i],checked(s->fields[F_DESKTOP+i]));
        if (!read_number(s,F_LEASE,desktop,"lease_seconds")) { error_box(s->window,"控制租约秒数必须填写整数（1～120）。"); return false; }
        char *hotkey=text(s->fields[F_HOTKEY]); string(s,desktop,"emergency_key",hotkey,false); free(hotkey);
        yyjson_mut_val *tools=yyjson_mut_arr(s->doc); for (int i=0;i<SB_PROJECT_TOOL_COUNT;++i) if (ListView_GetCheckState(s->tools,i)) yyjson_mut_arr_add_str(s->doc,tools,sb_project_tool_name((size_t)i));
        yyjson_mut_obj_remove_key(policy,"tools"); yyjson_mut_obj_add_val(s->doc,policy,"tools",tools);
        char *runtime=text(s->fields[F_RUNTIME]); yyjson_mut_obj_remove_key(execution,"runtime_files");
        if (runtime && *runtime) {
            yyjson_mut_val *array=yyjson_mut_arr(s->doc); char *context=NULL;
            for (char *line=strtok_s(runtime,"\r\n",&context);line;line=strtok_s(NULL,"\r\n",&context)) yyjson_mut_arr_add_strcpy(s->doc,array,line);
            yyjson_mut_obj_add_val(s->doc,execution,"runtime_files",array);
        }
        free(runtime);
    }
    yyjson_mut_val *gateway=object(s,yyjson_mut_doc_get_root(s->doc),"gateway");
    if (!read_number(s,F_PORT,gateway,"port")) { error_box(s->window,"本机端口必须填写整数（1024～65535）。"); return false; }
    const char *global_keys[]={"public_url","owner_token_env","default_project_id"};
    for (int i=0;i<3;++i) { char *v=text(s->fields[F_PUBLIC+i]); string(s,gateway,global_keys[i],v,true); free(v); }
    return true;
}
static bool form_error(settings *s,size_t index,int page,int field_id,const char *message) {
    if ((size_t)s->selected!=index) { s->selected=(int)index; list_projects(s); load_project(s); }
    s->page=page; SendMessageW(s->tabs,TCM_SETCURSEL,(WPARAM)page,0); show_page(s);
    error_box(s->window,message); SetFocus(field_id>=0 ? s->fields[field_id] : s->directories); return false;
}
static unsigned char path_character(char c) {
    if (c=='\\') return '/';
    return (unsigned char)(c>='A' && c<='Z' ? c+('a'-'A') : c);
}
static bool path_overlap(const char *a,const char *b) {
    size_t al=strlen(a),bl=strlen(b);
    while (al && path_character(a[al-1])=='/') --al;
    while (bl && path_character(b[bl-1])=='/') --bl;
    if (!al || !bl) return false;
    size_t common=al<bl ? al : bl;
    for (size_t i=0;i<common;++i) if (path_character(a[i])!=path_character(b[i])) return false;
    return al==bl || path_character((al<bl ? b : a)[common])=='/';
}
static bool validate_form(settings *s) {
    yyjson_mut_val *root=yyjson_mut_doc_get_root(s->doc),*projects=yyjson_mut_obj_get(root,"projects"),*gateway=yyjson_mut_obj_get(root,"gateway");
    size_t count=yyjson_mut_arr_size(projects);
    for (size_t i=0;i<count;++i) {
        yyjson_mut_val *p=yyjson_mut_arr_get(projects,i);
        if (!sb_identifier(str(p,"id"))) return form_error(s,i,0,F_ID,"项目标识只能使用 1～63 个英文字母、数字、下划线或连字符，例如 ShellBotTest。它用于 MCP 地址，不能包含中文或空格。");
        if (yyjson_mut_obj_get(p,"legacy_config")) continue;
        if (!sb_identifier(str(p,"token_env"))) return form_error(s,i,0,F_TOKEN,"令牌环境变量名称只能使用 1～63 个英文字母、数字、下划线或连字符；这里填写变量名称，不填写令牌值。");
        sb_error location={0};if (!sb_project_path_valid(str(p,"data_dir")) || !sb_platform_storage_path(str(p,"data_dir"),&location)) return form_error(s,i,0,F_DATA,"独立数据目录必须是非 C 盘本地绝对路径；路径可以包含中文。");
        const char *cwd=str(p,"default_workspace_id"); bool found=!*cwd;
        for (unsigned kind=0;kind<2;++kind) {
            yyjson_mut_val *dirs=yyjson_mut_obj_get(p,kind ? "extra_directories" : "workspaces");
            for (size_t d=0;d<yyjson_mut_arr_size(dirs);++d) {
                yyjson_mut_val *dir=yyjson_mut_arr_get(dirs,d);
                if (!kind && !strcmp(cwd,str(dir,"id"))) found=true;
                for (unsigned prior_kind=0;prior_kind<=kind;++prior_kind) {
                    yyjson_mut_val *prior_dirs=yyjson_mut_obj_get(p,prior_kind ? "extra_directories" : "workspaces");
                    size_t prior_count=prior_kind==kind ? d : yyjson_mut_arr_size(prior_dirs);
                    for (size_t prior=0;prior<prior_count;++prior) {
                        yyjson_mut_val *other=yyjson_mut_arr_get(prior_dirs,prior);
                        if (!strcmp(str(dir,"id"),str(other,"id"))) return form_error(s,i,2,-1,"同一项目中的目录标识不能重复，工作目录与额外目录也需要使用不同标识。");
                        if (path_overlap(str(dir,"path"),str(other,"path"))) return form_error(s,i,2,-1,"同一项目中的工作目录与额外目录不能相同或互相包含。");
                    }
                }
                for (size_t other=0;other<count;++other) {
                    yyjson_mut_val *peer=yyjson_mut_arr_get(projects,other);
                    if (path_overlap(str(dir,"path"),str(peer,"data_dir"))) {
                        bool editing_data=(size_t)s->selected==other;
                        return form_error(s,editing_data ? other : i,editing_data ? 0 : 2,editing_data ? F_DATA : -1,"数据目录与工作目录或额外目录不能相同，也不能互相包含。请使用分开的目录，例如 D:\\ShellBot\\Data 和 D:\\ShellBot\\Workspace。修改数据目录不会自动改变 MCP 的文件访问范围。");
                    }
                }
            }
        }
        if (!found) return form_error(s,i,0,F_CWD,"默认工作目录标识必须对应“目录”页中的一个工作目录标识；不能填写项目标识或目录路径。留空时使用第一个工作目录。");
        yyjson_mut_val *policy=yyjson_mut_obj_get(p,"policy"),*files=yyjson_mut_obj_get(policy,"files"),*execution=yyjson_mut_obj_get(policy,"execution"),*desktop=yyjson_mut_obj_get(policy,"desktop");
        if (yyjson_mut_get_bool(yyjson_mut_obj_get(execution,"administrator_allowed")) &&
            (strcmp(str(files,"scope"),"current_user") || strcmp(str(execution,"default_identity"),"current_user")))
            return form_error(s,i,1,F_ADMIN,"允许管理员操作时，文件范围须设为“当前账户可访问范围”，默认命令身份须设为“当前账户普通权限”。只做文件验收时请关闭管理员操作。");
        if (!yyjson_mut_get_bool(yyjson_mut_obj_get(desktop,"enabled")) &&
            (yyjson_mut_get_bool(yyjson_mut_obj_get(desktop,"control")) || yyjson_mut_get_bool(yyjson_mut_obj_get(desktop,"clipboard_read")) || yyjson_mut_get_bool(yyjson_mut_obj_get(desktop,"clipboard_write"))))
            return form_error(s,i,1,F_DESKTOP,"启用鼠标键盘输入或剪贴板访问前，须先启用“允许桌面观察”；只做文件验收时可全部关闭。");
        yyjson_mut_val *lease_value=yyjson_mut_obj_get(desktop,"lease_seconds"); uint64_t lease=lease_value ? yyjson_mut_get_uint(lease_value) : 30;
        if (lease<1 || lease>120) return form_error(s,i,1,F_LEASE,"控制租约秒数必须在 1～120 之间。");
        for (size_t j=0;j<i;++j) {
            yyjson_mut_val *prior=yyjson_mut_arr_get(projects,j);
            size_t selected=(size_t)s->selected==j ? j : i;
            if (!strcmp(str(p,"id"),str(prior,"id"))) return form_error(s,selected,0,F_ID,"项目标识重复，请为每个项目设置不同的标识。");
            if (!_stricmp(str(p,"token_env"),str(prior,"token_env"))) return form_error(s,selected,0,F_TOKEN,"不同项目必须使用不同的令牌环境变量名称（不区分大小写）。");
            if (path_overlap(str(p,"data_dir"),str(prior,"data_dir"))) return form_error(s,selected,0,F_DATA,"不同项目的数据目录不能相同或互相包含。");
        }
    }
    uint64_t port=yyjson_mut_get_uint(yyjson_mut_obj_get(gateway,"port"));
    if (port<1024 || port>65535) return form_error(s,(size_t)s->selected,4,F_PORT,"本机端口必须在 1024～65535 之间。");
    const char *owner=str(gateway,"owner_token_env"),*default_id=str(gateway,"default_project_id"); bool found=!*default_id;
    if (*owner && !sb_identifier(owner)) return form_error(s,(size_t)s->selected,4,F_OWNER,"所有者令牌环境变量名称只能使用 1～63 个英文字母、数字、下划线或连字符。");
    for (size_t i=0;i<count;++i) {
        yyjson_mut_val *p=yyjson_mut_arr_get(projects,i);
        if (!strcmp(default_id,str(p,"id"))) found=true;
        if (*owner && !_stricmp(owner,str(p,"token_env"))) return form_error(s,i,4,F_OWNER,"所有者审批必须使用独立的令牌环境变量，不能与项目令牌共用。");
    }
    if (!found) return form_error(s,(size_t)s->selected,4,F_DEFAULT,"兼容默认项目必须填写已存在的项目标识；不需要兼容 /mcp 地址时可以留空。");
    return true;
}
static bool replace_bytes(settings *s,const char *json,size_t length) {
    sb_project_registry registry={0}; sb_error error={0};
    bool valid=sb_projects_parse(&registry,json,length,&error); sb_projects_free(&registry);
    if (!valid) { error_box(s->window,error.text); return false; }
    yyjson_doc *input=yyjson_read(json,length,0); yyjson_mut_doc *doc=input ? yyjson_doc_mut_copy(input,NULL) : NULL; yyjson_doc_free(input);
    char *original=doc ? calloc(length+1,1) : NULL;
    if (!doc || !original) { yyjson_mut_doc_free(doc); free(original); error_box(s->window,"Cannot allocate configuration document."); return false; }
    memcpy(original,json,length);
    yyjson_mut_doc_free(s->doc); s->doc=doc; free(s->original); s->original=original; s->selected=0;
    s->dirty=false; list_projects(s); load_project(s); return true;
}
static bool replace_document(settings *s,const char *json) { return replace_bytes(s,json,strlen(json)); }
static void update_workspace_reference(settings *s,const char *old_id,const char *replacement) {
    yyjson_mut_val *p=project(s); const char *current=str(p,"default_workspace_id");
    if (*current && (!old_id || strcmp(current,old_id))) return;
    const char *id=replacement ? replacement : first_workspace(p);
    string(s,p,"default_workspace_id",id,true); bool loading=s->loading; s->loading=true;
    put(s->fields[F_CWD],id); s->loading=loading;
}
static void directory_edit(settings *s,int action) {
    yyjson_mut_val *p=project(s); if (!p || !commit(s)) return;
    int index=ListView_GetNextItem(s->directories,-1,LVNI_SELECTED); LVITEMW row={0}; row.mask=LVIF_PARAM; row.iItem=index;
    bool existing=index>=0 && SendMessageW(s->directories,LVM_GETITEMW,0,(LPARAM)&row);
    if (action!=C_DIR_ADD && !existing) return;
    unsigned kind=existing ? ((unsigned)row.lParam>>16) : 0; size_t item=existing ? (size_t)(row.lParam&0xffff) : 0;
    yyjson_mut_val *array=yyjson_mut_obj_get(p,kind ? "extra_directories" : "workspaces"),*old=existing ? yyjson_mut_arr_get(array,item) : NULL;
    if (action==C_DIR_REMOVE) { yyjson_mut_arr_remove(array,item); update_workspace_reference(s,str(old,"id"),NULL); s->dirty=true; render_directories(s); return; }
    modal m={0}; m.owner=s->window; m.font=s->font; m.text=sb_strdup(existing ? str(old,"path") : ""); m.name=sb_strdup(existing ? str(old,"id") : "workspace");
    m.is_workspace=!kind; yyjson_mut_val *read_only=yyjson_mut_obj_get(old,"read_only"); m.read_only=!read_only || yyjson_mut_get_bool(read_only);
    if (show_modal(&m)) {
        yyjson_mut_val *d=yyjson_mut_obj(s->doc); string(s,d,"id",m.name,false); string(s,d,"path",m.text,false); set_bool(s,d,"read_only",m.read_only);
        if (existing) yyjson_mut_arr_remove(array,item);
        yyjson_mut_val *target=yyjson_mut_obj_get(p,m.is_workspace ? "workspaces" : "extra_directories");
        if (!target) { target=yyjson_mut_arr(s->doc); yyjson_mut_obj_add_val(s->doc,p,m.is_workspace ? "workspaces" : "extra_directories",target); }
        yyjson_mut_arr_add_val(target,d); update_workspace_reference(s,existing ? str(old,"id") : NULL,m.is_workspace ? m.name : NULL); s->dirty=true; render_directories(s);
    }
    free(m.text); free(m.name);
}
static void save(settings *s) {
    if (!commit(s) || !validate_form(s)) return;
    char *json=yyjson_mut_write(s->doc,0,NULL); sb_project_registry registry={0}; sb_error error={0};
    bool valid=json && sb_projects_parse(&registry,json,strlen(json),&error); sb_projects_free(&registry);
    if (!valid) { error_box(s->window,*error.text ? error.text : "Cannot serialize configuration."); free(json); return; }
    if (s->embedded) { s->result=json; s->dirty=false; DestroyWindow(s->window); return; }
    /* Owner-edited revisions cannot revive old permits. Every retained project
       advances relative to the loaded source; new projects begin at one. */
    sb_project_registry original={0};
    if (s->original) sb_projects_parse(&original,s->original,strlen(s->original),&error);
    yyjson_mut_val *projects=yyjson_mut_obj_get(yyjson_mut_doc_get_root(s->doc),"projects");
    for (size_t i=0;i<yyjson_mut_arr_size(projects);++i) {
        yyjson_mut_val *p=yyjson_mut_arr_get(projects,i); const sb_project *old=sb_project_find(&original,str(p,"id"));
        if (old && old->policy_revision==UINT64_MAX) { error_box(s->window,"Policy revision is exhausted."); sb_projects_free(&original); free(json); return; }
        yyjson_mut_obj_remove_key(p,"policy_revision"); yyjson_mut_obj_add_uint(s->doc,p,"policy_revision",old ? old->policy_revision+1 : 1);
    }
    sb_projects_free(&original); free(json); json=yyjson_mut_write(s->doc,0,NULL);
    char *output=text(s->output);
    if (!output || !*output) {
        wchar_t path[SB_PATH_CAP]=L"projects.json";
        if (!file_dialog(s->window,true,path)) { free(output); free(json); return; }
        SetWindowTextW(s->output,path); free(output); output=text(s->output);
    }
    size_t size=strlen(output)+32; char *backup=malloc(size); if (backup) snprintf(backup,size,"%s.source-backup.json",output);
    bool ok=backup && json && sb_platform_export_config(output,backup,json,strlen(json),s->original ? s->original : json,s->original ? strlen(s->original) : strlen(json),&error);
    if (!ok) error_box(s->window,*error.text ? error.text : "Configuration export failed.");
    else {
        put(s->source,output); replace_document(s,json);
        SetWindowTextW(s->status,L"配置与原文备份已保存。停止管理器后，使用新配置启动；自动启动需另行切换快照。");
        SetPropW(s->window,L"ShellBotConfigurationSaved",(HANDLE)1);
    }
    free(output); free(backup); free(json);
}
static void command(settings *s,int id) {
    if (id==C_SAVE) { save(s); return; }
    if (id>=C_DIR_ADD && id<=C_DIR_REMOVE) { directory_edit(s,id); return; }
    if (id==C_OPEN || id==C_NEW) {
        if (s->dirty && MessageBoxW(s->window,L"放弃尚未保存的更改？",L"项目配置",MB_YESNO|MB_DEFBUTTON2)!=IDYES) return;
        if (id==C_NEW) {
            sb_error location={0};char *base=sb_platform_default_data_dir(&location);
            if(!base)base=sb_desktop_choose_storage("",location.text);if(!base)return;
            char *data=sb_desktop_join(base,"Projects\\project-a");free(base);
            yyjson_doc *parsed=yyjson_read("{\"format_version\":1,\"projects\":[{\"id\":\"project-a\",\"data_dir\":\"\",\"token_env\":\"SHELLBOT_PROJECT_A_TOKEN\",\"workspaces\":[],\"policy\":{\"approval_mode\":\"always\",\"tools\":[\"host_info\",\"host_capabilities\",\"fs_list\",\"fs_read\",\"fs_search\",\"image_view\"]}}]}",strlen("{\"format_version\":1,\"projects\":[{\"id\":\"project-a\",\"data_dir\":\"\",\"token_env\":\"SHELLBOT_PROJECT_A_TOKEN\",\"workspaces\":[],\"policy\":{\"approval_mode\":\"always\",\"tools\":[\"host_info\",\"host_capabilities\",\"fs_list\",\"fs_read\",\"fs_search\",\"image_view\"]}}]}"),0);yyjson_mut_doc *draft=parsed?yyjson_doc_mut_copy(parsed,NULL):NULL;yyjson_doc_free(parsed);
            yyjson_mut_val *entry=draft?yyjson_mut_arr_get(yyjson_mut_obj_get(yyjson_mut_doc_get_root(draft),"projects"),0):NULL;
            if(entry && data){yyjson_mut_obj_remove_key(entry,"data_dir");yyjson_mut_obj_add_strcpy(draft,entry,"data_dir",data);}
            char *json=entry && data?yyjson_mut_write(draft,0,NULL):NULL;bool replaced=json && replace_document(s,json);
            free(json);free(data);yyjson_mut_doc_free(draft);if(!replaced)return;
            SetWindowTextW(s->source,L"新配置"); SetWindowTextW(s->output,L""); s->dirty=true;
        } else {
            wchar_t path[SB_PATH_CAP]=L""; if (!file_dialog(s->window,false,path)) return;
            int bytes=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,path,-1,NULL,0,NULL,NULL); char *name=bytes ? malloc((size_t)bytes) : NULL;
            if (name) WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,path,-1,name,bytes,NULL,NULL);
            sb_error error={0}; size_t length=0; char *json=name ? sb_platform_load_config(name,&length,&error) : NULL;
            if (!json) error_box(s->window,error.text); else if (replace_bytes(s,json,length)) { SetWindowTextW(s->source,path); SetWindowTextW(s->output,L""); }
            free(json); free(name);
        }
        return;
    }
    if (!commit(s)) return;
    yyjson_mut_val *p=project(s),*projects=yyjson_mut_obj_get(yyjson_mut_doc_get_root(s->doc),"projects");
    if (id==C_ADD) {
        if (yyjson_mut_arr_size(projects)>=SB_MAX_PROJECTS) { error_box(s->window,"Maximum 16 projects."); return; }
        char name[64]; unsigned i=1; bool found;
        do { snprintf(name,sizeof(name),"project-%u",i++); found=false; for (size_t j=0;j<yyjson_mut_arr_size(projects);++j) if (!strcmp(str(yyjson_mut_arr_get(projects,j),"id"),name)) found=true; } while (found);
        yyjson_mut_val *added=yyjson_mut_obj(s->doc); string(s,added,"id",name,false); string(s,added,"token_env","",false); string(s,added,"data_dir","",false);
        yyjson_mut_arr_add_val(projects,added); s->selected=(int)yyjson_mut_arr_size(projects)-1; s->dirty=true; list_projects(s); load_project(s);
    } else if (id==C_REMOVE && p) {
        if (yyjson_mut_arr_size(projects)<=1) { error_box(s->window,"Keep at least one project."); return; }
        if (MessageBoxW(s->window,L"从新配置中移除此项目？运行中的项目和磁盘数据均保留。",L"移除项目",MB_YESNO|MB_DEFBUTTON2)!=IDYES) return;
        yyjson_mut_val *gateway=yyjson_mut_obj_get(yyjson_mut_doc_get_root(s->doc),"gateway");
        if (!strcmp(str(p,"id"),str(gateway,"default_project_id"))) yyjson_mut_obj_remove_key(gateway,"default_project_id");
        yyjson_mut_arr_remove(projects,(size_t)s->selected); s->selected=0; s->dirty=true; list_projects(s); load_project(s);
    } else if (id==C_JSON && p) {
        modal m={0}; m.owner=s->window; m.font=s->font; m.json=true; m.text=yyjson_mut_val_write(p,YYJSON_WRITE_PRETTY,NULL);
        if (show_modal(&m)) {
            yyjson_doc *input=yyjson_read(m.text,strlen(m.text),0);
            if (input && yyjson_is_obj(yyjson_doc_get_root(input))) {
                yyjson_mut_val *old=yyjson_mut_arr_get(projects,(size_t)s->selected);
                yyjson_mut_arr_replace(projects,(size_t)s->selected,yyjson_val_mut_copy(s->doc,yyjson_doc_get_root(input)));
                char *candidate=yyjson_mut_write(s->doc,0,NULL); sb_project_registry registry={0}; sb_error error={0};
                bool valid=candidate && sb_projects_parse(&registry,candidate,strlen(candidate),&error);
                free(candidate); sb_projects_free(&registry);
                if (!valid) { yyjson_mut_arr_replace(projects,(size_t)s->selected,old); error_box(s->window,error.text); }
                else { s->dirty=true; list_projects(s); load_project(s); }
            } else error_box(s->window,"Advanced project settings must be a JSON object.");
            yyjson_doc_free(input);
        }
        free(m.text);
    }
}
static LRESULT CALLBACK settings_proc(HWND window,UINT message,WPARAM wp,LPARAM lp) {
    settings *s=(settings *)GetWindowLongPtrW(window,GWLP_USERDATA);
    if (message==WM_NCCREATE) { s=((CREATESTRUCTW *)lp)->lpCreateParams; s->window=window; SetWindowLongPtrW(window,GWLP_USERDATA,(LONG_PTR)s); }
    if (!s) return DefWindowProcW(window,message,wp,lp);
    switch (message) {
    case DM_GETDEFID:return MAKELRESULT(C_SAVE,DC_HASDEFID);
    case WM_CREATE: {
        s->font=sb_native_font(window);s->dpi=sb_native_dpi(window);
        const wchar_t *buttons[]={L"打开配置…",L"新配置",L"添加项目",L"移除项目",L"高级配置…",L"保存新配置…"};
        for (int i=0;i<6;++i) s->buttons[i]=child(window,s->font,L"BUTTON",buttons[i],WS_TABSTOP,C_OPEN+i,18+i*154,16,144,32);
        s->source=child(window,s->font,L"EDIT",L"",ES_READONLY|ES_AUTOHSCROLL,C_SOURCE,18,60,1082,28);
        s->projects=child(window,s->font,L"LISTBOX",L"",WS_TABSTOP|WS_BORDER|WS_VSCROLL|LBS_NOTIFY,C_PROJECTS,18,104,202,514);
        s->tabs=child(window,s->font,WC_TABCONTROLW,L"",WS_TABSTOP,C_TABS,238,104,862,36);
        const wchar_t *tabs[]={L"项目",L"权限",L"目录",L"工具与运行资源",L"连接设置"};
        for (int i=0;i<5;++i) { TCITEMW tab={0}; tab.mask=TCIF_TEXT; tab.pszText=(wchar_t *)tabs[i]; SendMessageW(s->tabs,TCM_INSERTITEMW,(WPARAM)i,(LPARAM)&tab); }
        const wchar_t *basic[]={L"项目标识",L"令牌环境变量",L"独立数据目录",L"默认工作目录标识"};
        for (int i=0;i<4;++i) field(s,i,basic[i],258,166+i*54,false,false,580);
        field(s,F_ENABLED,L"启用项目",258,396,true,false,500);
        child(window,s->font,L"STATIC",L"项目与目录标识：1～63 个英文字母、数字、_ 或 -；路径支持中文。\r\n数据目录存放内部状态，与工作目录分开设置；两处路径不会自动联动。\r\n默认工作目录标识对应“目录”页；留空时使用第一个工作目录。",0,C_PROJECT_HINT,258,454,820,90);
        field(s,F_APPROVAL,L"审批方式",258,158,false,true,400); const wchar_t *modes[]={L"每次审批",L"工作目录自动、命令审批",L"范围内无需逐次审批"}; combo(s->fields[F_APPROVAL],modes,3);
        field(s,F_SCOPE,L"文件范围",258,202,false,true,400); const wchar_t *scopes[]={L"仅配置的目录",L"当前账户可访问范围"}; combo(s->fields[F_SCOPE],scopes,2);
        field(s,F_READONLY,L"账户文件范围只读",258,246,true,false,500);
        field(s,F_EXECUTION,L"允许执行命令",258,286,true,false,200);
        field(s,F_ADMIN,L"允许管理员操作",700,286,true,false,180);
        field(s,F_IDENTITY,L"默认命令身份",258,326,false,true,400); const wchar_t *identities[]={L"受限 LPAC",L"当前账户普通权限"}; combo(s->fields[F_IDENTITY],identities,2);
        field(s,F_DESKTOP,L"允许桌面观察",258,370,true,false,200); field(s,F_CONTROL,L"允许鼠标键盘输入",700,370,true,false,180);
        field(s,F_CLIPREAD,L"允许读取剪贴板",258,410,true,false,200); field(s,F_CLIPWRITE,L"允许写入剪贴板",700,410,true,false,180);
        field(s,F_LEASE,L"控制租约上限（秒）",258,458,false,false,200); field(s,F_HOTKEY,L"紧急键（Ctrl+Alt+）",258,506,false,false,200);
        s->directories=child(window,s->font,WC_LISTVIEWW,L"",WS_TABSTOP|LVS_REPORT|LVS_SINGLESEL|LVS_SHOWSELALWAYS|WS_BORDER,C_DIRECTORIES,258,164,820,366);
        ListView_SetExtendedListViewStyle(s->directories,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);
        const wchar_t *columns[]={L"标识",L"类别",L"权限",L"绝对路径"}; int widths[]={130,104,80,490};
        for (int i=0;i<4;++i) { LVCOLUMNW c={0}; c.mask=LVCF_TEXT|LVCF_WIDTH; c.pszText=(wchar_t *)columns[i]; c.cx=px(window,widths[i]); SendMessageW(s->directories,LVM_INSERTCOLUMNW,(WPARAM)i,(LPARAM)&c); }
        const wchar_t *dirs[]={L"添加目录…",L"编辑目录…",L"移除目录"}; for (int i=0;i<3;++i) s->buttons[6+i]=child(window,s->font,L"BUTTON",dirs[i],WS_TABSTOP,C_DIR_ADD+i,258+i*164,552,150,32);
        child(window,s->font,L"STATIC",L"目录标识独立于项目标识；重命名工作目录会同步默认引用。\r\n此处决定 MCP 可访问的文件范围，不能与内部数据目录重叠。",0,C_DIRECTORY_HINT,258,588,820,42);
        s->tools=child(window,s->font,WC_LISTVIEWW,L"",WS_TABSTOP|LVS_REPORT|LVS_NOCOLUMNHEADER|WS_BORDER,C_TOOLS,258,164,330,430);
        ListView_SetExtendedListViewStyle(s->tools,LVS_EX_CHECKBOXES|LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);
        LVCOLUMNW column={0}; column.mask=LVCF_WIDTH; column.cx=px(window,302); SendMessageW(s->tools,LVM_INSERTCOLUMNW,0,(LPARAM)&column);
        for (int i=0;i<SB_PROJECT_TOOL_COUNT;++i) { wchar_t *name=wtext(sb_project_tool_name((size_t)i)); LVITEMW row={0}; row.mask=LVIF_TEXT; row.iItem=i; row.pszText=name; SendMessageW(s->tools,LVM_INSERTITEMW,0,(LPARAM)&row); free(name); }
        s->labels[F_RUNTIME]=child(window,s->font,L"STATIC",L"受限任务只读运行文件（每行一个）",0,0,612,164,460,28);
        s->fields[F_RUNTIME]=child(window,s->font,L"EDIT",L"",WS_TABSTOP|ES_MULTILINE|ES_AUTOVSCROLL|WS_VSCROLL|WS_HSCROLL,4100+F_RUNTIME,612,204,466,390);
        const wchar_t *globals[]={L"本机端口",L"公共地址（可选）",L"所有者令牌环境变量",L"兼容默认项目（可选）"}; for (int i=0;i<4;++i) field(s,F_PORT+i,globals[i],258,166+i*60,false,false,580);
        child(window,s->font,L"STATIC",L"保存到新文件",0,C_OUTPUT_LABEL,18,640,180,28); s->output=child(window,s->font,L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,C_OUTPUT,202,636,898,30);
        s->status=child(window,s->font,L"STATIC",L"保存会校验完整配置并保留原文备份；应用新配置需要明确重启管理器。",0,C_STATUS,18,690,1082,40); return 0;
    }
    case WM_COMMAND:
        if(LOWORD(wp)==IDCANCEL){SendMessageW(window,WM_CLOSE,0,0);return 0;}
        if(LOWORD(wp)==IDOK){command(s,C_SAVE);return 0;}
        if (LOWORD(wp)==C_PROJECTS && HIWORD(wp)==LBN_SELCHANGE) {
            int selected=(int)SendMessageW(s->projects,LB_GETCURSEL,0,0);
            if (commit(s)) { s->selected=selected; list_projects(s); load_project(s); } else SendMessageW(s->projects,LB_SETCURSEL,(WPARAM)s->selected,0); return 0;
        }
        if (HIWORD(wp)==BN_CLICKED && LOWORD(wp)>=C_OPEN && LOWORD(wp)<=C_DIR_REMOVE) { command(s,LOWORD(wp)); return 0; }
        if (!s->loading && LOWORD(wp)>=4100 && LOWORD(wp)<4100+F_COUNT &&
            (HIWORD(wp)==EN_CHANGE || HIWORD(wp)==CBN_SELCHANGE || HIWORD(wp)==BN_CLICKED)) s->dirty=true;
        return 0;
    case WM_NOTIFY: {
        NMHDR *header=(NMHDR *)lp;
        if (header->idFrom==C_TABS && header->code==TCN_SELCHANGE) {
            if (commit(s)) { s->page=(int)SendMessageW(s->tabs,TCM_GETCURSEL,0,0); list_projects(s); render_directories(s); }
            else SendMessageW(s->tabs,TCM_SETCURSEL,(WPARAM)s->page,0);
            show_page(s);
        }
        if (!s->loading && header->idFrom==C_TOOLS && header->code==LVN_ITEMCHANGED) s->dirty=true;
        return 0;
    }
    case WM_DPICHANGED:sb_native_dpi_changed(window,&s->font,&s->dpi,wp,lp);return 0;
    case WM_CLOSE:
        if (s->dirty && MessageBoxW(window,L"关闭并放弃尚未保存的更改？",L"项目配置",MB_YESNO|MB_DEFBUTTON2)!=IDYES) return 0;
        DestroyWindow(window); return 0;
    case WM_DESTROY: s->closed=true; return 0;
    default: return DefWindowProcW(window,message,wp,lp);
    }
}
static int run_settings(HWND owner,const char *path,const char *snapshot,char **result) {
    HANDLE token=NULL; TOKEN_ELEVATION elevation={0}; DWORD size=0;
    bool allowed=OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token) && sb_win_owner_token_allowed(token) &&
        GetTokenInformation(token,TokenElevation,&elevation,sizeof(elevation),&size) && (!elevation.TokenIsElevated || sb_win_elevated_owner());
    if (token) CloseHandle(token); if (!allowed) return 2;
    settings s={0}; s.owner=owner; s.selected=-1; s.loading=true;s.embedded=result!=NULL;
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX controls={sizeof(controls),ICC_LISTVIEW_CLASSES|ICC_TAB_CLASSES}; InitCommonControlsEx(&controls); OleInitialize(NULL);
    WNDCLASSW wc={0}; wc.lpfnWndProc=settings_proc; wc.hInstance=GetModuleHandleW(NULL); wc.lpszClassName=L"ShellBotProjectSettings";
    wc.hIcon=sb_app_icon();wc.hCursor=LoadCursorW(NULL,IDC_ARROW); wc.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1); RegisterClassW(&wc);
    UINT dpi=sb_native_dpi(owner);RECT r={0,0,MulDiv(1120,(int)dpi,96),MulDiv(746,(int)dpi,96)};
    AdjustWindowRectExForDpi(&r,WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,FALSE,0,dpi);
    RECT position={0};GetWindowRect(owner,&position);
    HWND h=CreateWindowExW(0,wc.lpszClassName,L"ShellBot 项目配置",WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX|WS_CLIPCHILDREN,
        owner?position.left:CW_USEDEFAULT,owner?position.top:CW_USEDEFAULT,r.right-r.left,r.bottom-r.top,owner,NULL,wc.hInstance,&s);
    if (!h) { OleUninitialize(); return 2; }
    if (owner) EnableWindow(owner,FALSE); else FreeConsole();
    bool loaded=false; sb_error error={0}; size_t length=0; char *bytes=path ? sb_platform_load_config(path,&length,&error) : NULL;
    if (snapshot) loaded=replace_document(&s,snapshot); else if (bytes) loaded=replace_bytes(&s,bytes,length);
    if (path && loaded) put(s.source,path); else if (snapshot && loaded) SetWindowTextW(s.source,L"当前管理器配置快照");
    if (!loaded) { if (path && !bytes) error_box(h,error.text); command(&s,C_NEW); s.dirty=false; }
    if (s.embedded) {
        SetWindowTextW(h,L"项目权限与目录设置");
        TabCtrl_DeleteItem(s.tabs,4);
        for (int i=F_PORT;i<=F_DEFAULT;++i) { ShowWindow(s.fields[i],SW_HIDE);ShowWindow(s.labels[i],SW_HIDE); }
        for (int i=0;i<4;++i) ShowWindow(s.buttons[i],SW_HIDE);
        SetWindowTextW(s.buttons[5],L"应用到项目"); ShowWindow(s.source,SW_HIDE); ShowWindow(s.output,SW_HIDE);ShowWindow(GetDlgItem(h,C_OUTPUT_LABEL),SW_HIDE);
        SetWindowPos(s.buttons[4],NULL,px(h,18),px(h,16),px(h,144),px(h,32),SWP_NOZORDER|SWP_NOACTIVATE);
        SetWindowPos(s.buttons[5],NULL,px(h,180),px(h,16),px(h,144),px(h,32),SWP_NOZORDER|SWP_NOACTIVATE);
        SendMessageW(s.fields[F_ID],EM_SETREADONLY,TRUE,0);SendMessageW(s.fields[F_TOKEN],EM_SETREADONLY,TRUE,0);SendMessageW(s.fields[F_DATA],EM_SETREADONLY,TRUE,0);
        SetWindowTextW(s.labels[F_TOKEN],L"内部凭据引用（自动）");
        SetWindowTextW(s.status,L"应用后返回项目弹窗；项目存储目录在项目弹窗中修改。配置由管理界面保存。");
    }
    free(bytes); ShowWindow(h,SW_SHOW); UpdateWindow(h);
    sb_native_modal_loop(h,&s.closed);
    if (owner) { EnableWindow(owner,TRUE); SetActiveWindow(owner); }
    if (result) *result=s.result; else free(s.result);
    yyjson_mut_doc_free(s.doc); free(s.original); DeleteObject(s.font); OleUninitialize(); return 0;
}
int sb_project_settings(HWND owner,const char *path,const char *snapshot) { return run_settings(owner,path,snapshot,NULL); }
char *sb_project_settings_edit(HWND owner,const char *snapshot) { char *result=NULL;run_settings(owner,NULL,snapshot,&result);return result; }
