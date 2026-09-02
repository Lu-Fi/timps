/* hal_ingenic.c - real Ingenic SoC backend (T10/T20/T21/T23/T30/T31/T40/T41/C100).
 * Depends only on the vendor libimp (+pthread). Video via IMP_ISP/FrameSource/
 * Encoder, audio via IMP_AI (+ own G.711 or IMP_AENC AAC), OSD via IMP_OSD,
 * motion via IMP_IVS. Compiled only with -DHAL_INGENIC against the SDK headers.
 *
 * The control flow mirrors the proven prudynt-t pipeline but is stripped down
 * to the essentials for minimal footprint. */
#include "hal.h"
#ifdef HAL_INGENIC
#include "../hub.h"
#include "../log.h"
#include "../util.h"
#include "../codec/nal.h"
#include "../codec/g711.h"
#include "../isp_caps.h"
#include "../audio_caps.h"
#include "../rotate_caps.h"   /* ms_vstream_eff_dims (post-rotation dims) */
#include "../enc_caps.h"      /* ENC_LIVE_KEYS: live rc keys of this build */
#include "imp_osd.h"
#include "imp_motion.h"
#include "../rtsp/speaker.h"   /* speaker_set_volume/gain (spk_* live apply) */
#ifdef ROT_HAS_SW_90
/* Batch 5 (T23 + USE_SW_ROTATE=1 only): software 90/270 rotate + unbound
 * IMP_Encoder_Yuv* encode + software OSD compositing. Everything below that
 * is guarded by ROT_HAS_SW_90 is compiled out on every other build. */
#include "nv12_rot.h"
#include "osd_text.h"    /* embedded bitmap-font fallback rasterizer */
#include "osd_vars.h"    /* {hostname}/{timestamp}/... placeholder expansion */
#include "msttf.h"       /* TTF rasterizer (same one imp_osd.c uses) */
#endif

#include <imp/imp_system.h>
#include <imp/imp_isp.h>
#include <imp/imp_framesource.h>
#include <imp/imp_encoder.h>
#include <imp/imp_audio.h>
#include <imp/imp_osd.h>
#ifdef USE_FAAC
#include <faac.h>
#endif
#ifdef USE_STREAM_OPUS
/* libopus ENCODER API (RFC 7587 RTP streaming of the live mic). This is the
 * bare libopus codec, NOT opusfile - opusfile is the decode-only Ogg-Opus
 * reader used by the unrelated USE_PLAY_OPUS local-playback feature. */
#include <opus/opus.h>
#endif
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <errno.h>      /* ETIMEDOUT (act_wait predicate loop) */

#define MOD "HAL_ING"

#if defined(PLATFORM_T31)||defined(PLATFORM_C100)||defined(PLATFORM_T40)||defined(PLATFORM_T41)
#define ENC_NEW_API 1
/* IMP_Encoder_SetChnQpIPDelta exists only in the T31 and C100 SDKs (grepped
 * headers 2026-08-21); T40/T41 have no such call, so videoN.i_bias_lvl is
 * genuinely unsupported there, not merely unwired. */
#if defined(PLATFORM_T31)||defined(PLATFORM_C100)
#define ENC_HAS_QPIPDELTA 1
#endif
/* T41's SDK exports only IMP_Encoder_GetChnAttrRcMode - no setter at all;
 * everything except bitrate/QP bounds stays restart-bound there. */
#if !defined(PLATFORM_T41)
#define ENC_HAS_SETRCMODE 1
#endif
#else
/* classic encoder headers (T10..T30) spell the channel attr type with a
 * capitalized CHN; alias it so enc_create() reads the same on every SoC */
typedef IMPEncoderCHNAttr IMPEncoderChnAttr;
/* same capitalization split for the IMP_Encoder_Query stat struct (Item-2) */
typedef IMPEncoderCHNStat IMPEncoderChnStat;
#endif

/* Debounce for on-demand StartRecvPic/StopRecvPic: with several clients that
 * connect/disconnect rapidly, toggling the encoder in quick succession can
 * destabilize the IMP driver (observed kernel crashes with multiple ffplay
 * clients). Stop only after the source has had no consumer for this long. */
#ifndef MS_IDLE_STOP_US
#define MS_IDLE_STOP_US 2000000   /* 2 s */
#endif
/* jpeg.snapshot_path periodic file-snapshot cadence (M8): without this, a
 * configured snapshot path alone pins jwant=true in jpeg_thread forever, so
 * the JPEG pipeline (framesource + HW encoder) runs 24/7 and rewrites the
 * file at the full jc->fps drain rate. Gating on an interval lets the
 * existing on-demand idle-stop logic (MS_IDLE_STOP_US) shut the pipeline
 * back down between snapshots, matching the just-in-time pattern used
 * elsewhere. No dedicated config key yet (out of scope for this file) - this
 * is a sane fixed default; a jpeg.snapshot_interval_s config key would
 * replace it. */
#ifndef MS_SNAPSHOT_INTERVAL_US
#define MS_SNAPSHOT_INTERVAL_US 60000000LL   /* 60 s */
#endif
/* AU assembly buffer bounds (was a fixed 1 MB static per video thread; now
 * sized from the stream resolution to fit small-RAM SoCs like the T10) */
#ifndef MS_AU_BUF_MIN
#define MS_AU_BUF_MIN (128*1024)
#endif
#ifndef MS_AU_BUF_MAX
#define MS_AU_BUF_MAX (1024*1024)
#endif
/* JPEG assembly buffer (was a fixed 512 KB static). MAX matches MS_AU_BUF_MAX:
 * a real detail-heavy 1920x1080 q75 outdoor daytime scene was observed
 * needing ~800-820 KB (cam-L Y4), consistently exceeding the old 512 KB
 * cap on every single frame - the v1.6.4 growth fix correctly reported and
 * dropped each one instead of corrupting output, but the cap itself was too
 * low to ever succeed for this camera's actual content. */
#ifndef MS_JPEG_BUF_MIN
#define MS_JPEG_BUF_MIN (96*1024)
#endif
#ifndef MS_JPEG_BUF_MAX
#define MS_JPEG_BUF_MAX (1024*1024)
#endif
/* Audio input buffering. The Ingenic AI delivers a frame only once usrFrmDepth
 * frames are cached, so this depth IS the audio latency (depth x 40 ms). It was
 * 30 (~1.2 s) which made browser/RTSP audio audibly lag the video; keep it small
 * for low latency (must stay > 0 or the AI delivers no frames). Depth 2 (~80 ms)
 * gave too little slack: whenever audio_thread was starved by the H.264/H.265
 * encoder on a single-core SoC for >80 ms, the driver dropped captured frames ->
 * audible G.711 gaps. Depth 4 (~160 ms) absorbs that jitter; correct
 * sample-count RTP timestamps (see rtp.c) keep A/V sync exact despite the slack. */
#ifndef MS_AI_FRM_NUM
#define MS_AI_FRM_NUM   6
#endif
#ifndef MS_AI_FRM_DEPTH
#define MS_AI_FRM_DEPTH 4
#endif
/* Watchdog: IMP_AI_EnableChn can appear to succeed yet the channel never
 * actually yields a frame (same failure class as an unchecked EnableChn
 * error - seen on some T-series + sensor combos). PollingFrame is polled
 * every 10 ms on the no-frame path, so this many consecutive misses is ~5 s. */
#ifndef MS_AI_WATCHDOG_ITERS
#define MS_AI_WATCHDOG_ITERS 500
#endif
/* Watchdog: same failure class as MS_AI_WATCHDOG_ITERS above, but for video -
 * IMP_Encoder_StartRecvPic can succeed while the underlying framesource
 * enable silently didn't take (fs_use()'s EnableChn is unchecked and only
 * fires on the 0->1 user-count edge), leaving PollingStream returning
 * nothing forever with a client still attached - the idle-stop debounce
 * never fires while a client is subscribed, so the framesource never gets a
 * fresh Enable attempt on its own. Unlike audio, giving up isn't acceptable
 * here (video is the primary feature), so after this many consecutive
 * misses, force a real Stop/Disable/Enable/Start cycle instead. PollingStream
 * blocks up to general.imp_polling_timeout per miss (default 500 ms), so
 * this is ~5 s at the default. */
#ifndef MS_VIDEO_WATCHDOG_ITERS
#define MS_VIDEO_WATCHDOG_ITERS 10
#endif
/* Escalation for the recovery cycle above: a forced Stop/Disable/Enable/
 * Start cycle can itself report success (StartRecvPic returns 0) against
 * hardware that is genuinely gone and never deliver another frame - this is
 * exactly what happened in the field when a second timpsd instance's
 * IMP_ISP_Open reset the shared ISP driver state out from under an already-
 * running instance, destroying its FrameSource channels. Retrying that cycle
 * forever (the old behavior: dbg_pollfail unconditionally reset to 0 after
 * every attempt, win or lose) then means an infinite, non-escalating loop
 * producing zero video with no way to self-recover and no mechanism to give
 * up and exit so at least a manual/scheduled restart can bring the process
 * back (S95timps is plain SysV start/stop, it does NOT respawn on its own -
 * see the CHANGELOG entry for this fix). Track
 * CONSECUTIVE recovery cycles that never actually yielded a frame (a cycle
 * only counts as recovered once IMP_Encoder_GetStream succeeds afterward,
 * not merely because StartRecvPic returned 0); after this many in a row,
 * stop retrying and exit instead. 5 cycles at the default
 * MS_VIDEO_WATCHDOG_ITERS/imp_polling_timeout cadence is ~25 s of total dead
 * time - long enough to rule out a transient hiccup, short enough that the
 * daemon isn't stuck silently for minutes like the incident that motivated
 * this (2+ min of zero video before a human intervened). */
#ifndef MS_VIDEO_WATCHDOG_MAX_RECOVERIES
#define MS_VIDEO_WATCHDOG_MAX_RECOVERIES 5
#endif
/* Watchdog: same failure class as MS_VIDEO_WATCHDOG_ITERS, but jpeg_thread
 * (snapshots/MJPEG) had no watchdog at all (J1) - PollingStream returning
 * nothing forever produced zero log output, and when a snapshot_path is
 * configured the snapshot-due check pins jwant true permanently (idle-stop
 * never runs, the framesource never gets a fresh Enable attempt on its own),
 * so the whole pipeline pumped 24/7 with no output and no recovery. Same
 * cadence as video's watchdog since both share imp_polling_timeout. */
#ifndef MS_JPEG_WATCHDOG_ITERS
#define MS_JPEG_WATCHDOG_ITERS 10
#endif
/* Same infinite-retry risk as MS_VIDEO_WATCHDOG_MAX_RECOVERIES above:
 * dbg_jpollfail used to reset to 0 after every forced recovery cycle
 * regardless of whether it actually worked, so a dead-but-"successfully
 * restarted" JPEG channel would retry forever too. Unlike video, JPEG/
 * snapshot is not the primary feature (same tier as audio - see
 * MS_AI_WATCHDOG_ITERS), so after this many consecutive failed recovery
 * cycles the thread gives up on JUST this channel (mirrors audio_thread's
 * "disable and exit the thread" pattern) instead of taking the whole
 * process down: a video-only ISP fault has no business killing a still-
 * healthy jpeg pipeline, or vice versa. If the underlying fault is actually
 * systemic (the whole ISP is gone, as in the incident that motivated this),
 * video_thread's own watchdog independently detects and escalates that. */
#ifndef MS_JPEG_WATCHDOG_MAX_RECOVERIES
#define MS_JPEG_WATCHDOG_MAX_RECOVERIES 5
#endif

static IMPSensorInfo    g_sensor;
static const ms_config *g_hcfg;
static int              g_isp_sensor_w, g_isp_sensor_h;  /* real sensor res from
                                                          * the ISP (0 = unknown) */
#ifdef ROT_HAS_SW_90
/* Software OSD state for the unbound SW-rotate path (Batch 5b). There is no
 * IMP_OSD on an unbound stream (nothing to splice a hardware OSD group into),
 * so enabled TEXT items are rasterized in software (same msttf/osd_text
 * rasterizers imp_osd.c uses) and alpha-blended onto the rotated bounce
 * buffer's Y plane before YuvEncode. All OSD coordinates here are in the
 * ROTATED frame space (eff_w x eff_h) - the same space the operator sees.
 * NOT handled on this path (documented limitations):
 *   - logo (BGRA picture) items: skipped with a warning (text only in v1)
 *   - privacy cover masks: NOT composited (future item; lower priority)
 *   - chroma: luma-only blend in v1 (text tints toward the scene's hue) */
typedef struct {
    int         used;       /* item was an enabled TEXT item at startup */
    uint8_t    *bgra;       /* cached rendered BGRA text bitmap */
    int         w, h;       /* bitmap dims */
    msttf_font *font;       /* per-item TTF (owned) / shared TTF / NULL=bitmap font */
    int         font_owned; /* 1 = malloc'd + msttf_load'd here: free on teardown */
    char        last[256];  /* last expanded text (change detection) */
} sw_osd_slot;
typedef struct {
    int         active;           /* any usable text item on this stream */
    int64_t     next_refresh_us;  /* 1 Hz re-expand/re-render cadence (matches
                                   * imp_osd.c's updater thread period) */
    sw_osd_slot it[MS_MAX_OSD];
} sw_osd_state;
#endif

/* Fix 1 / A1: per-stream state for pts_sanitize() - turns the encoder/AI
 * hardware capture timestamp into a capture-accurate, strictly-monotonic
 * publish pts on the ms_now_us() timebase. One instance per video channel
 * (embedded in vchan) and one for the audio thread; never shared between
 * them. See pts_sanitize() for the algorithm. */
typedef struct {
    int64_t last_pub_pts;   /* last pts handed to the hub on this stream */
    int64_t pts_offset;     /* hw capture clock -> ms_now_us() base offset */
    int     have_pub_pts;   /* last_pub_pts is valid */
    int     pts_have_off;   /* pts_offset established from a good hw value */
} pts_sanitizer;

typedef struct {
    int chn, grp, codec, w, h;
    int og;                      /* OSD group id spliced between fs and enc for
                                  * this stream (imp_osd_setup), -1 = none: fs is
                                  * bound straight to enc. Teardown must unbind
                                  * the pairs that actually exist (M-1). */
    int nbound;                  /* successful IMP_System_Bind calls for this
                                  * slot (0..2) so teardown never unbinds a
                                  * pair that was never bound */
    int has_thr;                 /* pthread_create succeeded: join in stop.
                                  * Slots are counted in g_nv as soon as their
                                  * IMP channels exist (M8) so teardown frees
                                  * them even when the thread never started. */
    volatile int run, active, idr_req;
    pthread_t thr;
    /* Item-2: T31-only average-bitrate telemetry cache. GetChnAveBitrate needs
     * the just-fetched IMPEncoderStream (see imp_encoder.h), which only the
     * encode thread holds, so it is computed there and cached for the read-only
     * /control getter. ave_valid gates a torn/zero read before the first frame.
     * Unused on non-T31 builds. */
    volatile int ave_valid;
    double       ave_bitrate;
    /* cumulative producer-side frame drops (hal_enc_stat.au_drops). Single
     * writer (this channel's encode thread), read lock-free by /control. */
    volatile unsigned au_drops;
    /* Fix 1: capture-accurate, strictly-monotonic video presentation clock.
     * Replaces the old ms_now_us()-at-publish stamping that jittered and
     * burst-collapsed the RTP/fMP4 media clock. See pts_sanitize(). */
    int           fps;           /* configured output fps (nominal frame interval) */
    pts_sanitizer pts;           /* video capture-pts sanitizer state */
#ifdef ROT_HAS_SW_90
    /* Unbound SW-rotate path (Batch 5, T23 only): when sw_rot != 0 this slot
     * has NO encoder group/channel, NO OSD group and NO IMP_System_Bind - a
     * dedicated sw_rot_thread pulls raw NV12 frames off the (unbound)
     * framesource, rotates them in software into 'bounce' and feeds the
     * unbound IMP_Encoder_Yuv* encoder. grp/og/nbound stay -1/-1/0 so the
     * generic teardown never touches encoder/bind state for these slots. */
    int          sw_rot;      /* 0 = normal bound path, else 90 (CW) / 270 (CCW) */
    int          si;          /* video stream index (OSD item set owner) */
    int          src_w, src_h;/* PRE-rotation framesource dims */
    void        *yuv_h;       /* IMP_Encoder_YuvInit handle */
    uint8_t     *bounce;      /* rotated NV12 frame (IMP_Encoder_VbmAlloc:
                               * encoder input must be phys-contiguous; plain
                               * malloc memory is rejected/DMA-garbled) */
    uint32_t     bounce_phys; /* physical addr of bounce (VbmV2P) */
    uint32_t     bounce_size; /* eff_w*eff_h*3/2 */
    uint8_t     *ybuf;        /* CALLER-OWNED YuvEncode output buffer: on this
                               * libimp IMPEncoderYuvOut is IN/OUT - outAddr/
                               * outLen must carry a valid buffer + capacity IN
                               * (see contract note at the YuvEncode call) */
    uint32_t     ybuf_cap;    /* capacity handed to libimp in out.outLen */
    sw_osd_state osd;         /* software OSD compositing state (5b) */
    /* Batch 7: standalone JPEG for the SW-rotate stream. This unbound path has
     * NO encoder group, so the normal piggyback JPEG (jpeg_attach) cannot
     * register onto it. Instead sw_rot_thread feeds the already-rotated NV12
     * bounce frame to the documented standalone encoder IMP_Encoder_InputJpege
     * (T23 1.3.0 imp_encoder.h:1501) and publishes to HUB_JPEG_SRC_N(si) - the
     * same hub source id, media type (MS_MEDIA_JPEG) and on-demand contract as
     * jpeg_attach, so /snapshot.jpg?chn=N and /stream.mjpeg?chn=N work here too. */
    int          jpeg_on;     /* v->jpeg_enabled: emit JPEGs for this stream */
    int          jpeg_q;      /* v->jpeg_quality (1..100, default 75) */
    uint8_t     *jbuf;        /* standalone-JPEG output buffer (malloc'd once) */
    uint32_t     jbuf_cap;    /* capacity of jbuf */
    int64_t      jpeg_period; /* min us between JPEGs (1e6 / jpeg_fps) */
    int64_t      jpeg_next;   /* next allowed JPEG timestamp (throttle) */
#endif
} vchan;
static vchan g_v[MS_MAX_VSTREAM];
static int   g_nv;
static volatile int g_arun, g_aactive;
static pthread_t    g_athr;
static int          g_acodec = MS_AC_PCMU;   /* effective audio codec */
static int          g_asr    = 8000;         /* effective audio sample rate */
static int          g_ach    = 1;            /* effective channel count published
                                              * to the hub: 2 = SIMULATED stereo
                                              * (the mono AI capture duplicated
                                              * to L=R, encoded as 2-ch AAC).
                                              * AAC only - G.711 stays mono. */
static IMPAudioIOAttr g_aio;                 /* attr accepted by IMP_AI_SetPubAttr */
static volatile int   g_ai_up = 0;           /* AI dev 0/chn 0 enabled (audio_thread) */
#if defined(USE_CONTROL) || defined(USE_BACKCHANNEL) || defined(USE_PLAY)
/* g_ai_lock serializes the live AI parameter writes (volume/gain/alc_gain) that
 * /control applies directly. The DSP module toggles (HPF/AGC/NS) are NOT applied
 * live at all: libimp runs them on its own internal record thread and
 * IMP_AI_Disable{Agc,Ns,Hpf} frees that state unlocked, so any live toggle
 * races the vendor thread (-> UAF/SIGSEGV in libaudioProcess.so). They are
 * persist-only and applied once at boot before the frame loop.
 *
 * Item-1: AEC (IMP_AI_{Enable,Disable}Aec) is the SAME free-while-recording
 * DSP-module family, but it CANNOT be boot-only: it references the lazily-
 * opened AO (dev0/chn0), so it is toggled live at every hal_ao_open/close
 * (backchannel hangup / play completion) on the backchannel/play threads.
 * Those Enable/Disable calls and audio_thread's frame fetch on the same
 * dev0/chn0 therefore take this lock, so a DisableAec can never free the AEC
 * module while a GetFrame is reading the AI channel it lives on. That is why
 * the guard is broadened past USE_CONTROL to the AEC-capable builds too. */
static pthread_mutex_t g_ai_lock = PTHREAD_MUTEX_INITIALIZER;
#endif
/* JPEG channels: [0] = dedicated jpeg.* channel (own framesource), further
 * entries = optional encoders piggybacked on a video stream's encoder group
 * (videoN.jpeg = true) which share that stream's framesource (no extra rmem) */
typedef struct {
    int          chn;        /* IMP encoder channel */
    int          fs_chn;     /* framesource feeding it (own or the video's) */
    int          src;        /* hub source id (HUB_JPEG_SRC / HUB_JPEG_SRC_N) */
    int          w, h, fps;
    int          snapshot;   /* periodic file snapshot (dedicated chn only) */
    int          has_thr;    /* pthread_create succeeded: join in stop (M8) */
    int64_t      last_snapshot_us; /* ms_now_us() of the last file write (M8) */
    volatile int run, active;
    pthread_t    thr;
} jchan;
static jchan g_j[1+MS_MAX_VSTREAM];
static int   g_nj;

/* ---- idle blocking + on-demand framesource ----
 * Idle producer threads block on g_act_cond instead of usleep-polling.
 * ing_set_active() broadcasts on every activation; the 1 s timeout is only a
 * safety net against lost wakeups and bounds the shutdown join latency. */
static pthread_mutex_t g_act_mtx  = PTHREAD_MUTEX_INITIALIZER;
/* A2: CLOCK_MONOTONIC condvar (runtime-init, act_once) - a wall-clock/NTP step
 * (common shortly after boot, exactly when the first viewer connects) must not
 * stretch the 1 s safety timeout. Matches every other wait primitive here
 * (fanqueue.c, events.c, util.c ms_stopgate). */
static pthread_cond_t  g_act_cond;
static pthread_once_t  g_act_once = PTHREAD_ONCE_INIT;
static void act_once(void)
{
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(&g_act_cond, &a);
    pthread_condattr_destroy(&a);
}
static void act_wake(void)
{
    pthread_once(&g_act_once, act_once);
    pthread_mutex_lock(&g_act_mtx);
    pthread_cond_broadcast(&g_act_cond);
    pthread_mutex_unlock(&g_act_mtx);
}
/* A2: block an idle producer until its stream is (re)activated or ~1 s elapses.
 * ready(arg) is the caller's wake condition (the same active-flag / hub_active
 * level test its main loop uses), re-checked UNDER g_act_mtx in a loop. This
 * closes the lost-wakeup race the old single unconditional wait had: a
 * subscriber that arrives between the caller's want-check and this wait sets its
 * active flag before ing_set_active()'s act_wake(); the predicate re-test here
 * runs under the same mutex act_wake() takes, so it either observes the flag and
 * returns at once, or is still blocked and is delivered the broadcast - the
 * wakeup is never slept through. ready() only reads plain flags / hub_active()
 * (the hub source lock); ing_set_active() releases the hub source lock before
 * calling act_wake() (hub.c:18), so acquiring the source lock under g_act_mtx
 * here introduces no lock-order cycle. A spurious wake or ETIMEDOUT just
 * re-tests the predicate against the same absolute deadline, so there is no
 * busy-loop and the total wait stays bounded at ~1 s. */
static void act_wait(int (*ready)(void *), void *arg)
{
    pthread_once(&g_act_once, act_once);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += 1;
    pthread_mutex_lock(&g_act_mtx);
    while (!ready(arg)) {
        if (pthread_cond_timedwait(&g_act_cond, &g_act_mtx, &ts) == ETIMEDOUT)
            break;
    }
    pthread_mutex_unlock(&g_act_mtx);
}
/* act_wait() wake predicates: the same level test each producer's main loop
 * uses, minus the periodic/time-based parts (e.g. jpeg's snapshot-due) that get
 * no act_wake() and are correctly served by the 1 s safety timeout re-poll.
 * MUST also return true when the thread's run flag is cleared: ing_stop() drops
 * run/g_arun then act_wake()s once to make every idle-blocked thread return
 * promptly for the join - without the flag here the predicate would still be
 * false and the thread would re-wait up to the full 1 s, delaying shutdown. */
static int act_ready_vchan(void *a){ vchan *vc=(vchan*)a; return !vc->run  || vc->active || hub_active(vc->chn); }
static int act_ready_jchan(void *a){ jchan *jc=(jchan*)a; return !jc->run  || jc->active || hub_active(jc->src); }
static int act_ready_audio(void *a){ (void)a;             return !g_arun  || g_aactive; }

/* A FrameSource channel is enabled only while someone consumes its frames.
 * An enabled FS keeps the whole bound pipeline (FS -> OSD -> encoder group)
 * pumping frames at sensor fps inside libimp's worker threads even when the
 * encoder channel has StopRecvPic'd - that was ~19 % idle CPU with zero
 * clients. Refcounted because a video stream and its piggybacked JPEG
 * encoder share one FS (and motion detection pins the monitored stream). */
#define MS_FS_MAXCHN 8
static pthread_mutex_t g_fs_mtx = PTHREAD_MUTEX_INITIALIZER;
static int g_fs_users[MS_FS_MAXCHN];
/* F-fs/V2 hardening: tracks whether EnableChn has actually SUCCEEDED for this
 * channel, independent of the refcount. The refcount alone used to gate the
 * EnableChn attempt (only tried on the 0->1 edge) and was kept even when
 * EnableChn failed - callers still symmetrically fs_unuse() what they
 * fs_use()'d regardless of whether the hardware call inside it succeeded, so
 * unwinding the refcount on failure would desync it from callers' own
 * bookkeeping. That meant a failed 0->1 EnableChn left the channel
 * "logically on, physically off" until the count fully drained to 0 - which
 * can be never, e.g. while motion detection holds its own reference (imp_
 * motion.c pins the monitored stream's FS for as long as motion is enabled).
 * It also made the video/jpeg watchdogs' recovery cycle (fs_unuse()+fs_use())
 * a no-op whenever another holder kept the count above 0 across the cycle
 * (V2) - the exact failure class those watchdogs exist to recover from.
 * Retrying independently of the refcount transition fixes both: any fs_use()
 * call retries the real hardware enable until it actually reports success. */
static int g_fs_enabled[MS_FS_MAXCHN];
/* ISP tuning access lock. Defined HERE (not down in the USE_CONTROL /control
 * block alongside its only other user, ing_control()) because fs_use() below
 * now re-applies image tuning under it on every real chn0 enable edge (see the
 * relatch note in fs_kick_chn0's comment) and fs_use() is compiled into every
 * build, USE_CONTROL or not. isp_apply_image() is defined further down in the
 * ISP section; forward-declared so fs_use()'s chn0 relatch can reach it. */
static pthread_mutex_t g_isp_lock = PTHREAD_MUTEX_INITIALIZER;
static int isp_apply_image(const char *k);
static void fs_use(int chn)
{
    if (chn < 0 || chn >= MS_FS_MAXCHN) return;
    pthread_mutex_lock(&g_fs_mtx);
    int first = (g_fs_users[chn]++ == 0);
    int just_enabled = 0;
    if (!g_fs_enabled[chn]) {
        if (IMP_FrameSource_EnableChn(chn) != 0)
            LOGE(MOD,"framesource %d: EnableChn failed%s", chn,
                 first ? "" : " (retry)");
        else {
            LOGI(MOD,"framesource %d enabled", chn);
            g_fs_enabled[chn] = 1;
            just_enabled = 1;
        }
    }
    pthread_mutex_unlock(&g_fs_mtx);
    /* A real chn0 EnableChn (the genuine 0->1 hardware edge, NOT a refcount
     * bump on an already-live channel) is suspected of wiping the ISP-side
     * flip/running_mode latch, so an ALREADY-correct value can silently revert
     * across an ordinary on-demand idle->active cycle - observed live on
     * cam-L (T23/sc2336) 2026-08-09: 82 min uptime, no reboot, config still
     * hflip=1/vflip=1 yet the image was upside-down after chn0 had idled and
     * restarted for a reconnecting viewer, and a plain re-POST of the same
     * values fixed it instantly. That is consistent with this theory but does
     * NOT isolate it from other candidate causes (e.g. a running_mode/day-night
     * switch independently resetting the flip - see the belt-and-braces
     * re-assert in ing_control()); treat this as the leading hypothesis, not a
     * proven root cause. Re-issuing the Set calls here is harmless either way
     * and self-heals whichever mechanism is at play whenever chn0 genuinely
     * re-enables: the value re-latches once this caller's frame loop (about
     * to pull frames anyway) pumps chn0 - no synthetic sleep needed, unlike
     * fs_kick_chn0. Done OUTSIDE g_fs_mtx and ONLY on the true enable edge, so
     * refcount bumps on an already-streaming chn0 stay a cheap no-op. Lock order
     * is g_fs_mtx (released just above) then g_isp_lock; the only other
     * g_isp_lock holder, ing_control(), takes no further lock under it, so there
     * is no cycle. Guarded on g_hcfg (NULL only very early pre-config, and even
     * then the 0/0/day defaults are harmless). isp_apply_image() no-ops on SoCs
     * where these keys are unwired. */
    if (just_enabled && chn == 0 && g_hcfg) {
        pthread_mutex_lock(&g_isp_lock);
        isp_apply_image("hflip");
        isp_apply_image("vflip");
        isp_apply_image("running_mode");
        pthread_mutex_unlock(&g_isp_lock);
    }
}
static void fs_unuse(int chn)
{
    if (chn < 0 || chn >= MS_FS_MAXCHN) return;
    pthread_mutex_lock(&g_fs_mtx);
    if (g_fs_users[chn] > 0 && --g_fs_users[chn] == 0) {
        IMP_FrameSource_DisableChn(chn);
        g_fs_enabled[chn] = 0;
        LOGI(MOD,"framesource %d disabled (idle)", chn);
    }
    pthread_mutex_unlock(&g_fs_mtx);
}

/* ISP latch kick. Several ISP settings only take effect while framesource chn0
 * is delivering frames: the SDK Set call returns 0 but the change sits queued in
 * the driver until chn0 pumps a couple of frames. With the on-demand FS above,
 * chn0 is idle whenever nothing subscribes (and, crucially, at boot before any
 * client connects), so the queued change never latches.
 *
 * Two settings in this class are known:
 *  - running_mode (day/night): root-caused on cam-L Y4 (T23 / sc2336, ISP
 *    H20241028a / isp_20250722a) 2026-08-01 - the dusk switch-to-night plus BOTH
 *    daynight re-asserts returned rc=0 with chn0 idle, /proc/jz/isp/isp-m0
 *    stayed "Runing Mode : Day", and the always-on ch1 substream showed the
 *    magenta IR-in-colour-mode image all evening. Merely enabling chn0 (a
 *    snapshot) latched the queued mode within a couple of frames, no further Set.
 *  - image.hflip / image.vflip: same class, found 2026-08-09 on the same board.
 *    apply_image_tuning() sets the flip at boot (isp_init) BEFORE any FS exists,
 *    and it is never re-applied when the FS later starts, so with chn0 idle the
 *    flip never latches: the sensor comes up UNFLIPPED despite hflip=1/vflip=1
 *    (image upside-down), and only a live /control POST landing while the WebUI
 *    preview streams chn0 makes it take effect. RTSP masks a resulting constant
 *    A/V-independent orientation; the operator sees "live flip works, boot flip
 *    doesn't" for the same 1/1 values.
 *
 * A SECOND, distinct failure mode in the same latch class: a value that DID
 * latch can silently REVERT with no new Set involved. Observed live on cam-L
 * 2026-08-09 (see fs_use for the incident detail) coinciding with a chn0
 * idle->active cycle; IMP_FrameSource_DisableChn/EnableChn on chn0 - the normal
 * on-demand idle->active cycle (last viewer leaves, chn0 idles after
 * MS_IDLE_STOP_US, a later viewer re-enables it; also the video/jpeg watchdog
 * recovery cycle and a motion (re)pin on stream 0) - is the leading suspect for
 * resetting the ISP flip/running_mode latch, but a running_mode/day-night
 * switch resetting the flip independently of any chn0 edge has not been ruled
 * out either (hence the belt-and-braces re-assert in ing_control()). Either
 * way this is NOT an "apply while idle" case - no new Set is issued - so the
 * kick machinery here does not cover it. fs_use() now self-heals the chn0-edge
 * case directly: every genuine chn0 0->1 enable edge re-applies hflip/vflip/
 * running_mode under g_isp_lock. So the kick callers below - ing_control_commit()'s
 * pending flag, ing_start()'s boot-time kick, ing_control()'s inline running_mode
 * kick - remain necessary for the "apply a value while chn0 is genuinely idle
 * with nobody about to consume frames" case (a /control POST while no one is
 * watching); the idle->active-cycle case is now also handled automatically by
 * fs_use().
 *
 * So: after applying any of these, run chn0 for a few frames if it is idle.
 * Refcounted fs_use makes this a cheap ref bump / no-op when chn0 is already
 * streaming (a subscriber, or a motion pin on stream 0). ~500 ms = ~12 frames
 * @25fps, generous margin over the 1-2 frames the latch needs. Callers must NOT
 * hold g_isp_lock (fs_use -> IMP_FrameSource_EnableChn must not race an ISP Set
 * under that lock; and fs_use() now takes g_isp_lock itself on the chn0 enable
 * edge, so a caller already holding it would deadlock). */
#define FS0_KICK_US 500000
static void fs_kick_chn0(void)
{
    /* the latch belongs to the ISP's direct-output channel 0; per-stream
     * imp_chn maps stream 0 -> fs chn 0 (see fs_create) */
    if (!g_hcfg || !g_hcfg->video[0].enabled) return;
    fs_use(0);
    usleep(FS0_KICK_US);
    fs_unuse(0);
}

/* ================= motion detection lifecycle ================= */
/* Bring the IVS motion grid in sync with the current config. ANY runtime
 * change (enable/disable, cols/rows, sensitivity, monitor_stream) goes
 * through a clean stop + recreate: the move parameters (grid ROIs, sense)
 * are create-time attributes of the IVS interface, so live geometry or
 * sensitivity changes rebuild the channel. While motion runs, the monitored
 * stream's FrameSource is pinned (fs_use) so the idle logic never turns off
 * the frames the IVS group feeds on. Serialized: called from ing_start/
 * ing_stop (main thread) and from /control connection threads. */
static pthread_mutex_t g_motion_mtx = PTHREAD_MUTEX_INITIALIZER;
static int g_motion_pin = -1;    /* pinned FS channel while motion runs */
/* Per-stream rotation ACTUALLY applied at bring-up, as opposed to the
 * requested videoN.rotation. The two differ exactly when a 90/270 request was
 * REFUSED by a rotation safe-envelope check in ing_start (T23 sw_rot_start
 * SW_ROT_FALLBACK, T31 fs_create FS_ROT_FALLBACK) and the stream came up
 * UNROTATED. That refusal decision exists only here in hal_ingenic.c;
 * motion_sync() uses this record to hand IVS an EFFECTIVE copy of the
 * monitored stream's config, so motion geometry always matches the frames
 * the framesource really delivers - the same fix pattern as jpeg_attach()
 * taking the caller's effective v instead of re-reading the raw config.
 * rotation is restart-only (control.c VID_REST), so the value recorded at
 * ing_start stays valid for every later live motion_sync() rebuild. */
