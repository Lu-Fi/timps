#ifndef TIMPS_RTP_H
#define TIMPS_RTP_H

#include <stdint.h>

/*
 * rtp.h — RTP packet builder for H.264, H.265, AAC and G.711.
 *
 * All send functions write directly to a socket (fd) or to a caller-
 * supplied write callback so they can work for both UDP and interleaved
 * TCP (RTSP §10.12).
 */

#define RTP_MAX_PKT  1400   /* safe MTU – RTP header */
#define RTP_HDR_LEN    12

typedef struct {
    uint16_t  seq;
    uint32_t  ssrc;
    uint32_t  ts_freq;  /* clock rate in Hz */
    int       pt;       /* payload type */
} RtpSession;

/* Write callback used for interleaved TCP.
 * channel : the RTSP interleaved channel number (0,1,2,3,…)
 * data, len: RTP/RTCP data
 * Returns bytes written or <0 on error.
 */
typedef int (*rtp_write_fn)(int channel, const uint8_t *data, int len,
                            void *userdata);

void rtp_session_init(RtpSession *s, int pt, uint32_t ts_freq);

/*
 * Send H.264 Annex-B bitstream as RTP (RFC 6184).
 * Handles single NALU and FU-A fragmentation automatically.
 * pts_ms: presentation timestamp in milliseconds
 */
int rtp_send_h264(RtpSession *s, const uint8_t *data, int len,
                  uint32_t pts_ms,
                  rtp_write_fn write, int channel, void *ud);

/*
 * Send H.265 (HEVC) Annex-B bitstream as RTP (RFC 7798).
 */
int rtp_send_h265(RtpSession *s, const uint8_t *data, int len,
                  uint32_t pts_ms,
                  rtp_write_fn write, int channel, void *ud);

/*
 * Send G.711 (PCMA/PCMU) audio frame as RTP (RFC 3551).
 */
int rtp_send_g711(RtpSession *s, const uint8_t *data, int len,
                  uint32_t ts,
                  rtp_write_fn write, int channel, void *ud);

/*
 * Send AAC frame as RTP (RFC 3640, mode=AAC-hbr).
 */
int rtp_send_aac(RtpSession *s, const uint8_t *data, int len,
                 uint32_t ts,
                 rtp_write_fn write, int channel, void *ud);

/* Build a raw RTP header into buf (must be at least RTP_HDR_LEN bytes).
 * Returns RTP_HDR_LEN. */
int rtp_build_header(uint8_t *buf, int pt, uint16_t seq,
                     uint32_t ts, uint32_t ssrc, int marker);

#endif /* TIMPS_RTP_H */
