#ifndef TIMPS_FMP4_H
#define TIMPS_FMP4_H

#include <stdint.h>

/*
 * fmp4.h — minimal fragmented-MP4 (ISO BMFF) muxer for MSE playback.
 *
 * Produces a stream suitable for Media Source Extensions in modern browsers:
 *   ftyp box  +  moov box  (written once at the start)
 *   moof box  +  mdat box  (repeated for every video frame, optionally audio)
 *
 * Only H.264 and H.265 video is supported.  Audio is interleaved as AAC
 * or omitted.
 */

/* Opaque muxer context */
typedef struct Fmp4Ctx Fmp4Ctx;

/*
 * Write callback.  The muxer calls this whenever it has bytes to send.
 * Return bytes written or negative on error.
 */
typedef int (*fmp4_write_fn)(const uint8_t *data, int len, void *userdata);

/*
 * Create a new fMP4 context.
 *   write_fn / ud : output callback
 *   is_hevc       : 1 for H.265, 0 for H.264
 *   width, height : frame dimensions
 *   fps           : frames per second
 * Returns NULL on allocation failure.
 */
Fmp4Ctx *fmp4_create(fmp4_write_fn write_fn, void *ud,
                     int is_hevc, int width, int height, int fps);

/*
 * Write the ftyp + moov initialisation segment.
 * spspps: Annex-B encoded SPS+PPS (H.264) or VPS+SPS+PPS (H.265).
 * Must be called once before any fmp4_write_video_frame().
 */
int fmp4_write_init(Fmp4Ctx *ctx, const uint8_t *spspps, int spspps_len);

/*
 * Append a video frame.
 * data: Annex-B NAL units.  key: 1 if IDR/key frame.  pts_ms: PTS in ms.
 */
int fmp4_write_video_frame(Fmp4Ctx *ctx,
                           const uint8_t *data, int len,
                           int key, uint64_t pts_ms);

void fmp4_destroy(Fmp4Ctx *ctx);

#endif /* TIMPS_FMP4_H */