static int g_eff_rot[MS_MAX_VSTREAM];
/* M2: set by ing_control() when a motion.* key or a monitored-stream privacy
 * key changes during a /control request; ing_control_commit() (called once per
 * request via hub_control_commit) does the single motion_sync() rebuild. */
static int g_motion_resync_pending = 0;
/* Item-1: a pure sensitivity change (no geometry/monitor_stream/enabled change)
 * can update the running IVS channel's sense[] in place via IMP_IVS_SetParam,
 * skipping the full stop/destroy/recreate. Tracked separately so that if a
 * geometry key ALSO changes in the same request the full rebuild
 * (g_motion_resync_pending) supersedes and covers sensitivity too. */
static int g_motion_sense_pending = 0;
static void motion_sync(const ms_config *cfg)
{
    pthread_mutex_lock(&g_motion_mtx);
    if (g_motion_pin >= 0) {                     /* running -> stop first */
        imp_motion_stop();
        fs_unuse(g_motion_pin);
        g_motion_pin = -1;
    }
    if (cfg->motion.enabled) {
        int mon = cfg->motion.monitor_stream;
        if (mon < 0 || mon >= MS_MAX_VSTREAM || !g_cfg_boot.video[mon].enabled)
            mon = 0;
        /* Hand IVS the EFFECTIVE monitored-stream config: geometry from
         * g_cfg_boot (videoN.* is restart-only - a live /control write updates
         * g_cfg but NOT the running pipeline, see the g_cfg_boot WHY block in
         * config.h; a live rebuild must not pick up dims the encoder is not
         * producing), with rotation overridden to what ing_start ACTUALLY
         * applied (g_eff_rot), so a safe-envelope-refused 90/270 no longer
         * makes IVS run with swapped frame dims / a rotation-mapped ROI grid
         * against the unrotated frames the framesource really delivers. */
        ms_vstream_cfg mv = g_cfg_boot.video[mon];
        mv.rotation = g_eff_rot[mon];
        if (imp_motion_start(cfg, &mv, mon) == 0) {
            /* IVS needs the monitored stream's frames independent of
             * clients: pin that framesource until motion stops */
            g_motion_pin = mv.imp_chn;
            fs_use(g_motion_pin);
        }
    }
    pthread_mutex_unlock(&g_motion_mtx);
}

/* ================= system / sensor / ISP ================= */
/* Apply one image.* (ISP tuning) key from the current config (g_hcfg->image).
 * Returns 1 when the key is wired on this PLATFORM's IMP SDK, 0 when the SoC
 * cannot do it (the value is still parsed/persisted by the config layer).
 * The per-SoC guards come from ../isp_caps.h - keep them in sync with the
 * caps.image list control.c reports. Callers serialize ISP access (g_isp_lock
 * for live control; init is single-threaded). */
static int isp_apply_image(const char *k)
{
#if defined(NO_TUNINGS)
    (void)k;
    return 1;
#else
    const ms_image_cfg *im = &g_hcfg->image;
#ifdef ISP_NEW_TUNING_API           /* T40/T41: IMPVI_NUM + pointer args */
    if (!strcmp(k,"brightness")){ unsigned char u=(unsigned char)im->brightness;
        IMP_ISP_Tuning_SetBrightness(IMPVI_MAIN,&u); return 1; }
    if (!strcmp(k,"contrast")){ unsigned char u=(unsigned char)im->contrast;
        IMP_ISP_Tuning_SetContrast(IMPVI_MAIN,&u); return 1; }
    if (!strcmp(k,"saturation")){ unsigned char u=(unsigned char)im->saturation;
        IMP_ISP_Tuning_SetSaturation(IMPVI_MAIN,&u); return 1; }
    if (!strcmp(k,"sharpness")){ unsigned char u=(unsigned char)im->sharpness;
        IMP_ISP_Tuning_SetSharpness(IMPVI_MAIN,&u); return 1; }
    if (!strcmp(k,"hue")){ unsigned char u=(unsigned char)im->hue;
        IMP_ISP_Tuning_SetBcshHue(IMPVI_MAIN,&u); return 1; }
    if (!strcmp(k,"hflip") || !strcmp(k,"vflip")){
        /* T40/T41: the raw image.hflip/vflip drive the ISP/sensor flip directly.
         * (These SoCs realise per-channel rotation via I2D downstream, kept
         * independent of this flip path.) */
        IMPISPHVFLIP m = im->hflip
            ? (im->vflip ? IMPISP_FLIP_HV_MODE : IMPISP_FLIP_H_MODE)
            : (im->vflip ? IMPISP_FLIP_V_MODE  : IMPISP_FLIP_NORMAL_MODE);
#if defined(PLATFORM_T41)
        /* T41 wraps the mode in IMPISPHVFLIPAttr (sensor + per-channel ISP);
         * flip at the sensor, leave the ISP channels at NORMAL */
        IMPISPHVFLIPAttr fa; memset(&fa,0,sizeof fa);
        fa.sensor_mode = m;
        IMP_ISP_Tuning_SetHVFLIP(IMPVI_MAIN,&fa);
#else
        IMP_ISP_Tuning_SetHVFLIP(IMPVI_MAIN,&m);
#endif
        return 1;
    }
    if (!strcmp(k,"running_mode")){
        IMPISPRunningMode m = im->running_mode ? IMPISP_RUNNING_MODE_NIGHT
                                               : IMPISP_RUNNING_MODE_DAY;
        /* the SDK returns a status; a drop at the dusk day->night crossover left
         * the sensor colour under IR while the config claimed night. It used to
         * be ignored - surface it so an apply failure is visible, not silent. */
        int rc = IMP_ISP_Tuning_SetISPRunningMode(IMPVI_MAIN,&m);
        if (rc) LOGW(MOD,"SetISPRunningMode(%s) failed (rc=%d)",
                     im->running_mode?"night":"day", rc);
        return 1;
    }
    if (!strcmp(k,"anti_flicker")){ /* 0 off, 1 = 50 Hz, 2 = 60 Hz */
        IMPISPAntiflickerAttr fl; memset(&fl,0,sizeof fl);
        fl.mode = im->anti_flicker ? IMPISP_ANTIFLICKER_NORMAL_MODE
                                   : IMPISP_ANTIFLICKER_DISABLE_MODE;
        fl.freq = (im->anti_flicker==2) ? 60 : 50;
        IMP_ISP_Tuning_SetAntiFlickerAttr(IMPVI_MAIN,&fl); return 1;
    }
#else                               /* classic API (T10..T31, C100) */
    if (!strcmp(k,"brightness")){ IMP_ISP_Tuning_SetBrightness((unsigned char)im->brightness); return 1; }
    if (!strcmp(k,"contrast")){   IMP_ISP_Tuning_SetContrast((unsigned char)im->contrast);     return 1; }
    if (!strcmp(k,"saturation")){ IMP_ISP_Tuning_SetSaturation((unsigned char)im->saturation); return 1; }
    if (!strcmp(k,"sharpness")){  IMP_ISP_Tuning_SetSharpness((unsigned char)im->sharpness);   return 1; }
    if (!strcmp(k,"hue")){
#ifdef ISP_HAS_HUE
        IMP_ISP_Tuning_SetBcshHue((unsigned char)im->hue); return 1;
#else
        return 0;
#endif
    }
    if (!strcmp(k,"hflip")){ IMP_ISP_Tuning_SetISPHflip((IMPISPTuningOpsMode)(im->hflip?1:0)); return 1; }
    if (!strcmp(k,"vflip")){ IMP_ISP_Tuning_SetISPVflip((IMPISPTuningOpsMode)(im->vflip?1:0)); return 1; }
    if (!strcmp(k,"running_mode")){
        /* the SDK returns a status; a drop at the dusk day->night crossover left
         * the sensor colour under IR while the config claimed night. It used to
         * be ignored - surface it so an apply failure is visible, not silent. */
        int rc = IMP_ISP_Tuning_SetISPRunningMode(im->running_mode ? IMPISP_RUNNING_MODE_NIGHT
                                                                   : IMPISP_RUNNING_MODE_DAY);
        if (rc) LOGW(MOD,"SetISPRunningMode(%s) failed (rc=%d)",
                     im->running_mode?"night":"day", rc);
        /* DIAGNOSTIC (LOGD, off by default): read the mode straight back. NOTE
         * GetISPRunningMode is a libimp userspace value (no kernel read path on
         * T23) - it echoes the last Set, so this can NOT prove the pipeline
         * latched; it only helps spot a gross SDK disagreement if one ever shows
         * up during a stuck recurrence with debug logging enabled. */
        { IMPISPRunningMode gm = (IMPISPRunningMode)-1;
          int gr = IMP_ISP_Tuning_GetISPRunningMode(&gm);
          LOGD(MOD,"running_mode set=%d GetISPRunningMode->%d (rc=%d, cached echo)",
               im->running_mode?1:0, (gr==0)?(int)gm:-1, gr); }
        return 1;
    }
    if (!strcmp(k,"anti_flicker")){ /* enum: 0 off, 1 = 50 Hz, 2 = 60 Hz */
        IMP_ISP_Tuning_SetAntiFlickerAttr((IMPISPAntiflickerAttr)im->anti_flicker);
        return 1;
    }
    if (!strcmp(k,"ae_compensation")){
#ifdef ISP_HAS_AECOMP
        IMP_ISP_Tuning_SetAeComp(im->ae_compensation); return 1;
#else
        return 0;
#endif
    }
    if (!strcmp(k,"max_again")){ IMP_ISP_Tuning_SetMaxAgain((uint32_t)im->max_again); return 1; }
    if (!strcmp(k,"max_dgain")){ IMP_ISP_Tuning_SetMaxDgain((uint32_t)im->max_dgain); return 1; }
    if (!strcmp(k,"sinter_strength")){ IMP_ISP_Tuning_SetSinterStrength((uint32_t)im->sinter_strength); return 1; }
    if (!strcmp(k,"temper_strength")){ IMP_ISP_Tuning_SetTemperStrength((uint32_t)im->temper_strength); return 1; }
    if (!strcmp(k,"dpc_strength")){
#ifdef ISP_HAS_DPC
        IMP_ISP_Tuning_SetDPC_Strength((unsigned int)im->dpc_strength); return 1;
#else
        return 0;
#endif
    }
    if (!strcmp(k,"defog_strength")){
#ifdef ISP_HAS_DEFOG
        uint8_t d=(uint8_t)im->defog_strength;
        IMP_ISP_Tuning_SetDefog_Strength(&d); return 1;
#else
        return 0;
#endif
    }
    if (!strcmp(k,"drc_strength")){
#ifdef ISP_HAS_DRC
        IMP_ISP_Tuning_SetDRC_Strength((unsigned int)im->drc_strength); return 1;
#else
        return 0;
#endif
    }
    if (!strcmp(k,"highlight_depress")){ /* 0 disables */
        IMP_ISP_Tuning_SetHiLightDepress((uint32_t)im->highlight_depress); return 1;
    }
    if (!strcmp(k,"backlight_compensation")){
#ifdef ISP_HAS_BACKLIGHT
        IMP_ISP_Tuning_SetBacklightComp((uint32_t)im->backlight_compensation); return 1;
#else
        return 0;
#endif
    }
    /* white balance: mode + gains are one IMPISPWB, applied on any of them */
    if (!strcmp(k,"core_wb_mode")||!strcmp(k,"wb_rgain")||!strcmp(k,"wb_bgain")){
        IMPISPWB wb; memset(&wb,0,sizeof wb);
        wb.mode=(enum isp_core_wb_mode)im->core_wb_mode;
        wb.rgain=(uint16_t)im->wb_rgain; wb.bgain=(uint16_t)im->wb_bgain;
        IMP_ISP_Tuning_SetWB(&wb); return 1;
    }
#endif /* ISP_NEW_TUNING_API */
    return 0;
#endif /* NO_TUNINGS */
}

/* apply the whole image.* (ISP) tuning block (boot + config reload).
 * "core_wb_mode" stands in for the whole WB triple (one SetWB call). */
static void apply_image_tuning(void)
{
#if !defined(NO_TUNINGS)
    static const char *const keys[] = {
        "brightness","contrast","saturation","sharpness","hue",
        "hflip","vflip","running_mode","anti_flicker","ae_compensation",
        "max_again","max_dgain","sinter_strength","temper_strength",
        "dpc_strength","defog_strength","drc_strength","highlight_depress",
        "backlight_compensation","core_wb_mode"
    };
    for (size_t i=0;i<sizeof keys/sizeof keys[0];i++)
        if (!isp_apply_image(keys[i]))
            LOGD(MOD,"image.%s unsupported on this platform (skipped)",keys[i]);
    const ms_image_cfg *im = &g_hcfg->image;
    LOGI(MOD,"image tuning applied (bri=%d con=%d sat=%d sharp=%d)",
         im->brightness,im->contrast,im->saturation,im->sharpness);
#endif
}

static int isp_init(void)
{
    int ret;
    memset(&g_sensor,0,sizeof g_sensor);
    /* bounded copies: sensor.model (64) is larger than name (32) / i2c.type (20).
     * sensor.model is runtime-mutable via /control, so read it under
     * config_str_lock rather than directly off g_hcfg (M3). */
    config_str_lock();
    snprintf(g_sensor.name, sizeof g_sensor.name, "%.*s",
             (int)sizeof(g_sensor.name)-1, g_hcfg->sensor.model);
    g_sensor.cbus_type = TX_SENSOR_CONTROL_INTERFACE_I2C;
    snprintf(g_sensor.i2c.type, sizeof g_sensor.i2c.type, "%.*s",
             (int)sizeof(g_sensor.i2c.type)-1, g_hcfg->sensor.model);
    config_str_unlock();
    g_sensor.i2c.addr = g_hcfg->sensor.i2c_addr;

#if !(defined(PLATFORM_T40)||defined(PLATFORM_T41))
    IMP_OSD_SetPoolSize(g_hcfg->osd_pool_size * 1024);
#endif
    ret = IMP_ISP_Open();
    if (ret<0){ LOGE(MOD,"IMP_ISP_Open failed"); return -1; }

    /* AddSensor/EnableSensor were not return-checked: with a wrong sensor
     * model/i2c address they fail, yet init used to march on and bring up a
     * pipeline that could never deliver a frame (silent no-video). Fail hard
     * here instead, unwinding exactly what was already opened. */
#if defined(PLATFORM_T40)||defined(PLATFORM_T41)
    if (IMP_ISP_AddSensor(IMPVI_MAIN, &g_sensor) < 0){
        LOGE(MOD,"IMP_ISP_AddSensor failed (sensor=%s)", g_sensor.name);
        IMP_ISP_Close();
        return -1;
    }
    if (IMP_ISP_EnableSensor(IMPVI_MAIN, &g_sensor) < 0){
        LOGE(MOD,"IMP_ISP_EnableSensor failed (sensor=%s)", g_sensor.name);
        IMP_ISP_DelSensor(IMPVI_MAIN, &g_sensor);
        IMP_ISP_Close();
        return -1;
    }
#else
    if (IMP_ISP_AddSensor(&g_sensor) < 0){
        LOGE(MOD,"IMP_ISP_AddSensor failed (sensor=%s)", g_sensor.name);
        IMP_ISP_Close();
        return -1;
    }
    if (IMP_ISP_EnableSensor() < 0){
        LOGE(MOD,"IMP_ISP_EnableSensor failed (sensor=%s)", g_sensor.name);
        IMP_ISP_DelSensor(&g_sensor);
        IMP_ISP_Close();
        return -1;
    }
#endif
    /* on a fast restart the previous instance's IMP/rmem may not be released
     * yet - retry a few times before giving up instead of exiting the daemon */
    {
        int si_tries = 0;
        while (IMP_System_Init() < 0) {
            if (++si_tries >= 5) {
                LOGE(MOD,"IMP_System_Init failed after %d tries", si_tries);
#if defined(PLATFORM_T40)||defined(PLATFORM_T41)
                IMP_ISP_DisableSensor(IMPVI_MAIN);
                IMP_ISP_DelSensor(IMPVI_MAIN, &g_sensor);
#else
                IMP_ISP_DisableSensor();
                IMP_ISP_DelSensor(&g_sensor);
#endif
                IMP_ISP_Close();
                return -1;
            }
            LOGW(MOD,"IMP_System_Init busy, retry %d/5 in 1s (ISP still releasing?)", si_tries);
            usleep(1000000);
        }
    }
    /* non-fatal: without tuning the image.* keys won't apply, but frames flow */
    if (IMP_ISP_EnableTuning() < 0)
        LOGW(MOD,"IMP_ISP_EnableTuning failed - image tuning unavailable");
    apply_image_tuning();   /* full image.* block incl. running_mode */
#if defined(PLATFORM_T41)
    { IMPISPSensorFps fps={ .num=(uint32_t)g_hcfg->sensor.fps, .den=1 };
      IMP_ISP_Tuning_SetSensorFPS(IMPVI_MAIN,&fps); }
#elif defined(ISP_NEW_TUNING_API)   /* T40 */
    { uint32_t fn=(uint32_t)g_hcfg->sensor.fps, fd=1;
      IMP_ISP_Tuning_SetSensorFPS(IMPVI_MAIN,&fn,&fd); }
#else
    IMP_ISP_Tuning_SetSensorFPS(g_hcfg->sensor.fps, 1);
#endif
    IMP_System_GetVersion(NULL);

    /* Ask the ISP for the sensor's REAL output resolution (chip-independent).
     * Some sensor drivers report 0x0 to the framesource, which makes IMP reject
     * a non-cropped/non-scaled channel; using this for the crop/scale decision
     * in fs_create fixes video for ANY sensor. Falls back to the configured
     * resolution when the API is absent (T10/T20/T21/T30) or returns 0. */
#ifdef ISP_HAS_SENSOR_ATTR
    { IMPISPSENSORAttr sa; memset(&sa,0,sizeof sa);
#if defined(PLATFORM_T40)||defined(PLATFORM_T41)
      int sret = IMP_ISP_Tuning_GetSensorAttr(IMPVI_MAIN, &sa);
#else
      int sret = IMP_ISP_Tuning_GetSensorAttr(&sa);
#endif
      if (sret==0 && sa.width>0 && sa.height>0){
          g_isp_sensor_w=(int)sa.width; g_isp_sensor_h=(int)sa.height;
          LOGI(MOD,"ISP sensor resolution %ux%u", sa.width, sa.height);
      }
    }
#endif

    config_str_lock();
    LOGI(MOD,"ISP up, sensor=%s fps=%d", g_hcfg->sensor.model, g_hcfg->sensor.fps);
    config_str_unlock();
    return 0;
}

#ifdef ROT_HAS_FS_ROTATE
/* T31 FrameSource-rotate safe envelope (Fix 1). The vendor hardware FS-rotate
 * path only handles 64-aligned geometry within <=1280x704 <=15fps. Past either
 * bound libimp silently falls back to a SOFTWARE rotate whose oversized/
 * misaligned geometry then drives the downstream Encoder_CreateChn into a
 * failure that used to abort the ENTIRE multi-stream pipeline and take the whole
 * daemon down (reproduced live on T31/sc4336p: a 1920x1080@25 rotate request the
 * /control API had accepted and persisted). 64-alignment and pixel area are both
 * swap-invariant, so checking the pre-rotation v->width/v->height is equivalent
 * to the post-rotation dims. These are named constants shared by the warning
 * text and the enforcement so the two can never drift apart; values match the
 * long-standing warning: <=1280x704, <=15fps, 64-aligned. */
#define MS_FS_ROT_ALIGN       64
#define MS_FS_ROT_MAX_PIXELS  (1280L*704L)
#define MS_FS_ROT_MAX_FPS     15
/* fs_create() return code: not success and not a hard failure, but "rotation
 * refused/failed for THIS stream - caller should bring it up UNROTATED". Kept
 * distinct from -1 (unrecoverable) so unrelated streams and this one (minus the
 * rotation) still come up instead of the whole pipeline aborting. Positive so
 * the shared `fs_create(...)!=0` call sites on other platforms still treat it as
 * an error there (it can only ever be returned under ROT_HAS_FS_ROTATE). */
#define FS_ROT_FALLBACK 1
#endif

#if defined(PLATFORM_T31)
/* T31(L) kernel one-buffer gate: with the tx-isp module parameter
 * isp_ch0_pre_dequeue_time > 0, kernel framechan0 rejects REQBUFS for more
 * than one buffer ("one buffer schedule only support nrvbs = 1" in dmesg,
 * IMP_FrameSource_EnableChn returns -1). The gate is ONLY (framechan index
 * == 0 && pre_dequeue active); crop/scaler configuration is irrelevant -
 * established 2026-08-21 by disassembling tx-isp-t31.o
 * frame_channel_unlocked_ioctl, superseding the "non-scaled channel" theory
 * of 2026-07-26 (full story: dev_notes/TODO.md, single-buffer section).
 * Reads the 0444 module parameter once and caches it; <0 = unreadable. */
static int t31_ch0_pre_dequeue_time(void)
{
    static int cached = -2;                       /* -2 = not probed yet */
    if (cached == -2) {
        cached = -1;
        FILE *f = fopen("/sys/module/tx_isp_t31/parameters/"
                        "isp_ch0_pre_dequeue_time", "r");
        if (f) {
            int val;
            if (fscanf(f, "%d", &val) == 1 && val >= 0) cached = val;
            fclose(f);
        }
        if (cached < 0)
            LOGW(MOD,"cannot read isp_ch0_pre_dequeue_time - assuming the "
                 "pre-dequeue one-buffer schedule is active on framechan0");
    }
    return cached;
}
#endif

/* ================= framesource ================= */
static int fs_create(int chn, const ms_vstream_cfg *v)
{
    IMPFSChnAttr a; memset(&a,0,sizeof a);
    a.pixFmt = PIX_FMT_NV12;
    a.outFrmRateNum = v->fps; a.outFrmRateDen = 1;
    a.type  = FS_PHY_CHANNEL;
    /* Use the ISP-reported real sensor resolution when known (works for any
     * chip), else the configured/detected one. This drives both the scale
     * decision and the crop dimensions, so a full-FOV downscale stays correct
     * even on a 4MP sensor whose /proc reports nothing. */
    int sw = g_isp_sensor_w>0 ? g_isp_sensor_w : g_hcfg->sensor.width;
    int sh = g_isp_sensor_h>0 ? g_isp_sensor_h : g_hcfg->sensor.height;
    int scale = (sw!=v->width)||(sh!=v->height);
    a.nrVBs = v->buffers>0 ? v->buffers : 2;
    /* T31(L): clamp chn0 to one buffer ONLY when the kernel's pre-dequeue
     * one-buffer gate is actually active (see t31_ch0_pre_dequeue_time above).
     * Boards whose /etc/modules.d/20-isp does not set the parameter (e.g.
     * wuuk T31X) keep the normal multi-buffer ring on chn0 - that, not the
     * scaler, is why they run at full rate. Boards that set it (e.g. Cinnado
     * D1: isp_ch0_pre_dequeue_time=24) get nrVBs=1 exactly as before, on
     * scaled AND non-scaled chn0 alike (the old !scale condition left a
     * scaled chn0 unclamped -> guaranteed EnableChn failure on those boards).
     * An explicit "buffers" line (v->buffers_explicit) is still trusted
     * as-is so a runtime-echoed pre_dequeue_time=0 can be probed without a
     * rebuild. */
#if defined(PLATFORM_T31)
    if (chn == 0 && a.nrVBs > 1) {
        int pdq = t31_ch0_pre_dequeue_time();
        if (pdq != 0) {                    /* active, or unknown: stay safe */
            if (v->buffers_explicit)
                LOGW(MOD,"chn0: explicit buffers=%d but isp_ch0_pre_dequeue_time=%d "
                     "forces a one-buffer schedule on framechan0 - EnableChn "
                     "will fail (dmesg: 'one buffer schedule') unless "
                     "pre-dequeue is disabled at the driver", a.nrVBs, pdq);
            else
                a.nrVBs = 1;
        }
    }
    /* boot diagnostic: the exact geometry/buffer request for this channel */
    LOGI(MOD,"chn%d: fs %dx%d (sensor %dx%d, scale=%d) nrVBs=%d",
         chn, v->width, v->height, sw, sh, scale, a.nrVBs);
#endif
    /* When crop AND scaler are both disabled, IMP requires the framesource
     * output to equal the ISP-reported sensor resolution. Some sensor drivers
     * (e.g. sc2336 on T23) report 0x0, so IMP then rejects the channel:
     * "invalid picture resolution WxH, but sensor resolution 0x0 when crop and
     * scaler all disabled" -> no frames -> no video. Declare the input
     * resolution explicitly via crop on the full-res (non-scaled) stream, like
     * raptor does; the scaled sub-streams already carry it via the scaler. */
    a.crop.enable = !scale;
    a.crop.width  = sw;  a.crop.height = sh;
    a.scaler.enable = scale;
    a.scaler.outwidth = v->width; a.scaler.outheight = v->height;
    a.picWidth = v->width; a.picHeight = v->height;
#if defined(PLATFORM_T23)
    /* T23 ABI tripwire: thingino ships libimp SDK 1.3.0 for the T23, whose
     * IMPFSChnAttr carries a trailing 'fcrop' (frame crop) member (added in
     * SDK 1.1.2). Building against the older 1.1.0 header makes this struct
     * 20 bytes short: libimp then reads stack garbage as the frame-crop and
     * the framesource delivers NO frames at all (attr validation still passes
     * and VBMCreatePool succeeds, but the pool stays empty and the encoder's
     * PollingStream times out forever - exactly the sc2336/Cinnado D1 bug).
     * The memset above already zeroes fcrop with a matching header; this
     * explicit store exists so a build against a header without 'fcrop'
     * FAILS TO COMPILE instead of producing a silently broken binary.
     * (T31 is unaffected: its 1.1.6 header/lib pair matches and has fcrop.) */
    a.fcrop.enable = 0;
#endif
#ifdef ROT_HAS_HW_I2D
    /* T40/T41 (Batch 3): true hardware I2D rotate via the IMPFSI2DAttr that is
     * the FIRST member of IMPFSChnAttr (a.i2dattr; imp_framesource.h struct
     * i2dattr -> IMPFSI2DAttr {i2d_enable,flip_enable,mirr_enable,rotate_enable,
     * rotate_angle}). 180 is done per-channel here (keeps the ISP flip pure -
     * Batch 2 deliberately leaves T40/T41 un-XORed for exactly this); 90/270 is
     * a real rotate with swapped FS output dims. */
    if (v->rotation!=0){
        a.i2dattr.i2d_enable = 1;
        if (v->rotation==180){
            /* per-channel 180 = Hflip + Vflip (flip + mirror) */
            a.i2dattr.flip_enable = 1;
            a.i2dattr.mirr_enable = 1;
        } else { /* 90 or 270 */
            a.i2dattr.rotate_enable = 1;
            /* ON-DEVICE VERIFY (no T40/T41 hardware here): rotate_angle units are
             * UNDOCUMENTED in the header. This sets DEGREES {90,270}; if on-device
             * the output is unrotated or CreateChn rejects the attr, try the enum
             * form {1,2,3}. */
            a.i2dattr.rotate_angle = v->rotation;
            /* FS output picture is post-rotation (swapped); the scaler stays at
             * the pre-rotation geometry set above. Which of picWidth/picHeight
             * vs scaler is pre- vs post-rotation is underdocumented - this
             * mirrors the T31 split (swap picWidth/picHeight, keep scaler).
             * ON-DEVICE VERIFY: if stride-garbled/green frames or SetChnAttr
             * rejects, flip that one knob. */
            a.picWidth = v->height; a.picHeight = v->width;
        }
    }
#endif
#ifdef ROT_HAS_FS_ROTATE
    /* T31 (Batch 4): FrameSource 90/270 rotate. IMP_FrameSource_SetChnRotate's
     * rotTo90 arg is an ENUM {0=off,1=90 CCW,2=90 CW} (imp_framesource.h:574-576),
     * NOT raw degrees - the previous code passed 90/270 verbatim, which the
     * driver read as garbage, and it also wrongly swapped the SetChnRotate dims. */
    if (v->rotation==90 || v->rotation==270){
        /* rotTo90 enum: 1 = 90 CCW, 2 = 90 CW. Mapped so config 90 -> CCW(1),
         * 270 -> CW(2), matching prudynt/raptor's raw rotation=1|2 semantics
         * (see src/config.c). */
        uint8_t r90 = (v->rotation==90) ? 1 : 2;
        /* CAPS-GATE (Fix 1): the previous code only WARNED here and then marched
         * on into a SetChnRotate + Encoder bring-up that libimp fails outside the
         * vendor envelope - which used to tear down the whole daemon (see the
         * MS_FS_ROT_* comment above). Now keep the accurate warning but REFUSE
         * the rotation (no IMP channel has been created yet at this point -
         * SetChnRotate/CreateChn are still below) and tell the caller to bring
         * this stream up UNROTATED rather than proceed into a doomed config. */
        if ((v->width|v->height) & (MS_FS_ROT_ALIGN-1)){
            LOGW(MOD,"video%d: refusing FS-rotate %dx%d not %d-aligned (T31 FS-rotate "
                     "wants %d-alignment) - stream will run UNROTATED",
                 chn, v->width, v->height, MS_FS_ROT_ALIGN, MS_FS_ROT_ALIGN);
            return FS_ROT_FALLBACK;
        }
        if ((long)v->width*v->height > MS_FS_ROT_MAX_PIXELS || v->fps > MS_FS_ROT_MAX_FPS){
            LOGW(MOD,"video%d: refusing FS-rotate %dx%d@%d exceeds vendor FS-rotate cap "
                     "(<=1280x704, <=15fps; past it libimp software-rotates and the "
                     "oversized geometry fails Encoder bring-up) - stream will run UNROTATED",
                 chn, v->width, v->height, v->fps);
            return FS_ROT_FALLBACK;
        }
        /* Args are the PRE-rotation dims per header :575-576, run BEFORE
         * CreateChn (header :581).
         * Defense in depth (Fix 2): even inside the safe envelope above, if the
         * FS rotate-enable call itself fails for some other reason (an unexpected
         * sensor/firmware quirk, a future envelope miscalculation), it must NOT
         * cascade into a total HAL/daemon start failure. SetChnRotate runs BEFORE
         * CreateChn, so no IMP channel exists yet on this path: refuse the
         * rotation for THIS stream and let the caller bring it up UNROTATED,
         * leaving every other stream's bring-up untouched. */
        if (IMP_FrameSource_SetChnRotate(chn, r90, v->width, v->height) < 0){
            LOGW(MOD,"video%d: FS-rotate enable (SetChnRotate) failed - disabling "
                     "rotation for this stream, bringing it up UNROTATED", chn);
            return FS_ROT_FALLBACK;
        }
        /* CRUCIAL (T31 OSD-on-rotation fix, see docs/T31-OSD-rotation-handoff.md):
         * the whole FS chnAttr stays at the PRE-rotation (landscape) geometry -
         * scaler (line ~444) AND picWidth/picHeight. Only the ENCODER is
         * post-rotation (via ms_vstream_eff_dims). Swapping the FS picWidth to
         * the portrait dim gives the bound IPU-OSD the wrong stride -> scattered
         * "dots" on every overlay; prudynt-t marks that exact swap "// Breaks
         * OSD" in IMPFramesource.cpp. So do NOT swap here. */
        a.picWidth = v->width; a.picHeight = v->height;
    }
#endif
    if (IMP_FrameSource_CreateChn(chn,&a)<0){ LOGE(MOD,"FS_CreateChn %d",chn); return -1; }
    if (IMP_FrameSource_SetChnAttr(chn,&a)<0){
        /* attr rejected -> the channel would run with whatever defaults
         * CreateChn left behind (wrong fps/size) or deliver nothing at all */
        LOGE(MOD,"FS_SetChnAttr %d failed",chn);
        IMP_FrameSource_DestroyChn(chn);
        return -1;
    }
#ifdef ROT_HAS_HW_I2D
    /* Belt-and-braces: some libimp builds only latch the I2D config via this
     * explicit call, not through CreateChn's attr. Signature confirmed in
     * imp_framesource.h: int IMP_FrameSource_SetI2dAttr(int chnNum, IMPFSI2DAttr*).
     * ON-DEVICE VERIFY: if unneeded/harmful, drop it. */
    if (v->rotation!=0 && IMP_FrameSource_SetI2dAttr(chn,&a.i2dattr)<0)
        LOGW(MOD,"FS_SetI2dAttr %d failed (rotation may stay inactive)",chn);
#endif
    return 0;
}

/* ================= encoder ================= */

/* The effective QP bounds to program for a stream. Two separate things every
 * consumer needs, and each of them used to open-code only the first:
 *
 *  - 0 means "unset" -> the historical 15/45 defaults (0a8bb9f), which is what
 *    the (v->min_qp>0)?v->min_qp:15 ternaries at each site were doing.
 *  - min_qp > max_qp is an inverted range and nothing rejected it. config.c
 *    clamps each field to 1..51 but cannot compare the two: field_set() works
 *    from a name+offset table one field at a time and has no way to see a
 *    sibling's value. So an inverted pair reached the SDK verbatim - on the
 *    classic attr structs no vendor header documents what minQp>maxQp does,
 *    and on the new API IMP_Encoder_SetChnQpBounds simply fails, leaving the
 *    encoder's built-in range and silently ignoring BOTH configured values.
 *    Swapping is the honest reading: two bounds were given in the wrong order,
 *    and the range meant is the ordered one.
 *
 * Deliberately fixed here, at the point of use, rather than in config.c:
 * swapping at write time would rewrite a field the client never touched, and
 * rejecting the write would break the perfectly ordinary /control sequence of
 * raising min_qp before max_qp (each write is a separate request). The stored
 * config keeps exactly what was written and reads back unchanged; only what is
 * programmed into the encoder is ordered. Warned once, not per apply - the
 * live rc re-apply path calls straight through here on every /control write. */
static void qp_bounds(const ms_vstream_cfg *v, int *qmin, int *qmax)
{
    int lo = (v->min_qp>0) ? v->min_qp : 15;
    int hi = (v->max_qp>0) ? v->max_qp : 45;
    if (lo > hi){
        static int warned_qpswap = 0;
        if (!warned_qpswap){
            LOGW(MOD,"videoN.min_qp (%d) > max_qp (%d) - programming %d..%d",
                 lo, hi, hi, lo);
            warned_qpswap = 1;
        }
        int t = lo; lo = hi; hi = t;
    }
    *qmin = lo; *qmax = hi;
}

#ifndef ENC_NEW_API
/* One classic-API rc-union fill, shared by enc_create() and the live
 * re-apply in ing_control()'s video branch (IMP_Encoder_SetChnAttrRcMode
 * takes exactly this struct, so a live change re-derives the WHOLE block
 * from g_cfg instead of patching single fields). The T23 sw-rotate path
 * keeps its own deliberately H264-only copy (sw_rot_start) - that unbound
 * Yuv encoder has no runtime rc API anyway.
 *
 * Mode mapping: the classic rc enum offers only FIXQP/CBR/VBR/SMART (no
 * CAPPED_* - see ENC_RC_MODE_* in the vendored T20/T21/T30 headers), so
 * capped_vbr/capped_quality fall back to VBR (closest rate-bounded mode)
 * with one warning instead of silently running as CBR. The VBR and Smart
 * union members are layout-identical within each codec (verified in the
 * T21/T30 headers), so one VBR-shaped fill serves both rcModes - same-shape
 * overlap of two members, NOT the cross-codec H264/H265 reinterpretation
 * that broke H265 CBR (fixed separately). The union member filled MUST
 * match the channel's codec: the H265 structs are NOT layout-compatible
 * with H264's. staticTime/frmQPStep/gopQPStep/adaptiveMode/gopRelation stay
 * at the historical literals - no SDK header documents a range for them. */
