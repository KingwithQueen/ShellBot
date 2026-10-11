/* Strict bounded PNG raster validation. RFC 1950/1951 bit order and canonical trees.
   WIC can repair missing rows, so its successful CopyPixels alone is insufficient. */
#include "sb.h"
#include <stdlib.h>
#include <string.h>

typedef struct { const unsigned char *data; size_t size,offset; uint32_t buffer; unsigned count; bool failed; } bits;
typedef struct { int16_t child[576][2],symbol[576]; unsigned nodes; } tree;
static uint32_t be32(const unsigned char *p) { return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static unsigned take(bits *input,unsigned count) {
    while (input->count<count) {
        if (input->offset==input->size) { input->failed=true; return 0; }
        input->buffer|=(uint32_t)input->data[input->offset++]<<input->count; input->count+=8;
    }
    unsigned value=input->buffer&((1u<<count)-1u);
    input->buffer>>=count; input->count-=count; return value;
}
static bool make_tree(tree *output,const unsigned char *lengths,unsigned symbols,bool empty_allowed,bool incomplete_allowed) {
    unsigned counts[16]={0},next[16]={0},nonzero=0; int left=1;
    for (unsigned i=0;i<symbols;++i) { if (lengths[i]>15) return false; if (lengths[i]) { ++counts[lengths[i]]; ++nonzero; } }
    memset(output,0xff,sizeof(*output)); output->nodes=1;
    if (!nonzero) return empty_allowed;
    for (unsigned i=1;i<=15;++i) { left=left*2-(int)counts[i]; if (left<0) return false; }
    if (left && !(incomplete_allowed && nonzero==1 && counts[1]==1)) return false;
    unsigned code=0;
    for (unsigned i=1;i<=15;++i) { code=(code+counts[i-1])<<1; next[i]=code; }
    for (unsigned i=0;i<symbols;++i) {
        unsigned length=lengths[i]; if (!length) continue;
        code=next[length]++; unsigned node=0;
        for (unsigned bit=length;bit;--bit) {
            unsigned edge=(code>>(bit-1))&1u;
            if (output->symbol[node]>=0) return false;
            if (output->child[node][edge]<0) {
                if (output->nodes>=576) return false;
                output->child[node][edge]=(int16_t)output->nodes++;
            }
            node=(unsigned)output->child[node][edge];
        }
        if (output->symbol[node]>=0 || output->child[node][0]>=0 || output->child[node][1]>=0) return false;
        output->symbol[node]=(int16_t)i;
    }
    return true;
}
static int symbol(bits *input,const tree *codes) {
    int node=0;
    for (unsigned depth=0;depth<=15;++depth) {
        if (codes->symbol[node]>=0) return codes->symbol[node];
        node=codes->child[node][take(input,1)];
        if (input->failed || node<0) break;
    }
    input->failed=true; return -1;
}
static bool dynamic(bits *input,tree *literal,tree *distance) {
    static const unsigned actual_order[19]={16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    unsigned literals=take(input,5)+257,distances=take(input,5)+1,count=take(input,4)+4;
    unsigned char code_lengths[19]={0},lengths[318]={0}; tree code;
    if (input->failed || literals>286 || distances>32) return false;
    for (unsigned i=0;i<count;++i) code_lengths[actual_order[i]]=(unsigned char)take(input,3);
    if (input->failed || !make_tree(&code,code_lengths,19,false,false)) return false;
    unsigned used=0;
    while (used<literals+distances) {
        int value=symbol(input,&code); unsigned repeat=1; unsigned char length=0;
        if (value<0) return false;
        if (value<=15) length=(unsigned char)value;
        else if (value==16) { if (!used) return false; length=lengths[used-1]; repeat=take(input,2)+3; }
        else if (value==17) repeat=take(input,3)+3;
        else if (value==18) repeat=take(input,7)+11;
        else return false;
        if (input->failed || repeat>literals+distances-used) return false;
        memset(lengths+used,length,repeat); used+=repeat;
    }
    return lengths[256] && make_tree(literal,lengths,literals,false,true) && make_tree(distance,lengths+literals,distances,true,true);
}
static bool inflate(const unsigned char *compressed,size_t size,unsigned char *output,size_t expected) {
    if (size<6 || (compressed[0]&15)!=8 || (compressed[0]>>4)>7 || (compressed[1]&32) ||
        (((unsigned)compressed[0]<<8)|compressed[1])%31) return false;
    bits input={.data=compressed+2,.size=size-6}; size_t used=0; bool final=false;
    static const unsigned length_base[29]={3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const unsigned char length_extra[29]={0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const unsigned distance_base[30]={1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    while (!final && !input.failed) {
        final=take(&input,1)!=0; unsigned kind=take(&input,2);
        if (input.failed || kind==3) return false;
        if (!kind) {
            unsigned discard=input.count%8; input.buffer>>=discard; input.count-=discard;
            unsigned length=take(&input,16),inverse=take(&input,16);
            if (input.failed || (length^inverse)!=65535 || length>expected-used) return false;
            for (unsigned i=0;i<length;++i) output[used++]=(unsigned char)take(&input,8);
        } else {
            tree literal,distance;
            if (kind==1) {
                unsigned char lengths[288],distances[32];
                for (unsigned i=0;i<288;++i) lengths[i]=(unsigned char)(i<144 ? 8 : i<256 ? 9 : i<280 ? 7 : 8);
                memset(distances,5,sizeof(distances));
                if (!make_tree(&literal,lengths,288,false,false) || !make_tree(&distance,distances,32,false,false)) return false;
            } else if (!dynamic(&input,&literal,&distance)) return false;
            while (!input.failed) {
                int value=symbol(&input,&literal);
                if (value==256) break;
                if (value<0 || value>285) return false;
                if (value<256) { if (used==expected) return false; output[used++]=(unsigned char)value; continue; }
                unsigned index=(unsigned)value-257,length=length_base[index]+take(&input,length_extra[index]);
                value=symbol(&input,&distance); if (value<0 || value>29) return false;
                unsigned extra=value<4 ? 0 : (unsigned)value/2-1;
                unsigned back=distance_base[value]+take(&input,extra);
                if (input.failed || back>used || back>(1u<<((compressed[0]>>4)+8)) || length>expected-used) return false;
                for (unsigned i=0;i<length;++i) { output[used]=output[used-back]; ++used; }
            }
        }
    }
    if (input.failed || !final || used!=expected || input.offset!=input.size) return false;
    uint32_t first=1,second=0;
    for (size_t offset=0;offset<expected;) {
        size_t end=offset+4096; if (end>expected) end=expected;
        while (offset<end) { first+=output[offset++]; second+=first; }
        first%=65521; second%=65521;
    }
    return ((second<<16)|first)==be32(compressed+size-4);
}
bool sb_png_validate_raster(const unsigned char *bytes,size_t size) {
    /* Container and source dimension bounds have already been verified. */
    uint32_t width=be32(bytes+16),height=be32(bytes+20); unsigned depth=bytes[24],color=bytes[25],channels=0;
    if (color==0) channels=1; else if (color==2) channels=3; else if (color==3) channels=1; else if (color==4) channels=2; else if (color==6) channels=4; else return false;
    if (color==0) { if (depth!=1 && depth!=2 && depth!=4 && depth!=8 && depth!=16) return false; }
    else if (color==3) { if (depth!=1 && depth!=2 && depth!=4 && depth!=8) return false; }
    else if (depth!=8 && depth!=16) return false;
    static const unsigned x[7]={0,4,0,2,0,1,0},y[7]={0,0,4,0,2,0,1},dx[7]={8,8,4,4,2,2,1},dy[7]={8,8,8,4,4,2,2};
    size_t rows[7]={0},row_bytes[7]={0},expected=0; unsigned passes=bytes[28] ? 7 : 1;
    for (unsigned i=0;i<passes;++i) {
        uint32_t pass_width=passes==1 ? width : width>x[i] ? (width-x[i]+dx[i]-1)/dx[i] : 0;
        rows[i]=passes==1 ? height : height>y[i] ? (height-y[i]+dy[i]-1)/dy[i] : 0;
        if (!pass_width) rows[i]=0;
        row_bytes[i]=((size_t)pass_width*channels*depth+7)/8;
        expected+=rows[i]*(row_bytes[i]+1);
    }
    if (!expected || expected>134447104u) return false; /* 16M RGBA16 pixels plus pass filters. */
    unsigned char *compressed=malloc(size),*raster=malloc(expected); size_t compressed_size=0;
    if (!compressed || !raster) { free(compressed); free(raster); return false; }
    for (size_t offset=8;offset<size;) {
        size_t length=be32(bytes+offset);
        if (!memcmp(bytes+offset+4,"IDAT",4)) { memcpy(compressed+compressed_size,bytes+offset+8,length); compressed_size+=length; }
        offset+=length+12;
    }
    bool ok=inflate(compressed,compressed_size,raster,expected); size_t offset=0;
    for (unsigned i=0;ok && i<passes;++i) for (size_t row=0;row<rows[i];++row) { if (raster[offset]>4) ok=false; offset+=row_bytes[i]+1; }
    free(compressed); free(raster); return ok;
}
