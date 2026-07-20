/* nv12_rot.h - software 90/270 rotation of an NV12 frame (Batch 5, T23).
 *
 * Deliberately dependency-light (stdint only): compiles standalone on the host
 * for unit tests and on any target toolchain. Only built into the binary when
 * USE_SW_ROTATE=1 (see Makefile); referenced only under ROT_HAS_SW_90. */
#ifndef MS_NV12_ROT_H
#define MS_NV12_ROT_H
#include <stdint.h>

/* Rotate one NV12 frame by 90 or 270 degrees.
 *
 *   src : source frame, sw x sh pixels. Layout: Y plane (sw*sh bytes)
 *         immediately followed by the interleaved CbCr plane
 *         ((sw/2)*(sh/2) Cb/Cr byte PAIRS = sw*sh/2 bytes).
 *   dst : destination frame, sh x sw pixels, same NV12 layout
 *         (Y plane sh*sw bytes + CbCr plane sh*sw/2 bytes). Must not
 *         overlap src. Total size for both buffers: w*h*3/2 bytes.
 *   rot : 90  = CLOCKWISE  (matches the config semantics: rotation=90 turns
 *               the image the way a clock hand moves; the source's top-left
 *               pixel ends up at the destination's top-RIGHT corner)
 *         270 = COUNTER-CLOCKWISE (source top-left -> destination
 *               bottom-left). Any other value: 90 is assumed.
 *
 * sw and sh must be even (NV12 chroma is subsampled 2x2; the config layer
 * only produces even encoder dims). The chroma plane is transposed as
 * (sw/2 x sh/2) Cb/Cr pairs, so chroma siting follows the luma rotation. */
void nv12_rotate90(const uint8_t *src, int sw, int sh, uint8_t *dst, int rot);

#endif
