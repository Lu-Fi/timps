/* talk_ws.h - browser-microphone audio backchannel over a WebSocket.
 *
 * Optional feature (USE_BC_WS, which implies USE_BACKCHANNEL + USE_TLS +
 * USE_CONTROL). A browser cannot speak RTSP/RTP, so the ONVIF backchannel in
 * backchannel.c is unreachable from a web page. This module serves the same
 * speaker path over a WebSocket instead: the page captures the visitor's
 * microphone with getUserMedia(), encodes G.711 mu-law in an AudioWorklet and
 * sends 20 ms binary frames; we decode them with the g711.c the backchannel
 * already builds and hand the PCM to bc_feed_pcm(), which arbitrates against
 * any RTSP talker and forwards to speaker.c / IMP_AO.
 *
 * This module does NOT terminate TLS and does NOT authenticate.
 *
 *   - TLS: httpd.c's conn_thread has already run ms_tls_accept() before it
 *     dispatches, so the connection handed here is already decrypted. The
 *     ms_tls_conn is passed through opaquely (void *) purely so ws.c's
 *     transport seam can reach it; nothing here includes mbedTLS.
 *
 *   - AUTH: the /talk dispatch branch in httpd.c applies exactly the same
 *     gate /events does (localhost, or a valid ?token=, or no configured
 *     user) BEFORE calling in, and the global Basic/Digest gate has already
 *     run above that. By the time talk_ws_serve() is entered the caller is
 *     authorised. Do not add a second, divergent check here - but equally, do
 *     not call this from anywhere that has not applied that gate.
 */
#ifndef MS_TALK_WS_H
#define MS_TALK_WS_H
#ifdef USE_BC_WS

/* Serve one already-authenticated, already-TLS-terminated HTTP connection as
 * a WebSocket talk session. Returns when the session ends, for any reason.
 *
 *   fd        the connection's socket. Still owned and closed by the caller.
 *   tls       the hconn's ms_tls_conn *, or NULL on a plaintext connection
 *             (which the dispatch branch refuses before reaching here).
 *   head      the request head conn_thread already read, NUL-terminated. The
 *             handshake is parsed from THIS, never re-read from the socket -
 *             those bytes are already consumed and would never arrive again.
 *   head_len  bytes in head, for bounding.
 *   path      the request target including its query string (?token=, ?rate=).
 *
 * On any refusal this writes a plain HTTP error response and returns without
 * upgrading; the caller then closes as it would for any other handler. */
void talk_ws_serve(int fd, void *tls, const char *head, int head_len,
                   const char *path);

#endif /* USE_BC_WS */
#endif
