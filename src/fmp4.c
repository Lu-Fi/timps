#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "fmp4.h"

/* -------------------------------------------------------------------------
 * Box writer helpers
 * -----------------------------------------------------------------------*/

typedef struct {
    uint8_t *buf;
    int      cap;
    int      len;
} BufCtx;

static BufCtx *buf_new(int cap)
{
    BufCtx *b = malloc(sizeof(*b));
    if (!b) return NULL;
    b->buf = malloc(cap);
    b->cap = cap;
    b->len = 0;
    return b;
}

static void buf_free(BufCtx *b) { free(b->buf); free(b); }

static int buf_ensure(BufCtx *b, int extra)
{
    if (b->len + extra <= b->cap) return 0;
    int nc = b->cap * 2 + extra;
    uint8_t *nb = realloc(b->buf, nc);
    if (!nb) return -1;
    b->buf = nb;
    b->cap = nc;
    return 0;
}

static void buf_u8(BufCtx *b, uint8_t v)
{
    buf_ensure(b, 1);
    b->buf[b->len++] = v;
}

static void buf_u16(BufCtx *b, uint16_t v)
{
    buf_ensure(b, 2);
    b->buf[b->len++] = v >> 8;
    b->buf[b->len++] = v & 0xff;
}

static void buf_u24(BufCtx *b, uint32_t v)
{
    buf_ensure(b, 3);
    b->buf[b->len++] = (v >> 16) & 0xff;
    b->buf[b->len++] = (v >>  8) & 0xff;
    b->buf[b->len++] =  v        & 0xff;
}

static void buf_u32(BufCtx *b, uint32_t v)
{
    buf_ensure(b, 4);
    b->buf[b->len++] = v >> 24;
    b->buf[b->len++] = (v >> 16) & 0xff;
    b->buf[b->len++] = (v >>  8) & 0xff;
    b->buf[b->len++] =  v        & 0xff;
}

static void buf_u64(BufCtx *b, uint64_t v)
{
    buf_u32(b, (uint32_t)(v >> 32));
    buf_u32(b, (uint32_t)(v & 0xffffffff));
}

static void buf_bytes(BufCtx *b, const uint8_t *data, int len)
{
    buf_ensure(b, len);
    memcpy(b->buf + b->len, data, len);
    b->len += len;
}

static void buf_fourcc(BufCtx *b, const char *cc)
{
    buf_bytes(b, (const uint8_t *)cc, 4);
}

/* Write box size at offset (must be 4 bytes reserved earlier with buf_u32(0)) */
static void buf_patch_size(BufCtx *b, int size_offset)
{
    uint32_t sz = b->len - size_offset;
    b->buf[size_offset]   = sz >> 24;
    b->buf[size_offset+1] = (sz >> 16) & 0xff;
    b->buf[size_offset+2] = (sz >>  8) & 0xff;
    b->buf[size_offset+3] =  sz        & 0xff;
}

/* Convenience: write box header and return offset of size field */
static int box_begin(BufCtx *b, const char *fourcc)
{
    int off = b->len;
    buf_u32(b, 0);    /* placeholder for size */
    buf_fourcc(b, fourcc);
    return off;
}

static void box_end(BufCtx *b, int off)
{
    buf_patch_size(b, off);
}

/* =========================================================================
 * H.264 parameter-set extraction from Annex-B
 * ========================================================================= */

static const uint8_t *find_sc(const uint8_t *p, const uint8_t *end)
{
    for (; p + 3 <= end; p++)
        if (p[0]==0 && p[1]==0 && (p[2]==1 || (p[2]==0 && p[3]==1)))
            return p;
    return NULL;
}

static const uint8_t *skip_sc(const uint8_t *p)
{
    return (p[2]==1) ? p+3 : p+4;
}

/* =========================================================================
 * Muxer context
 * ========================================================================= */

