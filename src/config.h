/* config.h - minimal key=value config (replaces libconfig) */
#ifndef MS_CONFIG_H
#define MS_CONFIG_H

#include <stdint.h>
#include <stddef.h>   /* size_t */

#define MS_MAX_VSTREAM 2
#define MS_MAX_OSD     8
#define MS_MAX_PRIVACY 4
#define MS_MAX_STR     64

enum ms_vcodec { MS_VC_H264=0, MS_VC_H265=1 };
enum ms_acodec { MS_AC_NONE=0, MS_AC_AAC=1, MS_AC_PCMU=2, MS_AC_PCMA=3,
                 MS_AC_OPUS=4 /* RTSP/RTP streaming codec, USE_STREAM_OPUS only */ };
enum ms_rcmode { MS_RC_CBR=0, MS_RC_VBR, MS_RC_FIXQP, MS_RC_SMART,
                 MS_RC_CAPPED_VBR, MS_RC_CAPPED_QUALITY };
enum ms_osd_type { MS_OSD_TEXT=0, MS_OSD_LOGO=1 };

typedef struct {
    int      enabled;
    int      codec;          /* enum ms_vcodec */
    int      width, height;
    int      fps;
    int      bitrate_kbps;
    int      rc_mode;        /* enum ms_rcmode */
    int      gop;
    int      max_gop;
    int      profile;        /* 0 baseline,1 main,2 high */
    int      qp;             /* fixed qp / init qp */
    int      min_qp, max_qp;
    int      rotation;       /* 0,90,270 (+180 only on T40/T41 per-channel I2D;
                              * 180 removed elsewhere - use image.hflip+vflip) */
    int      buffers;        /* IMP nrVBs */
    int      buffers_explicit; /* 1 if "buffers" was set in timps.conf, so HAL
                                 * safety clamps (e.g. T31 non-scaled channel)
                                 * should trust it as-is instead of overriding */
    char     rtsp_path[MS_MAX_STR];
    int      imp_chn;        /* encoder channel */
    /* optional extra JPEG encoder piggybacked on this stream: it is
     * registered into the SAME encoder group, so it shares the stream's
     * FrameSource (no additional rmem for video buffers) and produces
     * JPEGs at this stream's resolution. Default: off. */
    int      jpeg_enabled;
    int      jpeg_quality;   /* 1..100 */
    int      jpeg_fps;       /* max snapshot/MJPEG publish rate */
    int      jpeg_chn;       /* IMP encoder channel (must be unique) */
} ms_vstream_cfg;

/* Effective (post-rotation) frame dimensions for a video stream: a 90/270
 * rotation swaps width/height, everything else keeps the raw dims. All
 * downstream consumers (encoder attrs, OSD, hub, muxers, JPEG piggyback) use
 * this instead of raw v->width/height so the geometry stays consistent once an
 * apply path is enabled. With rotation==0 (the default) eff dims == raw dims. */
static inline void ms_vstream_eff_dims(const ms_vstream_cfg *v, int *w, int *h){
    if (v->rotation==90 || v->rotation==270){ *w=v->height; *h=v->width; }
    else                                    { *w=v->width;  *h=v->height; }
}

typedef struct {
    int      enabled;
    int      codec;          /* enum ms_acodec */
    int      samplerate;
    int      channels;
    int      bitrate_kbps;   /* for aac */
    int      volume;         /* IMP_AI_SetVol level */
    int      gain;           /* IMP_AI_SetGain level */
    int      high_pass;      /* HPF on/off */
    int      agc;            /* automatic gain control on/off */
    int      ns;             /* noise suppression: 0 off, 1..3 = level */
    int      alc_gain;       /* IMP_AI_SetAlcGain PGA level 0..7 (T21/T31/C100) */
    int      agc_target_dbfs;    /* AGC TargetLevelDbfs 0..31 */
    int      agc_compression_db; /* AGC CompressionGaindB 0..90 */
    _Atomic int mute;        /* live mic mute: 1 = captured frames are dropped
                              * before the encoder/hub (no audio to any client);
                              * toggled at runtime via /control, no restart.
                              * _Atomic: the HAL/sim audio worker reads it
                              * lock-free once per captured frame (hal_ingenic.c
                              * ai loop / hal_sim.c), a DIFFERENT thread from the
                              * /control writer, so a plain int would be a C11
                              * data race (a load the compiler could hoist out of
                              * the frame loop). Written atomically via F_ATOMIC
                              * in config.c's field_set(). */
    /* persist-only (no runtime path in timps yet): kept so the WebUI audio
     * page can store them; capture is mono and there is no AO pipeline */
    int      force_stereo;
    int      spk_enabled;
    int      spk_volume;
    int      spk_gain;
    /* ONVIF audio backchannel (client -> speaker via /bin/iac). Compiled only
     * when USE_BACKCHANNEL; decode is pure-C G.711, AAC needs USE_BC_AAC. */
    int      backchannel;        /* master on/off for the RTSP backchannel */
    int      backchannel_codec;  /* 0=PCMU 1=PCMA 2=AAC (advertised format) */
    int      backchannel_rate;   /* speaker sample rate fed to iac (Hz) */
    /* Acoustic Echo Cancellation for the backchannel (IMP_AI_EnableAec):
     * opt-in, default OFF - it engages a vendor DSP path whose quality/latency
     * varies per SoC/mic/speaker pairing. Applied at the next AO open (like
     * spk_volume/spk_gain), only while both AI capture and AO output are live. */
    int      aec;
} ms_audio_cfg;

