/* webrtc.h - optional WHEP endpoint: ICE-lite + DTLS-SRTP + H264 video.
 *
 * Answer a browser's SDP offer, respond to its STUN connectivity checks as an
 * ICE-lite peer, complete the DTLS handshake as the passive side, then send
 * the hub's H264 stream through the rtsp/rtp.c packetizer and srtp.c.
 * Deliberately NOT here: audio in any codec, NACK/retransmission, FEC,
 * simulcast, congestion control, H265, IPv6, RTP header extensions.
 * Only compiled with USE_WEBRTC.
 */
#ifndef MS_WEBRTC_H
#define MS_WEBRTC_H
#ifdef USE_WEBRTC

#include "../config.h"

/* Builds the shared DTLS context (cert/key are http.tls_cert/http.tls_key)
 * when webrtc.enabled is set. Safe to call when disabled - it just logs
 * nothing and leaves webrtc_available() at 0. */
void webrtc_start(const ms_config *cfg);
void webrtc_stop(void);
int  webrtc_available(void);

/* POST /webrtc/whep. `offer` is the request body (an SDP offer), `local_ip`
 * the dotted address the client reached us on (getsockname on the HTTP
 * connection), which becomes our single host candidate. Returns the HTTP
 * status code to send; on 201 `ans` holds the SDP answer and `sid` the
 * session id for the Location header. */
int  webrtc_whep(const char *offer, const char *local_ip,
                 char *ans, int anscap, char *sid, int sidcap);

#endif /* USE_WEBRTC */
#endif
