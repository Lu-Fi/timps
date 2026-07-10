#include "md5.h"
#include <string.h>

static uint32_t rol(uint32_t x, int c){ return (x<<c)|(x>>(32-c)); }

static const uint32_t K[64] = {
0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391 };
static const int S[64] = {
7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21 };

static void block(md5_ctx *c, const uint8_t *p)
{
    uint32_t M[16];
    for (int i=0;i<16;i++)
        M[i]=(uint32_t)p[i*4]|((uint32_t)p[i*4+1]<<8)|((uint32_t)p[i*4+2]<<16)|((uint32_t)p[i*4+3]<<24);
    uint32_t A=c->a,B=c->b,C=c->c,D=c->d;
    for (int i=0;i<64;i++){
        uint32_t F; int g;
        if (i<16){ F=(B&C)|(~B&D); g=i; }
        else if (i<32){ F=(D&B)|(~D&C); g=(5*i+1)&15; }
        else if (i<48){ F=B^C^D; g=(3*i+5)&15; }
        else { F=C^(B|~D); g=(7*i)&15; }
        F=F+A+K[i]+M[g];
        A=D; D=C; C=B; B=B+rol(F,S[i]);
    }
    c->a+=A; c->b+=B; c->c+=C; c->d+=D;
}

void md5_init(md5_ctx *c)
{
    c->a=0x67452301; c->b=0xefcdab89; c->c=0x98badcfe; c->d=0x10325476;
    c->len=0; c->n=0;
}

void md5_update(md5_ctx *c, const void *data, size_t len)
{
    const uint8_t *p=data; c->len+=len;
    while (len){
        size_t k=64-c->n; if(k>len)k=len;
        memcpy(c->buf+c->n,p,k); c->n+=k; p+=k; len-=k;
        if (c->n==64){ block(c,c->buf); c->n=0; }
    }
}

void md5_final(md5_ctx *c, uint8_t out[16])
{
    uint64_t bits=c->len*8;
    uint8_t pad=0x80;
    md5_update(c,&pad,1);
    uint8_t z=0;
    while (c->n!=56) md5_update(c,&z,1);
    uint8_t lb[8];
    for (int i=0;i<8;i++) lb[i]=(uint8_t)(bits>>(8*i));
    md5_update(c,lb,8);
    uint32_t v[4]={c->a,c->b,c->c,c->d};
    for (int i=0;i<4;i++){ out[i*4]=v[i]; out[i*4+1]=v[i]>>8; out[i*4+2]=v[i]>>16; out[i*4+3]=v[i]>>24; }
}

void md5_hex(const char *s, char out[33])
{
    md5_ctx c; md5_init(&c); md5_update(&c,s,strlen(s));
    uint8_t d[16]; md5_final(&c,d);
    static const char *h="0123456789abcdef";
    for (int i=0;i<16;i++){ out[i*2]=h[d[i]>>4]; out[i*2+1]=h[d[i]&15]; }
    out[32]=0;
}
