#include "osd_text.h"
#include "font8x16.h"
#include <stdlib.h>
#include <string.h>

int osd_text_render(const char *s, int scale, uint32_t fg, uint32_t bg,
                    int outline, uint32_t oc, uint8_t **out, int *w, int *h)
{
    if (scale < 1) scale = 1;
    if (outline < 0) outline = 0;
    if (outline > 4*scale) outline = 4*scale;       /* keep the stroke sane */
    if (((oc>>24)&0xFF) == 0) outline = 0;          /* transparent = off */
    int n = (int)strlen(s);
    if (n < 1) n = 1;
    int W = n * FONT_W * scale + 2*outline;
    int H = FONT_H * scale + 2*outline;
    W = (W + 1) & ~1;   /* IMP_OSD needs an even picture width */
    uint32_t *px = (uint32_t*)malloc((size_t)W * H * 4);
    if (!px) return -1;
    for (int i=0;i<W*H;i++) px[i] = bg;

    /* two passes over the glyph bits: the outline pass stamps a (2o+1)^2
     * block in the outline color, the fill pass paints on top */
    for (int pass=0; pass<2; pass++) {
        if (pass==0 && outline==0) continue;
        uint32_t col = pass ? fg : oc;
        int o = pass ? 0 : outline;
        for (int ci=0; s[ci]; ci++) {
            unsigned char c = (unsigned char)s[ci];
            if (c < FONT_FIRST || c > FONT_LAST) c = '?';
            const uint8_t *g = font8x16[c - FONT_FIRST];
            int ox = ci * FONT_W * scale + outline;
            for (int y=0;y<FONT_H;y++){
                uint8_t row = g[y];
                for (int x=0;x<FONT_W;x++){
                    if (!(row & (1 << (7-x)))) continue;
                    for (int sy=-o;sy<scale+o;sy++)
                        for (int sx=-o;sx<scale+o;sx++){
                            int py=(y*scale+sy)+outline, pxx=(ox + x*scale+sx);
                            if (py<0||py>=H||pxx<0||pxx>=W) continue;
                            px[py*W + pxx] = col;
                        }
                }
            }
        }
    }
    *out = (uint8_t*)px; *w = W; *h = H;
    return 0;
}
