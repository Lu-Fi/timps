/* srt.c - MPEG-TS over SRT output (listener mode). Only built with USE_SRT
 * (libsrt selected). Serves srt.channel's video (+AAC audio) as an MPEG-TS
 * multiplex to any SRT caller. Reads the hub like the recorder/RTSP sinks.
 *
 * NOTE: the MPEG-TS muxer here is hand-rolled and compact; it targets the
 * common case (H.264/H.265 video PID 0x100 + AAC/ADTS audio PID 0x101, PCR on
 * the video PID). It compiles standalone but SHOULD be verified on device with
 * ffplay/VLC ("srt://<ip>:<port>") - TS bit-twiddling is easy to get subtly
 * wrong and cannot be exercised in the x86 sim without libsrt. */
#ifdef USE_SRT
#include "srt.h"
#include "hub.h"
#include "frame.h"
#include "fanqueue.h"
#include "log.h"
#include "util.h"
#include "codec/aac.h"

#include <srt/srt.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#define MOD    "SRT"
#define VPID   0x0100
#define APID   0x0101
#define PMTPID 0x1000
#define SRT_QCAP 256

static const ms_config *g_scfg;
static volatile int     g_run;
static pthread_t        g_thr;
static int              g_started;
static SRTSOCKET        g_ls = SRT_INVALID_SOCK; /* listener; srt_stop closes
                                                  * it to break the blocking
                                                  * srt_accept (else the join
                                                  * hangs forever) */
static volatile int     g_srt_clients;  /* in-flight client threads (sync
                                         * builtins): drained before the global
                                         * srt_cleanup() on shutdown */

typedef struct {
    SRTSOCKET sock;
    uint8_t   cc_pat, cc_pmt, cc_v, cc_a;
    int       vcodec;      /* MS_VC_H264 / MS_VC_H265 */
    int       have_audio;
    int       a_sr, a_ch;  /* AAC samplerate/channels for the ADTS header */
    int       a_idx;       /* sampling_frequency_index (cached) */
} ts_mux;

/* faac emits RAW AAC (no ADTS); MPEG-TS stream_type 0x0F needs each frame
 * framed with a 7-byte ADTS header. Build it from the actual sr/ch so the
 * decoder gets the right rate/layout (else it mis-syncs -> "7.1 / 32000 Hz"). */
static int aac_adts_wrap(const ts_mux *m, const uint8_t *aac, int aac_len,
                         uint8_t *out, int out_cap)
{
    int frame_len = aac_len + 7;                /* header + payload, no CRC */
    if (aac_len <= 0 || frame_len > out_cap || frame_len > 0x1FFF) return -1;
    int idx = m->a_idx;                         /* sampling_frequency_index */
    int ch  = (m->a_ch > 0 && m->a_ch < 8) ? m->a_ch : 1;
    out[0] = 0xFF;
    out[1] = 0xF1;                              /* MPEG-4, layer 0, no CRC */
    out[2] = (uint8_t)((1 << 6) | ((idx & 0x0F) << 2) | ((ch >> 2) & 0x01));
    out[3] = (uint8_t)(((ch & 0x03) << 6) | ((frame_len >> 11) & 0x03));
    out[4] = (uint8_t)((frame_len >> 3) & 0xFF);
    out[5] = (uint8_t)(((frame_len & 0x07) << 5) | 0x1F);   /* buf fullness hi */
    out[6] = 0xFC;                              /* buf fullness lo + 0 blocks */
    memcpy(out + 7, aac, (size_t)aac_len);
    return frame_len;
}

static uint32_t crc32_mpeg(const uint8_t *d, int len)
{
    uint32_t crc = 0xFFFFFFFF;
    for (int i = 0; i < len; i++) {
        crc ^= (uint32_t)d[i] << 24;
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : (crc << 1);
    }
    return crc;
}

static int ts_send(ts_mux *m, const uint8_t *pkt188)
{
    return srt_sendmsg2(m->sock, (const char *)pkt188, 188, NULL) < 0 ? -1 : 0;
}

static int send_section(ts_mux *m, int pid, uint8_t *cc, const uint8_t *sec, int n)
{
    uint8_t p[188]; memset(p, 0xFF, sizeof p);
    p[0] = 0x47;
    p[1] = 0x40 | ((pid >> 8) & 0x1F);       /* payload_unit_start_indicator */
    p[2] = pid & 0xFF;
    p[3] = 0x10 | (*cc & 0x0F); *cc = (*cc + 1) & 0x0F;
    p[4] = 0x00;                              /* pointer_field */
    if (n > 183) return -1;
    memcpy(p + 5, sec, n);
    return ts_send(m, p);
}

