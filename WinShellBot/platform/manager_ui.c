#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include "sb_projects.h"
#include "manager_ui.h"
#include "execution.h"
#include "startup.h"
#include "project_settings.h"
#include "desktop_app.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <stdarg.h>

enum { UI_REFRESH=1001, UI_START, UI_STOP, UI_PAUSE, UI_RESUME, UI_LOCAL, UI_PUBLIC,
       UI_DIAGNOSE, UI_COPY, UI_ADMIN_START, UI_ADMIN_STOP, UI_SETTINGS, UI_NEW,UI_CONNECTION,UI_TOKEN,UI_OWNER_TOKEN,UI_OAUTH,UI_STORAGE, UI_LIST=1100, UI_DETAILS, UI_STATUS, UI_HEADER, UI_PATH, UI_CONTEXT, UI_PROJECT_LABEL, UI_DETAIL_LABEL, UI_ABOUT, UI_DIVIDER, UI_PATH_LABEL };
#define UI_VIEW_PROJECTS 1301
#define UI_VIEW_CLASSIC 1302
#define UI_RESULT (WM_APP+1)
#define UI_TRAY (WM_APP+2)
#define UI_SHOW 1201
#define UI_HIDE 1202
#define UI_EXIT 1203
#define UI_LOGON_ENABLE 1204
#define UI_LOGON_DISABLE 1205
#define UI_ADMIN_LOGON_ENABLE 1206
#define UI_ADMIN_LOGON_DISABLE 1207
#define UI_TEXT_CAP 65536u
#define UI_SELECTION (WM_APP+3)
typedef struct {
    HWND window;
    char *data, *request, *reply;
    bool snapshot, diagnostic, logon, enable, admin_logon, settings, background;
    sb_error error;
    sb_desktop_app *app;
    char project[64];
    unsigned selection_generation;
} ui_work;
typedef struct {
    HWND window, list, details, status, header, path, context, project_label, detail_label, divider, path_label, buttons[18];
    HFONT font, heading, section;
    HMENU view_menu;
    bool compact;
    int tab_layout;
    HANDLE thread;
    char *data;
    yyjson_doc *snapshot;
    char selected[64];
    char details_project[64];
    bool busy, background, rendering, diagnostic;
    char row_ids[SB_MAX_PROJECTS][64];size_t row_count;
    bool resident, tray_added;
    HANDLE stop, manager_thread;
    const char *profile_path;
    UINT taskbar_created;
    sb_desktop_app *app;
    bool modal;
    int pending_navigation;
    char pending_project[64];
    bool exit_pending;
    unsigned oauth_pending;
    unsigned selection_generation;
    bool stale,notice_visible;
    ui_work *work;
} ui_state;

static void logon_request(ui_state *ui,bool enable,bool administrator);
static void layout(ui_state *ui);
static bool project_action(int id){return (id>=UI_START&&id<=UI_COPY)||id==UI_SETTINGS||id==UI_TOKEN;}

static bool tray(ui_state *ui,DWORD action) {
    NOTIFYICONDATAW icon={0};icon.cbSize=sizeof(icon);icon.hWnd=ui->window;icon.uID=1;
    icon.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP;icon.uCallbackMessage=UI_TRAY;
    icon.hIcon=LoadIconW(NULL,IDI_APPLICATION);wcscpy(icon.szTip,L"ShellBot · 后台运行；单击打开管理");
    bool ok=Shell_NotifyIconW(action,&icon)!=FALSE;
    if(action==NIM_ADD && !ok)ok=Shell_NotifyIconW(NIM_MODIFY,&icon)!=FALSE;
    if(action==NIM_ADD)ui->tray_added=ok;else if(action==NIM_DELETE)ui->tray_added=false;return ok;
}
static void show_resident(ui_state *ui) {
    ShowWindow(ui->window,SW_RESTORE);SetForegroundWindow(ui->window);
    PostMessageW(ui->window,WM_COMMAND,1001,0);
}
static void resident_command(ui_state *ui,int id) {
    if(id==UI_SHOW)show_resident(ui);
    else if(id==UI_HIDE){if(ui->tray_added)ShowWindow(ui->window,SW_HIDE);}
    else if(id==UI_EXIT){if(ui->app){ui->exit_pending=true;sb_desktop_request_exit(ui->app);if(ui->modal){SetWindowTextW(ui->status,L"正在停止全部项目并结束保存操作…");return;}}SetEvent(ui->stop);DestroyWindow(ui->window);}
    else if(id==UI_LOGON_ENABLE || id==UI_LOGON_DISABLE) {
        logon_request(ui,id==UI_LOGON_ENABLE,false);
    } else if(id==UI_ADMIN_LOGON_ENABLE || id==UI_ADMIN_LOGON_DISABLE) {
        logon_request(ui,id==UI_ADMIN_LOGON_ENABLE,true);
    }
}
static void tray_menu(ui_state *ui) {
    HMENU menu=CreatePopupMenu();if(!menu)return;
    AppendMenuW(menu,MF_STRING,UI_SHOW,L"打开项目管理");AppendMenuW(menu,MF_STRING,UI_HIDE,L"隐藏管理窗口");
    if(!ui->app){AppendMenuW(menu,MF_SEPARATOR,0,NULL);AppendMenuW(menu,MF_STRING,UI_LOGON_ENABLE,L"登录时自动启动");
    AppendMenuW(menu,MF_STRING,UI_LOGON_DISABLE,L"关闭登录自动启动");AppendMenuW(menu,MF_SEPARATOR,0,NULL);
    AppendMenuW(menu,MF_STRING|(sb_win_elevated_owner()?0:MF_GRAYED),UI_ADMIN_LOGON_ENABLE,L"登录时以管理员身份自动启动");
    AppendMenuW(menu,MF_STRING|(sb_win_elevated_owner()?0:MF_GRAYED),UI_ADMIN_LOGON_DISABLE,L"关闭管理员登录自动启动");}AppendMenuW(menu,MF_SEPARATOR,0,NULL);
    AppendMenuW(menu,MF_STRING,UI_EXIT,ui->app?L"退出 ShellBot（停止全部项目）":L"退出 ShellBot（停止本实例）");
    POINT cursor;GetCursorPos(&cursor);SetForegroundWindow(ui->window);
    int id=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_NONOTIFY|TPM_RIGHTBUTTON,cursor.x,cursor.y,0,ui->window,NULL);
    DestroyMenu(menu);PostMessageW(ui->window,WM_NULL,0,0);if(id)resident_command(ui,id);
}

