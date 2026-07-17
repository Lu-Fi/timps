#include "util.h"
#include <stdlib.h>

int ms_buf_init(ms_buf *b, size_t cap)
{
    if (cap < 64) cap = 64;
    b->data = (uint8_t*)malloc(cap);
    b->len = 0;
    b->cap = b->data ? cap : 0;
    b->err = b->data ? 0 : 1;
    return b->data ? 0 : -1;
}

int ms_buf_reserve(ms_buf *b, size_t extra)
{
    if (b->len + extra <= b->cap) return 0;
    size_t nc = b->cap ? b->cap : 64;
    while (nc < b->len + extra) nc *= 2;
    uint8_t *nd = (uint8_t*)realloc(b->data, nc);
    if (!nd) { b->err = 1; return -1; }
    b->data = nd; b->cap = nc;
    return 0;
}

int ms_buf_put(ms_buf *b, const void *src, size_t n)
{
    if (ms_buf_reserve(b, n)) return -1;
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return 0;
}

int ms_buf_u8(ms_buf *b, uint8_t v){ return ms_buf_put(b, &v, 1); }
int ms_buf_be16(ms_buf *b, uint16_t v){ uint8_t t[2]; wr_be16(t,v); return ms_buf_put(b,t,2); }
int ms_buf_be32(ms_buf *b, uint32_t v){ uint8_t t[4]; wr_be32(t,v); return ms_buf_put(b,t,4); }

void ms_buf_free(ms_buf *b){ free(b->data); b->data=NULL; b->len=b->cap=0; }

void ms_buf_reset(ms_buf *b, size_t soft)
{
    b->len = 0; b->err = 0;
    if (soft && b->cap > soft) {
        uint8_t *nd = (uint8_t*)realloc(b->data, soft);
        if (nd) { b->data = nd; b->cap = soft; }   /* shrink failure is harmless */
    }
}

int ms_base64(char *dst, const uint8_t *src, int n)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int i=0, o=0;
    while (i+3 <= n){
        uint32_t v = (src[i]<<16)|(src[i+1]<<8)|src[i+2];
        dst[o++]=t[(v>>18)&63]; dst[o++]=t[(v>>12)&63];
        dst[o++]=t[(v>>6)&63];  dst[o++]=t[v&63];
        i+=3;
    }
    int rem = n-i;
    if (rem==1){
        uint32_t v=src[i]<<16;
        dst[o++]=t[(v>>18)&63]; dst[o++]=t[(v>>12)&63]; dst[o++]='='; dst[o++]='=';
    } else if (rem==2){
        uint32_t v=(src[i]<<16)|(src[i+1]<<8);
        dst[o++]=t[(v>>18)&63]; dst[o++]=t[(v>>12)&63]; dst[o++]=t[(v>>6)&63]; dst[o++]='=';
    }
    dst[o]=0;
    return o;
}
