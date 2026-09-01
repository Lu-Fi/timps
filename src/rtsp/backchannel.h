/* backchannel.h - ONVIF audio backchannel (client speaks -> camera speaker).
 *
 * Optional feature (USE_BACKCHANNEL). Received RTP is decoded to PCM16 in pure
 * C (G.711) or via libhelix-aac (USE_BC_AAC), then handed to speaker.c, which
 * resamples it and drives IMP_AO directly (see speaker.h). timps now owns the
 * speaker natively - no external /bin/iac (ingenic-audiodaemon) dependency.
 */
#ifndef MS_BACKCHANNEL_H
#define MS_BACKCHANNEL_H
#ifdef USE_BACKCHANNEL
#include <stdint.h>

/* advertised backchannel codec (config.audio.backchannel_codec) */
enum { BC_CODEC_PCMU = 0, BC_CODEC_PCMA = 1, BC_CODEC_AAC = 2 };

/* Configure the advertised codec + speaker sample rate. Call once at startup. */
void bc_configure(int codec, int out_rate);

/* 1 whenever the feature is compiled in (speaker output is native IMP_AO now,
 * no external dependency to probe for). */
int  bc_available(void);

/* SDP helpers for the m=audio backchannel line (trackID=2). */
int         bc_payload_type(void);   /* 0=PCMU 8=PCMA 97=AAC */
const char *bc_rtpmap_name(void);    /* "PCMU"/"PCMA"/"mpeg4-generic" */
int         bc_clock_rate(void);     /* 8000 (G.711) or out_rate (AAC) */

/* Feed one received RTP packet (whole packet incl. 12-byte header). The first
 * caller becomes the exclusive speaker owner; frames from other owners are
 * dropped until the owner releases. `owner` is any stable per-session pointer. */
void bc_feed_rtp(const void *owner, const uint8_t *rtp, int len);

/* Feed ALREADY-DECODED mono PCM16 into the backchannel, subject to the SAME
 * single-talker election bc_feed_rtp() uses. For producers that do their own
 * decoding into their own buffer - currently the WebSocket talk endpoint
 * (talk_ws.c), which G.711 mu-law-decodes into a caller stack buffer.
 *
 * `rate` is the sample rate of `pcm`; speaker.c resamples to the AO rate, so
 * whatever the browser's AudioContext ended up at can be passed straight in.
 *
 * Returns 1 if the samples were accepted (this producer holds the election),
 * 0 if another talker owns the speaker. A caller that keeps getting 0 should
 * close its session rather than keep decoding into a void.
 *
 * `pcm` is read, never retained: it may be a stack buffer. */
int  bc_feed_pcm(const void *owner, const int16_t *pcm, int nsamp, int rate);

/* Release the speaker if this owner holds it (call at PLAY end / teardown). */
void bc_release(const void *owner);

#endif /* USE_BACKCHANNEL */
#endif