static wchar_t *to_wide(const char *text) {
    int size=text ? MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text,-1,NULL,0) : 0;
    wchar_t *value=size ? calloc((size_t)size,sizeof(wchar_t)) : NULL;
    if(value && !MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text,-1,value,size)) { free(value); value=NULL; }
    return value;
}
static void set_text(HWND window,const wchar_t *text) {
    int length=GetWindowTextLengthW(window);size_t size=wcslen(text);
    if((size_t)length==size){wchar_t *old=calloc(size+1,sizeof(*old));if(old){GetWindowTextW(window,old,length+1);bool same=!wcscmp(old,text);free(old);if(same)return;}}
    SetWindowTextW(window,text);
}
static void set_utf8(HWND window, const char *text) {
    wchar_t *value=to_wide(text); set_text(window,value ? value : L"内容不可用"); free(value);
}
static const char *str(yyjson_val *value, const char *key) {
    const char *text=sb_string(yyjson_obj_get(value,key)); return text ? text : "";
}
static const wchar_t *state_label(const char *state) {
    return !strcmp(state,"running") ? L"运行中" : !strcmp(state,"paused") ? L"已暂停" :
        !strcmp(state,"stopped") ? L"已停止" : !strcmp(state,"busy") ? L"处理中" :
        !strcmp(state,"cleanup_required") ? L"需要恢复清理" : L"运行失败";
}
static const wchar_t *approval_label(const char *mode) {
    return !strcmp(mode,"always") ? L"每次审批" : !strcmp(mode,"workspace_auto") ? L"工作目录自动" :
        !strcmp(mode,"never") ? L"无需逐次审批" : !strcmp(mode,"legacy") ? L"旧配置模式" : L"等待刷新";
}
static const wchar_t *identity_label(const char *identity) {
    return !strcmp(identity,"restricted") ? L"受限权限" : !strcmp(identity,"current_user") ? L"当前账户" :
        !strcmp(identity,"administrator") ? L"管理员" : L"等待刷新";
}
static yyjson_val *selected_project(ui_state *ui) {
    yyjson_val *projects=yyjson_obj_get(yyjson_doc_get_root(ui->snapshot),"projects"), *item; size_t i,max;
    yyjson_arr_foreach(projects,i,max,item)if(!strcmp(str(item,"project_id"),ui->selected))return item;
    return NULL;
}
static void project_heading(ui_state *ui){
    if(!ui->compact){set_text(ui->detail_label,L"项目详情");set_text(ui->project_label,L"项目列表");return;}
    wchar_t label[300],list_label[64];yyjson_val *p=selected_project(ui);
    const char *name=*str(p,"display_name")?str(p,"display_name"):str(p,"project_id");wchar_t *wide=to_wide(name);
    if(p)swprintf(label,300,L"%ls  ·  %ls",wide?wide:L"",state_label(str(p,"state")));
    else wcscpy(label,L"项目详情");free(wide);
    swprintf(list_label,64,L"项目  (%zu)",ui->row_count);
    set_text(ui->detail_label,label);set_text(ui->project_label,list_label);
}
static void append(wchar_t *text, size_t *length, const wchar_t *format, ...) {
    if(*length>=UI_TEXT_CAP-128)return;
    va_list args; va_start(args,format);
    int count=vswprintf(text+*length,UI_TEXT_CAP-*length,format,args); va_end(args);
    if(count<0) { text[*length]=0; wcscat(text,L"\r\n显示内容过长；请查看诊断或原配置。\r\n"); *length=UI_TEXT_CAP; }
    else *length+=(size_t)count;
}
static void update_buttons(ui_state *ui) {
    yyjson_val *p=selected_project(ui); const char *state=str(p,"state");
    bool busy=ui->modal || (ui->busy && !ui->background);
    bool available=p && !ui->stale && strcmp(state,"busy") && !busy;
    bool enabled=yyjson_get_bool(yyjson_obj_get(p,"enabled"));
    bool paused=yyjson_get_bool(yyjson_obj_get(p,"paused"));
    bool running=!strcmp(state,"running");
    bool states[18];for(int i=0;i<18;++i)states[i]=!busy;
    if(ui->app){states[UI_SETTINGS-UI_REFRESH]=available;states[UI_TOKEN-UI_REFRESH]=available;}
    states[UI_START-UI_REFRESH]=available && enabled && !paused && !running;
    states[UI_STOP-UI_REFRESH]=available && (running || paused);
    states[UI_PAUSE-UI_REFRESH]=available && enabled && !paused;
    states[UI_RESUME-UI_REFRESH]=available && enabled && paused;
    yyjson_val *links=yyjson_obj_get(p,"links");
    states[UI_LOCAL-UI_REFRESH]=available && *str(links,"local_mcp_url");
    states[UI_PUBLIC-UI_REFRESH]=available && *str(links,"public_mcp_url");
    states[UI_DIAGNOSE-UI_REFRESH]=available;
    states[UI_COPY-UI_REFRESH]=!busy && ui->diagnostic;
    yyjson_val *root=yyjson_doc_get_root(ui->snapshot);
    bool alive=yyjson_get_bool(yyjson_obj_get(root,"administrator_alive"));
    bool starting=yyjson_get_bool(yyjson_obj_get(root,"administrator_starting"));
    states[UI_ADMIN_START-UI_REFRESH]=root && !ui->stale && !busy && !alive && !starting;
    states[UI_ADMIN_STOP-UI_REFRESH]=!ui->stale && !busy && (alive || starting);
    HMENU menu=GetMenu(ui->window);
    for(int i=0;i<18;++i){
        if(ui->buttons[i] && (IsWindowEnabled(ui->buttons[i])!=FALSE)!=states[i])EnableWindow(ui->buttons[i],states[i]);
        if(menu)EnableMenuItem(menu,UI_REFRESH+i,MF_BYCOMMAND|(states[i]?MF_ENABLED:MF_GRAYED));
    }
}
static void set_project_details(ui_state *ui,const wchar_t *text,bool preserve) {
    int old_length=GetWindowTextLengthW(ui->details);
    wchar_t *old=calloc((size_t)old_length+1,sizeof(wchar_t));
    if(old) {
        GetWindowTextW(ui->details,old,old_length+1);
        bool unchanged=!wcscmp(old,text);free(old);
        if(preserve && unchanged)return;
    }
    DWORD start=0,end=0;
    LRESULT first=0;
    if(preserve) {
        SendMessageW(ui->details,EM_GETSEL,(WPARAM)&start,(LPARAM)&end);
        first=SendMessageW(ui->details,EM_GETFIRSTVISIBLELINE,0,0);
    }
    bool visible=IsWindowVisible(ui->details)!=FALSE;
    if(visible)SendMessageW(ui->details,WM_SETREDRAW,FALSE,0);
    SetWindowTextW(ui->details,text);
    if(preserve) {
        DWORD size=(DWORD)wcslen(text);
        SendMessageW(ui->details,EM_SETSEL,start>size?size:start,end>size?size:end);
        LRESULT current=SendMessageW(ui->details,EM_GETFIRSTVISIBLELINE,0,0);
        SendMessageW(ui->details,EM_LINESCROLL,0,first-current);
    }
    if(visible)SendMessageW(ui->details,WM_SETREDRAW,TRUE,0);
    RedrawWindow(ui->details,NULL,NULL,RDW_INVALIDATE|RDW_FRAME);
}
static void show_project(ui_state *ui,bool background) {
    yyjson_val *p=selected_project(ui);
    project_heading(ui);
    bool same=!strcmp(ui->selected,ui->details_project);
    if(background && same && ui->diagnostic){update_buttons(ui);return;}
    bool preserve=same && !ui->diagnostic;
    ui->diagnostic=false;
    snprintf(ui->details_project,sizeof(ui->details_project),"%s",ui->selected);
    if(!p) { set_project_details(ui,L"选择一个项目，查看地址、权限和运行状态。",preserve); update_buttons(ui); return; }
    wchar_t *text=calloc(UI_TEXT_CAP,sizeof(wchar_t)); size_t length=0;
    wchar_t *id=to_wide(str(p,"project_id"));
    if(!text) { free(id); return; }
    append(text,&length,L"%ls  ·  %ls\r\n\r\n",id ? id : L"",state_label(str(p,"state"))); free(id);
    if(ui->app){wchar_t *name=to_wide(str(p,"display_name")),*directory=to_wide(str(p,"project_directory"));append(text,&length,L"项目名称：%ls\r\n项目存储目录：%ls\r\n随管理界面启动：%ls\r\n关闭窗口收进托盘；从托盘退出后全部项目停止。\r\n\r\n",name?name:L"",directory?directory:L"",yyjson_get_bool(yyjson_obj_get(p,"automatic_start"))?L"是":L"否");free(name);free(directory);}
    if(!strcmp(str(p,"state"),"busy"))append(text,&length,L"该项目正在处理请求，请稍后刷新。\r\n");
    else {
        append(text,&length,L"审批方式：%ls\r\n文件范围：%ls\r\n默认任务身份：%ls\r\n任务执行：%ls\r\n管理员许可：%ls\r\n管理员代理：%ls\r\n策略版本：%llu\r\n\r\n",
            approval_label(str(p,"approval_mode")),!strcmp(str(p,"file_scope"),"current_user") ?
                (yyjson_get_bool(yyjson_obj_get(p,"current_user_read_only")) ? L"当前账户可读范围（只读）" : L"当前账户可访问范围") : L"已配置的目录",
            identity_label(str(p,"default_identity")),yyjson_get_bool(yyjson_obj_get(p,"execution_enabled")) ? L"已启用" : L"已关闭",
            yyjson_get_bool(yyjson_obj_get(p,"administrator_allowed")) ? L"已允许" : L"未允许",
            yyjson_get_bool(yyjson_obj_get(p,"administrator_backend")) ? L"可用" : L"未就绪",
            (unsigned long long)yyjson_get_uint(yyjson_obj_get(p,"policy_revision")));
        append(text,&length,L"桌面观察：%ls\r\n桌面输入：%ls\r\n剪贴板读取：%ls\r\n剪贴板写入：%ls\r\n\r\n",
            yyjson_get_bool(yyjson_obj_get(p,"desktop_enabled")) ? L"已允许（整个当前会话）" : L"已关闭",
            yyjson_get_bool(yyjson_obj_get(p,"desktop_control")) ? L"已允许" : L"已关闭",
            yyjson_get_bool(yyjson_obj_get(p,"clipboard_read")) ? L"已允许" : L"已关闭",
            yyjson_get_bool(yyjson_obj_get(p,"clipboard_write")) ? L"已允许" : L"已关闭");
        yyjson_val *links=yyjson_obj_get(p,"links"); wchar_t *local=to_wide(str(links,"local_mcp_url")), *public_url=to_wide(str(links,"public_mcp_url"));
        append(text,&length,L"本机 MCP 地址\r\n%ls\r\n\r\n公网 MCP 地址\r\n%ls\r\n网关：%ls\r\n\r\n",
            local ? local : L"",public_url && *public_url ? public_url : L"尚未填写公网地址",
            yyjson_get_bool(yyjson_obj_get(links,"gateway_running")) ? L"已启动" : L"未启动"); free(local); free(public_url);
        yyjson_val *directories=yyjson_obj_get(p,"directories"), *directory; size_t i,max;
        yyjson_arr_foreach(directories,i,max,directory) {
            wchar_t *name=to_wide(str(directory,"workspace_id")), *path=to_wide(str(directory,"path"));
            append(text,&length,L"%ls · %ls · %ls\r\n%ls\r\n\r\n",name ? name : L"",
                yyjson_get_bool(yyjson_obj_get(directory,"workspace")) ? L"工作目录" : L"额外目录",
                yyjson_get_bool(yyjson_obj_get(directory,"read_only")) ? L"只读" : L"可读写",path ? path : L""); free(name); free(path);
        }
        append(text,&length,L"暂停会结束本项目活动任务；恢复会创建新实例，旧批准和任务不会重跑。\r\n受限身份须在启动后核验，实际结果可查看诊断。\r\n");
    }
    set_project_details(ui,text,preserve); free(text); update_buttons(ui);
}
static void set_cell(HWND list,int row,int column,const wchar_t *text) {
    wchar_t previous[256];ListView_GetItemText(list,row,column,previous,256);
    if(wcscmp(previous,text))ListView_SetItemText(list,row,column,(wchar_t *)text);
}
static bool valid_snapshot(yyjson_val *root){
    yyjson_val *projects=yyjson_obj_get(root,"projects");size_t count=yyjson_arr_size(projects);
    if(!yyjson_is_obj(root)||!yyjson_is_arr(projects)||count>SB_MAX_PROJECTS)return false;
    for(size_t i=0;i<count;++i){yyjson_val *p=yyjson_arr_get(projects,i);const char *id=str(p,"project_id");
        if(!yyjson_is_obj(p)||!sb_identifier(id))return false;
        for(size_t j=0;j<i;++j)if(!strcmp(id,str(yyjson_arr_get(projects,j),"project_id")))return false;
    }return true;
}
static void render(ui_state *ui,bool background) {
    char reading_project[64];snprintf(reading_project,sizeof(reading_project),"%s",ui->selected);
    yyjson_val *root=yyjson_doc_get_root(ui->snapshot), *projects=yyjson_obj_get(root,"projects"), *p; size_t i,max; int selected=-1;
    size_t count=yyjson_arr_size(projects);bool structure=count!=ui->row_count||count!=(size_t)ListView_GetItemCount(ui->list);
    yyjson_arr_foreach(projects,i,max,p)if(i>=ui->row_count || strcmp(ui->row_ids[i],str(p,"project_id")))structure=true;
    int top=ListView_GetTopIndex(ui->list),horizontal=GetScrollPos(ui->list,SB_HORZ);char top_id[64]={0};
    if(top>=0 && (size_t)top<ui->row_count)snprintf(top_id,sizeof(top_id),"%s",ui->row_ids[top]);
    bool redraw=structure && IsWindowVisible(ui->list);
    ui->rendering=true;if(redraw)SendMessageW(ui->list,WM_SETREDRAW,FALSE,0);
    if(structure)ListView_DeleteAllItems(ui->list);
    yyjson_arr_foreach(projects,i,max,p) {
        wchar_t *id=to_wide(ui->app && *str(p,"display_name") ? str(p,"display_name") : str(p,"project_id"));
        if(structure){LVITEMW row={0};row.mask=LVIF_TEXT;row.iItem=(int)i;row.pszText=id?id:L"";SendMessageW(ui->list,LVM_INSERTITEMW,0,(LPARAM)&row);}
        else set_cell(ui->list,(int)i,0,id?id:L"");free(id);
        set_cell(ui->list,(int)i,1,state_label(str(p,"state")));
        set_cell(ui->list,(int)i,2,approval_label(str(p,"approval_mode")));
        set_cell(ui->list,(int)i,3,identity_label(str(p,"default_identity")));
        if(i<SB_MAX_PROJECTS)snprintf(ui->row_ids[i],sizeof(ui->row_ids[i]),"%s",str(p,"project_id"));
        if(!strcmp(ui->selected,str(p,"project_id")))selected=(int)i;
    }
    ui->row_count=count;
    if(selected<0 && yyjson_arr_size(projects)) { selected=0; snprintf(ui->selected,sizeof(ui->selected),"%s",str(yyjson_arr_get(projects,0),"project_id")); }
    if(selected>=0) { UINT flags=LVIS_SELECTED|LVIS_FOCUSED;if(ListView_GetItemState(ui->list,selected,flags)!=flags)ListView_SetItemState(ui->list,selected,flags,flags); }
    else *ui->selected=0;
    if(strcmp(reading_project,ui->selected))++ui->selection_generation;
    if(structure && background && count){RECT row;if(ListView_GetItemRect(ui->list,0,&row,LVIR_BOUNDS)){int target=top<(int)count?top:(int)count-1;for(size_t j=0;j<count;++j)if(!strcmp(top_id,ui->row_ids[j])){target=(int)j;break;}ListView_Scroll(ui->list,horizontal-GetScrollPos(ui->list,SB_HORZ),(target-ListView_GetTopIndex(ui->list))*(row.bottom-row.top));}}
    wchar_t context[256];
    swprintf(context,256,L"%zu 个项目  ·  网关%ls  ·  管理员代理%ls  ·  %ls",yyjson_arr_size(projects),
        yyjson_get_bool(yyjson_obj_get(root,"http_gateway")) ? L"已启动" : L"未启动",
        yyjson_get_bool(yyjson_obj_get(root,"administrator_alive")) ? L"已启动" : yyjson_get_bool(yyjson_obj_get(root,"administrator_starting")) ? L"等待授权" : L"未启动",
        yyjson_get_bool(yyjson_obj_get(root,"remote_owner_enabled")) ? L"远程审批已启用" : L"远程审批未启用");
    if(ui->app){size_t used=wcslen(context);unsigned pending=(unsigned)yyjson_get_uint(yyjson_obj_get(root,"oauth_pending"));swprintf(context+used,256-used,L"  ·  OAuth %ls（%u 待确认）",yyjson_get_bool(yyjson_obj_get(root,"oauth_enabled"))?L"已启用":L"待设置公网地址",pending);
        if(pending>ui->oauth_pending&&ui->tray_added){NOTIFYICONDATAW info={0};info.cbSize=sizeof(info);info.hWnd=ui->window;info.uID=1;info.uFlags=NIF_INFO;info.dwInfoFlags=NIIF_INFO|NIIF_NOSOUND;wcscpy(info.szInfoTitle,L"ShellBot 客户端连接待确认");wcscpy(info.szInfo,L"请打开管理界面的“客户端授权”，核对申请编号、项目及回调地址。");Shell_NotifyIconW(NIM_MODIFY,&info);}ui->oauth_pending=pending;}
    if(redraw){SendMessageW(ui->list,WM_SETREDRAW,TRUE,0);RedrawWindow(ui->list,NULL,NULL,RDW_INVALIDATE|RDW_FRAME);}
    set_text(ui->context,context); ui->rendering=false; show_project(ui,background);
}
static void free_work(ui_work *work) { free(work->data); free(work->request); free(work->reply); free(work); }
static DWORD WINAPI exchange(void *context) {
    ui_work *work=context;
    if(work->logon) {
        char *args[]={"WinShellBot",work->admin_logon?(work->enable?"--admin-logon-enable":"--admin-logon-disable"):(work->enable?"--logon-enable":"--logon-disable"),"--startup-profile",work->request};
        work->reply=sb_strdup(sb_startup_cli(4,args)==0?(work->enable?"{\"logon_enabled\":true}":"{\"logon_enabled\":false}"):
            "{\"error\":\"Logon result could not be confirmed. Inspect the current logon status before retrying.\"}");
    } else work->reply=work->app ? sb_desktop_exchange(work->app,work->request,&work->error) : sb_owner_exchange(work->data,work->request,strlen(work->request),&work->error);
    /* The UI owns the work until completion or shutdown, even if its HWND has
       already been destroyed. Never free a posted result from this thread. */
    PostMessageW(work->window,UI_RESULT,0,(LPARAM)work);
    return 0;
}
static void logon_request(ui_state *ui,bool enable,bool administrator) {
    if(ui->busy)return;ui_work *work=calloc(1,sizeof(*work));if(!work)return;
    work->window=ui->window;work->logon=true;work->enable=enable;work->admin_logon=administrator;work->request=sb_strdup(ui->profile_path);
    if(!work->request){free_work(work);return;}
    ui->work=work;ui->thread=CreateThread(NULL,0,exchange,work,0,NULL);
    if(!ui->thread){ui->work=NULL;free_work(work);return;}
    ui->busy=true;SetWindowTextW(ui->status,L"正在设置登录自动启动…");update_buttons(ui);
}
static void request(ui_state *ui, const char *command, bool diagnostic, bool background) {
    if(ui->busy)return;
    ui_work *work=calloc(1,sizeof(*work)); yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL);
    if(!work || !doc) { free(work); yyjson_mut_doc_free(doc); return; }
    yyjson_mut_val *root=yyjson_mut_obj(doc); yyjson_mut_doc_set_root(doc,root);
    yyjson_mut_obj_add_str(doc,root,"command",command);
    if(!strcmp(command,"project-request")) {
        static const char probe[]="{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"host_capabilities\",\"arguments\":{},\"_meta\":{\"io.modelcontextprotocol/protocolVersion\":\"2026-07-28\",\"io.modelcontextprotocol/clientCapabilities\":{}}}}";
        yyjson_doc *input=yyjson_read(probe,sizeof(probe)-1,0);
        yyjson_mut_obj_add_val(doc,root,"request",yyjson_val_mut_copy(doc,yyjson_doc_get_root(input))); yyjson_doc_free(input);
    }
    if(!strncmp(command,"project-",8))yyjson_mut_obj_add_strcpy(doc,root,"id",ui->selected);
    work->window=ui->window; work->app=ui->app;work->data=sb_strdup(ui->data); work->request=yyjson_mut_write(doc,0,NULL);
    work->snapshot=!strcmp(command,"management-status"); work->diagnostic=diagnostic; yyjson_mut_doc_free(doc);
    work->background=background;
    snprintf(work->project,sizeof(work->project),"%s",ui->selected);work->selection_generation=ui->selection_generation;
    work->settings=!strcmp(command,"configuration-snapshot");
    if(!work->data || !work->request) { free_work(work); return; }
    ui->work=work;ui->thread=CreateThread(NULL,0,exchange,work,0,NULL);
    if(!ui->thread) { ui->work=NULL;free_work(work); SetWindowTextW(ui->status,L"无法创建本机请求线程。"); return; }
    ui->busy=true;ui->background=background;
    if(!background){set_text(ui->status,L"正在查询或处理，请稍候…");update_buttons(ui);}
}
static bool copy_text(ui_state *ui, const wchar_t *text) {
    size_t bytes=(wcslen(text)+1)*sizeof(wchar_t); HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,bytes);
    wchar_t *buffer=memory ? GlobalLock(memory) : NULL;
    if(!buffer) { if(memory)GlobalFree(memory); return false; }
    memcpy(buffer,text,bytes); GlobalUnlock(memory);
    if(!OpenClipboard(ui->window)) { GlobalFree(memory);set_text(ui->status,L"剪贴板正忙，请重试复制。");return false; }
    bool copied=EmptyClipboard() && SetClipboardData(CF_UNICODETEXT,memory)!=NULL;
    if(!copied)GlobalFree(memory);CloseClipboard();
    set_text(ui->status,copied?L"已复制到剪贴板。":L"复制未完成，请重试。");return copied;
}
static bool confirm(ui_state *ui,const wchar_t *message,const wchar_t *title,UINT flags){
    ui->modal=true;update_buttons(ui);
    int result=MessageBoxW(ui->window,message,title,flags);
    ui->modal=false;
    if(ui->exit_pending){DestroyWindow(ui->window);return false;}
    update_buttons(ui);return result==IDYES;
}
static void click(ui_state *ui, int id) {
    if(id==UI_VIEW_PROJECTS||id==UI_VIEW_CLASSIC){if(!ui->modal){
        DWORD start=0,end=0;SendMessageW(ui->details,EM_GETSEL,(WPARAM)&start,(LPARAM)&end);LRESULT first=SendMessageW(ui->details,EM_GETFIRSTVISIBLELINE,0,0);
        ui->compact=id==UI_VIEW_PROJECTS;layout(ui);
        SendMessageW(ui->details,EM_SETSEL,start,end);SendMessageW(ui->details,EM_LINESCROLL,0,first-SendMessageW(ui->details,EM_GETFIRSTVISIBLELINE,0,0));
    }return;}
    if(ui->busy){if(!ui->modal && (ui->background || (ui->app && (id==UI_NEW || id==UI_SETTINGS || id==UI_CONNECTION || id==UI_OAUTH || id==UI_STORAGE)))){ui->pending_navigation=id;snprintf(ui->pending_project,sizeof(ui->pending_project),"%s",ui->selected);}return;}
    if(ui->modal || ui->exit_pending)return;
    if(id==UI_ABOUT){wchar_t info[256];swprintf(info,256,L"ShellBot %hs\r\n原生 Windows 项目管理\r\n\r\n关闭窗口收进托盘；从“文件”或托盘菜单退出会停止全部项目。",SB_VERSION);ui->modal=true;MessageBoxW(ui->window,info,L"关于 ShellBot",MB_OK|MB_ICONINFORMATION);ui->modal=false;if(ui->exit_pending)DestroyWindow(ui->window);return;}
    if(id>=UI_REFRESH && id<=UI_STORAGE && ui->buttons[id-UI_REFRESH] && !IsWindowEnabled(ui->buttons[id-UI_REFRESH]))return;
    if(ui->app && id==UI_OAUTH){ui->busy=true;ui->modal=true;update_buttons(ui);sb_desktop_oauth_dialog(ui->window,ui->app);ui->modal=false;ui->busy=false;if(ui->exit_pending){DestroyWindow(ui->window);return;}request(ui,"management-status",false,false);return;}
    if(ui->app && (id==UI_NEW || id==UI_SETTINGS || id==UI_CONNECTION || id==UI_STORAGE)) {
        if(id==UI_SETTINGS && !*ui->selected)return;
        ui->busy=true;ui->modal=true;update_buttons(ui);
        bool saved=id==UI_STORAGE ? sb_desktop_storage_dialog(ui->window,ui->app) : id==UI_CONNECTION ? sb_desktop_connection_dialog(ui->window,ui->app) : sb_desktop_project_dialog(ui->window,ui->app,id==UI_NEW ? NULL : ui->selected);
        ui->modal=false;ui->busy=false;
        if(id==UI_STORAGE && saved){resident_command(ui,UI_EXIT);return;}
        if(ui->exit_pending || WaitForSingleObject(ui->stop,0)==WAIT_OBJECT_0){DestroyWindow(ui->window);return;}
        if(saved)SetWindowTextW(ui->status,L"项目设置已保存；列表已同步。");
        update_buttons(ui);request(ui,"management-status",false,false);return;
    }
    if(ui->app && (id==UI_TOKEN || id==UI_OWNER_TOKEN)) {
        sb_desktop_draft draft={0};sb_error error={0};const char *token=id==UI_OWNER_TOKEN ? sb_desktop_owner_token(ui->app) : NULL;
        if(!token && sb_desktop_draft_load(ui->app,ui->selected,&draft,&error))token=draft.token;
        wchar_t *value=token ? to_wide(token) : NULL;bool copied=value && copy_text(ui,value);free(value);sb_desktop_draft_free(&draft);
        if(copied)SetWindowTextW(ui->status,id==UI_OWNER_TOKEN?L"已复制所有者审批令牌，用于 /owner；请妥善保管。":L"已复制所选项目的 MCP 令牌，客户端认证选择 Bearer。");
        else if(!value)set_text(ui->status,L"无法读取所选凭据，请刷新后重试。");return;
    }
    if(id==UI_REFRESH) { request(ui,"management-status",false,false); return; }
    if(id==UI_SETTINGS) { request(ui,"configuration-snapshot",false,false); return; }
    if(id==UI_ADMIN_START || id==UI_ADMIN_STOP) {
        if(id==UI_ADMIN_START && !confirm(ui,L"启动共享管理员代理需要 Windows 授权。只有已允许的项目可使用它。继续启动？",L"启动管理员代理",MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2))return;
        if(id==UI_ADMIN_STOP && !confirm(ui,L"停止代理会结束所有项目的活动管理员任务。继续停止？",L"停止管理员代理",MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2))return;
        request(ui,id==UI_ADMIN_START ? "administrator-start" : "administrator-stop",false,false); return;
    }
    yyjson_val *p=selected_project(ui); if(!p)return;
    if(id==UI_LOCAL || id==UI_PUBLIC) {
        wchar_t *url=to_wide(str(yyjson_obj_get(p,"links"),id==UI_LOCAL ? "local_mcp_url" : "public_mcp_url"));
        if(url && *url)copy_text(ui,url); free(url); return;
    }
    if(id==UI_COPY) { int size=GetWindowTextLengthW(ui->details)+1; wchar_t *text=calloc((size_t)size,sizeof(wchar_t));
        if(text) { GetWindowTextW(ui->details,text,size); copy_text(ui,text); free(text); } return; }
    if(id==UI_DIAGNOSE) {
        if(!strcmp(str(p,"state"),"running"))request(ui,"project-request",true,false);
        else { char *json=yyjson_val_write(p,YYJSON_WRITE_PRETTY,NULL); set_utf8(ui->details,json); free(json); ui->diagnostic=true; update_buttons(ui); }
        return;
    }
    if(id==UI_STOP || id==UI_PAUSE) {
        wchar_t message[256]; swprintf(message,256,L"%ls会结束所选项目的活动任务，并使当前批准失效。其他项目继续运行。是否继续？",id==UI_PAUSE ? L"暂停" : L"停止");
        if(!confirm(ui,message,L"确认项目操作",MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2))return;
    }
    const char *command=id==UI_START ? "project-start" : id==UI_STOP ? "project-stop" : id==UI_PAUSE ? "project-pause" : id==UI_RESUME ? "project-resume" : NULL;
    if(command)request(ui,command,false,false);
}
static int px(ui_state *ui, int value) { return MulDiv(value,(int)GetDpiForWindow(ui->window),96); }
static void fonts(ui_state *ui) {
    HFONT previous=ui->font,previous_heading=ui->heading,previous_section=ui->section;
    NONCLIENTMETRICSW metrics={0};metrics.cbSize=sizeof(metrics);
    if(!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS,sizeof(metrics),&metrics,0,GetDpiForWindow(ui->window))){
        metrics.lfMessageFont.lfHeight=-px(ui,15);wcscpy(metrics.lfMessageFont.lfFaceName,L"Segoe UI");
    }
    ui->font=CreateFontIndirectW(&metrics.lfMessageFont);
    metrics.lfMessageFont.lfHeight=-px(ui,17);metrics.lfMessageFont.lfWeight=FW_SEMIBOLD;
    ui->section=CreateFontIndirectW(&metrics.lfMessageFont);
    metrics.lfMessageFont.lfHeight=-px(ui,23);metrics.lfMessageFont.lfWeight=FW_SEMIBOLD;
    ui->heading=CreateFontIndirectW(&metrics.lfMessageFont);
    if(!ui->font)ui->font=previous;if(!ui->heading)ui->heading=previous_heading;
    if(!ui->section)ui->section=previous_section;
    HWND windows[]={ui->list,ui->details,ui->status,ui->path,ui->context,ui->path_label,ListView_GetHeader(ui->list)};
    for(size_t i=0;i<sizeof(windows)/sizeof(windows[0]);++i)if(windows[i])SendMessageW(windows[i],WM_SETFONT,(WPARAM)ui->font,TRUE);
    for(int i=0;i<18;++i)if(ui->buttons[i])SendMessageW(ui->buttons[i],WM_SETFONT,(WPARAM)ui->font,TRUE);
    if(ui->header)SendMessageW(ui->header,WM_SETFONT,(WPARAM)ui->heading,TRUE);
    if(ui->project_label)SendMessageW(ui->project_label,WM_SETFONT,(WPARAM)(ui->compact?ui->section:ui->font),TRUE);
    if(ui->detail_label)SendMessageW(ui->detail_label,WM_SETFONT,(WPARAM)(ui->compact?ui->section:ui->font),TRUE);
    if(previous && previous!=ui->font)DeleteObject(previous); if(previous_heading && previous_heading!=ui->heading)DeleteObject(previous_heading);
    if(previous_section && previous_section!=ui->section)DeleteObject(previous_section);
}
typedef struct {HWND window;int x,y,w,h;} placement;
static void place(ui_state *ui,placement *items,int *count,HWND window,int x,int y,int w,int h){
    (void)ui;if(window)items[(*count)++]=(placement){window,x,y,w>0?w:1,h>0?h:1};
}
static void classic_layout(ui_state *ui) {
    RECT rect;GetClientRect(ui->window,&rect);if(IsIconic(ui->window))return;
    int w=rect.right,h=rect.bottom,m=px(ui,16),gap=px(ui,8),bh=px(ui,30);
    placement items[32];int count=0;
    place(ui,items,&count,ui->header,m,px(ui,10),w-2*m,px(ui,32));
    place(ui,items,&count,ui->path,m,px(ui,46),w-2*m,px(ui,24));
    const int globals[]={UI_CONNECTION,UI_OAUTH,UI_STORAGE,UI_ADMIN_START,UI_ADMIN_STOP,UI_OWNER_TOKEN};
    int columns=(w-2*m+gap)/(px(ui,132)+gap);if(columns<1)columns=1;int n=0;
    for(int i=0;i<6;++i)if(ui->buttons[globals[i]-UI_REFRESH]){
        place(ui,items,&count,ui->buttons[globals[i]-UI_REFRESH],m+(n%columns)*(px(ui,132)+gap),px(ui,80+(n/columns)*40),px(ui,132),bh);++n;
    }
    int primary_y=80+((n+columns-1)/columns)*40+4;
    columns=(w-2*m+gap)/(px(ui,112)+gap);if(columns<1)columns=1;n=0;
    const int primary[]={UI_NEW,UI_SETTINGS,UI_START,UI_STOP,UI_PAUSE,UI_RESUME,UI_REFRESH};int x=m;
    for(int i=0;i<7;++i)if(ui->buttons[primary[i]-UI_REFRESH]){
        place(ui,items,&count,ui->buttons[primary[i]-UI_REFRESH],m+(n%columns)*(px(ui,112)+gap),px(ui,primary_y+(n/columns)*40),px(ui,112),bh);++n;
    }
    int context_y=primary_y+((n+columns-1)/columns)*40;
    place(ui,items,&count,ui->context,m,px(ui,context_y),w-2*m,px(ui,24));
    int top=px(ui,context_y+54),left=(w-2*m-gap)*40/100,right=w-2*m-left-gap,rx=m+left+gap;
    place(ui,items,&count,ui->project_label,m,px(ui,context_y+30),left,px(ui,22));
    place(ui,items,&count,ui->detail_label,rx,px(ui,context_y+30),right,px(ui,22));
    place(ui,items,&count,ui->list,m,top,left,h-top-px(ui,46));
    place(ui,items,&count,ui->details,rx,top,right,h-top-px(ui,86));
    const int detail[]={UI_LOCAL,UI_PUBLIC,UI_TOKEN,UI_DIAGNOSE,UI_COPY};int total=ui->app?5:4,slot=(right-(total-1)*gap)/total;x=rx;
    for(int i=0;i<5;++i)if(ui->buttons[detail[i]-UI_REFRESH]){
        place(ui,items,&count,ui->buttons[detail[i]-UI_REFRESH],x,h-px(ui,76),slot,bh);x+=slot+gap;
    }
    place(ui,items,&count,ui->status,m,h-px(ui,34),w-2*m,px(ui,26));
    HDWP batch=BeginDeferWindowPos(count);
    for(int i=0;i<count && batch;++i)batch=DeferWindowPos(batch,items[i].window,NULL,items[i].x,items[i].y,items[i].w,items[i].h,SWP_NOZORDER|SWP_NOACTIVATE);
    if(batch)EndDeferWindowPos(batch);
    else for(int i=0;i<count;++i)SetWindowPos(items[i].window,NULL,items[i].x,items[i].y,items[i].w,items[i].h,SWP_NOZORDER|SWP_NOACTIVATE);
    /* Keep readable columns and leave native horizontal scrolling at small widths. */
    int usable=left-GetSystemMetricsForDpi(SM_CXVSCROLL,GetDpiForWindow(ui->window))-px(ui,4);
    int minimum[]={132,72,108,100},weights[]={36,18,23,23};int required=0;
    for(int i=0;i<4;++i)required+=px(ui,minimum[i]);int spare=usable>required?usable-required:0;
    for(int i=0;i<4;++i){int width=px(ui,minimum[i])+spare*weights[i]/100;if(ListView_GetColumnWidth(ui->list,i)!=width)ListView_SetColumnWidth(ui->list,i,width);}
}
static void show_control(HWND window,bool visible){
    if(window && ((GetWindowLongPtrW(window,GWL_STYLE)&WS_VISIBLE)!=0)!=visible)ShowWindow(window,visible?SW_SHOWNA:SW_HIDE);
}
static void compact_layout(ui_state *ui){
    RECT r;GetClientRect(ui->window,&r);int w=r.right,h=r.bottom,m=px(ui,20),gap=px(ui,10),bh=px(ui,30);
    int left=(w-2*m-gap)*38/100,right=w-2*m-gap-left,rx=m+left+gap;
    placement items[32];int count=0;
    int global_width=px(ui,128),global_x=w-m-3*global_width-2*gap;
    place(ui,items,&count,ui->header,m,px(ui,14),global_x-m-gap,px(ui,34));
    const int globals[]={UI_CONNECTION,UI_OAUTH,UI_STORAGE};
    for(int i=0;i<3;++i)place(ui,items,&count,ui->buttons[globals[i]-UI_REFRESH],global_x+i*(global_width+gap),px(ui,18),global_width,bh);
    place(ui,items,&count,ui->context,m,px(ui,60),w-2*m,px(ui,24));
    place(ui,items,&count,ui->divider,m,px(ui,92),w-2*m,px(ui,2));
    place(ui,items,&count,ui->project_label,m,px(ui,108),left-px(ui,118),px(ui,26));
    place(ui,items,&count,ui->buttons[UI_NEW-UI_REFRESH],m+left-px(ui,112),px(ui,104),px(ui,112),bh);
    place(ui,items,&count,ui->detail_label,rx,px(ui,108),right-px(ui,126),px(ui,26));
    place(ui,items,&count,ui->buttons[UI_SETTINGS-UI_REFRESH],w-m-px(ui,120),px(ui,104),px(ui,120),bh);
    place(ui,items,&count,ui->list,m,px(ui,146),left,h-px(ui,262));
    int action_width=(right-3*gap)/4;if(action_width>px(ui,116))action_width=px(ui,116);
    for(int i=0;i<4;++i)place(ui,items,&count,ui->buttons[UI_START+i-UI_REFRESH],rx+i*(action_width+gap),px(ui,146),action_width,bh);
    place(ui,items,&count,ui->details,rx,px(ui,188),right,h-px(ui,304));
    place(ui,items,&count,ui->buttons[UI_REFRESH-UI_REFRESH],m,h-px(ui,104),px(ui,132),bh);
    const int details[]={UI_LOCAL,UI_PUBLIC,UI_TOKEN,UI_DIAGNOSE,UI_COPY};int slot=(right-4*gap)/5;
    for(int i=0;i<5;++i)place(ui,items,&count,ui->buttons[details[i]-UI_REFRESH],rx+i*(slot+gap),h-px(ui,104),slot,bh);
    place(ui,items,&count,ui->status,m,h-px(ui,62),w-2*m,px(ui,24));
    place(ui,items,&count,ui->path_label,m,h-px(ui,32),px(ui,76),px(ui,22));
    place(ui,items,&count,ui->path,m+px(ui,76),h-px(ui,32),w-2*m-px(ui,76),px(ui,22));
    HDWP batch=BeginDeferWindowPos(count);
    for(int i=0;i<count&&batch;++i)batch=DeferWindowPos(batch,items[i].window,NULL,items[i].x,items[i].y,items[i].w,items[i].h,SWP_NOZORDER|SWP_NOACTIVATE);
    if(batch)EndDeferWindowPos(batch);
    else for(int i=0;i<count;++i)SetWindowPos(items[i].window,NULL,items[i].x,items[i].y,items[i].w,items[i].h,SWP_NOZORDER|SWP_NOACTIVATE);
    int usable=left-GetSystemMetricsForDpi(SM_CXVSCROLL,GetDpiForWindow(ui->window))-px(ui,4);
    int minimum[]={110,70,104,98},weights[]={36,18,23,23},required=0;
    for(int i=0;i<4;++i)required+=px(ui,minimum[i]);int spare=usable>required?usable-required:0;
    for(int i=0;i<4;++i){int width=px(ui,minimum[i])+spare*weights[i]/100;if(ListView_GetColumnWidth(ui->list,i)!=width)ListView_SetColumnWidth(ui->list,i,width);}
}
static void layout(ui_state *ui){
    if(IsIconic(ui->window))return;
    bool compact=ui->compact&&ui->app;
    if(!ui->app)ui->compact=false;
    for(int i=0;i<18;++i)show_control(ui->buttons[i],!compact||(i!=UI_ADMIN_START-UI_REFRESH&&i!=UI_ADMIN_STOP-UI_REFRESH&&i!=UI_OWNER_TOKEN-UI_REFRESH));
    show_control(ui->divider,compact);show_control(ui->path_label,compact);
    set_text(ui->header,compact?L"ShellBot":L"ShellBot  项目管理");
    HFONT section=compact?ui->section:ui->font;
    if((HFONT)SendMessageW(ui->project_label,WM_GETFONT,0,0)!=section)SendMessageW(ui->project_label,WM_SETFONT,(WPARAM)section,TRUE);
    if((HFONT)SendMessageW(ui->detail_label,WM_GETFONT,0,0)!=section)SendMessageW(ui->detail_label,WM_SETFONT,(WPARAM)section,TRUE);
    if(compact)compact_layout(ui);else classic_layout(ui);
    /* Native Tab navigation follows the visible reading order in each view. */
    if(ui->tab_layout!=(compact?2:1)){
        HWND order[24];int n=0;
        if(compact){
            const int top[]={UI_CONNECTION,UI_OAUTH,UI_STORAGE,UI_NEW};for(int i=0;i<4;++i)order[n++]=ui->buttons[top[i]-UI_REFRESH];
            order[n++]=ui->list;order[n++]=ui->buttons[UI_REFRESH-UI_REFRESH];order[n++]=ui->buttons[UI_SETTINGS-UI_REFRESH];
            for(int i=0;i<4;++i)order[n++]=ui->buttons[UI_START+i-UI_REFRESH];
        }else{
            const int top[]={UI_CONNECTION,UI_OAUTH,UI_STORAGE,UI_ADMIN_START,UI_ADMIN_STOP,UI_OWNER_TOKEN,UI_NEW,UI_SETTINGS,UI_START,UI_STOP,UI_PAUSE,UI_RESUME,UI_REFRESH};
            for(int i=0;i<13;++i)order[n++]=ui->buttons[top[i]-UI_REFRESH];order[n++]=ui->list;
        }
        order[n++]=ui->details;const int bottom[]={UI_LOCAL,UI_PUBLIC,UI_TOKEN,UI_DIAGNOSE,UI_COPY};for(int i=0;i<5;++i)order[n++]=ui->buttons[bottom[i]-UI_REFRESH];
        for(int i=n-1;i>=0;--i)if(order[i])SetWindowPos(order[i],HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
        ui->tab_layout=compact?2:1;
        HWND focus=GetFocus();if(focus && GetParent(focus)==ui->window && !(GetWindowLongPtrW(focus,GWL_STYLE)&WS_VISIBLE))SetFocus(ui->list);
    }
    project_heading(ui);
    if(ui->view_menu)CheckMenuRadioItem(ui->view_menu,UI_VIEW_PROJECTS,UI_VIEW_CLASSIC,compact?UI_VIEW_PROJECTS:UI_VIEW_CLASSIC,MF_BYCOMMAND);
    InvalidateRect(ui->details,NULL,TRUE);
}
static void menu_item(HMENU menu,int id,const wchar_t *label){AppendMenuW(menu,MF_STRING,(UINT_PTR)id,label);}
static HMENU create_menu(ui_state *ui){
    HMENU bar=CreateMenu(),file=CreatePopupMenu(),project=CreatePopupMenu(),connection=CreatePopupMenu(),tools=CreatePopupMenu(),help=CreatePopupMenu();
    if(!bar||!file||!project||!connection||!tools||!help){if(bar)DestroyMenu(bar);if(file)DestroyMenu(file);if(project)DestroyMenu(project);if(connection)DestroyMenu(connection);if(tools)DestroyMenu(tools);if(help)DestroyMenu(help);return NULL;}
    if(ui->app){menu_item(file,UI_NEW,L"新建项目(&N)…\tCtrl+N");menu_item(file,UI_STORAGE,L"数据位置(&D)…");}
    menu_item(file,UI_REFRESH,L"刷新(&R)\tF5");AppendMenuW(file,MF_SEPARATOR,0,NULL);
    if(ui->resident)menu_item(file,UI_HIDE,L"收进托盘(&H)");menu_item(file,UI_EXIT,ui->app?L"退出并停止全部项目(&X)":L"退出管理窗口(&X)");
    const wchar_t *names[]={L"启动项目(&S)",L"停止项目(&T)",L"暂停项目(&P)",L"恢复项目(&R)"};for(int i=0;i<4;++i)menu_item(project,UI_START+i,names[i]);AppendMenuW(project,MF_SEPARATOR,0,NULL);menu_item(project,UI_SETTINGS,L"项目设置(&E)…\tCtrl+E");
    if(ui->app){menu_item(connection,UI_CONNECTION,L"连接设置(&S)…");menu_item(connection,UI_OAUTH,L"客户端授权(&A)…\tCtrl+L");AppendMenuW(connection,MF_SEPARATOR,0,NULL);}
    menu_item(connection,UI_LOCAL,L"复制本机地址(&L)");menu_item(connection,UI_PUBLIC,L"复制公网地址(&P)");if(ui->app)menu_item(connection,UI_TOKEN,L"复制项目令牌(&T)");
    menu_item(tools,UI_DIAGNOSE,L"查看诊断(&D)");menu_item(tools,UI_COPY,L"复制诊断(&C)");AppendMenuW(tools,MF_SEPARATOR,0,NULL);menu_item(tools,UI_ADMIN_START,L"启动管理员代理(&S)…");menu_item(tools,UI_ADMIN_STOP,L"停止管理员代理(&T)…");if(ui->app)menu_item(tools,UI_OWNER_TOKEN,L"复制审批令牌(&O)");menu_item(help,UI_ABOUT,L"关于 ShellBot(&A)…");
    AppendMenuW(bar,MF_POPUP,(UINT_PTR)file,L"文件(&F)");AppendMenuW(bar,MF_POPUP,(UINT_PTR)project,L"项目(&P)");AppendMenuW(bar,MF_POPUP,(UINT_PTR)connection,L"连接(&C)");
    if(ui->app){ui->view_menu=CreatePopupMenu();if(ui->view_menu){menu_item(ui->view_menu,UI_VIEW_PROJECTS,L"项目优先布局(&P)");menu_item(ui->view_menu,UI_VIEW_CLASSIC,L"经典布局(&C)");AppendMenuW(bar,MF_POPUP,(UINT_PTR)ui->view_menu,L"视图(&V)");}}
    AppendMenuW(bar,MF_POPUP,(UINT_PTR)tools,L"工具(&T)");AppendMenuW(bar,MF_POPUP,(UINT_PTR)help,L"帮助(&H)");return bar;
}
static HWND control(ui_state *ui, DWORD extended, const wchar_t *class_name, const wchar_t *text, DWORD style, int id) {
    HWND window=CreateWindowExW(extended,class_name,text,WS_CHILD|WS_VISIBLE|style,0,0,0,0,ui->window,(HMENU)(INT_PTR)id,GetModuleHandleW(NULL),NULL);
    SendMessageW(window,WM_SETFONT,(WPARAM)ui->font,TRUE); return window;
}
static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    ui_state *ui=(ui_state *)GetWindowLongPtrW(window,GWLP_USERDATA);
    if(message==WM_NCCREATE) { ui=((CREATESTRUCTW *)lparam)->lpCreateParams; ui->window=window; SetWindowLongPtrW(window,GWLP_USERDATA,(LONG_PTR)ui); }
    if(!ui)return DefWindowProcW(window,message,wparam,lparam);
    if(ui->resident && message==ui->taskbar_created){ui->tray_added=false;tray(ui,NIM_ADD);SetPropW(window,L"ShellBotTrayRegistered",ui->tray_added?(HANDLE)1:NULL);return 0;}
    switch(message) {
    case WM_CREATE: {
        fonts(ui);
        SetMenu(window,create_menu(ui));
        ui->header=control(ui,0,L"STATIC",L"ShellBot  项目管理",SS_ENDELLIPSIS,UI_HEADER); SendMessageW(ui->header,WM_SETFONT,(WPARAM)ui->heading,TRUE);
        ui->path=control(ui,0,L"EDIT",L"",ES_READONLY|ES_AUTOHSCROLL,UI_PATH); set_utf8(ui->path,ui->data);
        const wchar_t *labels[]={L"刷新 (F5)",L"启动项目",L"停止项目",L"暂停项目",L"恢复项目",L"本机地址",L"公网地址",L"查看诊断",L"复制诊断",L"启动管理员",L"停止管理员",L"项目设置…",L"新建项目…",L"连接设置…",L"项目令牌",L"复制审批令牌",L"客户端授权…",L"数据位置…"};
        for(int i=0;i<(ui->app?18:12);++i)ui->buttons[i]=control(ui,0,L"BUTTON",!ui->app && i==11?L"项目配置…":labels[i],WS_TABSTOP|BS_PUSHBUTTON,UI_REFRESH+i);
        ui->context=control(ui,0,L"STATIC",L"连接本机管理器…",SS_ENDELLIPSIS,UI_CONTEXT);
        ui->project_label=control(ui,0,L"STATIC",L"项目列表",SS_ENDELLIPSIS,UI_PROJECT_LABEL);
        ui->detail_label=control(ui,0,L"STATIC",L"项目详情",SS_ENDELLIPSIS,UI_DETAIL_LABEL);
        ui->divider=control(ui,0,L"STATIC",L"",SS_ETCHEDHORZ,UI_DIVIDER);
        ui->path_label=control(ui,0,L"STATIC",L"应用数据：",0,UI_PATH_LABEL);
        ui->list=control(ui,WS_EX_CLIENTEDGE,WC_LISTVIEWW,L"",WS_TABSTOP|LVS_REPORT|LVS_SINGLESEL|LVS_SHOWSELALWAYS,UI_LIST);
        ListView_SetExtendedListViewStyle(ui->list,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER|LVS_EX_LABELTIP);
        const wchar_t *columns[]={L"项目",L"状态",L"审批方式",L"任务身份"};
        for(int i=0;i<4;++i) { LVCOLUMNW column={0}; column.mask=LVCF_TEXT|LVCF_WIDTH; column.pszText=(wchar_t *)columns[i]; column.cx=100; SendMessageW(ui->list,LVM_INSERTCOLUMNW,(WPARAM)i,(LPARAM)&column); }
        ui->details=control(ui,WS_EX_CLIENTEDGE,L"EDIT",L"",WS_TABSTOP|ES_READONLY|ES_MULTILINE|ES_AUTOVSCROLL|WS_VSCROLL,UI_DETAILS);
        SendMessageW(ui->details,EM_SETLIMITTEXT,SB_RESPONSE_LIMIT,0);
        ui->status=control(ui,0,L"STATIC",L"",SS_ENDELLIPSIS,UI_STATUS); update_buttons(ui); layout(ui);
        PostMessageW(window,WM_COMMAND,UI_REFRESH,0); return 0;
    }
    case WM_COMMAND:
        if(LOWORD(wparam)==UI_EXIT && !ui->resident){DestroyWindow(window);return 0;}
        if(ui->resident && LOWORD(wparam)>=UI_SHOW && LOWORD(wparam)<=UI_ADMIN_LOGON_DISABLE)resident_command(ui,LOWORD(wparam));
        else if(HIWORD(wparam)==BN_CLICKED)click(ui,LOWORD(wparam));return 0;
    case WM_CTLCOLORSTATIC:
        if(ui->compact && (HWND)lparam==ui->details){SetTextColor((HDC)wparam,GetSysColor(COLOR_WINDOWTEXT));SetBkColor((HDC)wparam,GetSysColor(COLOR_WINDOW));return (LRESULT)GetSysColorBrush(COLOR_WINDOW);}
        if(ui->compact && ((HWND)lparam==ui->path || (HWND)lparam==ui->path_label || (HWND)lparam==ui->context)){SetTextColor((HDC)wparam,GetSysColor(COLOR_GRAYTEXT));SetBkColor((HDC)wparam,GetSysColor(COLOR_BTNFACE));return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);}
        break;
    case UI_TRAY:
        if(ui->resident && wparam==1){if(lparam==WM_LBUTTONUP || lparam==WM_LBUTTONDBLCLK)show_resident(ui);else if(lparam==NIN_BALLOONUSERCLICK){show_resident(ui);PostMessageW(window,WM_COMMAND,UI_OAUTH,0);}else if(lparam==WM_RBUTTONUP)tray_menu(ui);}return 0;
    case WM_TIMER:
        if(ui->resident){
            if(ui->app && !ui->modal && WaitForSingleObject(ui->stop,0)==WAIT_OBJECT_0){DestroyWindow(window);return 0;}
            if(!ui->app && WaitForSingleObject(ui->manager_thread,0)!=WAIT_TIMEOUT){DestroyWindow(window);return 0;}
            if(!ui->tray_added){tray(ui,NIM_ADD);SetPropW(window,L"ShellBotTrayRegistered",ui->tray_added?(HANDLE)1:NULL);}
        }
        if(!ui->busy && !ui->modal)request(ui,"management-status",false,true);
        return 0;
    case WM_CLOSE:
        if(ui->resident && ui->tray_added){ShowWindow(window,SW_HIDE);return 0;}
        if(ui->app){SetWindowTextW(ui->status,L"系统托盘暂不可用，窗口保持打开；请等待托盘恢复后右键退出。");return 0;}break;
    case WM_QUERYENDSESSION:return TRUE;
    case WM_ENDSESSION:
        if(ui->resident && wparam){SetEvent(ui->stop);DestroyWindow(window);}return 0;
    case WM_NOTIFY: {
        NMHDR *header=(NMHDR *)lparam;
        if(header->idFrom==UI_LIST && header->code==LVN_ITEMCHANGED && !ui->rendering)PostMessageW(window,UI_SELECTION,0,0);
        return 0;
    }
    case UI_SELECTION:
        if(!ui->rendering){
            int index=ListView_GetNextItem(ui->list,-1,LVNI_SELECTED);
            yyjson_val *p=index>=0 ? yyjson_arr_get(yyjson_obj_get(yyjson_doc_get_root(ui->snapshot),"projects"),(size_t)index) : NULL;
            if(strcmp(ui->selected,str(p,"project_id"))){snprintf(ui->selected,sizeof(ui->selected),"%s",str(p,"project_id"));++ui->selection_generation;show_project(ui,false);}
        }return 0;
    case UI_RESULT: {
        ui_work *work=(ui_work *)lparam;if(work!=ui->work)return 0;
        CloseHandle(ui->thread); ui->thread=NULL;ui->work=NULL;ui->busy=false;ui->background=false;
        yyjson_doc *reply=work->reply ? yyjson_read(work->reply,strlen(work->reply),0) : NULL;
        yyjson_val *root=yyjson_doc_get_root(reply); const char *error=str(root,"error");
        bool valid=!work->snapshot || valid_snapshot(root);
        if(!reply || *error || !valid) {
            if(work->snapshot){
                ui->stale=true;
                set_text(ui->status,L"刷新失败，保留上次内容；项目操作暂不可用，连接恢复后自动更新。");
                set_text(ui->context,L"管理器状态暂未更新 · 当前显示上次成功取得的内容");
            }else{
                wchar_t *failure=to_wide(*error?error:work->error.text);set_text(ui->status,failure&&*failure?failure:L"请求未完成，请刷新后核对当前状态。");free(failure);
            }
        } else if(work->settings) {
            yyjson_val *configuration=yyjson_obj_get(root,"configuration");
            char *json=configuration ? yyjson_val_write(configuration,0,NULL) : NULL;
            if(json){ui->modal=true;update_buttons(ui);sb_project_settings(ui->window,NULL,json);ui->modal=false;} free(json);
        } else if(work->logon) {
            SetWindowTextW(ui->status,work->enable?L"已启用此配置的登录自动启动。":L"已关闭此配置的登录自动启动；本次仍继续运行。");
        } else if(work->snapshot) {
            bool recovered=ui->stale;ui->stale=false;
            yyjson_doc_free(ui->snapshot); ui->snapshot=reply; reply=NULL; render(ui,work->background);
            const char *notice=str(yyjson_doc_get_root(ui->snapshot),"runtime_notice");
            if(ui->app && *notice)set_utf8(ui->status,notice);
            else if(!work->background || recovered || ui->notice_visible)set_text(ui->status,ui->app?L"状态已刷新。关闭窗口收进托盘；从托盘退出会停止全部项目。":L"状态已刷新。地址不含凭据；暂停状态仅在当前管理器有效。");
            ui->notice_visible=ui->app && *notice;
        } else if(work->diagnostic) {
            if(!strcmp(work->project,ui->selected) && work->selection_generation==ui->selection_generation){
                char *json=yyjson_val_write(root,YYJSON_WRITE_PRETTY,NULL);set_project_details(ui,L"",false);set_utf8(ui->details,json);free(json);ui->diagnostic=true;
                SetWindowTextW(ui->status,L"已查询实际运行能力；诊断可复制。");
            }else set_text(ui->status,L"诊断已完成；所选项目已改变，保留当前项目详情。");
        }
        bool refresh=!work->snapshot && !work->diagnostic && !work->logon && !work->settings && reply && !*error;
        yyjson_doc_free(reply); free_work(work); update_buttons(ui);
        if(ui->exit_pending){DestroyWindow(window);return 0;}
        if(ui->pending_navigation){int id=ui->pending_navigation;ui->pending_navigation=0;if(!project_action(id)||!strcmp(ui->pending_project,ui->selected))click(ui,id);else set_text(ui->status,L"所选项目已改变，请在当前项目上重新选择操作。");}
        else if(refresh)request(ui,"management-status",false,false);
        return 0;
    }
    case WM_APP+19:show_resident(ui);return 0;
    case WM_SIZE: if(ui->list)layout(ui); return 0;
    case WM_GETMINMAXINFO:{
        MONITORINFO monitor={sizeof(monitor)};GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&monitor);
        int w=px(ui,1000),h=px(ui,650);if(monitor.rcWork.right>monitor.rcWork.left && w>monitor.rcWork.right-monitor.rcWork.left)w=monitor.rcWork.right-monitor.rcWork.left;
        if(monitor.rcWork.bottom>monitor.rcWork.top && h>monitor.rcWork.bottom-monitor.rcWork.top)h=monitor.rcWork.bottom-monitor.rcWork.top;
        ((MINMAXINFO *)lparam)->ptMinTrackSize=(POINT){w,h};return 0;
    }
    case WM_DPICHANGED: {
        RECT *rect=(RECT *)lparam;fonts(ui);SetWindowPos(window,NULL,rect->left,rect->top,rect->right-rect->left,rect->bottom-rect->top,SWP_NOZORDER|SWP_NOACTIVATE);layout(ui);return 0;
    }
    case WM_SETTINGCHANGE:fonts(ui);layout(ui);InvalidateRect(window,NULL,TRUE);return 0;
    case WM_DESTROY:KillTimer(window,1);if(ui->resident){tray(ui,NIM_DELETE);RemovePropW(window,L"ShellBotTrayRegistered");SetEvent(ui->stop);if(ui->app)sb_desktop_request_exit(ui->app);}PostQuitMessage(0); return 0;
    default: return DefWindowProcW(window,message,wparam,lparam);
    }
    return DefWindowProcW(window,message,wparam,lparam);
}
static int run_ui(const char *data_dir,HANDLE stop,HANDLE manager_thread,const char *profile_path,sb_desktop_app *app) {
    HANDLE token=NULL; TOKEN_ELEVATION elevation={0}; DWORD size=0;
    bool allowed=OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token) && sb_win_owner_token_allowed(token) &&
        GetTokenInformation(token,TokenElevation,&elevation,sizeof(elevation),&size) && (!elevation.TokenIsElevated || sb_win_elevated_owner());
    if(token)CloseHandle(token);
    if(!allowed) { fputs("ShellBot: Management window requires a valid interactive owner token.\n",stderr); return 2; }
    ui_state ui={0};ui.resident=stop!=NULL;ui.stop=stop;ui.manager_thread=manager_thread;ui.profile_path=profile_path;ui.app=app;ui.compact=app!=NULL;
    ui.taskbar_created=RegisterWindowMessageW(L"TaskbarCreated");ui.data=sb_strdup(data_dir); if(!ui.data)return 2;
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX controls={sizeof(controls),ICC_LISTVIEW_CLASSES}; InitCommonControlsEx(&controls);
    WNDCLASSW klass={0}; klass.lpfnWndProc=window_proc; klass.hInstance=GetModuleHandleW(NULL); klass.hCursor=LoadCursorW(NULL,IDC_ARROW);
    klass.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1);klass.hIcon=LoadIconW(NULL,IDI_APPLICATION); klass.lpszClassName=L"ShellBotManagerWindow";
    if(!RegisterClassW(&klass) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) { free(ui.data); return 2; }
    RECT area={0}; SystemParametersInfoW(SPI_GETWORKAREA,0,&area,0);
    int dpi=(int)GetDpiForSystem(),width=MulDiv(1160,dpi,96),height=MulDiv(800,dpi,96);
    if(area.right-area.left>0 && width>area.right-area.left-40)width=area.right-area.left-40;
    if(area.bottom-area.top>0 && height>area.bottom-area.top-40)height=area.bottom-area.top-40;
    HWND window=CreateWindowExW(0,klass.lpszClassName,L"ShellBot 项目管理",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,width,height,NULL,NULL,klass.hInstance,&ui);
    if(!window) { free(ui.data); return 2; }
    if(app)SetPropW(window,sb_desktop_instance_key(app),(HANDLE)1);
    /* Detach only our GUI process; never hide the caller's terminal window. */
    FreeConsole(); STARTUPINFOW startup={0}; startup.cb=sizeof(startup); GetStartupInfoW(&startup);
    if(ui.resident){tray(&ui,NIM_ADD);SetPropW(window,L"ShellBotTrayRegistered",ui.tray_added?(HANDLE)1:NULL);}SetTimer(window,1,2000,NULL);
    ShowWindow(window,app?SW_SHOWNORMAL:ui.resident ? (ui.tray_added ? SW_HIDE : SW_SHOWNORMAL) : startup.dwFlags&STARTF_USESHOWWINDOW ? startup.wShowWindow : SW_SHOWNORMAL); UpdateWindow(window);
    ACCEL keys[]={{FVIRTKEY,VK_F5,UI_REFRESH},{FVIRTKEY|FCONTROL,'N',UI_NEW},{FVIRTKEY|FCONTROL,'E',UI_SETTINGS},{FVIRTKEY|FCONTROL,'L',UI_OAUTH}};
    HACCEL accelerators=CreateAcceleratorTableW(keys,sizeof(keys)/sizeof(keys[0]));
    MSG message;while(GetMessageW(&message,NULL,0,0)>0){if(!TranslateAcceleratorW(window,accelerators,&message) && !IsDialogMessageW(window,&message)){TranslateMessage(&message);DispatchMessageW(&message);}}
    if(accelerators)DestroyAcceleratorTable(accelerators);
    if(ui.thread){if(WaitForSingleObject(ui.thread,30000)!=WAIT_OBJECT_0)ExitProcess(3);CloseHandle(ui.thread);}
    if(ui.work)free_work(ui.work);
    yyjson_doc_free(ui.snapshot); DeleteObject(ui.font); DeleteObject(ui.heading);DeleteObject(ui.section); free(ui.data); return 0;
}
int sb_manager_ui(const char *data_dir) {return run_ui(data_dir,NULL,NULL,NULL,NULL);}
int sb_manager_resident_ui(const char *data_dir,HANDLE stop,HANDLE manager_thread,const char *profile_path) {
    return run_ui(data_dir,stop,manager_thread,profile_path,NULL);
}
int sb_manager_desktop_ui(sb_desktop_app *app) {return run_ui(sb_desktop_data(app),sb_desktop_stop(app),NULL,NULL,app);}
