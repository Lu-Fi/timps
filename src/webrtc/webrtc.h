/* webrtc.h - optional WHEP endpoint: ICE-lite + DTLS transport only.
 *
 * What this milestone does: answer a browser's SDP offer, respond to its
 * STUN connectivity checks as an ICE-lite peer, and complete the DTLS
 * handshake as the passive side. What it deliberately does NOT do yet: SRTP,
 * RTP packetisation, media of any kind - the answer marks the video
 * m-section a=inactive. Only compiled with USE_WEBRTC.
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
