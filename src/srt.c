/* srt.c - MPEG-TS over SRT output. Only built with USE_SRT (libsrt
 * selected). Serves srt.channel's video (+AAC audio) as an MPEG-TS multiplex,
 * either to any SRT caller (listener mode, default) or by dialing out to
 * srt.host:srt.port itself (srt.mode=caller, for cameras behind NAT or on
 * unreliable links). Reads the hub like the recorder/RTSP sinks.
 *
 * NOTE: the MPEG-TS muxer here is hand-rolled and compact; it targets the
 * common case (H.264/H.265 video PID 0x100 + AAC/ADTS audio PID 0x101, PCR on
 * the video PID). It SHOULD still be verified on device with ffplay/VLC
 * ("srt://<ip>:<port>") - TS bit-twiddling is easy to get subtly wrong - but
 * `make sim USE_SRT=1` now exercises both modes against host libsrt/ffmpeg. */
#ifdef USE_SRT
#include "srt.h"
#include "hub.h"
#include "frame.h"
#include "fanqueue.h"
#include "log.h"
#include "util.h"
#include "codec/aac.h"
#include "config.h"

#include <srt/srt.h>
/* srt/srt.h drags in <syslog.h> (via logging_api.h), whose LOG_INFO/LOG_DEBUG
 * macros (6/7) shadow log.h's enum (2/3) in every later LOGI/LOGD expansion -
 * levels above the logger's range, so every LOGI in this file was silently
 * dropped. Same collision log.c handles; restore the enum. */
#undef LOG_INFO
#undef LOG_DEBUG
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <netdb.h>     /* getaddrinfo for the caller target */

#define MOD    "SRT"
#define VPID   0x0100
#define APID   0x0101
#define PMTPID 0x1000
/* global limit on concurrent SRT clients (each costs a thread + a bounded
 * fanqueue), matching RTSP_MAX_CLIENTS/HTTP_MAX_CLIENTS - previously
 * unbounded (M9), so a flood of callers could exhaust threads/memory on
 * small-RAM SoCs */
#ifndef SRT_MAX_CLIENTS
#define SRT_MAX_CLIENTS 8
#endif
/* fanqueue capacity (packet pointers; retained packet payloads are the real
 * cost, same reasoning as record.c's REC_QCAP) - at ~2K@4-6 Mbit/s, 256
 * slots could pin several MB per stalled SRT client; 128 halves that worst
 * case (M10) */
#ifndef SRT_QCAP
#define SRT_QCAP 128
#endif
/* S-1 hardening: client_thread had ZERO liveness check on a source stall -
 * `if (!p) continue;` with no idle deadline, unlike httpd.c which at least
 * attempts a crecv() probe. If the hub source stops publishing (HAL wedge),
 * every SRT client thread spins at the fanqueue_pop timeout cadence forever:
 * healthy-looking thread, zero log output, the only real recovery (a send
 * failure) requires the very packets whose absence defines the state - not
 * even libsrt's own peer-idle detection can fire since no API call is ever
 * made on the socket. A subscribed hub source is expected to always publish
 * (the on-demand design's invariant, see hub.c), so a long run of zero
 * packets is itself proof of a stall; bound the leak the same way httpd.c's
 * H-2 fix does. Generous relative to any real bitrate/fps so it never fires
 * on a healthy, merely slow stream. */
#ifndef SRT_STALL_US
#define SRT_STALL_US (60LL*1000000)
#endif
/* stats cadence: 10 s. srt_bstats(clear=1) makes the counters per-interval,
 * so one bad window shows as its own spike at this grain; ~130 B/line keeps
 * a camera's 64 KB log ring holding ~1.5 h of link history (1 s cadence
 * would cut that to minutes and bury every other module). */
#ifndef SRT_STATS_PERIOD_S
#define SRT_STATS_PERIOD_S 10
#endif
/* caller reconnect backoff (doubling, capped, wakeable - the shape of
 * main.c's HAL-init retry). Min 1 s: a restarted receiver is back within
 * seconds and one dial per second is cheap. Cap 30 s rather than HAL's 60:
 * this outage costs live footage, and two quiet attempts per minute against
 * a dead host are still negligible. */