static void classic_rc_fill(IMPEncoderAttrRcMode *m, const ms_vstream_cfg *v)
{
    int qmin, qmax; qp_bounds(v, &qmin, &qmax);   /* unset defaults + ordered */
    if (v->rc_mode==MS_RC_FIXQP){
        m->rcMode = ENC_RC_MODE_FIXQP;
#if !(defined(PLATFORM_T10)||defined(PLATFORM_T20))
        if (v->codec==MS_VC_H265)
            m->attrH265FixQp.qp = (v->qp>0)?(uint32_t)v->qp:35;
        else
#endif
            m->attrH264FixQp.qp = (v->qp>0)?(uint32_t)v->qp:35;
    } else if (v->rc_mode==MS_RC_VBR || v->rc_mode==MS_RC_SMART ||
               v->rc_mode==MS_RC_CAPPED_VBR || v->rc_mode==MS_RC_CAPPED_QUALITY){
        int use_smart = (v->rc_mode==MS_RC_SMART);
        if (v->rc_mode==MS_RC_CAPPED_VBR || v->rc_mode==MS_RC_CAPPED_QUALITY){
            static int warned_capped = 0;
            if (!warned_capped){
                LOGW(MOD,"rc_mode capped_vbr/capped_quality has no classic-SoC equivalent -> using vbr");
                warned_capped = 1;
            }
        }
        m->rcMode = use_smart ? ENC_RC_MODE_SMART : ENC_RC_MODE_VBR;
#if !(defined(PLATFORM_T10)||defined(PLATFORM_T20))
        if (v->codec==MS_VC_H265){
            m->attrH265Vbr.maxQp       = (uint32_t)qmax;
            m->attrH265Vbr.minQp       = (uint32_t)qmin;
            m->attrH265Vbr.staticTime  = 2;   /* rate-stat window, seconds */
            m->attrH265Vbr.maxBitRate  = (uint32_t)v->bitrate_kbps;
            m->attrH265Vbr.iBiasLvl    = v->i_bias_lvl;
            m->attrH265Vbr.changePos   = (uint32_t)v->change_pos;
            m->attrH265Vbr.qualityLvl  = (uint32_t)v->quality_lvl;
            m->attrH265Vbr.frmQPStep   = 3;
            m->attrH265Vbr.gopQPStep   = 15;
            m->attrH265Vbr.flucLvl     = (uint32_t)v->fluc_lvl;
        } else
#endif
        {
            m->attrH264Vbr.maxQp       = (uint32_t)qmax;
            m->attrH264Vbr.minQp       = (uint32_t)qmin;
            m->attrH264Vbr.staticTime  = 2;   /* rate-stat window, seconds */
            m->attrH264Vbr.maxBitRate  = (uint32_t)v->bitrate_kbps;
            m->attrH264Vbr.iBiasLvl    = v->i_bias_lvl;
            m->attrH264Vbr.changePos   = (uint32_t)v->change_pos;
            m->attrH264Vbr.qualityLvl  = (uint32_t)v->quality_lvl;
            m->attrH264Vbr.frmQPStep   = 3;
            m->attrH264Vbr.gopQPStep   = 15;
            m->attrH264Vbr.gopRelation = 0;
        }
    } else {
        m->rcMode = ENC_RC_MODE_CBR;
#if !(defined(PLATFORM_T10)||defined(PLATFORM_T20))
        if (v->codec==MS_VC_H265){
            m->attrH265Cbr.maxQp      = (uint32_t)qmax;
            m->attrH265Cbr.minQp      = (uint32_t)qmin;
            m->attrH265Cbr.staticTime = 2;   /* rate-stat window, seconds */
            m->attrH265Cbr.outBitRate = (uint32_t)v->bitrate_kbps;
            m->attrH265Cbr.iBiasLvl   = v->i_bias_lvl;
            m->attrH265Cbr.frmQPStep  = 3;
            m->attrH265Cbr.gopQPStep  = 15;
            m->attrH265Cbr.flucLvl    = (uint32_t)v->fluc_lvl;
        } else
#endif
        {
            m->attrH264Cbr.maxQp        = (uint32_t)qmax;
            m->attrH264Cbr.minQp        = (uint32_t)qmin;
            m->attrH264Cbr.outBitRate   = (uint32_t)v->bitrate_kbps;
            m->attrH264Cbr.iBiasLvl     = v->i_bias_lvl;
            m->attrH264Cbr.frmQPStep    = 3;
            m->attrH264Cbr.gopQPStep    = 15;
            m->attrH264Cbr.adaptiveMode = 0;
            m->attrH264Cbr.gopRelation  = 0;
        }
    }
}
#endif /* !ENC_NEW_API */

static int enc_create(int chn, int grp, const ms_vstream_cfg *v)
{
    IMPEncoderChnAttr a; memset(&a,0,sizeof a);
    int ew, eh; ms_vstream_eff_dims(v,&ew,&eh);   /* post-rotation picture dims */
#ifdef ENC_NEW_API
    IMPEncoderProfile prof = (v->codec==MS_VC_H265)
        ? IMP_ENC_PROFILE_HEVC_MAIN
        : (v->profile>=2?IMP_ENC_PROFILE_AVC_HIGH:
           v->profile==1?IMP_ENC_PROFILE_AVC_MAIN:IMP_ENC_PROFILE_AVC_BASELINE);
    IMPEncoderRcMode rc;
    switch (v->rc_mode){
        case MS_RC_VBR:            rc=IMP_ENC_RC_MODE_VBR; break;
        case MS_RC_FIXQP:          rc=IMP_ENC_RC_MODE_FIXQP; break;
        case MS_RC_CAPPED_VBR:     rc=IMP_ENC_RC_MODE_CAPPED_VBR; break;
        case MS_RC_SMART: {
            /* same courtesy the classic path pays for its capped_* -> vbr
             * substitution (warned_capped): say the mode is being replaced
             * instead of silently running something else */
            static int warned_smart = 0;
            if (!warned_smart){
                LOGW(MOD,"rc_mode smart has no new-API equivalent -> using capped_quality");
                warned_smart = 1;
            }
        } /* fall through */
        case MS_RC_CAPPED_QUALITY: rc=IMP_ENC_RC_MODE_CAPPED_QUALITY; break;
        default:                   rc=IMP_ENC_RC_MODE_CBR; break;
    }
    /* iInitialQP: -1 ("auto") is fine for the rate-controlled modes, but
     * FIXQP has no rate control to fall back on - the SDK needs a real QP
     * here. Passing -1 for FIXQP crashed the closed-source encoder (reported
     * against v1.7.8: selecting rc_mode=fixqp brought the whole streamer
     * down). v->qp was already exposed via /control (F_CTRL, range 1..51)
     * but had no HAL consumer - wire it up for exactly this case. */
    int initial_qp = (v->rc_mode==MS_RC_FIXQP) ? (v->qp>0?v->qp:35) : -1;
    /* uMaxSameSenceCnt is NOT a "same scene" hint here - every new-API header
     * (T31/C100/T40/T41 imp_encoder.h, @param uMaxSameSenceCnt) spells out
     * "GOPLength = uGopLength * uMaxSameSenceCnt, Default is 2". Passing the
     * documented default 2 alongside uGopLength=v->gop therefore ran every
     * ENC_NEW_API SoC at DOUBLE the configured keyframe interval: videoN.gop
     * was accepted, clamped, persisted and echoed by /control, and the encoder
     * quietly used 2x it - exactly the 340fb1f/ff28ee2 shape. Measured on a
     * T31 (cinnado_d1_t31l_sc2336, video1.gop=50 @ 25fps): IDRs landed 4.000 s
     * apart (100 frames) instead of 2.000 s, on both chn0 and chn1 (this path
     * is shared by every stream). Pass 1 so the product is v->gop, and re-assert
     * both gopAttr fields afterwards so the effective GOP does not depend on how
     * a particular libimp build folds the argument into the struct. */
    /* M4: the AU assembly buffer is capped at MS_AU_BUF_MAX (1 MB) while
     * config.c lets videoN.bitrate reach 50000 kbps. An IDR runs roughly 8x an
     * average frame, i.e. about bitrate*1000/fps bytes; past ~0.8 MB every
     * keyframe is dropped at :1560, and the downstream healing path
     * (fanqueue_take_dropped_key -> hub_request_idr) then asks for another one
     * that is just as large - a livelock in which P-frames flow and no client
     * ever gets a keyframe. Dropping the config clamp would forbid legitimate
     * high-bitrate use, and growing the buffer costs RAM on every channel, so
     * neither is decided here: SAY IT at bring-up, where the number that caused
     * it is still in hand, instead of leaving a rate-limited drop message to be
     * decoded later. */
    if (v->fps > 0) {
        unsigned long idr_est = (unsigned long)v->bitrate_kbps * 1000UL
                              / (unsigned long)v->fps;
        if (idr_est > (unsigned long)(MS_AU_BUF_MAX / 10 * 8))
            LOGW(MOD,"encoder chn%d: %d kbps at %d fps implies ~%lu KB "
                     "keyframes, near or above the %d KB AU buffer - keyframes "
                     "will be dropped and clients may never get a decodable "
                     "stream; lower videoN.bitrate or raise MS_AU_BUF_MAX",
                 v->imp_chn, v->bitrate_kbps, v->fps, idr_est/1024,
                 MS_AU_BUF_MAX/1024);
    }
    if (IMP_Encoder_SetDefaultParam(&a, prof, rc,
            ew, eh, v->fps, 1, v->gop, 1, initial_qp, v->bitrate_kbps) != 0)
        LOGW(MOD,"IMP_Encoder_SetDefaultParam(chn%d) failed - the attr struct "
                 "below is only partly filled; CreateChn will likely reject it",
             v->imp_chn);
    a.gopAttr.uGopLength       = (uint16_t)v->gop;   /* config.c clamps 1..1000 */
    a.gopAttr.uMaxSameSenceCnt = 1;
    /* Non-default classic rc knobs on the new API: say once what happens to
     * each, instead of accepting them and doing nothing - that is exactly how
     * min_qp/max_qp silently missed this path until 0a8bb9f. Two different
     * truths, worded apart: quality_lvl/change_pos/fluc_lvl have NO equivalent
     * field in the new-API rc structs on any SoC; i_bias_lvl HAS a runtime
     * call (SetChnQpIPDelta, wired after RegisterChn below) but only the
     * T31/C100 SDKs ship it - on T40/T41 it is unsupported, not merely
     * unwired. */
    {
        static int warned_rcknobs = 0;
        if (!warned_rcknobs &&
            (v->quality_lvl!=2 || v->change_pos!=80 || v->fluc_lvl!=0)){
            LOGW(MOD,"videoN.quality_lvl/change_pos/fluc_lvl: no equivalent "
                     "field in this SoC's encoder API - values ignored");
            warned_rcknobs = 1;
        }
    }
#ifndef ENC_HAS_QPIPDELTA
    {
        static int warned_ipdelta = 0;
        if (!warned_ipdelta && v->i_bias_lvl!=0){
            LOGW(MOD,"videoN.i_bias_lvl: this SoC's SDK has no "
                     "IMP_Encoder_SetChnQpIPDelta - value ignored");
            warned_ipdelta = 1;
        }
    }
#endif
#else
    /* older platforms: manual attribute setup (H264 only path shown) */
#if defined(PLATFORM_T10)||defined(PLATFORM_T20)
    a.encAttr.enType   = PT_H264;               /* no H.265 on T10/T20 */
#else
    a.encAttr.enType   = (v->codec==MS_VC_H265)?PT_H265:PT_H264;
#endif
    a.encAttr.bufSize  = 0;
    a.encAttr.profile  = v->profile;
    a.encAttr.picWidth = ew;
    a.encAttr.picHeight= eh;
    a.rcAttr.outFrmRate.frmRateNum = v->fps;
    a.rcAttr.outFrmRate.frmRateDen = 1;
    a.rcAttr.maxGop = v->gop;
    /* rc mode MUST be filled: an all-zero attrRcMode means FIXQP with qp=0
     * (broken stream). rc_mode=fixqp used to silently no-op here (hardcoded
     * CBR regardless of v->rc_mode) instead of crashing like the ENC_NEW_API
     * path did - same underlying gap (videoN.qp never reached the HAL), just
     * a quieter failure mode on these SoCs. The fill itself lives in
     * classic_rc_fill() above, shared with the live rc re-apply. */
    classic_rc_fill(&a.rcAttr.attrRcMode, v);
#endif
    if (IMP_Encoder_CreateChn(chn,&a)<0){ LOGE(MOD,"Encoder_CreateChn %d",chn); return -1; }
    if (IMP_Encoder_RegisterChn(grp, chn)!=0){
        /* an unregistered channel never emits a stream (no SPS, no video) -
         * fail loudly instead of leaving a dead-but-running pipeline */
        LOGE(MOD,"Encoder_RegisterChn %d to group %d failed",chn,grp);
        IMP_Encoder_DestroyChn(chn);
        return -1;
    }
#ifdef ENC_NEW_API
    /* min_qp/max_qp have no consumer on this path: IMP_Encoder_SetDefaultParam()
     * takes no QP bounds, so videoN.min_qp/max_qp (exposed via /control, F_CTRL,
     * 1..51) silently did nothing on T31/C100/T40/T41 - only the classic and
     * T23 sw-rotate paths ever applied them. Apply them explicitly now via
     * IMP_Encoder_SetChnQpBounds(chn,min,max) (signature identical across all
     * four new-API headers; the SDK doc requires the channel to already exist,
     * hence after RegisterChn). Same "0 = unset -> existing default" pattern as
     * the qp fix, reusing the 15/45 defaults the classic CBR fill uses so the
     * effective bounds match across SoCs. Non-fatal: a rejected call just leaves
     * the encoder's built-in QP range, so warn and carry on. */
    {
        int qmin, qmax; qp_bounds(v, &qmin, &qmax);
        if (IMP_Encoder_SetChnQpBounds(chn, qmin, qmax)!=0)
            LOGW(MOD,"Encoder_SetChnQpBounds %d (%d..%d) failed - using SDK default range",
                 chn, qmin, qmax);
    }
#ifdef ENC_HAS_QPIPDELTA
    /* i_bias_lvl via IMP_Encoder_SetChnQpIPDelta (T31/C100 SDKs only), the
     * same after-RegisterChn pattern as the QP bounds above and non-fatal on
     * rejection. Only for a non-default value: 0 leaves the SDK's own
     * iIPDelta untouched (readable as encoder.<n>.rc.ip_delta). The classic
     * iBiasLvl and the new iIPDelta are close relatives, not proven identical
     * in sign/scale - passed through 1:1; verify against the rc readback on
     * hardware before trusting the mapping. */
    if (v->i_bias_lvl != 0 &&
        IMP_Encoder_SetChnQpIPDelta(chn, v->i_bias_lvl) != 0)
        LOGW(MOD,"Encoder_SetChnQpIPDelta %d (%d) failed - using SDK default",
             chn, v->i_bias_lvl);
#endif
#endif
    return 0;
}

/* concatenated pack -> keyframe test.
 *
 * Decided by the FIRST VCL NAL only: an access unit carries exactly one coded
 * picture, so all of its slices agree on IDR-/IRAP-ness, and the parameter
 * set / SEI / AUD NALs that may precede them are a few dozen bytes. So this
 * walks start codes, reads one NAL header byte at each, and stops at the first
 * slice - it does not need each NAL's END, which is what made the previous
 * nal_iter version walk the whole AU for every one of the ~96% of frames that
 * are not keyframes, at full frame rate, on the producer thread, per stream.
 *
 * Not read from IMPEncoderPack.nalType/.dataType, which the SDK does fill in:
 * that field is per PACK, and a pack is a libimp bitstream section, not
 * necessarily one NAL (see enc_assemble_packs' start-code fixup below), so it
 * cannot answer "is any NAL in this AU an IDR" without the same per-platform
 * assumptions this function exists to avoid - and the sw-rotate path
 * (IMP_Encoder_YuvEncode) has no pack array at all. With the early exit the
 * byte path costs a few dozen bytes per AU, so there is nothing left to win. */
static int au_is_key(int codec, const uint8_t *au, size_t len)
{
    for (size_t i=0; i+4<=len; ){
        size_t h;
        if (au[i]!=0 || au[i+1]!=0) { i++; continue; }
        if (au[i+2]==1)                     h = i+3;
        else if (au[i+2]==0 && au[i+3]==1)  h = i+4;
        else { i++; continue; }
        if (h>=len) break;
        if (codec==MS_VC_H264){
            int t=h264_nal_type(au+h);
            if (t==5) return 1;
            if (t>=1 && t<=5) return 0;      /* VCL of a non-IDR picture */
        } else {
            int t=h265_nal_type(au+h);
            if (t>=16 && t<=23) return 1;
            if (t<=31) return 0;             /* VCL of a non-IRAP picture */
        }
        i = h+1;
    }
    return 0;
}

/* Assemble the packs of one encoder stream into dst (capacity cap); returns the
 * assembled length and SETS (never clears) *overflow if a pack or its start
 * code did not fit, in which case the caller must drop the frame rather than
 * publish a truncated, syntactically-corrupt AU.
 *
 * want_start_codes is the only thing that ever differed between the video and
 * the JPEG path: H.264/H.265 access units must come out Annex-B, JPEG must not
 * gain four bytes it never asked for. Everything else - the address-convention
 * #ifdef and the ring-buffer reasoning below - was duplicated verbatim in both
 * loops, i.e. two places to keep right for one piece of libimp behaviour.
 *
 * L6 (Low, hardening): the vendor samples (and prudynt-t) treat the new-API
 * stream buffer as a RING and split a pack that wraps past streamSize into two
 * memcpys. We copy contiguously from virAddr+offset and do not - which is
 * PROVEN correct for the bundled T31 1.1.6 libimp rather than assumed:
 * update_one_frmstrm (@0x829d8 in 3rdparty/install/lib/libimp.so) compacts the
 * AL5 sections, pulls the header sections in front of the slice data, points
 * virAddr at the start of that compact block (@0x82cc4) and then REWRITES the
 * pack offsets as a running sum of the lengths (@0x82cd8-0x82cf8). A pack with
 * offset+length > streamSize cannot come out of that.
 *
 * For C100/T40/T41 it stays unverified - T41 1.2.5 exports a function of the
 * same name, which is not proof - and those libimps are not in this tree to
 * check. The cheap remSize idiom would decouple the assumption from the libimp
 * version; left undone because it would be an untested branch in the hot
 * per-packet path, which is its own risk. If a wrap ever does occur the symptom
 * is a corrupt AU, not a crash - look here first. */
static size_t enc_assemble_packs(const IMPEncoderStream *st, uint8_t *dst,
                                 size_t cap, int want_start_codes, int *overflow)
{
    size_t len = 0;
    for (uint32_t i=0;i<st->packCount;i++){
#ifdef ENC_NEW_API
        const uint8_t *p=(const uint8_t*)(uintptr_t)st->virAddr + st->pack[i].offset;
#else
        const uint8_t *p=(const uint8_t*)(uintptr_t)st->pack[i].virAddr;
#endif
        size_t l=st->pack[i].length;
        if (l==0) continue;
        if (want_start_codes){
            /* guarantee Annex-B: prepend a start code if the pack lacks one */
            int has_sc = (l>=3 && p[0]==0 && p[1]==0 &&
                          (p[2]==1 || (l>=4 && p[2]==0 && p[3]==1)));
            if (!has_sc){
                if (len+4<=cap){ dst[len++]=0; dst[len++]=0; dst[len++]=0; dst[len++]=1; }
                else *overflow=1;
            }
        }
        if (len+l<=cap){ memcpy(dst+len,p,l); len+=l; }
        else *overflow=1;
    }
    return len;
}

/* Fix 1 / A1: capture-accurate, strictly-monotonic publish timestamp, shared by
 * every video channel and the audio thread (each with its own pts_sanitizer).
 *
 * The RTP/fMP4 media clock used to be ms_now_us() sampled at hub-publish time.
 * That carries publish-thread scheduling jitter and, after any encoder-poll /
 * AI-FIFO-drain hiccup, bursts several frames with near-identical stamps as the
 * backlog drains - the non-monotonic / "No video PTS, making something up"
 * behaviour mpv reported over RTSP on cinnado_d1_t31l. rtp.c's audio RTP layer
 * is sample-count-driven and immune to this, but the gap-repair heuristics that
 * consume the publish pts (audio_gap_resync() in rtp.c, fmp4.c's M2 audio
 * re-anchor) still reacted to the jittery stamp: a publish-thread stall that
 * lost no samples (the AI FIFO buffered and burst them) looked like a gap and
 * inserted phantom samples -> permanent audio drift (A1).
 *
 * The encoder/AI hands us the true per-frame capture time
 * (IMPEncoderPack.timestamp / IMPFrameInfo.timeStamp / IMPAudioFrame.timeStamp,
 * unit us); its inter-frame deltas are the correct, jitter-free media clock, and
 * those deltas ARE exactly the signal the gap heuristics need. We do NOT trust
 * it blindly:
 *
 *  - Absolute base: libimp's timestamp base is not guaranteed to equal
 *    CLOCK_MONOTONIC (ms_now_us). rtp.c's RTCP Sender Report pairs ms_now_us()
 *    ("now") against the track's first pts (pts0); a pts0 on a foreign base
 *    would corrupt the NTP<->RTP mapping and A/V lip-sync. So we keep the media
 *    timeline ON the ms_now_us base: the first frame anchors at now_us and every
 *    later frame adds the hardware DELTA via a fixed offset (now0 - hw0). Any
 *    constant base difference cancels out; only the jitter-free deltas survive.
 *    Because the base is unchanged, no downstream consumer (RTP SR, fMP4 tfdt,
 *    recorder, SRT PES) sees a timebase shift - only the jitter is removed.
 *
 *  - Reliability: on some SoC/libimp builds the field can be 0, frozen, or
 *    garbage. We accept the hardware-derived value only while it is strictly
 *    monotonic AND within +/-skew_max_us of the wall clock. A genuine stall
 *    advances BOTH clocks, so a real multi-second gap is preserved and stays
 *    aligned with audio_gap_resync(); a frozen/garbage value diverges from the
 *    wall clock and is rejected. (This wall-clock cross-check replaces a raw
 *    ">N s jump" rule on purpose - a delta threshold cannot tell a real long
 *    stall, which we must preserve to stay A/V-aligned, from garbage, which we
 *    must reject.) On rejection we snap the pts to the wall clock (now_us, hard
 *    ratchet-stop so a fast burst of rejected frames can never accumulate a
 *    forward lead) and re-seat the offset so a later good frame re-locks the hw
 *    clock.
 *
 *  - Self-correction: a diverged-but-still-under-skew_max hw clock used to stay
 *    locked in forever (the accept path froze pts_offset). Since video and audio
 *    use different skew_max, one track could hold a drift the other rejected ->
 *    a permanent A/V skew. The accept path now SLEWS pts_offset back toward the
 *    zero-skew value at a tiny capped per-frame rate whenever the drift is
 *    non-trivial (see PTS_SLEW_* below), so no track can stay locked to a
 *    drifted clock. Convergence is gradual (invisible per frame), not a step.
 *
 * skew_max_us bounds the legitimate capture-vs-publish lag: video uses ~1 s
 * (encoder backlog can be large); audio uses a tighter bound sized to the AI
 * FIFO depth (MS_AI_FRM_DEPTH frames ~160 ms) plus the AAC accumulator, since
 * audio's hardware FIFO caps how far a legitimate burst can lag and a looser
 * bound would only let more garbage through.
 *
 * Always returns a strictly-increasing pts on the ms_now_us() timebase. */
/* Self-correction slew (accept path), shared by both streams:
 *  - DEADBAND: only correct a genuine hw-vs-wall DRIFT, not per-frame jitter.
 *    Steady-state skew is jitter-scale (a few ms - the offset is anchored to the
 *    capture cadence, not the absolute latency), so 50 ms sits well above it:
 *    the offset stays frozen in normal operation and cand keeps the smooth,
 *    jitter-free capture clock. Only a real drift event pushes skew past this.
 *  - MAX: per-frame offset step cap. 1 ms << any real frame interval (>=~8 ms
 *    at 120 fps), so it is invisible per frame and always leaves a large
 *    monotonicity margin. Convergence rate = MAX * fps (e.g. 25 ms/s at 25 fps,
 *    ~2.5% playback deviation during recovery); a ~0.9 s drift decays to the
 *    deadband in ~34 s. Tunable: raise MAX for faster recovery, lower for a
 *    gentler slew. */
#define PTS_SLEW_DEADBAND_US 50000LL
#define PTS_SLEW_MAX_US       1000LL
static int64_t pts_sanitize(pts_sanitizer *s, int64_t hw_us, int64_t now_us,
                            int64_t frame_us, int64_t skew_max_us)
{
    if (frame_us < 1) frame_us = 1;

    if (!s->have_pub_pts) {                  /* first frame: anchor to the wall clock */
        if (hw_us > 0) { s->pts_offset = now_us - hw_us; s->pts_have_off = 1; }
        else           { s->pts_offset = 0;              s->pts_have_off = 0; }
        s->have_pub_pts = 1;
        s->last_pub_pts = now_us;
        return now_us;
    }

    if (hw_us > 0) {
        if (!s->pts_have_off) { s->pts_offset = now_us - hw_us; s->pts_have_off = 1; }
        int64_t cand = hw_us + s->pts_offset;
        int64_t skew = cand - now_us; if (skew < 0) skew = -skew;
        if (cand > s->last_pub_pts && skew <= skew_max_us) {  /* monotonic & clock-consistent */
            /* Self-correction (NTP-style slew): the accept path used to freeze
             * pts_offset forever, so if the hw clock ever diverged from the wall
             * clock by an amount below skew_max the stream stayed locked at that
             * offset permanently - and because video (1 s) and audio (500 ms)
             * use different skew_max, one track could lock to a drift the other
             * rejected, giving a non-self-correcting A/V skew (the ~0.9 s cam-A
             * incident). Fix: whenever the accepted skew is non-trivial (> the
             * deadband; normal skew is jitter-scale and stays frozen so cand
             * remains the smooth, jitter-free capture clock), nudge pts_offset a
             * small capped step toward the zero-skew value (now_us - hw_us), i.e.
             * by err = now_us - cand = (now_us - hw_us) - pts_offset. The step is
             * far below one hw inter-frame delta, so it is invisible per frame and
             * a large drift converges back to wall over time instead of forever.
             *
             * MONOTONICITY: the value RETURNED is `cand`, computed with the
             * PRE-nudge offset. The nudge only changes FUTURE cand values;
             * next frame cand' = hw' + (offset +/- step), and cand' - cand =
             * (hw' - hw) +/- step. Since |step| <= PTS_SLEW_MAX_US << the hw
             * inter-frame delta, cand' still exceeds cand, and the `cand >
             * last_pub_pts` test above re-verifies it regardless (a pathological
             * tiny hw delta just falls to the monotonic fallback). Symmetric for
             * a hw clock running ahead (err < 0 -> negative step). */
            if (skew > PTS_SLEW_DEADBAND_US) {
                int64_t step = now_us - cand;                 /* toward zero skew */
                if (step >  PTS_SLEW_MAX_US) step =  PTS_SLEW_MAX_US;
                if (step < -PTS_SLEW_MAX_US) step = -PTS_SLEW_MAX_US;
                s->pts_offset += step;
            }
            s->last_pub_pts = cand;
            return cand;
        }
        /* hw unreliable here (backwards, frozen, or diverged from the wall
         * clock by more than skew_max): re-seat the offset so a subsequent good
         * frame re-locks the hw clock */
        s->pts_offset = now_us - hw_us;
    }

    /* Fallback: track real time, and NEVER run ahead of it. Snapping to now_us
     * (not last+frame_us) makes this a hard ratchet-stop - last_pub_pts can
     * never climb past now_us, so a fast burst of stale/rejected frames (e.g. a
     * capture backlog drained faster than real time) cannot accumulate a growing
     * forward lead. (frame_us is unused here now, but kept in the signature for
     * the accept-path clamp and documentation.) Strictly monotonic: if now_us
     * has not yet passed last_pub_pts, advance 1us and let real time catch up,
     * bounded by skew_max. This matches the pre-A1 ms_now_us() publish behaviour
     * in the degraded (no usable hw timestamp) case. */
    (void)frame_us;
    int64_t pts = now_us;
    if (pts <= s->last_pub_pts) pts = s->last_pub_pts + 1;   /* strict monotonic guard */
    s->last_pub_pts = pts;
    return pts;
}

/* video skew tolerance (encoder backlog can be ~1 s) */
#define PTS_SKEW_VIDEO_US 1000000LL
/* audio skew tolerance: the AI FIFO (MS_AI_FRM_DEPTH * ~40 ms) plus the AAC
 * accumulator (<64 ms) cap a legitimate burst's lag at ~250 ms; 500 ms gives
 * 2x margin while still rejecting garbage far tighter than video's 1 s. */
#define PTS_SKEW_AUDIO_US 500000LL