typedef struct {
    char     model[MS_MAX_STR];
    int      i2c_addr;
    int      fps;
    int      width, height;
} ms_sensor_cfg;

/* JPEG / MJPEG snapshot stream (the old streamer's "stream2") */
typedef struct {
    int      enabled;
    int      width, height;
    int      quality;               /* 1..100 */
    int      fps;                   /* max MJPEG frame rate */
    int      imp_chn;               /* JPEG encoder channel */
    char     snapshot_path[128];    /* periodic file snapshot ("" = none) */
} ms_jpeg_cfg;

/* ISP image tuning (mirrors the old streamer's image.* block) */
typedef struct {
    int      brightness, contrast, saturation, sharpness, hue;
    int      vflip, hflip;
    int      running_mode;          /* 0 day, 1 night */
    int      anti_flicker;          /* 0 disable,1 50Hz,2 60Hz */
    int      ae_compensation;
    int      max_again, max_dgain;
    int      sinter_strength, temper_strength, dpc_strength;
    int      defog_strength, drc_strength;
    int      highlight_depress, backlight_compensation;
    int      core_wb_mode, wb_rgain, wb_bgain;
} ms_image_cfg;

/* one OSD overlay: text (with placeholders) or a BGRA logo */
typedef struct {
    int      enabled;
    int      type;                  /* enum ms_osd_type */
    char     text[128];             /* template: {ph} + strftime %.. */
    char     logo_path[128];        /* raw BGRA logo file */
    int      logo_w, logo_h;
    int      x, y;                  /* position; negative = from right/bottom edge */
    int      font_size;
    uint32_t color;                 /* 0xAARRGGBB */
    int      transparency;          /* 0..255 group alpha */
    int      outline;               /* text outline width in px (0 = off) */
    uint32_t outline_color;         /* 0xAARRGGBB outline/stroke color */
    char     font_path[128];        /* optional per-item TTF override */
} ms_osd_item;

/* OSD config. Each video stream has its OWN independent item set
 * (items[stream][item]); the master switch, monitor stream and the font/vars
 * paths stay global.
 * Config keys:
 *   canonical (per stream): osd<S>.<N>.<field>   e.g. osd0.0.text (stream 0,
 *     item 0), osd1.3.enabled (stream 1, item 3)
 *   legacy (pre-per-stream): osd<N>.<field>      e.g. osd0.text - still
 *     parsed for backward compatibility and applied to item N on EVERY
 *     stream (old configs keep drawing the same overlays on both streams) */
typedef struct {
    int         enabled;            /* master switch (global, restart) */
    int         monitor_stream;     /* stream whose fps feeds the {fps} var */
    char        font_path[128];     /* default TTF for text items */
    char        vars_file[128];     /* custom placeholder source (e.g. /tmp/..) */
    int         supersample;        /* TTF rasterizer AA quality: samples/axis
                                      * per pixel (1-4, default 2). Cost scales
                                      * ~quadratically (4=16 samples/px, 2=4);
                                      * 2 is visually indistinguishable from 4
                                      * at typical OSD text sizes but roughly
                                      * halves the rasterizer's CPU cost. */
    int         hinting;            /* opt-in lightweight geometric autohint
                                      * (0/1, default 0 = off): snaps stem-like
                                      * outline edges to the pixel grid before
                                      * rasterizing, to reduce the uneven stroke
                                      * widths unhinted glyphs show at small
                                      * (e.g. substream 12px) OSD sizes. NOT a
                                      * TrueType hint-bytecode interpreter -
                                      * see msttf_set_hinting(). Off by default
                                      * so existing installs render byte-for-
                                      * byte identical OSD bitmaps unless this
                                      * is explicitly enabled. Applied once via
                                      * msttf_set_hinting() in imp_osd_setup(),
                                      * same as osd.supersample - File-only,
                                      * takes effect on restart. */
    ms_osd_item items[MS_MAX_VSTREAM][MS_MAX_OSD];  /* per-stream item sets */
} ms_osd_cfg;