struct Fmp4Ctx {
    fmp4_write_fn write_fn;
    void         *ud;
    int           is_hevc;
    int           width;
    int           height;
    int           fps;
    uint32_t      seq;          /* moof sequence number */
    uint64_t      decode_time;  /* running DTS (in timescale units) */

    /* Cached parameter sets (AVCC/HVCC format) */
    uint8_t  *sps;  int sps_len;
    uint8_t  *pps;  int pps_len;
    uint8_t  *vps;  int vps_len;  /* H.265 only */
};

Fmp4Ctx *fmp4_create(fmp4_write_fn write_fn, void *ud,
                     int is_hevc, int width, int height, int fps)
{
    Fmp4Ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->write_fn = write_fn;
    ctx->ud       = ud;
    ctx->is_hevc  = is_hevc;
    ctx->width    = width;
    ctx->height   = height;
    ctx->fps      = fps > 0 ? fps : 25;
    ctx->seq      = 1;
    return ctx;
}

void fmp4_destroy(Fmp4Ctx *ctx)
{
    if (!ctx) return;
    free(ctx->sps); free(ctx->pps); free(ctx->vps);
    free(ctx);
}

/* -------------------------------------------------------------------------
 * Parse Annex-B and cache SPS/PPS(/VPS)
 * -----------------------------------------------------------------------*/
static void parse_spspps(Fmp4Ctx *ctx, const uint8_t *data, int len)
{
    const uint8_t *end = data + len;
    const uint8_t *p   = data;

    while (p < end) {
        const uint8_t *sc = find_sc(p, end);
        if (!sc) break;
        const uint8_t *nalu    = skip_sc(sc);
        const uint8_t *next_sc = find_sc(nalu, end);
        int nalu_len = next_sc ? (int)(next_sc - nalu) : (int)(end - nalu);
        if (nalu_len <= 0) { p = nalu; continue; }

        if (!ctx->is_hevc) {
            uint8_t t = nalu[0] & 0x1f;
            if (t == 7) { free(ctx->sps); ctx->sps = malloc(nalu_len); memcpy(ctx->sps, nalu, nalu_len); ctx->sps_len = nalu_len; }
            if (t == 8) { free(ctx->pps); ctx->pps = malloc(nalu_len); memcpy(ctx->pps, nalu, nalu_len); ctx->pps_len = nalu_len; }
        } else {
            uint8_t t = (nalu[0] >> 1) & 0x3f;
            if (t == 32) { free(ctx->vps); ctx->vps = malloc(nalu_len); memcpy(ctx->vps, nalu, nalu_len); ctx->vps_len = nalu_len; }
            if (t == 33) { free(ctx->sps); ctx->sps = malloc(nalu_len); memcpy(ctx->sps, nalu, nalu_len); ctx->sps_len = nalu_len; }
            if (t == 34) { free(ctx->pps); ctx->pps = malloc(nalu_len); memcpy(ctx->pps, nalu, nalu_len); ctx->pps_len = nalu_len; }
        }
        p = nalu + nalu_len;
    }
}

/* -------------------------------------------------------------------------
 * Convert Annex-B stream to AVCC/HVCC (length-prefixed) NAL units
 * Returns a new malloc'd buffer and writes the length to *out_len.
 * -----------------------------------------------------------------------*/
