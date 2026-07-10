#include "osd_text.h"
#include "font8x16.h"
#include <stdlib.h>
#include <string.h>

int osd_text_render(const char *s, int scale, uint32_t fg, uint32_t bg,
                    uint8_t **out, int *w, int *h)
{
    if (scale < 1) scale = 1;
    int n = (int)strlen(s);
    if (n < 1) n = 1;
    int W = n * FONT_W * scale;
    int H = FONT_H * scale;
    uint32_t *px = (uint32_t*)malloc((size_t)W * H * 4);
    if (!px) return -1;
    for (int i=0;i<W*H;i++) px[i] = bg;

    for (int ci=0; s[ci]; ci++) {
        unsigned char c = (unsigned char)s[ci];
        if (c < FONT_FIRST || c > FONT_LAST) c = '?';
        const uint8_t *g = font8x16[c - FONT_FIRST];
        int ox = ci * FONT_W * scale;
        for (int y=0;y<FONT_H;y++){
            uint8_t row = g[y];
            for (int x=0;x<FONT_W;x++){
                if (row & (1 << (7-x))) {
                    for (int sy=0;sy<scale;sy++)
                        for (int sx=0;sx<scale;sx++){
                            int py=(y*scale+sy), pxx=(ox + x*scale+sx);
                            px[py*W + pxx] = fg;
                        }
                }
            }
        }
    }
    *out = (uint8_t*)px; *w = W; *h = H;
    return 0;
}
