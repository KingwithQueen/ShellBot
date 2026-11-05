/* Included by desktop.c on the lease-owning thread. No provider COM calls on that thread. */
#include "uia.h"
bool sb_win_uia_guard(HWND window,DWORD pid,FILETIME created,LONG epoch,uint64_t deadline,bool input,bool require_foreground){
    sb_desktop context={0};HANDLE mutex=NULL,pause=NULL,mapping=NULL;volatile LONG *current=NULL;
    if(!shared_objects(&mutex,&pause,&mapping,&current,&context.session,context.sid,&context.integrity,false))return false;
    DWORD actual=0;GetWindowThreadProcessId(window,&actual);HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,actual);FILETIME start={0},exit,kernel,user;
    bool ok=process && GetProcessTimes(process,&start,&exit,&kernel,&user) && !memcmp(&start,&created,sizeof(start)) && actual==pid && IsWindowVisible(window) &&
        interactive(&context) && input_permitted(&context,window) && sb_platform_millis()<deadline;
    if(input)ok=ok && (!require_foreground || GetForegroundWindow()==window) && !IsIconic(window) && WaitForSingleObject(pause,0)!=WAIT_OBJECT_0 && epoch==InterlockedCompareExchange(current,0,0);
    if(process)CloseHandle(process);UnmapViewOfFile((const void*)current);CloseHandle(mapping);CloseHandle(pause);CloseHandle(mutex);return ok;
}
typedef struct {sb_desktop *desktop;desktop_job *job;observation *view;window_ref window;char screen[33],lease[33];bool changing,require_foreground;} uia_binding;
static bool uia_guard(void *parameter){
    uia_binding *b=parameter;sb_desktop *d=b->desktop;pump();
    if(b->job->cancelled || sb_platform_millis()>=b->job->deadline || !owns(d,b->job->actor,b->lease) || !interactive(d) || b->view->invalidated || strcmp(b->screen,b->view->id) ||
        sb_platform_millis()-b->view->at>OBSERVATION_MS || b->view->input_epoch!=InterlockedCompareExchange(d->epoch,0,0) || !window_identity(&b->window) || !input_permitted(d,b->window.hwnd))return false;
    if(b->changing && (d->observation_lease || WaitForSingleObject(d->pause,0)==WAIT_OBJECT_0 || (b->require_foreground && GetForegroundWindow()!=b->window.hwnd) || IsIconic(b->window.hwnd)))return false;
    topology monitors;char hash[65];if(!layout(&monitors,hash) || strcmp(hash,b->view->layout))return false;
    return true;
}
static const char *uia_operation(sb_desktop *desktop,desktop_job *job,yyjson_val *args,yyjson_mut_doc *doc,yyjson_mut_val *data){
    bool changing=!strcmp(job->name,"uia_action");if(desktop->uia_busy)return "uia_busy: another provider request is in progress";
    const char *focus_setting=sb_string(yyjson_obj_get(args,"focus_mode"));bool require_foreground=changing && (!focus_setting || strcmp(focus_setting,"background"));
    if(changing && desktop->observation_lease)return "desktop_observation_lease_cannot_control_uia";
    observation *view=find_observation(desktop,sb_string(yyjson_obj_get(args,"screen_id")));
    /* 定向语义操作绑定窗口与控件身份，不依赖其他窗口的前台/矩形变化。 */
    const char *error=observe_guard_mode(desktop,view,changing,require_foreground,require_foreground,doc,data);if(error)return error;
    const char *id=sb_string(yyjson_obj_get(args,"window_id"));window_ref *target=NULL;
    for(unsigned i=0;i<view->window_count;++i)if(!strcmp(view->windows[i].id,id))target=&view->windows[i];
    if(!target || !window_identity(target))return "stale_window_id";
    if(!input_permitted(desktop,target->hwnd))return "uia_window_owner_or_integrity_not_permitted";
    if(changing && IsIconic(target->hwnd))return "uia_target_minimized: restore the target and refresh";
    if(require_foreground && GetForegroundWindow()!=target->hwnd)return "uia_target_not_foreground: focus and refresh or explicitly use focus_mode background";
    yyjson_val *element=NULL;
    if(changing){
        const char *element_id=sb_string(yyjson_obj_get(args,"element_id"));
        if(!desktop->uia_snapshot || strcmp(desktop->uia_screen,view->id) || strcmp(desktop->uia_window,id))return "uia_tree_stale: obtain a fresh uia_tree for this window";
        yyjson_val *nodes=yyjson_obj_get(yyjson_doc_get_root(desktop->uia_snapshot),"nodes");size_t i,n;yyjson_val *node;
        yyjson_arr_foreach(nodes,i,n,node){const char *candidate=sb_string(yyjson_obj_get(node,"element_id"));if(candidate && !strcmp(candidate,element_id))element=node;}
        if(!element)return "uia_element_id_not_in_observed_tree";
    }
    yyjson_mut_doc *request=yyjson_mut_doc_new(NULL);if(!request)return "uia_request_allocation_failed";yyjson_mut_val *body=object(request);
    int maximum=500,depth=16;integer(args,"max_nodes",500,1,1000,&maximum);integer(args,"max_depth",16,0,32,&depth);
    uint64_t deadline=sb_platform_millis()+4000;if(deadline>desktop->deadline)deadline=desktop->deadline;if(deadline>job->deadline)deadline=job->deadline;
    yyjson_mut_obj_add_uint(request,body,"hwnd",(uintptr_t)target->hwnd);yyjson_mut_obj_add_uint(request,body,"pid",target->pid);
    yyjson_mut_obj_add_uint(request,body,"created_low",target->created.dwLowDateTime);yyjson_mut_obj_add_uint(request,body,"created_high",target->created.dwHighDateTime);
    yyjson_mut_obj_add_sint(request,body,"epoch",view->input_epoch);yyjson_mut_obj_add_uint(request,body,"deadline",deadline);
    yyjson_mut_obj_add_uint(request,body,"max_nodes",changing?1000:(unsigned)maximum);yyjson_mut_obj_add_uint(request,body,"max_depth",changing?32:(unsigned)depth);
    yyjson_mut_obj_add_bool(request,body,"include_text",yyjson_get_bool(yyjson_obj_get(args,"include_text")));
    yyjson_mut_obj_add_bool(request,body,"require_foreground",require_foreground);
    if(element){
        yyjson_mut_val *identity=object(request);const char *fields[]={"runtime_id","control_type","name","automation_id","class_name"};
        for(unsigned i=0;i<5;++i)yyjson_mut_obj_add_val(request,identity,fields[i],yyjson_val_mut_copy(request,yyjson_obj_get(element,fields[i])));
        yyjson_mut_obj_add_val(request,body,"element",identity);yyjson_mut_obj_add_strcpy(request,body,"action",sb_string(yyjson_obj_get(args,"action")));
        const char *value=sb_string(yyjson_obj_get(args,"value"));if(value){char *encoded=sb_base64((const unsigned char*)value,strlen(value));if(!encoded){yyjson_mut_doc_free(request);return "uia_value_encoding_failed";}yyjson_mut_obj_add_strcpy(request,body,"value_base64",encoded);free(encoded);}
    }
    yyjson_mut_doc_set_root(request,body);yyjson_doc *normalized=yyjson_mut_doc_imut_copy(request,NULL);yyjson_mut_doc_free(request);if(!normalized)return "uia_request_allocation_failed";
    uia_binding binding={.desktop=desktop,.job=job,.view=view,.window=*target,.changing=changing,.require_foreground=require_foreground};strcpy(binding.screen,view->id);strcpy(binding.lease,desktop->lease_id);
    sb_error fault={0};desktop->uia_busy=true;char *wire=sb_win_uia_request(yyjson_doc_get_root(normalized),uia_guard,&binding,deadline,&fault);desktop->uia_busy=false;yyjson_doc_free(normalized);
    yyjson_doc *reply=wire?yyjson_read(wire,strlen(wire),0):NULL;free(wire);
    if(!reply){yyjson_mut_obj_add_strcpy(doc,data,"diagnostic",*fault.text?fault.text:"uia_invalid_worker_reply");if(changing){yyjson_mut_obj_add_str(doc,data,"action_state","unknown");invalidate(desktop);}return "uia_worker_failed: inspect possible effects; do not automatically replay";}
    yyjson_val *root=yyjson_doc_get_root(reply);const char *failure=sb_string(yyjson_obj_get(root,"error"));yyjson_mut_val *copy=yyjson_val_mut_copy(doc,root);
    if(!changing && !failure){
        yyjson_mut_val *nodes=yyjson_mut_obj_get(copy,"nodes");size_t i,n;yyjson_mut_val *node;
        yyjson_mut_arr_foreach(nodes,i,n,node){char nonce_id[33];if(!nonce(nonce_id)){failure="uia_random_source_unavailable";break;}yyjson_mut_obj_add_strcpy(doc,node,"element_id",nonce_id);}
        yyjson_doc *snapshot=yyjson_mut_val_imut_copy(copy,NULL);if(!snapshot)failure="uia_snapshot_allocation_failed";
        if(desktop->uia_snapshot)yyjson_doc_free(desktop->uia_snapshot);desktop->uia_snapshot=snapshot;strcpy(desktop->uia_screen,view->id);strcpy(desktop->uia_window,id);
        yyjson_mut_arr_foreach(nodes,i,n,node)yyjson_mut_obj_remove_str(node,"runtime_id");
    }
    size_t i,n;yyjson_mut_val *key,*value;yyjson_mut_obj_foreach(copy,i,n,key,value){const char *field=yyjson_mut_get_str(key);yyjson_mut_obj_add_val(doc,data,!strcmp(field,"error")?"provider_error":field,yyjson_mut_val_mut_copy(doc,value));}
    yyjson_mut_obj_add_bool(doc,data,"screenshot_verified",false);yyjson_mut_obj_add_str(doc,data,"provider","Windows UI Automation control view; same window process only");
    if(changing){yyjson_mut_obj_add_str(doc,data,"focus_mode",require_foreground?"foreground":"background");yyjson_mut_obj_add_bool(doc,data,"foreground_required",require_foreground);yyjson_mut_obj_add_bool(doc,data,"foreground_observed",GetForegroundWindow()==target->hwnd);}
    bool failed=failure!=NULL;if(changing)invalidate(desktop);yyjson_doc_free(reply);return failed?"uia_provider_or_element_error":NULL;
}
