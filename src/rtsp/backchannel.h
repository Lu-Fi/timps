/* backchannel.h - ONVIF audio backchannel (client speaks -> camera speaker).
 *
 * Optional feature (USE_BACKCHANNEL). The output path is deliberately HAL-free:
 * received RTP is decoded to PCM16 in pure C (G.711) or via libhelix-aac
 * (USE_BC_AAC), linearly resampled, and piped to thingino's audio daemon client
 * `/bin/iac -s` - exactly like prudynt/raptor. That daemon owns IMP_AO, so timps
 * never opens the speaker device itself (no device-exclusivity conflicts, works
 * the same on every SoC as long as ingenic-audiodaemon is present).
 */
#ifndef MS_BACKCHANNEL_H
#define MS_BACKCHANNEL_H
#ifdef USE_BACKCHANNEL
#include <stdint.h>

/* advertised backchannel codec (config.audio.backchannel_codec) */
enum { BC_CODEC_PCMU = 0, BC_CODEC_PCMA = 1, BC_CODEC_AAC = 2 };

/* Configure the advertised codec + speaker sample rate. Call once at startup. */
void bc_configure(int codec, int out_rate);

/* 1 if the feature is compiled+enabled AND /bin/iac is present, else 0. */
int  bc_available(void);

/* SDP helpers for the m=audio backchannel line (trackID=2). */
int         bc_payload_type(void);   /* 0=PCMU 8=PCMA 97=AAC */
const char *bc_rtpmap_name(void);    /* "PCMU"/"PCMA"/"mpeg4-generic" */
int         bc_clock_rate(void);     /* 8000 (G.711) or out_rate (AAC) */

/* Feed one received RTP packet (whole packet incl. 12-byte header). The first
 * caller becomes the exclusive speaker owner; frames from other owners are
 * dropped until the owner releases. `owner` is any stable per-session pointer. */
void bc_feed_rtp(const void *owner, const uint8_t *rtp, int len);

/* Release the speaker if this owner holds it (call at PLAY end / teardown). */
void bc_release(const void *owner);

#endif /* USE_BACKCHANNEL */
#endif
