/* msttf.h - minimal TrueType rasterizer (a compact libschrift/stb_truetype
 * replacement, ~zero dependencies) for OSD text.
 * Supports: cmap format 4, simple + composite glyphs, quadratic beziers,
 * non-zero winding fill with 4x supersampled anti-aliasing, and an optional
 * opt-in geometric autohinter (msttf_set_hinting()) - NOT a TrueType hint
 * bytecode interpreter; see that function's comment for why. */
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

/* Set the rasterizer's antialiasing quality: samples per axis per pixel
 * (1-4, clamped; default 2 if never called). Cost scales ~quadratically
 * (4 = 16 samples/px, 2 = 4 samples/px, roughly 2x the raster CPU cost of
 * 2 for text this small) with no visible difference at typical OSD sizes.
 * Global, not per-font: matches the single osd.supersample config knob. */
void msttf_set_ss(int ss);

/* Opt-in, OFF by default: enable the lightweight geometric autohinter
 * (osd.hinting config knob, default 0). This is NOT a TrueType instruction
 * interpreter - it never executes the font's embedded hint bytecode. It is
 * a coarse heuristic that snaps long, near-vertical/near-horizontal
 * flattened outline edges (typical letter stems/serifs) to whole pixel
 * columns/rows before rasterization, so equivalent stems across different
 * glyphs land on a consistent pixel boundary instead of whatever arbitrary
 * sub-pixel offset the raw scaled outline produces - the fix for uneven
 * stroke widths between glyphs at small OSD sizes. Purely geometric, no
 * bytecode execution surface. Global, not per-font, same pattern as
 * msttf_set_ss(). Default off: zero output change unless enabled. */
void msttf_set_hinting(int enable);

/* Render an ASCII/Latin-1 string into a newly allocated BGRA buffer.
 * pixel_h = cap height in pixels. fg/bg are 0xAARRGGBB.
 * outline = stroke width in px (0 = none): the glyph coverage is dilated by
 * that many pixels and blended in 'oc' (0xAARRGGBB) UNDER the fill; the
 * canvas grows by outline px on every side. Caller frees *out. */
int  msttf_render(msttf_font *f, const char *s, int pixel_h,
                  uint32_t fg, uint32_t bg, int outline, uint32_t oc,
                  uint8_t **out, int *w, int *h);

#endif
