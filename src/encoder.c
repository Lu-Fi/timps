#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <imp/imp_common.h>
#include <imp/imp_encoder.h>
#include "encoder.h"
#include "config.h"
#include "log.h"

#define MAX_SINKS  16
#define STREAM_BUF_SIZE (256 * 1024)

typedef struct {
    int           active;
    int           enc_chn;     /* IMP encoder channel id */
    int           enc_grp;     /* IMP encoder group id   */

    pthread_t     thread;
    volatile int  running;

    pthread_mutex_t  sink_lock;
    EncoderSink      sinks[MAX_SINKS];
    int              nsinks;

    uint8_t      *spspps;
    int           spspps_len;
} EncoderCtx;

static EncoderCtx g_enc[2];

/* --------------------------------------------------------------------- */

int encoder_create(int idx)
{
    StreamCfg        *s   = &g_cfg.stream[idx];
    EncoderCtx       *ctx = &g_enc[idx];
    IMPEncoderChnAttr attr;
    int               ret;

    if (!s->enabled) return -1;
    if (ctx->active)  return 0;

    memset(ctx, 0, sizeof(*ctx));
    pthread_mutex_init(&ctx->sink_lock, NULL);
    ctx->enc_grp = idx;
    ctx->enc_chn = idx;

    /* T31+ new API: create encoder group first, then channel */
    ret = IMP_Encoder_CreateGroup(ctx->enc_grp);
    if (ret && ret != -1) {
        log_error("IMP_Encoder_CreateGroup(%d) failed: %d", ctx->enc_grp, ret);
        return ret;
    }

    /* Use SetDefaultParam for T31+ API */
    IMP_Encoder_SetMaxStreamCnt(ctx->enc_chn, 2);
    IMP_Encoder_SetStreamBufSize(ctx->enc_chn, STREAM_BUF_SIZE);

    IMPEncoderProfile profile = (s->codec == CODEC_H265)
                              ? IMP_ENC_PROFILE_HEVC_MAIN
                              : IMP_ENC_PROFILE_AVC_HIGH;

    memset(&attr, 0, sizeof(attr));
    ret = IMP_Encoder_SetDefaultParam(&attr, profile, IMP_ENC_RC_MODE_CBR,
                                      (uint16_t)s->width, (uint16_t)s->height,
                                      (uint32_t)s->fps, 1,
                                      (uint32_t)s->gop, 2, -1,
                                      (uint32_t)(s->bitrate * 1000));
    if (ret) {
        log_warn("IMP_Encoder_SetDefaultParam(%d) failed: %d — using manual attr", idx, ret);
        /* Fallback: fill manually for older SDKs */
        attr.encAttr.eProfile   = profile;
        attr.encAttr.uWidth     = (uint32_t)s->width;
        attr.encAttr.uHeight    = (uint32_t)s->height;
        attr.encAttr.bufSize    = 0;
        attr.gopAttr.gopMode    = IMPENCODE_GOP_MODE_NORMALP;
        attr.gopAttr.gopLength  = s->gop;
    }

    ret = IMP_Encoder_CreateChn(ctx->enc_chn, &attr);
    if (ret) {
        log_error("IMP_Encoder_CreateChn(%d) failed: %d", ctx->enc_chn, ret);
        IMP_Encoder_DestroyGroup(ctx->enc_grp);
        return ret;
    }

    IMP_Encoder_SetChnGopLength(ctx->enc_chn, s->fps);

    ret = IMP_Encoder_RegisterChn(ctx->enc_grp, ctx->enc_chn);
    if (ret) {
        log_error("IMP_Encoder_RegisterChn(%d,%d) failed: %d",
                  ctx->enc_grp, ctx->enc_chn, ret);
        IMP_Encoder_DestroyChn(ctx->enc_chn);
        IMP_Encoder_DestroyGroup(ctx->enc_grp);
        return ret;
    }

    ctx->active = 1;
    log_info("encoder[%d] created: %dx%d %s %dkbps gop=%d",
             idx, s->width, s->height,
             s->codec == CODEC_H265 ? "H.265" : "H.264",
             s->bitrate, s->gop);
    return 0;
}

