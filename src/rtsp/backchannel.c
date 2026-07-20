/* backchannel.c - see backchannel.h. Compiled only when USE_BACKCHANNEL. */
#ifdef USE_BACKCHANNEL
#include "backchannel.h"
#include "../log.h"
#include "../codec/g711.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>

#ifdef USE_BC_AAC
#include <aacdec.h>              /* libhelix-aac (declares everything we use) */
#endif

#define MOD "bc"
#define BC_PIPE_BUF 4096         /* Linux PIPE_BUF: writes <= this are atomic */

/* -------- config + shared speaker state ---------------------------------- */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static const void *g_owner   = NULL;   /* session that owns the speaker */
static FILE       *g_pipe    = NULL;   /* popen("/bin/iac -s","w") */
static int         g_pipe_fd = -1;
static int         g_codec   = BC_CODEC_PCMU;
static int         g_out_rate = 16000;

/* scratch buffers - only ever touched under g_lock */
static int16_t g_pcm[8192];     /* decoded PCM (mono) */
static int16_t g_rs[16384];     /* resampled PCM */

#ifdef USE_BC_AAC
static HAACDecoder g_aac = NULL;
#endif

void bc_configure(int codec, int out_rate)
{
    if (codec < BC_CODEC_PCMU || codec > BC_CODEC_AAC) codec = BC_CODEC_PCMU;
#ifndef USE_BC_AAC
    if (codec == BC_CODEC_AAC) codec = BC_CODEC_PCMU;   /* no AAC decoder built */
#endif
    if (out_rate < 8000 || out_rate > 48000) out_rate = 16000;
    g_codec = codec; g_out_rate = out_rate;
}

int bc_available(void)
{
    return access("/bin/iac", X_OK) == 0;
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

/* -------- speaker pipe --------------------------------------------------- */
static int pipe_open(void)   /* caller holds g_lock */
{
    if (g_pipe) return 0;
    g_pipe = popen("/bin/iac -s", "w");
    if (!g_pipe){ LOGW(MOD,"popen /bin/iac failed: %s", strerror(errno)); g_pipe_fd=-1; return -1; }
    g_pipe_fd = fileno(g_pipe);
    if (g_pipe_fd >= 0){
        int fl = fcntl(g_pipe_fd, F_GETFL, 0);
        if (fl >= 0) fcntl(g_pipe_fd, F_SETFL, fl | O_NONBLOCK);
        fcntl(g_pipe_fd, F_SETFD, FD_CLOEXEC);   /* don't leak the pipe to future exec */
    }
    LOGI(MOD,"speaker pipe opened (/bin/iac -s, %d Hz)", g_out_rate);
    return 0;
}
static void pipe_close(void)  /* caller holds g_lock */
{
    if (g_pipe){ pclose(g_pipe); g_pipe=NULL; g_pipe_fd=-1; LOGI(MOD,"speaker pipe closed"); }
#ifdef USE_BC_AAC
    if (g_aac){ AACFreeDecoder(g_aac); g_aac=NULL; }
#endif
}
/* Write PCM16 to the pipe in <=PIPE_BUF chunks. A non-blocking write of
 * <=PIPE_BUF bytes is atomic (all-or-EAGAIN), so we never emit a partial,
 * odd-byte fragment - the 16-bit sample framing to iac can never desync.
 * On a fatal error the pipe is closed AND ownership released so the next
 * frame re-acquires and reopens (else the speaker would stay dead). */
static void pipe_write(const int16_t *pcm, int nsamp)  /* holds g_lock */
{
    if (!g_pipe || g_pipe_fd < 0 || nsamp <= 0) return;
    const uint8_t *d = (const uint8_t*)pcm;
    int left = nsamp * (int)sizeof(int16_t);
    while (left > 0){
        int chunk = left > BC_PIPE_BUF ? (BC_PIPE_BUF & ~1) : left;   /* keep even */
        ssize_t w = write(g_pipe_fd, d, (size_t)chunk);
        if (w == chunk){ d += chunk; left -= chunk; continue; }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return; /* clogged: drop rest (even boundary) */
        if (w < 0){
            LOGW(MOD,"speaker write failed (%s) - closing pipe, releasing owner", strerror(errno));
            pipe_close(); g_owner = NULL; return;
        }
        return;   /* short atomic write shouldn't happen; drop rest to stay aligned */
    }
}

/* linear resample mono int16 src_rate -> g_out_rate into g_rs[]; returns count */
static int resample(const int16_t *in, int n, int src_rate)
{
    int cap = (int)(sizeof g_rs / sizeof g_rs[0]);
    if (src_rate == g_out_rate || src_rate <= 0){
        int c = n; if (c > cap) c = cap;
        memcpy(g_rs, in, (size_t)c*sizeof(int16_t)); return c;
    }
    double ratio = (double)g_out_rate / (double)src_rate;
    int out = (int)(n * ratio);
    if (out > cap) out = cap;
    for (int i=0;i<out;i++){
        double pos = i / ratio;
        int i0 = (int)pos; if (i0 >= n) i0 = n-1;
        int i1 = (i0+1 < n) ? i0+1 : i0;
        double f = pos - i0;
        g_rs[i] = (int16_t)(in[i0]*(1.0-f) + in[i1]*f);
    }
    return out;
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
    for (int a=0; a<naus && data_off < plen && total < cap; a++){
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

void bc_feed_rtp(const void *owner, const uint8_t *rtp, int len)
{
    pthread_mutex_lock(&g_lock);
    if (g_owner == NULL){                 /* first talker becomes owner */
        if (pipe_open() != 0){ pthread_mutex_unlock(&g_lock); return; }
        g_owner = owner;
        LOGI(MOD,"speaker owner acquired");
    }
    if (g_owner != owner){ pthread_mutex_unlock(&g_lock); return; }  /* not the owner */

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
    if (nsamp > 0){
        int rn = resample(g_pcm, nsamp, src_rate);
        pipe_write(g_rs, rn);
    }
    pthread_mutex_unlock(&g_lock);
}

void bc_release(const void *owner)
{
    pthread_mutex_lock(&g_lock);
    if (g_owner == owner){
        pipe_close();
        g_owner = NULL;
        LOGI(MOD,"speaker owner released");
    }
    pthread_mutex_unlock(&g_lock);
}

#endif /* USE_BACKCHANNEL */
