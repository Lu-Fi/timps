/* test_hub_idr.c - host-only unit test for the hub's shared IDR clock.
 *
 * An IDR at CBR is the largest frame there is, so every forced one lengthens
 * every consumer's queue and provokes the next round of drops. The recovery
 * path therefore rate-limits per stream and COALESCES what it cannot issue
 * (hub_request_idr_recovery + hub_tick). That leaves one thing easy to get
 * wrong, and easy to reintroduce silently in a refactor: a coalesced request
 * whose keyframe has meanwhile arrived must be CANCELLED. Otherwise a single
 * overflow burst costs two IDRs - one to heal, one an interval later into an
 * encoder whose consumers have already recovered.
 *
 * Both halves of that contract are pinned here: a keyframe cancels a pending
 * request, and an absent keyframe still gets one. Also pinned: the hub counts
 * a consumer's evictions itself, so one that never pops again is counted too.
 *
 * Built with a shortened HUB_IDR_RECOVERY_MIN_US so the test does not have to
 * sleep a real second per case (the header's #ifndef makes the override move
 * hub.c and the test together).
 *
 * Build/run: make test-hub-idr
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "hub.h"
#include "util.h"

#if HUB_IDR_RECOVERY_MIN_US > 200000LL
#error "build with a shortened HUB_IDR_RECOVERY_MIN_US - see the Makefile target"
#endif

static int failures = 0;
static int checks   = 0;
static const char *cur = "";

static void ck_eq(long got, long want, const char *what)
{
    checks++;
    if (got != want) {
        failures++;
        printf("  FAIL [%s] %s: got %ld, want %ld\n", cur, what, got, want);
    }
}

static int g_idr[MS_MAX_VSTREAM];
static void idr_cb(int src) { if ((unsigned)src < MS_MAX_VSTREAM) g_idr[src]++; }

/* a publish the hub will accept: no subscribers needed, the per-frame
 * bookkeeping (and hub_tick) runs before the subscriber snapshot */
static void publish(int src, int keyframe)
{
    static const uint8_t au[8] = { 0,0,0,1, 0x65, 0x11, 0x22, 0x33 };
    hub_publish(src, au, sizeof au, ms_now_us(), keyframe,
                MS_MEDIA_VIDEO, ms_now_us());
}

/* A drop burst asks for recovery once per still-queued packet of the headless
 * GOP. The first request goes out; the rest coalesce. The keyframe that the
 * first one produced then arrives - and must retire the coalesced ones. */
static void t_keyframe_cancels_pending(void)
{
    const int src = 0;
    cur = "keyframe cancels a pending recovery";
    ck_eq(hub_request_idr_recovery(src), 1, "first recovery request is issued");
    ck_eq(g_idr[src], 1, "encoder asked once");
    ck_eq(hub_request_idr_recovery(src), 0, "a second one coalesces");
    ck_eq(hub_request_idr_recovery(src), 0, "and so does a third");
    ck_eq(g_idr[src], 1, "still only one request");

    publish(src, 1);                       /* the IDR we asked for arrives */
    usleep(HUB_IDR_RECOVERY_MIN_US * 2);   /* the deferral interval elapses */
    publish(src, 0);
    publish(src, 0);
    ck_eq(g_idr[src], 1, "no second IDR forced after the keyframe");
}

/* The other half: without a keyframe the coalesced request must still be
 * issued, or a consumer frozen until its next keyframe waits forever. */
static void t_pending_still_fires_without_keyframe(void)
{
    const int src = 1;
    cur = "coalesced recovery still fires";
    ck_eq(hub_request_idr_recovery(src), 1, "first recovery request is issued");
    ck_eq(hub_request_idr_recovery(src), 0, "a second one coalesces");
    ck_eq(g_idr[src], 1, "encoder asked once so far");

    publish(src, 0);
    ck_eq(g_idr[src], 1, "not before the interval has passed");
    usleep(HUB_IDR_RECOVERY_MIN_US * 2);
    publish(src, 0);
    ck_eq(g_idr[src], 2, "the deferred request is issued by hub_tick");
    publish(src, 0);
    ck_eq(g_idr[src], 2, "and only once");
}

/* A keyframe on one stream must not retire another stream's pending request:
 * the clock and the deferral are per video source. */
static void t_cancel_is_per_stream(void)
{
    const int a = 0, b = 1;
    cur = "cancellation is per stream";
    usleep(HUB_IDR_RECOVERY_MIN_US * 2);   /* clear both streams' rate limits */
    int base_a = g_idr[a], base_b = g_idr[b];
    ck_eq(hub_request_idr_recovery(b), 1, "stream b requests recovery");
    ck_eq(hub_request_idr_recovery(b), 0, "and coalesces the next one");
    publish(a, 1);                         /* keyframe on the OTHER stream */
    usleep(HUB_IDR_RECOVERY_MIN_US * 2);
    publish(b, 0);
    ck_eq(g_idr[a] - base_a, 0, "stream a was never asked");
    ck_eq(g_idr[b] - base_b, 2, "stream b's deferred request survived");
}

/* A consumer blocked in send never pops again, so it cannot report its own
 * drops; the hub has to count them at the push that evicted. A queue never
 * registered with hub_count_drops() is not counted. */
static void t_stalled_consumer_counted(void)
{
    const int src = 0;
    cur = "a stalled consumer's evictions are counted";
    fanqueue q, u;
    ck_eq(fanqueue_init(&q, 2), 0, "init counted queue");
    ck_eq(fanqueue_init(&u, 2), 0, "init uncounted queue");
    hub_count_drops(&q, src, HUB_DROP_RTSP);
    ck_eq(hub_subscribe(src, &q), 0, "subscribe counted queue");
    ck_eq(hub_subscribe(src, &u), 0, "subscribe uncounted queue");
    unsigned base = hub_get_drops(src);
    for (int i = 0; i < 5; i++) publish(src, 0);   /* nobody ever pops */
    ck_eq((long)(hub_get_drops(src) - base), 3, "three evicting pushes, three counts");
    hub_unsubscribe(src, &q);
    hub_unsubscribe(src, &u);
    fanqueue_free(&q);
    fanqueue_free(&u);
}

int main(void)
{
    printf("hub shared IDR clock\n\n");
    hub_init();
    hub_set_idr_cb(idr_cb);
    for (int i = 0; i < MS_MAX_VSTREAM; i++)
        hub_set_video_params(i, MS_VC_H264, 640, 480, 15);

    t_keyframe_cancels_pending();
    t_pending_still_fires_without_keyframe();
    t_cancel_is_per_stream();
    t_stalled_consumer_counted();

    printf("\n%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
