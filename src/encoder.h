#ifndef TIMPS_ENCODER_H
#define TIMPS_ENCODER_H

#include <stdint.h>
#include <pthread.h>
#include <imp/imp_encoder.h>

/* Max NAL units in a single IMP stream */
#define MAX_NAL_PER_STREAM  8

/*
 * A single encoded frame passed to consumers.
 * The data pointer is valid only until encoder_release_frame() is called.
 */
typedef struct {
    int       stream_idx;   /* 0=main, 1=sub */
    uint8_t  *data;
    int       len;
    uint64_t  pts;          /* microseconds, monotonic */
    int       key;          /* 1 if this is an IDR/key frame */
} EncoderFrame;

/*
 * Callback invoked (from encoder thread) when a frame is ready.
 * The callback MUST NOT block for long; copy the data if needed.
 */
typedef void (*encoder_frame_cb)(const EncoderFrame *frame, void *userdata);

typedef struct {
    encoder_frame_cb cb;
    void            *userdata;
} EncoderSink;

/* ---- per-stream encoder ---- */

int  encoder_create(int idx);           /* create encoder for stream idx */
void encoder_destroy(int idx);

int  encoder_start(int idx);            /* start receiving pictures */
void encoder_stop(int idx);

/* Register/unregister a frame consumer */
int  encoder_add_sink(int idx, EncoderSink *sink);
void encoder_remove_sink(int idx, EncoderSink *sink);

/* Request an IDR frame on stream idx */
void encoder_request_idr(int idx);

/* Return stream SPS/PPS in Annex-B format (H.264) or VPS/SPS/PPS (H.265).
 * Caller must free() the returned buffer.  len set on return. */
uint8_t *encoder_get_spspps(int idx, int *len);

#endif /* TIMPS_ENCODER_H */