static void *video_thread(void *arg)
{
    vchan *vc = (vchan*)arg;
    /* P-01: the AU is assembled DIRECTLY into a per-frame packet borrowed from
     * this source's recycling pool (hub_pkt_get) and handed to the hub with
     * hub_publish_take() - no persistent au[] scratch and no second full-frame
     * copy at publish. The pool sizes/reuses buffers to the observed frame peak
     * (bounded by MS_AU_BUF_MAX) exactly as the old on-demand-grown au[] did. */
    int receiving=0;
    int64_t idle_since=0;
    int dbg_first=0, dbg_pollfail=0;   /* one-shot encoder diagnostics */
    int dbg_startfail=0;               /* rate-limits StartRecvPic failures */
    int dbg_recover_fails=0;           /* consecutive forced-recovery cycles
                                         * that never yielded a real frame -
                                         * see MS_VIDEO_WATCHDOG_MAX_RECOVERIES */
    int dbg_ovf=0;                     /* rate-limits the AU-overflow warning */
#if defined(PLATFORM_T31)
    /* Item-2 (T31 only): IMP_Encoder_GetChnAveBitrate averages over a frame
     * count that must be an integral multiple of the GOP length. Resolve this
     * channel's configured GOP once (vc->chn == video[].imp_chn). */
    int ave_gop = 1;
    for (int i=0;i<MS_MAX_VSTREAM;i++)
        if (g_hcfg->video[i].imp_chn == vc->chn){
            if (g_hcfg->video[i].gop > 0) ave_gop = g_hcfg->video[i].gop;
            break;
        }
#endif
    while (vc->run) {
        /* on-demand: encode while there are consumers. The hub subscriber
         * count is the truth source (level, not edge), so a racing stale
         * "inactive" flag can never stop a stream that still has clients. */
        int want = vc->active || hub_active(vc->chn);
        if (!want) {
            /* fully idle: block until ing_set_active() wakes us (1 s safety
             * timeout). No polling, no frame flow - the framesource is off. */
            if (!receiving){ act_wait(act_ready_vchan, vc); continue; }
            /* debounce the stop: only shut the encoder down after a sustained
             * idle period; rapid client churn must not toggle Start/StopRecvPic */
            int64_t now = ms_now_us();
            if (idle_since==0) idle_since = now;
            if (now - idle_since >= MS_IDLE_STOP_US) {
                IMP_Encoder_StopRecvPic(vc->chn);
                fs_unuse(vc->chn);            /* stop the frame flow entirely */
                receiving=0; idle_since=0;
                LOGI(MOD,"video chn%d idle",vc->chn);
                continue;
            }
            /* during the debounce window keep draining the encoder below so
             * the pipeline never backs up (publish is a no-op with 0 subs) */
        } else {
            idle_since = 0;
            if (!receiving){
                fs_use(vc->chn);
                /* an unchecked StartRecvPic failure used to flip receiving=1
                 * anyway: the pipeline looked "streaming" but delivered
                 * nothing (H6). Back off and retry while consumers remain. */
                if (IMP_Encoder_StartRecvPic(vc->chn)!=0){
                    if ((dbg_startfail++ % 20)==0)
                        LOGE(MOD,"chn%d: StartRecvPic failed (attempt %d)",
                             vc->chn, dbg_startfail);
                    fs_unuse(vc->chn);
                    usleep(200000);
                    continue;
                }
                dbg_startfail=0; receiving=1;
                vc->idr_req=0; IMP_Encoder_RequestIDR(vc->chn);
                LOGI(MOD,"video chn%d streaming",vc->chn);
            }
        }
        /* honor IDR requests from RTSP/HTTP threads here (single-thread IMP) */
        if (vc->idr_req){ vc->idr_req=0; IMP_Encoder_RequestIDR(vc->chn); }
        int pr = IMP_Encoder_PollingStream(vc->chn, g_hcfg->imp_polling_timeout);
        if (pr!=0){
            if (receiving){
                dbg_pollfail++;
                if ((dbg_pollfail % 20)==1)
                    LOGW(MOD,"chn%d: PollingStream idle (rc=%d, miss#%d) - encoder emits no frames",
                         vc->chn, pr, dbg_pollfail);
                if (dbg_pollfail >= MS_VIDEO_WATCHDOG_ITERS){
                    dbg_recover_fails++;
                    if (dbg_recover_fails >= MS_VIDEO_WATCHDOG_MAX_RECOVERIES){
                        /* N consecutive recovery cycles all reported success
                         * (StartRecvPic==0) yet none ever produced a frame -
                         * the hardware is genuinely gone, not just wedged.
                         * Exit rather than spin forever: raise SIGTERM
                         * instead of exit()ing this thread directly so the
                         * existing on_signal path in main.c runs the normal
                         * orderly shutdown (other channels, RTSP/HTTP
                         * servers, record/timelapse/srt) for every other
                         * still-live subsystem, backstopped by its own
                         * existing 3 s hard_exit alarm if any of that wedges
                         * against the same dead ISP. S95timps does NOT
                         * auto-respawn (plain SysV start/stop) - this exit
                         * leaves the camera down until a human or an external
                         * scheduler restarts it, which is still strictly
                         * better than the prior silent, unbounded hang. */
                        LOGE(MOD,"chn%d: %d consecutive forced-recovery cycles never "
                             "produced a frame - encoder/ISP is not coming back on its "
                             "own; exiting (camera needs a manual/scheduled restart)",
                             vc->chn, dbg_recover_fails);
                        raise(SIGTERM);
                        break;
                    }
                    LOGE(MOD,"chn%d: encoder dead after %d consecutive misses - "
                         "forcing a framesource disable/enable cycle to recover "
                         "(recovery attempt %d/%d)",
                         vc->chn, dbg_pollfail, dbg_recover_fails, MS_VIDEO_WATCHDOG_MAX_RECOVERIES);
                    IMP_Encoder_StopRecvPic(vc->chn);
                    /* V2 (partial): when this is the SOLE holder of the FS
                     * channel, this fs_unuse()+fs_use() pair now genuinely
                     * retries EnableChn (see g_fs_enabled - F-fs), where it
                     * used to be a permanent no-op after any failed 0->1
                     * enable. It is STILL a no-op when another holder (e.g.
                     * motion detection pinning this same channel) keeps the
                     * refcount above 0 across the cycle: fs_unuse() only
                     * calls DisableChn (and clears g_fs_enabled) on the ->0
                     * edge, so under a co-holder neither Disable nor Enable
                     * actually run here and this degenerates to the
                     * Stop/StartRecvPic below only - identical to pre-fix
                     * behavior for that specific case (reviewed 2026-08-03;
                     * a true fix needs a refcount-independent force-recycle
                     * primitive, tracked as a follow-up, not implemented
                     * here to avoid disrupting the co-holder's own frame
                     * flow without hardware validation). */
                    fs_unuse(vc->chn);
                    fs_use(vc->chn);
                    if (IMP_Encoder_StartRecvPic(vc->chn)==0){
                        vc->idr_req=0; IMP_Encoder_RequestIDR(vc->chn);
                        dbg_first=0;             /* log the recovered first frame */
                    } else {
                        fs_unuse(vc->chn);        /* fully release; top-of-loop's
                                                    * !receiving path fs_use()s fresh */
                        receiving=0;
                    }
                    dbg_pollfail=0;
                }
            }
            continue;
        }
        IMPEncoderStream st;
        if (IMP_Encoder_GetStream(vc->chn,&st,1)!=0){
            LOGW(MOD,"chn%d: GetStream failed after PollingStream OK",vc->chn); continue; }
        dbg_pollfail=0;
        dbg_recover_fails=0;   /* a real frame arrived: the channel has genuinely
                                 * recovered, not just "successfully" restarted */
        /* Size the packet to the actual frame: sum the pack lengths (+4 for a
         * possible start code each) as a safe UPPER BOUND on the assembled
         * length, then borrow a pooled buffer of exactly that size. A frame
         * whose bound exceeds MS_AU_BUF_MAX is a genuinely >1 MB access unit -
         * drop it rather than publish a truncated, syntactically-corrupt AU.
         * Do NOT force an IDR on this drop: an IDR is the *largest* frame type,
         * so forcing one after a size overflow only guarantees the next frame
         * overflows too (the historical permanent-stall "only ch0 works" bug);
         * clients ride out a broken GOP until the next SCHEDULED IDR (<=
         * videoN.gop frames). Note fanqueue_take_dropped_key/hub_request_idr do
         * NOT fire here - they only see consumer-queue evictions, and a frame
         * dropped at the producer never enters any fanqueue. */
        size_t need=0;
        for (uint32_t i=0;i<st.packCount;i++)
            if (st.pack[i].length) need += (size_t)st.pack[i].length + 4; /* +startcode */
        if (need > MS_AU_BUF_MAX){
            __sync_fetch_and_add(&vc->au_drops, 1u);
            if ((dbg_ovf++ % 20)==0)
                LOGW(MOD,"chn%d: AU exceeds max buffer (need=%zu, max=%d, packCount=%u) - "
                     "dropping frame (%u dropped so far)",
                     vc->chn, need, MS_AU_BUF_MAX, st.packCount, vc->au_drops);
            IMP_Encoder_ReleaseStream(vc->chn,&st);
            continue;
        }
        /* P-01: assemble straight into a pooled packet - no au[] scratch, no
         * copy at publish. pkt_get returns cap >= need, so assembly cannot
         * overflow (the guard below is defensive only). */
        ms_pkt *pk = hub_pkt_get(vc->chn, need);
        if (!pk){
            __sync_fetch_and_add(&vc->au_drops, 1u);
            if ((dbg_ovf++ % 20)==0)
                LOGW(MOD,"chn%d: no memory for AU packet (need=%zu) - dropping frame",
                     vc->chn, need);
            IMP_Encoder_ReleaseStream(vc->chn,&st);
            continue;
        }
        uint8_t *au = pk->data;
        size_t   au_cap = pk->cap;
        int overflow=0;
        size_t aulen = enc_assemble_packs(&st, au, au_cap, 1, &overflow);
        /* Defensive: need is a strict upper bound on aulen so this is
         * unreachable; if a pool/SDK anomaly ever tripped it, drop the frame
         * (and return the pooled buffer) rather than publish a truncated AU. */
        if (overflow){
            __sync_fetch_and_add(&vc->au_drops, 1u);
            if ((dbg_ovf++ % 20)==0)
                LOGW(MOD,"chn%d: AU assembly overflow (cap=%zu, need=%zu, packCount=%u) - "
                     "dropping frame",
                     vc->chn, au_cap, need, st.packCount);
            pkt_unref(pk);
            IMP_Encoder_ReleaseStream(vc->chn,&st);
            continue;
        }
        pk->len = aulen;
        int key = au_is_key(vc->codec, au, aulen);
        if (!dbg_first){ dbg_first=1;
            LOGI(MOD,"chn%d: first encoded frame packCount=%u aulen=%zu key=%d",
                 vc->chn, st.packCount, aulen, key); }
        /* Fix 1: use the encoder's per-frame capture timestamp (jitter-free
         * media clock), sanitized/monotonized against the wall clock, instead
         * of ms_now_us() at publish time. All packs of one frame share the
         * capture time; pack[0] is representative. */
        int64_t hw_us = (st.packCount > 0) ? st.pack[0].timestamp : 0;
        int64_t pub_now = ms_now_us();
        int64_t pts = pts_sanitize(&vc->pts, hw_us, pub_now,
                                   1000000 / (vc->fps > 0 ? vc->fps : 25),
                                   PTS_SKEW_VIDEO_US);
        hub_publish_take(vc->chn, pk, pts, key, MS_MEDIA_VIDEO, pub_now);
#if defined(PLATFORM_T31)
        /* Item-2 (T31 only): cache the running average bitrate for the read-only
         * /control encoder-stats getter. Must run while 'st' is still held (the
         * SDK computes over the just-fetched stream) and BEFORE ReleaseStream;
         * calling GetStream from the control thread would steal packets from
         * this loop. Cache on success only, publish the value before the valid
         * flag so a concurrent lock-free reader never sees a stale/zero double
         * as valid. Failure (<0) just keeps the previous cached value. */
        {
            double br = 0;
            if (IMP_Encoder_GetChnAveBitrate(vc->chn, &st, ave_gop, &br) >= 0){
                vc->ave_bitrate = br;
                vc->ave_valid   = 1;
            }
        }
#endif
        IMP_Encoder_ReleaseStream(vc->chn,&st);
    }
    if (receiving){ IMP_Encoder_StopRecvPic(vc->chn); fs_unuse(vc->chn); }
    return NULL;
}

#ifdef ROT_HAS_SW_90
/* ================= software 90/270 rotate (Batch 5, T23 opt-in) =============
 * The T23 has no I2D block and its libimp has no FrameSource rotate, but the
 * shipped libimp 1.3.0 EXPORTS the unbound YUV encode API (IMP_Encoder_YuvInit/
 * YuvEncode/YuvExit/YuvRequestIDR + VbmAlloc/VbmFree/VbmV2P - verified against
 * dl/ingenic-lib T23 1.3.0 and include/T23/1.3.0/en/imp/imp_encoder.h). So a
 * 90/270 stream runs UNBOUND: framesource at the pre-rotation dims with a user
 * frame depth, sw_rot_thread GetFrame -> nv12_rotate90 into a phys-contiguous
 * bounce buffer -> software OSD -> IMP_Encoder_YuvEncode -> hub_publish.
 * Everything downstream (RTSP/fMP4/hub/record) already works from the eff
 * (rotated) dims + SPS-derived params, so nothing else changes.
 *
 * Costs/limitations of this path (all logged at start):
 *   - CPU: a full NV12 transpose per frame on the single MIPS core. Anything
 *     beyond ~704x576@15 (substream class) will eat the core - warned, not
 *     refused (the build knob is the operator's opt-in to that cost).
 *   - no HARDWARE OSD or privacy covers (no OSD group to splice in). Text OSD
 *     is composited in software below (5b); privacy covers are NOT drawn on
 *     this stream (future item).
 *   - no piggyback JPEG (videoN.jpeg) on this stream: the piggyback encoder
 *     registers into the video's encoder GROUP, which does not exist here.
 *   - IVS motion on this stream sees the UNROTATED frame (grid coords are
 *     pre-rotation).
 * OUTPUT-BUFFER CONTRACT (established by disassembling the T23 1.3.0
 * libimp.so, dl/ingenic-lib): IMPEncoderYuvOut is an IN/OUT parameter. The
 * CALLER must pass a valid 4-byte-aligned buffer in out.outAddr and its
 * capacity in out.outLen. Per frame, i264e_update_fenc (@0x3484c) sets the
 * raw bitstream area to outAddr+0x1080 / outLen-0x1080 (the Yuv path sets the
 * "external buffer" flag, so i264e_init allocates NO internal bitstream buf,
 * @0x3aa48), and i264e_encoder_encapsulate_nals (@0x38664, ext branch
 * @0x38840) escapes the final Annex-B AU contiguously to outAddr+0. On return
 * YuvEncode overwrites out.outLen with the AU length (sum of NAL sizes,
 * @0x58fb8) and leaves out.outAddr untouched. Passing a zeroed struct makes
 * libimp deref NULL+0x1080 -> SIGSEGV at load-offset 0x3d8dc (bs init in
 * i264e_encode, delay slot of the jalr). The AU in ybuf stays valid until the
 * next YuvEncode call (hub_publish copies it out immediately). Also per
 * disasm: the encode is pure SOFTWARE (i264e) and reads the input frame via
 * frame.virAddr only (frame.phyAddr is never read, @0x58f24), and the Yuv API
 * accepts PT_H264 ONLY - PT_H265 is a log-and-fail stub in both YuvInit
 * (@0x5874c) and YuvEncode (@0x5901c). */

/* ---- 5b: software OSD onto the rotated frame -------------------------------
 * Same position convention as imp_osd.c resolve_pos(), but applied against the
 * EFF (rotated) frame dims, so "top-right" is the portrait frame's top-right:
 * x/y > 0 from left/top, < 0 from right/bottom, 0 = centered. */
static void sw_resolve_pos(int W, int H, int w, int h, int x, int y,
                           int *ox, int *oy)
{
    *ox = (x>0) ? x : (x<0 ? W-w+x : (W-w)/2);
    *oy = (y>0) ? y : (y<0 ? H-h+y : (H-h)/2);
    if (*ox<0) *ox=0;
    if (*oy<0) *oy=0;
    if (*ox+w>W) *ox = (W-w>0) ? (W-w) : 0;
    if (*oy+h>H) *oy = (H-h>0) ? (H-h) : 0;
}

/* shared default TTF for the SW-OSD path (imp_osd.c's g_shared is static
 * there; loading our own copy keeps the modules decoupled). 0 = not tried
 * yet, 1 = loaded, -1 = failed (-> embedded bitmap font fallback). */
static msttf_font g_swrot_font;
static int        g_swrot_font_state = 0;

/* Alpha-blit a BGRA text bitmap onto the NV12 Y plane at (x0,y0), clipped to
 * the frame. Luma-only v1: the text color's BT.601 luma is blended by the
 * per-pixel alpha scaled with the item's group transparency; CbCr is left
 * untouched (the glyph tints toward the scene hue - acceptable, half the
 * work; full-chroma blit is a follow-up if wanted). */
static void sw_osd_blit_y(uint8_t *nv12, int fw, int fh, const uint8_t *bgra,
                          int w, int h, int x0, int y0, int galpha)
{
    if (galpha <= 0) return;
    if (galpha > 255) galpha = 255;
    for (int y=0; y<h; y++){
        int fy = y0 + y;
        if (fy < 0 || fy >= fh) continue;
        const uint8_t *sp = bgra + (size_t)y * (size_t)w * 4;
        uint8_t       *dp = nv12 + (size_t)fy * (size_t)fw;
        for (int x=0; x<w; x++){
            int fx = x0 + x;
            if (fx < 0 || fx >= fw) continue;
            const uint8_t *p = sp + (size_t)x * 4;  /* B,G,R,A (LE 0xAARRGGBB) */
            unsigned a = (unsigned)p[3] * (unsigned)galpha / 255u;
            if (!a) continue;
            unsigned ty = ((66u*p[2] + 129u*p[1] + 25u*p[0] + 128u) >> 8) + 16u;
            dp[fx] = (uint8_t)((a*ty + (255u-a)*dp[fx] + 127u) / 255u);
        }
    }
}

/* record which items this stream composites and load their fonts. Mirrors
 * imp_osd_setup's item scan: only items enabled at startup get a slot (same
 * "enable live -> applies on restart" limitation as the HW OSD path). */
static void sw_osd_init(vchan *vc, const ms_config *cfg, int si)
{
    memset(&vc->osd, 0, sizeof vc->osd);
    if (!cfg->osd.enabled) return;
    if (g_swrot_font_state == 0)
        g_swrot_font_state = (cfg->osd.font_path[0] &&
                              msttf_load(&g_swrot_font, cfg->osd.font_path)==0)
                             ? 1 : -1;
    for (int i=0; i<MS_MAX_OSD; i++){
        const ms_osd_item *it = &cfg->osd.items[si][i];
        if (!it->enabled) continue;
        if (it->type != MS_OSD_TEXT){
            LOGW(MOD,"sw-rot stream %d: OSD item %d is a logo - not composited "
                     "on the SW-rotate path (text only)", si, i);
            continue;
        }
        sw_osd_slot *sl = &vc->osd.it[i];
        sl->used = 1;
        if (it->font_path[0]){
            msttf_font *pf = (msttf_font*)malloc(sizeof *pf);
            if (pf && msttf_load(pf, it->font_path)==0){
                sl->font = pf; sl->font_owned = 1;
            } else {
                free(pf);
                sl->font = (g_swrot_font_state==1) ? &g_swrot_font : NULL;
            }
        } else
            sl->font = (g_swrot_font_state==1) ? &g_swrot_font : NULL;
        vc->osd.active = 1;
    }
    if (vc->osd.active)
        LOGI(MOD,"sw-rot stream %d: software OSD active (text items, "
                 "coordinates in ROTATED %dx%d frame space; no privacy covers)",
             si, vc->w, vc->h);
}

static void sw_osd_free(vchan *vc)
{
    for (int i=0; i<MS_MAX_OSD; i++){
        sw_osd_slot *sl = &vc->osd.it[i];
        free(sl->bgra); sl->bgra = NULL;
        if (sl->font_owned && sl->font){ msttf_free(sl->font); free(sl->font); }
        sl->font = NULL; sl->font_owned = 0;
        sl->used = 0;
    }
    vc->osd.active = 0;
}

/* free the shared SW-OSD font (ing_stop, after all sw slots are gone) */
static void sw_osd_global_shutdown(void)
{
    if (g_swrot_font_state == 1) msttf_free(&g_swrot_font);
    g_swrot_font_state = 0;
}

/* Composite the enabled text items onto the rotated bounce buffer. Called per
 * frame from sw_rot_thread; the (expensive) placeholder expansion + rasterize
 * runs at most once per second (imp_osd.c's updater cadence - keeps the
 * {timestamp} fresh at second granularity), the (cheap) Y-plane blit runs per
 * frame. Item fields are runtime-mutable via /control, so each pass snapshots
 * the item under config_str_lock (same M10 pattern as imp_osd.c). */
static void sw_osd_compose(vchan *vc)
{
    if (!vc->osd.active) return;
    int64_t now = ms_now_us();
    int refresh = (now >= vc->osd.next_refresh_us);
    if (refresh){
        vc->osd.next_refresh_us = now + 1000000;
        /* keep {fps} fresh even when no bound stream runs imp_osd's updater
         * (all-sw-rotated config -> imp_osd_setup never ran) */
        osd_vars_set_fps(hub_get_fps(g_hcfg->osd.monitor_stream));
        osd_vars_set_bitrate(hub_get_bitrate(g_hcfg->osd.monitor_stream));
    }
    int fw = vc->w, fh = vc->h;                  /* EFF (rotated) frame dims */
    for (int i=0; i<MS_MAX_OSD; i++){
        sw_osd_slot *sl = &vc->osd.it[i];
        if (!sl->used) continue;
        ms_osd_item it;
        /* osd.vars_file comes along in the SAME critical section as the item:
         * it is POST-settable too, and osd_expand() below reads it - the one
         * lock-free live-mutable-string read this path had left (imp_osd.c's
         * refresh_text() snapshots it for the same reason). */
        char vars_file[sizeof g_hcfg->osd.vars_file];
        config_str_lock();
        it = g_hcfg->osd.items[vc->si][i];
        memcpy(vars_file, g_hcfg->osd.vars_file, sizeof vars_file);
        config_str_unlock();
        if (!it.enabled){                        /* runtime-disabled: hide */
            if (sl->bgra){ free(sl->bgra); sl->bgra=NULL; sl->last[0]=0; }
            continue;
        }
        if (refresh || !sl->bgra){
            char txt[256];
            osd_expand(it.text, vars_file, txt, sizeof txt);
            if (!sl->bgra || strcmp(txt, sl->last)!=0){
                /* scale font with stream height like imp_osd.c refresh_text
                 * (font_size is calibrated for 1080p) */
                int fs = it.font_size * fh / 1080;
                if (fs < 12) fs = 12;
                if (fs > it.font_size) fs = it.font_size;
                uint8_t *bgra=NULL; int w=0, h=0, ok;
                if (sl->font)
                    ok = msttf_render(sl->font, txt, fs, it.color, 0x00000000,
                                      it.outline, it.outline_color,
                                      &bgra,&w,&h)==0;
                else {
                    int scale = fs/16; if (scale<1) scale=1;
                    ok = osd_text_render(txt, scale, it.color, 0x00000000,
                                         it.outline, it.outline_color,
                                         &bgra,&w,&h)==0;
                }
                if (ok){
                    if (w > fw || h > fh){       /* same H5 discard as imp_osd */
                        LOGW(MOD,"sw-rot stream %d item %d: rendered %dx%d "
                                 "exceeds frame %dx%d - skipped",
                             vc->si, i, w, h, fw, fh);
                        free(bgra);
                    } else {
                        free(sl->bgra);
                        sl->bgra=bgra; sl->w=w; sl->h=h;
                        snprintf(sl->last, sizeof sl->last, "%s", txt);
                    }
                }
            }
        }
        if (!sl->bgra) continue;
        int x, y;
        sw_resolve_pos(fw, fh, sl->w, sl->h, it.x, it.y, &x, &y);
        sw_osd_blit_y(vc->bounce, fw, fh, sl->bgra, sl->w, sl->h, x, y,
                      it.transparency);
    }
}

/* Cadence-gate tuning for the unbound SW-rotate path (see the block comment at
 * sw_rot_thread's GetFrame). RESET_US: a capture timestamp this far BEHIND the
 * gate's deadline is a clock reset/wrap, not an early frame - re-anchor on it
 * rather than drop frames forever. 1 s matches PTS_SKEW_VIDEO_US, the same
 * "how far may capture legitimately lag" bound pts_sanitize() uses on the very
 * same field. REPORT_US: length of the one-shot window over which each rotate
 * thread measures and logs delivered-vs-encoded fps, so the gap this gate
 * exists for stays visible on any board/SDK combination (and so it is obvious
 * if a future libimp starts honouring the rate divider itself). */
#define MS_SW_ROT_CAD_RESET_US   1000000LL
#define MS_SW_ROT_CAD_REPORT_US 10000000LL

/* ---- 5a: the unbound rotate+encode thread ---------------------------------
 * Mirrors video_thread's on-demand/publish structure, minus everything that
 * needs an encoder channel: there is no Start/StopRecvPic (WE push frames, so
 * "not pulling" == "not encoding") and no GetStream/pack loop (YuvEncode
 * returns one contiguous AU). Idle gating is identical: block on act_wait()
 * with no consumers, debounced fs_unuse after MS_IDLE_STOP_US. */
static void *sw_rot_thread(void *arg)
{
    vchan *vc = (vchan*)arg;
    int receiving=0;
    int64_t idle_since=0;
    int dbg_first=0, dbg_getfail=0, dbg_encfail=0;
    /* cadence gate (rationale at the GetFrame call below): this unbound
     * consumer is handed every SENSOR frame regardless of videoN.fps, so the
     * configured rate is enforced here. cad_due = the capture timestamp at
     * which the next frame is due; 0 = unarmed (re-armed on every (re)start of
     * the frame flow, so the first frame after an idle stop is always kept). */
    const int64_t cad_period = 1000000 / (vc->fps > 0 ? vc->fps : 25);
    const int64_t cad_tol    = cad_period / 4;
    int64_t cad_due=0, cad_t0=0;
    int cad_seen=0, cad_enc=0, cad_reported=0;
    while (vc->run) {
        int want = vc->active || hub_active(vc->chn);
        if (!want) {
            if (!receiving){ act_wait(act_ready_vchan, vc); continue; }
            int64_t now = ms_now_us();
            if (idle_since==0) idle_since = now;
            if (now - idle_since >= MS_IDLE_STOP_US) {
                fs_unuse(vc->chn);            /* stop the frame flow entirely */
                receiving=0; idle_since=0;
                LOGI(MOD,"sw-rot chn%d idle",vc->chn);
                continue;
            }
            /* debounce window: keep encoding below (publish no-ops, 0 subs) */
        } else {
            idle_since = 0;
            if (!receiving){
                fs_use(vc->chn);                 /* EnableChn (refcounted) */
                /* depth for GetFrame must be set AFTER EnableChn on this libimp;
                 * re-set on every re-enable (DisableChn on idle clears it) */
                if (IMP_FrameSource_SetFrameDepth(vc->chn, 2)!=0)
                    LOGW(MOD,"sw-rot chn%d: SetFrameDepth failed",vc->chn);
                receiving=1;
                cad_due=0; cad_t0=0; cad_seen=0; cad_enc=0;  /* re-arm the gate */
                vc->idr_req=0; IMP_Encoder_YuvRequestIDR(vc->yuv_h);
                LOGI(MOD,"sw-rot chn%d streaming",vc->chn);
            }
        }
        if (vc->idr_req){ vc->idr_req=0; IMP_Encoder_YuvRequestIDR(vc->yuv_h); }
        /* GetFrame blocks up to the channel's frame period once the FS is
         * enabled; on a disabled/empty channel it fails fast -> pace the retry
         *
         * CADENCE GATE - why videoN.fps is enforced HERE, in user code, and not
         * by the framesource. MEASURED on cam-H (T23 libimp 1.3.0,
         * sc2336, 2026-08-18): what this loop is handed is the SENSOR rate, not
         * videoN.fps. video1 = 640x480@15 rotated 90: fs_create() set
         * outFrmRateNum=15 and YuvInit was told 15 (so SPS and container both
         * advertise 15), yet the stream delivered 29.85 fps over 60 s - the full
         * 30 fps sensor rate - while the BOUND channel 0 delivered its
         * configured 25 (24.88 measured) at the same moment. The framesource
         * rate divider is therefore honoured for a bound consumer but NOT for a
         * user-mode GetFrame consumer: the same class of gap as the missing HW
         * OSD / privacy / piggyback-JPEG on this path. There is no SDK call to
         * reach for either - T23 1.3.0 exports no IMP_FrameSource_SetFrameRate
         * (imp_framesource.h has CreateChn/SetChnAttr and no per-channel rate
         * call), and SetChnAttr's outFrmRateNum is precisely the field already
         * proven not to reach this consumer. So the rate is enforced in this
         * loop.
         *
         * What it cost while unenforced, same board: the transpose and the
         * software encode ran at 2x the intended rate (sw_rot_thread at 39% of
         * the lone core for 480x640, against ~21% total daemon CPU for the same
         * stream unrotated); rate control was parameterised for 15 fps and fed
         * 30; and the safe envelope in sw_rot_start - which gates on the
         * CONFIGURED <=704x576 / <=15 fps - did not bound what this thread
         * actually did. Under 5 clients delivery came in bursts (ffmpeg reported
         * ~14.6 non-monotonic DTS/s on the rotated stream against 0.03/s
         * unrotated), consistent with a CPU-saturated path draining a capture
         * backlog in gulps.
         *
         * The gate drops surplus frames immediately after GetFrame and BEFORE
         * nv12_rotate90, so a dropped frame costs one GetFrame/ReleaseFrame pair
         * and nothing else: the transpose, the software encode, the OSD
         * composite, the JPEG and the publish all run at the configured rate. It
         * is a ceiling and never a source of latency - if a future libimp ever
         * honours the divider for this consumer, nothing arrives early and the
         * gate simply stops dropping. */
        IMPFrameInfo *frm = NULL;
        if (IMP_FrameSource_GetFrame(vc->chn, &frm)!=0 || !frm){
            if (receiving && (dbg_getfail++ % 100)==0)
                LOGW(MOD,"sw-rot chn%d: GetFrame delivered nothing (miss#%d)",
                     vc->chn, dbg_getfail);
            usleep(10000);
            continue;
        }
        dbg_getfail=0;
        int64_t ts = frm->timeStamp;
        /* Key the gate off the CAPTURE timestamp, not arrival time: the frames
         * kept are then evenly spaced in MEDIA time, which is what the pts
         * published below is derived from. ms_now_us() substitutes when libimp
         * leaves timeStamp at 0 - pts_sanitize() distrusts this very field for
         * the same reason.
         *
         * cad_due is a FIXED grid advanced by exactly one frame period per KEPT
         * frame - not "last kept + period" - so the long-run rate is capped at
         * exactly vc->fps even when the source ratio is not an integer (30 -> 12
         * then keeps a 2-3-2-3 pattern rather than every 2nd frame = 15).
         * cad_tol (a quarter period) absorbs capture jitter: without it a frame
         * arriving a few hundred us early is dropped and its successor lands a
         * full source interval late, halving the rate in bursts. A quarter
         * period is always well under half a source interval while we are
         * genuinely downsampling (period >= 2x the source interval there), so it
         * can never let two ADJACENT source frames through.
         * Two re-anchors keep a bad clock from wedging the gate shut: a capture
         * ts more than MS_SW_ROT_CAD_RESET_US behind the deadline is a reset or
         * wrap, so re-arm on it (a garbage ts far in the FUTURE passes once and
         * pushes cad_due out; the next sane ts then trips this same re-anchor);
         * and a deadline still not past cad_key after the advance means the
         * source stalled longer than a period, so snap forward instead of
         * encoding a catch-up burst against a deadline stuck in the past. */
        int64_t cad_key = (ts > 0) ? ts : ms_now_us();
        cad_seen++;
        if (cad_t0 == 0)  cad_t0  = cad_key;
        if (cad_due == 0) cad_due = cad_key;               /* first frame: due now */
        else if (cad_key < cad_due - MS_SW_ROT_CAD_RESET_US)
            cad_due = cad_key;                             /* clock reset/wrap */
        if (cad_key + cad_tol < cad_due) {                 /* not due yet -> drop */
            IMP_FrameSource_ReleaseFrame(vc->chn, frm);
            continue;
        }
        cad_due += cad_period;
        if (cad_due <= cad_key) cad_due = cad_key + cad_period;   /* after a stall */
        cad_enc++;
        /* one-shot delivered-vs-encoded report (integer hundredths - this is a
         * soft-float MIPS target, no doubles in the frame path) */
        if (!cad_reported && cad_key - cad_t0 >= MS_SW_ROT_CAD_REPORT_US) {
            int64_t span = cad_key - cad_t0;
            int in_x100  = (int)((int64_t)(cad_seen-1)*100*1000000/span);
            int out_x100 = (int)((int64_t)(cad_enc -1)*100*1000000/span);
            cad_reported = 1;
            LOGI(MOD,"sw-rot chn%d: framesource delivered %d.%02d fps, encoded "
                     "%d.%02d fps (target %d) over %ds",
                 vc->chn, in_x100/100, in_x100%100, out_x100/100, out_x100%100,
                 vc->fps, (int)(span/1000000));
        }
        /* transpose into the phys-contiguous bounce buffer, then release the
         * source frame BEFORE encoding so the FS depth-2 pool never starves */
        nv12_rotate90((const uint8_t*)(uintptr_t)frm->virAddr,
                      vc->src_w, vc->src_h, vc->bounce, vc->sw_rot);
        IMP_FrameSource_ReleaseFrame(vc->chn, frm);
        sw_osd_compose(vc);                   /* 5b: text OSD in eff coords */
        IMPFrameInfo f; memset(&f,0,sizeof f);
        f.width    = (uint32_t)vc->w;         /* EFF (rotated) dims */
        f.height   = (uint32_t)vc->h;
        f.pixfmt   = PIX_FMT_NV12;
        f.size     = vc->bounce_size;
        f.phyAddr  = vc->bounce_phys;
        f.virAddr  = (uint32_t)(uintptr_t)vc->bounce;
        f.timeStamp= ts;
        /* IN/OUT contract (see block comment above sw_osd section): hand
         * libimp OUR output buffer + capacity; it clobbers out.outLen with
         * the AU length on return, so BOTH fields are re-armed every call. */
        IMPEncoderYuvOut out;
        out.outAddr = vc->ybuf;
        out.outLen  = vc->ybuf_cap;
        if (IMP_Encoder_YuvEncode(vc->yuv_h, f, &out)!=0){
            if ((dbg_encfail++ % 100)==0)
                LOGW(MOD,"sw-rot chn%d: YuvEncode failed (miss#%d)",
                     vc->chn, dbg_encfail);
            continue;
        }
        dbg_encfail=0;
        if (!out.outAddr || out.outLen==0) continue;
        int key = au_is_key(vc->codec, (const uint8_t*)out.outAddr,
                            (size_t)out.outLen);
        if (!dbg_first){ dbg_first=1;
            LOGI(MOD,"sw-rot chn%d: first encoded frame len=%u key=%d",
                 vc->chn, out.outLen, key); }
        /* hub_publish copies the AU into its own refcounted pkt, so the
         * encoder-owned out.outAddr is done with by the time we loop.
         *
         * L7b - why this path is STILL on the copying publish API while
         * video_thread/jpeg_thread assemble straight into a pooled packet and
         * hand it over with hub_publish_take() (P-01). Not an oversight: it is
         * the YuvEncode IN/OUT contract documented in the block comment above.
         * Measured on real T23 hardware (cam-H, 640x480 -> 480x640
         * @15 SW-rotate, 2026-08-18); numbers below are from that board:
         *
         *  - The AU length is known only AFTER the call, so a packet used as
         *    out.outAddr must carry the WORST-CASE capacity the contract
         *    demands (ybuf_cap = ew*eh + 0x1080 = 304 KiB here, 400 KiB at the
         *    704x576 envelope limit) while ->len is the real AU: measured mean
         *    2.7 KB, P-frames ~2.2 KB, IDR ~34 KB. Publishing such a packet
         *    breaks the invariant the fan-out's only memory backstop rests on,
         *    cap ~= len: fanqueue's FQ_MAX_BYTES budget accounts ->len, so ONE
         *    stalled client would pin slots*cap - 64 * 304 KiB = 19 MiB
         *    (RTSP) or 128 * 304 KiB = 38 MiB (record ring) - on a board with
         *    37 MiB of usable RAM and ~3.4 MiB free. The budget cannot see it.
         *  - Leaving HUB_POOL_KEEP_CAP (96 KB) alone instead frees every
         *    returned buffer, i.e. a 304 KiB malloc+free PER FRAME: measured
         *    96.0 us/frame = 1.44 ms/s, against the 4.3 us/frame = 64 us/s the
         *    copy actually costs (malloc+memcpy+free at the mean AU size, same
         *    board). The "zero-copy" variant would be 23x more expensive than
         *    the copy it removes.
         *  - The copy is also what lets the encoder keep ONE output buffer and
         *    reuse it next call. Rotating out.outAddr per frame through a
         *    black-box software encoder is unverified: the disassembly shows
         *    the buffer being re-armed per frame (i264e_update_fenc), which is
         *    not a guarantee that a different address per call is supported.
         *
         * So the copy stays: 15 fps * 2.7 KB = 40 KB/s, ~64 us/s = 0.006% of
         * the core, against the ~39% of the core this thread measurably spends
         * on nv12_rotate90 + the software encode. If this path ever needs CPU
         * back, those two are the targets - not this memcpy. Same reasoning,
         * only more so, for the JPEG publish further down (jbuf_cap is
         * 2*ew*eh + 64 KiB = 664 KiB against a measured ~25 KiB JPEG).
         * ORDERING GUARD for anyone revisiting this: the copy is the LAST
         * step of the frame, long after the composition above - rotate
         * (nv12_rotate90) -> software OSD onto the ROTATED bounce in EFF
         * coordinates (sw_osd_compose, fw/fh = vc->w/vc->h) -> YuvEncode.
         * Nothing about a pooled-output variant would move that: it would
         * only change where the ENCODED BITSTREAM lands, never where or in
         * which geometry the OSD is composited. Any future attempt here must
         * keep that order intact - compositing before the rotate, or against
         * pre-rotation dims, puts the timestamp sideways or in the corner
         * that was right before the transpose. Verified on hardware with the
         * rotation on (480x640 snapshots, 2026-08-18): the three text items
         * read upright and are anchored to the ROTATED frame's top edge.
         *
         * The one part that COULD be converted without any of the above -
         * encode into ybuf as now, then hub_pkt_get(outLen)+memcpy+
         * hub_publish_take() to drop the per-frame malloc/free - buys
         * 0.32 us/frame (4.8 us/s, measured) and keeps the copy. Not worth a
         * second ownership model on the one path that has no soak history. */
        /* Fix 1: use the framesource capture timestamp ('ts' = frm->timeStamp,
         * read above), sanitized/monotonized against the wall clock, instead of
         * ms_now_us() at publish time. */
        int64_t pub_now = ms_now_us();
        int64_t pts = pts_sanitize(&vc->pts, ts, pub_now,
                                   1000000 / (vc->fps > 0 ? vc->fps : 25),
                                   PTS_SKEW_VIDEO_US);
        hub_publish(vc->chn, (const uint8_t*)out.outAddr, (size_t)out.outLen,
                    pts, key, MS_MEDIA_VIDEO, pub_now);

        /* ---- Batch 7: standalone JPEG on the SW-rotate stream ----------------
         * On-demand + throttled, mirroring jpeg_thread's contract:
         *   - only when jpeg is enabled for this stream (vc->jpeg_on) AND a
         *     JPEG subscriber is present (hub_active(HUB_JPEG_SRC_N(si))): an
         *     MJPEG/snapshot client. No subscriber -> no encode, zero cost.
         *   - throttled to the stream's jpeg_fps cadence (vc->jpeg_period), so
         *     the CPU-heavy T23 doesn't JPEG every rotated video frame.
         * We already hold the rotated NV12 frame (post-OSD) in vc->bounce at eff
         * dims vc->w x vc->h, so feed it straight to IMP_Encoder_InputJpege and
         * publish jbuf[0..len] to the per-channel JPEG hub source.
         *
         * ON-DEVICE VERIFY: IMP_Encoder_InputJpege's src pixel format is assumed
         * NV12 here (that is exactly what the transpose + OSD leave in bounce).
         * If the resulting JPEG is garbled or colour-swapped on the T23, the
         * expected input format / plane order differs and this call needs the
         * pixels converted (e.g. NV12 vs NV21, or a packed layout) first. Also
         * unverified on this SoC: whether InputJpege is blocking and whether it
         * is safe to call from this thread while HW encoders run elsewhere - if
         * it serialises against the VPU it may add latency to the video cadence. */
        if (vc->jpeg_on && vc->jbuf &&
            hub_active(HUB_JPEG_SRC_N(vc->si))) {
            int64_t jn = ms_now_us();
            if (jn >= vc->jpeg_next) {
                vc->jpeg_next = jn + vc->jpeg_period;
                int jlen = 0;
                if (IMP_Encoder_InputJpege(vc->bounce, vc->jbuf,
                                           vc->w, vc->h, vc->jpeg_q, &jlen)==0
                    && jlen > 0 && (uint32_t)jlen <= vc->jbuf_cap) {
                    hub_publish(HUB_JPEG_SRC_N(vc->si), vc->jbuf, (size_t)jlen,
                                jn, 1, MS_MEDIA_JPEG, jn);
                } else if (jlen > 0 && (uint32_t)jlen > vc->jbuf_cap) {
                    LOGW(MOD,"sw-rot chn%d: JPEG (%d) exceeds buf (%u) - dropped",
                         vc->chn, jlen, vc->jbuf_cap);
                }
            }
        }
    }
    if (receiving) fs_unuse(vc->chn);
    return NULL;
}

