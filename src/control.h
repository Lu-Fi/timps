/* control.h - optional live-control parsing (compiled only with -DUSE_CONTROL).
 *
 * POST /control takes a nested JSON blob (prudynt-like, but native timps):
 *
 *   { "image": {"brightness":140,"contrast":128,"saturation":128,
 *               "sharpness":128,"hue":128,"hflip":0,"vflip":0,"running_mode":1,
 *               "anti_flicker":2,"ae_compensation":128,"max_again":160,
 *               "max_dgain":80,"sinter_strength":128,"temper_strength":128,
 *               "dpc_strength":128,"defog_strength":128,"drc_strength":128,
 *               "highlight_depress":0,"backlight_compensation":0,
 *               "core_wb_mode":0,"wb_rgain":0,"wb_bgain":0},
 *     "audio": {"volume":90,"gain":30,"alc_gain":0,"high_pass":1,"agc":1,
 *               "agc_target_dbfs":10,"agc_compression_db":0,"ns":2,
 *               "enabled":1,"codec":"aac","samplerate":16000,"channels":1,
 *               "bitrate":32,"force_stereo":0,
 *               "spk_enabled":0,"spk_volume":80,"spk_gain":25},
 *     "osd":   {"enabled":1},                          <- master switch only
 *     "osd0":  {"0":{"enabled":1,"text":"%Y-%m-%d %H:%M:%S","x":10,"y":10,
 *                    "font_size":32,"color":"0xFFFFFFFF",
 *                    "outline":1,"outline_color":"0xFF000000"},
 *               "3":{"enabled":0}},                    <- video stream 0 items
 *     "osd1":  {"0":{"text":"sub cam"}},               <- video stream 1 items
 *     "video": {"0":{"enabled":1,"codec":"h264","width":1920,"height":1080,
 *                    "fps":25,"bitrate":3500,"rc_mode":"cbr","gop":50,
 *                    "max_gop":60,"profile":2,"qp":35,"min_qp":20,
 *                    "max_qp":45,"rotation":0,"buffers":2,
 *                    "rtsp_path":"/ch0"},
 *               "1":{"bitrate":600}},
 *     "sensor":{"model":"gc2053","i2c_addr":55,"fps":25,
 *               "width":1920,"height":1080} }
 *
 * Every recognized setting is flattened to its config-file key (image.*,
 * audio.*, osdS.N.* per stream S, osd.enabled, videoN.*, sensor.*), applied
 * to the in-memory config,
 * OSD: every video stream has its own independent overlay set. The canonical
 * keys are osd<S>.<N>.<field> ("osd0"/"osd1" JSON sections, applied live via
 * imp_osd). The legacy shared form {"osd":{"0":{...}}} -> osdN.* keys is
 * still parsed and mirrors the item onto EVERY stream (backward compat).
 * applied live through hub_control() -> HAL, and finally persisted into the
 * config file (only the changed keys; comments/order preserved). Live audio
 * keys are volume/gain/alc_gain/high_pass/agc/agc_target_dbfs/
 * agc_compression_db/ns; the attribute-level audio keys (enabled/codec/
 * samplerate/channels/bitrate/force_stereo/spk_*) are persist-only and take
 * effect on restart. videoN.* and sensor.* keys are persist-first: every one
 * is stored + persisted, and MOST take effect on the next restart only
 * (encoder/FrameSource/sensor attributes are not reconfigured on the running
 * pipeline) - EXCEPT the rate-control subset this platform can apply to the
 * live encoder (enc_caps.h; classic SoCs: the whole rc block incl. rc_mode,
 * new-API SoCs: bitrate/min_qp/max_qp and per SoC qp/i_bias_lvl; host sim:
 * none). GET /control advertises that subset as "caps":{"video_live":[...]}
 * next to the conservative "restart":["video","sensor"] section list, and
 * each POST reply reports in "deferred"/"deferred_keys" which of ITS changed
 * video/sensor fields did NOT reach the running pipeline (channel down,
 * classic H265, rejected IMP call, sim) - so a caller can tell what is in
 * effect now from what waits for a restart, per request, not per platform
 * guess. Unknown keys and missing fields are ignored. The legacy flat form ({"brightness":140,
 * "running_mode":1, "force_mode":"night"}) still works and maps to image.*.
 *
 * Further sections (applied live + persisted, see control.c):
 *   "record":    {"active":1|0} manual override + the record.* config keys
 *                (enabled/channel/mode/dir/name/segment_s/pre_roll_s/
 *                post_roll_s/min_free_mb/audio)
 *   "timelapse": the timelapse.* config keys (enabled/channel/dir/name/
 *                interval_s/keep_days); the running timelapse thread reads
 *                them live. GET /control echoes both sections with live
 *                status (recording/free_mb/last_file/...). */
