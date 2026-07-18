/* util.h - common small helpers, byte writers, time */
#ifndef MS_UTIL_H
#define MS_UTIL_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

/* monotonic microseconds */
static inline int64_t ms_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* big-endian writers used by RTP and MP4 muxers */
static inline void wr_be16(uint8_t *p, uint16_t v){ p[0]=v>>8; p[1]=v; }
static inline void wr_be24(uint8_t *p, uint32_t v){ p[0]=v>>16; p[1]=v>>8; p[2]=v; }
static inline void wr_be32(uint8_t *p, uint32_t v){ p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v; }
static inline void wr_be64(uint8_t *p, uint64_t v){ for(int i=0;i<8;i++) p[i]=(uint8_t)(v>>(56-8*i)); }

static inline uint16_t rd_be16(const uint8_t *p){ return (uint16_t)((p[0]<<8)|p[1]); }
static inline uint32_t rd_be32(const uint8_t *p){ return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }

/* growable byte buffer (used to assemble MP4 boxes) */
typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
    int      err;   /* sticky: set once any ms_buf_* call fails to grow the
                      * buffer (OOM). Once set, box_close()/fragment() must
                      * not patch size/offset fields into it - the buffer's
                      * content is short some bytes, so `pos` no longer
                      * points at what the caller thinks it does. */
} ms_buf;

int  ms_buf_init(ms_buf *b, size_t cap);
int  ms_buf_reserve(ms_buf *b, size_t extra);
int  ms_buf_put(ms_buf *b, const void *src, size_t n);
int  ms_buf_u8(ms_buf *b, uint8_t v);
int  ms_buf_be16(ms_buf *b, uint16_t v);
int  ms_buf_be32(ms_buf *b, uint32_t v);
void ms_buf_free(ms_buf *b);
/* reuse a persistent buffer: len=0, err=0, and shrink the backing store back to
 * `soft` if a rare huge frame grew it past that, so per-connection/-recorder
 * buffers don't stay ballooned. Normal frames fit under `soft` -> no realloc. */
void ms_buf_reset(ms_buf *b, size_t soft);

/* base64 encode; returns bytes written (excludes NUL). dst must hold
 * >= ((n+2)/3)*4 + 1 bytes. */
int ms_base64(char *dst, const uint8_t *src, int n);

#endif