/* privacy mask: a solid filled rectangle drawn over the video (IMP OSD cover
 * region) to black out a sensitive area. Each video stream has its own set of
 * up to MS_MAX_PRIVACY regions. Applied LIVE via /control (created/shown/moved
 * at runtime, like OSD items) - no restart required.
 * Config keys (per stream): privacy<S>.<N>.<field>, e.g. privacy0.0.enabled,
 * privacy1.2.w. Fields: enabled, x, y, w, h, color (0xAARRGGBB fill). */
typedef struct {
    int      enabled;
    int      x, y, w, h;            /* rect in the stream's frame, pixels */
    uint32_t color;                 /* 0xAARRGGBB fill color */
} ms_privacy_region;

/* native automatic day/night detection (thread compiled with -DUSE_DAYNIGHT;
 * keys are always parsed so a config with daynight.* loads warning-free).
 * See dev_notes/DAYNIGHT_REDESIGN_2026-08-17.md for the design this
 * implements, and docs/wiki/Day-Night-Design-Notes.md for the incident
 * record it replaces.
 *
 * The decision metric is the EXPOSURE INDEX, not bare gain:
 *
 *     D = total_gain * (integration_time / max_integration_time)
 *
 * higher = darker. In a dark scene the AE has the integration time railed at
 * max, so D == total_gain and every threshold keeps its historic meaning. In
 * a BRIGHT scene the gain rails at its 256 (1.0x) floor and the AE keeps
 * shortening the exposure instead - which bare gain cannot see at all. That
 * blindness at the bright end is what made an IR-saturated scene (a camera
 * close to a reflective object rests at gain 256-268 under its own
 * illuminator) structurally undetectable, and it is why several of the
 * historic scheduling hacks existed. When the integration-time fields are
 * unreadable D degrades to total_gain, i.e. to the old behaviour. */
/* day/night decision source (daynight.mode).
 *   AUTO     - the full automaton: honest day-pipeline measurement for
 *              day->night, probe-mediated night->day, calendar (when
 *              configured) only as a probe scheduler. This is the default and
 *              works without any location data.
 *   SCHEDULE - the calendar decides outright: no sensor, no probes, zero
 *              IR-cut clicks beyond the two real transitions per day.
 * mode only matters when enabled=1; enabled=0 (manual) suppresses forcing in
 * both. Parsed as a string token at the config-file and JSON boundary; the
 * pre-2026-08-17 tokens are still accepted ("sensor" -> auto, "time"/"sun" ->
 * schedule, since the calendar source is now auto-selected, see below). */
enum { DN_MODE_AUTO = 0, DN_MODE_SCHEDULE = 1 };

