/* SHA-256, FIPS 180-4 sections 4.2.2 and 6.2. No platform crypto dependency. */
#include "posix.h"
#include <string.h>
static uint32_t rotate(uint32_t v, unsigned n) { return (v >> n) | (v << (32 - n)); }
static void compress(sb_sha *c, const unsigned char *p) {
    static const uint32_t k[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t w[64];
    for (unsigned i=0;i<16;i++) w[i]=(uint32_t)p[i*4]<<24|(uint32_t)p[i*4+1]<<16|(uint32_t)p[i*4+2]<<8|p[i*4+3];
    for (unsigned i=16;i<64;i++) {
        uint32_t a=w[i-15],b=w[i-2];
        w[i]=w[i-16]+(rotate(a,7)^rotate(a,18)^(a>>3))+w[i-7]+(rotate(b,17)^rotate(b,19)^(b>>10));
    }
    uint32_t a=c->h[0],b=c->h[1],d=c->h[3],e=c->h[4],f=c->h[5],g=c->h[6],h=c->h[7],v=c->h[2];
    for (unsigned i=0;i<64;i++) {
        uint32_t t=h+(rotate(e,6)^rotate(e,11)^rotate(e,25))+((e&f)^(~e&g))+k[i]+w[i];
        uint32_t u=(rotate(a,2)^rotate(a,13)^rotate(a,22))+((a&b)^(a&v)^(b&v));
        h=g;g=f;f=e;e=d+t;d=v;v=b;b=a;a=t+u;
    }
    c->h[0]+=a;c->h[1]+=b;c->h[2]+=v;c->h[3]+=d;c->h[4]+=e;c->h[5]+=f;c->h[6]+=g;c->h[7]+=h;
}
void sb_sha_init(sb_sha *c) {
    *c=(sb_sha){.h={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19}};
}
void sb_sha_update(sb_sha *c, const void *data, size_t length) {
    const unsigned char *p=data; c->bytes+=length;
    while (length) {
        size_t n=64-c->used; if(n>length)n=length;
        memcpy(c->block+c->used,p,n);c->used+=n;p+=n;length-=n;
        if(c->used==64){compress(c,c->block);c->used=0;}
    }
}
void sb_sha_finish(sb_sha *c, char hex[65]) {
    uint64_t bits=c->bytes*8; unsigned char pad[128]={0x80};
    size_t n=c->used<56?56-c->used:120-c->used;
    for(unsigned i=0;i<8;i++)pad[n+i]=(unsigned char)(bits>>(56-i*8));
    sb_sha_update(c,pad,n+8);
    static const char digits[]="0123456789abcdef";
    for(unsigned i=0;i<32;i++){unsigned b=(c->h[i/4]>>(24-(i%4)*8))&255;hex[i*2]=digits[b>>4];hex[i*2+1]=digits[b&15];}
    hex[64]=0;memset(c,0,sizeof(*c));
}
bool sb_platform_sha256(const void *bytes,size_t length,char hex[65],sb_error *error) {
    (void)error;sb_sha c;sb_sha_init(&c);sb_sha_update(&c,bytes,length);sb_sha_finish(&c,hex);return true;
}