void encoder_destroy(int idx)
{
    EncoderCtx *ctx = &g_enc[idx];
    if (!ctx->active) return;

    encoder_stop(idx);

    IMP_Encoder_UnRegisterChn(ctx->enc_grp, ctx->enc_chn);
    IMP_Encoder_DestroyChn(ctx->enc_chn);
    IMP_Encoder_DestroyGroup(ctx->enc_grp);

    free(ctx->spspps);
    pthread_mutex_destroy(&ctx->sink_lock);
    memset(ctx, 0, sizeof(*ctx));
}

/* --------------------------------------------------------------------- */
/* Pack data extraction (handles ring-buffer wrap on T31+)               */
/* --------------------------------------------------------------------- */

static int extract_pack(const IMPEncoderStream *stream, int pack_idx,
                        uint8_t *dst)
{
    const IMPEncoderPack *pack = &stream->pack[pack_idx];
    uint32_t plen = pack->length;

    if (stream->virAddr && stream->streamSize > 0) {
        /* New API: virAddr is ring-buffer base, offset is the pack start */
        uint8_t  *base    = (uint8_t *)(uintptr_t)stream->virAddr;
        uint32_t  offset  = pack->offset;
        uint32_t  remains = stream->streamSize - offset;

        if (remains < plen) {
            /* Wrap-around copy */
            memcpy(dst,           base + offset, remains);
            memcpy(dst + remains, base,           plen - remains);
        } else {
            memcpy(dst, base + offset, plen);
        }
    } else {
        /* Old API: virAddr in the pack is an absolute virtual address */
        memcpy(dst, (uint8_t *)(uintptr_t)pack->virAddr, plen);
    }
    return (int)plen;
}

/* --------------------------------------------------------------------- */
/* Encoding thread                                                        */
/* --------------------------------------------------------------------- */

static void *enc_thread(void *arg)
{
    int         idx = (int)(intptr_t)arg;
    EncoderCtx *ctx = &g_enc[idx];
    IMPEncoderStream stream;
    EncoderFrame ef;

    log_debug("encoder[%d] thread started", idx);

    while (ctx->running) {
        int ret = IMP_Encoder_PollingStream(ctx->enc_chn, 1000);
        if (ret < 0) {
            log_error("IMP_Encoder_PollingStream(%d): %d", idx, ret);
            break;
        }
        if (ret == 0) continue; /* timeout */

        memset(&stream, 0, sizeof(stream));
        ret = IMP_Encoder_GetStream(ctx->enc_chn, &stream, 1);
        if (ret) {
            log_error("IMP_Encoder_GetStream(%d): %d", idx, ret);
            continue;
        }

        /* Assemble frame data from pack list */
        int total = 0;
        for (int i = 0; i < (int)stream.packCount; i++)
            total += (int)stream.pack[i].length;

        uint8_t *buf = malloc(total);
        if (buf) {
            int off = 0;
            for (int i = 0; i < (int)stream.packCount; i++) {
                off += extract_pack(&stream, i, buf + off);
            }

            /* Determine if this is a key frame:
             * For H.264: NAL type 5 (IDR) at the start.
             * For H.265: NAL type 19 or 20 (IDR_W_RADL / IDR_N_LP).
             * With Annex-B data the NAL type is in byte 4 (after 3-byte SC). */
            int key = 0;
            if (off >= 5) {
                uint8_t nal = buf[4];
                if (g_cfg.stream[idx].codec == CODEC_H265) {
                    int t = (nal >> 1) & 0x3f;
                    key = (t == 19 || t == 20);
                } else {
                    key = ((nal & 0x1f) == 5);
                }
            }

            memset(&ef, 0, sizeof(ef));
            ef.stream_idx = idx;
            ef.data       = buf;
            ef.len        = off;
            ef.pts        = (uint64_t)stream.seq;
            ef.key        = key;

            pthread_mutex_lock(&ctx->sink_lock);
            for (int s = 0; s < ctx->nsinks; s++) {
                if (ctx->sinks[s].cb)
                    ctx->sinks[s].cb(&ef, ctx->sinks[s].userdata);
            }
            pthread_mutex_unlock(&ctx->sink_lock);

            free(buf);
        }

        IMP_Encoder_ReleaseStream(ctx->enc_chn, &stream);
    }

    log_debug("encoder[%d] thread exiting", idx);
    return NULL;
}