typedef struct {
    int      enabled;            /* 0 = manual mode (thread idles) */
    int      mode;               /* DN_MODE_AUTO / DN_MODE_SCHEDULE, see above */
    /* THE CALENDAR. Optional in AUTO (where it only sharpens the heartbeat
     * schedule), required in SCHEDULE. The source is auto-selected: an
     * explicit time window wins, else lat/lon if either is non-zero, else
     * there is no calendar at all and AUTO falls back to the plain heartbeat.
     * A fixed local-clock window: night from time_night_start until
     * time_day_start, may wrap past midnight (night 20:00, day 06:30).
     * Empty = that edge is unset. */
    char     time_night_start[6];  /* "HH:MM" local: night at/after */
    char     time_day_start[6];    /* "HH:MM" local: day   at/after */
    /* ... or today's real sunrise/sunset for a location, each nudged by an
     * offset in minutes (may be negative). */
    float    sun_latitude;         /* degrees, +N / -S */
    float    sun_longitude;        /* degrees, +E / -W */
    int      sun_sunrise_offset_min; /* minutes added to sunrise before -> day */
    int      sun_sunset_offset_min;  /* minutes added to sunset  before -> night */
    /* THE TWO THRESHOLDS. Both are absolute exposure-index levels and both
     * are only ever evaluated on the DAY pipeline, which is the only optical
     * path that reports ambient light honestly (in night the IR-cut is open
     * and the illuminator lights the scene itself). Night-pipeline readings
     * are used exclusively as a RELATIVE change detector, never against a
     * threshold - which is why there is no dead-zone and no undecided
     * outcome any more: day_gain answers "is it day", night_gain answers
     * "has day ended", and the gap between them is plain hysteresis.
     * Old key names total_gain_day_threshold / total_gain_night_threshold
     * still parse (aliases) - the units are unchanged. */
    float    day_gain;           /* D below this (day pipeline) = day    */
    float    night_gain;         /* D above this (in day)       = night  */
    int      day_confirm_s;      /* how long D must exceed night_gain    */
    /* PROBE ECONOMY - one rate limit, one trigger, one safety net. A probe
     * is the only way from night to day: switch the board to the day
     * pipeline, wait DN_PROBE_SETTLE_S for the AE, then judge once against
     * day_gain and either stay or fall straight back. Two things can ask for
     * one, and probe_min_gap_s bounds the audible cost of both together, so
     * the worst-case click rate is a property of the configuration rather
     * than of nine interacting heuristics.
     *
     * probe_jump_pct (trigger the probe below this % of the night reference)
     * and probe_settle_s (AE settle before the verdict) were per-camera
     * fields here until the 2026-08-22 consolidation - see
     * DN_PROBE_JUMP_PCT/DN_PROBE_SETTLE_S in daynight.h for why they are now
     * fixed constants instead. */
    int      probe_min_gap_s;    /* no two probes closer than this        */
    int      probe_confirm_s;    /* ... and stays there this long          */
    /* THE SILENT PROBE (2026-08-18). The IR illuminator is wired to its own
     * GPIO on every camera measured, independent of the IR-cut motor, so it
     * can be switched off for a few seconds without an audible click and
     * without moving anything. That turns the interference into the
     * measurement:
     *
     *     r = D(illuminator off) / D(illuminator on)
     *
     * r >> 1  removing the IR made the scene much darker: the illuminator was
     *         doing the work, so it is genuinely night.
     * r ~= 1  removing it changed nothing: the room supplies the light, so it
     *         is day - whatever the absolute level happens to be.
     *
     * That last clause is the whole reason this exists. Genuine daylight
     * measured across this fleet at ONE instant spanned a factor of 63, and a
     * single camera swung by 4.5x within one overcast morning, so no absolute
     * day threshold can be right everywhere. Two measured events settle it:
     * an outbuilding light produced a day-pipeline reading of ~3700 and a
     * cellar light 1024, both far above any workable default - an absolute
     * rule calls those lit rooms "night". r is dimensionless and needs no
     * per-camera calibration.
     *
     * irprobe_cmd is run as "<cmd> on|off" via fork+execlp, never a shell,
     * exactly like switch_cmd. Empty (the default) disables the silent probe
     * entirely and every night->day question falls back to the audible
     * IR-cut probe - which is what boards without separate illuminator
     * control must do.
     *
     * The verdict thresholds (ir_ratio_night/ir_ratio_day) and the minimum AE
     * reserve for the ratio to mean anything (ir_min_headroom) were
     * per-camera fields here until the 2026-08-22 consolidation, when the
     * fleet-wide campaign that set them (see DN_IR_RATIO_NIGHT in
     * daynight.h) showed every camera wanted the same values. A reading
     * between the two thresholds is not a verdict: it means the ratio could
     * not answer, and the audible probe decides instead. */
    char     irprobe_cmd[64];
    /* THE HEARTBEAT - the sensor-independent safety net, and the ONLY bound
     * on how long a wrong night can last. heartbeat_s applies while the
     * scene is moving; a scene that has not moved measurably since the last
     * probe waits heartbeat_max_s instead (there is no new evidence, so
     * there is nothing to spend a click on). Deliberately NOT a multiplying
     * backoff: an interval that doubles per failure is exactly how the
     * previous design turned a bounded guarantee into an unbounded one. */
    int      heartbeat_s;
    int      heartbeat_max_s;
    /* Boot: the persisted running_mode is a guess about a scene nobody
     * measured. If it says day we are already in the honest pipeline and one
     * reading settles it for free; if it says night, a single probe turns the
     * guess into a measurement (one IR-cut click per boot). 0 disables that
     * probe - the first heartbeat then does the same job, later.
     * boot_settle_s (minimum settle before that first decision) was a field
     * here until the 2026-08-22 consolidation - see DN_BOOT_SETTLE_S in
     * daynight.h. */
    int      boot_probe;
    int      interval_ms;        /* sample interval */
    /* transition_s (minimum dwell between switches), ref_delay_s (wait after
     * entering night before anchoring the reference), and the learning
     * subsystem that used to live here (per-camera `learn`/`state_path`) are
     * gone as of the 2026-08-22 consolidation: transition_s and ref_delay_s
     * are now the fixed DN_TRANSITION_S/DN_REF_DELAY_S constants in
     * daynight.h, and `learn` was removed outright - its own safety clamp
     * (night_gain/2) could not raise day_gain far enough for the cameras that
     * actually needed it (private/fleet/camera-fleet.md), so
     * diagnose_thresholds below is what is left: it tells the operator what
     * to raise instead of trying to raise it automatically. */
    char     switch_cmd[64];     /* board script, run as "<cmd> day|night" */
    char     isp_path[128];      /* ISP exposure proc file */
    /* Opt-in decision-trace recorder (2026-08-14, the replay harness's
     * "step 0" - see docs/wiki/Day-Night-Design-Notes.md section 6). Empty =
     * off (the default: zero cost, nothing written). When set, the detection
     * thread appends one CSV line per DN_TRACE_EVERY samples: monotonic ms,
     * mode, the exposure index and its two inputs (gain and the
     * integration-time ratio), the smoothed value, the night reference, the
     * probe bar and the two pending deadlines - i.e. the full evidence AND
     * scheduling state, which is exactly what every past incident had to be
     * hand-reconstructed from syslog fragments for. The file is size-
     * capped (DN_TRACE_MAX_BYTES, rotated once to <path>.1, so bounded at
     * 2x the cap) and MUST live on tmpfs (/tmp, /run) - these are
     * camera-grade eMMC/NAND and a trace is a diagnostic to copy off on
     * demand, not a flash-wearing log (a LOGW reminds if the path does not
     * look like tmpfs). File-only key, deliberately NOT settable via
     * /control: it names a path the daemon writes to as root, the same
     * arbitrary-file-write boundary that keeps switch_cmd/isp_path out of
     * the POST surface. */
    char     trace_path[128];
    /* Opt-in threshold diagnostics (default 0 = off). When a probe fails and
     * the BEST day-pipeline reading of that excursion was still clear of
     * day_gain, warn that the threshold is unreachable for this scene and
     * name the value to raise it above - this is now the only diagnosis of
     * that failure mode; the `learn` subsystem that used to raise the
     * threshold automatically is gone (see the note above switch_cmd).
     *
     * Off by default deliberately: it is a WARN that repeats once per probe,
     * forever, until a human edits the config - pure log growth on
     * flash-backed syslog for a correctly configured camera. Unlike
     * trace_path it is safe on the /control surface (names no path, writes no
     * file), so it can be toggled live while investigating. */
    int      diagnose_thresholds;
} ms_daynight_cfg;