#ifndef SRT_CALLER_BACKOFF_MIN_S
#define SRT_CALLER_BACKOFF_MIN_S 1
#endif
#ifndef SRT_CALLER_BACKOFF_MAX_S
#define SRT_CALLER_BACKOFF_MAX_S 30
#endif

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
/* M-2: every live client socket, so shutdown can CLOSE them instead of only
 * waiting for their threads. Waiting alone is not enough - a thread blocked in
 * srt_sendmsg2() because the receiver stopped ACKing unblocks on the configured
 * srt.latency_ms timescale, not on the teardown's 500 ms one, and would then
 * wake up inside an already srt_cleanup()'d library. Closing the socket wakes
 * the blocked sender at once, so the drain below has something to drain.
 * Ownership follows srt_close_listener's rule (libsrt reuses socket ids, so a
 * double close could hit an unrelated newer socket): whoever atomically swaps
 * the entry out is the one that closes it. */
static SRTSOCKET        g_client_sock[SRT_MAX_CLIENTS];
static SRTSOCKET        g_cs = SRT_INVALID_SOCK; /* caller-mode socket; same
                                                  * close-once contract as g_ls
                                                  * (srt_stop closes it to break
                                                  * a blocking connect/send) */
static int              g_caller;      /* resolved srt.mode, set before g_thr */
static volatile int     g_connected;   /* receivers currently in stream_run() */
static pthread_mutex_t  g_stats_mx = PTHREAD_MUTEX_INITIALIZER;
static ms_srt_stats     g_stats;       /* last stats_tick() sample, /control */

static int srt_client_reg(SRTSOCKET cs)
{
    for (int i = 0; i < SRT_MAX_CLIENTS; i++)
        if (__sync_bool_compare_and_swap(&g_client_sock[i], SRT_INVALID_SOCK, cs))
            return i;
    return -1;                     /* full: caller keeps sole ownership */
}

/* take the socket out of the registry and close it if it was still there;
 * a no-op when the other side got there first. */
static void srt_client_close(int slot)
{
    if (slot < 0 || slot >= SRT_MAX_CLIENTS) return;
    SRTSOCKET s = (SRTSOCKET)__sync_lock_test_and_set(&g_client_sock[slot],
                                                      SRT_INVALID_SOCK);
    if (s != SRT_INVALID_SOCK) srt_close(s);
}

#define TS_BATCH_PKTS 7          /* 7*188 = 1316B, the conventional TS-over-SRT
                                  * payload size (fits one SRT/UDP datagram
                                  * without fragmenting) */
typedef struct {
    SRTSOCKET sock;
    int       slot;        /* index in g_client_sock, -1 if unregistered */
    uint8_t   cc_pat, cc_pmt, cc_v, cc_a;
    int       vcodec;      /* MS_VC_H264 / MS_VC_H265 */
    int       have_audio;
    int       a_sr, a_ch;  /* AAC samplerate/channels for the ADTS header */
    int       a_idx;       /* sampling_frequency_index (cached) */
    uint8_t   batch[TS_BATCH_PKTS * 188];  /* accumulated, not-yet-sent packets */
    int       batch_n;                     /* packets currently in batch[] */
} ts_mux;

/* One srt_bstats() sample per SRT_STATS_PERIOD_S while a receiver is being
 * served - the numbers SRT exists for (retransmits, loss, RTT, estimated
 * bandwidth, sender-buffer backlog), previously never queried at all. The
 * line is machine-readable on purpose (key=value, no spaces inside values,
 * -1 = not measured), same contract as daynight's probe line - dashboards
 * grep these, and a reworded line would empty them silently. Like that line
 * it is LOGD, not LOGI: a sample every 10 s (8640 lines/day per receiver)
 * is a measurement, not an event. A dashboard that wants the series sets
 * general.debug_modules = srt. */
