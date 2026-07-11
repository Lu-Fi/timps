/* hub.h - one publisher (HAL) fans encoded packets out to many subscribers
 * (RTSP sessions, HTTP fMP4 clients). Sources: video[0..N-1], audio[N]. */
#ifndef MS_HUB_H
#define MS_HUB_H
#include "config.h"
#include "frame.h"
#include "fanqueue.h"
#include "codec/vparam.h"

#define HUB_AUDIO_SRC   MS_MAX_VSTREAM
/* JPEG sources: [HUB_JPEG_SRC] = dedicated jpeg.* channel (own framesource),
 * [HUB_JPEG_SRC_N(i)] = optional JPEG encoder piggybacked on video stream i
 * (videoN.jpeg = true; shares that stream's framesource). */
#define HUB_JPEG_SRC    (MS_MAX_VSTREAM+1)
#define HUB_NJPEG       (1+MS_MAX_VSTREAM)
#define HUB_JPEG_SRC_N(i) (HUB_JPEG_SRC+1+(i))
#define HUB_NSRC        (HUB_JPEG_SRC+HUB_NJPEG)
#define HUB_MAX_SUBS    16

typedef struct hub_source {
    int              active;
    pthread_mutex_t  lock;
    fanqueue        *subs[HUB_MAX_SUBS];
    int              nsub;
    int              vcodec;   /* enum ms_vcodec, video only */
    int              acodec;   /* enum ms_acodec, audio only */
    int              width, height, fps;      /* video */
    vparam           vp;                       /* cached SPS/PPS/VPS */
    int              vp_ready;
    int              samplerate, channels;    /* audio */
    double           mfps;                     /* measured video fps */
    uint32_t         fcount; int64_t fwin;     /* fps window */
} hub_source;

void        hub_init(void);
hub_source *hub_get(int src);
/* HAL calls this for every encoded access unit (takes a borrowed buffer,
 * copies into a refcounted pkt). */
void        hub_publish(int src, const uint8_t *data, size_t len,
                        int64_t pts_us, int keyframe, int media);
/* subscribe returns 0 on success; caller supplies its own fanqueue. */
int         hub_subscribe(int src, fanqueue *q);
void        hub_unsubscribe(int src, fanqueue *q);
void        hub_set_video_params(int src, int vcodec, int w, int h, int fps);
/* copy cached video parameter sets out; returns 1 if ready. */
int         hub_get_vparam(int src, vparam *out);
/* IDR request plumbing: HAL registers a callback; sinks call request. */
void        hub_set_idr_cb(void (*cb)(int src));
void        hub_request_idr(int src);
double      hub_get_fps(int src);
void        hub_set_audio_params(int acodec, int samplerate, int channels);
/* read back the audio params the producer actually set; returns 1 if active. */
int         hub_get_audio(int *acodec, int *samplerate, int *channels);

/* On-demand: HAL registers an activity callback. The hub invokes it with
 * active=1 when a source gets its first subscriber and active=0 when the last
 * subscriber leaves, so the HAL can start/stop capture+encode on demand. */
void        hub_set_activity_cb(void (*cb)(int src, int active));
int         hub_active(int src);   /* nonzero if the source has subscribers */
/* total subscribers across all video streams (rough "viewer" count for OSD) */
int         hub_video_subs(void);

/* optional live control: HAL registers a handler; the control endpoint forwards
 * parsed settings as dotted config keys with the raw value string (e.g.
 * "image.brightness"/"140", "osd0.text"/"cam1", "video0.bitrate"/"3500").
 * The handler parses numbers itself. No-op if no handler is registered. */
void        hub_set_control_cb(void (*cb)(const char *key, const char *val));
void        hub_control(const char *key, const char *val);

#endif
