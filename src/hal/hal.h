/* hal.h - hardware abstraction. A backend captures + encodes and publishes
 * encoded access units into the hub. Backends: ingenic (real SoC) / sim (x86). */
#ifndef MS_HAL_H
#define MS_HAL_H
#include "../config.h"

typedef struct {
    const char *name;
    int  (*init)(const ms_config *cfg);   /* open ISP/sensor/system */
    int  (*start)(const ms_config *cfg);  /* create channels (idle until demanded) */
    void (*request_idr)(int src);         /* force keyframe on a video source */
    void (*set_active)(int src, int on);  /* on-demand: start/stop a source's encode */
    void (*stop)(void);                   /* stop + teardown */
} hal_backend;

/* A video/encode worker sets this just before raise(SIGTERM) when IT decided
 * the process must exit (the watchdog's recovery budget is spent, or
 * StartRecvPic never recovers) - as opposed to an operator- or init-system-
 * requested SIGTERM, which leaves it 0. main(), once its normal orderly
 * shutdown finishes, checks it to tell the two apart: only a HAL give-up
 * escalates to the same one-shot recovery reboot bring-up failures already
 * get (see startup_give_up() in main.c) - a plain `S95timps stop` must never
 * reboot the camera. Set from the HAL thread, read from main() after that
 * thread's raise(SIGTERM) has already joined every subsystem via the normal
 * shutdown path, so there is no concurrent writer left by the time it is
 * read; plain int is enough. */
extern volatile int g_hal_watchdog_gave_up;

/* returns the backend selected at compile time */
const hal_backend *hal_get(void);

/* general.fs_keepalive as resolved at start(): 1 = the video framesources are
 * held enabled until stop(). The sim reports the resolution, it has no FS. */
int hal_fs_keepalive(void);

/* Picture consumers that do not go through the hub: the motion pin and a
 * configured jpeg.snapshot_path (counted for good, its grab is periodic).
 * Lock-free, so the day/night thread can poll it every tick. The sim reads
 * the count from the file named by TIMPS_SIM_WATCH (dn-replay fixture). */
int hal_image_pinned(void);

/* ISP total gain for day/night detection: the ISP's own overall gain
 * (IMP [24.8] linear, 256 = 1x - the value prudynt/raptor plot), read straight
 * from the IMP API instead of scraping /proc/jz/isp/isp-m0. Returns 0 and fills
 * *gain on success, <0 when unavailable (sim, T40/T41 new-tuning API, or the
 * ISP is not initialised) - daynight then falls back to the proc scrape. */
int hal_isp_total_gain(uint32_t *gain);

/* ISP AE average luminance (raptor's ae_luma) - a secondary day/night metric.
 * IMP_ISP_Tuning_GetAeLuma exists on T21/T23/T31/C100 only; returns 0 and fills
 * *luma on success, <0 when unavailable (other SoCs, sim, ISP down). */
int hal_isp_ae_luma(uint32_t *luma);
/* WB gains in effect (in auto: what AWB currently applies), SetWB's scale.
 * -1 where the SDK has no IMPISPWB (ISP_HAS_WB unset). */
int hal_isp_wb_gains(int *rgain, int *bgain);
/* Scene colour from the AWB statistics: global R/G and B/G in a fixed scale
 * per platform (only the change between two reads counts). 0 ok, <0 when
 * this libimp has none or the ISP is not up. */
int hal_isp_awb_ratio(uint32_t *rg, uint32_t *bg);

/* ISP AE exposure readback, straight from IMP_ISP_Tuning_GetExpr (+GetEVAttr).
 *
 * The integration-time half of daynight's exposure index used to have no IMP
 * accessor at all, so it came from scraping /proc/jz/isp/isp-m0 (or isp_info on
 * the T20s) - and that dump publishes no MAXIMUM on the older SDKs, which is
 * why dn_read() carries a high-water-mark estimate for it. GetExpr publishes
 * integration_time / _min / _max in sensor lines AND one_line_expr_in_us, so
 * the real maximum and a microsecond conversion are both available directly.
 *
 * Present on every classic-tuning SoC (T10..T31, C100); not in the T40/T41
 * reworked tuning API - see ISP_HAS_EXPR in isp_caps.h. Returns 0 and fills
 * *out on success, <0 when unavailable (T40/T41, sim, ISP not up, or the SDK
 * rejected the call). Fields the SDK could not supply are left 0.
 *
 * NOTE the SDK's own units: lines are sensor lines for the CURRENT sensor mode
 * and frame rate, so it_lines is only comparable against it_max_lines from the
 * same sample - never cached across a mode change. */
typedef struct {
    uint32_t it_lines;      /* g_attr.integration_time      (sensor lines) */
    uint32_t it_min_lines;  /* g_attr.integration_time_min  (sensor lines) */
    uint32_t it_max_lines;  /* g_attr.integration_time_max  (sensor lines) */
    uint32_t line_us;       /* g_attr.one_line_expr_in_us; 0 = not supplied */
    uint32_t expr_us;       /* GetEVAttr.expr_us;   0 = GetEVAttr unavailable */
    uint32_t again;         /* GetEVAttr.again      (SDK's own units) */
    uint32_t dgain;         /* GetEVAttr.dgain      (SDK's own units) */
} hal_isp_expo;
int hal_isp_exposure(hal_isp_expo *out);

