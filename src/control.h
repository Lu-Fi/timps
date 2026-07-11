/* control.h - optional live-control parsing (compiled only with -DUSE_CONTROL).
 *
 * POST /control takes a nested JSON blob (prudynt-like, but native timps):
 *
 *   { "image": {"brightness":140,"contrast":128,"saturation":128,
 *               "sharpness":128,"hue":128,"hflip":0,"vflip":0,"running_mode":1},
 *     "audio": {"volume":90,"gain":30},
 *     "osd":   {"0":{"enabled":1,"text":"%Y-%m-%d %H:%M:%S","x":10,"y":10,
 *                    "font_size":32,"color":"0xFFFFFFFF"},
 *               "3":{"enabled":0}},
 *     "video": {"0":{"bitrate":3500},"1":{"bitrate":600}} }
 *
 * Every recognized setting is flattened to its config-file key (image.*,
 * audio.volume/gain, osdN.*, videoN.bitrate), applied to the in-memory config,
 * applied live through hub_control() -> HAL, and finally persisted into the
 * config file (only the changed keys; comments/order preserved). Unknown keys
 * and missing fields are ignored. The legacy flat form ({"brightness":140,
 * "running_mode":1, "force_mode":"night"}) still works and maps to image.*. */
#ifndef MS_CONTROL_H
#define MS_CONTROL_H
#include <stddef.h>

/* Apply + persist the settings found in a JSON text. No-op for unknown keys.
 * Safe to call with a partial/empty string. */
void control_apply_json(const char *json);

/* Serialize the current (in-memory) controllable values as JSON into buf.
 * Returns the number of bytes written (excluding the NUL). */
int  control_get_json(char *buf, size_t cap);

#endif