static void stats_tick(ts_mux *m)
{
    SRT_TRACEBSTATS s;
    if (srt_bstats(m->sock, &s, 1) == SRT_ERROR) return;
    LOGD(MOD, "stats: id=%d rtt_ms=%.1f bw_mbps=%.2f rate_mbps=%.2f "
              "sent=%lld retrans=%d loss=%d drop=%d sndbuf_ms=%d flight=%d",
         (int)m->sock, s.msRTT, s.mbpsBandwidth, s.mbpsSendRate,
         (long long)s.pktSent, s.pktRetrans, s.pktSndLoss, s.pktSndDrop,
         s.msSndBuf, s.pktFlightSize);
    pthread_mutex_lock(&g_stats_mx);
    g_stats.t_us      = ms_now_us();
    g_stats.rtt_ms    = s.msRTT;
    g_stats.bw_mbps   = s.mbpsBandwidth;
    g_stats.rate_mbps = s.mbpsSendRate;
    g_stats.sent      = s.pktSent;
    g_stats.retrans   = s.pktRetrans;
    g_stats.loss      = s.pktSndLoss;
    g_stats.drop      = s.pktSndDrop;
    pthread_mutex_unlock(&g_stats_mx);
}

/* /control status snapshot. With several listener clients the last tick wins
 * - acceptable, the block answers "is the link healthy", not "which one". */
void srt_get_stats(ms_srt_stats *out)
{
    pthread_mutex_lock(&g_stats_mx);
    *out = g_stats;
    pthread_mutex_unlock(&g_stats_mx);
    out->caller    = g_caller;
    out->connected = g_connected;
}

/* faac emits RAW AAC (no ADTS); MPEG-TS stream_type 0x0F needs each frame
 * framed with a 7-byte ADTS header. Build it from the actual sr/ch so the
 * decoder gets the right rate/layout (else it mis-syncs -> "7.1 / 32000 Hz"). */
static int aac_adts_wrap(const ts_mux *m, const uint8_t *aac, int aac_len,
                         uint8_t *out, int out_cap)
{
    int frame_len = aac_len + 7;                /* header + payload, no CRC */
    if (aac_len <= 0 || frame_len > out_cap || frame_len > 0x1FFF) return -1;
    int idx = m->a_idx;                         /* sampling_frequency_index */
    if (idx < 0) return -1;                     /* L2: never emit reserved idx 15 */
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

/* flush any packets accumulated in m->batch as a single srt_sendmsg2() call */
static int ts_flush(ts_mux *m)
{
    if (m->batch_n == 0) return 0;
    int n = m->batch_n; m->batch_n = 0;   /* reset before sending: on error
                                            * the caller tears the client
                                            * down anyway, and we must not
                                            * resend stale packets on retry */
    return srt_sendmsg2(m->sock, (const char *)m->batch, n * 188, NULL) < 0 ? -1 : 0;
}

/* Previously issued one srt_sendmsg2() syscall per 188-byte TS packet - a
 * single AAC/H.264 access unit can span many packets, so a busy stream did
 * one send(2)-class syscall per 188 bytes. Batch up to TS_BATCH_PKTS packets
 * (1316B, the standard TS-over-UDP/SRT payload size) into one call instead;
 * ts_flush() is called at natural boundaries (end of each PSI/PES write) so
 * packets never linger unsent for more than one access unit. */
static int ts_send(ts_mux *m, const uint8_t *pkt188)
{
    memcpy(m->batch + (size_t)m->batch_n * 188, pkt188, 188);
    if (++m->batch_n >= TS_BATCH_PKTS) return ts_flush(m);
    return 0;
}

/* Same batching, but for a producer that can write the 188 bytes in place:
 * ts_slot() hands out the next free packet slot, ts_commit() accounts for it
 * (and flushes when the batch is full, so the following ts_slot() is valid
 * again). send_pes() - the only per-access-unit producer here - used to build
 * each packet in a stack buffer that ts_send() then memcpy'd into the very
 * same array. The PSI writers keep using ts_send(): PAT/PMT go out about once
 * a second, and send_section() builds its packet before it knows the length
 * check will let it be sent at all. */
static uint8_t *ts_slot(ts_mux *m)
{
    return m->batch + (size_t)m->batch_n * 188;
}
static int ts_commit(ts_mux *m)
{
    if (++m->batch_n >= TS_BATCH_PKTS) return ts_flush(m);
    return 0;
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
        uint8_t *p = ts_slot(m); int o = 0;
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
        int n = 188 - o;
        if (n > hn) n = hn;
        if (n > 0) { memcpy(p + o, hp, (size_t)n); o += n; hp += n; hn -= n; }
        n = 188 - o;
        if (n > bn) n = bn;
        if (n > 0) { memcpy(p + o, bp, (size_t)n); o += n; bp += n; bn -= n; }
        if (o < 188) memset(p + o, 0xFF, (size_t)(188 - o));  /* should not happen */

        if (ts_commit(m) < 0) return -1;
        first = 0;
    }
    return 0;
}