/* AE state from IMP_ISP_Tuning_GetAeExprInfo (T41 open stack, ISP_HAS_AE_EXPR_INFO).
 * again_max is the AE's effective ceiling (the configured cap if one is set),
 * unlike the sensor maximum /proc prints. <0 = unavailable (other builds, sim
 * without a fixture, or before the first AE run). */
typedef struct {
    uint32_t it, it_min, it_max;   /* sensor lines */
    uint32_t again, again_max;     /* Q10, 1024 = 1x */
    uint32_t total_log2_q16;       /* TotalGainDb: log2(total gain) in Q16 */
} hal_isp_ae;
int hal_isp_ae_info(hal_isp_ae *out);

/* Encoder ROI / ISP FrontCrop (roi_caps.h): the CAP_TIER_* this build AND
 * this libimp allow (a missing weak setter reads UNSUPPORTED), the per-stream
 * ROI state (roi_state_name()) and the FrontCrop state ("off", "on",
 * "rejected", "failed", "unsupported"). hal_sensor_size() is the frame the
 * FrontCrop window lives in. */
int hal_roi_tier(void);
const char *hal_roi_state(int si);
int hal_fcrop_tier(void);
/* caps.fcrop.live: image.fcrop_* applies to the running pipeline */
int hal_fcrop_live(void);
const char *hal_fcrop_state(void);
void hal_sensor_size(int *w, int *h);
/* bitmask of stream si's ROI regions refused as outside the picture */
unsigned hal_roi_rejected(int si);
/* caps.fcrop limits: upscale_pct is the T31/T23 driver's fcrop_upscale_pct
 * (-1 not applicable or not readable), min_w/min_h the smallest window the
 * driver takes, zoom_max_x100 the zoom it allows against the full sensor,
 * zoomable/zoomed per-stream bitmasks (can show a window at all / shows the
 * current one), restart: streams that take the current window only at the
 * next timps start (T41 AutoZoom on a kept large framesource). */
typedef struct {
    int upscale_pct;
    int upscale_max;   /* image.fcrop_upscale_pct ceiling, -1 unsupported */
    int min_w, min_h;
    int zoom_max_x100;
    unsigned zoomable, zoomed, restart;
} hal_fcrop_lim;
void hal_fcrop_limits(hal_fcrop_lim *o);

/* Rotation refusals as ERR_ROT_* (err_codes.h), 0 = none. hal_rot_state: why
 * stream si runs unrotated against its request since the last start.
 * hal_rot_check: what the next start would refuse for v. */
int hal_rot_state(int si);
/* 0: the libimp cannot rotate (T40/T41 OpenIMP without I2D) */
int hal_rot_hw_ok(void);

/* OpenIMP test pattern (OpenIMP_TestPattern_Set/Get); -1 fields = default */
typedef struct {
    int obj_size, obj_contrast, obj_speed, obj_cell_x, obj_cell_y;
    const char *script;   /* mode 3 object script, NULL = built-in object */
    int ae_hold, awb_hold;   /* mode 4, -1 = default */
} hal_tp_params;
typedef struct {
    int mode;
    uint64_t frames, objects_shown;
    unsigned objects_active;
    int obj_cell_x, obj_cell_y;   /* built-in object cell in the 8 x 6 grid */
    int scripted;                 /* objects come from a script file */
    int ae_hold, awb_hold;
} hal_tp_state;
int hal_tp_available(void);
int hal_tp_set(int mode, const hal_tp_params *p);   /* p NULL: keep parameters */
int hal_tp_get(hal_tp_state *o);
/* 1: the libimp can run this mode now (OpenIMP_TestPattern_Supported); 0 also
 * when the libimp lacks the query */
int hal_tp_supported(int mode);
/* mode 4: the running sensor has a built-in pattern; name/desc may be NULL */
int hal_tp_sensor(char *name, size_t cap, const char **desc);
int hal_rot_check(int si, const ms_vstream_cfg *v);
/* How the running stream si is rotated: "off", "sw" (T23 software path:
 * text OSD only), "hw" (OSD/privacy over the whole frame) or "band" (vendor
 * libimp: OSD/privacy only in the top eff_width rows). */
const char *hal_rot_path(int si);

/* 1 when image.<key> is built in but this libimp lacks its (weak, OpenIMP-only)
 * setter. Defined only where isp_caps.h sets ISP_WEAK_KEYS. */
int hal_isp_key_missing(const char *key);

/* Read-only encoder queue/buffer telemetry for one encoder channel, from
 * IMP_Encoder_Query (IMPEncoderChnStat/CHNStat, present on all 9 platforms).
 * All counts are instantaneous. On T31, ave_bitrate is additionally filled from
 * IMP_Encoder_GetChnAveBitrate (T31-exclusive; the raw SDK value, cached by the
 * encode thread) - on every other platform, and on T31 before the first frame
 * has flowed, it is left <0 (unavailable). hal_enc_stats() returns 0 and fills
 * *out on success, <0 when the query failed or the backend has no encoder
 * (host sim) - the caller must then OMIT the stats rather than emit zeros. */
