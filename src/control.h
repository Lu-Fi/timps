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
 * effect on restart. ALL videoN.* and sensor.* keys are persist-only too:
 * encoder/FrameSource/sensor attributes are never reconfigured on the running
 * pipeline (the HAL just logs "applies on restart"); GET /control marks these
 * sections in "caps":{"restart":["video","sensor"]}. Unknown keys and missing
 * fields are ignored. The legacy flat form ({"brightness":140,
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
void control_apply_json(const char *json);

/* Shared read-only status object builders: GET /control embeds these and the
 * /events SSE stream pushes them stand-alone, so both endpoints emit the
 * IDENTICAL shape by construction. Each writes one complete {...} object and
 * returns the would-be length like snprintf (>= cap means truncated). The
 * caller provides the snapshot so dedup compares exactly what was sent. */
int  control_motion_json(char *buf, size_t cap, const ms_motion_status *st);
int  control_daynight_json(char *buf, size_t cap, int enabled, int mode,
                           float brightness, float total_gain);

/* Serialize the current (in-memory) controllable values as JSON into buf.
 * The dump starts with a per-build capability list
 * "caps":{"image":[...],"audio":[...],"osd":[...],"restart":[...]}:
 * image/audio list the keys the HAL wires LIVE on this PLATFORM
 * (isp_caps.h/audio_caps.h) so the WebUI can grey out what the SoC cannot do;
 * "osd" lists the per-item leaf keys /control accepts (incl. outline/
 * outline_color; the sets are dumped per stream as "osd0"/"osd1" and the
 * master "enabled" needs a restart); "restart" lists the sections (video,
 * sensor) whose keys are persist-only and need a daemon restart.
 * Returns the number of bytes written (excluding the NUL). */
int  control_get_json(char *buf, size_t cap);

#endif