#ifndef MS_CONTROL_H
#define MS_CONTROL_H
#include <stddef.h>
#include "hal/imp_motion.h"   /* ms_motion_status for control_motion_json */

/* Apply + persist the settings found in a JSON text. No-op for unknown keys.
 * Safe to call with a partial/empty string. */
/* Outcome of one POST /control body. The old signature was void and httpd
 * answered {"ok":true} unconditionally - the response did not merely omit an
 * error, it ASSERTED success, so a client could not tell a typo from a write.
 *
 * What is and is not reportable follows from the parser's shape: apply_ctrl_fields
 * walks the field TABLE and looks each name up in the body, so a key the tables
 * do not know is never seen and cannot be listed individually. What CAN be stated
 * exactly is how many known fields the body carried - and that is enough for a
 * client to verify its own write: post one key, expect accepted >= 1.
 *
 *   accepted - fields recognised and applied, INCLUDING writes of the value a
 *              field already had. Kept separate from `changed` on purpose: a
 *              client re-posting the current value must not read an empty
 *              change list as failure.
 *   changed  - subset that actually differed; these are echoed with their
 *              EFFECTIVE value, i.e. after clamping. Clamping is deliberate and
 *              documented, so a clamped write is a success, not an error - the
 *              echo is how the caller learns what it really got.
 *   rejected - values refused outright (empty, "null", "undefined"), plus
 *              COMMANDS that were understood and failed (record.clip with an
 *              unwritable path). accepted==0 with rejected>0 is a different
 *              answer from accepted==0 with rejected==0 and httpd.c grades it
 *              differently (409 vs 422): the first means the key names were
 *              right and the values were not, the second means this build does
 *              not know the key names at all. */
#define CTRL_ECHO_CAP 512
#define CTRL_DEFER_CAP 1024
typedef struct {
    int accepted;
    int changed;
    int rejected;
    /* applied but NOT written to the config file: one request changed more
     * keys than the persist list holds. Live now, gone after a reboot - and
     * until this counter existed the caller had no way to learn that. */
    int not_persisted;
    /* CHANGED videoN.* / sensor.* fields that were persisted but did NOT reach
     * the running pipeline (hub_control() returned 0): they take effect on
     * the next daemon restart. Subset of `changed` - an unchanged re-post
     * reports nothing here, exactly like the echo. defer[] carries the keys
     * as a ready-made JSON array body ("\"video0.width\",\"sensor.fps\"");
     * defer_full mirrors echo_full (0 = list overflowed, count still exact). */
    int deferred;
    /* The changed settings as a ready-made JSON object body, e.g.
     * "\"daynight.day_confirm_s\":\"41\"" - EFFECTIVE values, i.e. after
     * clamping, which is the whole point: a caller that posted 999 and got 99
     * learns so from the reply instead of having to GET the whole document
     * again. Bounded on purpose (one connection thread's stack is already the
     * tightest in the daemon); if more changed than fits, `echo_full` stays 0
     * and the caller falls back to a GET. */
    char echo[CTRL_ECHO_CAP];
    int  echo_full;   /* 1 = echo lists every changed field */
    char defer[CTRL_DEFER_CAP];
    int  defer_full;  /* 1 = defer lists every deferred field */
} ctrl_result;

