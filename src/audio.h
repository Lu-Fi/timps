#ifndef TIMPS_AUDIO_H
#define TIMPS_AUDIO_H

#include <stdint.h>

/*
 * audio.h — audio capture and encoding.
 *
 * Captures PCM from the built-in audio device and encodes it to
 * G.711 A-law, G.711 µ-law, or AAC using libimp AENC.
 */

typedef struct {
    uint8_t  *data;
    int       len;
    uint64_t  pts;    /* microseconds */
} AudioFrame;

typedef void (*audio_frame_cb)(const AudioFrame *frame, void *userdata);

typedef struct {
    audio_frame_cb cb;
    void          *userdata;
} AudioSink;

/* Initialise audio capture + encoder.  Returns 0 on success. */
int  audio_init(void);
void audio_exit(void);

int  audio_start(void);
void audio_stop(void);

int  audio_add_sink(AudioSink *sink);
void audio_remove_sink(AudioSink *sink);

/* Returns the RTP payload type for the configured codec:
 *   PCMA=8, PCMU=0, AAC dynamic=97 */
int  audio_rtp_pt(void);

/* Returns the sample rate (Hz) */
int  audio_sample_rate(void);

#endif /* TIMPS_AUDIO_H */