/* stream the hub to m->sock until send error, encoder stall or shutdown.
 * Shared by both modes: the listener runs it once per accepted client
 * (client_thread below), the caller runs it on each dialed connection.
 * Owns neither the socket nor m - the caller does. */
static void stream_run(ts_mux *m)
{
    int chn = g_scfg->srt.channel;
    /* matches the validation httpd.c/timelapse.c use for the same config
     * field: a channel index that's in range but not actually enabled
     * (e.g. left over after the user disabled that stream) used to be
     * accepted as-is here, subscribing to a hub source with no publisher -
     * the client would then just hang with no video ever arriving instead
     * of falling back to the first enabled stream. */
    /* enabled/codec are restart-only: read the boot snapshot so a live edit
     * cannot make an unpublished channel look servable or set a PMT codec the
     * encoder is not producing. See config.h. */
    if (chn < 0 || chn >= MS_MAX_VSTREAM || !g_cfg_boot.video[chn].enabled) chn = 0;

    fanqueue q;
    if (fanqueue_init(&q, SRT_QCAP)) return;
    if (hub_subscribe(chn, &q) != 0) { fanqueue_free(&q); return; }
    __sync_fetch_and_add(&g_connected, 1);

    int ac = MS_AC_NONE, asr = 0, ach = 0, sub_a = 0;
    int have_a = hub_get_audio(&ac, &asr, &ach);
    if (have_a && ac == MS_AC_AAC)
        sub_a = (hub_subscribe(HUB_AUDIO_SRC, &q) == 0);
    else if (g_scfg->audio.enabled) {
        /* B5: the MPEG-TS mux here only carries AAC. With G.711 (the
         * USE_FAAC=0 fallback) the SRT stream is video-only - say so once
         * instead of silently serving no audio. */
        static int aac_warned;
        if (!aac_warned) { aac_warned = 1;
            LOGW(MOD, "audio.enabled=1 but codec is %s - SRT/MPEG-TS carries "
                      "AAC only, stream is video-only",
                 ac == MS_AC_PCMU ? "g711u" : ac == MS_AC_PCMA ? "g711a" :
                 have_a ? "unknown" : "none"); }
    }
    m->a_sr = asr; m->a_ch = (ach > 0 ? ach : 1);
    m->a_idx = aac_srate_index(asr);
    if (m->a_idx < 0 && sub_a) {
        /* L2: the configured rate has no ADTS sampling_frequency_index.
         * Emitting a header anyway (the old "assume 16 kHz" fallback, or
         * letting aac_adts_wrap mask -1 to the reserved index 15) makes
         * decoders reject or mis-sync the TS audio, so serve this session
         * video-only - the same shape as the no-audio case below. */
        static int warned;
        if (!warned) { warned = 1;
            LOGW(MOD, "AAC rate %d Hz has no ADTS index, SRT audio disabled", asr); }
        hub_unsubscribe(HUB_AUDIO_SRC, &q);
        sub_a = 0;
    }
    m->have_audio = sub_a;
    m->vcodec = g_cfg_boot.video[chn].codec;    /* restart-only: see config.h */
    hub_request_idr(chn);

    int got_key = 0, psi = 0; int64_t psi_t = 0;
    int64_t last_pkt_us = ms_now_us();   /* S-1: encoder-stall bound, see above */
    int64_t stats_t = last_pkt_us;
    while (g_run) {
        ms_pkt *p = fanqueue_pop(&q, 200);
        /* before the !p bail: the link stats stay interesting (and the
         * /control snapshot fresh) even while the encoder goes quiet */
        int64_t snow = ms_now_us();
        if (snow - stats_t >= SRT_STATS_PERIOD_S * 1000000LL) {
            stats_tick(m); stats_t = snow;
        }
        if (!p) {
            if (ms_now_us() - last_pkt_us > SRT_STALL_US) {
                LOGW(MOD,"chn=%d: no packets for %llds - encoder stall, "
                         "dropping this client", chn,
                     (long long)(SRT_STALL_US/1000000));
                break;
            }
            continue;
        }
        last_pkt_us = ms_now_us();
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
            /* Publishers aren't guaranteed to hand us bare raw AAC: the real
             * HW encoder path (hal_ingenic.c, FAAC_STREAM_RAW) does, but
             * hal_sim.c's test source plays back an already ADTS-framed
             * file straight into the hub. rtp.c/fmp4.c already strip any
             * existing ADTS header before doing their own framing; this
             * path didn't, so under the sim (or any future raw-vs-ADTS
             * source mismatch) it wrapped an ADTS frame in a second ADTS
             * header, corrupting the stream for every AAC decoder. */
            size_t raw_len;
            int strip_off = aac_adts_strip(p->data, p->len, &raw_len);
            uint8_t adts[8192];
            int alen = aac_adts_wrap(m, p->data + strip_off, (int)raw_len,
                                     adts, sizeof adts);
            if (alen > 0)
                rc = send_pes(m, APID, &m->cc_a, 0xC0, adts, alen,
                              p->pts_us, 0, 0);
        }
        /* flush at the end of every access unit: ts_send() only flushes
         * once TS_BATCH_PKTS packets have piled up, which would otherwise
         * let a partial batch (e.g. a small audio AU, or a video AU whose
         * packet count isn't a multiple of 7) sit unsent - potentially for
         * a long time on a low-bitrate stream - defeating live/low-latency
         * SRT playback. This keeps the batching syscall win for the common
         * multi-packet AU case while still bounding added latency to at
         * most one AU. */
        if (rc == 0) rc = ts_flush(m);
        pkt_unref(p);
        if (rc < 0) break;                               /* client gone */
    }

    hub_unsubscribe(chn, &q);
    if (sub_a) hub_unsubscribe(HUB_AUDIO_SRC, &q);
    fanqueue_free(&q);
    __sync_fetch_and_sub(&g_connected, 1);
}