/* Returns 0 if the body was a JSON object, -1 if it was not parseable at all,
 * -2 on an internal failure (OOM) - which is NOT the caller's fault and must
 * not be reported to it as a bad request (httpd.c answers 503 to -2, 400 to
 * -1). res may be NULL; on -2 only its counters are set, not its echo. */
int control_apply_json(const char *json, ctrl_result *res);

/* Shared read-only status object builders: GET /control embeds these and the
 * /events SSE stream pushes them stand-alone, so both endpoints emit the
 * IDENTICAL shape by construction. Each writes one complete {...} object and
 * returns the would-be length like snprintf (>= cap means truncated). The
 * caller provides the snapshot so dedup compares exactly what was sent. */
int  control_motion_json(char *buf, size_t cap, const ms_motion_status *st);
int  control_daynight_json(char *buf, size_t cap, int enabled, int mode,
                           float brightness, float total_gain, float exposure,
                           float ae_luma, float night_ref, float probe_bar,
                           int isp_desync);

/* Serialize the current (in-memory) controllable values as JSON into buf.
 * The dump starts with a per-build capability list
 * "caps":{"image":[...],"audio":[...],"osd":[...],"restart":[...]}:
 * image/audio list the keys the HAL wires LIVE on this PLATFORM
 * (isp_caps.h/audio_caps.h) so the WebUI can grey out what the SoC cannot do;
 * "osd" lists the per-item leaf keys /control accepts (incl. outline/
 * outline_color; the sets are dumped per stream as "osd0"/"osd1" and the
 * master "enabled" needs a restart); "restart" lists the sections (video,
 * sensor) whose keys are persist-only and need a daemon restart;
 * rtsp_max_clients/http_max_clients/events_max_clients are the concurrent-
 * client ceilings each server refuses past (453 / 503 / 503) - not inferable
 * from anywhere else, and per-board, since the first two are -D overridable.
 * Top-level "srt" and "tls" objects report per-BUILD feature availability the
 * same way ({"available":0} when the binary was compiled without it): the
 * fleet's firmware and standalone builds differ in exactly these, and the
 * version string cannot tell them apart.
 * Returns the number of bytes written (excluding the NUL), or -1 if the output
 * did not fit in cap (the buffer holds a truncated, INVALID-JSON prefix - the
 * caller must not serve it as a successful response). */
int  control_get_json(char *buf, size_t cap);

/* GET /control?fields=1: the authoritative inventory of every F_CTRL-flagged
 * config field, grouped by section (config.h's cfg_fields_*() accessor
 * names: "image","audio","sensor","osd","osd_item","motion","record",
 * "timelapse","daynight","video","privacy" - the only sections that have any
 * F_CTRL fields at all; jpeg/rtsp/http/events/general/sim/srt have none).
 * Walks the SAME tables apply_ctrl_fields() uses in control.c - never
 * hand-lists field names a second time, so this can't itself drift from what
 * POST /control actually accepts. Exists so an external test harness (see
 * scripts/timps-qa.sh section 8) can diff its own hand-maintained
 * "fields I test" list against this and flag newly-added POST-able fields
 * that nobody wired into test coverage yet - the exact bug class the F_CTRL
 * consolidation itself fixed one layer down (config.c fields existing but
 * never reachable from a hand-written array), recurring one layer up in the
 * QA script. "osd_item"/"video"/"privacy" are per-instance tables (one
 * field-name set shared by every OSD item / video stream / privacy region,
 * not enumerated per index) - a listed name means "instance>0" too, not just
 * index 0. Returns the byte count like control_get_json, or -1 if truncated. */
int  control_fields_json(char *buf, size_t cap);

#endif
