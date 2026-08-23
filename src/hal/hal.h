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

/* returns the backend selected at compile time */
const hal_backend *hal_get(void);

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