/* per-client streaming thread (listener mode) */
static void *client_thread(void *arg)
{
    ts_mux *m = (ts_mux *)arg;
    stream_run(m);
    /* Found by review: srt_client_close(-1) is a no-op (see its own guard),
     * and m->slot is -1 exactly when srt_client_reg() found the registry
     * full and left this thread sole owner of m->sock (listen_thread's
     * comment: "-1 when full"). Today that can't actually happen - the
     * count-based admission gate in listen_thread() never lets g_srt_clients
     * exceed SRT_MAX_CLIENTS, so the registry can't appear full while
     * admission would still allow a new client - but if that ever changes,
     * the no-op above would leak this socket's fd silently, forever, on a
     * memory/fd-constrained embedded device. Cheap enough to just always be
     * correct rather than correct-by-invariant. */
    if (m->slot < 0) srt_close(m->sock);
    else srt_client_close(m->slot);
    free(m);
    __sync_fetch_and_sub(&g_srt_clients, 1);
    return NULL;
}

/* close the listener exactly once (L4): both listen_thread's teardown and
 * srt_stop() want to close it (srt_stop must, to break the blocking
 * srt_accept), and libsrt reuses socket ids - a double srt_close could hit an
 * unrelated newer socket. Atomically swap g_ls to invalid so whichever side
 * gets there first does the close and the other is a no-op. */
