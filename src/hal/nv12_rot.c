/* nv12_rot.c - software 90/270 NV12 rotation (Batch 5, T23 SW-rotate path).
 *
 * A plain transpose walk touches src rows sequentially but dst columns with a
 * full-image stride - on the T23's small D-cache that thrashes badly. 32x32
 * tiling keeps both the source rows and the destination columns of one tile
 * resident, which is the standard cache-blocked transpose. Elements are 1 byte
 * (Y) or a 2-byte Cb/Cr pair (chroma); the pair is copied bytewise so there is
 * no unaligned 16-bit access anywhere (safe on MIPS and under ASan).
 *
 * Direction convention (must match config semantics, see nv12_rot.h):
 *   90 (CW) : dst(x', y') = src(x, y) with x' = sh-1-y, y' = x
 *             src top-left -> dst top-right.
 *   270 (CCW): x' = y, y' = sw-1-x
 *             src top-left -> dst bottom-left.
 * In both cases the destination is sh x sw (dims swapped). */
#include "nv12_rot.h"
#include <string.h>

#define NV12_ROT_TILE 32

/* rotate one plane of sw x sh elements of size esz (1 = Y, 2 = CbCr pair)
 * into a dst plane of sh x sw elements. cw: 1 = 90 deg clockwise, 0 = 270. */
static void rot_plane(const uint8_t *s, int sw, int sh, uint8_t *d,
                      int esz, int cw)
{
    const int dw = sh;   /* destination plane width in elements */
    for (int ty = 0; ty < sh; ty += NV12_ROT_TILE) {
        int ye = ty + NV12_ROT_TILE; if (ye > sh) ye = sh;
        for (int tx = 0; tx < sw; tx += NV12_ROT_TILE) {
            int xe = tx + NV12_ROT_TILE; if (xe > sw) xe = sw;
            for (int y = ty; y < ye; y++) {
                const uint8_t *sp = s + ((size_t)y * (size_t)sw + tx) * esz;
                for (int x = tx; x < xe; x++, sp += esz) {
                    size_t di = cw ? ((size_t)x * dw + (size_t)(sh - 1 - y))
                                   : ((size_t)(sw - 1 - x) * dw + (size_t)y);
                    uint8_t *dp = d + di * esz;
                    dp[0] = sp[0];
                    if (esz == 2) dp[1] = sp[1];
                }
            }
        }
    }
}

void nv12_rotate90(const uint8_t *src, int sw, int sh, uint8_t *dst, int rot)
{
    if (!src || !dst || sw <= 0 || sh <= 0) return;
    int cw = (rot != 270);            /* 90 = clockwise; anything else != 270 too */
    /* luma: sw x sh bytes -> sh x sw */
    rot_plane(src, sw, sh, dst, 1, cw);
    /* chroma: interleaved CbCr, half resolution in BOTH axes -> transpose
     * (sw/2 x sh/2) two-byte pairs. Plane offsets are the full-res luma size. */
    const uint8_t *sc = src + (size_t)sw * (size_t)sh;
    uint8_t       *dc = dst + (size_t)sw * (size_t)sh;
    rot_plane(sc, sw / 2, sh / 2, dc, 2, cw);
}
