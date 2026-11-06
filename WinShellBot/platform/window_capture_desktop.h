/* 只在租约线程使用；WGC 和 PrintWindow 的阻塞调用均在独立进程。 */
#include "window_capture.h"
#include <wincrypt.h>
typedef struct {sb_desktop *desktop;desktop_job *job;observation *view;window_ref target;char lease[33],screen[33];} capture_binding;
static bool capture_guard(void *parameter){
    capture_binding *b=parameter;sb_desktop *d=b->desktop;pump();
    if(b->job->cancelled || sb_platform_millis()>=b->job->deadline || !owns(d,b->job->actor,b->lease) || !lease_available(d) || b->view->invalidated || strcmp(b->screen,b->view->id) || sb_platform_millis()-b->view->at>OBSERVATION_MS || b->view->input_epoch!=InterlockedCompareExchange(d->epoch,0,0) || !window_identity(&b->target) || !input_permitted(d,b->target.hwnd) || IsIconic(b->target.hwnd))return false;
    topology monitors;char hash[65];return layout(&monitors,hash) && !strcmp(hash,b->view->layout);
}
static bool window_capture_payload(yyjson_val *r){
    const char *png=sb_string(yyjson_obj_get(r,"png_base64"));uint64_t bytes=0,w=0,h=0;
    if(!png || !sb_integer(yyjson_obj_get(r,"encoded_bytes"),&bytes) || !bytes || bytes>SB_IMAGE_LIMIT || strlen(png)!=4*((bytes+2)/3) || !sb_integer(yyjson_obj_get(r,"width"),&w) || !w || w>SB_IMAGE_DIMENSION || !sb_integer(yyjson_obj_get(r,"height"),&h) || !h || h>SB_IMAGE_DIMENSION)return false;
    DWORD actual=0;if(!CryptStringToBinaryA(png,0,CRYPT_STRING_BASE64|CRYPT_STRING_STRICT,NULL,&actual,NULL,NULL) || actual!=bytes || actual<24)return false;
    BYTE *raw=malloc(actual);if(!raw)return false;bool ok=CryptStringToBinaryA(png,0,CRYPT_STRING_BASE64|CRYPT_STRING_STRICT,raw,&actual,NULL,NULL)!=0;
    static const BYTE signature[]={137,80,78,71,13,10,26,10};if(ok)ok=!memcmp(raw,signature,8) && !memcmp(raw+12,"IHDR",4) && (((uint64_t)raw[16]<<24)|((uint64_t)raw[17]<<16)|((uint64_t)raw[18]<<8)|raw[19])==w && (((uint64_t)raw[20]<<24)|((uint64_t)raw[21]<<16)|((uint64_t)raw[22]<<8)|raw[23])==h;
    free(raw);return ok;
}
static const char *window_capture_operation(sb_desktop *desktop,desktop_job *job,yyjson_val *args,yyjson_mut_doc *doc,yyjson_mut_val *data,char **image){
    *image=NULL;observation *view=find_observation(desktop,sb_string(yyjson_obj_get(args,"screen_id")));
    const char *failure=observe_guard_mode(desktop,view,false,false,false,doc,data);if(failure)return failure;
    const char *id=sb_string(yyjson_obj_get(args,"window_id"));window_ref *target=NULL;for(unsigned i=0;i<view->window_count;++i)if(!strcmp(view->windows[i].id,id))target=&view->windows[i];
    if(!target || !window_identity(target))return "stale_window_id";if(!input_permitted(desktop,target->hwnd))return "window_capture_owner_or_integrity_not_permitted";if(IsIconic(target->hwnd))return "window_capture_target_minimized: restore without automatic focus before capturing";
    int maximum=1600;integer(args,"max_dimension",1600,200,SB_IMAGE_DIMENSION,&maximum);const char *backend=sb_string(yyjson_obj_get(args,"capture_backend"));if(!backend)backend="auto";bool automatic=!strcmp(backend,"auto");
    capture_binding binding={.desktop=desktop,.job=job,.view=view,.target=*target};strcpy(binding.lease,desktop->lease_id);strcpy(binding.screen,view->id);
    yyjson_mut_val *diagnostic=object(doc),*attempts=yyjson_mut_arr(doc);yyjson_mut_obj_add_str(doc,diagnostic,"kind","window");yyjson_mut_obj_add_strcpy(doc,diagnostic,"requested_backend",backend);yyjson_mut_obj_add_bool(doc,diagnostic,"foreground_requested",false);yyjson_mut_obj_add_val(doc,diagnostic,"attempts",attempts);yyjson_mut_obj_add_val(doc,data,"capture_diagnostic",diagnostic);
    uint64_t end=sb_platform_millis()+4000;if(end>job->deadline)end=job->deadline;if(end>desktop->deadline)end=desktop->deadline;yyjson_doc *selected=NULL;desktop->capture_busy=true;
    for(unsigned attempt=0;attempt<(automatic?2u:1u);++attempt){
        if(!capture_guard(&binding)){failure="window_capture_guard_changed";break;}
        const char *method=automatic?(attempt?"printwindow":"wgc"):backend;uint64_t deadline=end;if(automatic && !attempt && deadline>sb_platform_millis()+1800)deadline=sb_platform_millis()+1800;
        yyjson_mut_doc *request=yyjson_mut_doc_new(NULL);if(!request){failure="window_capture_allocation_failed";break;}yyjson_mut_val *body=object(request);
        yyjson_mut_obj_add_uint(request,body,"hwnd",(uintptr_t)target->hwnd);yyjson_mut_obj_add_uint(request,body,"pid",target->pid);yyjson_mut_obj_add_uint(request,body,"created_low",target->created.dwLowDateTime);yyjson_mut_obj_add_uint(request,body,"created_high",target->created.dwHighDateTime);yyjson_mut_obj_add_uint(request,body,"deadline",deadline);yyjson_mut_obj_add_uint(request,body,"max_dimension",(unsigned)maximum);yyjson_mut_obj_add_str(request,body,"backend",method);yyjson_mut_obj_add_bool(request,body,"application_background",desktop->background_lease);yyjson_mut_obj_add_uint(request,body,"session_signature",desktop->background_signature);
        yyjson_doc *normalized=yyjson_mut_val_imut_copy(body,NULL);yyjson_mut_doc_free(request);if(!normalized){failure="window_capture_allocation_failed";break;}
        sb_error fault={0};char *wire=sb_win_capture_request(yyjson_doc_get_root(normalized),capture_guard,&binding,deadline,&fault);yyjson_doc_free(normalized);yyjson_doc *reply=wire?yyjson_read(wire,strlen(wire),0):NULL;free(wire);yyjson_val *r=yyjson_doc_get_root(reply);bool payload=reply && yyjson_is_obj(r) && !yyjson_obj_get(r,"error") && window_capture_payload(r);
        yyjson_mut_val *entry=reply && yyjson_is_obj(r)?yyjson_val_mut_copy(doc,r):object(doc);yyjson_mut_obj_remove_str(entry,"png_base64");yyjson_mut_obj_add_bool(doc,entry,"image_validated",payload);
        if(!reply){yyjson_mut_obj_add_str(doc,entry,"backend",method);yyjson_mut_obj_add_strcpy(doc,entry,"error",*fault.text?fault.text:"capture_worker_invalid_reply");}
        yyjson_mut_arr_add_val(attempts,entry);
        if(payload){if(selected)yyjson_doc_free(selected);selected=reply;reply=NULL;if(!automatic || !yyjson_get_bool(yyjson_obj_get(r,"all_black"))){yyjson_doc_free(reply);break;}}
        yyjson_doc_free(reply);
    }
    desktop->capture_busy=false;
    if(!capture_guard(&binding)){if(selected)yyjson_doc_free(selected);return "window_capture_guard_changed";}
    if(!selected)return failure?failure:"window_capture_all_backends_failed";
    yyjson_val *chosen=yyjson_doc_get_root(selected);const char *png=sb_string(yyjson_obj_get(chosen,"png_base64"));*image=sb_strdup(png);if(!*image){yyjson_doc_free(selected);return "window_capture_allocation_failed";}
    yyjson_mut_obj_add_str(doc,diagnostic,"code","ok");yyjson_mut_obj_add_strcpy(doc,diagnostic,"selected_backend",sb_string(yyjson_obj_get(chosen,"backend")));yyjson_mut_obj_add_bool(doc,diagnostic,"pixels_captured",true);yyjson_mut_obj_add_bool(doc,diagnostic,"application_paint_verified",false);yyjson_mut_obj_add_bool(doc,diagnostic,"content_may_be_blank",yyjson_get_bool(yyjson_obj_get(chosen,"all_black")));
    const char *fields[]={"width","height","physical_region","physical_mapping_known","cursor_capture_disabled","alpha_preserved"};for(unsigned i=0;i<6;++i)yyjson_mut_obj_add_val(doc,data,fields[i],yyjson_val_mut_copy(doc,yyjson_obj_get(chosen,fields[i])));
    yyjson_mut_obj_add_uint(doc,data,"bytes",yyjson_get_uint(yyjson_obj_get(chosen,"encoded_bytes")));yyjson_mut_obj_add_strcpy(doc,data,"window_id",id);rect_json(doc,data,"window_bounds",&target->rect);yyjson_mut_obj_add_str(doc,data,"mime_type","image/png");yyjson_mut_obj_add_str(doc,data,"coordinate_space","window_image_pixels");yyjson_mut_obj_add_bool(doc,data,"coordinate_input_source",false);yyjson_mut_obj_add_bool(doc,data,"foreground_requested",false);
    if(yyjson_get_bool(yyjson_obj_get(chosen,"physical_mapping_known"))){yyjson_val *region=yyjson_obj_get(chosen,"physical_region");yyjson_mut_obj_add_real(doc,data,"physical_pixels_per_image_x",yyjson_get_num(yyjson_obj_get(region,"width"))/yyjson_get_num(yyjson_obj_get(chosen,"width")));yyjson_mut_obj_add_real(doc,data,"physical_pixels_per_image_y",yyjson_get_num(yyjson_obj_get(region,"height"))/yyjson_get_num(yyjson_obj_get(chosen,"height")));}
    yyjson_doc_free(selected);return NULL;
}
