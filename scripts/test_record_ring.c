/* test_record_ring.c - host-only unit test for record.c's motion pre-roll ring
 * (ring_push(), opened for test via -DREC_RING_TEST).
 *
 * The bug this guards: the ring was trimmed by time alone, so the keyframe the
 * pre-roll window starts decoding at was dropped as soon as it aged past
 * pre_roll_s. flush_ring() then started at the NEXT keyframe - at the fleet's
 * gop=50 @15fps up to 3.3 s later - so a 3 s pre-roll came out anywhere
 * between 3 s and ~0 s.
 *
 * Built and run by `make test-record-ring`.
 */
#include <stdio.h>
#include <stdint.h>
#include "frame.h"

extern void    rec_ring_push_test(ms_pkt *p, int64_t pre_us);
extern void    rec_ring_clear_test(void);
extern int64_t rec_ring_start_test(void);
extern int     rec_ring_count_test(void);

static int g_fail;

#include "record.h"

static void clip_cap(void)
{
    /* 3000 kbps = 391 KB/s with the margin */
    int a = rec_clip_cap_s(30, 3000, -1, -1);
    int b = rec_clip_cap_s(30, 3000, 16, 64);   /* 16 MB tmpfs: 8 MB usable */
    int c = rec_clip_cap_s(30, 3000, 32, 6);    /* 6 MB available */
    int d = rec_clip_cap_s(6, 3000, 0, 64);     /* /tmp full */
    int e = rec_clip_cap_s(6, 0, 1, -1);        /* unknown bitrate: 4000 assumed */
    if (a != 30) { printf("FAIL clip cap unknown: %d\n", a); g_fail = 1; }
    if (b != 21) { printf("FAIL clip cap 16 MB: want 21 got %d\n", b); g_fail = 1; }
    if (c != 8)  { printf("FAIL clip cap 6 MB avail: want 8 got %d\n", c); g_fail = 1; }
    if (d != 0)  { printf("FAIL clip cap /tmp full: %d\n", d); g_fail = 1; }
    if (e != 1)  { printf("FAIL clip cap 1 MB: want 1 got %d\n", e); g_fail = 1; }
}

static void push(int64_t pts, int key, int64_t pre_us)
{
    static const uint8_t b[16];
    ms_pkt *p = pkt_new(b, sizeof b, pts, key, MS_MEDIA_VIDEO);
    rec_ring_push_test(p, pre_us);
    pkt_unref(p);
}

/* 15 fps, keyframe every `gop` frames; after `frames`, the pre-roll start must
 * lie at least pre_us before the newest frame */
static void run(const char *what, int gop, int frames, int64_t pre_us)
{
    rec_ring_clear_test();
    int64_t pts = 0, step = 1000000 / 15;
    for (int i = 0; i < frames; i++, pts += step)
        push(pts, i % gop == 0, pre_us);
    int64_t newest = pts - step, start = rec_ring_start_test();
    if (start < 0 || newest - start < pre_us) {
        fprintf(stderr, "FAIL %s: pre-roll %.2fs, want >= %.2fs\n", what,
                start < 0 ? -1.0 : (newest - start) / 1e6, pre_us / 1e6);
        g_fail = 1;
    } else if (newest - start > pre_us + (int64_t)gop * step) {
        fprintf(stderr, "FAIL %s: pre-roll %.2fs holds more than one extra GOP\n",
                what, (newest - start) / 1e6);
        g_fail = 1;
    } else
        printf("ok %s: pre-roll %.2fs (%d packets)\n", what,
               (newest - start) / 1e6, rec_ring_count_test());
}

int main(void)
{
    run("gop=50 pre=3s, just past a keyframe", 50, 101, 3000000);
    run("gop=50 pre=3s, just before a keyframe", 50, 149, 3000000);
    run("gop=50 pre=3s, mid-GOP", 50, 180, 3000000);
    run("gop=15 pre=5s", 15, 400, 5000000);
    run("gop=50 pre=1s", 50, 170, 1000000);
    rec_ring_clear_test();
    clip_cap();
    if (g_fail) { fprintf(stderr, "FAILED\n"); return 1; }
    printf("all record pre-roll ring tests passed\n");
    return 0;
}
