/* msttf.h - minimal TrueType rasterizer (a compact libschrift/stb_truetype
 * replacement, ~zero dependencies) for OSD text.
 * Supports: cmap format 4, simple + composite glyphs, quadratic beziers,
 * non-zero winding fill with 4x supersampled anti-aliasing. */
#ifndef MS_TTF_H
#define MS_TTF_H
#include <stdint.h>

typedef struct {
    uint8_t *data; long size;
    int      loca_fmt, num_glyphs, units_per_em, num_hmetrics;
    uint32_t off_head, off_glyf, off_loca, off_cmap, off_hmtx, off_hhea, off_maxp;
} msttf_font;

int  msttf_load(msttf_font *f, const char *path);
void msttf_free(msttf_font *f);

/* Render an ASCII/Latin-1 string into a newly allocated BGRA buffer.
 * pixel_h = cap height in pixels. fg/bg are 0xAARRGGBB. Caller frees *out. */
int  msttf_render(msttf_font *f, const char *s, int pixel_h,
                  uint32_t fg, uint32_t bg, uint8_t **out, int *w, int *h);

#endif
