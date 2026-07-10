#ifndef TIMPS_RTSP_H
#define TIMPS_RTSP_H

/*
 * rtsp.h — RTSP/1.0 server (RFC 2326).
 *
 * Supports:
 *   - OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN
 *   - RTP/AVP over TCP (interleaved, §10.12)
 *   - RTP/AVP/UDP (unicast)
 *   - Digest authentication (optional)
 *   - H.264 and H.265 video, G.711 and AAC audio
 *
 * URL scheme:
 *   rtsp://<host>:<port>/stream0    main stream
 *   rtsp://<host>:<port>/stream1    sub stream
 *   rtsp://<host>:<port>/           alias for /stream0
 */

/* Initialise and start the RTSP server.  Spawns a listener thread.
 * Returns 0 on success. */
int  rtsp_init(void);

/* Stop and clean up. */
void rtsp_exit(void);

#endif /* TIMPS_RTSP_H */