/* teardown of one sw-rotate slot (thread must already be stopped+joined by
 * the caller). No encoder group/chn, no OSD group, no binds to undo. */
static void sw_rot_teardown(vchan *vc)
{
    IMP_FrameSource_DisableChn(vc->chn);
    if (vc->yuv_h){ IMP_Encoder_YuvExit(vc->yuv_h); vc->yuv_h=NULL; }
    if (vc->bounce){ IMP_Encoder_VbmFree(vc->bounce); vc->bounce=NULL; }
    if (vc->ybuf){ free(vc->ybuf); vc->ybuf=NULL; }
    if (vc->jbuf){ free(vc->jbuf); vc->jbuf=NULL; }   /* Batch 7 JPEG buffer */
    sw_osd_free(vc);
    IMP_FrameSource_DestroyChn(vc->chn);
    vc->sw_rot=0;
}

/* SDK-safe envelope for the single-core software rotate path (substream class).
 * Beyond this the per-frame NV12 transpose + software encode overwhelms the
 * lone MIPS core AND (observed on T23 libimp 1.3.0) IMP_Encoder_YuvInit itself
 * refuses the oversized geometry and fails - which used to abort the entire
 * multi-stream pipeline bring-up and take the whole daemon down. Both the
 * long-standing CPU-HEAVY warning and the enforcement below share these, so the
 * numbers cited in the warning text and the numbers actually enforced can never
 * drift apart. Values match that warning: <=704x576, <=15fps.
 * NOTE the fps half of this envelope only became real with the cadence gate in
 * sw_rot_thread: it gates on the CONFIGURED fps, and until that gate existed
 * the thread was handed - and processed - the full sensor rate regardless. */
#define MS_SW_ROT_MAX_PIXELS (704L*576L)
#define MS_SW_ROT_MAX_FPS    15
/* Hardware geometry the T23 encoder demands of the ENCODED (post-rotation)
 * frame. IMPEncoderAttr (imp_encoder.h): picWidth "must be 16 aligned,
 * shouldn't less than 256"; picHeight ">= 16". IMP_Encoder_YuvInit feeds its
 * inWidth/inHeight straight into those fields, so a post-rotation width that is
 * not a multiple of 16 (or below 256) makes YuvInit fail outright - even for a
 * geometry comfortably inside the CPU envelope above. Confirmed on real T23
 * hardware: a 640x360 substream rotated 90 -> 360x640 fails because 360 is not
 * 16-aligned, while the SAME stream unrotated (encoder width 640) is fine. */
#define MS_SW_ROT_WIDTH_ALIGN 16
#define MS_SW_ROT_MIN_WIDTH   256
/* sw_rot_start return code: not success and not a hard failure, but "rotation
 * refused/failed for THIS stream - caller should bring it up UNROTATED". Kept
 * distinct from -1 (unrecoverable) so unrelated streams and this one (minus the
 * rotation) still come up instead of the whole pipeline aborting. */
#define SW_ROT_FALLBACK 1

/* bring up one 90/270 stream on the unbound SW-rotate path. On failure all
 * partial state is unwound here and no g_v slot is consumed (the caller's
 * generic fail path then only has to deal with fully-recorded slots).
 * Returns 0 on success, SW_ROT_FALLBACK (>0) when the caller should retry this
 * stream unrotated, or -1 on an unrecoverable failure. */
static int sw_rot_start(const ms_config *cfg, int i)
{
    const ms_vstream_cfg *v = &cfg->video[i];
    int chn = v->imp_chn;
    int ew, eh; ms_vstream_eff_dims(v, &ew, &eh);   /* post-rotation dims */
    /* CAPS-GATE. The USE_SW_ROTATE build knob is the operator's opt-in to the
     * CPU cost, but beyond the substream-class envelope the T23 1.3.0
     * IMP_Encoder_YuvInit REFUSES the geometry and fails - and that failure used
     * to tear down the whole daemon. So: keep the accurate CPU-HEAVY warning,
     * then REFUSE the rotation (no IMP state has been created yet at this point)
     * and tell the caller to bring this stream up UNROTATED rather than march
     * into a YuvInit call known to fail. */
    if ((long)ew*eh > MS_SW_ROT_MAX_PIXELS || v->fps > MS_SW_ROT_MAX_FPS){
        LOGW(MOD,"video%d: SW rotate at %dx%d@%dfps is CPU-HEAVY on this SoC - "
                 "strongly consider a substream-class setting (<=704x576, <=15fps)",
             i, ew, eh, v->fps);
        LOGW(MOD,"video%d: refusing SW rotate at %dx%d@%dfps (exceeds safe "
                 "envelope <=704x576 <=15fps) - stream will run UNROTATED",
             i, ew, eh, v->fps);
        return SW_ROT_FALLBACK;
    }
    /* CAPS-GATE 2 (hardware geometry, empirically confirmed on T23). The encoder
     * requires the ENCODED (post-rotation) WIDTH to be 16-aligned and >=256
     * (IMPEncoderAttr.picWidth). A 90/270 rotate makes the encoder width = the
     * SOURCE HEIGHT, so a source whose height is not a multiple of 16 (or <256)
     * drives IMP_Encoder_YuvInit into a hard failure even well inside the CPU
     * envelope (e.g. 640x360 -> 360x640: 360 is not 16-aligned). Refuse the
     * rotation rather than march into that failure - bring the stream up
     * UNROTATED (where the encoder width is the source WIDTH, typically aligned).
     * ew == v->height here (eff_dims swaps for 90/270). */
    if ((ew % MS_SW_ROT_WIDTH_ALIGN) != 0 || ew < MS_SW_ROT_MIN_WIDTH){
        LOGW(MOD,"video%d: SW rotate post-rotation width %d unusable for the "
                 "encoder (needs picWidth%%16==0 and >=256; a 90/270 rotate makes "
                 "encoder width = source height %d) - refusing rotation, stream "
                 "will run UNROTATED (use a source HEIGHT that is a multiple of "
                 "16 and >=256, e.g. 704x576 -> 576x704)",
             i, ew, v->height);
        return SW_ROT_FALLBACK;
    }
    if ((v->width|v->height)&1){
        LOGE(MOD,"video%d: SW rotate needs even dims (got %dx%d)",
             i, v->width, v->height);
        return -1;
    }
    /* framesource at the PRE-rotation dims, never bound to anything; the
     * ROT_HAS_FS_ROTATE/ROT_HAS_HW_I2D blocks in fs_create are compiled out
     * on T23, so fs_create sets no rotation attrs here. */
    if (fs_create(chn, v)!=0) return -1;
    /* NOTE: SetFrameDepth is deferred to the thread, AFTER fs_use()/EnableChn.
     * This libimp (T23 1.3.0) rejects SetFrameDepth(depth>0) on a not-yet-
     * enabled channel: "Please use IMP_FrameSource_EnableChn first". */
    /* unbound encoder: header T23/1.3.0 imp_encoder.h:534-549 (IMPEncoderYuvIn/
     * Out) + :1710-1763 (YuvInit/YuvEncode/YuvExit). Rc fill follows
     * enc_create()'s classic path: FIXQP when selected (post-f003655), else CBR
     * with the same defaults/clamps. FIXQP must be set explicitly - an all-zero
     * mode union reads back as FIXQP with qp=0 (broken stream), and before this
     * fixqp on a sw-rotated T23 stream silently ran as CBR here. Kept H264-only
     * on purpose (no H264/H265 branch like enc_create()): T23's vendored SDK
     * marks every H.265 rc struct "不支持" (HEVC encode unsupported), so there
     * is no attrH265* path worth mirroring on this SoC. */
    IMPEncoderYuvIn yin; memset(&yin,0,sizeof yin);
    int qmin, qmax; qp_bounds(v, &qmin, &qmax);   /* unset defaults + ordered */
    yin.type = (v->codec==MS_VC_H265) ? PT_H265 : PT_H264;
    yin.outFrmRate.frmRateNum = (uint32_t)v->fps;
    yin.outFrmRate.frmRateDen = 1;
    yin.maxGop = (uint32_t)v->gop;
    if (v->rc_mode==MS_RC_FIXQP){
        yin.mode.rcMode = ENC_RC_MODE_FIXQP;
        yin.mode.attrH264FixQp.qp = (v->qp>0)?(uint32_t)v->qp:35;
    } else if (v->rc_mode==MS_RC_VBR || v->rc_mode==MS_RC_SMART ||
               v->rc_mode==MS_RC_CAPPED_VBR || v->rc_mode==MS_RC_CAPPED_QUALITY){
        /* Same substitution as enc_create()'s classic path (see the comment
         * there): the classic rc enum has no CAPPED_* mode, so capped_vbr/
         * capped_quality fall back to VBR with a one-time warning instead of
         * silently running as CBR. H264-only here - T23's vendored SDK marks
         * every attrH265* rc struct unsupported, so there is no H265 side to
         * this branch (unlike enc_create()'s classic path). */
        static int warned_capped = 0;
        if ((v->rc_mode==MS_RC_CAPPED_VBR || v->rc_mode==MS_RC_CAPPED_QUALITY) && !warned_capped){
            LOGW(MOD,"rc_mode capped_vbr/capped_quality has no classic-SoC equivalent -> using vbr");
            warned_capped = 1;
        }
        yin.mode.rcMode = (v->rc_mode==MS_RC_SMART) ? ENC_RC_MODE_SMART : ENC_RC_MODE_VBR;
        yin.mode.attrH264Vbr.maxQp       = (uint32_t)qmax;
        yin.mode.attrH264Vbr.minQp       = (uint32_t)qmin;
        yin.mode.attrH264Vbr.staticTime  = 2;
        yin.mode.attrH264Vbr.maxBitRate  = (uint32_t)v->bitrate_kbps;
        yin.mode.attrH264Vbr.iBiasLvl    = v->i_bias_lvl;
        yin.mode.attrH264Vbr.changePos   = (uint32_t)v->change_pos;
        yin.mode.attrH264Vbr.qualityLvl  = (uint32_t)v->quality_lvl;
        yin.mode.attrH264Vbr.frmQPStep   = 3;
        yin.mode.attrH264Vbr.gopQPStep   = 15;
        yin.mode.attrH264Vbr.gopRelation = 0;
    } else {
        yin.mode.rcMode = ENC_RC_MODE_CBR;
        yin.mode.attrH264Cbr.maxQp        = (uint32_t)qmax;
        yin.mode.attrH264Cbr.minQp        = (uint32_t)qmin;
        yin.mode.attrH264Cbr.outBitRate   = (uint32_t)v->bitrate_kbps;
        yin.mode.attrH264Cbr.iBiasLvl     = v->i_bias_lvl;
        yin.mode.attrH264Cbr.frmQPStep    = 3;
        yin.mode.attrH264Cbr.gopQPStep    = 15;
        yin.mode.attrH264Cbr.adaptiveMode = 0;
        yin.mode.attrH264Cbr.gopRelation  = 0;
    }
    void *h = NULL;
    if (IMP_Encoder_YuvInit(&h, ew, eh, &yin)!=0 || !h){
        LOGE(MOD,"sw-rot chn%d: IMP_Encoder_YuvInit %dx%d failed",chn,ew,eh);
        IMP_FrameSource_DestroyChn(chn);
        /* Defense in depth (Fix 2): a YuvInit failure that slips past the
         * envelope gate above (an unexpected sensor/firmware combo, a future
         * safe-envelope miscalculation) must NOT take down the whole daemon.
         * The framesource is already destroyed, so no IMP state survives:
         * ask the caller to bring THIS stream up unrotated and leave every
         * other stream's bring-up untouched. */
        LOGW(MOD,"video%d: SW rotate init failed - disabling rotation for this "
                 "stream, bringing it up UNROTATED",i);
        return SW_ROT_FALLBACK;
    }
    /* phys-contiguous bounce frame for the encoder input (VbmAlloc, page
     * aligned); VbmV2P yields the physical addr YuvEncode's DMA needs */
    uint32_t bsz = (uint32_t)((size_t)ew*(size_t)eh*3/2);
    uint8_t *bv = (uint8_t*)IMP_Encoder_VbmAlloc(bsz, 4096);
    if (!bv){
        LOGE(MOD,"sw-rot chn%d: VbmAlloc %u failed (rmem exhausted?)",chn,bsz);
        IMP_Encoder_YuvExit(h);
        IMP_FrameSource_DestroyChn(chn);
        return -1;
    }
    uint32_t bp = (uint32_t)IMP_Encoder_VbmV2P((intptr_t)bv);
    if (!bp){
        LOGE(MOD,"sw-rot chn%d: VbmV2P failed",chn);
        IMP_Encoder_VbmFree(bv);
        IMP_Encoder_YuvExit(h);
        IMP_FrameSource_DestroyChn(chn);
        return -1;
    }
    /* caller-owned YuvEncode OUTPUT buffer (IN/OUT contract, see block
     * comment above): plain heap is fine - the i264e encode path is pure
     * software (no DMA touches the output; input is read via virAddr) and
     * libimp only requires 4-byte alignment of outAddr (checked at
     * i264e_update_fenc @0x34840; malloc gives >=8). Capacity = 0x1080
     * bytes libimp reserves as raw-bitstream headroom + 1 byte/pixel for
     * the worst-case escaped Annex-B AU (far above any CBR IDR here). */
    uint32_t ycap = (uint32_t)((size_t)ew*(size_t)eh) + 0x1080u;
    uint8_t *yb = (uint8_t*)malloc(ycap);
    if (!yb){
        LOGE(MOD,"sw-rot chn%d: no memory for YuvEncode out buf (%u)",chn,ycap);
        IMP_Encoder_VbmFree(bv);
        IMP_Encoder_YuvExit(h);
        IMP_FrameSource_DestroyChn(chn);
        return -1;
    }
    vchan *vc = &g_v[g_nv++];
    vc->chn=chn; vc->grp=-1; vc->codec=v->codec;
    vc->w=ew; vc->h=eh;                      /* EFF dims (AU sizing unused here) */
    vc->fps=v->fps;                          /* Fix 1: nominal frame interval */
    memset(&vc->pts, 0, sizeof vc->pts);     /* reset capture-pts sanitizer */
    vc->og=-1; vc->nbound=0;
    vc->run=0; vc->active=0; vc->idr_req=0; vc->has_thr=0;
    vc->sw_rot=(v->rotation==270)?270:90;    /* 90 = clockwise (config semantics) */
    vc->si=i; vc->src_w=v->width; vc->src_h=v->height;
    vc->yuv_h=h; vc->bounce=bv; vc->bounce_phys=bp; vc->bounce_size=bsz;
    vc->ybuf=yb; vc->ybuf_cap=ycap;
    sw_osd_init(vc, cfg, i);                 /* 5b: text OSD in eff coords */
    /* Batch 7: standalone JPEG for this stream (fed from the rotated bounce via
     * IMP_Encoder_InputJpege in sw_rot_thread). Allocate the output buffer once,
     * sized generously - a JPEG of an eff_w*eff_h NV12 frame is well under one
     * byte/pixel, so eff_w*eff_h + 64 KiB is a safe cap. A JPEG alloc failure
     * only disables JPEG for this stream; the video path is unaffected. */
    vc->jpeg_on=0; vc->jbuf=NULL; vc->jbuf_cap=0; vc->jpeg_next=0;
    vc->jpeg_q = (v->jpeg_quality>0 && v->jpeg_quality<=100) ? v->jpeg_quality : 75;
    { int jfps = v->jpeg_fps>0 ? v->jpeg_fps : 5;
      vc->jpeg_period = 1000000/jfps; }
    if (v->jpeg_enabled && ((ew & 31) || (eh & 7))){
        /* IMP_Encoder_InputJpege requires width%32==0 and height%8==0. For a
         * 90/270 stream the rotated width is the source HEIGHT, so make the
         * source height a multiple of 32 (e.g. 704, not 720). Disable JPEG
         * rather than fail every frame. */
        LOGW(MOD,"video%d.jpeg: rotated %dx%d not 32/8-aligned for InputJpege "
                 "(need width%%32==0, height%%8==0; make source height a "
                 "multiple of 32, e.g. 704) - JPEG disabled on this stream",
             i, ew, eh);
    } else if (v->jpeg_enabled){
        /* M3: IMP_Encoder_InputJpege takes NO capacity argument - the T23
         * signature is src/dst/w/h/q/len and nothing more (confirmed by
         * disassembling the vendored 1.3.0 libimp). The size check further down
         * therefore runs AFTER the write: by the time it says "exceeds buf", the
         * heap past jbuf is already gone. A check cannot fix that; only the
         * buffer size can, so make an overflow arithmetically impossible instead
         * of merely improbable.
         *
         * The source is one NV12 frame, ew*eh*3/2 bytes. A JPEG cannot carry
         * more entropy than its input, but its container can EXPAND it: every
         * 0xFF byte in the entropy-coded stream is stuffed to 0xFF00, so the
         * pathological worst case is ~1.25x the coded data, plus headers and
         * tables. 2x the pixel count covers ew*eh*1.5 * 1.25 = 1.875x with room
         * to spare, and the +64 KiB absorbs markers and quantisation tables.
         *
         * Cost: at 704x576 this grows the buffer from ~460 KB to ~856 KB, once
         * per SW-rotate stream that has JPEG enabled - a T23-only opt-in path.
         * Paying 400 KB to make heap corruption impossible is the right trade on
         * a daemon that runs unattended for months. */
        uint32_t jcap = (uint32_t)((size_t)ew*(size_t)eh*2u) + 65536u;
        vc->jbuf = (uint8_t*)malloc(jcap);
        if (vc->jbuf){ vc->jbuf_cap=jcap; vc->jpeg_on=1;
            /* Deliberately NOT printing jpeg_q here: on this path the frame
             * goes through IMP_Encoder_InputJpege, whose q parameter the T23
             * header marks "Not supported at this time". Logging a quality
             * that has no effect makes people tune a knob that is not
             * connected - say so instead. */
            LOGI(MOD,"video%d.jpeg: standalone JPEG on SW-rotate stream "
                     "(%dx%d, <=%d fps; jpeg_quality has NO effect on this "
                     "path - the SoC's InputJpege ignores it) "
                     "-> /snapshot.jpg /stream.mjpeg",
                 i, ew, eh, v->jpeg_fps>0?v->jpeg_fps:5);
        } else
            LOGW(MOD,"video%d.jpeg: no memory for SW-rotate JPEG buf (%u) - "
                     "JPEG disabled on this stream", i, jcap);
    }
    hub_set_video_params(i, v->codec, ew, eh, v->fps);
    vc->run=1;
    if (ms_thread_create(&vc->thr,MS_STACK_STREAM,sw_rot_thread,vc)==0) vc->has_thr=1;
    else { vc->run=0; LOGE(MOD,"sw-rot chn%d thread create failed",chn); }
    LOGI(MOD,"video%d: SOFTWARE rotate %d (%dx%d -> %dx%d) via unbound "
             "YuvEncode - no HW OSD/privacy; JPEG via standalone InputJpege%s",
         i, vc->sw_rot, v->width, v->height, ew, eh,
         vc->jpeg_on ? "" : " (disabled)");
    return 0;
}
#endif /* ROT_HAS_SW_90 */

/* ================= JPEG / MJPEG ================= */
static void *jpeg_thread(void *arg)
{
    jchan *jc = (jchan*)arg;
    /* P-01: each JPEG frame is assembled directly into a pooled packet
     * (hub_pkt_get) and handed off with hub_publish_take() - no persistent
     * jbuf scratch and no copy at publish. */
    int receiving=0;
    int dbg_jstartfail=0;              /* rate-limits StartRecvPic failures */
    int dbg_jpollfail=0;               /* J1: PollingStream-miss watchdog */
    int dbg_jrecover_fails=0;          /* consecutive forced-recovery cycles
                                         * that never yielded a real frame -
                                         * see MS_JPEG_WATCHDOG_MAX_RECOVERIES */
    int dbg_jempty=0;                  /* rate-limits the empty-stream drop */
    int64_t next=0, idle_since=0;
    int64_t period = 1000000/(jc->fps>0?jc->fps:5);
    while (jc->run) {
        /* run when an MJPEG/snapshot client is connected, hub_active() sees
         * a subscriber, or a periodic file snapshot is configured AND due
         * (M8): a bare configured snapshot_path no longer pins the pipeline
         * on 24/7 - between snapshots the existing idle-stop debounce below
         * (MS_IDLE_STOP_US) shuts framesource+encoder back down, same as the
         * on-demand client path. snapshot_path carries no F_CTRL today
         * (config.c: FS("snapshot_path", 0, snapshot_path, 0)), so it is not
         * actually /control-mutable - the config_str_lock here is kept as
         * cheap insurance against that changing, not because it must (M3).
         * Stop is debounced like video to avoid Start/
         * StopRecvPic churn. */
        int64_t nowj = ms_now_us();
        config_str_lock();
        int snap_configured = jc->snapshot && g_hcfg->jpeg.snapshot_path[0]!=0;
        config_str_unlock();
        int snap_due = snap_configured &&
                        (nowj - jc->last_snapshot_us >= MS_SNAPSHOT_INTERVAL_US);
        int jwant = jc->active || snap_due || hub_active(jc->src);
        if (!jwant) {
            /* fully idle: block until reactivated (see act_wait). snap_due is
             * time-based (no act_wake), so it rides the 1 s timeout re-poll. */
            if (!receiving){ act_wait(act_ready_jchan, jc); continue; }
            int64_t nowi = ms_now_us();
            if (idle_since==0) idle_since = nowi;
            if (nowi - idle_since >= MS_IDLE_STOP_US) {
                IMP_Encoder_StopRecvPic(jc->chn);
                fs_unuse(jc->fs_chn);         /* stop the frame flow entirely */
                receiving=0; idle_since=0;
                continue;
            }
        } else idle_since = 0;
        if (!receiving){
            fs_use(jc->fs_chn);
            /* same failure class as video_thread: never mark the channel
             * receiving when StartRecvPic was rejected (H6) */
            if (IMP_Encoder_StartRecvPic(jc->chn)!=0){
                if ((dbg_jstartfail++ % 20)==0)
                    LOGE(MOD,"jpeg chn%d: StartRecvPic failed (attempt %d)",
                         jc->chn, dbg_jstartfail);
                fs_unuse(jc->fs_chn);
                usleep(200000);
                continue;
            }
            dbg_jstartfail=0; receiving=1;
        }
        int64_t now=ms_now_us();
        if (now<next){ usleep(next-now); }
        next=ms_now_us()+period;

        if (IMP_Encoder_PollingStream(jc->chn, g_hcfg->imp_polling_timeout)!=0){
            dbg_jpollfail++;
            if ((dbg_jpollfail % 20)==1)
                LOGW(MOD,"jpeg chn%d: PollingStream idle (miss#%d) - encoder emits no frames",
                     jc->chn, dbg_jpollfail);
            if (dbg_jpollfail >= MS_JPEG_WATCHDOG_ITERS){
                dbg_jrecover_fails++;
                if (dbg_jrecover_fails >= MS_JPEG_WATCHDOG_MAX_RECOVERIES){
                    /* Same reasoning as video_thread's escalation, but give
                     * up on just this channel instead of the whole process
                     * (see MS_JPEG_WATCHDOG_MAX_RECOVERIES above) - mirrors
                     * audio_thread's "disable and exit the thread" pattern.
                     * receiving is still 1 here, so falling out of the loop
                     * runs the normal StopRecvPic/fs_unuse epilogue below. */
                    LOGE(MOD,"jpeg chn%d: %d consecutive forced-recovery cycles never "
                         "produced a frame - giving up on this channel (MJPEG/snapshot "
                         "output disabled until restart)",
                         jc->chn, dbg_jrecover_fails);
                    break;
                }
                LOGE(MOD,"jpeg chn%d: encoder dead after %d consecutive misses - "
                     "forcing a framesource disable/enable cycle to recover "
                     "(recovery attempt %d/%d)",
                     jc->chn, dbg_jpollfail, dbg_jrecover_fails, MS_JPEG_WATCHDOG_MAX_RECOVERIES);
                IMP_Encoder_StopRecvPic(jc->chn);
                fs_unuse(jc->fs_chn);
                fs_use(jc->fs_chn);
                if (IMP_Encoder_StartRecvPic(jc->chn)!=0){
                    fs_unuse(jc->fs_chn);   /* fully release; top-of-loop's
                                             * !receiving path fs_use()s fresh */
                    receiving=0;
                }
                dbg_jpollfail=0;
            }
            continue;
        }
        dbg_jpollfail=0;
        dbg_jrecover_fails=0;   /* a real frame arrived: genuinely recovered */
        IMPEncoderStream st;
        if (IMP_Encoder_GetStream(jc->chn,&st,1)!=0){
            LOGW(MOD,"jpeg chn%d: GetStream failed after PollingStream OK", jc->chn);
            continue;
        }
        /* Size to the actual frame and assemble the JPEG straight into a
         * pooled buffer (see video_thread for the pool rationale). Summing the
         * pack lengths is exact - the old ~0.5 byte/pixel estimate under-sized
         * detail-heavy daytime frames and dropped them forever. A frame beyond
         * MS_JPEG_BUF_MAX (a genuinely huge frame or a pool failure) is dropped
         * rather than published/saved truncated. */
        size_t jneed=0;
        for (uint32_t i=0;i<st.packCount;i++) jneed += (size_t)st.pack[i].length;
        if (jneed > MS_JPEG_BUF_MAX){
            LOGW(MOD,"chn%d: JPEG exceeds max buffer (need=%zu, max=%d) - dropping frame",
                 jc->chn, jneed, MS_JPEG_BUF_MAX);
            IMP_Encoder_ReleaseStream(jc->chn,&st);
            continue;
        }
        ms_pkt *pk = hub_pkt_get(jc->src, jneed ? jneed : 1);
        if (!pk){
            LOGW(MOD,"chn%d: no memory for JPEG packet (need=%zu) - dropping frame",
                 jc->chn, jneed);
            IMP_Encoder_ReleaseStream(jc->chn,&st);
            continue;
        }
        uint8_t *jbuf = pk->data;
        size_t   jcap = pk->cap;
        int overflow=0;
        size_t jlen = enc_assemble_packs(&st, jbuf, jcap, 0, &overflow);
        /* Defensive: jneed is a strict upper bound on jlen, so unreachable;
         * drop (and return the pooled buffer) rather than publish/save truncated. */
        if (overflow){
            LOGW(MOD,"chn%d: JPEG assembly overflow (cap=%zu, need=%zu) - dropping frame",
                 jc->chn, jcap, jneed);
            pkt_unref(pk);
            IMP_Encoder_ReleaseStream(jc->chn,&st);
            continue;
        }
        /* An assembly that came out empty (packCount 0, or every pack
         * zero-length) is not a JPEG and must not reach either consumer. The
         * snapshot writer below checks for a SHORT write - fwrite(...,0,f) != 0
         * is false, so a zero-length buffer sailed through it and rename()
         * replaced the last good snapshot with a 0-byte file: exactly the
         * outcome that check exists to prevent, arrived at from the one
         * direction it does not cover. Drop the frame instead, and keep the
         * previous snapshot. */
        if (jlen == 0){
            if ((dbg_jempty++ % 20)==0)
                LOGW(MOD,"jpeg chn%d: empty stream (packCount=%u) - dropping frame (%d)",
                     jc->chn, st.packCount, dbg_jempty);
            pkt_unref(pk);
            IMP_Encoder_ReleaseStream(jc->chn,&st);
            continue;
        }
        dbg_jempty=0;
        pk->len = jlen;
        /* Give the IMP stream buffer back HERE, not after the snapshot
         * write and publish below. enc_assemble_packs() above has copied every
         * pack into pk->data, so nothing past this point reads `st` - while the
         * snapshot fwrite/rename can stall for hundreds of ms on SD-card wear
         * levelling, and holding a checked-out stream across that stalls the
         * encoder's own ring (the next frame has nowhere to land). */
        IMP_Encoder_ReleaseStream(jc->chn,&st);
        /* Snapshot-to-file is subscriber-independent and must read the buffer
         * BEFORE the hand-off: after hub_publish_take() the packet may already
         * be recycled or in flight to a subscriber. */
        int64_t pub_now = ms_now_us();
        if (snap_configured &&
            pub_now - jc->last_snapshot_us >= MS_SNAPSHOT_INTERVAL_US) {
            /* copy the path under config_str_lock, then do the (blocking)
             * file I/O against the local copy - never hold the lock across
             * fopen/fwrite/rename (M3). */
            char path[128], tmp[160];
            config_str_lock();
            snprintf(path, sizeof path, "%s", g_hcfg->jpeg.snapshot_path);
            config_str_unlock();
            snprintf(tmp,sizeof tmp,"%s.tmp",path);
            /* Check the writes before publishing: a short fwrite or a failing
             * fclose means a full/yanked SD card, and an unconditional rename()
             * would then replace the last GOOD snapshot with a truncated one.
             * Same handling timelapse.c:159-166 already does for its shots. */
            FILE *f=fopen(tmp,"wb");
            if (f){
                int werr = (fwrite(jbuf,1,jlen,f) != (size_t)jlen);
                if (fclose(f)!=0) werr=1;
                if (werr || rename(tmp,path)!=0){
                    LOGW(MOD,"snapshot %s: %s", path, strerror(errno));
                    unlink(tmp);
                }
            }
            jc->last_snapshot_us = ms_now_us();
        }
        /* Hand off to the hub. A 0-subscriber publish returns the buffer
         * straight to the pool - equivalent to the old jc->active/hub_active
         * gate, which only ever skipped the now-eliminated malloc+copy. */
        hub_publish_take(jc->src, pk, pub_now, 1, MS_MEDIA_JPEG, pub_now);
    }
    if (receiving){ IMP_Encoder_StopRecvPic(jc->chn); fs_unuse(jc->fs_chn); }
    return NULL;
}

#ifndef ENC_NEW_API
/* Item-2: apply jpeg.quality on the classic-API SoCs. The ENC_NEW_API path
 * folds quality straight into iInitialQP at SetDefaultParam, but the classic
 * encoder exposes no scalar quality knob - the only lever is a user JPEG
 * quantization table via IMP_Encoder_SetJpegeQl. So we synthesize one from the
 * standard JPEG Annex-K base tables scaled by the IJG (libjpeg) quality formula,
 * exactly the 1..100 -> quant-table mapping libjpeg uses. Previously the classic
 * branch did (void)quality and every JPEG came out at the SDK's fixed default,
 * silently ignoring jpeg.quality / videoN.jpeg_quality. */
#if !defined(PLATFORM_T10)
/* Standard JPEG Annex-K base quantization tables (== quality 50), natural
 * (row-major) order. Used by every classic SoC except T10 - including T20,
 * whose IMPEncoderJpegeQl.qmem_table is 256 bytes (vs 128 on T21/T23/T30):
 * prudynt-t (src/IMPEncoder.cpp's MakeTables()) fills the same first 128
 * bytes there too, leaving the rest zeroed, and that's been the field-proven
 * behaviour across real T20 cameras - so T20 gets the same 128-byte fill as
 * the others rather than being excluded alongside T10. */
static const uint8_t k_jpeg_luma_q50[64] = {
    16, 11, 10, 16, 24, 40, 51, 61,
    12, 12, 14, 19, 26, 58, 60, 55,
    14, 13, 16, 24, 40, 57, 69, 56,
    14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68,109,103, 77,
    24, 35, 55, 64, 81,104,113, 92,
    49, 64, 78, 87,103,121,120,101,
    72, 92, 95, 98,112,100,103, 99
};
static const uint8_t k_jpeg_chroma_q50[64] = {
    17, 18, 24, 47, 99, 99, 99, 99,
    18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99
};
static uint8_t jpeg_scale_q(uint8_t base, int scale)
{
    int v = ((int)base * scale + 50) / 100;   /* IJG: round(base*scale/100) */
    if (v < 1)   v = 1;                        /* 0 is illegal in a JPEG DQT  */
    if (v > 255) v = 255;
    return (uint8_t)v;
}
#endif /* !T10 */

static void jpeg_apply_quality(int chn, int quality)
{
    if (quality < 1)   quality = 1;
    if (quality > 100) quality = 100;
#if defined(PLATFORM_T10)
    /* prudynt-t special-cases exactly T10 ("fix for bad jpeg image quality on
     * T10 based cameras": user_ql_en=0, i.e. leave the SDK's own default table
     * alone) - a custom table there makes JPEG WORSE, not just unconfirmed, so
     * report honestly instead of guessing or (as before) silently dropping it. */
    (void)chn;
    LOGW(MOD,"jpeg.quality=%d not applied: custom quantization tables are known "
             "to degrade JPEG quality on T10 (JPEG left at SDK default)", quality);
#else
    /* T20/T21/T23/T30: fill the qtable's first 128 bytes - 64 luma then 64
     * chroma, 8-bit, natural order (T20's is 256 bytes total; see the comment
     * above). IJG quality->scale (base tables = q50). */
    int s = (quality < 50) ? (5000 / quality) : (200 - quality * 2);
    IMPEncoderJpegeQl ql; memset(&ql, 0, sizeof ql);
    _Static_assert(sizeof ql.qmem_table >= 128, "JPEG qtable smaller than 128B");
    ql.user_ql_en = 1;
    for (int i = 0; i < 64; i++){
        ql.qmem_table[i]      = jpeg_scale_q(k_jpeg_luma_q50[i],   s);
        ql.qmem_table[64 + i] = jpeg_scale_q(k_jpeg_chroma_q50[i], s);
    }
    if (IMP_Encoder_SetJpegeQl(chn, &ql) != 0)
        LOGW(MOD,"IMP_Encoder_SetJpegeQl(chn%d,q%d) failed (JPEG left at default)",
             chn, quality);
    else
        LOGD(MOD,"JPEG chn%d quality %d applied (user qtable)", chn, quality);
#endif
}
#endif /* !ENC_NEW_API */

/* create one JPEG encoder channel (not yet registered to a group).
 * fps: target encode rate for this channel (<=0 falls back to the old
 * hardcoded 24). For a channel piggybacked on a video group (jpeg_attach)
 * the group's framesource runs at the video fps, but jpeg_thread only
 * drains at jc->fps - passing that same rate here makes the HW encoder
 * itself skip down to the drain rate instead of encoding (and mostly
 * discarding) a JPEG on every video frame (see M7). The dedicated jpeg.*
 * channel has its own framesource already running at cfg->jpeg.fps, so this
 * is a no-op for it. */
