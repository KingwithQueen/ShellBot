#include "desktop.h"
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
static void u32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}
static size_t chunk(unsigned char *out, const char *type, const unsigned char *data, size_t n) {
    u32(out, (uint32_t)n);
    memcpy(out + 4, type, 4);
    if (n)
        memcpy(out + 8, data, n);
    u32(out + 8 + n, (uint32_t)crc32(0, out + 4, (uInt)n + 4));
    return n + 12;
}
bool sb_posix_png(const unsigned char *bgra, unsigned w, unsigned h, sb_image *image, sb_error *e) {
    if (!bgra || !w || !h || w > 1600 || h > 1600) {
        sb_fail(e, "Invalid desktop image dimensions.");
        return false;
    }
    size_t stride = (size_t)w * 4 + 1, n = stride * h;
    unsigned char *raw = malloc(n);
    if (!raw)
        return false;
    for (unsigned y = 0; y < h; y++) {
        raw[y * stride] = 0;
        for (unsigned x = 0; x < w; x++) {
            const unsigned char *p = bgra + ((size_t)y * w + x) * 4;
            unsigned char *q = raw + y * stride + 1 + x * 4;
            q[0] = p[2];
            q[1] = p[1];
            q[2] = p[0];
            q[3] = 255;
        }
    }
    uLongf size = compressBound((uLong)n);
    unsigned char *compressed = malloc(size);
    if (!compressed) {
        free(raw);
        return false;
    }
    int rc = compress2(compressed, &size, raw, (uLong)n, Z_BEST_SPEED);
    free(raw);
    if (rc != Z_OK) {
        free(compressed);
        return false;
    }
    unsigned char *out = malloc((size_t)size + 57);
    if (!out) {
        free(compressed);
        return false;
    }
    memcpy(out, "\x89PNG\r\n\x1a\n", 8);
    unsigned char head[13] = {0};
    u32(head, w);
    u32(head + 4, h);
    head[8] = 8;
    head[9] = 6;
    size_t at = 8;
    at += chunk(out + at, "IHDR", head, 13);
    at += chunk(out + at, "IDAT", compressed, size);
    at += chunk(out + at, "IEND", NULL, 0);
    free(compressed);
    image->encoded = out;
    image->size = at;
    image->source_width = image->width = w;
    image->source_height = image->height = h;
    image->mime = image->source_mime = "image/png";
    return true;
}
