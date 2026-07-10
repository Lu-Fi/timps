/* control.c - optional live control. The whole file is empty unless built with
 * -DUSE_CONTROL, so the feature adds nothing to a minimal build. */
#ifdef USE_CONTROL
#include "control.h"
#include "hub.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* return a pointer just past the ':' of "key" in the JSON text, or NULL */
static const char *find_val(const char *json, const char *key)
{
    char pat[64];
    int n = snprintf(pat, sizeof pat, "\"%s\"", key);
    if (n <= 0 || n >= (int)sizeof pat) return NULL;
    const char *p = strstr(json, pat);
    if (!p) return NULL;
    p += n;
    while (*p && *p != ':' && *p != ',' && *p != '}') p++;
    if (*p != ':') return NULL;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static int get_int(const char *json, const char *key, int *out)
{
    const char *p = find_val(json, key);
    if (!p || *p == '"') return 0;                 /* absent or a string */
    char *end; long v = strtol(p, &end, 10);
    if (end == p) return 0;
    *out = (int)v; return 1;
}

void control_apply_json(const char *json)
{
    if (!json || !json[0]) return;
    int v;

    /* day/night: either numeric image.running_mode, or daynight.force_mode string */
    const char *fm = find_val(json, "force_mode");
    if (fm && *fm == '"') {
        if      (!strncmp(fm, "\"night\"", 7)) hub_control("running_mode", 1);
        else if (!strncmp(fm, "\"day\"",   5)) hub_control("running_mode", 0);
    }
    if (get_int(json, "running_mode", &v)) hub_control("running_mode", v);

    if (get_int(json, "brightness", &v)) hub_control("brightness", v);
    if (get_int(json, "contrast",   &v)) hub_control("contrast",   v);
    if (get_int(json, "saturation", &v)) hub_control("saturation", v);
    if (get_int(json, "sharpness",  &v)) hub_control("sharpness",  v);
    if (get_int(json, "hflip",      &v)) hub_control("hflip",      v);
    if (get_int(json, "vflip",      &v)) hub_control("vflip",      v);
}
#endif /* USE_CONTROL */
