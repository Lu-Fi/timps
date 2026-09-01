/* backchannel.c - see backchannel.h. Compiled only when USE_BACKCHANNEL. */
#ifdef USE_BACKCHANNEL
#include "backchannel.h"
#include "speaker.h"
#include "../log.h"
#include "../util.h"
#include "../codec/g711.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#ifdef USE_BC_AAC
#include <aacdec.h>              /* libhelix-aac (declares everything we use) */
#endif

#define MOD "bc"

/* -------- config + backchannel session election -------------------------- */
/* g_lock/g_owner elect ONE backchannel session as the RTP->PCM decoder (other
 * sessions' frames are dropped until it releases); that session then hands the
 * decoded PCM to speaker.c, which arbitrates the physical speaker (IMP_AO)
 * between backchannel and the play queue. */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static const void *g_owner   = NULL;   /* backchannel session that owns decode */
static int         g_codec   = BC_CODEC_PCMU;
static int         g_out_rate = 16000;
/* 1 once bc_configure() has run, i.e. audio.backchannel was enabled AT BOOT.
 * main.c calls bc_configure() only when g_cfg.audio.backchannel is set at
 * startup, so this is the honest "backchannel was configured and initialised
 * at startup" flag: the decode pipeline's codec/rate (g_codec/g_out_rate) are
 * only valid after that call. audio.backchannel is restart-only (AUD_REST),
 * so rtsp.c must gate on this, not on the live config value. */
static int         g_configured = 0;
static int64_t     g_owner_last_us = 0;   /* mono clock of the owner's last accepted frame */
/* F2 hardening: g_owner used to latch until the elected session's own
 * bc_release() (session teardown) - a standing connection that SETUP the
 * backchannel, sent a few frames, and then simply stops talking (but keeps
 * the RTSP session open via keepalives, e.g. an ONVIF NVR that only
 * occasionally talks) held the decode election for as long as the session
 * stayed connected, silently dropping every other session's talk frames at
 * the `g_owner != owner` check below with no log and no time-based recovery.
 * Re-elect after this much silence from the current owner instead of only on
 * an explicit release. Generous vs typical backchannel RTP packetization
 * (20-40ms) so a mid-sentence pause never causes a spurious steal. */
#ifndef BC_OWNER_STALE_US
#define BC_OWNER_STALE_US (10LL*1000000)
#endif

/* scratch buffer - only ever touched by the elected owner under g_lock */
static int16_t g_pcm[8192];     /* decoded PCM (mono) */

#ifdef USE_BC_AAC
static HAACDecoder g_aac = NULL;
/* worst-case samples a single AACDecode can emit into g_pcm (libhelix:
 * AAC_MAX_NCHANS * AAC_MAX_NSAMPS = 2 * 1024). Used as the M-B1 headroom guard
 * so the decoder can never write past g_pcm[] before `total` is clamped. */
#define BC_AAC_MAX_BLK 2048
#endif

void bc_configure(int codec, int out_rate)
{
    if (codec < BC_CODEC_PCMU || codec > BC_CODEC_AAC) codec = BC_CODEC_PCMU;
#ifndef USE_BC_AAC
    if (codec == BC_CODEC_AAC) codec = BC_CODEC_PCMU;   /* no AAC decoder built */
#endif
    if (out_rate < 8000 || out_rate > 48000) out_rate = 16000;
    g_codec = codec; g_out_rate = out_rate;
    g_configured = 1;
}

int bc_available(void)
{
    /* Native IMP_AO now: no external /bin/iac dependency, so the ONLY thing to
     * probe is whether the backchannel was actually enabled AND configured at
     * startup (bc_configure ran). audio.backchannel is restart-only: it, plus
     * backchannel_codec/backchannel_rate, are read once at boot to set up the
     * decode/speaker pipeline. Reporting the LIVE config value here (as rtsp.c
     * used to) let a /control enable advertise/accept a backchannel that then
     * ran on compile-time defaults (PCMU/16000) instead of the configured
     * codec/rate until a real restart. Gating on the boot-time flag keeps the
     * feature honestly restart-only. The AO device itself is still opened
     * lazily on the first frame; a bring-up failure there yields silence. */
    return g_configured;
}

int bc_payload_type(void)
{
    switch (g_codec){ case BC_CODEC_PCMA: return 8; case BC_CODEC_AAC: return 97; default: return 0; }
}
const char *bc_rtpmap_name(void)
{
    switch (g_codec){ case BC_CODEC_PCMA: return "PCMA"; case BC_CODEC_AAC: return "mpeg4-generic"; default: return "PCMU"; }
}
int bc_clock_rate(void)
{
    return (g_codec == BC_CODEC_AAC) ? g_out_rate : 8000;
}

