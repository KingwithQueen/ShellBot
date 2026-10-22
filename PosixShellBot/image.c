#include "media.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 32768
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

/* Bilinear interpolation in premultiplied alpha prevents dark/colored fringes. */
static unsigned char *resize(const unsigned char *src, unsigned sw, unsigned sh, unsigned w, unsigned h) {
    unsigned char *out = malloc((size_t)w * h * 4);
    if (!out)
        return NULL;
    for (unsigned y = 0; y < h; y++)
        for (unsigned x = 0; x < w; x++) {
            double sx = ((double)x + .5) * sw / w - .5, sy = ((double)y + .5) * sh / h - .5;
            if (sx < 0)
                sx = 0;
            if (sy < 0)
                sy = 0;
            unsigned x0 = (unsigned)sx, y0 = (unsigned)sy, x1 = x0 + 1 < sw ? x0 + 1 : x0,
                     y1 = y0 + 1 < sh ? y0 + 1 : y0;
            double dx = sx - x0, dy = sy - y0,
                   weights[4] = {(1 - dx) * (1 - dy), dx * (1 - dy), (1 - dx) * dy, dx * dy};
            const unsigned char *p[4] = {src + ((size_t)y0 * sw + x0) * 4, src + ((size_t)y0 * sw + x1) * 4,
                                         src + ((size_t)y1 * sw + x0) * 4, src + ((size_t)y1 * sw + x1) * 4};
            double a = 0, c[3] = {0};
            for (unsigned k = 0; k < 4; k++) {
                a += weights[k] * p[k][3];
                for (unsigned j = 0; j < 3; j++)
                    c[j] += weights[k] * p[k][j] * p[k][3];
            }
            unsigned char *q = out + ((size_t)y * w + x) * 4;
            q[3] = (unsigned char)(a + .5);
            for (unsigned j = 0; j < 3; j++)
                q[j] = a > 0 ? (unsigned char)fmin(255, c[j] / a + .5) : 0;
        }
    return out;
}
bool sb_platform_image_decode(const unsigned char *bytes, size_t size, unsigned dimension, sb_image *image,
                              sb_error *error) {
    int w = 0, h = 0, channels = 0;
    unsigned char *pixels = stbi_load_from_memory(bytes, (int)size, &w, &h, &channels, 4);
    if (!pixels || w < 1 || h < 1 || (unsigned)w != image->source_width ||
        (unsigned)h != image->source_height) {
        stbi_image_free(pixels);
        sb_fail(error, "Full PNG/JPEG pixel decoding failed.");
        return false;
    }
    image->width = (unsigned)w;
    image->height = (unsigned)h;
    image->mime = image->source_mime;
    image->size = size;
    unsigned longest = (unsigned)(w > h ? w : h), limit = dimension ? dimension : longest;
    if (size > SB_IMAGE_LIMIT && limit > 1600)
        limit = 1600;
    if (longest <= limit && size <= SB_IMAGE_LIMIT) {
        stbi_image_free(pixels);
        return true;
    }
    if (limit > longest)
        limit = longest;
    bool ok = false;
    while (limit >= 1) {
        unsigned nw = (unsigned)((uint64_t)(unsigned)w * limit / longest),
                 nh = (unsigned)((uint64_t)(unsigned)h * limit / longest);
        if (!nw)
            nw = 1;
        if (!nh)
            nh = 1;
        unsigned char *small = resize(pixels, (unsigned)w, (unsigned)h, nw, nh);
        sb_image result = {0};
        ok = small && sb_media_png(small, nw, nh, false, &result, error);
        free(small);
        if (!ok)
            break;
        if (result.size <= SB_IMAGE_LIMIT) {
            image->encoded = result.encoded;
            image->size = result.size;
            image->width = nw;
            image->height = nh;
            image->mime = "image/png";
            break;
        }
        free(result.encoded);
        ok = false;
        if (limit == 1)
            break;
        limit = limit * 3 / 4;
        if (!limit)
            limit = 1;
    }
    stbi_image_free(pixels);
    if (!ok)
        sb_fail(error, "Image derivative exceeds output budget or could not be encoded.");
    return ok;
}
bool sb_platform_bitmap_png(const unsigned char *bgra, unsigned w, unsigned h, sb_image *image, sb_error *e) {
    return sb_media_png(bgra, w, h, true, image, e);
}
