/* test_fs_rot.c - host-only test for the FrameSource-rotate safe envelope,
 * the T23 software-rotate envelope and the T1x SetChnRotate verification
 * (src/hal/fs_rot_env.h): the decisions fs_create()/sw_rot_start() turn into
 * an UNROTATED fallback, and the ERR_ROT_* code /control reports for them.
 * Build/run: make test-fs-rot */
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

static void sw(int ew, int eh, int fps, int want)
{
    int got = sw_rot_check(ew, eh, fps);
    checks++;
    if (got != want) {
        failures++;
        printf("  FAIL T23 sw %dx%d@%d: got %d, want %d\n", ew, eh, fps, got, want);
    }
}

/* IMP_FrameSource_SetChnRotate as the OpenIMP pins implement it on T1x */
static int calls, last_rot, last_w, last_h;
static void track(int rot, int w, int h) { calls++; last_rot = rot; last_w = w; last_h = h; }

static int oimp_9cab219(int chn, int rot, int h, int w)     /* stub */
{
    track(rot, w, h);
    return (chn < 0 || chn >= 5) ? -1 : 0;
}
static int oimp_6857610(int chn, int rot, int w, int h)     /* refusing stub */
{
    track(rot, w, h);
    if (chn < 0 || chn >= 5) return -1;
    return (rot & 0xff) == 0 ? 0 : -1;
}
static int oimp_6b7f5ae(int chn, int rot, int w, int h)     /* T1x rotation */
{
    int mode = rot & 0xff;
    track(rot, w, h);
    if (chn < 0 || chn >= 5) return -1;
    if (mode == 0) return 0;
    if (mode > 3 || w <= 0 || h <= 0 || w > 4096 || h > 4096 || ((w | h) & 1))
        return -1;
    if ((w | h) & 15) return -1;
    if ((long)w * h > 704L * 576L) return -1;
    return 0;
}

static void cs(const char *what, fs_rot_set_fn fn, int chn, int rot, int w,
               int h, int want, int want_last_rot)
{
    calls = 0; last_rot = -1;
    int got = fs_rot_set(fn, chn, rot, w, h);
    checks++;
    if (got != want || (want_last_rot >= 0 && last_rot != want_last_rot)) {
        failures++;
        printf("  FAIL %s chn%d rot%d %dx%d: got %d (last rot %d), want %d (%d)\n",
               what, chn, rot, w, h, got, last_rot, want, want_last_rot);
    }
}

int main(void)
{
    const fs_rot_env t31 = FS_ROT_ENV_T31, t1x = FS_ROT_ENV_T1X;

    ck("T1x", &t1x, 1, 640, 368, 15, ERR_OK);
    ck("T1x", &t1x, 1, 640, 360, 15, ERR_ROT_ALIGN);
    ck("T1x", &t1x, 1, 704, 576, 15, ERR_OK);
    ck("T1x", &t1x, 1, 704, 576, 30, ERR_OK);          /* no fps cap */
    ck("T1x", &t1x, 1, 720, 576, 15, ERR_ROT_SIZE);
    ck("T1x", &t1x, 1, 1280, 720, 15, ERR_ROT_SIZE);
    ck("T1x", &t1x, 0, 640, 368, 15, ERR_ROT_MAIN);
    ck("T1x", &t1x, 0, 1920, 1080, 25, ERR_ROT_MAIN);
    ck("T1x", &t1x, 2, 368, 640, 15, ERR_OK);

    ck("T31", &t31, 0, 1280, 704, 15, ERR_OK);         /* main allowed */
    ck("T31", &t31, 1, 640, 384, 15, ERR_OK);
    ck("T31", &t31, 0, 1920, 1088, 15, ERR_ROT_SIZE);
    ck("T31", &t31, 0, 1920, 1080, 25, ERR_ROT_ALIGN);
    ck("T31", &t31, 1, 640, 368, 15, ERR_ROT_ALIGN);
    ck("T31", &t31, 1, 1280, 704, 20, ERR_ROT_FPS);
    ck("T31", &t31, 1, 1920, 1088, 25, ERR_ROT_SIZE);     /* size before fps */

    /* post-rotation dims */
    sw(576, 704, 15, ERR_OK);
    sw(368, 640, 15, ERR_OK);
    sw(360, 640, 15, ERR_ROT_ALIGN);
    sw(240, 320, 15, ERR_ROT_SMALL);
    sw(720, 1280, 15, ERR_ROT_SIZE);
    sw(576, 704, 20, ERR_ROT_FPS);
    sw(720, 1280, 25, ERR_ROT_SIZE);

    /* the codes are the documented, stable /control numbers */
    checks++;
    if (ERR_ROT_UNSUPPORTED != 201 || ERR_ROT_MAIN != 202 || ERR_ROT_ALIGN != 203 ||
        ERR_ROT_SIZE != 204 || ERR_ROT_FPS != 205 || ERR_ROT_LIB != 206 ||
        ERR_ROT_REFUSED != 207 || ERR_ROT_SMALL != 208) {
        failures++;
        printf("  FAIL ERR_ROT_* numbering changed\n");
    }

    cs("no symbol", NULL, 1, 1, 640, 368, FS_ROT_SET_MISSING, -1);
    /* the stub is left at rotation 0 */
    cs("9cab219", oimp_9cab219, 1, 1, 640, 368, FS_ROT_SET_STUB, 0);
    cs("9cab219", oimp_9cab219, 1, 2, 368, 640, FS_ROT_SET_STUB, 0);
    cs("6857610", oimp_6857610, 1, 1, 640, 368, FS_ROT_SET_REFUSED, 1);
    cs("6857610", oimp_6857610, 1, 2, 640, 368, FS_ROT_SET_REFUSED, 2);
    /* the last call is the real request */
    cs("6b7f5ae", oimp_6b7f5ae, 1, 1, 640, 368, FS_ROT_SET_OK, 1);
    cs("6b7f5ae", oimp_6b7f5ae, 2, 2, 704, 576, FS_ROT_SET_OK, 2);
    cs("6b7f5ae", oimp_6b7f5ae, 1, 1, 640, 360, FS_ROT_SET_REFUSED, 1);
    cs("6b7f5ae", oimp_6b7f5ae, 1, 1, 1280, 720, FS_ROT_SET_REFUSED, 1);

    printf("test_fs_rot: %d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