typedef struct {
    unsigned registered;         /* channel registered to its encode group */
    unsigned left_pics;          /* leftPics: images still to encode */
    unsigned left_stream_bytes;  /* leftStreamBytes: bytes left in stream buffer */
    unsigned left_stream_frames; /* leftStreamFrames: frames left in stream buffer */
    unsigned cur_packs;          /* curPacks: stream packets in the current frame */
    unsigned work_done;          /* work_done: 0 = running, 1 = not running */
    double   ave_bitrate;        /* T31 IMP_Encoder_GetChnAveBitrate, else <0 */
    unsigned au_drops;           /* CUMULATIVE producer-side frame drops (AU >
                                  * MS_AU_BUF_MAX, pool OOM, assembly overflow) -
                                  * the events behind the throttled "dropping
                                  * frame" log, previously invisible between
                                  * every 20th line */
    int      tel;                /* 1: the fields below are filled
                                  * (OpenIMP_Encoder_GetTelemetry, T40/T41) */
    unsigned long long tel_frames, tel_bytes, tel_idr;
    unsigned tel_fps_x100, tel_kbps, tel_enc_us_avg, tel_enc_us_max;
} hal_enc_stat;
int hal_enc_stats(int enc_chn, hal_enc_stat *out);

/* Read-only rate-control attributes the encoder ACTUALLY holds for one
 * channel, from IMP_Encoder_GetChnAttrRcMode (present on every supported
 * SoC). This is the missing diagnostic for the whole rc-key family: timps
 * has always written these attrs at bring-up and never once verified they
 * arrive unaltered. Values are the raw SDK readback, NOT g_cfg - compare
 * them against the configured videoN.* to tell "written" from "held".
 *
 * Only the fields the encoder's CURRENT mode/API carries are filled; the
 * rest stay HAL_RC_UNSET (an out-of-band sentinel: several fields are
 * legitimately negative). On the classic API, bitrate is maxBitRate (VBR/
 * Smart) or outBitRate (CBR), unit kbps per the SDK header. On the
 * ENC_NEW_API SoCs, bitrate/max_bitrate are uTargetBitRate/uMaxBitRate in
 * whatever unit the SDK stores internally - NOT verified to be kbps; that
 * open question is one of the things this readback exists to answer.
 * Returns 0 on success, <0 when the channel has no queryable encoder
 * (disabled stream, T23 sw-rotate path, host sim). */
#define HAL_RC_UNSET (-1000000)
typedef struct {
    char mode[16];               /* rc mode held, timps token spelling */
    long long bitrate;           /* classic maxBitRate/outBitRate; new API uTargetBitRate */
    long long max_bitrate;       /* new API uMaxBitRate (VBR/capped modes) */
    int  qp;                     /* fixqp qp / new API iInitialQP */
    int  min_qp, max_qp;
    int  i_bias_lvl, change_pos, quality_lvl;      /* classic */
    int  static_time, frm_qp_step, gop_qp_step;    /* classic */
    int  adaptive_mode, gop_relation, fluc_lvl;    /* classic H264/H265 */
    int  ip_delta, pb_delta;     /* new API iIPDelta/iPBDelta */
    long long rc_options;        /* new API eRcOptions bitmask */
    long long max_picture_size;  /* new API uMaxPictureSize */
    int  max_psnr;               /* new API capped modes uMaxPSNR */
} hal_enc_rc;
int hal_enc_rc_read(int enc_chn, hal_enc_rc *out);

#if defined(USE_BACKCHANNEL) || defined(USE_PLAY)
/* Speaker output (IMP_AO). The HAL is the sole owner of the AO device; speaker.c
 * (backchannel + play queue) drives these, opening lazily on first use and
 * closing when idle, mirroring the IMP_AI capture lifecycle. */

/* Bring up AO dev/chn 0 at (or near) want_rate. Returns the sample rate the AO
 * was actually programmed at (>0, may differ if want_rate was unsupported and a
 * fallback was used) or -1 on failure. Idempotent while open (returns the live
 * rate). */
int  hal_ao_open(int want_rate);

/* Send nsamp mono int16 samples (at the rate hal_ao_open returned). Blocks with
 * the AO's own buffering as backpressure. Returns 0 on success, <0 on error. */
int  hal_ao_write(const int16_t *pcm, int nsamp);

/* Tear down AO dev/chn 0. Idempotent (no-op if not open). drain=1: wait for
 * the ring buffer to actually finish playing out before disabling (use at
 * the natural end of a clip). drain=0: discard whatever is still queued and
 * disable immediately (use when preempting/stopping - e.g. backchannel must
 * take the speaker without waiting out a play tail). */
void hal_ao_close(int drain);

void hal_ao_set_vol(int vol);    /* IMP_AO_SetVol, clamped to the SDK range */
void hal_ao_set_gain(int gain);  /* IMP_AO_SetGain, clamped to the SDK range */
#endif

#endif
