/* control.h - optional live-control parsing (compiled only with -DUSE_CONTROL).
 * Parses a small JSON blob (as POSTed to /control by the thingino web UI / a
 * proxy) for the handful of settings we support and forwards each as a
 * (key,int) pair through hub_control() to the HAL, which applies it live. */
#ifndef MS_CONTROL_H
#define MS_CONTROL_H

/* Apply the settings found in a JSON text. No-op for unknown keys. Safe to call
 * with a partial/empty string. */
void control_apply_json(const char *json);

#endif
