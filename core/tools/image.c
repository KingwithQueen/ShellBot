#include "sb.h"
#include <stdio.h>
#include <string.h>

static uint32_t be32(const unsigned char *p) {
    return ((uint32_t)p[0]<<24) | ((uint32_t)p[1]<<16) | ((uint32_t)p[2]<<8) | p[3];
}
static unsigned be16(const unsigned char *p) { return ((unsigned)p[0]<<8) | p[1]; }
static uint32_t crc32(const unsigned char *p,size_t size) {
    uint32_t crc=UINT32_MAX;
    for (size_t i=0;i<size;++i) {
        crc^=p[i];
        for (unsigned bit=0;bit<8;++bit) crc=(crc>>1) ^ (UINT32_C(0xedb88320) & (0u-(crc&1u)));
    }
    return ~crc;
}
static bool png(const unsigned char *p,size_t size,sb_image *image) {
    if (size<57 || memcmp(p,"\x89PNG\r\n\x1a\n",8)) return false;
    size_t offset=8; bool header=false,palette=false,data=false,data_ended=false;
    unsigned color=0;
    while (offset<size) {
        if (size-offset<12) return false;
        size_t length=be32(p+offset); const unsigned char *kind=p+offset+4,*body=p+offset+8;
        if (length>size-offset-12 || be32(body+length)!=crc32(kind,length+4)) return false;
        for (unsigned i=0;i<4;++i) if (!((kind[i]>='A' && kind[i]<='Z') || (kind[i]>='a' && kind[i]<='z'))) return false;
        if (kind[2]<'A' || kind[2]>'Z') return false;
        if (!header) {
            if (memcmp(kind,"IHDR",4) || length!=13) return false;
            image->source_width=be32(body); image->source_height=be32(body+4); color=body[9];
            if (body[10] || body[11] || body[12]>1) return false;
            header=true;
        } else if (!memcmp(kind,"IHDR",4)) return false;
        else if (!memcmp(kind,"PLTE",4)) {
            if (palette || data || !length || length>768 || length%3) return false;
            palette=true;
        } else if (!memcmp(kind,"IDAT",4)) {
            if (data_ended || (color==3 && !palette)) return false;
            data=true;
        } else if (!memcmp(kind,"IEND",4)) {
            return !length && data && offset+12==size;
        } else {
            if (kind[0]>='A' && kind[0]<='Z') return false; /* Unknown critical chunk. */
            if (!memcmp(kind,"acTL",4) || !memcmp(kind,"fcTL",4) || !memcmp(kind,"fdAT",4)) return false;
            if (data) data_ended=true;
        }
        offset+=length+12;
    }
    return false;
}
static bool jpeg(const unsigned char *p,size_t size,sb_image *image) {
    if (size<4 || p[0]!=0xff || p[1]!=0xd8) return false;
    size_t offset=2; bool frame=false,scan=false,entropy=false; unsigned components=0;
    while (offset<size) {
        if (p[offset++]!=0xff) return false;
        while (offset<size && p[offset]==0xff) ++offset;
        if (offset>=size) return false;
        unsigned marker=p[offset++];
        if (marker==0xd9) return frame && scan && entropy && offset==size;
        if (!marker || marker==0xd8 || (marker>=0xd0 && marker<=0xd7)) return false;
        if (marker==1) continue;
        if (size-offset<2) return false;
        unsigned length=be16(p+offset);
        if (length<2 || length>size-offset) return false;
        if (marker>=0xc0 && marker<=0xcf && marker!=0xc4 && marker!=0xc8 && marker!=0xcc) {
            if (frame || (marker!=0xc0 && marker!=0xc1 && marker!=0xc2) || length<8 || p[offset+2]!=8) return false;
            image->source_height=be16(p+offset+3); image->source_width=be16(p+offset+5);
            components=p[offset+7];
            if (!components || components>4 || length!=8+components*3) return false;
            frame=true;
        }
        if (marker==0xda) {
            if (!frame || length<6 || !p[offset+2] || p[offset+2]>components || length!=6u+(unsigned)p[offset+2]*2u) return false;
            scan=true; offset+=length;
            while (offset<size) {
                if (p[offset]!=0xff) { entropy=true; ++offset; continue; }
                size_t start=offset++;
                while (offset<size && p[offset]==0xff) ++offset;
                if (offset>=size) return false;
                if (!p[offset]) { entropy=true; ++offset; continue; }
                if (p[offset]>=0xd0 && p[offset]<=0xd7) { ++offset; continue; }
                offset=start; break;
            }
        } else offset+=length;
    }
    return false;
}
bool sb_image_prepare(const unsigned char *bytes,size_t size,unsigned max_dimension,sb_image *image,sb_error *error) {
    memset(image,0,sizeof(*image));
    if (!size || size>SB_IMAGE_INPUT_LIMIT || (max_dimension && (max_dimension<128 || max_dimension>4096))) {
        snprintf(error->text,sizeof(error->text),"Image source must be 1 byte to 16 MiB; max_dimension must be 128 to 4096."); return false;
    }
    bool is_png=size>=8 && !memcmp(bytes,"\x89PNG\r\n\x1a\n",8);
    if (!(is_png ? png(bytes,size,image) : jpeg(bytes,size,image))) {
        snprintf(error->text,sizeof(error->text),"Invalid, truncated or unsupported PNG/JPEG container (including PNG CRC/order and animated PNG)."); return false;
    }
    if (!image->source_width || !image->source_height || image->source_width>32768 || image->source_height>32768 ||
        (uint64_t)image->source_width*image->source_height>SB_IMAGE_PIXEL_LIMIT) {
        snprintf(error->text,sizeof(error->text),"Image dimensions exceed 32768 per side or 16777216 source pixels; resizing cannot bypass the source limit."); return false;
    }
    image->source_mime=is_png ? "image/png" : "image/jpeg";
    if (is_png && !sb_png_validate_raster(bytes,size)) {
        snprintf(error->text,sizeof(error->text),"Invalid PNG compressed raster, filter rows, exact length or Adler-32 checksum."); return false;
    }
    return sb_platform_image_decode(bytes,size,max_dimension,image,error);
}