static int send_pat(ts_mux *m)
{
    uint8_t s[16]; int i = 0;
    s[i++] = 0x00;                            /* table_id */
    s[i++] = 0xB0; s[i++] = 0x0D;            /* section_syntax + length 13 */
    s[i++] = 0x00; s[i++] = 0x01;            /* transport_stream_id */
    s[i++] = 0xC1;                           /* version 0, current */
    s[i++] = 0x00; s[i++] = 0x00;            /* section 0 of 0 */
    s[i++] = 0x00; s[i++] = 0x01;            /* program_number 1 */
    s[i++] = 0xE0 | ((PMTPID >> 8) & 0x1F); s[i++] = PMTPID & 0xFF;
    uint32_t crc = crc32_mpeg(s, i);
    s[i++] = crc >> 24; s[i++] = crc >> 16; s[i++] = crc >> 8; s[i++] = crc;
    return send_section(m, 0x0000, &m->cc_pat, s, i);
}

static int send_pmt(ts_mux *m)
{
    uint8_t s[32]; int i = 0;
    int vstype = (m->vcodec == MS_VC_H265) ? 0x24 : 0x1B;
    s[i++] = 0x02;                           /* table_id PMT */
    int lp = i; s[i++] = 0xB0; s[i++] = 0x00;/* section_length (fixed below) */
    s[i++] = 0x00; s[i++] = 0x01;            /* program_number */
    s[i++] = 0xC1; s[i++] = 0x00; s[i++] = 0x00;
    s[i++] = 0xE0 | ((VPID >> 8) & 0x1F); s[i++] = VPID & 0xFF; /* PCR PID */
    s[i++] = 0xF0; s[i++] = 0x00;            /* program_info_length 0 */
    s[i++] = vstype; s[i++] = 0xE0 | ((VPID >> 8) & 0x1F); s[i++] = VPID & 0xFF;
    s[i++] = 0xF0; s[i++] = 0x00;
    if (m->have_audio) {
        s[i++] = 0x0F; s[i++] = 0xE0 | ((APID >> 8) & 0x1F); s[i++] = APID & 0xFF;
        s[i++] = 0xF0; s[i++] = 0x00;
    }
    int seclen = (i - (lp + 2)) + 4;         /* bytes after length field incl CRC */
    s[lp]     = 0xB0 | ((seclen >> 8) & 0x0F);
    s[lp + 1] = seclen & 0xFF;
    uint32_t crc = crc32_mpeg(s, i);
    s[i++] = crc >> 24; s[i++] = crc >> 16; s[i++] = crc >> 8; s[i++] = crc;
    return send_section(m, PMTPID, &m->cc_pmt, s, i);
}

/* one PES (with PTS) for an access unit, chunked into TS packets on `pid`.
 * First video packet carries an adaptation field with PCR + random-access. */