static int jpeg_enc_create(int chn, int w, int h, int quality, int fps)
{
    IMPEncoderChnAttr a; memset(&a,0,sizeof a);
    if (fps <= 0) fps = 24;
#ifdef ENC_NEW_API
    /* exact JPEG params from prudynt: frmRate 24/1, gop 0, maxSameScene 0,
     * iInitialQP = quality, targetBitRate = 0. (Putting quality into the
     * bitrate slot with iInitialQP=-1 caused the div-by-zero SIGFPE.) */
    IMP_Encoder_SetDefaultParam(&a, IMP_ENC_PROFILE_JPEG, IMP_ENC_RC_MODE_FIXQP,
        w, h, fps, 1, 0, 0, quality, 0);
#else
    /* L5 (Low, classic T10..T30 only, NOT decidable from outside): the fps
     * argument is dropped here and rcAttr is left all-zero, so the piggyback
     * JPEG encoder in the video group most likely encodes EVERY video frame
     * (25 fps) while jpeg_thread only drains at jpeg_fps (5) - wasted VPU work,
     * and a snapshot that serves the OLDEST buffered frame rather than a fresh
     * one. The encoder-side skipping that fixes this (M7, fps via
     * SetDefaultParam) exists only on the ENC_NEW_API path above.
     *
     * Deliberately NOT "fixed": whether classic libimp reads outFrmRate 0/0 as
     * "every frame" or as "encoder default" cannot be settled without a classic
     * libimp to disassemble, and this tree bundles only the T31 and T23 ones.
     * Guessing at rcAttr on a path no camera in this fleet runs would be a
     * change nobody can verify. Recorded as a probable finding, not a proven
     * one - if a T10..T30 board ever shows stale snapshots with videoN.jpeg
     * enabled, start here. */
    (void)fps;
    a.encAttr.enType=PT_JPEG;
    a.encAttr.picWidth=w; a.encAttr.picHeight=h;
#endif
    if (IMP_Encoder_CreateChn(chn,&a)!=0){ LOGE(MOD,"JPEG CreateChn %d failed",chn); return -1; }
#ifndef ENC_NEW_API
    /* header contract: the channel must exist before SetJpegeQl */
    jpeg_apply_quality(chn, quality);
#endif
    return 0;
}

static void jpeg_chan_start(int chn, int fs_chn, int src, int w, int h,
                            int fps, int snapshot)
{
    jchan *jc=&g_j[g_nj++];
    jc->chn=chn; jc->fs_chn=fs_chn; jc->src=src; jc->w=w; jc->h=h;
    jc->fps=fps>0?fps:5; jc->snapshot=snapshot;
    jc->last_snapshot_us=0;  /* due immediately: first loop iteration takes one */
    jc->run=1; jc->active=0; jc->has_thr=0;
    if (ms_thread_create(&jc->thr,MS_STACK_STREAM,jpeg_thread,jc)!=0){
        /* keep the slot (the IMP channel exists and must be destroyed in
         * stop) but mark it thread-less so stop() never joins a pthread_t
         * that was never created (M8) */
        jc->run=0;
        LOGE(MOD,"jpeg chn%d thread create failed (channel kept for teardown)",chn);
    } else jc->has_thr=1;
}

/* dedicated JPEG channel: own framesource + own encoder group (jpeg.*) */
static int jpeg_setup(const ms_config *cfg)
{
    int chn = cfg->jpeg.imp_chn;
    ms_vstream_cfg jv; memset(&jv,0,sizeof jv);
    jv.width=cfg->jpeg.width; jv.height=cfg->jpeg.height; jv.fps=cfg->jpeg.fps>0?cfg->jpeg.fps:5;
    jv.buffers=2; jv.codec=MS_VC_H264; /* framesource is codec-agnostic */
    if (fs_create(chn,&jv)!=0) return -1;
    if (jpeg_enc_create(chn, cfg->jpeg.width, cfg->jpeg.height, cfg->jpeg.quality,
                        cfg->jpeg.fps)!=0){
        IMP_FrameSource_DestroyChn(chn);
        return -1;
    }
    /* the group/register/bind chain must succeed or the channel never emits
     * a frame - unwind exactly what was created on each failure (H6/M8) */
    if (IMP_Encoder_CreateGroup(chn)<0){
        LOGE(MOD,"JPEG CreateGroup %d failed",chn);
        IMP_Encoder_DestroyChn(chn);
        IMP_FrameSource_DestroyChn(chn);
        return -1;
    }
    if (IMP_Encoder_RegisterChn(chn, chn)!=0){
        LOGE(MOD,"JPEG RegisterChn %d failed",chn);
        IMP_Encoder_DestroyGroup(chn);
        IMP_Encoder_DestroyChn(chn);
        IMP_FrameSource_DestroyChn(chn);
        return -1;
    }
    IMPCell fs={DEV_ID_FS,chn,0}, enc={DEV_ID_ENC,chn,0};
    if (IMP_System_Bind(&fs,&enc)<0){
        LOGE(MOD,"JPEG Bind fs%d->enc%d failed",chn,chn);
        IMP_Encoder_UnRegisterChn(chn);
        IMP_Encoder_DestroyGroup(chn);
        IMP_Encoder_DestroyChn(chn);
        IMP_FrameSource_DestroyChn(chn);
        return -1;
    }
    /* framesource is enabled on demand by jpeg_thread (fs_use/fs_unuse) */
    jpeg_chan_start(chn, chn, HUB_JPEG_SRC, cfg->jpeg.width, cfg->jpeg.height,
                    cfg->jpeg.fps, cfg->jpeg.snapshot_path[0]!=0);
    LOGI(MOD,"JPEG channel %d ready (%dx%d q%d)",chn,cfg->jpeg.width,cfg->jpeg.height,cfg->jpeg.quality);
    return 0;
}

/* optional JPEG encoder piggybacked on video stream vi: registered into the
 * SAME encoder group, so it shares the video framesource. Costs no extra rmem
 * (no new framesource buffers), only the encoder channel itself. */
/* v must be the EFFECTIVE (post-safe-envelope-decision) stream config the caller
 * is actually bringing the video channel up with - NOT the raw cfg->video[vi].
 * When a 90/270 rotation is refused by the safe-envelope check (sw_rot_start on
 * T23, fs_create FALLBACK on T31), the caller retargets v at a local copy with
 * rotation zeroed and brings the video encoder up UNROTATED. This piggyback
 * JPEG shares that same framesource, so it must size its channel from the SAME
 * v: reading cfg->video[vi] here instead would compute the ROTATED dims (e.g.
 * 1920x1080 -> 1080x1920) while the framesource is actually unrotated
 * (1920x1080), and the mismatched (and typically non-16-aligned) picWidth makes
 * IMP_Encoder_CreateChn fail - silently killing /snapshot.jpg on that channel
 * even though the video/RTSP path fell back to unrotated cleanly. */
static int jpeg_attach(const ms_vstream_cfg *v, int vi, int grp)
{
    int chn = v->jpeg_chn;
    int q   = (v->jpeg_quality>0 && v->jpeg_quality<=100) ? v->jpeg_quality : 75;
    int jfps = v->jpeg_fps>0 ? v->jpeg_fps : 5;
    int ew, eh; ms_vstream_eff_dims(v,&ew,&eh);   /* shares the stream's (rotated) frame */
    if (jpeg_enc_create(chn, ew, eh, q, jfps)!=0) return -1;
    if (IMP_Encoder_RegisterChn(grp, chn)!=0){
        LOGE(MOD,"JPEG RegisterChn %d to group %d failed",chn,grp);
        IMP_Encoder_DestroyChn(chn);
        return -1;
    }
    jpeg_chan_start(chn, grp, HUB_JPEG_SRC_N(vi), ew, eh, jfps, 0);
    LOGI(MOD,"JPEG-on-video%d: encoder chn %d in group %d (%dx%d q%d)",
         vi,chn,grp,ew,eh,q);
    return 0;
}

/* ================= audio ================= */
/* Map a config samplerate to the IMP enum, or -1 when the AI capture path
 * does not support it. Only 8/16 kHz are kept: they are the rates this AI
 * bring-up (mono, 40 ms frames) is known to run on the T-series. Returning
 * -1 (instead of silently coercing to 8 kHz as before) makes the caller's
 * fallback loop pick a real rate, so g_asr - and with it SDP/faac/fMP4/SRT -
 * always matches the rate the AI is actually programmed at. */
static int ai_rate_enum(int sr)
{
    switch (sr) {
        case 8000:  return AUDIO_SAMPLE_RATE_8000;
        case 16000: return AUDIO_SAMPLE_RATE_16000;
        default:    return -1;   /* unsupported: caller must fall back */
    }
}

/* Apply one live audio.* key from the current config (g_hcfg->audio) to the
 * running audio input (dev 0 / chn 0, opened by audio_thread). Returns 1 when
 * the key is wired on this PLATFORM's IMP SDK, 0 when the SoC cannot do it
 * (the value is still parsed/persisted by the config layer). The per-SoC
 * guards come from ../audio_caps.h - keep them in sync with the caps.audio
 * list control.c reports. volume/gain/alc_gain are live parameter writes
 * (serialized via g_ai_lock). The HPF/AGC/NS enable/disable hooks are called
 * ONLY from boot-apply (single-threaded, before g_ai_up / the frame loop):
 * libimp processes those modules on its own record thread and frees them
 * unlocked, so a live toggle would race the vendor thread -> UAF. ing_control
 * therefore treats agc/ns/high_pass as persist-only (restart-required).
 * NOT here either: codec/samplerate/bitrate/channels/force_stereo/enabled are
 * init-time attributes, persist-only, applied on the next restart (channels=2/
 * force_stereo = simulated dual-mono stereo, read by audio_thread at init).
 * The spk_* speaker keys are not handled here (they drive the AO, not the AI):
 * ing_control routes them to speaker_set_volume/gain directly. */
static int ai_apply_key(const char *k)
{
    const ms_audio_cfg *a = &g_hcfg->audio;
    if (!strcmp(k,"volume")){ IMP_AI_SetVol(0, 0, a->volume); return 1; }
    if (!strcmp(k,"gain"))  { IMP_AI_SetGain(0, 0, a->gain);  return 1; }
    if (!strcmp(k,"alc_gain")){
#ifdef AUDIO_HAS_ALC_GAIN
        IMP_AI_SetAlcGain(0, 0, a->alc_gain); return 1;
#else
        return 0;
#endif
    }
    if (!strcmp(k,"high_pass")){
        if (a->high_pass) IMP_AI_EnableHpf(&g_aio);
        else              IMP_AI_DisableHpf();
        return 1;
    }
    /* the two agc_* values parameterize the AGC -> re-enable with new config */
    if (!strcmp(k,"agc")||!strcmp(k,"agc_target_dbfs")||!strcmp(k,"agc_compression_db")){
        if (a->agc){
            IMPAudioAgcConfig agc;
            agc.TargetLevelDbfs   = a->agc_target_dbfs;
            agc.CompressionGaindB = a->agc_compression_db;
            IMP_AI_EnableAgc(&g_aio, agc);
        } else IMP_AI_DisableAgc();
        return 1;
    }
    if (!strcmp(k,"ns")){                    /* 0 = off, 1..3 = level */
        if (a->ns > 0) IMP_AI_EnableNs(&g_aio, a->ns);
        else           IMP_AI_DisableNs();
        return 1;
    }
    if (!strcmp(k,"mute"))                   /* live mic mute: no IMP call - */
        return 1;                            /* audio_thread gates the publish
                                              * on g_hcfg->audio.mute per frame */
    return 0;
}

static void *audio_thread(void *arg)
{
    (void)arg;
    int dev=0, chnid=0;
    int use_aac = (g_acodec==MS_AC_AAC);
#ifdef USE_STREAM_OPUS
    int use_opus = (g_acodec==MS_AC_OPUS);
#else
    const int use_opus = 0;   /* compiled out: opus is never the effective codec */
#endif

    /* G.711 (PCMA/PCMU) is ALWAYS 8 kHz. Pin it here so a stray samplerate in
     * the config can't bring the AI up at 16 kHz for a stream that SDP then
     * tags as 8 kHz (2x-speed / unbounded buffering). AAC and Opus keep their
     * rate (Opus encodes at the capture rate; RTP still signals 48 kHz per RFC
     * 7587, that is a fixed clock label independent of the encoding rate). */
    if (!use_aac && !use_opus && g_asr != 8000) g_asr = 8000;

    /* --- configure the audio input FIRST, with a samplerate fallback ---
     * The AI frame size is decoupled from the AAC encoder: the SoC only accepts
     * "natural" frame sizes (a 40 ms frame here), so 16 kHz/1024 was rejected.
     * We capture 40 ms frames and re-block them into faac's 1024-sample units.
     * Fallback order (deduped): configured rate, then 16 kHz (AAC only - the
     * better degrade for e.g. a 48 kHz request), then 8 kHz. A rate
     * ai_rate_enum() rejects counts as a failed attempt, so g_asr is ONLY ever
     * set to the rate the AI was really programmed at. */
    int want_sr[3]; int nsr = 0;
    want_sr[nsr++] = g_asr;
    /* AAC and Opus both prefer 16 kHz as the first fallback (16 kHz is a valid
     * libopus rate; the better degrade for e.g. an unsupported 48 kHz request). */
    if ((use_aac || use_opus) && g_asr != 16000) want_sr[nsr++] = 16000;
    if (g_asr != 8000)             want_sr[nsr++] = 8000;
    int ai_ok = 0;
    IMPAudioIOAttr aio;
    for (int r=0; r<nsr && !ai_ok; r++){
        int sr = want_sr[r];
        int re = ai_rate_enum(sr);
        if (re < 0){
            LOGW(MOD,"audio.samplerate %dHz unsupported by AI%s", sr,
                 (r+1<nsr)?" -> falling back":"");
            continue;
        }
        memset(&aio,0,sizeof aio);
        aio.samplerate = re;
        aio.bitwidth   = AUDIO_BIT_WIDTH_16;
        aio.soundmode  = AUDIO_SOUND_MODE_MONO;
        aio.frmNum     = MS_AI_FRM_NUM;
        aio.numPerFrm  = sr*40/1000;          /* 40 ms: 320@8k, 640@16k */
        aio.chnCnt     = 1;
        if (IMP_AI_SetPubAttr(dev,&aio)==0){ g_asr=sr; ai_ok=1; break; }
        LOGW(MOD,"IMP_AI_SetPubAttr %dHz failed%s", sr, (r+1<nsr)?" -> falling back":"");
    }
    if (!ai_ok){
        /* Nothing was advertised yet (ing_start no longer pre-publishes the
         * hub audio params); the clear is kept as a belt-and-braces reset of
         * any state left from a previous start/stop cycle. */
        LOGE(MOD,"audio input unavailable");
        hub_clear_audio_params();
        return NULL;
    }

    /* Bring up dev 0 / chn 0. None of these three calls were previously
     * return-checked: on some T-series + sensor combos (e.g. sc2336 on T23,
     * see the prudynt-t report for the same chip on a Sonoff board) one of
     * them fails, yet the old code fell through to g_ai_up=1 and
     * hub_set_audio_params() regardless - RTSP then advertises a live-looking
     * audio track that silently never carries a single frame, with no error
     * anywhere in the log to explain why. */
    if (IMP_AI_Enable(dev)!=0){
        LOGE(MOD,"IMP_AI_Enable failed");
        hub_clear_audio_params();
        return NULL;
    }
    /* REQUIRED on T-series: without a channel frame depth the AI delivers no
     * frames (PollingFrame then returns empty -> silent audio) */
    IMPAudioIChnParam chnp; memset(&chnp,0,sizeof chnp);
    chnp.usrFrmDepth = MS_AI_FRM_DEPTH;   /* low depth = low audio latency */
    if (IMP_AI_SetChnParam(dev,chnid,&chnp)!=0){
        LOGE(MOD,"IMP_AI_SetChnParam failed");
        IMP_AI_Disable(dev);
        hub_clear_audio_params();
        return NULL;
    }
    if (IMP_AI_EnableChn(dev,chnid)!=0){
        LOGE(MOD,"IMP_AI_EnableChn failed");
        IMP_AI_Disable(dev);
        hub_clear_audio_params();
        return NULL;
    }
    /* boot-apply the live audio set from the config (same ai_apply_key path
     * the /control endpoint uses at runtime); the off-state features are
     * simply not enabled instead of calling the IMP_AI_Disable* hooks.
     * g_ai_up is raised only afterwards: until then ing_control() leaves the
     * AI alone (the value lands in g_cfg first, so it is picked up here). */
    g_aio = aio;
    ai_apply_key("volume");
    ai_apply_key("gain");
    if (g_hcfg->audio.alc_gain > 0 && !ai_apply_key("alc_gain"))
        LOGW(MOD,"audio.alc_gain unsupported on this platform (ignored)");
    if (g_hcfg->audio.high_pass) ai_apply_key("high_pass");
    if (g_hcfg->audio.agc)       ai_apply_key("agc");
    if (g_hcfg->audio.ns > 0)    ai_apply_key("ns");
    g_ai_up = 1;

    /* --- now open faac at the samplerate the AI actually accepted --- */
#ifdef USE_FAAC
    /* modern libfaac API: the legacy faacEnc* API (faacEncOpen/…/faacEncClose)
     * was removed upstream, so we drive faac_encoder_* instead. */
    faac_encoder *faac = NULL;
    uint32_t faac_in = 1024, faac_max = 8192;   /* filled from encoder info below */
    if (use_aac) {
        faac_params fp; faac_params_init(&fp);
        fp.sample_rate   = (uint32_t)g_asr;
        fp.num_channels  = (uint32_t)g_ach;     /* 2 = simulated stereo (dual-mono) */
        fp.mpeg_version  = FAAC_MPEG4;
        fp.object_type   = FAAC_OBJ_LOW;        /* AAC-LC */
        fp.input_format  = FAAC_INPUT_16BIT;
        fp.output_format = FAAC_STREAM_RAW;     /* raw AAC (no ADTS) */
        fp.use_tns = false; fp.use_lfe = false;
        /* faac's bit_rate is PER CHANNEL; audio.bitrate is the total stream
         * rate, so a dual-mono stereo stream keeps the configured total */
        if (g_hcfg->audio.bitrate_kbps>0)
            fp.bit_rate = (uint32_t)g_hcfg->audio.bitrate_kbps*1000/(uint32_t)g_ach;
        faac_status st = faac_encoder_open(&fp, &faac);
        if (st==FAAC_OK && faac) {
            faac_encoder_info fi; fi.struct_size = sizeof fi;
            if (faac_encoder_get_info(faac, &fi)==FAAC_OK) {
                faac_in  = fi.frame_samples;    /* samples/channel per frame */
                faac_max = fi.max_output_bytes;
            }
            /* faac_encoder_encode() takes the TOTAL interleaved sample count,
             * so the re-blocking unit is frame_samples * channels (AAC-LC:
             * 1024 mono, 2048 stereo interleaved) */
            faac_in *= (uint32_t)g_ach;
            LOGI(MOD,"faac AAC encoder: %dHz ch=%d frame=%u max=%u",
                 g_asr,g_ach,faac_in,faac_max);
        } else {
            LOGW(MOD,"faac_encoder_open failed (%s) -> PCMU", faac_strerror(st));
            faac = NULL;
            use_aac=0; g_acodec=MS_AC_PCMU;
            g_ach=1;                            /* stereo simulation is AAC-only */
        }
    }
#else
    use_aac = 0;   /* built without USE_FAAC: no software AAC */
    if (g_acodec==MS_AC_AAC) g_acodec=MS_AC_PCMU;
    g_ach = 1;     /* stereo simulation is AAC-only (G.711 fallback = mono) */
#endif

#ifdef USE_STREAM_OPUS
    /* --- open the libopus encoder at the rate the AI actually accepted --- */
    OpusEncoder *opus = NULL;
    if (use_opus) {
        int oerr = OPUS_OK;
        /* Encode at the true capture rate (g_asr; 8/12/16/24/48 kHz are all
         * valid libopus rates and the AI fallback only ever lands on 16 or 8).
         * OPUS_APPLICATION_VOIP is the right mode for a live mic feed over IP:
         * it tunes the encoder for speech intelligibility at low bitrate, unlike
         * _AUDIO (music-optimized) or _RESTRICTED_LOWDELAY (drops the speech
         * enhancement/DTX we want for a talk stream). Mono (g_ach==1 - stereo
         * simulation stays AAC-only); the 48 kHz/2ch in the SDP rtpmap is a
         * fixed RFC 7587 label and is independent of these encoder settings. */
        opus = opus_encoder_create(g_asr, g_ach, OPUS_APPLICATION_VOIP, &oerr);
        if (opus && oerr==OPUS_OK) {
            if (g_hcfg->audio.bitrate_kbps > 0)
                opus_encoder_ctl(opus,
                    OPUS_SET_BITRATE(g_hcfg->audio.bitrate_kbps*1000));
            LOGI(MOD,"opus encoder: %dHz ch=%d VOIP", g_asr, g_ach);
        } else {
            LOGW(MOD,"opus_encoder_create failed (%s) -> PCMU",
                 opus_strerror(oerr));
            if (opus){ opus_encoder_destroy(opus); opus=NULL; }
            use_opus=0; g_acodec=MS_AC_PCMU;
            g_ach=1;                            /* G.711 fallback is mono */
        }
    }
#endif

    /* the codec/rate the HAL actually produces must be what SDP/ASC advertise.
     * If AAC was configured but faac failed, we fell back to PCMU while the AI
     * is still running at the AAC rate (e.g. 16 kHz). Relabeling g_asr alone
     * would ship 16 kHz samples tagged 8 kHz -> 2x speed, A/V drift, unbounded
     * player buffering. So reconfigure the AI to 8 kHz for real. */
    if (!use_aac && (g_acodec==MS_AC_PCMU || g_acodec==MS_AC_PCMA) && g_asr!=8000) {
        LOGW(MOD,"faac fallback: reconfiguring AI %dHz -> 8000 for G.711", g_asr);
        /* Item-6: hold g_ai_lock across the ENTIRE disable/re-enable. Clearing
         * g_ai_up alone is not enough: it is the LOCK, not the flag, that keeps a
         * /control volume/gain write off the channel mid-rebuild. (That writer
         * used to test g_ai_up before taking g_ai_lock, so one that passed the
         * test an instant before we clear it could still land an IMP_AI parameter
         * call on a disabled channel -> failed live-apply; it now takes the lock
         * first and re-tests inside it.) Under the lock such a writer blocks until
         * the channel is fully back up, or bails on the cleared g_ai_up. Same lock
         * the /control path uses (fix Item-1). */
#if defined(USE_CONTROL) || defined(USE_BACKCHANNEL) || defined(USE_PLAY)
        pthread_mutex_lock(&g_ai_lock);
#endif
        g_ai_up = 0;                       /* keep /control off the AI mid-rebuild */
        IMP_AI_DisableChn(dev,chnid);
        IMP_AI_Disable(dev);
        memset(&aio,0,sizeof aio);
        aio.samplerate = ai_rate_enum(8000);
        aio.bitwidth   = AUDIO_BIT_WIDTH_16;
        aio.soundmode  = AUDIO_SOUND_MODE_MONO;
        aio.frmNum     = MS_AI_FRM_NUM;
        aio.numPerFrm  = 8000*40/1000;     /* 320 samples / 40 ms */
        aio.chnCnt     = 1;
        if (IMP_AI_SetPubAttr(dev,&aio)!=0 || IMP_AI_Enable(dev)!=0 ||
            IMP_AI_SetChnParam(dev,chnid,&chnp)!=0 || IMP_AI_EnableChn(dev,chnid)!=0) {
            LOGE(MOD,"AI re-init at 8000 failed");
#if defined(USE_CONTROL) || defined(USE_BACKCHANNEL) || defined(USE_PLAY)
            pthread_mutex_unlock(&g_ai_lock);   /* g_ai_up stays 0: AI is down */
#endif
            hub_clear_audio_params();
            return NULL;
        }
        g_asr = 8000;
        g_aio = aio;
        /* re-apply the live audio settings to the fresh channel */
        ai_apply_key("volume");
        ai_apply_key("gain");
        if (g_hcfg->audio.alc_gain > 0) ai_apply_key("alc_gain");
        if (g_hcfg->audio.high_pass)    ai_apply_key("high_pass");
        if (g_hcfg->audio.agc)          ai_apply_key("agc");
        if (g_hcfg->audio.ns > 0)       ai_apply_key("ns");
        g_ai_up = 1;
#if defined(USE_CONTROL) || defined(USE_BACKCHANNEL) || defined(USE_PLAY)
        pthread_mutex_unlock(&g_ai_lock);
#endif
    }
    hub_set_audio_params(g_acodec, g_asr, g_ach);

    LOGI(MOD,"audio in: %dHz %s ch=%d%s vol=%d gain=%d numPerFrm=%d", g_asr,
         use_aac?"AAC":use_opus?"Opus":(g_acodec==MS_AC_PCMA?"PCMA":"PCMU"), g_ach,
         g_ach==2?" (simulated stereo)":"",
         g_hcfg->audio.volume, g_hcfg->audio.gain, aio.numPerFrm);

#ifdef USE_FAAC
    /* re-blocking buffer: accumulate 40 ms AI frames, feed faac_in-sized units.
     * acc counts INTERLEAVED samples: with simulated stereo the mono capture is
     * duplicated to L=R on append, so acc_n and faac_in use the same unit.
     * Capacity check: worst case is faac_in-1 leftover (stereo LC: 2047) plus
     * one doubled 40 ms frame (16 kHz: 2*640=1280) = 3327 < 4096. */
    int16_t   acc[4096];
    size_t    acc_n = 0;
    int dbg_logged = 0;
#endif
#ifdef USE_STREAM_OPUS
    int dbg_logged_opus = 0;
#endif

    int ai_fail_streak = 0;
    /* A1: capture-pts sanitizer for audio (own instance, never shared with a
     * video channel). Feeds the AI hardware timeStamp through pts_sanitize() so
     * a publish-thread stall (audio starved by the video encoders, then the AI
     * FIFO bursts on drain) can no longer masquerade as an audio gap and make
     * audio_gap_resync()/fmp4 M2 insert phantom samples -> drift. */
    pts_sanitizer apts;
    memset(&apts, 0, sizeof apts);
    int was_idle = 1;   /* also flushes any backlog buffered between AI-enable
                         * and the first subscriber (thread starts before them) */
    while (g_arun) {
        if (!g_aactive){ act_wait(act_ready_audio, NULL); was_idle = 1; continue; }
        if (was_idle) {
            was_idle = 0;
            /* Resume from idle. Unlike the video encoder (StopRecvPic +
             * fs_unuse on idle, see video_thread ~line 1353), the AI is NEVER
             * stopped while g_aactive==0 - it keeps capturing into its FIFO for
             * the entire idle period. Draining that stale backlog on resume
             * would feed pts_sanitize() a fast burst of seconds-old capture
             * timestamps; the burst inflates its offset and (before the FIX 2
             * ratchet-stop) drove the audio pts seconds ahead of real time. The
             * RTSP path hides such a constant A/V offset (each RTP track
             * re-anchors to NTP via its own RTCP SR), but the fMP4 muxer shares
             * one base_pts_us zero across both tracks, so it rendered as a
             * multi-second startup A/V skew (real-HW QA: fMP4 A/V drift ~-4s,
             * audio starting late). The old ms_now_us()-at-publish stamping
             * never exposed this because every drained frame was tagged "now"
             * regardless of its capture age.
             * Fix: flush the buffered backlog (drain+discard, the AI equivalent
             * of the encoder's StopRecvPic) so publishing resumes from a fresh
             * ~now frame, and re-anchor the sanitizer to it. */
            IMPAudioFrame fl;
            int drained = 0;
            while (drained < 512) {          /* bound: never spin on a live feed */
#if defined(USE_BACKCHANNEL) || defined(USE_PLAY)
                pthread_mutex_lock(&g_ai_lock);
                int rc = IMP_AI_GetFrame(dev, chnid, &fl, NOBLOCK);
                pthread_mutex_unlock(&g_ai_lock);
#else
                int rc = IMP_AI_GetFrame(dev, chnid, &fl, NOBLOCK);
#endif
                if (rc != 0) break;          /* FIFO empty: caught up to live */
                IMP_AI_ReleaseFrame(dev, chnid, &fl);
                drained++;
            }
            memset(&apts, 0, sizeof apts);   /* fresh capture-pts anchor */
            if (drained)
                LOGI(MOD, "audio resume: flushed %d stale AI frame(s)", drained);
        }
        int64_t a_t0 = ms_now_us();
        /* sleep on the no-frame path so a non-blocking/failing PollingFrame
         * can never spin the CPU (audio input may be idle on some boards) */
        if (IMP_AI_PollingFrame(dev,chnid, g_hcfg->imp_polling_timeout)!=0){
            if (++ai_fail_streak >= MS_AI_WATCHDOG_ITERS){
                /* the channel never delivered a single frame since it was
                 * (apparently) enabled - stop advertising it and let the
                 * thread exit through the normal teardown below instead of
                 * spinning forever with a dead-but-described audio track */
                LOGE(MOD,"no audio frames received - disabling audio input");
                hub_clear_audio_params();
                break;
            }
            usleep(10000); continue;
        }
        IMPAudioFrame frm;
#if defined(USE_BACKCHANNEL) || defined(USE_PLAY)
        /* Item-1: serialize the frame fetch against live AEC enable/disable
         * (hal_ao_open/close, on the backchannel/play threads). IMP_AI_
         * {Enable,Disable}Aec create/free the AI-side AEC DSP module on this
         * same dev0/chn0; without the lock a backchannel hangup's DisableAec
         * could free that module mid-fetch -> UAF/SIGSEGV in libaudioProcess.so.
         * Held ONLY across the non-blocking GetFrame (PollingFrame above already
         * confirmed a frame is ready), never across that blocking poll, so an
         * AO open/close never stalls waiting on a poll timeout. */
        pthread_mutex_lock(&g_ai_lock);
        int gf_rc = IMP_AI_GetFrame(dev,chnid,&frm,1);
        pthread_mutex_unlock(&g_ai_lock);
#else
        int gf_rc = IMP_AI_GetFrame(dev,chnid,&frm,1);
#endif
        if (gf_rc!=0){
            /* A1: PollingFrame reported a frame ready but GetFrame failed to
             * fetch it - treat this exactly like a PollingFrame miss for
             * watchdog purposes. ai_fail_streak used to be zeroed
             * unconditionally right above, before this call, regardless of
             * what it returned: a persistently-failing GetFrame could then
             * never trip the watchdog (the counter was reset every tick) and
             * this bare `continue` had no usleep, so audio died silently and
             * permanently while potentially hot-spinning this core. */
            if (++ai_fail_streak >= MS_AI_WATCHDOG_ITERS){
                LOGE(MOD,"no audio frames received - disabling audio input");
                hub_clear_audio_params();
                break;
            }
            usleep(10000); continue;
        }
        ai_fail_streak = 0;
        /* live mic mute (audio.mute via /control): keep draining the AI so
         * nothing backs up, but never feed the encoder/hub - RTSP and MP4
         * clients simply receive no audio frames until unmuted */
        if (g_hcfg->audio.mute){
            IMP_AI_ReleaseFrame(dev,chnid,&frm);
            int64_t m_dt = ms_now_us() - a_t0;
            if (m_dt < 15000) usleep(15000 - m_dt);
            continue;
        }
        const int16_t *pcm=(const int16_t*)frm.virAddr;
        size_t samples=frm.len/2;
        if (use_aac) {
#ifdef USE_FAAC
            /* append into the accumulator, then drain in faac_in blocks.
             * Simulated stereo (g_ach==2): each mono AI sample is duplicated
             * to L=R HERE, before accumulation, so acc_n always counts
             * interleaved samples - the same unit as faac_in. */
            for (size_t off=0; off<samples; ){
                size_t take = samples-off;                       /* mono samples */
                size_t room = sizeof(acc)/sizeof(acc[0]) - acc_n; /* interleaved */
                if (take*(size_t)g_ach > room) take = room/(size_t)g_ach;
                if (g_ach==2){
                    for (size_t i=0;i<take;i++){
                        acc[acc_n+2*i]   = pcm[off+i];           /* L */
                        acc[acc_n+2*i+1] = pcm[off+i];           /* R = L (dual-mono) */
                    }
                } else {
                    memcpy(acc+acc_n, pcm+off, take*sizeof(int16_t));
                }
                acc_n += take*(size_t)g_ach; off += take;
                while (acc_n >= faac_in){
                    /* P-01, audio: encode STRAIGHT into a packet borrowed from
                     * the audio source's recycling pool, exactly as the video
                     * and JPEG producers do, and hand it over with
                     * hub_publish_take(). This used to encode into an 8 KB
                     * __thread scratch buffer and let hub_publish() malloc +
                     * copy the frame out of it again, per frame, forever. The
                     * scratch (and the Opus one below) is gone with it, so the
                     * audio worker's thread-local footprint drops by ~12 KB.
                     * A borrow that yields no frame (n==0, or OOM) is returned
                     * to the pool by the pkt_unref() below. */
                    ms_pkt *pk = hub_pkt_get(HUB_AUDIO_SRC, 8192);
                    uint32_t n = 0;
                    /* FAAC_INPUT_16BIT: pass the int16 PCM directly; in_samples
                     * is the TOTAL interleaved count (frame_samples * channels
                     * == faac_in: 1024 mono, 2048 stereo). */
                    if (pk){
                        faac_status st = faac_encoder_encode(faac, acc, (uint32_t)faac_in,
                                                             pk->data, (uint32_t)pk->cap, &n);
                        if (st!=FAAC_OK) LOGW(MOD,"faac_encoder_encode: %s",faac_strerror(st));
                    }
                    if (n>0){
                        if (!dbg_logged){ LOGI(MOD,"AAC encoder producing (%u bytes/frame)",n); dbg_logged=1; }
                        /* A1: stamp with the AI capture time (frm.timeStamp),
                         * sanitized to the ms_now_us base. At most one AAC frame
                         * drains per AI frame (640 samples in, 1024-sample unit),
                         * so successive drains carry distinct, increasing capture
                         * stamps; the AAC nominal frame (1024/g_asr) is the
                         * fallback interval. */
                        int64_t a_now = ms_now_us();
                        int64_t a_pts = pts_sanitize(&apts, frm.timeStamp, a_now,
                                                     (int64_t)1024*1000000/(g_asr>0?g_asr:16000),
                                                     PTS_SKEW_AUDIO_US);
                        pk->len = (size_t)n;
                        hub_publish_take(HUB_AUDIO_SRC, pk, a_pts, 0,
                                         MS_MEDIA_AUDIO, a_now);
                    } else pkt_unref(pk);      /* NULL-safe */
                    acc_n -= faac_in;
                    memmove(acc, acc+faac_in, acc_n*sizeof(int16_t));
                }
            }
#endif
#ifdef USE_STREAM_OPUS
        } else if (use_opus) {
            /* Encode the whole native 40 ms AI capture frame as ONE Opus frame.
             * 40 ms is a valid Opus frame duration at every rate the AI runs at
             * (samples == numPerFrm == g_asr*40/1000: 320@8k, 640@16k), so no
             * re-blocking is needed (unlike faac's fixed 1024-sample unit). Mono
             * only, so `samples` is directly opus_encode's per-channel count. */
            /* P-01, audio: pooled packet as the encode target (see the AAC
             * branch above). 4 KB >> the 1275 B/frame RFC 7587 max. */
            ms_pkt *pk = hub_pkt_get(HUB_AUDIO_SRC, 4096);
            int on = pk ? opus_encode(opus, pcm, (int)samples, pk->data,
                                      (opus_int32)pk->cap) : 0;
            if (on <= 0) {
                if (on < 0) LOGW(MOD,"opus_encode: %s", opus_strerror(on));
                pkt_unref(pk);                 /* NULL-safe */
            } else {
                if (!dbg_logged_opus){
                    LOGI(MOD,"opus encoder producing (%d bytes/frame)", on);
                    dbg_logged_opus=1;
                }
                /* A1: one Opus frame per AI frame -> stamp with this frame's
                 * capture time; fallback interval = this frame's duration. */
                int64_t a_now = ms_now_us();
                int64_t a_pts = pts_sanitize(&apts, frm.timeStamp, a_now,
                                             (int64_t)samples*1000000/(g_asr>0?g_asr:16000),
                                             PTS_SKEW_AUDIO_US);
                pk->len = (size_t)on;
                hub_publish_take(HUB_AUDIO_SRC, pk, a_pts, 0,
                                 MS_MEDIA_AUDIO, a_now);
            }
#endif
        } else {
            /* P-01, audio: pooled packet as the encode target (see above).
             * 1 byte per sample, so the byte cap is also the sample cap - the
             * same bound the old 2 KB stack buffer imposed. */
            const size_t g711_max = 2048;
            if (samples>g711_max) samples=g711_max;
            ms_pkt *pk = hub_pkt_get(HUB_AUDIO_SRC, g711_max);
            if (pk){
                if (g_acodec==MS_AC_PCMA) g711_alaw_encode(pcm,samples,pk->data);
                else                      g711_ulaw_encode(pcm,samples,pk->data);
                pk->len = samples;
                /* A1: one G.711 frame per AI frame -> capture time, sanitized. */
                int64_t a_now = ms_now_us();
                int64_t a_pts = pts_sanitize(&apts, frm.timeStamp, a_now,
                                             (int64_t)samples*1000000/(g_asr>0?g_asr:8000),
                                             PTS_SKEW_AUDIO_US);
                hub_publish_take(HUB_AUDIO_SRC,pk,a_pts,0,MS_MEDIA_AUDIO,a_now);
            }
        }
        IMP_AI_ReleaseFrame(dev,chnid,&frm);
        /* adaptive pacing: if IMP_AI didn't actually block (spin), throttle to
         * ~real time; if it blocked (~20 ms), this adds nothing */
        int64_t a_dt = ms_now_us() - a_t0;
        if (a_dt < 15000) usleep(15000 - a_dt);
    }
#ifdef USE_FAAC
    if (faac) faac_encoder_close(&faac);
#endif
#ifdef USE_STREAM_OPUS
    if (opus) opus_encoder_destroy(opus);
#endif
    /* Same disable sequence as the faac-fallback rebuild above, and it needs
     * the same lock for the same reason (Item-6/Item-1): clearing g_ai_up is
     * not enough on its own. A /control writer's g_ai_up test and its IMP_AI
     * parameter call are one critical section under g_ai_lock (the test used
     * to sit OUTSIDE the lock, so a writer that passed it an instant before
     * we clear the flag could still land a call on the channel we are tearing
     * down; it now re-tests inside) - and on the AEC-capable builds the
     * same window belongs to hal_ao_open/close's IMP_AI_{Enable,
     * Disable}Aec on this very dev0/chn0, which is the documented
     * free-while-in-use UAF in libaudioProcess.so. Holding the lock across
     * the whole sequence makes such a caller block until the channel is fully
     * down and then see g_ai_up==0.
     * Normal shutdown gets here with httpd already stopped, so the window is
     * narrow - but the audio thread also exits on its own (the AI watchdog's
     * "no audio frames received" break) while /control is very much alive,
     * and that path is not narrow at all. */
#if defined(USE_CONTROL) || defined(USE_BACKCHANNEL) || defined(USE_PLAY)
    pthread_mutex_lock(&g_ai_lock);
#endif
    g_ai_up = 0;
    IMP_AI_DisableChn(dev,chnid);
    IMP_AI_Disable(dev);
#if defined(USE_CONTROL) || defined(USE_BACKCHANNEL) || defined(USE_PLAY)
    pthread_mutex_unlock(&g_ai_lock);
#endif
    return NULL;
}

