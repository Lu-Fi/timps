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
    double           mkbps;                    /* measured video bitrate, kbit/s */
    uint64_t         bcount; int64_t bwin;     /* bitrate window: bytes so far */
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
/* measured video throughput of the stream in kbit/s; 0 when idle (no producer,
 * i.e. the last 1s measurement window is stale). */
double      hub_get_bitrate(int src);
void        hub_set_audio_params(int acodec, int samplerate, int channels);
/* read back the audio params the producer actually set; returns 1 if active. */
int         hub_get_audio(int *acodec, int *samplerate, int *channels);
/* Mark the audio source inactive again (e.g. the HAL failed to actually bring
 * up the capture channel after hub_set_audio_params was called at start-of-day
 * config time). Clients that (re)connect after this point no longer see an
 * audio track advertised; already-open sessions are unaffected. */
void        hub_clear_audio_params(void);

/* On-demand: HAL registers an activity callback. The hub invokes it with
 * active=1 when a source gets its first subscriber and active=0 when the last
 * subscriber leaves, so the HAL can start/stop capture+encode on demand. */
void        hub_set_activity_cb(void (*cb)(int src, int active));
int         hub_active(int src);   /* nonzero if the source has subscribers */
int         hub_subs(int src);     /* subscriber count of one source */
/* total subscribers across all video streams (rough "viewer" count for OSD) */
int         hub_video_subs(void);

/* optional live control: HAL registers a handler; the control endpoint forwards
 * parsed settings as dotted config keys with the raw value string (e.g.
 * "image.brightness"/"140", "osd0.0.text"/"cam1", "video0.bitrate"/"3500").
 * The handler parses numbers itself. No-op if no handler is registered. */
void        hub_set_control_cb(void (*cb)(const char *key, const char *val));
void        hub_control(const char *key, const char *val);
/* Optional batch-commit hook: some HAL applies (the IVS motion-grid rebuild)
 * are expensive stop/destroy/recreate cycles that a single /control POST can
 * otherwise trigger once per key. The HAL registers a commit callback and just
 * flags "needs rebuild" while keys stream through hub_control(); the control
 * endpoint calls hub_control_commit() ONCE after all keys of a request are
 * applied so the rebuild runs at most once. No-op if no handler is registered. */
void        hub_set_control_commit_cb(void (*cb)(void));
void        hub_control_commit(void);

/* ---- JPEG source selection & on-demand grab ----------------------------
 * Shared by mp4/httpd.c (/snapshot.jpg, /stream.mjpeg) and timelapse.c: both
 * pick a JPEG hub source the same way and both cold-wake it the same way,
 * and this used to be two hand-mirrored copies - a fix applied to one and
 * forgotten on the other three times over (see CHANGELOG). */

/* Priority: (1) the JPEG encoder piggybacked on video stream 'chn' (skipped
 * if chn<0), (2) the dedicated jpeg.* channel, (3) any enabled videoN.jpeg
 * piggyback. 'strict' (used for an explicit caller-requested channel, e.g.
 * /snapshot.jpg?chn=N): if tier (1) isn't usable, return -1 instead of
 * falling through (2)/(3) - silently substituting a channel the caller
 * didn't ask for would be surprising. Returns a hub source id, or -1 if
 * nothing suitable is enabled. */
int         hub_pick_jpeg_src(const ms_config *cfg, int chn, int strict);

/* default per-half wait for hub_grab_jpeg(); override with -D if needed. */
#ifndef HUB_JPEG_GRAB_WAIT_MS
#define HUB_JPEG_GRAB_WAIT_MS 1500
#endif

/* One on-demand JPEG grab: subscribe to 'src' (a HUB_JPEG_SRC/_N id), wait
 * up to two bounded halves of 'wait_ms' each for a fresh JPEG (the 2nd half
 * additionally subscribes the parent video source of a piggyback 'src' to
 * force a cold pipeline up, mirroring the on-demand start RTSP/fMP4 clients
 * use), then unsubscribe. See hub.c for the full rationale. Returns a ref'd
 * ms_pkt (caller must pkt_unref) or NULL on timeout/subscribe failure.
 * 'busy' is optional (pass NULL if the caller doesn't care, e.g. a bare
 * retry loop): when non-NULL, *busy is set to 1 if the NULL return was
 * because hub_subscribe() itself failed (source already at HUB_MAX_SUBS),
 * vs. 0 for a plain grab timeout - callers that report the difference to a
 * client (e.g. HTTP 503 "busy" vs. "no frame") need this distinction. */
ms_pkt     *hub_grab_jpeg(int src, int wait_ms, int *busy);

#endif
