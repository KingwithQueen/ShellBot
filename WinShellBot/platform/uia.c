#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <wincrypt.h>
#include <uiautomation.h>
#include "uia.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

typedef struct {
    IUIAutomation2 *automation;IUIAutomationTreeWalker *walker;
    yyjson_mut_doc *doc;yyjson_mut_val *nodes;yyjson_val *request,*wanted;
    IUIAutomationElement *found;DWORD pid;HWND window;FILETIME created;LONG epoch;
    unsigned count,maximum,depth;size_t bytes;uint64_t deadline;bool truncated,include_text,require_foreground;
    HRESULT failure;
} uia_query;
static char *encode(BSTR value,size_t limit){
    if(!value)return sb_strdup("");UINT length=SysStringLen(value);if(length>limit)length=(UINT)limit;
    if(wmemchr(value,0,length))return NULL;
    if(length && value[length-1]>=0xd800 && value[length-1]<=0xdbff)--length;
    int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value,(int)length,NULL,0,NULL,NULL);
    if(length && !n)return NULL;
    char *text=malloc((size_t)n+1);if(!text)return NULL;
    if(n && !WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value,(int)length,text,n,NULL,NULL)){free(text);return NULL;}text[n]=0;return text;
}
static wchar_t *decode(const char *text){int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text,-1,NULL,0);wchar_t *value=n?malloc((size_t)n*sizeof(wchar_t)):NULL;if(value)MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text,-1,value,n);return value;}
static wchar_t *request_value(yyjson_val *request){
    const char *text=sb_string(yyjson_obj_get(request,"value_base64"));if(!text || strlen(text)>21848)return NULL;if(!*text)return decode("");
    DWORD bytes=0;DWORD flags=CRYPT_STRING_BASE64|CRYPT_STRING_STRICT;
    if(!CryptStringToBinaryA(text,0,flags,NULL,&bytes,NULL,NULL) || bytes>16384)return NULL;
    unsigned char *raw=malloc((size_t)bytes+1);if(!raw)return NULL;
    if(!CryptStringToBinaryA(text,0,flags,raw,&bytes,NULL,NULL) || memchr(raw,0,bytes)){free(raw);return NULL;}raw[bytes]=0;
    wchar_t *value=decode((const char*)raw);free(raw);return value;
}
static yyjson_mut_val *runtime_id(uia_query *q,IUIAutomationElement *element){
    SAFEARRAY *array=NULL;if(FAILED(IUIAutomationElement_GetRuntimeId(element,&array)) || !array)return NULL;
    LONG low=0,high=-1;SafeArrayGetLBound(array,1,&low);SafeArrayGetUBound(array,1,&high);
    yyjson_mut_val *id=NULL;
    if(SafeArrayGetDim(array)==1 && high>=low && high-low<128){id=yyjson_mut_arr(q->doc);for(LONG i=low;i<=high;++i){LONG value=0;if(FAILED(SafeArrayGetElement(array,&i,&value))){id=NULL;break;}yyjson_mut_arr_add_int(q->doc,id,value);}}
    SafeArrayDestroy(array);return id;
}
static bool same_runtime(yyjson_mut_val *actual,yyjson_val *wanted){
    if(!actual || !yyjson_is_arr(wanted) || yyjson_mut_arr_size(actual)!=yyjson_arr_size(wanted))return false;
    for(size_t i=0;i<yyjson_arr_size(wanted);++i)if(yyjson_mut_get_sint(yyjson_mut_arr_get(actual,i))!=yyjson_get_sint(yyjson_arr_get(wanted,i)))return false;return true;
}
static char *property_text(IUIAutomationElement *element,PROPERTYID property,size_t limit){
    VARIANT v;VariantInit(&v);HRESULT hr=IUIAutomationElement_GetCurrentPropertyValue(element,property,&v);
    char *value=SUCCEEDED(hr) && V_VT(&v)==VT_BSTR?encode(V_BSTR(&v),limit):sb_strdup("");VariantClear(&v);return value;
}
static bool snapshot(uia_query *q,IUIAutomationElement *element,unsigned depth){
    int pid=0,type=0;BOOL enabled=FALSE,password=TRUE,offscreen=TRUE;RECT bounds={0};
    if(FAILED(IUIAutomationElement_get_CurrentProcessId(element,&pid)) || (DWORD)pid!=q->pid)return false;
    IUIAutomationElement_get_CurrentControlType(element,&type);
    IUIAutomationElement_get_CurrentIsEnabled(element,&enabled);IUIAutomationElement_get_CurrentIsPassword(element,&password);IUIAutomationElement_get_CurrentIsOffscreen(element,&offscreen);IUIAutomationElement_get_CurrentBoundingRectangle(element,&bounds);
    yyjson_mut_val *node=yyjson_mut_obj(q->doc),*id=runtime_id(q,element);if(!node || !id)return false;
    yyjson_mut_obj_add_val(q->doc,node,"runtime_id",id);yyjson_mut_obj_add_uint(q->doc,node,"depth",depth);
    yyjson_mut_obj_add_uint(q->doc,node,"process_id",q->pid);yyjson_mut_obj_add_int(q->doc,node,"control_type",type);
    yyjson_mut_obj_add_bool(q->doc,node,"enabled",enabled!=0);yyjson_mut_obj_add_bool(q->doc,node,"password",password!=0);yyjson_mut_obj_add_bool(q->doc,node,"offscreen",offscreen!=0);
    const PROPERTYID properties[]={UIA_NamePropertyId,UIA_AutomationIdPropertyId,UIA_ClassNamePropertyId};const char *names[]={"name","automation_id","class_name"};
    for(unsigned i=0;i<3;++i){char *text=password && !i?sb_strdup(""):property_text(element,properties[i],1024);if(!text){q->failure=E_OUTOFMEMORY;return false;}q->bytes+=strlen(text);yyjson_mut_obj_add_strcpy(q->doc,node,names[i],text);free(text);}
    yyjson_mut_val *rect=yyjson_mut_obj(q->doc);yyjson_mut_obj_add_int(q->doc,rect,"x",bounds.left);yyjson_mut_obj_add_int(q->doc,rect,"y",bounds.top);yyjson_mut_obj_add_int(q->doc,rect,"width",(int64_t)bounds.right-bounds.left);yyjson_mut_obj_add_int(q->doc,rect,"height",(int64_t)bounds.bottom-bounds.top);yyjson_mut_obj_add_val(q->doc,node,"bounds",rect);
    if(q->include_text && !password){
        IUIAutomationValuePattern *value=NULL;BSTR content=NULL;
        if(SUCCEEDED(IUIAutomationElement_GetCurrentPatternAs(element,UIA_ValuePatternId,&IID_IUIAutomationValuePattern,(void**)&value)) && value){IUIAutomationValuePattern_get_CurrentValue(value,&content);IUIAutomationValuePattern_Release(value);}
        if(!content){IUIAutomationTextPattern *text=NULL;IUIAutomationTextRange *range=NULL;if(SUCCEEDED(IUIAutomationElement_GetCurrentPatternAs(element,UIA_TextPatternId,&IID_IUIAutomationTextPattern,(void**)&text)) && text){if(SUCCEEDED(IUIAutomationTextPattern_get_DocumentRange(text,&range)) && range){IUIAutomationTextRange_GetText(range,4096,&content);IUIAutomationTextRange_Release(range);}IUIAutomationTextPattern_Release(text);}}
        if(content){char *text=encode(content,4096);SysFreeString(content);if(text){q->bytes+=strlen(text);yyjson_mut_obj_add_strcpy(q->doc,node,"text",text);yyjson_mut_obj_add_bool(q->doc,node,"text_may_be_truncated",true);free(text);}}
    }
    yyjson_mut_val *patterns=yyjson_mut_arr(q->doc);
    const PROPERTYID available[]={UIA_IsInvokePatternAvailablePropertyId,UIA_IsValuePatternAvailablePropertyId,UIA_IsTogglePatternAvailablePropertyId,UIA_IsSelectionItemPatternAvailablePropertyId,UIA_IsExpandCollapsePatternAvailablePropertyId};const char *pattern_names[]={"invoke","set_value","toggle","select","expand_collapse"};
    for(unsigned i=0;i<5;++i){VARIANT v;VariantInit(&v);if(SUCCEEDED(IUIAutomationElement_GetCurrentPropertyValue(element,available[i],&v)) && V_VT(&v)==VT_BOOL && V_BOOL(&v))yyjson_mut_arr_add_str(q->doc,patterns,pattern_names[i]);VariantClear(&v);}yyjson_mut_obj_add_val(q->doc,node,"patterns",patterns);
    char *encoded=yyjson_mut_val_write(node,0,NULL);if(!encoded){q->failure=E_OUTOFMEMORY;return false;}q->bytes+=strlen(encoded);free(encoded);
    yyjson_mut_arr_add_val(q->nodes,node);++q->count;return true;
}
static void walk(uia_query *q,IUIAutomationElement *element,unsigned depth){
    if(q->found || q->count>=q->maximum || depth>q->depth || sb_platform_millis()>=q->deadline || q->bytes>262144){q->truncated=true;return;}
    yyjson_mut_val *id=q->wanted?runtime_id(q,element):NULL;
    if(q->wanted){++q->count;if(same_runtime(id,yyjson_obj_get(q->wanted,"runtime_id"))){q->found=element;IUIAutomationElement_AddRef(element);return;}}
    else if(!snapshot(q,element,depth)){q->truncated=true;return;}
    IUIAutomationElement *child=NULL;HRESULT hr=IUIAutomationTreeWalker_GetFirstChildElement(q->walker,element,&child);
    if(FAILED(hr)){q->truncated=true;q->failure=hr;return;}
    while(child){
        walk(q,child,depth+1);IUIAutomationElement *next=NULL;
        if(q->found || q->count>=q->maximum || sb_platform_millis()>=q->deadline || q->bytes>262144){IUIAutomationElement_Release(child);if(!q->found)q->truncated=true;break;}
        hr=IUIAutomationTreeWalker_GetNextSiblingElement(q->walker,child,&next);IUIAutomationElement_Release(child);child=next;
        if(FAILED(hr)){q->truncated=true;q->failure=hr;break;}
    }
}
static const char *act(uia_query *q,HRESULT *api,bool *attempted){
    IUIAutomationElement *e=q->found;if(!e)return "uia_element_stale_or_outside_bounded_tree";
    int pid=0,type=0;BOOL password=TRUE,enabled=FALSE,offscreen=TRUE;
    IUIAutomationElement_get_CurrentProcessId(e,&pid);IUIAutomationElement_get_CurrentControlType(e,&type);IUIAutomationElement_get_CurrentIsPassword(e,&password);IUIAutomationElement_get_CurrentIsEnabled(e,&enabled);IUIAutomationElement_get_CurrentIsOffscreen(e,&offscreen);
    if((DWORD)pid!=q->pid || type!=yyjson_get_int(yyjson_obj_get(q->wanted,"control_type")))return "uia_element_identity_changed";
    if(password || !enabled || offscreen)return "uia_element_password_disabled_or_offscreen";
    const PROPERTYID properties[]={UIA_NamePropertyId,UIA_AutomationIdPropertyId,UIA_ClassNamePropertyId};const char *names[]={"name","automation_id","class_name"};
    for(unsigned i=0;i<3;++i){char *text=property_text(e,properties[i],1024);const char *old=sb_string(yyjson_obj_get(q->wanted,names[i]));bool same=text && old && !strcmp(text,old);free(text);if(!same)return "uia_element_identity_changed";}
    const char *action=sb_string(yyjson_obj_get(q->request,"action"));void *pattern=NULL;IID iid;PATTERNID id=0;
    if(!strcmp(action,"invoke")){iid=IID_IUIAutomationInvokePattern;id=UIA_InvokePatternId;}
    else if(!strcmp(action,"set_value")){iid=IID_IUIAutomationValuePattern;id=UIA_ValuePatternId;}
    else if(!strcmp(action,"toggle")){iid=IID_IUIAutomationTogglePattern;id=UIA_TogglePatternId;}
    else if(!strcmp(action,"select")){iid=IID_IUIAutomationSelectionItemPattern;id=UIA_SelectionItemPatternId;}
    else if(!strcmp(action,"expand") || !strcmp(action,"collapse")){iid=IID_IUIAutomationExpandCollapsePattern;id=UIA_ExpandCollapsePatternId;}
    else return "uia_action_unsupported";
    *api=IUIAutomationElement_GetCurrentPatternAs(e,id,&iid,&pattern);if(FAILED(*api) || !pattern)return "uia_pattern_unavailable";
    BOOL readonly=FALSE;if(id==UIA_ValuePatternId){*api=IUIAutomationValuePattern_get_CurrentIsReadOnly((IUIAutomationValuePattern*)pattern,&readonly);if(FAILED(*api) || readonly){IUnknown_Release((IUnknown*)pattern);return "uia_value_read_only_or_unknown";}}
    if(!sb_win_uia_guard(q->window,q->pid,q->created,q->epoch,q->deadline,true,q->require_foreground)){IUnknown_Release((IUnknown*)pattern);return "uia_input_guard_changed";}
    *attempted=true;
    if(id==UIA_InvokePatternId)*api=IUIAutomationInvokePattern_Invoke((IUIAutomationInvokePattern*)pattern);
    else if(id==UIA_ValuePatternId){wchar_t *value=request_value(q->request);if(!value){*attempted=false;*api=E_INVALIDARG;}else{*api=IUIAutomationValuePattern_SetValue((IUIAutomationValuePattern*)pattern,value);free(value);}}
    else if(id==UIA_TogglePatternId)*api=IUIAutomationTogglePattern_Toggle((IUIAutomationTogglePattern*)pattern);
    else if(id==UIA_SelectionItemPatternId)*api=IUIAutomationSelectionItemPattern_Select((IUIAutomationSelectionItemPattern*)pattern);
    else *api=!strcmp(action,"expand")?IUIAutomationExpandCollapsePattern_Expand((IUIAutomationExpandCollapsePattern*)pattern):IUIAutomationExpandCollapsePattern_Collapse((IUIAutomationExpandCollapsePattern*)pattern);
    IUnknown_Release((IUnknown*)pattern);return FAILED(*api)?"uia_pattern_call_failed_effect_unknown":NULL;
}
static char *query(yyjson_val *request){
    uia_query q={0};q.request=request;q.wanted=yyjson_obj_get(request,"element");q.maximum=(unsigned)yyjson_get_uint(yyjson_obj_get(request,"max_nodes"));q.depth=(unsigned)yyjson_get_uint(yyjson_obj_get(request,"max_depth"));q.pid=(DWORD)yyjson_get_uint(yyjson_obj_get(request,"pid"));q.window=(HWND)(uintptr_t)yyjson_get_uint(yyjson_obj_get(request,"hwnd"));q.created.dwLowDateTime=(DWORD)yyjson_get_uint(yyjson_obj_get(request,"created_low"));q.created.dwHighDateTime=(DWORD)yyjson_get_uint(yyjson_obj_get(request,"created_high"));q.epoch=(LONG)yyjson_get_sint(yyjson_obj_get(request,"epoch"));q.deadline=yyjson_get_uint(yyjson_obj_get(request,"deadline"));q.include_text=yyjson_get_bool(yyjson_obj_get(request,"include_text"));
    q.require_foreground=!yyjson_is_false(yyjson_obj_get(request,"require_foreground"));
    q.doc=yyjson_mut_doc_new(NULL);if(!q.doc)return NULL;yyjson_mut_val *result=yyjson_mut_obj(q.doc);q.nodes=yyjson_mut_arr(q.doc);yyjson_mut_obj_add_val(q.doc,result,"nodes",q.nodes);
    const char *error=NULL;HRESULT hr=E_FAIL;bool attempted=false;IUIAutomationElement *root=NULL;
    if(!q.maximum || q.maximum>1000 || q.depth>32 || !q.deadline || sb_platform_millis()>=q.deadline || !sb_win_uia_guard(q.window,q.pid,q.created,q.epoch,q.deadline,q.wanted!=NULL,q.require_foreground)){error="uia_window_or_input_guard_unavailable";goto done;}
    hr=CoCreateInstance(&CLSID_CUIAutomation8,NULL,CLSCTX_INPROC_SERVER,&IID_IUIAutomation2,(void**)&q.automation);if(FAILED(hr)){error="uia_provider_unavailable";goto done;}
    hr=IUIAutomation2_put_ConnectionTimeout(q.automation,500);if(SUCCEEDED(hr))hr=IUIAutomation2_put_TransactionTimeout(q.automation,500);if(SUCCEEDED(hr))hr=IUIAutomation2_put_AutoSetFocus(q.automation,FALSE);
    if(FAILED(hr)){error="uia_provider_configuration_failed";goto done;}
    hr=IUIAutomation2_ElementFromHandle(q.automation,q.window,&root);if(FAILED(hr) || !root){error="uia_root_unavailable";goto done;}
    hr=IUIAutomation2_get_ControlViewWalker(q.automation,&q.walker);if(FAILED(hr) || !q.walker){error="uia_tree_walker_unavailable";goto done;}
    walk(&q,root,0);if(q.wanted){const char *action=sb_string(yyjson_obj_get(request,"action"));error=action?act(&q,&hr,&attempted):"uia_action_missing";}
done:
    if(error)yyjson_mut_obj_add_str(q.doc,result,"error",error);
    yyjson_mut_obj_add_bool(q.doc,result,"truncated",q.truncated);yyjson_mut_obj_add_uint(q.doc,result,"visited_nodes",q.count);yyjson_mut_obj_add_uint(q.doc,result,"hresult",(uint32_t)hr);
    yyjson_mut_obj_add_str(q.doc,result,"coordinate_space","physical_desktop_pixels");
    if(q.wanted){yyjson_mut_obj_add_bool(q.doc,result,"api_attempted",attempted);yyjson_mut_obj_add_str(q.doc,result,"action_state",attempted?SUCCEEDED(hr)?"dispatched":"unknown":"not_attempted");yyjson_mut_obj_add_bool(q.doc,result,"application_result_verified",false);}
    if(FAILED(q.failure))yyjson_mut_obj_add_uint(q.doc,result,"provider_hresult",(uint32_t)q.failure);
    if(q.found)IUIAutomationElement_Release(q.found);if(q.walker)IUIAutomationTreeWalker_Release(q.walker);if(root)IUIAutomationElement_Release(root);if(q.automation)IUIAutomation2_Release(q.automation);
    char *wire=yyjson_mut_val_write(result,0,NULL);yyjson_mut_doc_free(q.doc);return wire;
}
int sb_win_uia_worker(void){
    /* UIA 与桌面线程统一物理像素，防止 RDP/DPI 缩放导致控件边界被虚拟化。 */
    DPI_AWARENESS_CONTEXT previous=SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);if(!previous)return 2;
    char input[65536];DWORD got=0;size_t used=0;HANDLE in=GetStdHandle(STD_INPUT_HANDLE);
    while(used<sizeof(input)-1 && ReadFile(in,input+used,(DWORD)(sizeof(input)-1-used),&got,NULL) && got)used+=got;
    if(!used || used>=sizeof(input)-1)return 2;input[used]=0;yyjson_doc *doc=yyjson_read(input,used,0);if(!doc)return 2;
    HRESULT hr=CoInitializeEx(NULL,COINIT_MULTITHREADED);if(FAILED(hr)){yyjson_doc_free(doc);return 2;}
    char *wire=query(yyjson_doc_get_root(doc));yyjson_doc_free(doc);CoUninitialize();if(!wire)return 2;
    size_t length=strlen(wire);DWORD written=0;bool ok=WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),wire,(DWORD)length,&written,NULL) && written==length;free(wire);return ok?0:2;
}
