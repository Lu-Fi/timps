/* test_fs_rot.c - host-only test for the FrameSource-rotate safe envelope
 * (src/hal/fs_rot_env.h), the decision fs_create() turns into an UNROTATED
 * fallback. Build/run: make test-fs-rot */
#include <stdio.h>
#include "hal/fs_rot_env.h"

static int failures, checks;

static void ck(const char *soc, const fs_rot_env *e, int chn, int w, int h,
               int fps, int want)
{
    int got = fs_rot_check(e, chn, w, h, fps);
    checks++;
    if (got != want) {
        failures++;
        printf("  FAIL %s chn%d %dx%d@%d: got %d, want %d\n",
               soc, chn, w, h, fps, got, want);
    }
}

int main(void)
{
    const fs_rot_env t31 = FS_ROT_ENV_T31, t1x = FS_ROT_ENV_T1X;

    ck("T1x", &t1x, 1, 640, 368, 15, FS_ROT_OK);
    ck("T1x", &t1x, 1, 640, 360, 15, FS_ROT_REFUSE_ALIGN);
    ck("T1x", &t1x, 1, 704, 576, 15, FS_ROT_OK);
    ck("T1x", &t1x, 1, 704, 576, 30, FS_ROT_OK);          /* no fps cap */
    ck("T1x", &t1x, 1, 720, 576, 15, FS_ROT_REFUSE_SIZE);
    ck("T1x", &t1x, 1, 1280, 720, 15, FS_ROT_REFUSE_SIZE);
    ck("T1x", &t1x, 0, 640, 368, 15, FS_ROT_REFUSE_MAIN);
    ck("T1x", &t1x, 0, 1920, 1080, 25, FS_ROT_REFUSE_MAIN);
    ck("T1x", &t1x, 2, 368, 640, 15, FS_ROT_OK);

    ck("T31", &t31, 0, 1280, 704, 15, FS_ROT_OK);         /* main allowed */
    ck("T31", &t31, 1, 640, 384, 15, FS_ROT_OK);
    ck("T31", &t31, 0, 1920, 1088, 15, FS_ROT_REFUSE_SIZE);
    ck("T31", &t31, 0, 1920, 1080, 25, FS_ROT_REFUSE_ALIGN);
    ck("T31", &t31, 1, 640, 368, 15, FS_ROT_REFUSE_ALIGN);
    ck("T31", &t31, 1, 1280, 704, 20, FS_ROT_REFUSE_SIZE);

    printf("test_fs_rot: %d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