/* -------- RTP + decode --------------------------------------------------- */
/* strip the RTP header (handles CSRC + one extension); returns payload offset,
 * or -1 if malformed. */
static int rtp_payload_off(const uint8_t *p, int len)
{
    if (len < 12 || (p[0]>>6) != 2) return -1;      /* version 2 */
    int cc = p[0] & 0x0F;
    int off = 12 + 4*cc;
    if (p[0] & 0x10){                               /* extension present */
        if (len < off+4) return -1;
        int extw = (p[off+2]<<8) | p[off+3];
        off += 4 + 4*extw;
    }
    if (off >= len) return -1;
    return off;
}

#ifdef USE_BC_AAC
/* decode an RFC3640 (mpeg4-generic) AAC payload -> g_pcm. Handles multiple AUs
 * per packet (each AU-header is sizelength(13)+indexdeltalength(3)=16 bits).
 * Returns total sample count; *out_rate gets the decoder's real output rate. */
static int decode_aac(const uint8_t *pl, int plen, int *out_rate)
{
    if (!g_aac){
        g_aac = AACInitDecoder();
        if (!g_aac){ LOGW(MOD,"AACInitDecoder failed"); return 0; }
        AACFrameInfo fi; memset(&fi,0,sizeof fi);
        fi.nChans = 1; fi.sampRateCore = g_out_rate; fi.profile = AAC_PROFILE_LC;
        AACSetRawBlockParams(g_aac, 0, &fi);
    }
    if (plen < 4) return 0;
    int au_hdr_bits  = (pl[0]<<8) | pl[1];
    int au_hdr_bytes = (au_hdr_bits + 7) / 8;
    int naus = au_hdr_bytes / 2;                 /* each AU-header is 16 bits */
    int data_off = 2 + au_hdr_bytes;
    if (naus < 1 || plen < data_off) return 0;

    int cap = (int)(sizeof g_pcm / sizeof g_pcm[0]);
    int total = 0, rate = g_out_rate;
    /* M-B1: AACDecode writes up to AAC_MAX_NCHANS*AAC_MAX_NSAMPS (2*1024) int16
     * per call into g_pcm+total BEFORE we can clamp `total`. A packet carrying
     * many AU-headers (naus is derived from the network payload) could otherwise
     * drive total near cap and let one more decode write past g_pcm[8192].
     * Gate the loop on room for a FULL worst-case block, not just total<cap. */
    for (int a=0; a<naus && data_off < plen && total + BC_AAC_MAX_BLK <= cap; a++){
        int sz = ((pl[2+2*a]<<8) | pl[3+2*a]) >> 3;   /* 13-bit AU-size */
        if (sz <= 0 || data_off + sz > plen) sz = plen - data_off;
        if (sz <= 0) break;
        unsigned char *in = (unsigned char*)pl + data_off;
        int left = sz;
        if (AACDecode(g_aac, &in, &left, g_pcm + total) < 0){ data_off += sz; continue; }
        AACFrameInfo out; AACGetLastFrameInfo(g_aac, &out);
        if (out.sampRateOut > 0) rate = out.sampRateOut;
        int ns = out.outputSamps;
        if (ns > 0){ total += ns; if (total > cap) total = cap; }
        data_off += sz;
    }
    if (out_rate) *out_rate = rate;
    return total;
}
#endif

/* Single-talker election. CALLER MUST HOLD g_lock.
 *
 * Returns 1 if `owner` holds (or has just won) the decode election, having
 * stamped g_owner_last_us; 0 if another producer owns it and is not yet stale.
 *
 * Extracted from bc_feed_rtp() so the non-RTP producers (bc_feed_pcm below,
 * used by the WebSocket talk endpoint) arbitrate against RTSP talkers through
 * exactly this policy rather than a second, divergent one. It REQUIRES the
 * lock instead of taking it so that bc_feed_rtp() can keep its single locked
 * region: see the AAC note below for why that shape matters.
 *
 * The steal branch frees the AAC decoder so no state from the previous
 * owner's stream leaks into the new one. That free is safe here and ONLY
 * here: every caller that can be inside AACDecode holds g_lock across the
 * whole decode, so no thread can be executing in the decoder while this frees
 * it. Do NOT "optimise" bc_feed_rtp() by moving its decode out of the locked
 * region - that invariant is what keeps this line from being a use-after-free
 * once a second producer can trigger a steal. */