#ifdef USE_CONTROL
/* Apply a live setting from /control (via hub_control). Keys are the config
 * file keys: image.*, audio.* (live: volume/gain/alc_gain/high_pass/agc/
 * agc_target_dbfs/agc_compression_db/ns), osdS.N.* (per-stream) and legacy
 * osdN.* (all streams). The value arrives as a string; numbers are parsed
 * here. ISP calls serialize under g_isp_lock, AI runtime calls under g_ai_lock
 * (AGC/NS/HPF module toggles deferred to the audio thread); OSD goes
 * through imp_osd_apply(stream,item). ALL videoN.*
 * and sensor.* keys plus the attribute-level audio keys (enabled/codec/
 * samplerate/bitrate/channels/force_stereo) are NOT applied live
 * (persisted only, take effect on restart); the spk_* speaker keys are
 * persisted but ignored - timps has no AO pipeline. Compiled only with
 * -DUSE_CONTROL. */
/* g_isp_lock is defined up in the FrameSource section (fs_use() also takes it on
 * a chn0 enable edge, and fs_use() is compiled in non-USE_CONTROL builds too). */
/* set by ing_control() when a live hflip/vflip apply needs the ISP latch kick;
 * consumed once in ing_control_commit() so a settings POST carrying both
 * hflip+vflip collapses to a single fs_kick_chn0() (see fs_kick_chn0). flip is
 * only ever set via /control, which always calls hub_control_commit(). */
static int g_isp_flip_kick_pending = 0;
/* ---- live rate-control apply (ENC_LIVE_KEYS, enc_caps.h) ----------------
 * Everything here is strictly per channel: stream N's key is applied to
 * stream N's own encoder channel and no other. The caller (control.c) has
 * already stored the new value in g_cfg, so the appliers read from there. */

/* is this videoN.* leaf one of the keys this BUILD can apply live? */
static int rc_key_live(const char *k)
{
#ifdef ENC_LIVE_KEYS
    static const char *const live[] = { ENC_LIVE_KEYS };
    for (size_t i=0;i<sizeof live/sizeof live[0];i++)
        if (!strcmp(k, live[i])) return 1;
#else
    (void)k;
#endif
    return 0;
}

/* stream index -> its running vchan; NULL when the stream never came up
 * (disabled at boot, bring-up failed) - then live apply is impossible and
 * the persisted value waits for the next restart. */
static vchan *rc_live_vchan(int si)
{
    if (!g_hcfg || si<0 || si>=MS_MAX_VSTREAM) return NULL;
    int chn = g_hcfg->video[si].imp_chn;
    for (int i=0;i<g_nv;i++)
        if (g_v[i].chn==chn) return &g_v[i];
    return NULL;
}

/* Apply one live rc key to stream si's running encoder. Returns 1 when the
 * IMP call succeeded, 0 on any fallback (the value is persisted either way).
 * Takes effect at the next IDR/GOP per the SDK docs, never mid-frame. */
static int rc_live_apply(int si, const char *k)
{
    vchan *vc = rc_live_vchan(si);
    if (!vc) return 0;
#ifdef ROT_HAS_SW_90
    if (vc->sw_rot) return 0;   /* unbound Yuv encoder: no runtime rc API */
#endif
    const ms_vstream_cfg *v = &g_hcfg->video[si];
    int chn = vc->chn;
#ifndef ENC_NEW_API
    /* Classic API: SetChnAttrRcMode takes the whole rc union, so ANY live rc
     * key re-derives the complete block from g_cfg via the same fill
     * enc_create used - the encoder never sees a half-updated struct, and
     * this includes a live rc_mode switch (the header supports FIXQP/CBR/
     * VBR/SMART). H264 only: T23's header (en+zh) marks the call H264-only;
     * T21/T30 say "H264 and H265" but stay refused here until hardware
     * confirms it (dev_notes/TODO.md), so any H265 stream is restart-bound. */
    if (v->codec==MS_VC_H265){
        static int warned_h265_live = 0;
        if (!warned_h265_live){
            LOGW(MOD,"live rc change on an H265 stream: the classic SDK's "
                     "SetChnAttrRcMode is H264-only - applies on restart");
            warned_h265_live = 1;
        }
        return 0;
    }
    /* qp only feeds attrFixQp.iInitialQP, a union member the encoder ignores
     * outside fixqp - the whole-union refill below "succeeds" either way, so
     * without this gate a qp POST under cbr/vbr/smart would be graded live
     * (deferred:0) despite having no observable effect on the running
     * channel (2026-08-22 hardware measurement, cam-vorne/T23N; the same
     * honest-vs-optimistic gap the new-API path already closes for qp). */
    if (!strcmp(k,"qp") && v->rc_mode!=MS_RC_FIXQP) return 0;
    IMPEncoderAttrRcMode m; memset(&m,0,sizeof m);
    classic_rc_fill(&m, v);
    if (IMP_Encoder_SetChnAttrRcMode(chn, &m)!=0){
        LOGW(MOD,"SetChnAttrRcMode chn%d failed - value applies on restart", chn);
        return 0;
    }
    return 1;
#else
    /* New API: no complete fill exists (SetDefaultParam owns fields we never
     * write and cannot rebuild), so each key uses its dedicated runtime call
     * and everything else in the channel's attrs stays untouched. */
    if (!strcmp(k,"bitrate")){
        /* SetChnBitRate(chn, target, max) takes bit/s (all four new-API
         * headers), while videoN.bitrate is kbps. The max: preserve the
         * channel's CURRENT target:max ratio read back from the encoder -
         * a raw/raw quotient, so it holds whatever unit the SDK stores -
         * because uMaxBitRate is an SDK default we never wrote and blindly
         * flattening it to the target would change VBR semantics. CBR/fixqp
         * (no max field): max = target. */
        long long t_bps = (long long)v->bitrate_kbps * 1000;
        long long m_bps = t_bps;
        IMPEncoderAttrRcMode m;
        if (IMP_Encoder_GetChnAttrRcMode(chn,&m)==0){
            long long tr=0, mr=0;
            if (m.rcMode==IMP_ENC_RC_MODE_VBR){
                tr=m.attrVbr.uTargetBitRate; mr=m.attrVbr.uMaxBitRate;
            } else if (m.rcMode==IMP_ENC_RC_MODE_CAPPED_VBR ||
                       m.rcMode==IMP_ENC_RC_MODE_CAPPED_QUALITY){
                tr=m.attrCappedVbr.uTargetBitRate; mr=m.attrCappedVbr.uMaxBitRate;
            }
            if (tr>0 && mr>=tr) m_bps = t_bps*mr/tr;
        }
        if (t_bps > 0x7fffffffLL) return 0;         /* int argument */
        if (m_bps > 0x7fffffffLL) m_bps = 0x7fffffffLL;
        if (IMP_Encoder_SetChnBitRate(chn,(int)t_bps,(int)m_bps)!=0){
            LOGW(MOD,"SetChnBitRate chn%d failed - value applies on restart", chn);
            return 0;
        }
        return 1;
    }
    if (!strcmp(k,"min_qp") || !strcmp(k,"max_qp")){
        /* same call + same 15/45 unset-defaults as the boot apply (0a8bb9f) */
        int qmin, qmax; qp_bounds(v, &qmin, &qmax);
        if (IMP_Encoder_SetChnQpBounds(chn,qmin,qmax)!=0){
            LOGW(MOD,"SetChnQpBounds chn%d failed - value applies on restart", chn);
            return 0;
        }
        return 1;
    }
#ifdef ENC_HAS_QPIPDELTA
    if (!strcmp(k,"i_bias_lvl")){
        if (IMP_Encoder_SetChnQpIPDelta(chn, v->i_bias_lvl)!=0){
            LOGW(MOD,"SetChnQpIPDelta chn%d failed - value applies on restart", chn);
            return 0;
        }
        return 1;
    }
#endif
#ifdef ENC_HAS_SETRCMODE
    if (!strcmp(k,"qp")){
        /* UNREACHABLE since 2026-08-22: `qp` is no longer in ENC_LIVE_KEYS on
         * any new-API SoC, so rc_key_live() never lets it in here. Kept as the
         * record of what SetChnAttrRcMode actually does - it stores the value
         * where the next Get reads it back and never re-programs the running
         * channel (measured on cam-garage; see enc_caps.h). Restoring a live
         * qp means IMP_Encoder_SetChnQp() plus a bitstream measurement, not
         * re-listing the key.
         *
         * only operative under fixqp; read-modify-write so nothing else in
         * the union is disturbed. Under any other mode qp has no effect
         * anyway, so "restart-bound" is the honest answer there. */
        IMPEncoderAttrRcMode m;
        if (IMP_Encoder_GetChnAttrRcMode(chn,&m)!=0) return 0;
        if (m.rcMode!=IMP_ENC_RC_MODE_FIXQP) return 0;
        m.attrFixQp.iInitialQP = (int16_t)((v->qp>0)?v->qp:35);
        if (IMP_Encoder_SetChnAttrRcMode(chn,&m)!=0){
            LOGW(MOD,"SetChnAttrRcMode chn%d (fixqp qp) failed - value applies on restart", chn);
            return 0;
        }
        return 1;
    }
#endif
    return 0;
#endif /* ENC_NEW_API */
}

/* Returns 1 when the key reached the RUNNING pipeline, 0 when it only
 * persisted (applies on restart / unsupported here) - see hub.h. Only the
 * videoN.* / sensor.* grading in control.c consumes the value today, but every
 * branch answers truthfully so a future consumer does not inherit a lie. */
static int ing_control(const char *key, const char *val)
{
    int v = (int)strtol(val, NULL, 0);

    if (!strncmp(key,"image.",6)){
        /* the control layer already stored the value in g_cfg (config_apply_kv
         * runs before hub_control), so the HAL applies from the config */
        const char *k = key+6;
        pthread_mutex_lock(&g_isp_lock);
        int ok = isp_apply_image(k);
        /* Belt-and-braces for the fs_use() chn0 relatch below: a running_mode
         * (day/night) switch is suspected of being able to reset hflip/vflip
         * on some ISP/driver combos even WITHOUT a chn0 Disable/Enable cycle
         * (e.g. mid-stream, with a viewer already connected - fs_use() only
         * self-heals on a genuine 0->1 edge, which a live viewer never
         * produces). Re-asserting the current flip values here is a cheap,
         * idempotent no-op when nothing actually reset, and closes that
         * window regardless of which mechanism is the real cause. */
        if (ok && !strcmp(k,"running_mode")){
            isp_apply_image("hflip");
            isp_apply_image("vflip");
        }
        pthread_mutex_unlock(&g_isp_lock);
        if (ok) LOGI(MOD,"control %s=%d", key, v);
        else    LOGD(MOD,"image.%s unsupported on this platform (persisted only)", k);
        /* day/night: the ISP only latches a running-mode change while fs chn0
         * runs - kick it if idle (see fs_kick_chn0; blocks ~500 ms, fine for
         * /control connection threads and the daynight thread). Kept INLINE
         * (not deferred to commit) because daynight.c calls hub_control(
         * "image.running_mode") directly, without a hub_control_commit(). */
        if (ok && !strcmp(k,"running_mode")) fs_kick_chn0();
        /* hflip/vflip are the same latch class (fs_kick_chn0): a boot-time or
         * chn0-idle apply otherwise never takes effect. Coalesce to a single
         * kick in ing_control_commit() so a POST carrying both hflip and vflip
         * does not kick twice; safe because flip is only ever set via /control,
         * which always ends in hub_control_commit(). */
        else if (ok && (!strcmp(k,"hflip") || !strcmp(k,"vflip")))
            g_isp_flip_kick_pending = 1;
        return ok ? 1 : 0;
    }

    if (!strncmp(key,"audio.",6)){
        const char *k = key+6;
        /* speaker keys: spk_volume/spk_gain are live AO parameter writes -
         * applied now if a play/backchannel session holds the speaker, and
         * re-applied from the config at every AO open (speaker.c ao_ensure),
         * so they also become the default for the next session. spk_enabled is
         * the master AO gate, also read live in ao_ensure() (0 = keep the AO
         * closed, no output). Without an AO pipeline compiled in they just
         * persist. */
        if (!strncmp(k,"spk_",4)){
#if defined(USE_BACKCHANNEL) || defined(USE_PLAY)
            if      (!strcmp(k,"spk_volume")) speaker_set_volume(g_hcfg->audio.spk_volume);
            else if (!strcmp(k,"spk_gain"))   speaker_set_gain(g_hcfg->audio.spk_gain);
            LOGI(MOD,"control %s=%d", key, v);
            return 1;
#else
            LOGD(MOD,"audio.%s persisted (no speaker/AO in this build)", k);
            return 0;
#endif
        }
        /* Item-3: AEC engages inside hal_ao_open (it needs both AI capture and
         * AO output live), so a live toggle just persists and takes effect at
         * the next AO open - the same "applied at next AO open" contract as the
         * spk_* keys above. No IMP call here. */
        if (!strcmp(k,"aec")){
#if defined(USE_BACKCHANNEL) || defined(USE_PLAY)
            LOGI(MOD,"control %s=%d (applies at next AO open)", key, v);
#else
            LOGD(MOD,"audio.%s persisted (no speaker/AO in this build)", k);
#endif
            return 0;   /* not in effect until the next AO open */
        }
        /* persist-only keys (take effect on restart): encoder/SetPubAttr-level
         * attributes - including channels/force_stereo, which audio_thread
         * reads at the next init to enable simulated stereo (dual-mono AAC) -
         * AND the DSP module toggles high_pass/agc/ns. The latter are
         * restart-required by necessity: libimp runs AGC/NS/HPF on its OWN
         * internal record thread, and IMP_AI_Disable{Agc,Ns,Hpf} frees that
         * module state with no lock, so toggling it live races the vendor
         * thread -> use-after-free / SIGSEGV inside libaudioProcess.so
         * (epc=0, ra in libimp). Boot-apply (before g_ai_up, single-threaded,
         * one-shot Enable, never a Disable) is the only safe place. */
        if (!strcmp(k,"enabled")   || !strcmp(k,"codec")    ||
            !strcmp(k,"samplerate")|| !strcmp(k,"bitrate")  ||
            !strcmp(k,"channels")  || !strcmp(k,"force_stereo") ||
            !strcmp(k,"high_pass") || !strcmp(k,"agc")      ||
            !strcmp(k,"agc_target_dbfs") || !strcmp(k,"agc_compression_db") ||
            !strcmp(k,"ns")){
            LOGI(MOD,"%s persisted, applies on restart", key);
            return 0;
        }
        /* volume/gain/alc_gain: plain parameter writes (no module create or
         * destroy), safe to apply live; serialized via g_ai_lock.
         *
         * Found by review: g_ai_up used to be checked here BEFORE taking
         * the lock below, the same TOCTOU window as hal_ao_open()'s AEC
         * enable - a disable can complete in the gap between this check
         * and actually acquiring g_ai_lock, and audio_thread's disable path
         * holds that same lock across its whole clear-g_ai_up sequence
         * specifically so a re-check under the lock sees the true state.
         * So take the lock first, then check. */
        pthread_mutex_lock(&g_ai_lock);        /* dev 0 / chn 0 as in audio_thread */
        if (!g_ai_up){                         /* audio input not running */
            pthread_mutex_unlock(&g_ai_lock);
            LOGD(MOD,"%s persisted (audio input not running)", key);
            return 0;
        }
        int ok = ai_apply_key(k);
        pthread_mutex_unlock(&g_ai_lock);
        if (ok) LOGI(MOD,"control %s=%d", key, v);
        else    LOGD(MOD,"audio.%s unsupported on this platform (persisted only)", k);
        return ok ? 1 : 0;
    }

    /* videoN.*: geometry/codec/fps keys stay config-only (a live change
     * would need a stream-killing channel/ISP reconfig) - but the rate-
     * control subset CAN reach the running encoder on this build
     * (ENC_LIVE_KEYS, enc_caps.h). Strictly per channel: stream N's key
     * touches stream N's encoder channel only. On any fallback (channel not
     * running, classic H265, IMP call rejected) the value is already
     * persisted, so "applies on restart" remains true. */
    if (!strncmp(key,"video",5) && key[5]>='0' && key[5]<'0'+MS_MAX_VSTREAM
        && key[6]=='.'){
        if (rc_key_live(key+7) && rc_live_apply(key[5]-'0', key+7)){
            LOGI(MOD,"control %s applied to the running encoder "
                     "(takes effect at the next IDR/GOP)", key);
            return 1;
        }
        LOGI(MOD,"%s persisted, applies on restart", key);
        return 0;
    }
    /* sensor.*: config-only, applied at the next ISP init */
    if (!strncmp(key,"sensor.",7)){
        LOGI(MOD,"%s persisted, applies on restart", key);
        return 0;
    }

    /* daynight.*: config-only (the detection thread polls g_cfg), no HAL
     * action - the actual ISP mode change comes in as image.running_mode
     * via the board's color script. The detection thread reads the new
     * value on its next tick, so it IS in effect without a restart. */
    if (!strncmp(key,"daynight.",9)) return 1;

    /* motion.*: enabled/cols/rows/sensitivity/monitor_stream/hold_ms/skip_frames
     * are applied LIVE by cleanly stopping and recreating the IVS grid (see
     * motion_sync; hold_ms is the motion_thread hold window re-read in
     * imp_motion_start, skip_frames is the IVS create-time skipFrameCnt).
     * cooldown_ms/on_motion are config-only: the polling thread reads them from
     * g_cfg per event. */
    if (!strncmp(key,"motion.",7)){
        const char *k = key+7;
        if (!strcmp(k,"sensitivity")){
            /* Item-1: a pure sensitivity change updates the running channel's
             * per-ROI sense[] in place (IMP_IVS_SetParam) instead of the full
             * stop/destroy/recreate. Flagged separately from the geometry keys
             * below; if a geometry key ALSO arrives in this request the full
             * rebuild supersedes at commit and re-applies sensitivity anyway.
             * Deferred to commit like the rest so one POST = one apply. */
            g_motion_sense_pending = 1;
            LOGD(MOD,"control %s applied (IVS sensitivity update deferred to commit)", key);
            return 1;
        } else if (!strcmp(k,"enabled") || !strcmp(k,"cols") || !strcmp(k,"rows") ||
                   !strcmp(k,"monitor_stream") ||
                   !strcmp(k,"hold_ms") || !strcmp(k,"skip_frames")){
            /* M2: defer the (expensive) IVS stop/destroy/recreate to a single
             * commit at the end of the /control request. A settings-form POST
             * naturally carries several of these keys (cols+rows+sensitivity+
             * monitor_stream) at once; rebuilding per key would blind detection
             * and stall the response for seconds.
             * Item-4: hold_ms and skip_frames are both F_CTRL (POST-able) but
             * used to fall through here with no action - accepted-but-silently-
             * deferred. They take effect only through imp_motion_start (hold_ms
             * = g_hold_ms; skip_frames = IVS create-time skipFrameCnt), which is
             * exactly what motion_sync re-runs, so route them through the same
             * deferred resync as the geometry keys. */
            g_motion_resync_pending = 1;
            LOGD(MOD,"control %s applied (IVS grid re-sync deferred to commit)", key);
            return 1;
        }
        return 1;   /* cooldown_ms etc.: read from g_cfg per event, live */
    }

    /* osdS.N.* (per-stream) / legacy osdN.* (all streams): config (g_cfg) is
     * already updated -> re-apply the whole item on the right stream(s) */
    if (!strncmp(key,"osd",3) && key[3]>='0' && key[3]<'0'+MS_MAX_OSD && key[4]=='.'){
        if (key[5]>='0' && key[5]<'0'+MS_MAX_OSD && key[6]=='.' &&
            key[3]<'0'+MS_MAX_VSTREAM)
            imp_osd_apply(key[3]-'0', key[5]-'0');   /* osdS.N.* */
        else
            imp_osd_apply(-1, key[3]-'0');           /* legacy osdN.* */
        return 1;
    }

    /* privacy<S>.<N>.* cover masks: config (g_cfg) already updated -> re-apply
     * the region LIVE (create/show/hide/move) on that stream */
    if (!strncmp(key,"privacy",7) && key[7]>='0' && key[7]<'0'+MS_MAX_VSTREAM &&
        key[8]=='.' && key[9]>='0' && key[9]<'0'+MS_MAX_PRIVACY && key[10]=='.'){
        int s = key[7]-'0';
        imp_osd_privacy_apply(s, key[9]-'0');
        /* privacy zones are excluded from the IVS motion grid (they share the
         * FrameSource with the cover, which would otherwise trip motion). If this
         * region is on the monitored stream and motion is running, rebuild the
         * grid so the mask takes/loses effect live. */
        int mon = g_hcfg->motion.monitor_stream;
        if (mon<0 || mon>=MS_MAX_VSTREAM || !g_hcfg->video[mon].enabled) mon=0;
        /* M2: same deferral - dragging one privacy rectangle posts x+y+w+h in a
         * single request; rebuild the grid once at commit, not four times. */
        if (g_hcfg->motion.enabled && s==mon) g_motion_resync_pending = 1;
        return 1;
    }

    /* osd.* (master switch/global font/vars file): config-only - the OSD
     * groups are built once in imp_osd_setup at startup, so these take
     * effect on the next daemon restart */
    if (!strncmp(key,"osd.",4)){
        LOGI(MOD,"%s persisted, applies on restart", key);
        return 0;
    }
    return 0;   /* unknown to the HAL: persisted only */
}

/* M2: flush deferred HAL applies after a full /control request. Currently only
 * the IVS motion-grid rebuild is batched: any number of motion/privacy keys in
 * one POST collapse to a single stop/destroy/recreate here instead of one per
 * key. A request that touched no motion/privacy key leaves the flag clear and
 * this is a no-op. */
static void ing_control_commit(void)
{
    if (g_motion_resync_pending){
        g_motion_resync_pending = 0;
        g_motion_sense_pending = 0;     /* the full rebuild re-applies sensitivity too */
        motion_sync(g_hcfg);
        LOGI(MOD,"IVS motion grid re-synced (batched, once per request)");
    } else if (g_motion_sense_pending){
        g_motion_sense_pending = 0;
        /* Item-1 fast path: sensitivity-only change - update the live channel's
         * sense[] via IMP_IVS_SetParam instead of a stop/destroy/recreate.
         * Serialize against motion_sync with the same mutex (same g_motion_mtx
         * -> g_st_lock order the start/stop path already uses). On ANY failure
         * (or if motion isn't actually running) fall back to the full rebuild so
         * the channel is never left half-updated. */
        pthread_mutex_lock(&g_motion_mtx);
        int running = (g_motion_pin >= 0);
        int rc = running ? imp_motion_set_sensitivity(g_hcfg) : 0;
        pthread_mutex_unlock(&g_motion_mtx);
        if (running && rc != 0){
            motion_sync(g_hcfg);
            LOGI(MOD,"IVS sensitivity live-update failed, full grid rebuild done");
        }
    }
    /* one ISP latch kick for the whole request if any hflip/vflip changed, so a
     * live flip toggle takes effect immediately instead of only by luck of chn0
     * already streaming (see fs_kick_chn0). Not under g_isp_lock. */
    if (g_isp_flip_kick_pending){
        g_isp_flip_kick_pending = 0;
        fs_kick_chn0();
        LOGI(MOD,"ISP flip latch kicked (batched, once per request)");
    }
}
#endif

/* Teardown bookkeeping: a failed IMP_* call here surfaces only as an ISP
 * init failure of the NEXT run (see S95timps wait_stop), by which time the
 * cause never logged a line. Collect, report ONE summary line at the end. */
static int  g_td_nerr;
static char g_td_first[64];
static int td(int rc, const char *name)
{
    if (rc != 0 && g_td_nerr++ == 0)
        snprintf(g_td_first, sizeof g_td_first, "%s rc=%d", name, rc);
    return rc;
}
static void td_report(const char *what)
{
    if (g_td_nerr)
        LOGW(MOD,"%s: %d IMP call(s) failed (first: %s) - the next start may "
             "fail ISP init", what, g_td_nerr, g_td_first);
}

/* ================= HAL entry points ================= */
static int ing_init(const ms_config *cfg)
{
    g_hcfg=cfg;
    int r = isp_init();
#ifdef USE_CONTROL
    if (r==0){ hub_set_control_cb(ing_control); hub_set_control_commit_cb(ing_control_commit); }
#endif
    return r;
}