static void srt_close_listener(void)
{
    SRTSOCKET s = (SRTSOCKET)__sync_lock_test_and_set(&g_ls, SRT_INVALID_SOCK);
    if (s != SRT_INVALID_SOCK) srt_close(s);
}

/* caller-mode socket, same close-once rule: srt_stop() must be able to close
 * it to break a blocking srt_connect()/srt_sendmsg2(), else the join hangs. */
static void srt_close_caller(void)
{
    SRTSOCKET s = (SRTSOCKET)__sync_lock_test_and_set(&g_cs, SRT_INVALID_SOCK);
    if (s != SRT_INVALID_SOCK) srt_close(s);
}

/* latency + optional passphrase, shared by both modes. The passphrase result
 * MUST be checked (H3): libsrt requires a 10..79 char passphrase and fails
 * SRTO_PASSPHRASE on anything shorter - ignoring that ran the socket silently
 * UNENCRYPTED. Nonzero = refuse to run, never fall back to plaintext. */
static int srt_common_opts(SRTSOCKET s)
{
    int lat = g_scfg->srt.latency_ms;
    srt_setsockflag(s, SRTO_LATENCY, &lat, sizeof lat);
    if (g_scfg->srt.passphrase[0] &&
        srt_setsockflag(s, SRTO_PASSPHRASE, g_scfg->srt.passphrase,
                        (int)strlen(g_scfg->srt.passphrase)) == SRT_ERROR) {
        LOGE(MOD, "SRTO_PASSPHRASE rejected (need 10-79 chars): %s - "
                  "refusing to run unencrypted", srt_getlasterror_str());
        return -1;
    }
    return 0;
}

/* accept-time hook (M4): SRTO_STREAMID on a LISTENER is not access control -
 * it's a caller-side option, and the accepted socket's streamid was never
 * checked, so any caller connected regardless of the configured id. When
 * srt.streamid is set, require the incoming caller's streamid to match and
 * reject the handshake otherwise (nonzero return = reject). */
static int listen_cb(void *opaq, SRTSOCKET ns, int hsversion,
                     const struct sockaddr *peeraddr, const char *streamid)
{
    (void)opaq; (void)ns; (void)hsversion; (void)peeraddr;
    if (!g_scfg->srt.streamid[0]) return 0;           /* not configured */
    if (streamid && strcmp(streamid, g_scfg->srt.streamid) == 0) return 0;
    LOGW(MOD, "rejecting caller with missing/wrong streamid");
    return -1;
}

