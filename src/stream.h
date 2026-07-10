#ifndef TIMPS_STREAM_H
#define TIMPS_STREAM_H

/*
 * stream.h — on-demand stream lifecycle.
 *
 * Encoders and audio capture are started only when at least one client
 * is watching.  A reference-count is maintained per stream.  When the
 * last client disconnects the encoder is stopped to save power.
 */

#include "encoder.h"
#include "audio.h"

/*
 * Acquire a video stream.  Creates and starts the encoder on first call.
 * Returns 0 on success.
 */
int  stream_acquire(int idx);

/*
 * Release a video stream.  Stops and destroys the encoder when the last
 * client releases it.
 */
void stream_release(int idx);

/*
 * Same for audio (shared across all streams).
 */
int  stream_audio_acquire(void);
void stream_audio_release(void);

/* Register/unregister frame consumers (thin wrappers around encoder_*). */
int  stream_add_video_sink(int idx, EncoderSink *sink);
void stream_remove_video_sink(int idx, EncoderSink *sink);

int  stream_add_audio_sink(AudioSink *sink);
void stream_remove_audio_sink(AudioSink *sink);

/* Number of active viewers per stream */
int stream_viewer_count(int idx);

/* Total number of active audio listeners */
int stream_audio_listeners(void);

#endif /* TIMPS_STREAM_H */