int encoder_start(int idx)
{
    EncoderCtx *ctx = &g_enc[idx];
    int ret;

    if (!ctx->active || ctx->running) return 0;

    ret = IMP_Encoder_StartRecvPic(ctx->enc_chn);
    if (ret) {
        log_error("IMP_Encoder_StartRecvPic(%d) failed: %d", idx, ret);
        return ret;
    }

    ctx->running = 1;
    if (pthread_create(&ctx->thread, NULL, enc_thread, (void *)(intptr_t)idx)) {
        log_error("encoder[%d] thread create failed", idx);
        ctx->running = 0;
        IMP_Encoder_StopRecvPic(ctx->enc_chn);
        return -1;
    }

    log_info("encoder[%d] started", idx);
    return 0;
}

void encoder_stop(int idx)
{
    EncoderCtx *ctx = &g_enc[idx];
    if (!ctx->running) return;

    ctx->running = 0;
    pthread_join(ctx->thread, NULL);
    IMP_Encoder_StopRecvPic(ctx->enc_chn);
    log_info("encoder[%d] stopped", idx);
}

int encoder_add_sink(int idx, EncoderSink *sink)
{
    EncoderCtx *ctx = &g_enc[idx];
    if (!ctx->active) return -1;

    pthread_mutex_lock(&ctx->sink_lock);
    if (ctx->nsinks >= MAX_SINKS) {
        pthread_mutex_unlock(&ctx->sink_lock);
        return -1;
    }
    ctx->sinks[ctx->nsinks++] = *sink;
    pthread_mutex_unlock(&ctx->sink_lock);
    return 0;
}

void encoder_remove_sink(int idx, EncoderSink *sink)
{
    EncoderCtx *ctx = &g_enc[idx];

    pthread_mutex_lock(&ctx->sink_lock);
    for (int i = 0; i < ctx->nsinks; i++) {
        if (ctx->sinks[i].cb       == sink->cb &&
            ctx->sinks[i].userdata == sink->userdata) {
            ctx->sinks[i] = ctx->sinks[--ctx->nsinks];
            break;
        }
    }
    pthread_mutex_unlock(&ctx->sink_lock);
}

void encoder_request_idr(int idx)
{
    EncoderCtx *ctx = &g_enc[idx];
    if (ctx->active && ctx->running)
        IMP_Encoder_RequestIDR(ctx->enc_chn);
}

uint8_t *encoder_get_spspps(int idx, int *out_len)
{
    EncoderCtx       *ctx = &g_enc[idx];
    IMPEncoderStream  stream;
    uint8_t          *buf = NULL;

    if (!ctx->active) return NULL;

    IMP_Encoder_RequestIDR(ctx->enc_chn);

    if (IMP_Encoder_PollingStream(ctx->enc_chn, 2000) <= 0) return NULL;

    if (IMP_Encoder_GetStream(ctx->enc_chn, &stream, 1)) return NULL;

    int total = 0;
    for (int i = 0; i < (int)stream.packCount; i++)
        total += (int)stream.pack[i].length;

    buf = malloc(total);
    if (buf) {
        int off = 0;
        for (int i = 0; i < (int)stream.packCount; i++)
            off += extract_pack(&stream, i, buf + off);
        *out_len = off;
        free(ctx->spspps);
        ctx->spspps     = buf;
        ctx->spspps_len = off;
    }

    IMP_Encoder_ReleaseStream(ctx->enc_chn, &stream);
    return ctx->spspps;
}