static void *listen_thread(void *arg)
{
    (void)arg;
    if (srt_startup() < 0) { LOGE(MOD, "srt_startup failed"); return NULL; }
    /* SRT_INVALID_SOCK is not 0, so the static zero-init would make slot 0 look
     * like it already held socket 0. Set the empty marker explicitly. */
    for (int i = 0; i < SRT_MAX_CLIENTS; i++) g_client_sock[i] = SRT_INVALID_SOCK;

    SRTSOCKET ls = srt_create_socket();
    if (ls == SRT_INVALID_SOCK) { LOGE(MOD, "create_socket"); srt_cleanup(); return NULL; }

    if (g_scfg->srt.streamid[0])
        srt_listen_callback(ls, listen_cb, NULL);     /* enforce streamid (M4) */
    if (srt_common_opts(ls) < 0) {          /* H3: no unencrypted fallback */
        srt_close(ls); srt_cleanup(); return NULL;
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
        /* concurrent-client cap (M9): each client costs a thread + bounded
         * fanqueue, same reasoning as RTSP/HTTP's caps. libsrt has already
         * completed the connection by the time srt_accept() returns it, so
         * "reject" here means accept-then-close, same pattern rtsp.c/httpd.c
         * use for their own accept-time caps. */
        if (g_srt_clients >= SRT_MAX_CLIENTS) {
            LOGW(MOD, "client limit (%d) reached, rejecting", SRT_MAX_CLIENTS);
            srt_close(cs);
            continue;
        }
        ts_mux *m = calloc(1, sizeof *m);
        if (!m) { srt_close(cs); continue; }
        m->slot = srt_client_reg(cs);   /* -1 when full: this thread stays sole owner */
        m->sock = cs;
        __sync_fetch_and_add(&g_srt_clients, 1);
        pthread_t t;
        if (ms_thread_create(&t, MS_STACK_CONN, client_thread, m) == 0) pthread_detach(t);
        else {
            /* cs is PUBLISHED in the registry by now (srt_client_reg above), so
             * closing it directly would leave g_client_sock[m->slot] naming a
             * closed socket id - and libsrt reuses ids, so the teardown sweep
             * below (or srt_stop's) would then close whatever unrelated newer
             * socket inherited it. Same reason srt_close_listener() exists (L4).
             * Take it back out of the registry instead; srt_close(cs) is only
             * correct when registration failed and this thread is sole owner.
             * Mirrors client_thread's exit exactly - see its comment. */
            if (m->slot < 0) srt_close(cs);
            else srt_client_close(m->slot);
            free(m); __sync_fetch_and_sub(&g_srt_clients, 1);
        }
    }
    srt_close_listener();      /* no-op if srt_stop() already closed it (L4) */
    /* M-2: close every live client socket BEFORE draining. g_run=0 alone only
     * releases threads sitting in fanqueue_pop (~200 ms); one blocked in
     * srt_sendmsg2() because its receiver stopped ACKing waits on the
     * srt.latency_ms timescale instead and would outlive the 500 ms window
     * below, then wake inside an already srt_cleanup()'d library. Closing the
     * socket errors that send out immediately, so the drain has something to
     * drain rather than a deadline it silently blows past. The swap inside
     * srt_client_close() keeps this safe against the owning thread closing the
     * same socket concurrently. */
    for (int i = 0; i < SRT_MAX_CLIENTS; i++) srt_client_close(i);
    for (int i = 0; i < 50 && g_srt_clients > 0; i++) usleep(10000);
    if (g_srt_clients > 0)
        LOGW(MOD,"%d client thread(s) still in libsrt after the drain - "
                 "proceeding to srt_cleanup()", g_srt_clients);
    srt_cleanup();
    return NULL;
}

/* caller mode: dial srt.host:srt.port and stream, reconnecting forever with
 * the capped backoff above. The whole point of this mode is an unreliable
 * path, so an outage is logged exactly ONCE (on loss / first failed dial),
 * not once per attempt - the reconnect itself then announces recovery. */
