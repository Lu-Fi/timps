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
/* Optional SECOND audio source (audio.codec2 = pcmu): the same captured PCM
 * encoded a second time as G.711u, published alongside - never instead of -
 * HUB_AUDIO_SRC. WebRTC is the only consumer: its SRTP audio path carries
 * G.711 only, while RTSP/fMP4/record want AAC. Appended AFTER the JPEG block
 * so every existing source id keeps its number. */
#define HUB_AUDIO_SRC2  (HUB_JPEG_SRC+HUB_NJPEG)
#define HUB_NSRC        (HUB_AUDIO_SRC2+1)
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
/* HAL calls this for every encoded access unit (takes a BORROWED buffer,
 * copies into a refcounted pkt). Skips the malloc+copy entirely when the
 * source has 0 subscribers.
 *
 * `now_us` is the PRODUCER's ms_now_us() reading for this frame: the publish
 * instant stamped into p->enq_us and the clock the fps/bitrate windows
 * advance on. Passed in rather than sampled here because every producer
 * already took one a few microseconds earlier (pts_sanitize needs it), and
 * this target has no vDSO - so each ms_now_us() is a real syscall and the
 * publish path used to make three of them per frame for one instant. */
void        hub_publish(int src, const uint8_t *data, size_t len,
                        int64_t pts_us, int keyframe, int media, int64_t now_us);
/* P-01 zero-second-copy variant: the producer assembles the access unit
 * DIRECTLY into a pooled packet obtained from hub_pkt_get(src, cap), sets
 * p->len, and hands ownership here. No second full-frame copy is made. On a
 * 0-subscriber source the packet is returned straight to the pool (no copy,
 * no free), so publishing through the idle-stop debounce window stays as cheap
 * as before. After this call the producer must NOT touch p (it may already be
 * back in the pool or in flight to a subscriber). Passing a NULL p is a safe
 * no-op. */
ms_pkt     *hub_pkt_get(int src, size_t cap);
/* Give back this source's retained oversized pool buffer (see pkt_pool->big
 * and R-01 in hub.c). Producers call it where they shut their encoder down for
 * a sustained idle period - an idle source must not sit on an IDR-sized
 * buffer. Purely a memory hint: it frees only a buffer already idle in the
 * pool, never one a subscriber still references, and the pool refills itself
 * from the next large frame. */
void        hub_pool_trim(int src);
void        hub_publish_take(int src, ms_pkt *p,
                             int64_t pts_us, int keyframe, int media,
                             int64_t now_us);   /* see hub_publish() */
/* subscribe returns 0 on success; caller supplies its own fanqueue. */
int         hub_subscribe(int src, fanqueue *q);
void        hub_unsubscribe(int src, fanqueue *q);
void        hub_set_video_params(int src, int vcodec, int w, int h, int fps);
/* Read back the video params the producer actually pushed via
 * hub_set_video_params() - i.e. the EFFECTIVE (post-rotation, and for a
 * 90/270 request that exceeded the T23 SW-rotate / T31 FS-rotate safe
 * envelope, post-REFUSAL) width/height the HAL is ACTUALLY running for this
 * stream. hal_ingenic.c only calls hub_set_video_params() after any rotation
 * refusal has already been decided (see g_eff_rot), so this is the single
 * source of truth for "what geometry is this stream really producing right
 * now" - unlike ms_vstream_eff_dims() on a raw ms_vstream_cfg, which only
 * knows the CONFIGURED rotation and has no notion of a refusal.
 * Any output pointer may be NULL to skip that field. Returns 1 if the source
 * has been populated (the HAL has started this stream at least once since
 * hub_init()), 0 otherwise (e.g. very early startup before the HAL's start
 * routine has run) - callers must fall back to a raw ms_vstream_eff_dims()
 * computation on 0 rather than trust zeroed output params. */
int         hub_get_video_params(int src, int *vcodec, int *w, int *h, int *fps);
/* copy cached video parameter sets out; returns 1 if ready. */
int         hub_get_vparam(int src, vparam *out);
/* IDR request plumbing: HAL registers a callback; sinks call request. */
void        hub_set_idr_cb(void (*cb)(int src));
/* A client that cannot START without a keyframe (subscribe, RTSP PLAY/DESCRIBE,
 * a fresh fMP4 GET, a WebRTC answer): issued immediately, never deferred. */
void        hub_request_idr(int src);

