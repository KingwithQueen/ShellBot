#include "sb.h"
#include <stdlib.h>
#include <string.h>

char *sb_base64(const unsigned char *data, size_t length) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t size = ((length + 2) / 3) * 4;
    char *output = malloc(size + 1);
    if (!output) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < length; i += 3) {
        uint32_t n = (uint32_t)data[i] << 16;
        if (i + 1 < length) n |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < length) n |= data[i + 2];
        output[j++] = alphabet[(n >> 18) & 63];
        output[j++] = alphabet[(n >> 12) & 63];
        output[j++] = i + 1 < length ? alphabet[(n >> 6) & 63] : '=';
        output[j++] = i + 2 < length ? alphabet[n & 63] : '=';
    }
    output[j] = 0;
    return output;
}

/* Strict Unicode decoding, with a byte cursor that never splits a character. */
bool sb_decode(const unsigned char *data, size_t length, bool end_of_file,
               const char *encoding, bool at_start, char **text, size_t *text_length,
               size_t *consumed, const char **detected, sb_error *error) {
    size_t i = 0, j = 0;
    int mode = 0;
    if (!strcmp(encoding, "auto")) {
        if (!at_start) { sb_fail(error, "Use the returned encoding for nonzero offsets."); return false; }
        if (length >= 2 && data[0] == 0xff && data[1] == 0xfe) mode = 1;
        if (length >= 2 && data[0] == 0xfe && data[1] == 0xff) mode = 2;
    } else if (!strcmp(encoding, "utf-16le")) mode = 1;
    else if (!strcmp(encoding, "utf-16be")) mode = 2;
    else if (strcmp(encoding, "utf-8")) { sb_fail(error, "Unsupported text encoding."); return false; }
    *detected = mode == 1 ? "utf-16le" : mode == 2 ? "utf-16be" : "utf-8";
    if (at_start) {
        if (!mode && length >= 3 && !memcmp(data, "\xef\xbb\xbf", 3)) i = 3;
        if (mode && length >= 2 && ((mode == 1 && data[0] == 0xff && data[1] == 0xfe) ||
            (mode == 2 && data[0] == 0xfe && data[1] == 0xff))) i = 2;
    }
    char *output = malloc(length * 2 + 1);
    if (!output) { sb_fail(error, "Out of memory."); return false; }
    while (i < length) {
        size_t start = i;
        uint32_t cp;
        if (mode) {
            if (length - i < 2) goto incomplete;
            cp = mode == 1 ? (uint32_t)(data[i] | (data[i + 1] << 8)) : (uint32_t)((data[i] << 8) | data[i + 1]);
            i += 2;
            if (cp >= 0xd800 && cp <= 0xdbff) {
                if (length - i < 2) goto incomplete;
                uint32_t low = mode == 1 ? (uint32_t)(data[i] | (data[i + 1] << 8)) : (uint32_t)((data[i] << 8) | data[i + 1]);
                if (low < 0xdc00 || low > 0xdfff) goto invalid;
                i += 2; cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
            } else if (cp >= 0xdc00 && cp <= 0xdfff) goto invalid;
        } else {
            unsigned char first = data[i++];
            unsigned extra = first < 0x80 ? 0 : first >= 0xc2 && first <= 0xdf ? 1 :
                first >= 0xe0 && first <= 0xef ? 2 : first >= 0xf0 && first <= 0xf4 ? 3 : 99;
            if (extra == 99) goto invalid;
            cp = first & (extra ? (0x7f >> extra) : 0x7f);
            if (length - i < extra) goto incomplete;
            for (unsigned k = 0; k < extra; ++k) {
                unsigned char c = data[i++];
                if ((c & 0xc0) != 0x80) goto invalid;
                cp = (cp << 6) | (c & 0x3f);
            }
            if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) ||
                (extra == 3 && cp < 0x10000) || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) goto invalid;
        }
        if (cp == 0 || (cp < 0x20 && cp != 9 && cp != 10 && cp != 13)) goto invalid;
        if (cp < 0x80) output[j++] = (char)cp;
        else if (cp < 0x800) { output[j++] = (char)(0xc0 | (cp >> 6)); output[j++] = (char)(0x80 | (cp & 63)); }
        else if (cp < 0x10000) {
            output[j++] = (char)(0xe0 | (cp >> 12)); output[j++] = (char)(0x80 | ((cp >> 6) & 63)); output[j++] = (char)(0x80 | (cp & 63));
        } else {
            output[j++] = (char)(0xf0 | (cp >> 18)); output[j++] = (char)(0x80 | ((cp >> 12) & 63));
            output[j++] = (char)(0x80 | ((cp >> 6) & 63)); output[j++] = (char)(0x80 | (cp & 63));
        }
        continue;
incomplete:
        if (end_of_file) goto invalid;
        i = start; break;
    }
    if (!i && length) { sb_fail(error, "Byte budget is too small for a complete character."); free(output); return false; }
    output[j] = 0; *text = output; *text_length = j; *consumed = i;
    return true;
invalid:
    free(output); sb_fail(error, "Invalid text, binary data, or offset inside a Unicode character."); return false;
}