static void *caller_thread(void *arg)
{
    (void)arg;
    if (srt_startup() < 0) { LOGE(MOD, "srt_startup failed"); return NULL; }

    const char *host = g_scfg->srt.host;   /* persist-only keys: stable */
    int port = g_scfg->srt.port;
    int backoff = SRT_CALLER_BACKOFF_MIN_S;
    int quiet = 0;                         /* 1 = this outage already logged */
    LOGI(MOD, "SRT caller to %s:%d (MPEG-TS, chn %d)",
         host, port, g_scfg->srt.channel);

    while (g_run) {
        SRTSOCKET s = srt_create_socket();
        if (s == SRT_INVALID_SOCK) {
            LOGE(MOD, "create_socket: %s", srt_getlasterror_str());
            break;
        }
        if (srt_common_opts(s) < 0) {      /* H3: no unencrypted fallback */
            srt_close(s); break;
        }
        /* on a caller SRTO_STREAMID is what actually reaches the peer */
        if (g_scfg->srt.streamid[0])
            srt_setsockflag(s, SRTO_STREAMID, g_scfg->srt.streamid,
                            (int)strlen(g_scfg->srt.streamid));

        const char *why = NULL;
        struct addrinfo hints, *ai = NULL;
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
        char ps[8]; snprintf(ps, sizeof ps, "%d", port);
        int gaerr = getaddrinfo(host, ps, &hints, &ai);
        g_cs = s;   /* publish first: srt_stop() closing g_cs is what breaks a
                     * blocking srt_connect (worst case the default 3 s
                     * SRTO_CONNTIMEO bounds the join if stop wins the race) */
        if (gaerr != 0) {
            why = gai_strerror(gaerr);
        } else {
            if (srt_connect(s, ai->ai_addr, (int)ai->ai_addrlen) == SRT_ERROR)
                why = srt_getlasterror_str();
            freeaddrinfo(ai);
        }

        if (!why && g_run) {
            LOGI(MOD, "connected to %s:%d", host, port);
            quiet = 0;
            ts_mux m; memset(&m, 0, sizeof m);
            m.sock = s; m.slot = -1;
            int64_t t0 = ms_now_us();
            stream_run(&m);        /* returns on send error / stall / stop */
            srt_close_caller();
            /* only a session that actually held earns the fast backoff again;
             * a peer that accepts and instantly drops keeps backing off like
             * a failed dial, so a flapper cannot cycle (and log) at 1 s */
            if (ms_now_us() - t0 >= 5*1000000LL)
                backoff = SRT_CALLER_BACKOFF_MIN_S;
            if (g_run) {
                LOGW(MOD, "connection to %s:%d lost - reconnecting "
                          "(quiet retries, backoff up to %ds)",
                     host, port, SRT_CALLER_BACKOFF_MAX_S);
                quiet = 1;
            }
        } else {
            srt_close_caller();
            if (g_run && !quiet) {
                LOGW(MOD, "connect to %s:%d failed: %s - retrying "
                          "(quiet retries, backoff up to %ds)",
                     host, port, why ? why : "?", SRT_CALLER_BACKOFF_MAX_S);
                quiet = 1;
            }
        }
        /* wakeable backoff: poll g_run at 100 ms so srt_stop's join never
         * waits out a full 30 s sleep */
        for (int i = 0; g_run && i < backoff * 10; i++) usleep(100000);
        if (backoff < SRT_CALLER_BACKOFF_MAX_S) {
            backoff *= 2;
            if (backoff > SRT_CALLER_BACKOFF_MAX_S)
                backoff = SRT_CALLER_BACKOFF_MAX_S;
        }
    }
    srt_close_caller();
    srt_cleanup();
    return NULL;
}

void srt_start(const ms_config *cfg)
{
    if (g_started || !cfg->srt.enabled) return;
    g_scfg = cfg;
    /* unknown srt.mode falls back to listener - the pre-mode behavior */
    g_caller = (strcmp(cfg->srt.mode, "caller") == 0);
    if (!g_caller && cfg->srt.mode[0] && strcmp(cfg->srt.mode, "listener"))
        LOGW(MOD, "unknown srt.mode '%s' - using listener", cfg->srt.mode);
    if (g_caller && !cfg->srt.host[0]) {
        LOGE(MOD, "srt.mode=caller but srt.host is empty - SRT disabled");
        return;
    }
    /* /control shows -1 until the first stats tick */
    g_stats.rtt_ms = g_stats.bw_mbps = g_stats.rate_mbps = -1;
    g_stats.sent = g_stats.retrans = g_stats.loss = g_stats.drop = -1;
    g_run = 1; g_started = 1;
    if (ms_thread_create(&g_thr, MS_STACK_STREAM,
                         g_caller ? caller_thread : listen_thread, NULL) != 0) {
        g_started = 0; g_run = 0;
    }
}

void srt_stop(void)
{
    if (!g_started) return;
    g_run = 0;
    srt_close_listener();      /* unblock srt_accept; closes at most once (L4) */
    srt_close_caller();        /* unblock srt_connect/srt_sendmsg2 (caller) */
    pthread_join(g_thr, NULL);
    g_started = 0;
}

#else /* !USE_SRT */
#include "srt.h"
void srt_start(const ms_config *cfg) { (void)cfg; }
void srt_stop(void) {}
void srt_get_stats(ms_srt_stats *out) { *out = (ms_srt_stats){0}; }
#endif
