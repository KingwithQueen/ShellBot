#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "desktop_app.h"
#include "native_ui.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

enum { O_LIST=6100,O_DETAILS,O_ALLOW,O_DENY,O_REVOKE,O_REMOVE,O_REFRESH,O_CLOSE,O_STATUS,O_PERSISTENT };
#define O_RESULT (WM_APP+42)
typedef struct { char id[65];int kind;yyjson_val *value; } row;
typedef struct {
    HWND window,owner,list,details,status;HFONT font;sb_desktop_app *app;
    HANDLE thread;char *request,*reply;sb_error error;yyjson_doc *snapshot;
    row rows[112];size_t count;bool busy,done,closing,background,confirming,stale;
    UINT dpi;char detail_id[65],pending_id[65];int detail_kind,pending_action,pending_kind;
} dialog;
static const char *str(yyjson_val *o,const char *key){const char *v=sb_string(yyjson_obj_get(o,key));return v?v:"";}
static const wchar_t *reason_text(const char *reason){
    if(!strcmp(reason,"refresh_reused"))return L"旧续期凭据被重复使用，已撤销连接";
    if(!strcmp(reason,"owner_revoked"))return L"本机所有者已撤销连接";
    if(!strcmp(reason,"client_revoked"))return L"客户端已撤销连接";
    if(!strcmp(reason,"policy_changed"))return L"限时授权对应的项目权限已变更";
    if(!strcmp(reason,"project_unavailable"))return L"原项目已禁用、删除或凭据已更换";
    if(!strcmp(reason,"authorization_expired"))return L"限时授权已到期";
    if(!strcmp(reason,"scope_mismatch"))return L"客户端请求的续期权限与原授权不同";
    if(!strcmp(reason,"resource_mismatch"))return L"客户端请求的项目地址与原授权不同";
    if(!strcmp(reason,"unknown_client"))return L"客户端登记不存在，请核对数据目录和公网地址";
    if(!strcmp(reason,"authorization_code_invalid"))return L"授权码已过期、已使用或校验不匹配";
    if(!strcmp(reason,"refresh_unknown")||!strcmp(reason,"refresh_invalid"))return L"续期凭据无法识别或校验失败";
    return *reason?L"客户端续期请求未通过校验":L"无";
}
static void detail_text(HWND control,const wchar_t *text){
    int length=GetWindowTextLengthW(control);wchar_t *old=calloc((size_t)length+1,sizeof(wchar_t));
    if(old){GetWindowTextW(control,old,length+1);bool same=!wcscmp(old,text);free(old);if(same)return;}
    DWORD start=0,end=0;SendMessageW(control,EM_GETSEL,(WPARAM)&start,(LPARAM)&end);LRESULT first=SendMessageW(control,EM_GETFIRSTVISIBLELINE,0,0);
    bool visible=IsWindowVisible(control)!=FALSE;if(visible)SendMessageW(control,WM_SETREDRAW,FALSE,0);
    SetWindowTextW(control,text);DWORD size=(DWORD)wcslen(text);SendMessageW(control,EM_SETSEL,start>size?size:start,end>size?size:end);
    SendMessageW(control,EM_LINESCROLL,0,first-SendMessageW(control,EM_GETFIRSTVISIBLELINE,0,0));
    if(visible)SendMessageW(control,WM_SETREDRAW,TRUE,0);InvalidateRect(control,NULL,TRUE);
}
static int px(HWND h,int n){return MulDiv(n,(int)GetDpiForWindow(h),96);}
static HWND child(dialog *d,const wchar_t *cls,const wchar_t *text,DWORD style,int id,int x,int y,int w,int h){HWND c=CreateWindowExW(!wcscmp(cls,L"EDIT")?WS_EX_CLIENTEDGE:0,cls,text,WS_CHILD|WS_VISIBLE|style,px(d->window,x),px(d->window,y),px(d->window,w),px(d->window,h),d->window,(HMENU)(INT_PTR)id,GetModuleHandleW(NULL),NULL);SendMessageW(c,WM_SETFONT,(WPARAM)d->font,0);return c;}
static void show_row(dialog *d){LRESULT index=SendMessageW(d->list,LB_GETCURSEL,0,0);row *r=index>=0&&(size_t)index<d->count?&d->rows[index]:NULL;
    bool idle=!d->busy||d->background,ready=idle&&!d->stale;int ids[]={O_ALLOW,O_DENY,O_REVOKE,O_REMOVE,O_REFRESH,O_PERSISTENT};
    bool states[]={ready&&r&&r->kind==0,ready&&r&&r->kind==0,ready&&r&&r->kind==1,ready&&r&&r->kind==2,idle,ready&&r&&r->kind==0};
    for(int i=0;i<6;++i){HWND h=GetDlgItem(d->window,ids[i]);if((IsWindowEnabled(h)!=FALSE)!=states[i])EnableWindow(h,states[i]);}
    if(strcmp(d->detail_id,r?r->id:"")||d->detail_kind!=(r?r->kind:-1)){
        snprintf(d->detail_id,sizeof(d->detail_id),"%s",r?r->id:"");d->detail_kind=r?r->kind:-1;SetWindowTextW(d->details,L"");
    }
    if(!r){detail_text(d->details,L"等待客户端发起 OAuth 连接。\r\n在客户端填写项目公网 MCP 地址，认证选择 OAuth；此处会显示待确认申请。\r\n\r\n长期授权首次确认后自动续期，直到手动撤销。取消勾选可使用 30 天限时授权。");return;}
    wchar_t *name=sb_desktop_wide(str(r->value,"client_name")),*project=sb_desktop_wide(str(r->value,"project")),*redirect=sb_desktop_wide(str(r->value,"redirect_uri")),*status=sb_desktop_wide(str(r->value,"status"));wchar_t text[2400];
    if(r->kind==0)swprintf(text,2400,L"待确认的连接申请\r\n\r\n申请编号：%.8hs\r\n项目：%ls\r\n客户端名称：%ls\r\n回调地址：%ls\r\n\r\n请与浏览器页面的申请编号核对。客户端名称由对方提供，不能作为身份凭证。\r\n允许后，该客户端可按项目当前权限访问 MCP；文件、任务和管理员审批规则继续生效。\r\n\r\n%ls\r\n请选择“允许此连接”或“拒绝”。",r->id,project?project:L"",name?name:L"",redirect?redirect:L"",SendMessageW(GetDlgItem(d->window,O_PERSISTENT),BM_GETCHECK,0,0)==BST_CHECKED?L"长期授权：重启及调整权限后继续有效，包括之后新增的项目权限。可随时撤销；删除项目或更换项目凭据后需重新授权。":L"限时授权：自动续期最长 30 天；项目权限变更后需重新授权。");
    else if(r->kind==1)swprintf(text,2400,L"已授予的项目连接\r\n\r\n项目：%ls\r\n客户端名称：%ls\r\n状态：%ls\r\n授权方式：%ls\r\n撤销原因：%ls\r\n\r\n撤销会立即禁止该连接使用现有访问和续期凭据，不停止项目，也不改变其他客户端。\r\n%ls",project?project:L"",name?name:L"",!strcmp(str(r->value,"status"),"active")?L"有效":!strcmp(str(r->value,"status"),"revoked")?L"已撤销":!strcmp(str(r->value,"status"),"expired")?L"已到期":!strcmp(str(r->value,"status"),"project_unavailable")?L"原项目不可用或凭据已更换":L"项目权限已变更，需要重新授权",yyjson_get_bool(yyjson_obj_get(r->value,"persistent"))?L"长期，直到手动撤销":L"30 天限时授权",reason_text(str(r->value,"revocation_reason")),yyjson_get_bool(yyjson_obj_get(r->value,"persistent"))?L"连接按项目当前权限执行，包括之后新增的权限。客户端自动续期，访问令牌每次有效 15 分钟。":L"项目权限变更或授权到期后，请从客户端重新连接。");
    else swprintf(text,2400,L"已登记的 OAuth 客户端\r\n\r\n客户端名称：%ls\r\n回调地址：%ls\r\n\r\n登记不代表已授权。客户端名称由对方提供。\r\n移除客户端会撤销其全部项目连接，取消待确认申请；下次连接需重新登记和授权。",name?name:L"",redirect?redirect:L"");
    detail_text(d->details,text);free(name);free(project);free(redirect);free(status);
}
static void render(dialog *d,yyjson_doc *snapshot){
    LRESULT selected=SendMessageW(d->list,LB_GETCURSEL,0,0);char keep[65]={0};int kind=-1;if(selected>=0&&(size_t)selected<d->count){strcpy(keep,d->rows[selected].id);kind=d->rows[selected].kind;}
    row rows[112];wchar_t labels[112][300];size_t count=0;bool changed=false;
    yyjson_val *root=yyjson_doc_get_root(snapshot);const char *keys[]={"pending","connections","clients"};int restore=-1;
    for(int type=0;type<3;++type){yyjson_val *array=yyjson_obj_get(root,keys[type]);for(size_t i=0;i<yyjson_arr_size(array)&&count<112;++i){
        yyjson_val *v=yyjson_arr_get(array,i);row *r=&rows[count];snprintf(r->id,sizeof(r->id),"%s",str(v,"id"));r->kind=type;r->value=v;
        wchar_t *name=sb_desktop_wide(str(v,"client_name")),*project=sb_desktop_wide(str(v,"project"));
        swprintf(labels[count],300,L"%ls  ·  %ls  ·  %ls",type==0?L"待确认":type==1?L"已授权":L"客户端",name?name:L"",type==2?L"登记信息":project?project:L"");free(name);free(project);
        wchar_t previous[300]={0};LRESULT size=SendMessageW(d->list,LB_GETTEXTLEN,(WPARAM)count,0);
        if(size>=0 && size<300)SendMessageW(d->list,LB_GETTEXT,(WPARAM)count,(LPARAM)previous);
        if(count>=d->count||d->rows[count].kind!=type||strcmp(d->rows[count].id,r->id)||wcscmp(previous,labels[count]))changed=true;
        if(kind==type&&!strcmp(keep,r->id))restore=(int)count;++count;
    }}
    changed=changed||count!=d->count;
    yyjson_doc_free(d->snapshot);d->snapshot=snapshot;memcpy(d->rows,rows,count*sizeof(row));d->count=count;
    if(restore<0&&count)restore=0;
    if(changed){LRESULT top=SendMessageW(d->list,LB_GETTOPINDEX,0,0);bool visible=IsWindowVisible(d->list)!=FALSE;if(visible)SendMessageW(d->list,WM_SETREDRAW,FALSE,0);
        SendMessageW(d->list,LB_RESETCONTENT,0,0);for(size_t i=0;i<count;++i)SendMessageW(d->list,LB_ADDSTRING,0,(LPARAM)labels[i]);
        SendMessageW(d->list,LB_SETCURSEL,restore,0);if(count)SendMessageW(d->list,LB_SETTOPINDEX,top<(LRESULT)count?top:(LRESULT)count-1,0);
        if(visible)SendMessageW(d->list,WM_SETREDRAW,TRUE,0);InvalidateRect(d->list,NULL,TRUE);
    }
    wchar_t diagnostic[500];swprintf(diagnostic,500,L"OAuth 已启用。最近认证失败：%ls（新成功连接不会清除此历史记录）。",reason_text(str(root,"last_failure")));
    detail_text(d->status,!yyjson_get_bool(yyjson_obj_get(root,"enabled"))?L"请先在“连接设置”填写 HTTPS 公网地址，OAuth 将自动启用。":!yyjson_get_bool(yyjson_obj_get(root,"storage_available"))?L"凭据存储不可用；请保留原文件并检查诊断。":*str(root,"last_failure")?diagnostic:L"OAuth 已启用。长期授权可自动续期至手动撤销；原有限时授权规则保留。");show_row(d);
}
static DWORD WINAPI exchange(void *context){dialog *d=context;d->reply=sb_desktop_exchange(d->app,d->request,&d->error);PostMessageW(d->window,O_RESULT,0,0);return 0;}
static void request(dialog *d,int action){
    if(d->closing||d->confirming)return;
    if(action!=O_REFRESH && d->stale)return;
    LRESULT index=SendMessageW(d->list,LB_GETCURSEL,0,0);row *r=index>=0&&(size_t)index<d->count?&d->rows[index]:NULL;
    if(d->busy){if(d->background&&action!=O_REFRESH&&r){d->pending_action=action;d->pending_kind=r->kind;strcpy(d->pending_id,r->id);}return;}
    const char *command=action==O_ALLOW?"oauth-approve":action==O_DENY?"oauth-deny":action==O_REVOKE?"oauth-revoke":action==O_REMOVE?"oauth-remove-client":"oauth-status";if(action!=O_REFRESH&&!r)return;
    if(action==O_REVOKE||action==O_REMOVE){d->confirming=true;int choice=MessageBoxW(d->window,action==O_REVOKE?L"撤销所选客户端的项目连接？":L"移除此客户端并撤销其所有项目连接？",L"确认客户端授权操作",MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2);d->confirming=false;if(d->closing){DestroyWindow(d->window);return;}if(choice!=IDYES)return;}
    yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL);if(!doc)return;yyjson_mut_val *root=yyjson_mut_obj(doc);yyjson_mut_doc_set_root(doc,root);yyjson_mut_obj_add_str(doc,root,"command",command);if(action!=O_REFRESH)yyjson_mut_obj_add_str(doc,root,"id",r->id);if(action==O_ALLOW)yyjson_mut_obj_add_bool(doc,root,"persistent",SendMessageW(GetDlgItem(d->window,O_PERSISTENT),BM_GETCHECK,0,0)==BST_CHECKED);free(d->request);d->request=yyjson_mut_write(doc,0,NULL);yyjson_mut_doc_free(doc);memset(&d->error,0,sizeof(d->error));if(!d->request)return;d->busy=true;d->background=action==O_REFRESH;show_row(d);d->thread=CreateThread(NULL,0,exchange,d,0,NULL);if(!d->thread){d->busy=false;show_row(d);SetWindowTextW(d->status,L"无法创建本机请求线程。");}
}
static LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wp,LPARAM lp){dialog *d=(dialog *)GetWindowLongPtrW(window,GWLP_USERDATA);if(message==WM_NCCREATE){d=((CREATESTRUCTW *)lp)->lpCreateParams;d->window=window;SetWindowLongPtrW(window,GWLP_USERDATA,(LONG_PTR)d);}if(!d)return DefWindowProcW(window,message,wp,lp);
    switch(message){
    case DM_GETDEFID:return MAKELRESULT(O_CLOSE,DC_HASDEFID);
    case WM_CREATE:
        d->font=sb_native_font(window);d->dpi=sb_native_dpi(window);d->detail_kind=-1;child(d,L"STATIC",L"客户端授权 · 在此确认连接、查看和撤销项目访问",0,0,18,18,850,26);
        d->list=child(d,L"LISTBOX",L"",WS_BORDER|WS_VSCROLL|WS_TABSTOP|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT,O_LIST,18,56,330,370);d->details=child(d,L"EDIT",L"",WS_TABSTOP|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|WS_VSCROLL,O_DETAILS,362,56,510,370);
        HWND permanent=child(d,L"BUTTON",L"长期授权，直到手动撤销（取消勾选为 30 天限时授权）",WS_TABSTOP|BS_AUTOCHECKBOX,O_PERSISTENT,18,436,854,28);SendMessageW(permanent,BM_SETCHECK,BST_CHECKED,0);
        child(d,L"BUTTON",L"允许此连接",WS_TABSTOP,O_ALLOW,18,474,142,34);child(d,L"BUTTON",L"拒绝",WS_TABSTOP,O_DENY,174,474,110,34);child(d,L"BUTTON",L"撤销连接",WS_TABSTOP,O_REVOKE,298,474,142,34);child(d,L"BUTTON",L"移除客户端",WS_TABSTOP,O_REMOVE,454,474,142,34);child(d,L"BUTTON",L"刷新",WS_TABSTOP,O_REFRESH,610,474,110,34);child(d,L"BUTTON",L"关闭",WS_TABSTOP|BS_DEFPUSHBUTTON,O_CLOSE,734,474,138,34);d->status=child(d,L"STATIC",L"正在读取授权状态…",0,O_STATUS,18,524,854,48);SetTimer(window,1,1500,NULL);request(d,O_REFRESH);return 0;
    case WM_COMMAND:if((LOWORD(wp)==O_LIST&&HIWORD(wp)==LBN_SELCHANGE)||LOWORD(wp)==O_PERSISTENT)show_row(d);else if(LOWORD(wp)==O_CLOSE||LOWORD(wp)==IDOK||LOWORD(wp)==IDCANCEL){d->closing=true;if(!d->busy&&!d->confirming)DestroyWindow(window);}else if(HIWORD(wp)==BN_CLICKED)request(d,LOWORD(wp));return 0;
    case O_RESULT:{WaitForSingleObject(d->thread,INFINITE);CloseHandle(d->thread);d->thread=NULL;d->busy=false;yyjson_doc *doc=d->reply?yyjson_read(d->reply,strlen(d->reply),0):NULL;const char *failure=str(yyjson_doc_get_root(doc),"error");bool success=doc&&!*failure;d->stale=!success;if(success){char *old=d->snapshot?yyjson_val_write(yyjson_doc_get_root(d->snapshot),0,NULL):NULL;if(!old||strcmp(old,d->reply)){render(d,doc);doc=NULL;}else show_row(d);free(old);}else{wchar_t *error=sb_desktop_wide(*failure?failure:d->error.text);SetWindowTextW(d->status,error?error:L"本机请求未完成，请刷新后核对。");free(error);show_row(d);}yyjson_doc_free(doc);free(d->reply);d->reply=NULL;if(d->closing)DestroyWindow(window);
        else if(d->pending_action){int action=d->pending_action;d->pending_action=0;LRESULT index=SendMessageW(d->list,LB_GETCURSEL,0,0);row *r=index>=0&&(size_t)index<d->count?&d->rows[index]:NULL;if(success&&r&&r->kind==d->pending_kind&&!strcmp(r->id,d->pending_id))request(d,action);else detail_text(d->status,L"所选连接已变化，请核对当前申请后重新操作。");}return 0;}
    case WM_TIMER:if(WaitForSingleObject(sb_desktop_stop(d->app),0)==WAIT_OBJECT_0){d->closing=true;if(!d->busy&&!d->confirming)DestroyWindow(window);}else request(d,O_REFRESH);return 0;
    case WM_DPICHANGED:sb_native_dpi_changed(window,&d->font,&d->dpi,wp,lp);return 0;
    case WM_CLOSE:d->closing=true;if(!d->busy&&!d->confirming)DestroyWindow(window);return 0;
    case WM_DESTROY:KillTimer(window,1);d->done=true;return 0;
    default:return DefWindowProcW(window,message,wp,lp);
    }
}
void sb_desktop_oauth_dialog(HWND owner,sb_desktop_app *app){dialog *d=calloc(1,sizeof(*d));if(!d)return;d->owner=owner;d->app=app;WNDCLASSW wc={0};wc.lpfnWndProc=procedure;wc.hInstance=GetModuleHandleW(NULL);wc.lpszClassName=L"ShellBotOAuthDialog";wc.hIcon=sb_app_icon();wc.hCursor=LoadCursorW(NULL,IDC_ARROW);wc.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1);RegisterClassW(&wc);RECT rect={0,0,px(owner,890),px(owner,584)};AdjustWindowRectExForDpi(&rect,WS_CAPTION|WS_SYSMENU,FALSE,WS_EX_DLGMODALFRAME,sb_native_dpi(owner));RECT position={0};GetWindowRect(owner,&position);HWND h=CreateWindowExW(WS_EX_DLGMODALFRAME,wc.lpszClassName,L"ShellBot 客户端授权",WS_CAPTION|WS_SYSMENU|WS_CLIPCHILDREN,position.left,position.top,rect.right-rect.left,rect.bottom-rect.top,owner,NULL,wc.hInstance,d);if(h){EnableWindow(owner,FALSE);ShowWindow(h,SW_SHOW);sb_native_modal_loop(h,&d->done);EnableWindow(owner,TRUE);SetActiveWindow(owner);}if(d->thread){if(WaitForSingleObject(d->thread,30000)!=WAIT_OBJECT_0)ExitProcess(3);CloseHandle(d->thread);}yyjson_doc_free(d->snapshot);free(d->request);free(d->reply);if(d->font)DeleteObject(d->font);free(d);}