static int send_pes(ts_mux *m, int pid, uint8_t *cc, int stream_id,
                    const uint8_t *data, int len, int64_t pts_us,
                    int is_video, int keyframe)
{
    int64_t pts = (pts_us > 0 ? pts_us : 0) * 9 / 100;   /* us -> 90 kHz */

    uint8_t hdr[19]; int h = 0;
    hdr[h++] = 0x00; hdr[h++] = 0x00; hdr[h++] = 0x01; hdr[h++] = (uint8_t)stream_id;
    int pes_len = is_video ? 0 : (len + 8);              /* 0 = unbounded (video) */
    hdr[h++] = (pes_len >> 8) & 0xFF; hdr[h++] = pes_len & 0xFF;
    hdr[h++] = 0x80;                                     /* marker bits */
    hdr[h++] = 0x80;                                     /* PTS_DTS = PTS only */
    hdr[h++] = 0x05;                                     /* PES header data length */
    hdr[h++] = 0x21 | ((pts >> 29) & 0x0E);
    hdr[h++] = (pts >> 22) & 0xFF;
    hdr[h++] = 0x01 | ((pts >> 14) & 0xFE);
    hdr[h++] = (pts >> 7) & 0xFF;
    hdr[h++] = 0x01 | ((pts << 1) & 0xFE);

    const uint8_t *hp = hdr; int hn = h;
    const uint8_t *bp = data; int bn = len;
    int first = 1;

    while (hn > 0 || bn > 0) {
        uint8_t p[188]; int o = 0;
        p[o++] = 0x47;
        p[o++] = (first ? 0x40 : 0x00) | ((pid >> 8) & 0x1F);
        p[o++] = pid & 0xFF;

        int remain = hn + bn;
        int want_pcr = (first && is_video);
        int need_af  = want_pcr || (remain < 184);

        if (need_af) {
            p[o++] = 0x30 | (*cc & 0x0F);                /* adaptation + payload */
            *cc = (*cc + 1) & 0x0F;
            int aflen_pos = o; p[o++] = 0;               /* adaptation_field_length */
            uint8_t flags = 0;
            if (want_pcr) flags |= 0x10;                 /* PCR present */
            if (first && is_video && keyframe) flags |= 0x40; /* random access */
            p[o++] = flags;
            if (want_pcr) {
                int64_t pcr = pts;                       /* base; ext 0 */
                p[o++] = (pcr >> 25) & 0xFF;
                p[o++] = (pcr >> 17) & 0xFF;
                p[o++] = (pcr >> 9) & 0xFF;
                p[o++] = (pcr >> 1) & 0xFF;
                p[o++] = ((pcr & 1) << 7) | 0x7E;
                p[o++] = 0x00;
            }
            /* stuff so header+payload exactly fills 184 */
            int payload_room = 184 - (o - 4);
            int payload_now = (hn + bn);
            if (payload_now < payload_room) {
                int stuff = payload_room - payload_now;
                memmove(p + o + stuff, p + o, 0);        /* no-op; clarity */
                for (int k = 0; k < stuff; k++) p[o + k] = 0xFF;
                o += stuff;
            }
            p[aflen_pos] = (uint8_t)(o - aflen_pos - 1);
        } else {
            p[o++] = 0x10 | (*cc & 0x0F);
            *cc = (*cc + 1) & 0x0F;
        }

        /* fill remaining bytes of this 188 packet with header then payload */
        while (o < 188 && hn > 0) { p[o++] = *hp++; hn--; }
        while (o < 188 && bn > 0) { p[o++] = *bp++; bn--; }
        while (o < 188) p[o++] = 0xFF;                   /* should not happen */

        if (ts_send(m, p) < 0) return -1;
        first = 0;
    }
    return 0;
}

/* per-client streaming thread */
static void *client_thread(void *arg)
{
    ts_mux *m = (ts_mux *)arg;
    int chn = g_scfg->srt.channel;
    if (chn < 0 || chn >= MS_MAX_VSTREAM) chn = 0;

    fanqueue q;
    if (fanqueue_init(&q, SRT_QCAP)) { srt_close(m->sock); free(m);
        __sync_fetch_and_sub(&g_srt_clients, 1); return NULL; }
    if (hub_subscribe(chn, &q) != 0) { fanqueue_free(&q); srt_close(m->sock); free(m);
        __sync_fetch_and_sub(&g_srt_clients, 1); return NULL; }

    int ac = MS_AC_NONE, asr = 0, ach = 0, sub_a = 0;
    if (hub_get_audio(&ac, &asr, &ach) && ac == MS_AC_AAC)
        sub_a = (hub_subscribe(HUB_AUDIO_SRC, &q) == 0);
    m->have_audio = sub_a;
    m->a_sr = asr; m->a_ch = (ach > 0 ? ach : 1);
    m->a_idx = aac_srate_index(asr);
    if (m->a_idx < 0) m->a_idx = 8;   /* 16 kHz fallback */
    m->vcodec = g_scfg->video[chn].codec;
    hub_request_idr(chn);

    int got_key = 0, psi = 0; int64_t psi_t = 0;
    while (g_run) {
        ms_pkt *p = fanqueue_pop(&q, 200);
        if (!p) continue;
        if (fanqueue_take_dropped_key(&q)) hub_request_idr(chn);

        /* (re)send PAT/PMT ~every second and before the first packet */
        int64_t now = ms_now_us();
        if (!psi || now - psi_t > 1000000) {
            if (send_pat(m) < 0 || send_pmt(m) < 0) { pkt_unref(p); break; }
            psi = 1; psi_t = now;
        }

        int rc = 0;
        if (p->media == MS_MEDIA_VIDEO) {
            if (!got_key) { if (!p->keyframe) { pkt_unref(p); continue; } got_key = 1; }
            rc = send_pes(m, VPID, &m->cc_v, 0xE0, p->data, (int)p->len,
                          p->pts_us, 1, p->keyframe);
        } else if (p->media == MS_MEDIA_AUDIO && m->have_audio && got_key) {
            /* wrap the raw AAC access unit in an ADTS header for TS */
            uint8_t adts[8192];
            int alen = aac_adts_wrap(m, p->data, (int)p->len, adts, sizeof adts);
            if (alen > 0)
                rc = send_pes(m, APID, &m->cc_a, 0xC0, adts, alen,
                              p->pts_us, 0, 0);
        }
        pkt_unref(p);
        if (rc < 0) break;                               /* client gone */
    }

    hub_unsubscribe(chn, &q);
    if (sub_a) hub_unsubscribe(HUB_AUDIO_SRC, &q);
    fanqueue_free(&q);
    srt_close(m->sock);
    free(m);
    __sync_fetch_and_sub(&g_srt_clients, 1);
    return NULL;
}

