/* test_hub_hold.c - host-only unit test for the snapshot warm-up hold
 * (hub_set_image_ready_at() / hub_grab_jpeg(), src/hub.c).
 *
 * Idle-Dark switches the IR illuminator on when a consumer arrives, and the
 * AE needs a few seconds on it: the first JPEG after the wake is dark or
 * overexposed. hub_grab_jpeg() therefore discards JPEGs until the ready
 * instant the day/night thread published - set synchronously from the
 * activity callback inside hub_subscribe(), i.e. before the grab's first
 * pop - with its deadline extended by the hold and the hold capped.
 *
 * Built with a shortened HUB_IMAGE_HOLD_MAX_MS so the cap case runs fast.
 *
 * Build/run: make test-hub-hold
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include "hub.h"
#include "frame.h"
#include "util.h"

#if HUB_IMAGE_HOLD_MAX_MS > 1000
#error "build with a shortened HUB_IMAGE_HOLD_MAX_MS - see the Makefile target"
#endif

static int failures = 0;
static int checks   = 0;
static const char *cur = "";

static void ck(int ok, const char *what, long got)
{
    checks++;
    if (!ok) {
        failures++;
        printf("  FAIL [%s] %s (got %ld)\n", cur, what, got);
    }
}

/* a JPEG source publishing every 20 ms; the payload is its publish time */
static volatile int g_run = 1, g_publish = 1;
static void *publisher(void *arg)
{
    (void)arg;
    while (g_run) {
        if (g_publish) {
            int64_t t = ms_now_us();
            uint8_t b[16] = { 0xff, 0xd8 };
            memcpy(b + 4, &t, sizeof t);
            hub_publish(HUB_JPEG_SRC, b, sizeof b, t, 1, MS_MEDIA_JPEG, t);
        }
        usleep(20000);
    }
    return NULL;
}

static int64_t stamp(ms_pkt *p)
{
    int64_t t = 0;
    if (p && p->len >= 12) memcpy(&t, p->data + 4, sizeof t);
    return t;
}

/* what daynight_watch_kick() does from the trampoline while dark */
static int64_t g_hold_us;
static void act_cb(int src, int on)
{
    if (on && src == HUB_JPEG_SRC && g_hold_us)
        hub_set_image_ready_at(ms_now_us() + g_hold_us);
}

static ms_pkt *grab(int wait_ms, int64_t *took_us)
{
    int64_t t0 = ms_now_us();
    ms_pkt *p = hub_grab_jpeg(HUB_JPEG_SRC, wait_ms, NULL, NULL, NULL);
    *took_us = ms_now_us() - t0;
    return p;
}

static void t_no_hold(void)
{
    cur = "no hold";
    g_hold_us = 0;
    hub_set_image_ready_at(0);
    int64_t took;
    ms_pkt *p = grab(1500, &took);
    ck(p != NULL, "a JPEG", 0);
    ck(took < 150000, "the first one, at once", (long)took);
    if (p) pkt_unref(p);
}

static void t_hold_from_subscribe(void)
{
    cur = "hold set by the subscribe";
    g_hold_us = 300000;
    int64_t t0 = ms_now_us(), took;
    ms_pkt *p = grab(1500, &took);
    ck(p != NULL, "a JPEG", 0);
    ck(took >= 300000, "not before the ready instant", (long)took);
    ck(took < 300000 + 150000, "the next one after it", (long)took);
    ck(stamp(p) >= t0 + 300000 - 25000, "published at most one frame early",
       (long)(stamp(p) - t0));
    if (p) pkt_unref(p);
    g_hold_us = 0;
}

static void t_hold_extends_deadline(void)
{
    cur = "hold longer than the wait";
    g_hold_us = 300000;
    int64_t took;
    ms_pkt *p = grab(100, &took);
    ck(p != NULL, "still answered", 0);
    ck(took >= 300000, "after the hold", (long)took);
    if (p) pkt_unref(p);
    g_hold_us = 0;
}

static void t_hold_capped(void)
{
    cur = "hold capped";
    hub_set_image_ready_at(ms_now_us() + 60LL * 1000000);
    int64_t took;
    ms_pkt *p = grab(1500, &took);
    ck(p != NULL, "answered", 0);
    ck(took >= (int64_t)HUB_IMAGE_HOLD_MAX_MS * 1000, "held to the cap", (long)took);
    ck(took < (int64_t)HUB_IMAGE_HOLD_MAX_MS * 1000 + 150000, "not longer", (long)took);
    if (p) pkt_unref(p);
    hub_set_image_ready_at(0);
}

static void t_hold_passed(void)
{
    cur = "ready instant already passed";
    hub_set_image_ready_at(ms_now_us() - 1000000);
    int64_t took;
    ms_pkt *p = grab(1500, &took);
    ck(p != NULL && took < 150000, "no hold", (long)took);
    if (p) pkt_unref(p);
}

static void t_no_frames(void)
{
    cur = "hold with a silent source";
    g_publish = 0;
    usleep(50000);
    hub_set_image_ready_at(ms_now_us() + 200000);
    int64_t took;
    ms_pkt *p = grab(100, &took);
    ck(p == NULL, "times out", 0);
    ck(took >= 300000 && took < 600000, "after hold + wait", (long)took);
    if (p) pkt_unref(p);
    g_publish = 1;
    hub_set_image_ready_at(0);
}

int main(void)
{
    printf("hub snapshot warm-up hold\n\n");
    hub_init();
    hub_set_activity_cb(act_cb);
    pthread_t th;
    pthread_create(&th, NULL, publisher, NULL);

    t_no_hold();
    t_hold_from_subscribe();
    t_hold_extends_deadline();
    t_hold_capped();
    t_hold_passed();
    t_no_frames();

    g_run = 0;
    pthread_join(th, NULL);
    printf("\n%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