typedef struct {
    int      enabled;            /* IMP_IVS motion detection */
    int      monitor_stream;
    int      sensitivity;        /* 0..255 (mapped to IMP's 0..4 in the HAL) */
    int      cols, rows;         /* detection GRID over the monitor stream's
                                  * frame; cols*rows is clamped to the SDK's
                                  * IMP_IVS_MOVE_MAX_ROI_CNT (motion_caps.h:
                                  * 52 on most SDKs, 4 on T10/T20 3.9.0) */
    int      roi_x, roi_y, roi_w, roi_h;  /* legacy single-ROI keys: still
                                  * parsed (old configs load warning-free)
                                  * but unused since the grid replaced them */
    int      cooldown_ms;        /* min gap between motion events */
    int      hold_ms;            /* keep a cell "active" this long after its last
                                  * retRoi hit so async /events + /control readers
                                  * reliably observe single-frame motion instead
                                  * of racing the clear back to 0 (0 = no hold) */
    int      skip_frames;        /* IMP_IVS_MoveParam.skipFrameCnt: analyse every
                                  * Nth frame. Higher = cheaper but more latency;
                                  * lower = snappier but more CPU (>=1, default 5) */
    char     on_motion[128];     /* script/program to run on motion ("" = none),
                                  * no arguments (fork()+execlp(), not a shell
                                  * command line). Config-file only, NOT
                                  * settable via /control. */
} ms_motion_cfg;

/* local recording to SD (fragmented MP4 segments, like raptor's RMR). Reuses
 * the fMP4 muxer; motion-triggered or continuous. */
typedef struct {
    int      enabled;            /* master enable (also gates on-boot start) */
    int      channel;            /* video stream to record (0..MS_MAX_VSTREAM-1) */
    int      mode;               /* 0 = continuous, 1 = motion-triggered */
    char     dir[128];           /* SD base dir, e.g. /mnt/mmcblk0p1 */
    char     name[96];           /* strftime path template (under <dir>/<host>/records/) */
    int      segment_s;          /* max segment length (seconds), 0 = single file */
    int      pre_roll_s;         /* motion: seconds of buffered video kept before the trigger */
    int      post_roll_s;        /* motion: keep recording this long after the last motion */
    int      min_free_mb;        /* delete oldest segments until at least this much is free */
    int      audio;              /* 1 = mux audio into the recording when available */
} ms_record_cfg;

