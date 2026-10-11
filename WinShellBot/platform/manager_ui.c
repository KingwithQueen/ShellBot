#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include "sb_projects.h"
#include "manager_ui.h"
#include "execution.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <stdarg.h>

enum { UI_REFRESH=1001, UI_START, UI_STOP, UI_PAUSE, UI_RESUME, UI_LOCAL, UI_PUBLIC,
       UI_DIAGNOSE, UI_COPY, UI_ADMIN_START, UI_ADMIN_STOP, UI_LIST=1100, UI_DETAILS, UI_STATUS, UI_HEADER, UI_PATH, UI_CONTEXT };
#define UI_RESULT (WM_APP+1)
#define UI_TEXT_CAP 65536u
typedef struct {
    HWND window;
    char *data, *request, *reply;
    bool snapshot, diagnostic;
    sb_error error;
} ui_work;
typedef struct {
    HWND window, list, details, status, header, path, context, buttons[11];
    HFONT font, heading;
    HANDLE thread;
    char *data;
    yyjson_doc *snapshot;
    char selected[64];
    bool busy, rendering, diagnostic;
} ui_state;

static wchar_t *to_wide(const char *text) {
    int size=text ? MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text,-1,NULL,0) : 0;
    wchar_t *value=size ? calloc((size_t)size,sizeof(wchar_t)) : NULL;
    if(value && !MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text,-1,value,size)) { free(value); value=NULL; }
    return value;
}
static void set_utf8(HWND window, const char *text) {
    wchar_t *value=to_wide(text); SetWindowTextW(window,value ? value : L"内容不可用"); free(value);
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
static void append(wchar_t *text, size_t *length, const wchar_t *format, ...) {
    if(*length>=UI_TEXT_CAP-128)return;
    va_list args; va_start(args,format);
    int count=vswprintf(text+*length,UI_TEXT_CAP-*length,format,args); va_end(args);
    if(count<0) { text[*length]=0; wcscat(text,L"\r\n显示内容过长；请查看诊断或原配置。\r\n"); *length=UI_TEXT_CAP; }
    else *length+=(size_t)count;
}
static void update_buttons(ui_state *ui) {
    yyjson_val *p=selected_project(ui); const char *state=str(p,"state");
    bool available=p && strcmp(state,"busy") && !ui->busy;
    bool enabled=yyjson_get_bool(yyjson_obj_get(p,"enabled"));
    bool paused=yyjson_get_bool(yyjson_obj_get(p,"paused"));
    bool running=!strcmp(state,"running");
    for(int i=0;i<11;++i)EnableWindow(ui->buttons[i],!ui->busy);
    EnableWindow(ui->buttons[UI_START-UI_REFRESH],available && enabled && !paused && !running);
    EnableWindow(ui->buttons[UI_STOP-UI_REFRESH],available && (running || paused));
    EnableWindow(ui->buttons[UI_PAUSE-UI_REFRESH],available && enabled && !paused);
    EnableWindow(ui->buttons[UI_RESUME-UI_REFRESH],available && enabled && paused);
    yyjson_val *links=yyjson_obj_get(p,"links");
    EnableWindow(ui->buttons[UI_LOCAL-UI_REFRESH],available && *str(links,"local_mcp_url"));
    EnableWindow(ui->buttons[UI_PUBLIC-UI_REFRESH],available && *str(links,"public_mcp_url"));
    EnableWindow(ui->buttons[UI_DIAGNOSE-UI_REFRESH],available);
    EnableWindow(ui->buttons[UI_COPY-UI_REFRESH],!ui->busy && ui->diagnostic);
    yyjson_val *root=yyjson_doc_get_root(ui->snapshot);
    bool alive=yyjson_get_bool(yyjson_obj_get(root,"administrator_alive"));
    bool starting=yyjson_get_bool(yyjson_obj_get(root,"administrator_starting"));
    EnableWindow(ui->buttons[UI_ADMIN_START-UI_REFRESH],root && !ui->busy && !alive && !starting);
    EnableWindow(ui->buttons[UI_ADMIN_STOP-UI_REFRESH],!ui->busy && (alive || starting));
}
static void show_project(ui_state *ui) {
    yyjson_val *p=selected_project(ui);
    ui->diagnostic=false;
    if(!p) { SetWindowTextW(ui->details,L"选择一个项目，查看地址、权限和运行状态。"); update_buttons(ui); return; }
    wchar_t *text=calloc(UI_TEXT_CAP,sizeof(wchar_t)); size_t length=0;
    wchar_t *id=to_wide(str(p,"project_id"));
    if(!text) { free(id); return; }
    append(text,&length,L"%ls  ·  %ls\r\n\r\n",id ? id : L"",state_label(str(p,"state"))); free(id);
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
    SetWindowTextW(ui->details,text); free(text); update_buttons(ui);
}
static void render(ui_state *ui) {
    ui->rendering=true; ListView_DeleteAllItems(ui->list);
    yyjson_val *root=yyjson_doc_get_root(ui->snapshot), *projects=yyjson_obj_get(root,"projects"), *p; size_t i,max; int selected=-1;
    yyjson_arr_foreach(projects,i,max,p) {
        wchar_t *id=to_wide(str(p,"project_id")); LVITEMW row={0}; row.mask=LVIF_TEXT; row.iItem=(int)i; row.pszText=id ? id : L"";
        int index=(int)SendMessageW(ui->list,LVM_INSERTITEMW,0,(LPARAM)&row); free(id);
        ListView_SetItemText(ui->list,index,1,(wchar_t *)state_label(str(p,"state")));
        ListView_SetItemText(ui->list,index,2,(wchar_t *)approval_label(str(p,"approval_mode")));
        ListView_SetItemText(ui->list,index,3,(wchar_t *)identity_label(str(p,"default_identity")));
        if(!strcmp(ui->selected,str(p,"project_id")))selected=index;
    }
    if(selected<0 && yyjson_arr_size(projects)) { selected=0; snprintf(ui->selected,sizeof(ui->selected),"%s",str(yyjson_arr_get(projects,0),"project_id")); }
    if(selected>=0) { ListView_SetItemState(ui->list,selected,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED); }
    else *ui->selected=0;
    wchar_t context[256];
    swprintf(context,256,L"%zu 个项目  ·  网关%ls  ·  管理员代理%ls  ·  %ls",yyjson_arr_size(projects),
        yyjson_get_bool(yyjson_obj_get(root,"http_gateway")) ? L"已启动" : L"未启动",
        yyjson_get_bool(yyjson_obj_get(root,"administrator_alive")) ? L"已启动" : yyjson_get_bool(yyjson_obj_get(root,"administrator_starting")) ? L"等待授权" : L"未启动",
        yyjson_get_bool(yyjson_obj_get(root,"remote_owner_enabled")) ? L"远程审批已启用" : L"远程审批未启用");
    SetWindowTextW(ui->context,context); ui->rendering=false; show_project(ui);
}
static void free_work(ui_work *work) { free(work->data); free(work->request); free(work->reply); free(work); }
static DWORD WINAPI exchange(void *context) {
    ui_work *work=context;
    work->reply=sb_owner_exchange(work->data,work->request,strlen(work->request),&work->error);
    if(!PostMessageW(work->window,UI_RESULT,0,(LPARAM)work))free_work(work);
    return 0;
}
static void request(ui_state *ui, const char *command, bool diagnostic) {
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
    work->window=ui->window; work->data=sb_strdup(ui->data); work->request=yyjson_mut_write(doc,0,NULL);
    work->snapshot=!strcmp(command,"management-status"); work->diagnostic=diagnostic; yyjson_mut_doc_free(doc);
    if(!work->data || !work->request) { free_work(work); return; }
    ui->thread=CreateThread(NULL,0,exchange,work,0,NULL);
    if(!ui->thread) { free_work(work); SetWindowTextW(ui->status,L"无法创建本机请求线程。"); return; }
    ui->busy=true; SetWindowTextW(ui->status,L"正在查询或处理，请稍候…"); update_buttons(ui);
}
static void copy_text(ui_state *ui, const wchar_t *text) {
    size_t bytes=(wcslen(text)+1)*sizeof(wchar_t); HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,bytes);
    wchar_t *buffer=memory ? GlobalLock(memory) : NULL;
    if(!buffer) { if(memory)GlobalFree(memory); return; }
    memcpy(buffer,text,bytes); GlobalUnlock(memory);
    if(!OpenClipboard(ui->window)) { GlobalFree(memory); return; }
    EmptyClipboard(); if(!SetClipboardData(CF_UNICODETEXT,memory))GlobalFree(memory); CloseClipboard();
    SetWindowTextW(ui->status,L"已复制到剪贴板。");
}
static void click(ui_state *ui, int id) {
    if(ui->busy)return;
    if(id==UI_REFRESH) { request(ui,"management-status",false); return; }
    if(id==UI_ADMIN_START || id==UI_ADMIN_STOP) {
        if(id==UI_ADMIN_START && MessageBoxW(ui->window,L"启动共享管理员代理需要 Windows 授权。只有已允许的项目可使用它。继续启动？",L"启动管理员代理",MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2)!=IDYES)return;
        if(id==UI_ADMIN_STOP && MessageBoxW(ui->window,L"停止代理会结束所有项目的活动管理员任务。继续停止？",L"停止管理员代理",MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2)!=IDYES)return;
        request(ui,id==UI_ADMIN_START ? "administrator-start" : "administrator-stop",false); return;
    }
    yyjson_val *p=selected_project(ui); if(!p)return;
    if(id==UI_LOCAL || id==UI_PUBLIC) {
        wchar_t *url=to_wide(str(yyjson_obj_get(p,"links"),id==UI_LOCAL ? "local_mcp_url" : "public_mcp_url"));
        if(url && *url)copy_text(ui,url); free(url); return;
    }
    if(id==UI_COPY) { int size=GetWindowTextLengthW(ui->details)+1; wchar_t *text=calloc((size_t)size,sizeof(wchar_t));
        if(text) { GetWindowTextW(ui->details,text,size); copy_text(ui,text); free(text); } return; }
    if(id==UI_DIAGNOSE) {
        if(!strcmp(str(p,"state"),"running"))request(ui,"project-request",true);
        else { char *json=yyjson_val_write(p,YYJSON_WRITE_PRETTY,NULL); set_utf8(ui->details,json); free(json); ui->diagnostic=true; update_buttons(ui); }
        return;
    }
    if(id==UI_STOP || id==UI_PAUSE) {
        wchar_t message[256]; swprintf(message,256,L"%ls会结束所选项目的活动任务，并使当前批准失效。其他项目继续运行。是否继续？",id==UI_PAUSE ? L"暂停" : L"停止");
        if(MessageBoxW(ui->window,message,L"确认项目操作",MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2)!=IDYES)return;
    }
    const char *command=id==UI_START ? "project-start" : id==UI_STOP ? "project-stop" : id==UI_PAUSE ? "project-pause" : id==UI_RESUME ? "project-resume" : NULL;
    if(command)request(ui,command,false);
}
static int px(ui_state *ui, int value) { return MulDiv(value,(int)GetDpiForWindow(ui->window),96); }
static void fonts(ui_state *ui) {
    HFONT previous=ui->font,previous_heading=ui->heading;
    ui->font=CreateFontW(-px(ui,16),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    ui->heading=CreateFontW(-px(ui,24),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    HWND windows[]={ui->list,ui->details,ui->status,ui->path,ui->context};
    for(size_t i=0;i<sizeof(windows)/sizeof(windows[0]);++i)if(windows[i])SendMessageW(windows[i],WM_SETFONT,(WPARAM)ui->font,TRUE);
    for(int i=0;i<11;++i)if(ui->buttons[i])SendMessageW(ui->buttons[i],WM_SETFONT,(WPARAM)ui->font,TRUE);
    if(ui->header)SendMessageW(ui->header,WM_SETFONT,(WPARAM)ui->heading,TRUE);
    if(previous)DeleteObject(previous); if(previous_heading)DeleteObject(previous_heading);
}
static void layout(ui_state *ui) {
    RECT rect; GetClientRect(ui->window,&rect); int w=rect.right,h=rect.bottom,m=px(ui,20),gap=px(ui,10),button=px(ui,116),bh=px(ui,32);
    MoveWindow(ui->header,m,m,w-2*m,px(ui,32),TRUE);
    MoveWindow(ui->path,m,px(ui,56),w-2*m,px(ui,26),TRUE);
    for(int i=0;i<7;++i)MoveWindow(ui->buttons[i],m+i*(button+gap),px(ui,96),button,bh,TRUE);
    for(int i=7;i<11;++i)MoveWindow(ui->buttons[i],m+(i-7)*(button+gap),px(ui,138),button,bh,TRUE);
    MoveWindow(ui->context,m,px(ui,184),w-2*m,px(ui,28),TRUE);
    int top=px(ui,220),bottom=px(ui,56),left=(w-2*m-gap)*48/100;
    MoveWindow(ui->list,m,top,left,h-top-bottom,TRUE);
    MoveWindow(ui->details,m+left+gap,top,w-2*m-left-gap,h-top-bottom,TRUE);
    MoveWindow(ui->status,m,h-px(ui,40),w-2*m,px(ui,28),TRUE);
    int widths[]={left*30/100,left*19/100,left*27/100,left*24/100-4};
    for(int i=0;i<4;++i)ListView_SetColumnWidth(ui->list,i,widths[i]);
}
static HWND control(ui_state *ui, DWORD extended, const wchar_t *class_name, const wchar_t *text, DWORD style, int id) {
    HWND window=CreateWindowExW(extended,class_name,text,WS_CHILD|WS_VISIBLE|style,0,0,0,0,ui->window,(HMENU)(INT_PTR)id,GetModuleHandleW(NULL),NULL);
    SendMessageW(window,WM_SETFONT,(WPARAM)ui->font,TRUE); return window;
}
static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    ui_state *ui=(ui_state *)GetWindowLongPtrW(window,GWLP_USERDATA);
    if(message==WM_NCCREATE) { ui=((CREATESTRUCTW *)lparam)->lpCreateParams; ui->window=window; SetWindowLongPtrW(window,GWLP_USERDATA,(LONG_PTR)ui); }
    if(!ui)return DefWindowProcW(window,message,wparam,lparam);
    switch(message) {
    case WM_CREATE: {
        fonts(ui);
        ui->header=control(ui,0,L"STATIC",L"ShellBot  项目管理",0,UI_HEADER); SendMessageW(ui->header,WM_SETFONT,(WPARAM)ui->heading,TRUE);
        ui->path=control(ui,0,L"EDIT",L"",ES_READONLY|ES_AUTOHSCROLL,UI_PATH); set_utf8(ui->path,ui->data);
        const wchar_t *labels[]={L"刷新",L"启动项目",L"停止项目",L"暂停项目",L"恢复项目",L"复制本机地址",L"复制公网地址",L"查看诊断",L"复制诊断",L"启动管理员",L"停止管理员"};
        for(int i=0;i<11;++i)ui->buttons[i]=control(ui,0,L"BUTTON",labels[i],WS_TABSTOP|BS_PUSHBUTTON,UI_REFRESH+i);
        ui->context=control(ui,0,L"STATIC",L"连接本机管理器…",0,UI_CONTEXT);
        ui->list=control(ui,WS_EX_CLIENTEDGE,WC_LISTVIEWW,L"",WS_TABSTOP|LVS_REPORT|LVS_SINGLESEL|LVS_SHOWSELALWAYS,UI_LIST);
        ListView_SetExtendedListViewStyle(ui->list,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);
        const wchar_t *columns[]={L"项目",L"状态",L"审批方式",L"任务身份"};
        for(int i=0;i<4;++i) { LVCOLUMNW column={0}; column.mask=LVCF_TEXT|LVCF_WIDTH; column.pszText=(wchar_t *)columns[i]; column.cx=100; SendMessageW(ui->list,LVM_INSERTCOLUMNW,(WPARAM)i,(LPARAM)&column); }
        ui->details=control(ui,WS_EX_CLIENTEDGE,L"EDIT",L"",WS_TABSTOP|ES_READONLY|ES_MULTILINE|ES_AUTOVSCROLL|WS_VSCROLL,UI_DETAILS);
        SendMessageW(ui->details,EM_SETLIMITTEXT,SB_RESPONSE_LIMIT,0);
        ui->status=control(ui,0,L"STATIC",L"",0,UI_STATUS); update_buttons(ui); layout(ui);
        PostMessageW(window,WM_COMMAND,UI_REFRESH,0); return 0;
    }
    case WM_COMMAND: if(HIWORD(wparam)==BN_CLICKED)click(ui,LOWORD(wparam)); return 0;
    case WM_NOTIFY: {
        NMHDR *header=(NMHDR *)lparam;
        if(header->idFrom==UI_LIST && header->code==LVN_ITEMCHANGED && !ui->rendering) {
            int index=ListView_GetNextItem(ui->list,-1,LVNI_SELECTED);
            yyjson_val *p=index>=0 ? yyjson_arr_get(yyjson_obj_get(yyjson_doc_get_root(ui->snapshot),"projects"),(size_t)index) : NULL;
            snprintf(ui->selected,sizeof(ui->selected),"%s",str(p,"project_id")); show_project(ui);
        }
        return 0;
    }
    case UI_RESULT: {
        ui_work *work=(ui_work *)lparam; CloseHandle(ui->thread); ui->thread=NULL; ui->busy=false;
        yyjson_doc *reply=work->reply ? yyjson_read(work->reply,strlen(work->reply),0) : NULL;
        yyjson_val *root=yyjson_doc_get_root(reply); const char *error=str(root,"error");
        if(!reply || *error) {
            ui->diagnostic=true; set_utf8(ui->details,reply ? work->reply : work->error.text);
            SetWindowTextW(ui->status,L"请求未完成。请核对诊断与当前状态，再决定后续操作。");
            if(work->snapshot) { yyjson_doc_free(ui->snapshot); ui->snapshot=NULL; *ui->selected=0; ListView_DeleteAllItems(ui->list); SetWindowTextW(ui->context,L"未连接管理器。请先启动对应管理器，再刷新。 "); }
        } else if(work->snapshot) {
            yyjson_doc_free(ui->snapshot); ui->snapshot=reply; reply=NULL; render(ui);
            SetWindowTextW(ui->status,L"状态已刷新。地址不含凭据；暂停状态仅在当前管理器有效。");
        } else if(work->diagnostic) {
            char *json=yyjson_val_write(root,YYJSON_WRITE_PRETTY,NULL); set_utf8(ui->details,json); free(json); ui->diagnostic=true;
            SetWindowTextW(ui->status,L"已查询实际运行能力；诊断可复制。");
        }
        bool refresh=!work->snapshot && !work->diagnostic && reply && !*error;
        yyjson_doc_free(reply); free_work(work); update_buttons(ui);
        if(refresh)request(ui,"management-status",false);
        return 0;
    }
    case WM_SIZE: if(ui->list)layout(ui); return 0;
    case WM_GETMINMAXINFO: ((MINMAXINFO *)lparam)->ptMinTrackSize=(POINT){px(ui,1020),px(ui,660)}; return 0;
    case WM_DPICHANGED: {
        RECT *rect=(RECT *)lparam; SetWindowPos(window,NULL,rect->left,rect->top,rect->right-rect->left,rect->bottom-rect->top,SWP_NOZORDER|SWP_NOACTIVATE);
        fonts(ui); layout(ui); return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    default: return DefWindowProcW(window,message,wparam,lparam);
    }
}
int sb_manager_ui(const char *data_dir) {
    HANDLE token=NULL; TOKEN_ELEVATION elevation={0}; DWORD size=0;
    bool allowed=OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token) && sb_win_owner_token_allowed(token) &&
        GetTokenInformation(token,TokenElevation,&elevation,sizeof(elevation),&size) && !elevation.TokenIsElevated;
    if(token)CloseHandle(token);
    if(!allowed) { fputs("ShellBot: Management window requires an ordinary, unelevated owner token.\n",stderr); return 2; }
    ui_state ui={0}; ui.data=sb_strdup(data_dir); if(!ui.data)return 2;
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX controls={sizeof(controls),ICC_LISTVIEW_CLASSES}; InitCommonControlsEx(&controls);
    WNDCLASSW klass={0}; klass.lpfnWndProc=window_proc; klass.hInstance=GetModuleHandleW(NULL); klass.hCursor=LoadCursorW(NULL,IDC_ARROW);
    klass.hbrBackground=(HBRUSH)(COLOR_WINDOW+1); klass.lpszClassName=L"ShellBotManagerWindow";
    if(!RegisterClassW(&klass) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) { free(ui.data); return 2; }
    RECT area={0}; SystemParametersInfoW(SPI_GETWORKAREA,0,&area,0);
    int dpi=(int)GetDpiForSystem(),width=MulDiv(1160,dpi,96),height=MulDiv(800,dpi,96);
    if(area.right-area.left>0 && width>area.right-area.left-40)width=area.right-area.left-40;
    if(area.bottom-area.top>0 && height>area.bottom-area.top-40)height=area.bottom-area.top-40;
    HWND window=CreateWindowExW(0,klass.lpszClassName,L"ShellBot 项目管理",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,width,height,NULL,NULL,klass.hInstance,&ui);
    if(!window) { free(ui.data); return 2; }
    /* Detach only our GUI process; never hide the caller's terminal window. */
    FreeConsole(); STARTUPINFOW startup={0}; startup.cb=sizeof(startup); GetStartupInfoW(&startup);
    ShowWindow(window,startup.dwFlags&STARTF_USESHOWWINDOW ? startup.wShowWindow : SW_SHOWNORMAL); UpdateWindow(window);
    MSG message; while(GetMessageW(&message,NULL,0,0)>0) { if(!IsDialogMessageW(window,&message)) { TranslateMessage(&message); DispatchMessageW(&message); } }
    /* Work items own their data independently; closing the GUI never stops or
       retries a manager operation. Process exit releases any outstanding item. */
    if(ui.thread)CloseHandle(ui.thread);
    yyjson_doc_free(ui.snapshot); DeleteObject(ui.font); DeleteObject(ui.heading); free(ui.data); return 0;
}
