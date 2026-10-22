#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include "folder_dialog.h"
#include "desktop_app.h"
#include "project_settings.h"
#include "native_ui.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
enum { D_NAME=5001,D_ID,D_DIRECTORY,D_WORKSPACE,D_AUTOMATIC,D_READONLY,D_APPROVAL,D_BROWSE,D_ADVANCED,D_SAVE,D_CANCEL,D_STATUS,D_PORT,D_ORIGIN,D_OWNER };
#define D_COMPLETE (WM_APP+25)
typedef struct {
    HWND window,owner;HFONT font;sb_desktop_app *app;sb_desktop_draft draft;char *original_id,*last_directory;
    bool done,accepted,connection,storage,storage_complete,busy;HANDLE thread;sb_error error;bool saved;unsigned port;char *origin,*notice;UINT dpi;
} dialog;
static char *text(HWND control) {
    int n=GetWindowTextLengthW(control);wchar_t *wide=calloc((size_t)n+1,sizeof(*wide));if (!wide) return NULL;GetWindowTextW(control,wide,n+1);
    int size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide,-1,NULL,0,NULL,NULL);char *out=size ? malloc((size_t)size) : NULL;
    if (out) WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide,-1,out,size,NULL,NULL);free(wide);return out;
}
static void put(HWND parent,int id,const char *value) { wchar_t *wide=sb_desktop_wide(value);SetWindowTextW(GetDlgItem(parent,id),wide ? wide : L"");free(wide); }
static int scale(HWND window,int n) {UINT dpi=GetDpiForWindow(window);return MulDiv(n,(int)(dpi?dpi:GetDpiForSystem()),96);}
static HWND child(dialog *d,const wchar_t *klass,const wchar_t *label,DWORD style,int id,int x,int y,int width,int height) {
    HWND h=CreateWindowExW(!wcscmp(klass,L"EDIT") ? WS_EX_CLIENTEDGE : 0,klass,label,WS_CHILD|WS_VISIBLE|style,scale(d->window,x),scale(d->window,y),scale(d->window,width),scale(d->window,height),d->window,(HMENU)(INT_PTR)id,GetModuleHandleW(NULL),NULL);
    SendMessageW(h,WM_SETFONT,(WPARAM)d->font,TRUE);return h;
}
static void field(dialog *d,int id,const wchar_t *label,int y,bool readonly) {
    child(d,L"STATIC",label,0,0,20,y+4,150,26);child(d,L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL|(readonly ? ES_READONLY : 0),id,180,y,544,30);
}
static void error(dialog *d,const char *value) { wchar_t *wide=sb_desktop_wide(value);MessageBoxW(d->window,wide ? wide : L"操作未完成。",L"ShellBot 项目",MB_OK|MB_ICONERROR);free(wide); }
static bool checked(HWND parent,int id) {return SendMessageW(GetDlgItem(parent,id),BM_GETCHECK,0,0)==BST_CHECKED;}
static bool read_draft(dialog *d) {
    char *name=text(GetDlgItem(d->window,D_NAME)),*id=text(GetDlgItem(d->window,D_ID)),*directory=text(GetDlgItem(d->window,D_DIRECTORY)),*workspace=text(GetDlgItem(d->window,D_WORKSPACE));
    bool ok=name && *name && strlen(name)<256 && sb_identifier(id) && directory && sb_project_path_valid(directory) && directory[1]==':' && workspace && sb_project_path_valid(workspace);
    if (!ok) { free(name);free(id);free(directory);free(workspace);error(d,"请填写项目名称、1～63 个英数字标识（允许 _ 和 -），以及有效的本地存储目录和工作目录。路径和项目名称支持中文。");return false; }
    yyjson_doc *input=yyjson_read(d->draft.definition,strlen(d->draft.definition),0);yyjson_mut_doc *doc=input ? yyjson_doc_mut_copy(input,NULL) : NULL;yyjson_doc_free(input);
    if (!doc) { free(name);free(id);free(directory);free(workspace);return false; }
    yyjson_mut_val *root=yyjson_mut_doc_get_root(doc),*array=yyjson_mut_obj_get(root,"workspaces"),*work=yyjson_mut_arr_get(array,0);
    char *data=sb_desktop_join(directory,"State");if (data) {yyjson_mut_obj_remove_key(root,"data_dir");yyjson_mut_obj_add_strcpy(doc,root,"data_dir",data);free(data);}
    if (!array) { array=yyjson_mut_arr(doc);yyjson_mut_obj_add_val(doc,root,"workspaces",array); }
    if (!work) { work=yyjson_mut_obj(doc);yyjson_mut_obj_add_str(doc,work,"id","workspace");yyjson_mut_arr_add_val(array,work); }
    yyjson_mut_obj_remove_key(work,"path");yyjson_mut_obj_add_strcpy(doc,work,"path",workspace);
    yyjson_mut_obj_remove_key(work,"read_only");yyjson_mut_obj_add_bool(doc,work,"read_only",checked(d->window,D_READONLY));
    yyjson_mut_val *policy=yyjson_mut_obj_get(root,"policy");if (!policy) { policy=yyjson_mut_obj(doc);yyjson_mut_obj_add_val(doc,root,"policy",policy); }
    const char *modes[]={"always","workspace_auto","never"};LRESULT mode=SendMessageW(GetDlgItem(d->window,D_APPROVAL),CB_GETCURSEL,0,0);
    yyjson_mut_obj_remove_key(policy,"approval_mode");yyjson_mut_obj_add_str(doc,policy,"approval_mode",modes[mode>=0 && mode<3 ? mode : 0]);
    yyjson_mut_obj_remove_key(root,"id");yyjson_mut_obj_add_strcpy(doc,root,"id",id);
    char *definition=yyjson_mut_write(doc,0,NULL);yyjson_mut_doc_free(doc);free(workspace);
    if (!definition) { free(name);free(id);free(directory);return false; }
    free(d->draft.name);free(d->draft.directory);free(d->draft.definition);d->draft.name=name;d->draft.directory=directory;d->draft.definition=definition;
    strcpy(d->draft.id,id);free(id);d->draft.automatic=checked(d->window,D_AUTOMATIC);return true;
}
static void workspace_from_definition(dialog *d) {
    yyjson_doc *doc=yyjson_read(d->draft.definition,strlen(d->draft.definition),0);yyjson_val *root=yyjson_doc_get_root(doc),*work=yyjson_arr_get(yyjson_obj_get(root,"workspaces"),0);
    const char *path=sb_string(yyjson_obj_get(work,"path"));put(d->window,D_WORKSPACE,path ? path : "");
    yyjson_val *readonly=yyjson_obj_get(work,"read_only");SendMessageW(GetDlgItem(d->window,D_READONLY),BM_SETCHECK,!readonly || yyjson_get_bool(readonly) ? BST_CHECKED : BST_UNCHECKED,0);
    const char *mode=sb_string(yyjson_obj_get(yyjson_obj_get(root,"policy"),"approval_mode"));SendMessageW(GetDlgItem(d->window,D_APPROVAL),CB_SETCURSEL,mode && !strcmp(mode,"never") ? 2 : mode && !strcmp(mode,"workspace_auto") ? 1 : 0,0);
    yyjson_doc_free(doc);
}
static void synchronize_directory(dialog *d) {
    char *directory=text(GetDlgItem(d->window,D_DIRECTORY)),*workspace=text(GetDlgItem(d->window,D_WORKSPACE));
    if (directory && workspace && d->last_directory && !strcmp(workspace,d->last_directory)) {
        char *next=sb_desktop_join(directory,"Workspace");if (next) put(d->window,D_WORKSPACE,next);free(next);
    }
    free(d->last_directory);d->last_directory=directory ? sb_desktop_join(directory,"Workspace") : NULL;free(directory);free(workspace);
}
static DWORD WINAPI save_thread(void *context) {
    dialog *d=context;
    d->saved=d->storage ? sb_desktop_relocate(d->app,d->draft.directory,&d->error) : d->connection ? sb_desktop_connection(d->app,d->port,d->origin,&d->error) : sb_desktop_save(d->app,d->original_id,&d->draft,&d->error);
    PostMessageW(d->window,D_COMPLETE,0,0);return 0;
}
static void enable_fields(dialog *d,bool enabled){
    for(int id=D_NAME;id<=D_OWNER;++id){HWND h=GetDlgItem(d->window,id);if(h && id!=D_STATUS)EnableWindow(h,enabled);}
    if(d->original_id)SendMessageW(GetDlgItem(d->window,D_ID),EM_SETREADONLY,TRUE,0);
}
static void begin_save(dialog *d) {
    if (d->busy) return;
    if(d->storage_complete){d->accepted=true;sb_desktop_request_exit(d->app);DestroyWindow(d->window);return;}
    if(d->storage){
        char *path=text(GetDlgItem(d->window,D_DIRECTORY));if(!path)return;
        if(!sb_platform_storage_path(path,&d->error)){error(d,d->error.text);free(path);return;}
        free(d->draft.directory);d->draft.directory=path;
        if(!d->app){if(!sb_desktop_make_directory(path,false,&d->error)){error(d,d->error.text);return;}d->accepted=true;DestroyWindow(d->window);return;}
        if(MessageBoxW(d->window,L"迁移将停止全部项目，复制并逐文件校验应用数据。成功后请重新打开程序；旧目录保留，请在退出后手动删除。继续？",L"迁移应用数据",MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2)!=IDYES)return;
    }else if (d->connection) {
        char *port=text(GetDlgItem(d->window,D_PORT)),*end=NULL;unsigned long n=port ? strtoul(port,&end,10) : 0;
        bool valid=port && *port && strspn(port,"0123456789")==strlen(port) && end && !*end && n>=1 && n<=65535;free(port);
        if (!valid) { error(d,"本机端口须在 1～65535 之间。");return; }
        d->port=(unsigned)n;free(d->origin);d->origin=text(GetDlgItem(d->window,D_ORIGIN));if (!d->origin) return;
        if (MessageBoxW(d->window,L"应用连接设置会重新打开 HTTP 网关，并停止当前活动任务。项目将按“随管理界面启动”设置重新启动。继续？",L"应用连接设置",MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2)!=IDYES) return;
    } else if (!read_draft(d)) return;
    d->busy=true;memset(&d->error,0,sizeof(d->error));
    enable_fields(d,false);
    SetWindowTextW(GetDlgItem(d->window,D_STATUS),L"正在保存配置和校验项目数据，请稍候；原目录保留用于回退。");
    d->thread=CreateThread(NULL,0,save_thread,d,0,NULL);
    if (!d->thread) { d->busy=false;enable_fields(d,true);SetWindowTextW(GetDlgItem(d->window,D_STATUS),L"保存未开始，请重试。");error(d,"无法创建保存线程。"); }
}
static LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wp,LPARAM lp) {
    dialog *d=(dialog *)GetWindowLongPtrW(window,GWLP_USERDATA);
    if (message==WM_NCCREATE) { d=((CREATESTRUCTW *)lp)->lpCreateParams;d->window=window;SetWindowLongPtrW(window,GWLP_USERDATA,(LONG_PTR)d); }
    if (!d) return DefWindowProcW(window,message,wp,lp);
    switch (message) {
    case DM_GETDEFID:return MAKELRESULT(D_SAVE,DC_HASDEFID);
    case WM_CREATE:
        d->font=sb_native_font(window);d->dpi=sb_native_dpi(window);
        if(d->storage){
            field(d,D_OWNER,L"当前数据目录",24,true);put(window,D_OWNER,d->draft.directory);
            field(d,D_DIRECTORY,L"新的数据目录",76,false);put(window,D_DIRECTORY,d->draft.directory);
            child(d,L"BUTTON",L"选择目录…",WS_TABSTOP,D_BROWSE,554,118,170,30);
            child(d,L"STATIC",L"数据位置必须是非 C 盘本地目录，不会使用用户目录或系统临时目录。\r\n迁移先复制并逐文件校验，成功后需要退出并重新打开程序。\r\n旧目录保留供退出后手动删除；另行指定的项目目录在项目设置中迁移。",0,0,20,164,704,80);
        }else if (d->connection) {
            field(d,D_PORT,L"本机监听端口",24,false);char number[16];snprintf(number,sizeof(number),"%u",sb_desktop_port(d->app));put(window,D_PORT,number);
            field(d,D_ORIGIN,L"公共地址（可选）",76,false);put(window,D_ORIGIN,sb_desktop_origin(d->app));
            child(d,L"STATIC",L"服务只监听 127.0.0.1，端口填写已有隧道的本机目标端口。\r\n填入 HTTPS 公网地址会自动启用 OAuth；在客户端选择 OAuth。\r\n连接申请在管理界面的“客户端授权”中确认，无需填写 token。",0,0,20,138,704,72);
        } else {
            field(d,D_NAME,L"项目名称",20,false);put(window,D_NAME,d->draft.name);
            field(d,D_ID,L"地址标识（英数字）",68,d->original_id!=NULL);put(window,D_ID,d->draft.id);
            field(d,D_DIRECTORY,L"项目存储目录",116,false);put(window,D_DIRECTORY,d->draft.directory);
            child(d,L"BUTTON",L"选择存储目录…",WS_TABSTOP,D_BROWSE,554,158,170,30);
            field(d,D_WORKSPACE,L"工作目录",206,false);
            child(d,L"BUTTON",L"随管理界面启动此项目",WS_TABSTOP|BS_AUTOCHECKBOX,D_AUTOMATIC,180,254,300,28);
            SendMessageW(GetDlgItem(window,D_AUTOMATIC),BM_SETCHECK,d->draft.automatic ? BST_CHECKED : BST_UNCHECKED,0);
            child(d,L"BUTTON",L"工作目录只读",WS_TABSTOP|BS_AUTOCHECKBOX,D_READONLY,490,254,234,28);
            child(d,L"STATIC",L"审批方式",0,0,20,302,150,26);HWND combo=child(d,L"COMBOBOX",L"",WS_TABSTOP|CBS_DROPDOWNLIST,D_APPROVAL,180,298,544,180);
            SendMessageW(combo,CB_ADDSTRING,0,(LPARAM)L"每次审批");SendMessageW(combo,CB_ADDSTRING,0,(LPARAM)L"工作目录自动，命令需审批");SendMessageW(combo,CB_ADDSTRING,0,(LPARAM)L"授权范围内无需逐次审批");
            child(d,L"BUTTON",L"权限、目录与工具…",WS_TABSTOP,D_ADVANCED,180,350,240,32);
            child(d,L"STATIC",L"存储目录保存配置、加密凭据和 State，工作目录保存 MCP 操作的文件。\r\n修改存储目录会先复制并逐文件校验。\r\n完成后请退出并重新打开程序，确认新目录正常后手动删除旧目录。",0,0,20,400,704,76);
            workspace_from_definition(d);d->last_directory=sb_desktop_join(d->draft.directory,"Workspace");
        }
        {int y=d->storage ? 256 : d->connection ? 216 : 494;child(d,L"BUTTON",d->storage ? (d->app?L"复制并切换数据位置":L"使用此数据目录") : d->connection ? L"应用连接设置" : d->original_id ? L"保存项目" : L"创建项目",WS_TABSTOP|BS_DEFPUSHBUTTON,D_SAVE,444,y,160,34);child(d,L"BUTTON",L"取消",WS_TABSTOP,D_CANCEL,620,y,104,34);child(d,L"STATIC",L"",0,D_STATUS,20,y+48,704,d->storage?90:58);if(d->notice)put(window,D_STATUS,d->notice);}SetTimer(window,1,100,NULL);return 0;
    case WM_COMMAND:
        if(LOWORD(wp)==IDOK){begin_save(d);return 0;}
        if(LOWORD(wp)==IDCANCEL){SendMessageW(window,WM_CLOSE,0,0);return 0;}
        if (LOWORD(wp)==D_DIRECTORY && HIWORD(wp)==EN_KILLFOCUS && !d->connection && !d->storage) synchronize_directory(d);
        if (LOWORD(wp)==D_SAVE) { begin_save(d);return 0; }
        if (LOWORD(wp)==D_CANCEL && !d->busy) { DestroyWindow(window);return 0; }
        if (LOWORD(wp)==D_BROWSE && !d->busy) {
            if(sb_choose_directory(window,GetDlgItem(window,D_DIRECTORY),d->storage ? L"选择 ShellBot 数据目录" : L"选择此项目独立的存储目录（新项目使用空目录）") && !d->storage)synchronize_directory(d);
            return 0;
        }
        if (LOWORD(wp)==D_ADVANCED && !d->busy && read_draft(d)) {
            yyjson_doc *project=yyjson_read(d->draft.definition,strlen(d->draft.definition),0);yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL);
            yyjson_mut_val *root=yyjson_mut_obj(doc),*array=yyjson_mut_arr(doc);yyjson_mut_doc_set_root(doc,root);yyjson_mut_obj_add_uint(doc,root,"format_version",1);
            yyjson_mut_arr_add_val(array,yyjson_val_mut_copy(doc,yyjson_doc_get_root(project)));yyjson_mut_obj_add_val(doc,root,"projects",array);
            char *snapshot=yyjson_mut_write(doc,0,NULL),*edited=snapshot ? sb_project_settings_edit(window,snapshot) : NULL;free(snapshot);yyjson_mut_doc_free(doc);yyjson_doc_free(project);
            yyjson_doc *result=edited ? yyjson_read(edited,strlen(edited),0) : NULL;yyjson_val *p=yyjson_arr_get(yyjson_obj_get(yyjson_doc_get_root(result),"projects"),0);
            char *definition=p ? yyjson_val_write(p,0,NULL) : NULL;if (definition) {free(d->draft.definition);d->draft.definition=definition;workspace_from_definition(d);}yyjson_doc_free(result);free(edited);return 0;
        }
        return 0;
    case D_COMPLETE:
        WaitForSingleObject(d->thread,INFINITE);CloseHandle(d->thread);d->thread=NULL;d->busy=false;
        if(d->saved && d->storage){
            d->storage_complete=true;SetWindowTextW(GetDlgItem(window,D_STATUS),L"复制与 SHA-256 校验完成，数据位置已保存。\r\n请完成并退出，然后重新打开 ShellBot 使用新位置。\r\n确认新位置正常后，可手动删除上方“当前数据目录”中的旧文件。");
            SetWindowTextW(GetDlgItem(window,D_SAVE),L"完成并退出");EnableWindow(GetDlgItem(window,D_SAVE),TRUE);ShowWindow(GetDlgItem(window,D_CANCEL),SW_HIDE);return 0;
        }
        if (d->saved || WaitForSingleObject(sb_desktop_stop(d->app),0)==WAIT_OBJECT_0) { d->accepted=d->saved;DestroyWindow(window); }
        else {enable_fields(d,true);SetWindowTextW(GetDlgItem(window,D_STATUS),L"保存未完成，原项目及目录保留。请按错误提示修改后重试。");error(d,d->error.text); }return 0;
    case WM_CLOSE:if (!d->busy) {if(d->storage_complete){d->accepted=true;sb_desktop_request_exit(d->app);}DestroyWindow(window);}return 0;
    case WM_TIMER:if(d->app && WaitForSingleObject(sb_desktop_stop(d->app),0)==WAIT_OBJECT_0){if(!d->busy)DestroyWindow(window);else {HWND status=GetDlgItem(window,D_STATUS);wchar_t text[100];GetWindowTextW(status,text,100);if(wcscmp(text,L"正在取消迁移并停止项目；原目录保持完整。"))SetWindowTextW(status,L"正在取消迁移并停止项目；原目录保持完整。");}}return 0;
    case WM_DPICHANGED:sb_native_dpi_changed(window,&d->font,&d->dpi,wp,lp);return 0;
    case WM_DESTROY:KillTimer(window,1);d->done=true;return 0;
    default:return DefWindowProcW(window,message,wp,lp);
    }
}
static bool show(dialog *d) {
    WNDCLASSW wc={0};wc.lpfnWndProc=procedure;wc.hInstance=GetModuleHandleW(NULL);wc.lpszClassName=L"ShellBotDesktopProjectDialog";wc.hIcon=sb_app_icon();wc.hCursor=LoadCursorW(NULL,IDC_ARROW);wc.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1);RegisterClassW(&wc);
    RECT r={0,0,scale(d->owner,748),scale(d->owner,d->storage ? 412 : d->connection ? 338 : 616)};AdjustWindowRectExForDpi(&r,WS_CAPTION|WS_SYSMENU,FALSE,WS_EX_DLGMODALFRAME,sb_native_dpi(d->owner));
    RECT owner={0};GetWindowRect(d->owner,&owner);int x=d->owner?owner.left:CW_USEDEFAULT,y=d->owner?owner.top:CW_USEDEFAULT;
    HWND h=CreateWindowExW(WS_EX_DLGMODALFRAME,wc.lpszClassName,d->storage ? L"ShellBot 数据位置" : d->connection ? L"ShellBot 连接设置" : d->original_id ? L"项目设置" : L"新建项目",WS_CAPTION|WS_SYSMENU|WS_CLIPCHILDREN,x,y,r.right-r.left,r.bottom-r.top,d->owner,NULL,wc.hInstance,d);
    if (!h) return false;EnableWindow(d->owner,FALSE);ShowWindow(h,SW_SHOW);SetFocus(GetDlgItem(h,d->storage ? D_DIRECTORY : d->connection ? D_PORT : D_NAME));
    sb_native_modal_loop(h,&d->done);
    if (d->thread) {if (WaitForSingleObject(d->thread,30000)!=WAIT_OBJECT_0)ExitProcess(3);CloseHandle(d->thread);d->thread=NULL;}
    EnableWindow(d->owner,TRUE);SetActiveWindow(d->owner);DeleteObject(d->font);free(d->last_directory);free(d->origin);free(d->notice);return d->accepted;
}
bool sb_desktop_project_dialog(HWND owner,sb_desktop_app *app,const char *id) {
    dialog d={0};d.owner=owner;d.app=app;d.original_id=id ? sb_strdup(id) : NULL;
    bool loaded=sb_desktop_draft_load(app,id,&d.draft,&d.error),saved=loaded && show(&d);
    if (!loaded) error(&d,*d.error.text ? d.error.text : "无法读取所选项目。");sb_desktop_draft_free(&d.draft);free(d.original_id);return saved;
}
bool sb_desktop_connection_dialog(HWND owner,sb_desktop_app *app) {dialog d={0};d.owner=owner;d.app=app;d.connection=true;return show(&d);}

bool sb_desktop_storage_dialog(HWND owner,sb_desktop_app *app){dialog d={0};d.owner=owner;d.app=app;d.storage=true;d.draft.directory=sb_strdup(sb_desktop_storage(app));bool saved=d.draft.directory && show(&d);sb_desktop_draft_free(&d.draft);return saved;}
char *sb_desktop_choose_storage(const char *current,const char *notice){
    dialog d={0};d.storage=true;d.draft.directory=sb_strdup(current);d.notice=notice?sb_strdup(notice):NULL;
    bool saved=d.draft.directory && show(&d);char *out=saved?d.draft.directory:NULL;if(saved)d.draft.directory=NULL;sb_desktop_draft_free(&d.draft);return out;
}