/* native timelapse: periodic JPEG snapshots from a stream's piggyback JPEG
 * encoder (falls back to the dedicated jpeg.* channel), written under
 * <dir>/<host>/timelapses/. Shots older than keep_days are pruned. */
typedef struct {
    int      enabled;            /* master enable (also gates on-boot start) */
    int      channel;            /* video stream whose JPEG is captured (0..MS_MAX_VSTREAM-1) */
    char     dir[128];           /* base dir, any writable path (SD, NFS, ...) */
    char     name[96];           /* strftime path template (under <dir>/<host>/timelapses/) */
    int      interval_s;         /* seconds between shots */
    int      keep_days;          /* delete shots older than this (0 = keep forever) */
} ms_timelapse_cfg;

/* optional SRT output (USE_SRT builds only, libsrt): serves one video stream
 * (+audio) as MPEG-TS over SRT, as a listener (default) or as a caller that
 * dials out to srt.host (for cameras behind NAT / on unreliable links). */
typedef struct {
    int      enabled;
    int      port;                  /* listener: local bind port; caller:
                                     * remote port (default 9000) */
    int      channel;               /* video stream to serve */
    int      latency_ms;            /* SRT receive/peer latency */
    char     mode[16];              /* "listener" (default) or "caller" */
    char     host[64];              /* caller mode: remote host/address */
    char     streamid[64];          /* listener: required STREAMID;
                                     * caller: STREAMID to present */
    char     passphrase[64];        /* optional AES passphrase ("" = none) */
} ms_srt_cfg;

typedef struct {
    /* general */
    int            loglevel;
    /* Modules raised to DEBUG regardless of loglevel, comma separated
     * ("DAYNIGHT,MAIN"). See log_set_debug_modules() for why names and not a
     * bitmask. */
    char           debug_modules[64];
    int            imp_polling_timeout;
    int            osd_pool_size;

    ms_sensor_cfg  sensor;
    ms_image_cfg   image;

    /* rtsp */
    int            rtsp_enabled;
    int            rtsp_port;
    int            rtsp_mtu;       /* max RTP packet size (header+payload) for
                                    * UDP packetization; default 1200 so the
                                    * resulting IP packets survive WireGuard/
                                    * OpenVPN/PPPoE paths without fragmenting */
    char           rtsp_user[MS_MAX_STR];
    char           rtsp_pass[MS_MAX_STR];

    /* http fmp4 preview */
    int            http_enabled;
    int            http_port;
    int            http_preview_chn;   /* which video stream to expose */
    /* per-client adaptive frame-dropping for the fMP4 preview path: when a
     * specific /stream.mp4 client's own fanqueue backs up (a weak link that
     * can't keep up), that client freezes on its last frame and resumes
     * cleanly at the next natural keyframe instead of decoding a corrupted
     * headless GOP. Purely a per-client delivery-layer decision - never
     * touches the shared encoder nor any other subscriber. 0 = off (default;
     * strictly forward every frame - the legacy all-or-bust path, byte-for-byte
     * the pre-feature behavior), 1 = enable the per-client dropping. Default OFF
     * until verified on real hardware: an early build hung the stream on a slow
     * MIPS SoC when 'dropping' could not find a keyframe to resume on. */
    int            http_adaptive_drop;
    char           http_user[MS_MAX_STR];  /* empty = fall back to rtsp creds */
    char           http_pass[MS_MAX_STR];
    /* /control token auth (startup/security settings, NOT settable via
     * /control): http_token = optional persistent remote secret (also
     * accepted as a valid token, for automation); http_token_file = where
     * the random per-boot token is published for local privileged readers
     * ("" = don't write). The configured secret is NEVER written there. */
    char           http_token[MS_MAX_STR];
    char           http_token_file[128];
    /* GET /events SSE push stream (USE_CONTROL builds only; startup
     * settings, deliberately NOT settable via /control) */
    int            events_enabled;      /* 0 = endpoint answers 404 */
    int            events_stats_ms;     /* "stats" event period, 0 = none */
    int            events_max_clients;  /* concurrent /events conns -> 503 */
    /* optional TLS (USE_TLS builds only): HTTPS for the http port + RTSPS for a
     * second RTSP port. Plain HTTP/RTSP still run as before. */
    int            http_https;          /* 1 = serve the http port over TLS */
    char           http_tls_cert[128];  /* PEM cert file */
    char           http_tls_key[128];   /* PEM private key file */
    int            rtsp_tls;            /* 1 = also run an RTSPS (TLS) listener */
    int            rtsp_tls_port;       /* RTSPS port (default 322) */

    ms_vstream_cfg video[MS_MAX_VSTREAM];
    ms_audio_cfg   audio;
    ms_jpeg_cfg    jpeg;
    ms_osd_cfg     osd;
    ms_privacy_region privacy[MS_MAX_VSTREAM][MS_MAX_PRIVACY]; /* cover masks */
    ms_motion_cfg  motion;
    ms_record_cfg  record;
    ms_timelapse_cfg timelapse;
    ms_srt_cfg     srt;
    ms_daynight_cfg daynight;

    /* sim backend (x86 testing) */
    char           sim_video0[256];
    char           sim_video1[256];
    char           sim_audio[256];
    char           sim_jpeg[256];
} ms_config;