static int ing_start(const ms_config *cfg)
{
    g_nv=0; g_nj=0;
    memset(g_eff_rot, 0, sizeof g_eff_rot);   /* rewritten per stream below */
    for (int i=0;i<MS_MAX_VSTREAM;i++){
        if (!cfg->video[i].enabled) continue;
        const ms_vstream_cfg *v=&cfg->video[i];
#ifdef ROT_HAS_SW_90
        ms_vstream_cfg lv;   /* local UNROTATED fallback copy (cfg is const) */
        /* T23 + USE_SW_ROTATE: 90/270 streams take the unbound SW-rotate path
         * (no encoder group, no binds - see sw_rot_start). Non-rotated (0)
         * streams stay on the normal bound pipeline below. */
        if (v->rotation==90 || v->rotation==270){
            int r = sw_rot_start(cfg,i);
            if (r==0){ g_eff_rot[i]=v->rotation; continue; } /* sw-rotate up */
            if (r<0)  goto fail;        /* unrecoverable */
            /* r==SW_ROT_FALLBACK: rotation refused (envelope) or its init
             * failed for THIS stream only. Do NOT abort the whole pipeline -
             * fall through and bring this one stream up on the normal bound
             * path UNROTATED. cfg is const, so retarget v at a local copy with
             * rotation zeroed (in-memory only, does not persist to the file).
             * No IMP state survives sw_rot_start's fallback returns, so the
             * bound path below starts clean. */
            lv = cfg->video[i]; lv.rotation = 0; v = &lv;
        }
#endif
        int chn=v->imp_chn, grp=v->imp_chn;
#ifdef ROT_HAS_FS_ROTATE
        ms_vstream_cfg flv;   /* local UNROTATED fallback copy (cfg is const) */
        {
            int r = fs_create(chn,v);
            if (r<0) goto fail;             /* unrecoverable */
            if (r==FS_ROT_FALLBACK){
                /* T31: fs_create refused the rotation (outside the safe
                 * envelope - Fix 1) or its FS rotate-enable call failed (Fix 2).
                 * Either way no IMP channel was created (SetChnRotate/CreateChn
                 * both run after the refusal points, and SetChnRotate runs
                 * before CreateChn), so bring THIS one stream up UNROTATED on the
                 * normal bound path instead of aborting the whole pipeline. cfg
                 * is const, so retarget v at a local copy with rotation zeroed
                 * (in-memory only, does not persist to the file); the encoder
                 * dims below then follow the unrotated geometry too. */
                flv = cfg->video[i]; flv.rotation = 0; v = &flv;
                if (fs_create(chn,v)!=0) goto fail;
            }
        }
#else
        if (fs_create(chn,v)!=0) goto fail;
#endif
        /* v now points at the config this stream is ACTUALLY coming up with
         * (retargeted at an unrotated local copy above if a rotation safe-
         * envelope check refused the request): record the effective rotation
         * for motion_sync (see g_eff_rot). */
        g_eff_rot[i] = v->rotation;
        /* The encoder GROUP must exist before enc_create's
         * IMP_Encoder_RegisterChn(grp,chn): the canonical Ingenic order is
         * CreateGroup -> CreateChn -> RegisterChn. Older libimp (T23 and other
         * pre-ENC_NEW_API SoCs) rejects RegisterChn into a not-yet-created group,
         * so the channel stays unregistered and never emits a stream (no video,
         * no SPS). T31's newer libimp happened to tolerate the wrong order. */
        if (IMP_Encoder_CreateGroup(grp)<0){
            LOGE(MOD,"Encoder_CreateGroup %d failed",grp);
            IMP_FrameSource_DestroyChn(chn);
            goto fail;
        }
        if (enc_create(chn,grp,v)!=0){
            IMP_Encoder_DestroyGroup(grp);
            IMP_FrameSource_DestroyChn(chn);
            goto fail;
        }
        /* record the slot as soon as its IMP channels exist: g_nv drives the
         * channel teardown (stop AND the failure path below), independent of
         * whether the drain thread ever starts (M8) */
        int ew, eh; ms_vstream_eff_dims(v,&ew,&eh);   /* post-rotation stream dims */
        /* Publish the effective (post-refusal) dims to the hub BEFORE anything
         * that renders against them. imp_osd_setup() below performs the FIRST
         * OSD text/logo/cover render synchronously (refresh_text/setup_logo/
         * setup_cover), and those call osd_rotated() (imp_osd.c), which asks
         * the hub for the ACTUAL post-rotation dims to detect a refused 90/270
         * request. This used to run AFTER imp_osd_setup()+the binds (right
         * before vc->run=1), so that very first render always hit the "hub not
         * populated yet" fallback and, on a refused rotation, briefly used the
         * pre-refusal (wrong) rotated/hlim answer for one render pass - the
         * gap identified in review of cb4c7de. ew/eh/v are already final here
         * (post rotation-refusal retarget), hub_set_video_params() is a pure
         * mutex-protected struct store with no side effects on OSD/bind state,
         * and nothing reads the hub for this stream until after ing_start()
         * returns (imp_osd_start_updater() below only starts once every stream
         * in this loop is done) - so moving the publish earlier is safe and
         * makes the first OSD render see the correct answer immediately. */
        hub_set_video_params(i, v->codec, ew, eh, v->fps);
        vchan *vc=&g_v[g_nv++];
        vc->chn=chn; vc->grp=grp; vc->codec=v->codec;
        vc->w=ew; vc->h=eh;
        vc->fps=v->fps;                          /* Fix 1: nominal frame interval */
        memset(&vc->pts, 0, sizeof vc->pts);     /* reset capture-pts sanitizer */
        vc->og=-1; vc->nbound=0;
        vc->run=0; vc->active=0; vc->idr_req=0; vc->has_thr=0;
        vc->ave_valid=0; vc->ave_bitrate=-1.0;   /* Item-2: reset telemetry cache */
#ifdef ROT_HAS_SW_90
        /* slots are static + reused across start/stop cycles: make sure a
         * bound-path slot never carries stale sw-rotate state into teardown */
        vc->sw_rot=0; vc->si=i; vc->yuv_h=NULL; vc->bounce=NULL;
#endif

        /* optional per-stream JPEG encoder in the same group (videoN.jpeg);
         * non-fatal: the video stream works without it (logged inside) */
        if (v->jpeg_enabled) jpeg_attach(v, i, grp);

        /* pipeline: FrameSource -> [OSD] -> Encoder. Every stream gets its
         * own OSD group so overlays appear on all streams. An unchecked
         * failed bind used to leave a "running" pipeline that never moves a
         * single frame (H6). */
        IMPCell fs  = { DEV_ID_FS,  chn, 0 };
        IMPCell enc = { DEV_ID_ENC, grp, 0 };
        int og = imp_osd_setup(cfg, i, ew, eh);
        vc->og = og;                   /* teardown must unbind the REAL pairs */
        if (og >= 0) {
            IMPCell osd = { DEV_ID_OSD, og, 0 };
            if (IMP_System_Bind(&fs,&osd)<0){
                LOGE(MOD,"Bind fs%d->osd%d failed",chn,og);
                goto fail;             /* slot recorded: teardown handles it */
            }
            vc->nbound=1;
            if (IMP_System_Bind(&osd,&enc)<0){
                LOGE(MOD,"Bind osd%d->enc%d failed",og,grp);
                goto fail;
            }
            vc->nbound=2;
        } else {
            if (IMP_System_Bind(&fs,&enc)<0){
                LOGE(MOD,"Bind fs%d->enc%d failed",chn,grp);
                goto fail;
            }
            vc->nbound=1;
        }
        /* NOT enabled here: the framesource runs on demand (fs_use/fs_unuse
         * from the consumer threads) so an idle timps pumps no frames at all */

        vc->run=1;
        if (ms_thread_create(&vc->thr,MS_STACK_STREAM,video_thread,vc)==0) vc->has_thr=1;
        else { vc->run=0; LOGE(MOD,"video chn%d thread create failed",chn); }
    }

    imp_osd_start_updater();   /* one thread refreshes OSD on all streams */

    if (cfg->audio.enabled && cfg->audio.codec!=MS_AC_NONE){
        /* pick the codec the SoC can actually encode. IMP_AENC on the
         * T-series only does G.711/G.726 - there is no hardware AAC, so an
         * AAC request transparently degrades to PCMU (G.711u @ 8 kHz). */
        g_acodec = cfg->audio.codec;
#if !defined(USE_FAAC) && !defined(IMP_AUDIO_ENC_TYPE_AAC)
        if (g_acodec==MS_AC_AAC){
            LOGW(MOD,"no AAC encoder (build with USE_FAAC) -> using PCMU (G.711u)");
            g_acodec=MS_AC_PCMU;
        }
#endif
        g_asr = (g_acodec==MS_AC_AAC || g_acodec==MS_AC_OPUS) ? cfg->audio.samplerate : 8000; /* G.711 = 8 kHz */
        /* simulated stereo: audio.channels=2 (or force_stereo) duplicates the
         * mono AI capture to L=R and encodes 2-channel AAC (dual-mono) for
         * clients that require a stereo track. The RTP payload spec for G.711
         * (PCMA/PCMU) is mono-only, so a stereo request with G.711 - including
         * an AAC request that already degraded to PCMU above - stays mono.
         * audio_thread drops g_ach back to 1 if faac fails at runtime and the
         * codec falls back to PCMU. */
        {
            int want_stereo = (cfg->audio.channels==2 || cfg->audio.force_stereo);
            if (want_stereo && g_acodec!=MS_AC_AAC){
                LOGW(MOD,"audio.channels=2/force_stereo requires AAC - G.711 has no standard stereo, staying mono");
                want_stereo = 0;
            }
            g_ach = want_stereo ? 2 : 1;
        }
        /* NO hub_set_audio_params here: the hub audio params are published
         * ONCE, by audio_thread, after the AI is validated and the final
         * codec/rate is known (samplerate fallback, faac->PCMU fallback). An
         * early publish let a client DESCRIBE AAC@cfg-rate and then receive
         * the fallback codec on PLAY - and fMP4/SRT/record latch the codec
         * once, muxing G.711 bytes as AAC. The cost is only that DESCRIBE in
         * the sub-second bring-up window sees no audio track yet. */
        g_arun=1;
        if (ms_thread_create(&g_athr,MS_STACK_STREAM,audio_thread,NULL)!=0){
            g_arun=0;                /* had_audio stays 0 -> no join in stop */
            LOGE(MOD,"audio thread create failed");
        }
    }

    /* non-fatal: video streams work without the dedicated JPEG channel;
     * jpeg_setup unwinds its own partial state on failure */
    if (cfg->jpeg.enabled && jpeg_setup(cfg)!=0)
        LOGW(MOD,"dedicated JPEG channel unavailable");
    motion_sync(cfg);      /* start the IVS motion grid if motion.enabled */
    /* Latch the boot-time ISP tuning that only takes effect while fs chn0 runs.
     * apply_image_tuning() (isp_init, before any FS existed) already issued
     * hflip/vflip/running_mode, but on the on-demand pipeline chn0 is idle at
     * boot, so those Sets sit queued and never apply - the sensor comes up
     * unflipped / in the wrong day-night mode until a live /control re-apply.
     * Run chn0 briefly now (all FS channels exist at this point) to latch them.
     * No-op ref bump when motion pinned chn0 above; ~500 ms one-time otherwise.
     * Must run here (end of ing_start), NOT at the apply_image_tuning() call in
     * isp_init, where fs chn0 does not yet exist. */
    fs_kick_chn0();
    return 0;

fail:
    /* A stream's bring-up failed: tear down everything created so far so a
     * failed start leaves NO IMP channels bound/created (M8). Only video
     * slots and piggybacked JPEG channels can exist at this point (audio and
     * the dedicated JPEG channel start after the loop). Threads first, then
     * channels, mirroring ing_stop; the IMP UnBind/UnRegister calls are
     * tolerated on never-bound/never-started objects. */
    LOGE(MOD,"video pipeline bring-up failed - tearing down partial state");
    g_td_nerr = 0;   /* summary may include tolerated never-bound failures */
    for (int k=0;k<g_nv;k++) g_v[k].run=0;
    for (int k=0;k<g_nj;k++) g_j[k].run=0;
    act_wake();
    for (int k=0;k<g_nv;k++) if (g_v[k].has_thr) pthread_join(g_v[k].thr,NULL);
    for (int k=0;k<g_nj;k++) if (g_j[k].has_thr) pthread_join(g_j[k].thr,NULL);
    for (int k=0;k<g_nj;k++){          /* piggybacks: encoder channel only */
        td(IMP_Encoder_UnRegisterChn(g_j[k].chn),"Encoder_UnRegisterChn");
        td(IMP_Encoder_DestroyChn(g_j[k].chn),"Encoder_DestroyChn");
    }
    g_nj=0;
    int had_osd=0;
    for (int k=0;k<g_nv;k++){
#ifdef ROT_HAS_SW_90
        /* sw-rotate slots have no encoder chn/group, no OSD group, no binds -
         * their whole teardown is FS + YuvExit + VbmFree (thread joined above) */
        if (g_v[k].sw_rot){ sw_rot_teardown(&g_v[k]); continue; }
#endif
        int c=g_v[k].chn, g=g_v[k].grp, og=g_v[k].og;
        IMPCell f={DEV_ID_FS,c,0}, e={DEV_ID_ENC,g,0};
        td(IMP_FrameSource_DisableChn(c),"FrameSource_DisableChn");
        /* unbind the pairs that were REALLY bound, downstream pair first.
         * With OSD the pipeline is fs->osd->enc (M-1) - unbinding fs->enc
         * there would leave both real bindings in place and the Destroy
         * calls below would run against still-bound objects. */
        if (og>=0){
            had_osd=1;
            IMPCell o={DEV_ID_OSD,og,0};
            if (g_v[k].nbound>=2) td(IMP_System_UnBind(&o,&e),"System_UnBind osd->enc");
            if (g_v[k].nbound>=1) td(IMP_System_UnBind(&f,&o),"System_UnBind fs->osd");
        } else if (g_v[k].nbound>=1){
            td(IMP_System_UnBind(&f,&e),"System_UnBind fs->enc");
        }
        td(IMP_Encoder_UnRegisterChn(c),"Encoder_UnRegisterChn");
        td(IMP_Encoder_DestroyChn(c),"Encoder_DestroyChn");
        td(IMP_Encoder_DestroyGroup(g),"Encoder_DestroyGroup");
        td(IMP_FrameSource_DestroyChn(c),"FrameSource_DestroyChn");
    }
    g_nv=0;
    /* OSD groups/regions/fonts built by imp_osd_setup: destroy them AFTER
     * the unbinds above (a bound group must not be destroyed). imp_osd_stop
     * is global, skips streams that were never set up, and the updater
     * thread only starts after the loop, so there is nothing to join. */
    if (had_osd) imp_osd_stop();
    td_report("bring-up teardown");
    return -1;
}

static void ing_set_active(int src, int on)
{
    if (src==HUB_AUDIO_SRC){ g_aactive=on; }
    else if (src>=HUB_JPEG_SRC && src<HUB_JPEG_SRC+HUB_NJPEG){
        for (int i=0;i<g_nj;i++) if (g_j[i].src==src){ g_j[i].active=on; break; }
    } else {
        for (int i=0;i<g_nv;i++) if (g_v[i].chn==src){ g_v[i].active=on; break; }
    }
    if (on) act_wake();   /* unblock idle producer threads immediately */
}

static void ing_request_idr(int src)
{
    /* IMP_Encoder is not safe to touch from foreign threads. Just flag the
     * request; the owning video_thread issues the actual IMP_Encoder_RequestIDR
     * so each encoder channel is only ever accessed from its own thread. */
    for (int i=0;i<g_nv;i++) if (g_v[i].chn==src) g_v[i].idr_req=1;
}

static void ing_stop(void)
{
    g_td_nerr = 0;   /* see td()/td_report() */
    /* stop the IVS motion grid (uses the pinned channel recorded at start,
     * so a runtime monitor_stream change can never unpin the wrong FS) */
    pthread_mutex_lock(&g_motion_mtx);
    if (g_motion_pin >= 0){
        imp_motion_stop();
        fs_unuse(g_motion_pin);
        g_motion_pin = -1;
    }
    pthread_mutex_unlock(&g_motion_mtx);
    /* raise all stop flags first, then wake the idle-blocked threads once so
     * every join returns promptly (act_wait also times out after 1 s) */
    int had_audio = g_arun;
    for (int i=0;i<g_nv;i++) g_v[i].run=0;
    for (int i=0;i<g_nj;i++) g_j[i].run=0;
    g_arun=0;
    act_wake();
    /* join only slots whose pthread_create succeeded; the channels of
     * thread-less slots are still destroyed below (M8) */
    for (int i=0;i<g_nv;i++) if (g_v[i].has_thr) pthread_join(g_v[i].thr,NULL);
    if (had_audio) pthread_join(g_athr,NULL);
    for (int i=0;i<g_nj;i++) if (g_j[i].has_thr) pthread_join(g_j[i].thr,NULL);
    for (int i=0;i<g_nj;i++){
        jchan *jc=&g_j[i];
        if (jc->src==HUB_JPEG_SRC){
            /* dedicated channel: own framesource + own group */
            IMPCell fs={DEV_ID_FS,jc->chn,0}, enc={DEV_ID_ENC,jc->chn,0};
            td(IMP_FrameSource_DisableChn(jc->chn),"FrameSource_DisableChn");
            td(IMP_System_UnBind(&fs,&enc),"System_UnBind fs->enc");
            td(IMP_Encoder_UnRegisterChn(jc->chn),"Encoder_UnRegisterChn");
            td(IMP_Encoder_DestroyChn(jc->chn),"Encoder_DestroyChn");
            td(IMP_Encoder_DestroyGroup(jc->chn),"Encoder_DestroyGroup");
            td(IMP_FrameSource_DestroyChn(jc->chn),"FrameSource_DestroyChn");
        } else {
            /* piggyback: only the encoder channel; framesource and group
             * belong to the video stream and are torn down below */
            td(IMP_Encoder_UnRegisterChn(jc->chn),"Encoder_UnRegisterChn");
            td(IMP_Encoder_DestroyChn(jc->chn),"Encoder_DestroyChn");
        }
    }
    g_nj=0;

    /* unbind the pipelines FIRST, downstream pair before upstream pair: with
     * OSD the real bindings are fs->osd and osd->enc, not fs->enc (M-1), and
     * imp_osd_stop below destroys the OSD groups - they must be unbound by
     * then, as must the encoder/framesource channels destroyed after it. */
    int had_osd = 0;
    for (int i=0;i<g_nv;i++){
#ifdef ROT_HAS_SW_90
        if (g_v[i].sw_rot) continue;   /* nothing bound - torn down whole below */
#endif
        int chn=g_v[i].chn, grp=g_v[i].grp, og=g_v[i].og;
        IMPCell fs={DEV_ID_FS,chn,0}, enc={DEV_ID_ENC,grp,0};
        td(IMP_FrameSource_DisableChn(chn),"FrameSource_DisableChn");
        if (og>=0){
            had_osd=1;
            IMPCell osd={DEV_ID_OSD,og,0};
            td(IMP_System_UnBind(&osd,&enc),"System_UnBind osd->enc");
            td(IMP_System_UnBind(&fs,&osd),"System_UnBind fs->osd");
        } else {
            td(IMP_System_UnBind(&fs,&enc),"System_UnBind fs->enc");
        }
    }
    /* OSD groups also exist for privacy-only configs (osd.enabled==0), so key
     * the teardown off the recorded group ids, not just the master switch */
    if (g_hcfg->osd.enabled || had_osd) imp_osd_stop();

    for (int i=0;i<g_nv;i++){
#ifdef ROT_HAS_SW_90
        /* sw-rotate slot: FS disable/destroy + YuvExit + VbmFree, no encoder
         * chn/group ever existed (thread was joined above) */
        if (g_v[i].sw_rot){ sw_rot_teardown(&g_v[i]); continue; }
#endif
        int chn=g_v[i].chn, grp=g_v[i].grp;
        td(IMP_Encoder_UnRegisterChn(chn),"Encoder_UnRegisterChn");
        td(IMP_Encoder_DestroyChn(chn),"Encoder_DestroyChn");
        td(IMP_Encoder_DestroyGroup(grp),"Encoder_DestroyGroup");
        td(IMP_FrameSource_DestroyChn(chn),"FrameSource_DestroyChn");
    }
    g_nv=0;
#ifdef ROT_HAS_SW_90
    sw_osd_global_shutdown();   /* shared SW-OSD TTF (per-item fonts freed above) */
#endif
    td(IMP_System_Exit(),"System_Exit");
#if defined(PLATFORM_T40)||defined(PLATFORM_T41)
    td(IMP_ISP_DisableSensor(IMPVI_MAIN),"ISP_DisableSensor");
    td(IMP_ISP_DelSensor(IMPVI_MAIN,&g_sensor),"ISP_DelSensor");
#else
    td(IMP_ISP_DisableSensor(),"ISP_DisableSensor");
    td(IMP_ISP_DelSensor(&g_sensor),"ISP_DelSensor");
#endif
    td(IMP_ISP_DisableTuning(),"ISP_DisableTuning");
    td(IMP_ISP_Close(),"ISP_Close");
    td_report("teardown");
}

static const hal_backend g_ingenic = {
    .name="ingenic", .init=ing_init, .start=ing_start,
    .request_idr=ing_request_idr, .set_active=ing_set_active, .stop=ing_stop
};
const hal_backend *hal_get(void){ return &g_ingenic; }

/* daynight gain source: the ISP's own total gain (see hal.h). GetTotalGain is
 * absent from the T40/T41 new tuning API - return unavailable there so daynight
 * falls back to the /proc scrape. IMP returns <0 if the ISP is not yet up. */
int hal_isp_total_gain(uint32_t *gain)
{
#ifndef ISP_NEW_TUNING_API
    if (!gain) return -1;
    return IMP_ISP_Tuning_GetTotalGain(gain) < 0 ? -1 : 0;
#else
    (void)gain; return -1;
#endif
}

int hal_isp_ae_luma(uint32_t *luma)
{
#ifdef ISP_HAS_AELUMA
    if (!luma) return -1;
    int v = 0;                     /* IMP_ISP_Tuning_GetAeLuma takes int* */
    if (IMP_ISP_Tuning_GetAeLuma(&v) < 0) return -1;
    *luma = (uint32_t)v;
    return 0;
#else
    (void)luma; return -1;
#endif
}

/* Item-2: read-only encoder queue/buffer telemetry via IMP_Encoder_Query (all
 * 9 platforms). Fills *out and returns 0 on success; <0 (caller omits the
 * stats) when the query fails - e.g. a channel that doesn't exist (disabled
 * stream) or the SW-rotate path that has no encoder channel. On T31 the average
 * bitrate cached by the encode thread (GetChnAveBitrate) is attached too; on
 * every other platform, and before the first frame flows, ave_bitrate is <0. */
int hal_enc_stats(int enc_chn, hal_enc_stat *out)
{
    if (!out) return -1;
    IMPEncoderChnStat s;
    if (IMP_Encoder_Query(enc_chn, &s) != 0) return -1;
    out->registered         = s.registered ? 1u : 0u;
    out->left_pics          = s.leftPics;
    out->left_stream_bytes  = s.leftStreamBytes;
    out->left_stream_frames = s.leftStreamFrames;
    out->cur_packs          = s.curPacks;
    out->work_done          = s.work_done;
    out->ave_bitrate        = -1.0;
    out->au_drops           = 0;
    for (int i=0;i<g_nv;i++)
        if (g_v[i].chn == enc_chn){
            out->au_drops = g_v[i].au_drops;
#if defined(PLATFORM_T31)
            if (g_v[i].ave_valid) out->ave_bitrate = g_v[i].ave_bitrate;
#endif
            break;
        }
    return 0;
}

/* Rate-control readback (hal.h): what the encoder ACTUALLY holds, via
 * IMP_Encoder_GetChnAttrRcMode. Which union member is valid follows from the
 * rcMode read back plus (classic API) the channel's codec, so the codec is
 * looked up in g_v first. Read-only by construction - this call cannot
 * change encoder state on any platform. */
int hal_enc_rc_read(int enc_chn, hal_enc_rc *out)
{
    if (!out) return -1;
    int codec = -1;
    for (int i=0;i<g_nv;i++)
        if (g_v[i].chn == enc_chn){
#ifdef ROT_HAS_SW_90
            /* unbound Yuv encoder handle, no enc channel to query */
            if (g_v[i].sw_rot) return -1;
#endif
            codec = g_v[i].codec;
            break;
        }
    if (codec < 0) return -1;
    IMPEncoderAttrRcMode m; memset(&m,0,sizeof m);
    if (IMP_Encoder_GetChnAttrRcMode(enc_chn, &m) != 0) return -1;
    out->bitrate = out->max_bitrate = -1;
    out->rc_options = out->max_picture_size = -1;
    out->qp = out->min_qp = out->max_qp = HAL_RC_UNSET;
    out->i_bias_lvl = out->change_pos = out->quality_lvl = HAL_RC_UNSET;
    out->static_time = out->frm_qp_step = out->gop_qp_step = HAL_RC_UNSET;
    out->adaptive_mode = out->gop_relation = out->fluc_lvl = HAL_RC_UNSET;
    out->ip_delta = out->pb_delta = out->max_psnr = HAL_RC_UNSET;
#ifdef ENC_NEW_API
    (void)codec;   /* new-API rc structs are codec-agnostic */
    switch (m.rcMode){
    case IMP_ENC_RC_MODE_FIXQP:
        snprintf(out->mode,sizeof out->mode,"fixqp");
        out->qp = m.attrFixQp.iInitialQP;
        break;
    case IMP_ENC_RC_MODE_CBR:
        snprintf(out->mode,sizeof out->mode,"cbr");
        out->bitrate      = (long long)m.attrCbr.uTargetBitRate;
        out->qp           = m.attrCbr.iInitialQP;
        out->min_qp       = m.attrCbr.iMinQP;
        out->max_qp       = m.attrCbr.iMaxQP;
        out->ip_delta     = m.attrCbr.iIPDelta;
        out->pb_delta     = m.attrCbr.iPBDelta;
        out->rc_options   = (long long)m.attrCbr.eRcOptions;
        out->max_picture_size = (long long)m.attrCbr.uMaxPictureSize;
        break;
    case IMP_ENC_RC_MODE_VBR:
        snprintf(out->mode,sizeof out->mode,"vbr");
        out->bitrate      = (long long)m.attrVbr.uTargetBitRate;
        out->max_bitrate  = (long long)m.attrVbr.uMaxBitRate;
        out->qp           = m.attrVbr.iInitialQP;
        out->min_qp       = m.attrVbr.iMinQP;
        out->max_qp       = m.attrVbr.iMaxQP;
        out->ip_delta     = m.attrVbr.iIPDelta;
        out->pb_delta     = m.attrVbr.iPBDelta;
        out->rc_options   = (long long)m.attrVbr.eRcOptions;
        out->max_picture_size = (long long)m.attrVbr.uMaxPictureSize;
        break;
    case IMP_ENC_RC_MODE_CAPPED_VBR:
    case IMP_ENC_RC_MODE_CAPPED_QUALITY:
        snprintf(out->mode,sizeof out->mode,
                 m.rcMode==IMP_ENC_RC_MODE_CAPPED_VBR?"capped_vbr":"capped_quality");
        out->bitrate      = (long long)m.attrCappedVbr.uTargetBitRate;
        out->max_bitrate  = (long long)m.attrCappedVbr.uMaxBitRate;
        out->qp           = m.attrCappedVbr.iInitialQP;
        out->min_qp       = m.attrCappedVbr.iMinQP;
        out->max_qp       = m.attrCappedVbr.iMaxQP;
        out->ip_delta     = m.attrCappedVbr.iIPDelta;
        out->pb_delta     = m.attrCappedVbr.iPBDelta;
        out->rc_options   = (long long)m.attrCappedVbr.eRcOptions;
        out->max_picture_size = (long long)m.attrCappedVbr.uMaxPictureSize;
        out->max_psnr     = m.attrCappedVbr.uMaxPSNR;
        break;
    default:
        snprintf(out->mode,sizeof out->mode,"%d",(int)m.rcMode);
        break;
    }
#else /* classic API */
    switch (m.rcMode){
    case ENC_RC_MODE_FIXQP:
        snprintf(out->mode,sizeof out->mode,"fixqp");
#if !(defined(PLATFORM_T10)||defined(PLATFORM_T20))
        if (codec==MS_VC_H265) out->qp = (int)m.attrH265FixQp.qp;
        else
#endif
            out->qp = (int)m.attrH264FixQp.qp;
        break;
    case ENC_RC_MODE_CBR:
        snprintf(out->mode,sizeof out->mode,"cbr");
#if !(defined(PLATFORM_T10)||defined(PLATFORM_T20))
        if (codec==MS_VC_H265){
            out->max_qp       = (int)m.attrH265Cbr.maxQp;
            out->min_qp       = (int)m.attrH265Cbr.minQp;
            out->static_time  = (int)m.attrH265Cbr.staticTime;
            out->bitrate      = (long long)m.attrH265Cbr.outBitRate;
            out->i_bias_lvl   = (int)m.attrH265Cbr.iBiasLvl;
            out->frm_qp_step  = (int)m.attrH265Cbr.frmQPStep;
            out->gop_qp_step  = (int)m.attrH265Cbr.gopQPStep;
            out->fluc_lvl     = (int)m.attrH265Cbr.flucLvl;
        } else
#endif
        {
            out->max_qp        = (int)m.attrH264Cbr.maxQp;
            out->min_qp        = (int)m.attrH264Cbr.minQp;
            out->bitrate       = (long long)m.attrH264Cbr.outBitRate;
            out->i_bias_lvl    = (int)m.attrH264Cbr.iBiasLvl;
            out->frm_qp_step   = (int)m.attrH264Cbr.frmQPStep;
            out->gop_qp_step   = (int)m.attrH264Cbr.gopQPStep;
            out->adaptive_mode = (int)m.attrH264Cbr.adaptiveMode;
            out->gop_relation  = (int)m.attrH264Cbr.gopRelation;
        }
        break;
    case ENC_RC_MODE_VBR:
    case ENC_RC_MODE_SMART:
        snprintf(out->mode,sizeof out->mode,
                 m.rcMode==ENC_RC_MODE_SMART?"smart":"vbr");
        /* VBR and Smart unions are layout-identical per codec (see the fill
         * in enc_create) - one VBR-shaped read serves both */
#if !(defined(PLATFORM_T10)||defined(PLATFORM_T20))
        if (codec==MS_VC_H265){
            out->max_qp       = (int)m.attrH265Vbr.maxQp;
            out->min_qp       = (int)m.attrH265Vbr.minQp;
            out->static_time  = (int)m.attrH265Vbr.staticTime;
            out->bitrate      = (long long)m.attrH265Vbr.maxBitRate;
            out->i_bias_lvl   = (int)m.attrH265Vbr.iBiasLvl;
            out->change_pos   = (int)m.attrH265Vbr.changePos;
            out->quality_lvl  = (int)m.attrH265Vbr.qualityLvl;
            out->frm_qp_step  = (int)m.attrH265Vbr.frmQPStep;
            out->gop_qp_step  = (int)m.attrH265Vbr.gopQPStep;
            out->fluc_lvl     = (int)m.attrH265Vbr.flucLvl;
        } else
#endif
        {
            out->max_qp       = (int)m.attrH264Vbr.maxQp;
            out->min_qp       = (int)m.attrH264Vbr.minQp;
            out->static_time  = (int)m.attrH264Vbr.staticTime;
            out->bitrate      = (long long)m.attrH264Vbr.maxBitRate;
            out->i_bias_lvl   = (int)m.attrH264Vbr.iBiasLvl;
            out->change_pos   = (int)m.attrH264Vbr.changePos;
            out->quality_lvl  = (int)m.attrH264Vbr.qualityLvl;
            out->frm_qp_step  = (int)m.attrH264Vbr.frmQPStep;
            out->gop_qp_step  = (int)m.attrH264Vbr.gopQPStep;
            out->gop_relation = (int)m.attrH264Vbr.gopRelation;
        }
        break;
    default:
        snprintf(out->mode,sizeof out->mode,"%d",(int)m.rcMode);
        break;
    }
#endif /* ENC_NEW_API */
    return 0;
}

#if defined(USE_BACKCHANNEL) || defined(USE_PLAY)
/* ---- speaker output (IMP_AO) -------------------------------------------------
 * Sole owner of AO dev/chn 0. Brought up lazily by speaker.c on the first
 * backchannel or play request and torn down when both are idle, mirroring the
 * lazy IMP_AI capture lifecycle above. Mono int16 only (the AI/encoder side is
 * mono too); everything upstream resamples to the rate we report from open. */
static int g_ao_up   = 0;
static int g_ao_rate = 0;
static int g_ao_npf  = 0;   /* samples per SendFrame period (numPerFrm the AO was opened with) */
static int g_aec_on  = 0;   /* Item-3: IMP_AI_EnableAec actually issued (so teardown
                             * disables exactly what was enabled, never on a chn that
                             * had no AEC) */

static int ao_rate_enum(int sr)
{
    switch (sr){
        case 8000: case 16000: case 24000: case 32000:
        case 44100: case 48000: case 96000: return sr;
        default: return -1;
    }
}

int hal_ao_open(int want_rate)
{
    if (g_ao_up) return g_ao_rate;
    const int dev = 0, chn = 0;
    /* try the requested rate first, then 16k/8k - same fallback shape as the AI
     * bring-up, since a given codec/board may not accept every rate. */
    int want[3]; int nw = 0;
    if (ao_rate_enum(want_rate) > 0) want[nw++] = want_rate;
    if (want_rate != 16000) want[nw++] = 16000;
    if (want_rate != 8000)  want[nw++] = 8000;

    IMPAudioIOAttr aio;
    int ok = 0, rate = 0;
    for (int i = 0; i < nw && !ok; i++){
        int sr = want[i];
        memset(&aio, 0, sizeof aio);
        aio.samplerate = sr;
        aio.bitwidth   = AUDIO_BIT_WIDTH_16;
        aio.soundmode  = AUDIO_SOUND_MODE_MONO;
        aio.frmNum     = MS_AI_FRM_NUM;
        aio.numPerFrm  = sr * 40 / 1000;      /* 40 ms: 320@8k, 640@16k */
        aio.chnCnt     = 1;
        if (IMP_AO_SetPubAttr(dev, &aio) != 0){
            LOGW(MOD, "IMP_AO_SetPubAttr %dHz failed%s", sr, (i+1<nw)?" -> falling back":"");
            continue;
        }
        if (IMP_AO_Enable(dev) != 0){ LOGW(MOD, "IMP_AO_Enable failed"); continue; }
        if (IMP_AO_EnableChn(dev, chn) != 0){
            LOGW(MOD, "IMP_AO_EnableChn failed");
            IMP_AO_Disable(dev);
            continue;
        }
        rate = sr; ok = 1;
    }
    if (!ok){ LOGE(MOD, "audio output (speaker) unavailable"); return -1; }
    g_ao_up = 1; g_ao_rate = rate; g_ao_npf = rate * 40 / 1000;
    LOGI(MOD, "speaker (IMP_AO) up: %dHz mono", rate);

    /* Item-3: opt-in Acoustic Echo Cancellation (audio.aec, default off). Engage
     * only when BOTH directions are live - AEC subtracts the AO output from the
     * AI capture, so it needs the mic (AI dev0/chn0, g_ai_up) running as well as
     * the speaker we just brought up (AO dev0/chn0). If the AI isn't up yet (e.g.
     * a play-only session) AEC is skipped for this AO session. g_aec_on records
     * that we actually enabled it, so hal_ao_close disables exactly what we
     * enabled and never calls DisableAec on a chn that never had it. */
    int aec_on = 0;
    if (g_hcfg){ config_str_lock(); aec_on = g_hcfg->audio.aec; config_str_unlock(); }  /* F-02: cold read under lock */
    if (aec_on){
        /* Item-1: serialize the AEC module create against audio_thread's
         * concurrent GetFrame on the same AI dev0/chn0 (see g_ai_lock).
         *
         * Found by review: g_ai_up used to be read here BEFORE taking the
         * lock, then trusted for the IMP_AI_EnableAec call below it without
         * a re-check - exactly the TOCTOU window the Item-6/audio_thread
         * comments elsewhere in this file already describe (a disable
         * holds g_ai_lock across its whole clear-g_ai_up sequence
         * specifically so a caller blocked on the lock sees the up-to-date
         * value once it gets in). Re-checking g_ai_up under the lock closes
         * that window: either the disable already finished and this now
         * correctly skips, or it hasn't started and the channel is
         * genuinely still up. */
        pthread_mutex_lock(&g_ai_lock);
        if (g_ai_up) {
            int aec_rc = IMP_AI_EnableAec(0, 0, 0, 0);
            pthread_mutex_unlock(&g_ai_lock);
            if (aec_rc == 0){
                g_aec_on = 1;
                LOGI(MOD, "AEC enabled (AI 0/0 <- AO 0/0)");
            } else {
                LOGW(MOD, "IMP_AI_EnableAec failed - continuing without echo cancellation");
            }
        } else {
            pthread_mutex_unlock(&g_ai_lock);
        }
    }
    return rate;
}

int hal_ao_write(const int16_t *pcm, int nsamp)
{
    if (!g_ao_up || nsamp <= 0) return -1;
    /* Send in numPerFrm-sized periods: IMP_AO_SendFrame rejects a frame larger
     * than the channel's configured period (and a whole play-decode block,
     * e.g. 8192 samples resampled from a 4096-sample 8k mu-law read, dwarfs it).
     * Backchannel already delivers sub-period chunks, so it sends as one pass. */
    int npf = g_ao_npf > 0 ? g_ao_npf : nsamp;
    for (int off = 0; off < nsamp; ){
        int chunk = nsamp - off;
        if (chunk > npf) chunk = npf;
        IMPAudioFrame frm; memset(&frm, 0, sizeof frm);
        frm.bitwidth  = AUDIO_BIT_WIDTH_16;
        frm.soundmode = AUDIO_SOUND_MODE_MONO;
        frm.virAddr   = (uint32_t*)(pcm + off);      /* AO reads, never writes */
        frm.len       = chunk * (int)sizeof(int16_t);
        /* BLOCK: the AO's ring buffer backpressure is our playback clock, so a
         * producer that outruns real time is throttled here instead of overrunning. */
        if (IMP_AO_SendFrame(0, 0, &frm, BLOCK) != 0){
            LOGW(MOD, "IMP_AO_SendFrame failed");
            return -1;
        }
        off += chunk;
    }
    return 0;
}

void hal_ao_close(int drain)
{
    if (!g_ao_up) return;
    /* Item-3: tear down AEC before the AO it references, and only if we enabled
     * it (matches the AO/AI teardown symmetry: never disable on a chn where AEC
     * was never turned on). */
    if (g_aec_on){
        /* Item-1: DisableAec frees the AI-side AEC DSP module. audio_thread may
         * be mid-GetFrame on the same dev0/chn0, so serialize via g_ai_lock -
         * without it this free-while-recording races the capture thread ->
         * UAF/SIGSEGV in libaudioProcess.so (the same hazard HPF/AGC/NS avoid by
         * being boot-only; AEC can't be, it is AO-session-scoped). */
        pthread_mutex_lock(&g_ai_lock);
        IMP_AI_DisableAec(0, 0);
        pthread_mutex_unlock(&g_ai_lock);
        g_aec_on = 0;
        LOGI(MOD, "AEC disabled");
    }
    if (drain){
        /* IMP_AO_SendFrame(BLOCK) only waits for ring-buffer space, not for the
         * audio to actually reach the DAC. Worse, the AO keeps its own playback
         * cache on top of the MS_AI_FRM_NUM-period ring, so the residual still
         * queued when the last write returns is well over a ring's worth (~0.7s
         * on this board, not the ~0.24s a ring implies) - a fixed sleep sized to
         * the ring therefore still lops ~0.5s off a clip's tail. IMP_AO_FlushChnBuf
         * is the SDK's "wait for the last segment to finish playing" primitive
         * (present in every SoC's shipped libimp); it blocks until the whole
         * cache has actually played out, however deep it is. */
        IMP_AO_FlushChnBuf(0, 0);
    } else {
        /* ClearChnBuf (discard): the preempt/stop path, where backchannel
         * must take the speaker immediately - a queued play tail must not
         * delay it. */
        IMP_AO_ClearChnBuf(0, 0);
    }
    IMP_AO_DisableChn(0, 0);
    IMP_AO_Disable(0);
    g_ao_up = 0; g_ao_rate = 0; g_ao_npf = 0;
    LOGI(MOD, "speaker (IMP_AO) down");
}

void hal_ao_set_vol(int vol)
{
    if (!g_ao_up) return;
    /* map the play wrapper's 0..100 onto IMP_AO's [-30..120] (0.5 dB steps,
     * 60 = unity): 0 -> mute, 100 -> +30 dB. */
    int v = (vol <= 0) ? -30 : (vol >= 100) ? 120 : -30 + (int)(vol * 1.5);
    IMP_AO_SetVol(0, 0, v);
    LOGI(MOD, "IMP_AO_SetVol %d (spk_volume=%d)", v, vol);
}

void hal_ao_set_gain(int gain)
{
    if (!g_ao_up) return;
    if (gain < 0) gain = 0;
    if (gain > 31) gain = 31;
    IMP_AO_SetGain(0, 0, gain);
    LOGI(MOD, "IMP_AO_SetGain %d", gain);
}
#endif /* USE_BACKCHANNEL || USE_PLAY */
#endif /* HAL_INGENIC */
