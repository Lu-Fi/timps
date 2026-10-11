#ifndef MS_FS_ROT_ENV_H
#define MS_FS_ROT_ENV_H
#include "../err_codes.h"
/* FrameSource-rotate safe envelope per SoC family. Pure so the host test
 * (scripts/test_fs_rot.c) covers the decision fs_create() acts on. Alignment
 * and pixel count are swap-invariant, so pre-rotation dims suffice. */

typedef struct {
    int         align;       /* width and height multiple of this */
    long        max_pixels;
    int         max_fps;     /* 0 = no limit */
    int         sub_only;    /* refuse IMP channel 0 (video0, the main stream) */
    const char *why;         /* log text for a size or fps refusal */
} fs_rot_env;

/* T31 vendor libimp: past this it software-rotates and the oversized
 * geometry fails Encoder bring-up. */
#define FS_ROT_ENV_T31 { 64, 1280L*704L, 15, 0, \
    "<=1280x704, <=15fps; past it libimp software-rotates and the oversized " \
    "geometry fails Encoder bring-up" }
/* T10/T20/T21 OpenIMP: CPU rotate; the encoder reads NV12 with pitch = width
 * and chroma after ALIGN16(height), hence 16; OpenIMP refuses > 704x576. */
#define FS_ROT_ENV_T1X { 16, 704L*576L, 0, 1, \
    "<=704x576 pixels, OpenIMP software rotate is sub-stream only" }

/* returns ERR_OK or the ERR_ROT_* code /control reports */
static inline int fs_rot_check(const fs_rot_env *e, int chn, int w, int h, int fps)
{
    if (e->sub_only && chn == 0) return ERR_ROT_MAIN;
    if ((w | h) & (e->align - 1)) return ERR_ROT_ALIGN;
    if ((long)w * h > e->max_pixels) return ERR_ROT_SIZE;
    if (e->max_fps > 0 && fps > e->max_fps) return ERR_ROT_FPS;
    return ERR_OK;
}

/* T23 software rotate (USE_SW_ROTATE), on the post-rotation dims: the single
 * core manages <=704x576 <=15fps, and IMP_Encoder_YuvInit refuses an encoded
 * width that is not 16-aligned or below 256. */
#define MS_SW_ROT_MAX_PIXELS  (704L*576L)
#define MS_SW_ROT_MAX_FPS     15
#define MS_SW_ROT_WIDTH_ALIGN 16
#define MS_SW_ROT_MIN_WIDTH   256

static inline int sw_rot_check(int ew, int eh, int fps)
{
    if ((long)ew * eh > MS_SW_ROT_MAX_PIXELS) return ERR_ROT_SIZE;
    if (fps > MS_SW_ROT_MAX_FPS) return ERR_ROT_FPS;
    if (ew % MS_SW_ROT_WIDTH_ALIGN) return ERR_ROT_ALIGN;
    if (ew < MS_SW_ROT_MIN_WIDTH) return ERR_ROT_SMALL;
    return ERR_OK;
}

/* T1x SetChnRotate is OpenIMP-only and older OpenIMPs stub it: 9cab219
 * returns 0 for anything without rotating, 6857610..636b6ac return -1 for
 * 90/270, 6b7f5ae rotates and validates the geometry. A 0x0 probe first
 * tells the success-returning stub apart; a real implementation refuses it. */
typedef int (*fs_rot_set_fn)(int chn, int rot, int w, int h);

enum {
    FS_ROT_SET_OK = 0,
    FS_ROT_SET_MISSING,
    FS_ROT_SET_STUB,
    FS_ROT_SET_REFUSED,
};

static inline int fs_rot_set(fs_rot_set_fn set, int chn, int rot, int w, int h)
{
    if (!set) return FS_ROT_SET_MISSING;
    if (set(chn, rot, 0, 0) == 0) {
        set(chn, 0, w, h);
        return FS_ROT_SET_STUB;
    }
    return set(chn, rot, w, h) == 0 ? FS_ROT_SET_OK : FS_ROT_SET_REFUSED;
}

#endif