extern ms_config g_cfg;
extern const char *g_cfg_path;   /* config file in use (set by config_load) */

/* Boot-time snapshot of the whole config, taken once at startup by
 * config_snapshot_boot() before any /control thread can run. It is immutable
 * afterwards, so it is read lock-free.
 *
 * WHY: videoN.* encoder/geometry/codec keys (enabled/codec/width/height/fps/
 * bitrate/rotation) are restart-only. A /control POST still updates the LIVE
 * g_cfg.video[] (so the change persists to the config file for the next boot
 * and is echoed by GET /control), but the RUNNING encoder keeps producing the
 * stream it was started with. Any consumer that packetizes, describes (RTSP
 * SDP), muxes (fMP4), records or builds an SRT PMT MUST read those fields from
 * g_cfg_boot, NOT the live g_cfg, or a live edit desyncs every new session/
 * segment/client from the actual elementary stream: a live codec change makes
 * new consumers packetize/describe the wrong codec (corrupt/undecodable
 * output), a live-enable of a boot-disabled stream is "servable" but has no
 * publisher (client hangs), and live geometry/fps/bitrate describe values the
 * encoder is not producing.
 * EXCEPTION: videoN.rtsp_path is honestly live (a DESCRIBE re-matches it every
 * request) - read it from the live g_cfg, not from here. */
extern ms_config g_cfg_boot;
void config_snapshot_boot(void);   /* g_cfg_boot = g_cfg; call once at startup */

void config_defaults(ms_config *c);
int  config_load(ms_config *c, const char *path); /* 0 ok, <0 file err (defaults kept) */
/* auto-detect unset sensor.* from /proc/jz/sensor/<X>/ (config wins, then a
 * safe fallback); call once after config_load(), before the HAL starts */
void config_sensor_finalize(ms_config *c);
/* apply a single key=value (same keys as the config file) to c */
void config_apply_kv(ms_config *c, const char *key, const char *val);
/* read the current value of a key back as a normalized string (the same form
 * config_apply_kv would store). Covers the keys the /control endpoint touches
 * (image.*, audio.*, videoN.*, sensor.*, osdS.N.*, legacy osdN.*, osd.*).
 * Returns 1 if the key is known (out filled), 0 otherwise. Used for change-
 * detection. (audio/video/sensor coverage includes the persist-only restart
 * keys. A legacy osdN.* key reads back only while all streams agree on the
 * value - otherwise it reports unknown so a legacy write always applies.) */
int  config_get_kv(const ms_config *c, const char *key, char *out, size_t cap);
/* 1 if `key` names a T_STR field. Lets a caller tell an empty value that MEANS
 * something (clear this text) from one that would silently zero a number. */
int  config_key_is_str(const char *key);
/* replace/append "key = value" lines in the config file (atomic, keeps
 * comments/order). Returns 0 on success. */
int  config_write_keys(const char *path, const char *const *keys,
                       const char *const *vals, int n);
/* Serializes /control's runtime writes of g_cfg (the whole config_apply_kv
 * runs under this lock, see timps_apply_setting) against concurrent readers
 * (OSD updater, recorder, timelapse, day/night, RTSP path match, GET /control).
 * Leaf lock: hold it only for a short copy/compare, never across HAL/status/
 * blocking calls.
 * Covers STRINGS (copystr tears) AND the live ints/enums written alongside
 * them: a reader that takes this lock therefore sees a consistent value for
 * BOTH, so the numeric live fields of an item read under the lock (e.g. the
 * whole-ms_osd_item snapshot in imp_osd.c/hal_ingenic.c) are race-free too.
 * (Aligned word reads don't TEAR, but a lock-free int read of a concurrently
 * written field is still a C11 data race / UB regardless - so a live int must
 * either be read under this lock or be _Atomic. The one live int read lock-free
 * on a hot path, audio.mute in the per-frame audio worker, is _Atomic instead
 * of taking the lock every frame; see config.h.) */
