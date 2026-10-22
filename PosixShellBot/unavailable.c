#include "posix.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
bool sb_desktop_available(sb_desktop *d){(void)d;return false;}
yyjson_mut_val *sb_desktop_capabilities(sb_desktop *d,yyjson_mut_doc *doc){(void)d;yyjson_mut_val *v=yyjson_mut_obj(doc);yyjson_mut_obj_add_bool(doc,v,"available",false);return v;}
char *sb_desktop_request(sb_desktop *d,const char *a,const char *n,yyjson_val *v,bool p){(void)d;(void)a;(void)n;(void)v;(void)p;return sb_strdup("{\"error\":\"POSIX desktop backend is not implemented\"}");}
bool sb_platform_image_decode(const unsigned char *b,size_t s,unsigned m,sb_image *i,sb_error *e){(void)b;(void)s;(void)m;(void)i;sb_fail(e,"POSIX image decoding is not implemented.");return false;}
bool sb_platform_bitmap_png(const unsigned char *b,unsigned w,unsigned h,sb_image *i,sb_error *e){(void)b;(void)w;(void)h;(void)i;sb_fail(e,"POSIX bitmap encoding is not implemented.");return false;}
bool sb_platform_document(const unsigned char *b,size_t s,const char *f,const char *o,unsigned u,unsigned m,yyjson_mut_doc *d,sb_document *r,sb_error *e){(void)b;(void)s;(void)f;(void)o;(void)u;(void)m;(void)d;(void)r;sb_fail(e,"POSIX document backends are not implemented.");return false;}
