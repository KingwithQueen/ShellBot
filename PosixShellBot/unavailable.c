#include "posix.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
struct sb_execution {char instance[33];};
sb_execution *sb_execution_new(const sb_config *c,sb_error *e){if(c->execution_enabled){sb_fail(e,"POSIX command backends are not implemented; execution.enabled must be false.");return NULL;}sb_execution *x=calloc(1,sizeof(*x));unsigned char nonce[16];if(!x||!sb_platform_random(nonce,sizeof(nonce),e)){free(x);return NULL;}for(unsigned i=0;i<16;i++)snprintf(x->instance+i*2,3,"%02x",nonce[i]);return x;}
void sb_execution_free(sb_execution *x){free(x);}
bool sb_execution_available(sb_execution *x){(void)x;return false;}
bool sb_execution_restricted_verified(sb_execution *x){(void)x;return false;}
bool sb_execution_program_permitted(sb_execution *x,const char *p){(void)x;(void)p;return false;}
void sb_execution_prepare_project(sb_execution *x,sb_host *h){(void)x;(void)h;}
bool sb_execution_task_owned(sb_execution *x,const char *a,const char *id){(void)x;(void)a;(void)id;return false;}
const char *sb_execution_instance(sb_execution *x){return x?x->instance:"";}
yyjson_mut_val *sb_execution_status(sb_execution *x,yyjson_mut_doc *d){yyjson_mut_val *v=yyjson_mut_obj(d);yyjson_mut_obj_add_bool(d,v,"available",false);yyjson_mut_obj_add_str(d,v,"reason","POSIX command backends are not implemented");yyjson_mut_obj_add_str(d,v,"host_instance_id",sb_execution_instance(x));return v;}
char *sb_execution_task(sb_execution *x,const char *n,yyjson_val *a,sb_error *e){(void)x;(void)n;(void)a;sb_fail(e,"Command execution is unavailable.");return NULL;}
bool sb_desktop_available(sb_desktop *d){(void)d;return false;}
yyjson_mut_val *sb_desktop_capabilities(sb_desktop *d,yyjson_mut_doc *doc){(void)d;yyjson_mut_val *v=yyjson_mut_obj(doc);yyjson_mut_obj_add_bool(doc,v,"available",false);return v;}
char *sb_desktop_request(sb_desktop *d,const char *a,const char *n,yyjson_val *v,bool p){(void)d;(void)a;(void)n;(void)v;(void)p;return sb_strdup("{\"error\":\"POSIX desktop backend is not implemented\"}");}
bool sb_platform_image_decode(const unsigned char *b,size_t s,unsigned m,sb_image *i,sb_error *e){(void)b;(void)s;(void)m;(void)i;sb_fail(e,"POSIX image decoding is not implemented.");return false;}
bool sb_platform_bitmap_png(const unsigned char *b,unsigned w,unsigned h,sb_image *i,sb_error *e){(void)b;(void)w;(void)h;(void)i;sb_fail(e,"POSIX bitmap encoding is not implemented.");return false;}
bool sb_platform_document(const unsigned char *b,size_t s,const char *f,const char *o,unsigned u,unsigned m,yyjson_mut_doc *d,sb_document *r,sb_error *e){(void)b;(void)s;(void)f;(void)o;(void)u;(void)m;(void)d;(void)r;sb_fail(e,"POSIX document backends are not implemented.");return false;}
