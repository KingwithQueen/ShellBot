#include "posix.h"
#include "desktop.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
bool sb_platform_image_decode(const unsigned char *b, size_t s, unsigned m, sb_image *i, sb_error *e) {
    (void)b;
    (void)s;
    (void)m;
    (void)i;
    sb_fail(e, "POSIX image decoding is not implemented.");
    return false;
}
bool sb_platform_bitmap_png(const unsigned char *b, unsigned w, unsigned h, sb_image *i, sb_error *e) {
    return sb_posix_png(b, w, h, i, e);
}
bool sb_platform_document(const unsigned char *b, size_t s, const char *f, const char *o, unsigned u,
                          unsigned m, yyjson_mut_doc *d, sb_document *r, sb_error *e) {
    (void)b;
    (void)s;
    (void)f;
    (void)o;
    (void)u;
    (void)m;
    (void)d;
    (void)r;
    sb_fail(e, "POSIX document backends are not implemented.");
    return false;
}