static void *listen_thread(void *arg)
{
    (void)arg;
    if (srt_startup() < 0) { LOGE(MOD, "srt_startup failed"); return NULL; }
    SRTSOCKET ls = srt_create_socket();
    if (ls == SRT_INVALID_SOCK) { LOGE(MOD, "create_socket"); srt_cleanup(); return NULL; }

    int lat = g_scfg->srt.latency_ms;
    srt_setsockflag(ls, SRTO_LATENCY, &lat, sizeof lat);
    if (g_scfg->srt.streamid[0])
        srt_setsockflag(ls, SRTO_STREAMID, g_scfg->srt.streamid, (int)strlen(g_scfg->srt.streamid));
    if (g_scfg->srt.passphrase[0]) {
        srt_setsockflag(ls, SRTO_PASSPHRASE, g_scfg->srt.passphrase, (int)strlen(g_scfg->srt.passphrase));
    }

    struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_addr.s_addr = INADDR_ANY;
    sa.sin_port = htons((uint16_t)g_scfg->srt.port);
    if (srt_bind(ls, (struct sockaddr *)&sa, sizeof sa) == SRT_ERROR ||
        srt_listen(ls, 4) == SRT_ERROR) {
        LOGE(MOD, "bind/listen on %d failed: %s", g_scfg->srt.port, srt_getlasterror_str());
        srt_close(ls); srt_cleanup(); return NULL;
    }
    LOGI(MOD, "SRT listener on port %d (MPEG-TS, chn %d)", g_scfg->srt.port, g_scfg->srt.channel);
    g_ls = ls;   /* publish so srt_stop can close it and unblock accept */

    while (g_run) {
        struct sockaddr_storage peer; int plen = sizeof peer;
        /* srt_accept blocks; srt_stop closes g_ls to break it */
        SRTSOCKET cs = srt_accept(ls, (struct sockaddr *)&peer, &plen);
        if (cs == SRT_INVALID_SOCK) { if (g_run) usleep(100000); continue; }
        ts_mux *m = calloc(1, sizeof *m);
        if (!m) { srt_close(cs); continue; }
        m->sock = cs;
        __sync_fetch_and_add(&g_srt_clients, 1);
        pthread_t t;
        if (pthread_create(&t, NULL, client_thread, m) == 0) pthread_detach(t);
        else { srt_close(cs); free(m); __sync_fetch_and_sub(&g_srt_clients, 1); }
    }
    srt_close(ls);
    /* detached client threads may still be inside srt_sendmsg2(): give them a
     * bounded window to drain (g_run=0 pops them out of fanqueue_pop within
     * ~200 ms) before the global libsrt teardown - else use-after-cleanup */
    for (int i = 0; i < 50 && g_srt_clients > 0; i++) usleep(10000);
    srt_cleanup();
    return NULL;
}

void srt_start(const ms_config *cfg)
{
    if (g_started || !cfg->srt.enabled) return;
    g_scfg = cfg; g_run = 1; g_started = 1;
    if (pthread_create(&g_thr, NULL, listen_thread, NULL) != 0) { g_started = 0; g_run = 0; }
}

void srt_stop(void)
{
    if (!g_started) return;
    g_run = 0;
    if (g_ls != SRT_INVALID_SOCK) srt_close(g_ls);  /* unblock srt_accept */
    pthread_join(g_thr, NULL);
    g_ls = SRT_INVALID_SOCK;
    g_started = 0;
}

#else /* !USE_SRT */
#include "srt.h"
void srt_start(const ms_config *cfg) { (void)cfg; }
void srt_stop(void) {}
#endif
