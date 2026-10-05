#ifndef MS_FS_ROT_ENV_H
#define MS_FS_ROT_ENV_H
/* FrameSource-rotate safe envelope per SoC family. Pure so the host test
 * (scripts/test_fs_rot.c) covers the decision fs_create() acts on. Alignment
 * and pixel count are swap-invariant, so pre-rotation dims suffice. */

typedef struct {
    int         align;       /* width and height multiple of this */
    long        max_pixels;
    int         max_fps;     /* 0 = no limit */
    int         sub_only;    /* refuse IMP channel 0 (video0, the main stream) */
    const char *why;         /* log text for a size refusal */
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

enum {
    FS_ROT_OK = 0,
    FS_ROT_REFUSE_MAIN,
    FS_ROT_REFUSE_ALIGN,
    FS_ROT_REFUSE_SIZE,
};

static inline int fs_rot_check(const fs_rot_env *e, int chn, int w, int h, int fps)
{
    if (e->sub_only && chn == 0) return FS_ROT_REFUSE_MAIN;
    if ((w | h) & (e->align - 1)) return FS_ROT_REFUSE_ALIGN;
    if ((long)w * h > e->max_pixels || (e->max_fps > 0 && fps > e->max_fps))
        return FS_ROT_REFUSE_SIZE;
    return FS_ROT_OK;
}

#endif