static uint8_t *annexb_to_avcc(const uint8_t *data, int len, int *out_len)
{
    const uint8_t *end = data + len;
    const uint8_t *p   = data;
    int total = 0;

    /* First pass: count size */
    while (p < end) {
        const uint8_t *sc = find_sc(p, end);
        if (!sc) break;
        const uint8_t *nalu    = skip_sc(sc);
        const uint8_t *next_sc = find_sc(nalu, end);
        int nalu_len = next_sc ? (int)(next_sc - nalu) : (int)(end - nalu);
        if (nalu_len > 0) {
            uint8_t t = (!((data[0]>>1)&0x3f)) ? (nalu[0] & 0x1f) : ((nalu[0]>>1)&0x3f);
            /* Skip parameter sets and AUD in the data stream */
            (void)t;
            total += 4 + nalu_len;
        }
        p = nalu + (nalu_len > 0 ? nalu_len : 1);
    }

    if (total == 0) return NULL;

    uint8_t *buf = malloc(total);
    uint8_t *out = buf;
    p = data;

    while (p < end) {
        const uint8_t *sc = find_sc(p, end);
        if (!sc) break;
        const uint8_t *nalu    = skip_sc(sc);
        const uint8_t *next_sc = find_sc(nalu, end);
        int nalu_len = next_sc ? (int)(next_sc - nalu) : (int)(end - nalu);
        if (nalu_len > 0) {
            out[0] = nalu_len >> 24;
            out[1] = (nalu_len >> 16) & 0xff;
            out[2] = (nalu_len >>  8) & 0xff;
            out[3] =  nalu_len        & 0xff;
            memcpy(out + 4, nalu, nalu_len);
            out += 4 + nalu_len;
        }
        p = nalu + (nalu_len > 0 ? nalu_len : 1);
    }

    *out_len = (int)(out - buf);
    return buf;
}

/* -------------------------------------------------------------------------
 * Write avcC box (H.264 decoder config)
 * -----------------------------------------------------------------------*/
static void write_avcc(BufCtx *b, const uint8_t *sps, int sps_len,
                                  const uint8_t *pps, int pps_len)
{
    int off = box_begin(b, "avcC");
    buf_u8(b, 1);                  /* configurationVersion */
    buf_u8(b, sps[1]);             /* AVCProfileIndication */
    buf_u8(b, sps[2]);             /* profile_compatibility */
    buf_u8(b, sps[3]);             /* AVCLevelIndication */
    buf_u8(b, 0xff);               /* lengthSizeMinusOne = 3 */
    buf_u8(b, 0xe1);               /* numSequenceParameterSets = 1 */
    buf_u16(b, (uint16_t)sps_len);
    buf_bytes(b, sps, sps_len);
    buf_u8(b, 1);                  /* numPictureParameterSets = 1 */
    buf_u16(b, (uint16_t)pps_len);
    buf_bytes(b, pps, pps_len);
    box_end(b, off);
}

/* -------------------------------------------------------------------------
 * Write hvcC box (H.265 decoder config) — simplified
 * -----------------------------------------------------------------------*/
static void write_hvcc(BufCtx *b,
                       const uint8_t *vps, int vps_len,
                       const uint8_t *sps, int sps_len,
                       const uint8_t *pps, int pps_len)
{
    int off = box_begin(b, "hvcC");
    buf_u8(b, 1);         /* configurationVersion */
    /* profile/level/tier — parse from SPS would be ideal; use safe defaults */
    buf_u8(b, 0x01);      /* general_profile_space=0, general_tier=0, general_profile=1 */
    buf_u32(b, 0x60000000);
    buf_u8(b, 0x90);
    buf_u8(b, 0x00); buf_u8(b, 0x00); buf_u8(b, 0x00); buf_u8(b, 0x00);
    buf_u8(b, 0x00); buf_u8(b, 0x00); buf_u8(b, 0x00);
    buf_u8(b, 90);        /* general_level_idc */
    buf_u16(b, 0x0000);
    buf_u16(b, 0x0000);
    buf_u8(b, 0x00);
    buf_u16(b, 0x0000);
    buf_u16(b, 0x0000);
    buf_u8(b, 0x00);
    buf_u8(b, 0xff);      /* lengthSizeMinusOne = 3 */
    buf_u8(b, 3);         /* numOfArrays */

    /* VPS */
    buf_u8(b, 0x20);      /* array_completeness=0, NAL_unit_type=32 */
    buf_u16(b, 1);
    buf_u16(b, (uint16_t)vps_len);
    buf_bytes(b, vps, vps_len);

    /* SPS */
    buf_u8(b, 0x21);
    buf_u16(b, 1);
    buf_u16(b, (uint16_t)sps_len);
    buf_bytes(b, sps, sps_len);

    /* PPS */
    buf_u8(b, 0x22);
    buf_u16(b, 1);
    buf_u16(b, (uint16_t)pps_len);
    buf_bytes(b, pps, pps_len);

    box_end(b, off);
}

