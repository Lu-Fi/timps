#ifndef TIMPS_HTTP_H
#define TIMPS_HTTP_H

/*
 * http.h — minimal HTTP/1.1 server.
 *
 * Endpoints:
 *   GET /              — simple info page
 *   GET /snapshot.jpg  — single JPEG frame
 *   GET /mjpeg         — MJPEG stream (multipart/x-mixed-replace)
 *   GET /stream        — fragmented-MP4 (MSE) video stream (stream0)
 *   GET /stream1       — fragmented-MP4 (MSE) video stream (stream1)
 *   GET /stream.m3u8   — placeholder HLS manifest (redirect to /stream)
 *
 * Supports HTTP Basic authentication (optional).
 */

int  http_init(void);
void http_exit(void);

#endif /* TIMPS_HTTP_H */
