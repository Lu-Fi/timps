/* osd_text.h - render a string to a BGRA bitmap with the embedded font.
 * Portable (no SDK); used by the Ingenic OSD region and unit-testable. */
#ifndef MS_OSD_TEXT_H
#define MS_OSD_TEXT_H
#include <stdint.h>

/* Render 's' into a newly-allocated BGRA (little-endian 0xAARRGGBB) buffer.
 * scale>=1 integer magnification. fg/bg are 0xAARRGGBB (bg alpha 0 = transparent).
 * outline = stroke width in px (0 = none), drawn in 'oc' (0xAARRGGBB) under
 * the fill; the canvas grows by outline px on every side.
 * Caller frees *out. Returns 0 on success. */
int osd_text_render(const char *s, int scale, uint32_t fg, uint32_t bg,
                    int outline, uint32_t oc, uint8_t **out, int *w, int *h);

#endif