/* =========================================================================
 * ftyp + moov
 * ========================================================================= */

int fmp4_write_init(Fmp4Ctx *ctx, const uint8_t *spspps, int spspps_len)
{
    parse_spspps(ctx, spspps, spspps_len);

    if (!ctx->sps || !ctx->pps) return -1;
    if (ctx->is_hevc && !ctx->vps) return -1;

    BufCtx *b = buf_new(4096);
    if (!b) return -1;

    /* ---- ftyp ---- */
    {
        int off = box_begin(b, "ftyp");
        buf_fourcc(b, "iso5");  /* major brand */
        buf_u32(b, 512);        /* minor version */
        buf_fourcc(b, "iso5");
        buf_fourcc(b, "iso6");
        buf_fourcc(b, "mp41");
        box_end(b, off);
    }

    /* ---- moov ---- */
    {
        int moov = box_begin(b, "moov");

        /* mvhd */
        {
            int off = box_begin(b, "mvhd");
            buf_u8(b, 0); buf_u24(b, 0); /* version+flags */
            buf_u32(b, 0);  /* creation time */
            buf_u32(b, 0);  /* modification time */
            buf_u32(b, 1000); /* timescale */
            buf_u32(b, 0);  /* duration */
            buf_u32(b, 0x00010000); /* rate = 1.0 */
            buf_u16(b, 0x0100);     /* volume = 1.0 */
            buf_u16(b, 0);
            buf_u32(b, 0); buf_u32(b, 0); /* reserved */
            /* unity matrix */
            buf_u32(b, 0x00010000); buf_u32(b, 0); buf_u32(b, 0);
            buf_u32(b, 0); buf_u32(b, 0x00010000); buf_u32(b, 0);
            buf_u32(b, 0); buf_u32(b, 0); buf_u32(b, 0x40000000);
            for (int i = 0; i < 6; i++) buf_u32(b, 0); /* pre-defined */
            buf_u32(b, 2); /* next track id */
            box_end(b, off);
        }

        /* mvex */
        {
            int mvex = box_begin(b, "mvex");
            int off  = box_begin(b, "trex");
            buf_u8(b, 0); buf_u24(b, 0);
            buf_u32(b, 1);  /* track_ID */
            buf_u32(b, 1);  /* default_sample_description_index */
            buf_u32(b, 0);  /* default_sample_duration */
            buf_u32(b, 0);  /* default_sample_size */
            buf_u32(b, 0);  /* default_sample_flags */
            box_end(b, off);
            box_end(b, mvex);
        }

        /* trak */
        {
            int trak = box_begin(b, "trak");

            /* tkhd */
            {
                int off = box_begin(b, "tkhd");
                buf_u8(b, 0);  /* version */
                buf_u24(b, 3); /* flags: enabled + in movie */
                buf_u32(b, 0); buf_u32(b, 0); /* creation/modification */
                buf_u32(b, 1); /* track_ID */
                buf_u32(b, 0); /* reserved */
                buf_u32(b, 0); /* duration */
                buf_u32(b, 0); buf_u32(b, 0); /* reserved */
                buf_u16(b, 0); buf_u16(b, 0); /* layer, alternate group */
                buf_u16(b, 0); buf_u16(b, 0); /* volume, reserved */
                buf_u32(b, 0x00010000); buf_u32(b, 0); buf_u32(b, 0);
                buf_u32(b, 0); buf_u32(b, 0x00010000); buf_u32(b, 0);
                buf_u32(b, 0); buf_u32(b, 0); buf_u32(b, 0x40000000);
                buf_u32(b, (uint32_t)ctx->width  << 16);
                buf_u32(b, (uint32_t)ctx->height << 16);
                box_end(b, off);
            }

            /* mdia */
            {
                int mdia = box_begin(b, "mdia");

                /* mdhd */
                {
                    int off = box_begin(b, "mdhd");
                    buf_u8(b, 0); buf_u24(b, 0);
                    buf_u32(b, 0); buf_u32(b, 0);
                    buf_u32(b, 90000); /* timescale for video = 90 kHz */
                    buf_u32(b, 0);     /* duration */
                    buf_u16(b, 0x55c4); /* language und */
                    buf_u16(b, 0);
                    box_end(b, off);
                }

                /* hdlr */
                {
                    int off = box_begin(b, "hdlr");
                    buf_u8(b, 0); buf_u24(b, 0);
                    buf_u32(b, 0);
                    buf_fourcc(b, "vide");
                    buf_u32(b, 0); buf_u32(b, 0); buf_u32(b, 0);
                    buf_u8(b, 0); /* empty handler name */
                    box_end(b, off);
                }

                /* minf */
                {
                    int minf = box_begin(b, "minf");

                    /* vmhd */
                    {
                        int off = box_begin(b, "vmhd");
                        buf_u8(b, 0); buf_u24(b, 1);
                        buf_u16(b, 0); buf_u16(b, 0); buf_u16(b, 0); buf_u16(b, 0);
                        box_end(b, off);
                    }

                    /* dinf / dref */
                    {
                        int dinf = box_begin(b, "dinf");
                        int dref = box_begin(b, "dref");
                        buf_u8(b, 0); buf_u24(b, 0);
                        buf_u32(b, 1);
                        /* url: self-contained */
                        int url = box_begin(b, "url ");
                        buf_u8(b, 0); buf_u24(b, 1);
                        box_end(b, url);
                        box_end(b, dref);
                        box_end(b, dinf);
                    }

                    /* stbl */
                    {
                        int stbl = box_begin(b, "stbl");

                        /* stsd */
                        {
                            int stsd = box_begin(b, "stsd");
                            buf_u8(b, 0); buf_u24(b, 0);
                            buf_u32(b, 1); /* entry count */

                            /* avc1 or hev1 */
                            int vis = box_begin(b, ctx->is_hevc ? "hev1" : "avc1");
                            /* 6 reserved + data reference index */
                            buf_u8(b,0);buf_u8(b,0);buf_u8(b,0);
                            buf_u8(b,0);buf_u8(b,0);buf_u8(b,0);
                            buf_u16(b, 1);
                            /* pre-defined 16 + reserved 2 + pre-defined 12 */
                            for (int i = 0; i < 4; i++) buf_u32(b, 0);
                            buf_u32(b, 0); /* reserved */
                            buf_u16(b, (uint16_t)ctx->width);
                            buf_u16(b, (uint16_t)ctx->height);
                            buf_u32(b, 0x00480000); /* horiz resolution */
                            buf_u32(b, 0x00480000); /* vert resolution */
                            buf_u32(b, 0);
                            buf_u16(b, 1);  /* frame count */
                            for (int i = 0; i < 32; i++) buf_u8(b, 0); /* compressorname */
                            buf_u16(b, 0x0018); /* depth */
                            buf_u16(b, 0xffff); /* pre_defined */

                            if (ctx->is_hevc)
                                write_hvcc(b, ctx->vps, ctx->vps_len,
                                              ctx->sps, ctx->sps_len,
                                              ctx->pps, ctx->pps_len);
                            else
                                write_avcc(b, ctx->sps, ctx->sps_len,
                                              ctx->pps, ctx->pps_len);

                            box_end(b, vis);
                            box_end(b, stsd);
                        }

                        /* stts, stsc, stsz, stco — empty for fragmented */
                        {
                            int off = box_begin(b, "stts");
                            buf_u8(b,0); buf_u24(b,0); buf_u32(b,0);
                            box_end(b, off);
                        }
                        {
                            int off = box_begin(b, "stsc");
                            buf_u8(b,0); buf_u24(b,0); buf_u32(b,0);
                            box_end(b, off);
                        }
                        {
                            int off = box_begin(b, "stsz");
                            buf_u8(b,0); buf_u24(b,0);
                            buf_u32(b,0); buf_u32(b,0);
                            box_end(b, off);
                        }
                        {
                            int off = box_begin(b, "stco");
                            buf_u8(b,0); buf_u24(b,0); buf_u32(b,0);
                            box_end(b, off);
                        }
                        box_end(b, stbl);
                    }
                    box_end(b, minf);
                }
                box_end(b, mdia);
            }
            box_end(b, trak);
        }
        box_end(b, moov);
    }

    ctx->write_fn(b->buf, b->len, ctx->ud);
    buf_free(b);
    return 0;
}