/* Minimum spacing between IDRs forced by drop RECOVERY. One second is exactly
 * the cadence rtsp.c/httpd.c/record.c each already promised themselves, so a
 * single slow consumer heals no slower than before; what changes is that the
 * budget is now the STREAM's, not the consumer's, and N slow consumers can no
 * longer force N IDRs/s onto the one shared encoder. Deliberately not tied to
 * videoN.gop: at the fleet's gop=50 / fps=15-25 the natural GOP is 2-3.3 s, so
 * a GOP-length interval would have made recovery two to three times slower
 * than it is today for the common single-consumer case, which is a regression
 * the fleet would feel. No config key - recovery latency is not something an
 * operator should have to tune. */
#ifndef HUB_IDR_RECOVERY_MIN_US
#define HUB_IDR_RECOVERY_MIN_US (1000000LL)
#endif
/* A consumer healing from a fanqueue overflow. Rate-limited per stream against
 * every other consumer's recovery request (and against any start request, which
 * delivers the same keyframe anyway). Returns 1 if the request went to the
 * encoder now, 0 if it was COALESCED: a deferred request is remembered and
 * issued by the next published frame once the interval has passed, so a
 * consumer frozen until its next keyframe always gets one even if it never
 * asks again. */
int         hub_request_idr_recovery(int src);

/* Consumers (RTSP/fMP4/record) report a fanqueue overflow-heal event here;
 * GET /control sums them per video stream as "queue_drops" - the only
 * always-on trace of the silent drop->IDR->bitrate-spike cycle. `kind` labels
 * the reporter for the rate-limited summary WARN only; it does not change what
 * "queue_drops" counts. */
enum {
    HUB_DROP_REC = 0, HUB_DROP_RTSP, HUB_DROP_MP4, HUB_DROP_WEBRTC,
    HUB_DROP_SRT, HUB_DROP_NKIND
};
/* How often at most a (kind, stream) pair may emit its overflow summary. */
#ifndef HUB_DROP_REPORT_US
#define HUB_DROP_REPORT_US (60*1000000LL)
#endif
void        hub_note_drop(int src, int kind);
unsigned    hub_get_drops(int src);
/* measured video frame rate of the stream; 0 when idle (no producer, i.e. the
 * last 1s measurement window is stale) - same rule as hub_get_bitrate(). */
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
/* Same three calls for HUB_AUDIO_SRC2. Independent state: the secondary is
 * advertised only once the HAL really starts the extra encode pass, so a
 * consumer that finds it inactive simply falls back to the primary source. */
void        hub_set_audio2_params(int acodec, int samplerate, int channels);
void        hub_clear_audio2_params(void);
int         hub_get_audio2(int *acodec, int *samplerate, int *channels);

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
 * The handler parses numbers itself. Returns 1 when the key reached the
 * RUNNING pipeline, 0 when it only persisted (applies on restart, or is
 * unsupported on this platform/build) - the control endpoint uses that to
 * report per-key what took effect now and what waits for a restart (the
 * videoN.* / sensor.* "deferred" grading in control.c). Returns 0 if no
 * handler is registered (host sim). */
void        hub_set_control_cb(int (*cb)(const char *key, const char *val));
int         hub_control(const char *key, const char *val);
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
/* Optional hook, called with the grab's queue right after it is subscribed and
 * with NULL just before it is released. hub_grab_jpeg() builds that queue on
 * its OWN stack, so it is invisible to anything outside - which meant a
 * shutdown could reach neither it nor the thread parked on it: /snapshot.jpg
 * sat there for up to 2 x HUB_JPEG_GRAB_WAIT_MS (measured 2694 ms with an idle
 * JPEG source), sailed past httpd_stop()'s drain, and then wrote its response
 * through a TLS context that had already been freed. The hook lets a caller
 * publish the queue somewhere a wake can find it (mp4/httpd.c registers it in
 * its ms_client_reg slot) without hub.c having to know what that somewhere is.
 * NULL for callers that do not need it - timelapse.c runs on its own thread,
 * which main() stops before the servers. */
struct fanqueue;   /* fanqueue.h; only the pointer is needed here */
typedef void (*hub_grab_hook)(struct fanqueue *q, void *ctx);
ms_pkt     *hub_grab_jpeg(int src, int wait_ms, int *busy,
                          hub_grab_hook hook, void *hook_ctx);

#endif