static int bc_elect_locked(const void *owner, int64_t now)
{
    if (g_owner == NULL){                 /* first talker becomes decode owner */
        g_owner = owner;
        LOGI(MOD,"backchannel decode owner acquired");
    } else if (g_owner != owner && now - g_owner_last_us > BC_OWNER_STALE_US){
        /* F2: previous owner has gone quiet for a while - steal the election
         * rather than dropping this talker forever. Reset the decoder so no
         * AAC state from the old owner's stream leaks into the new one. */
#ifdef USE_BC_AAC
        if (g_aac){ AACFreeDecoder(g_aac); g_aac=NULL; }
#endif
        LOGI(MOD,"backchannel decode owner re-elected (previous was idle >%llds)",
             (long long)(BC_OWNER_STALE_US/1000000));
        g_owner = owner;
    }
    if (g_owner != owner) return 0;       /* not the owner */
    g_owner_last_us = now;
    return 1;
}

/* Feed ALREADY-DECODED mono PCM, subject to the same election. See
 * backchannel.h. Locking sequence, deliberately different from bc_feed_rtp():
 *
 *   lock -> elect (+stamp) -> unlock -> speaker_write_pcm
 *
 * There is no decode step inside, so nothing shared is touched after the
 * unlock: `pcm` belongs to the caller (talk_ws.c decodes mu-law into a stack
 * buffer) and speaker_write_pcm() only reads it, copying through ms_resample
 * under speaker.c's own, separate mutex. g_pcm is NOT involved on this path.
 *
 * g_lock is released before speaker_write_pcm() for the same reason
 * bc_feed_rtp() releases it there: backchannel's g_lock and speaker.c's
 * g_lock are distinct mutexes and are never held simultaneously, so no lock
 * ordering exists between them to violate. */
int bc_feed_pcm(const void *owner, const int16_t *pcm, int nsamp, int rate)
{
    if (!pcm || nsamp <= 0) return 0;
    pthread_mutex_lock(&g_lock);
    int win = bc_elect_locked(owner, ms_now_us());
    pthread_mutex_unlock(&g_lock);
    if (!win) return 0;                   /* another talker holds the speaker */
    speaker_write_pcm(owner, pcm, nsamp, rate);
    return 1;
}

void bc_feed_rtp(const void *owner, const uint8_t *rtp, int len)
{
    pthread_mutex_lock(&g_lock);
    int64_t now = ms_now_us();
    if (!bc_elect_locked(owner, now)){ pthread_mutex_unlock(&g_lock); return; }

    int off = rtp_payload_off(rtp, len);
    if (off < 0){ pthread_mutex_unlock(&g_lock); return; }
    /* drop RTP padding (P bit): last byte = pad length */
    if ((rtp[0] & 0x20) && len > off){
        int pad = rtp[len-1];
        if (pad > 0 && pad <= len - off) len -= pad;
    }
    /* only the advertised payload type (also skips muxed RTCP on this channel) */
    if ((rtp[1] & 0x7F) != bc_payload_type()){ pthread_mutex_unlock(&g_lock); return; }
    const uint8_t *pl = rtp + off;
    int plen = len - off;
    if (plen <= 0){ pthread_mutex_unlock(&g_lock); return; }

    int nsamp = 0, src_rate = 8000;
    int cap = (int)(sizeof g_pcm / sizeof g_pcm[0]);
    switch (g_codec){
        case BC_CODEC_PCMA:
            nsamp = plen > cap ? cap : plen;
            g711_alaw_decode(pl, (size_t)nsamp, g_pcm); src_rate = 8000; break;
        case BC_CODEC_AAC:
#ifdef USE_BC_AAC
            nsamp = decode_aac(pl, plen, &src_rate); break;
#else
            break;
#endif
        default: /* PCMU */
            nsamp = plen > cap ? cap : plen;
            g711_ulaw_decode(pl, (size_t)nsamp, g_pcm); src_rate = 8000; break;
    }
    pthread_mutex_unlock(&g_lock);
    /* Hand the decoded PCM to speaker.c OUTSIDE g_lock: only the elected owner
     * reaches here and its RTP frames arrive serially (one session = one
     * thread), so g_pcm is stable across the unlock. speaker.c does the resample
     * to the AO rate and owns the physical IMP_AO arbitration/preemption. */
    if (nsamp > 0)
        speaker_write_pcm(owner, g_pcm, nsamp, src_rate);
}

void bc_release(const void *owner)
{
    pthread_mutex_lock(&g_lock);
    if (g_owner == owner){
        g_owner = NULL;
        g_owner_last_us = 0;
        LOGI(MOD,"backchannel decode owner released");
#ifdef USE_BC_AAC
        if (g_aac){ AACFreeDecoder(g_aac); g_aac=NULL; }
#endif
    }
    pthread_mutex_unlock(&g_lock);
    speaker_release(owner);
}

#endif /* USE_BACKCHANNEL */