void config_str_lock(void);
void config_str_unlock(void);

/* ------------------------------------------------------------------------
 * cfg_field: config.c's descriptor-driven key table entry (see the B2 doc
 * comment above set_kv()'s tables in config.c for the full {name, offset,
 * type, clamp} rationale). Exposed here - not just kept private to config.c -
 * so control.c's /control POST handling can walk these SAME tables instead
 * of hand-listing field names a second time; see F_CTRL below. */
typedef struct {
    const char    *name;    /* canonical key name (after the section prefix) */
    const char    *alias;   /* optional legacy/alternate spelling, or NULL */
    unsigned short off;     /* byte offset from the section base struct */
    unsigned char  type;    /* config.c's internal T_* (opaque outside
                             * config.c - only its field_set()/field_get()
                             * interpret it) */
    unsigned char  flags;   /* F_* below */
    int lo, hi;             /* T_INT/T_FLT: clamp when lo<hi; T_STR: buf size in hi */
} cfg_field;

/* Set/persist-only key: config_get_kv() keeps reporting it unknown (full
 * rationale in config.c, above set_kv()'s tables). */
#define F_NOGET  0x01
/* Live int/bool field read lock-free by a different thread than the /control
 * writer, so it must be an `_Atomic int` in the struct (see config.c). */
#define F_ATOMIC 0x02
/* POST-able via /control: control.c's generic per-section field walker
 * (apply_ctrl_fields() in control.c) only ever applies a JSON key whose
 * matching cfg_field entry has THIS flag set - a deliberate, mandatory-per-
 * field SECURITY ALLOWLIST, not a walk-everything default. A field missing
 * F_CTRL is unreachable via HTTP POST even though it may be fully GET-able
 * and settable by hand-editing the config file - e.g. motion.on_motion/
 * cooldown_ms (fork()+execlp() hook / re-exec floor), daynight.switch_cmd/
 * isp_path (exec'd command / scraped proc path), every rtsp.* / http.*
 * credential/token, and the videoN.imp_chn / jpeg* / jpeg_chn internal channel-
 * wiring fields all intentionally have NO F_CTRL. Adding F_CTRL to a field is
 * therefore an explicit, reviewable decision to expose it over HTTP - never
 * inferred just because a field exists in one of these tables. */
#define F_CTRL   0x04
/* Belongs in GET /control's "caps":{"image":[...],"audio":[...]} advertisement
 * (control.c's caps builder only emits entries that carry BOTH F_CTRL and
 * F_CAP). This is a SEPARATE axis from F_CTRL, not a refinement of it:
 * F_CTRL is a fixed security allowlist (identical on every build), while
 * F_CAP is compile-time gated per platform/feature build - image_fields[]
 * ties it to isp_caps.h's ISP_HAS_* matrix (same one hal_ingenic.c's
 * isp_apply_image() guards its IMP_ISP_Tuning_Set* calls with), and
 * audio_fields[] ties it to audio_caps.h's AUDIO_HAS_* matrix / USE_PLAY
 * /USE_BACKCHANNEL PLUS a deliberate curation: several audio fields keep
 * F_CTRL (POST-able, persists to config) but never F_CAP, because they are
 * restart-required or persist-only, not because the hardware lacks them
 * (see the comment above audio_fields[] in config.c). This replaces the old
 * hand-written IMG_CAPS/AUD_CAPS arrays in control.c, which re-listed these
 * exact names under the exact same #ifdef conditions a second time. */
#define F_CAP    0x08

/* Accessors handing control.c's generic /control POST walker the section
 * field tables it needs (config.c keeps the tables themselves static - these
 * just expose a read-only view + count). Only entries with F_CTRL set in the
 * returned table are POST-eligible; see the flag's doc comment above. */
const cfg_field *cfg_fields_image(int *n);
const cfg_field *cfg_fields_audio(int *n);
const cfg_field *cfg_fields_sensor(int *n);
const cfg_field *cfg_fields_osd(int *n);       /* osd.* globals (not items) */
const cfg_field *cfg_fields_osd_item(int *n);  /* one OSD overlay item */
const cfg_field *cfg_fields_motion(int *n);
const cfg_field *cfg_fields_record(int *n);
const cfg_field *cfg_fields_timelapse(int *n);
const cfg_field *cfg_fields_daynight(int *n);
const cfg_field *cfg_fields_general(int *n);
const cfg_field *cfg_fields_video(int *n);     /* one videoN stream */
const cfg_field *cfg_fields_privacy(int *n);   /* one privacy region */

#endif