/* =========================================================================
 * moof + mdat per frame
 * ========================================================================= */

int fmp4_write_video_frame(Fmp4Ctx *ctx,
                           const uint8_t *data, int len,
                           int key, uint64_t pts_ms)
{
    int avcc_len  = 0;
    uint8_t *avcc = annexb_to_avcc(data, len, &avcc_len);
    if (!avcc || avcc_len == 0) { free(avcc); return -1; }

    /* timescale = 90000 */
    uint64_t ts = pts_ms * 90;

    BufCtx *b = buf_new(avcc_len + 256);
    if (!b) { free(avcc); return -1; }

    /* ---- moof ---- */
    {
        int moof = box_begin(b, "moof");

        /* mfhd */
        {
            int off = box_begin(b, "mfhd");
            buf_u8(b, 0); buf_u24(b, 0);
            buf_u32(b, ctx->seq++);
            box_end(b, off);
        }

        /* traf */
        {
            int traf = box_begin(b, "traf");

            /* tfhd */
            {
                int off = box_begin(b, "tfhd");
                buf_u8(b, 0);
                buf_u24(b, 0x020000); /* default-base-is-moof */
                buf_u32(b, 1);        /* track ID */
                box_end(b, off);
            }

            /* tfdt */
            {
                int off = box_begin(b, "tfdt");
                buf_u8(b, 1); buf_u24(b, 0); /* version=1 */
                buf_u64(b, ts);
                box_end(b, off);
            }

            /* trun */
            {
                /* flags: data-offset-present (0x0001) +
                          sample-flags-present (0x0004) +
                          sample-duration-present (0x0100) +
                          sample-size-present (0x0200) */
                int off = box_begin(b, "trun");
                buf_u8(b, 0);
                buf_u24(b, 0x0305);
                buf_u32(b, 1);    /* sample count */
                /* data-offset: size of moof + 8 bytes for mdat header */
                int data_off_pos = b->len;
                buf_u32(b, 0);    /* placeholder */

                /* sample flags: sync point or not */
                uint32_t sflags = key ? 0x02000000 : 0x01010000;
                buf_u32(b, sflags);

                /* sample duration: 1/fps * 90000 */
                buf_u32(b, 90000 / ctx->fps);
                buf_u32(b, (uint32_t)avcc_len);

                box_end(b, off);

                /* Patch data-offset = moofSize + 8 */
                int moof_size = b->len - moof;
                uint32_t doff = (uint32_t)(moof_size + 8);
                b->buf[data_off_pos]   = doff >> 24;
                b->buf[data_off_pos+1] = (doff >> 16) & 0xff;
                b->buf[data_off_pos+2] = (doff >>  8) & 0xff;
                b->buf[data_off_pos+3] =  doff        & 0xff;
            }

            box_end(b, traf);
        }
        box_end(b, moof);
    }

    /* ---- mdat ---- */
    {
        int off = box_begin(b, "mdat");
        buf_bytes(b, avcc, avcc_len);
        box_end(b, off);
    }

    ctx->write_fn(b->buf, b->len, ctx->ud);
    buf_free(b);
    free(avcc);

    ctx->decode_time = ts + 90000 / ctx->fps;
    return 0;
}
