/* test_fs_ref.c - host-only test for the FrameSource refcount, the chn0
 * latch-kick hold and the keepalive reference (src/hal/fs_ref.h). IMP
 * Enable/DisableChn are stubs that record every hardware edge.
 * Built once plain (the state machine) and once per SoC/stack with
 * -DFSR_EXPECT_AUTO (the general.fs_keepalive = -1 resolution).
 * Build/run: make test-fs-ref */
#include <stdio.h>
#include <string.h>
#include "hal/fs_ref.h"

#define KICK  500000LL
#define IDLE 2000000LL

static int failures, checks;
static int n_en, n_dis, fail_enable;
static char edges[256];

static void edge(char c, int chn)
{
    size_t l = strlen(edges);
    if (l + 3 < sizeof edges) { edges[l] = c; edges[l+1] = (char)('0' + chn); edges[l+2] = 0; }
}
static int st_en(int chn)  { if (fail_enable) return -1; n_en++; edge('E', chn); return 0; }
static int st_dis(int chn) { n_dis++; edge('D', chn); return 0; }
static const fsr_ops ops = { st_en, st_dis };

static void reset(fsr_state *s)
{
    memset(s, 0, sizeof *s);
    n_en = n_dis = fail_enable = 0;
    edges[0] = 0;
}

#define CK(cond, ...) do { checks++; if (!(cond)) { failures++; \
    printf("  FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define EDGES(want) CK(!strcmp(edges, want), "edges \"%s\", want \"%s\"", edges, want)

static void t_refcount(void)
{
    fsr_state s; reset(&s);
    CK(fsr_use(&s, &ops, 1) == FSR_EDGE, "first use is an edge");
    CK(fsr_use(&s, &ops, 1) == FSR_BUMP, "second use is a bump");
    CK(fsr_unuse(&s, &ops, 1) == 0, "1 user left");
    CK(fsr_unuse(&s, &ops, 1) == 1, "last unuse disables");
    CK(fsr_unuse(&s, &ops, 1) == 0, "unbalanced unuse is ignored");
    EDGES("E1D1");
}

static void t_enable_retry(void)
{
    fsr_state s; reset(&s);
    fail_enable = 1;
    CK(fsr_use(&s, &ops, 0) == FSR_FAIL, "failed enable reported");
    CK(s.users[0] == 1 && !s.enabled[0], "ref kept, channel off");
    fail_enable = 0;
    CK(fsr_use(&s, &ops, 0) == FSR_EDGE, "next use retries the enable");
    fsr_unuse(&s, &ops, 0);
    CK(fsr_unuse(&s, &ops, 0) == 1, "disabled once both are gone");
    EDGES("E0D0");
}

static void t_recycle(void)
{
    fsr_state s; reset(&s);
    fsr_use(&s, &ops, 1);
    fsr_use(&s, &ops, 1);
    CK(fsr_recycle(&s, &ops, 1) == 0, "recycle ok");
    CK(s.users[1] == 2 && s.enabled[1], "recycle is refcount neutral");
    EDGES("E1D1E1");
}

/* idle chn0: one enable, wait, and the disable only after the debounce */
static void t_kick_idle(void)
{
    fsr_state s; reset(&s);
    long long t = 1000000;
    CK(fsr_kick_begin(&s, &ops, t, KICK, IDLE) == FSR_EDGE, "idle kick enables");
    t += KICK;
    fsr_kick_end(&s, t, IDLE);
    CK(s.enabled[0] && s.users[0] == 1, "kick keeps its reference after the wait");
    CK(fsr_kick_reap(&s, &ops, t + IDLE - 1) == 0, "not reaped inside the debounce");
    CK(fsr_kick_reap(&s, &ops, t + IDLE) == 1, "reaped after the debounce");
    CK(!s.kick_held && s.users[0] == 0, "hold released");
    EDGES("E0D0");
}

/* boot kick followed within ms by a consumer: no D->E in between */
static void t_kick_then_consumer(void)
{
    fsr_state s; reset(&s);
    long long t = 0;
    fsr_kick_begin(&s, &ops, t, KICK, IDLE);
    t += KICK;
    fsr_kick_end(&s, t, IDLE);
    t += 5000;
    CK(fsr_use(&s, &ops, 0) == FSR_BUMP, "consumer right after the kick is a bump");
    CK(fsr_kick_reap(&s, &ops, t + IDLE) == 0, "reap with a consumer left does not disable");
    CK(fsr_unuse(&s, &ops, 0) == 1, "consumer leaving disables");
    EDGES("E0D0");
}

/* chn0 already running: no edge, no wait, chn0 kept for the debounce */
static void t_kick_running(void)
{
    fsr_state s; reset(&s);
    long long t = 0;
    fsr_use(&s, &ops, 0);
    edges[0] = 0;
    CK(fsr_kick_begin(&s, &ops, t, KICK, IDLE) == FSR_BUMP, "kick on running chn0 is a bump");
    CK(fsr_unuse(&s, &ops, 0) == 0, "consumer leaving right after the Set does not disable");
    CK(fsr_kick_reap(&s, &ops, t + IDLE) == 1, "hold reaped after the debounce");
    EDGES("D0");
}

/* repeated kicks share one reference and push the deadline */
static void t_kick_repeat(void)
{
    fsr_state s; reset(&s);
    long long t = 0;
    fsr_kick_begin(&s, &ops, t, KICK, IDLE);
    fsr_kick_end(&s, t + KICK, IDLE);
    CK(fsr_kick_begin(&s, &ops, t + 1000000, KICK, IDLE) == FSR_BUMP, "second kick is a bump");
    CK(s.users[0] == 1, "one reference for both kicks");
    CK(fsr_kick_reap(&s, &ops, t + KICK + IDLE) == 0, "deadline was extended");
    CK(fsr_kick_reap(&s, &ops, t + 1000000 + IDLE) == 1, "reaped after the later deadline");
    EDGES("E0D0");
}

/* a concurrent kick during the first one's wait cannot shorten it */
static void t_kick_concurrent(void)
{
    fsr_state s; reset(&s);
    fsr_kick_begin(&s, &ops, 0, KICK, IDLE);
    fsr_kick_begin(&s, &ops, 100, KICK, IDLE);
    CK(fsr_kick_reap(&s, &ops, KICK) == 0, "not reaped during the first wait");
    fsr_kick_end(&s, KICK, IDLE);
    CK(fsr_kick_reap(&s, &ops, KICK + IDLE) == 1, "reaped after the first kick's debounce");
    EDGES("E0D0");
}

/* old behaviour for contrast: kick then immediate unuse put D right before
 * the next E. The new path must not produce "D0E0" for the same sequence. */
static void t_no_disable_enable_pair(void)
{
    fsr_state s; reset(&s);
    long long t = 0;
    fsr_kick_begin(&s, &ops, t, KICK, IDLE);
    fsr_kick_end(&s, t + KICK, IDLE);
    fsr_use(&s, &ops, 0);                      /* timelapse grab */
    fsr_kick_reap(&s, &ops, t + KICK + 1000);  /* idle loop tick */
    CK(strstr(edges, "D0E0") == NULL, "no D->E pair: \"%s\"", edges);
}

static void t_teardown(void)
{
    fsr_state s; reset(&s);
    int leaked;
    fsr_kick_begin(&s, &ops, 0, KICK, IDLE);
    CK(fsr_teardown(&s, &ops, 0, &leaked) == 0 && leaked == 0,
       "kick hold is released by teardown, not reported as a leak");
    CK(!s.kick_held && !s.enabled[0] && s.users[0] == 0, "teardown zeroes state");
    fsr_kick_end(&s, KICK, IDLE);
    CK(!s.kick_held, "kick ending after teardown re-arms nothing");
    EDGES("E0D0");

    reset(&s);
    fsr_use(&s, &ops, 1);
    fsr_teardown(&s, &ops, 1, &leaked);
    CK(leaked == 1, "a foreign reference is reported");
    CK(fsr_teardown(&s, &ops, 1, &leaked) == 0 && leaked == 0, "second teardown is a no-op");
    EDGES("E1D1");
}

/* keepalive: one enable per channel, consumers never cause an edge */
static void t_keep_consumers(void)
{
    fsr_state s; reset(&s);
    CK(fsr_keep_acquire(&s, &ops, 0) == FSR_EDGE, "acquire enables chn0");
    CK(fsr_keep_acquire(&s, &ops, 1) == FSR_EDGE, "acquire enables chn1");
    CK(fsr_keep_acquire(&s, &ops, 0) == FSR_BUMP && s.users[0] == 1,
       "second acquire takes no second reference");
    for (int i = 0; i < 100; i++) {             /* client churn, timelapse */
        fsr_use(&s, &ops, i & 1);
        fsr_unuse(&s, &ops, i & 1);
    }
    fsr_use(&s, &ops, 1);                       /* motion pin */
    fsr_unuse(&s, &ops, 1);
    EDGES("E0E1");
    CK(fsr_kept(&s, 0) && fsr_kept(&s, 1) && !fsr_kept(&s, 2), "kept flags");
}

/* the T41 repro: chn0 idle-stopped and restarted within 200 ms while chn1
 * streams. With the keepalive it is no edge at all. */
static void t_keep_t41_repro(void)
{
    fsr_state s; reset(&s);
    fsr_keep_acquire(&s, &ops, 0);
    fsr_keep_acquire(&s, &ops, 1);
    fsr_use(&s, &ops, 1);                       /* sub stream client */
    edges[0] = 0;
    fsr_use(&s, &ops, 0);
    fsr_unuse(&s, &ops, 0);                     /* idle stop */
    fsr_use(&s, &ops, 0);                       /* < 200 ms later */
    EDGES("");
}

/* with chn0 kept the kick is a no-op: no hold, no wait */
static void t_keep_kick(void)
{
    fsr_state s; reset(&s);
    fsr_keep_acquire(&s, &ops, 0);
    edges[0] = 0;
    CK(fsr_kick_begin(&s, &ops, 0, KICK, IDLE) == FSR_BUMP, "kick is a bump");
    CK(!s.kick_held && s.users[0] == 1, "kick takes no reference");
    CK(fsr_kick_reap(&s, &ops, 10 * IDLE) == 0, "nothing to reap");
    EDGES("");
}

/* EnableChn failed at acquire: the reference stays, the next user retries,
 * and the kick still works as without keepalive */
static void t_keep_enable_fail(void)
{
    fsr_state s; reset(&s);
    fail_enable = 1;
    CK(fsr_keep_acquire(&s, &ops, 0) == FSR_FAIL, "failed acquire reported");
    CK(fsr_kept(&s, 0) && s.users[0] == 1, "keep reference held anyway");
    fail_enable = 0;
    CK(fsr_kick_begin(&s, &ops, 0, KICK, IDLE) == FSR_EDGE, "kick retries the enable");
    fsr_kick_end(&s, KICK, IDLE);
    CK(fsr_kick_reap(&s, &ops, KICK + IDLE) == 0, "kick release keeps the kept channel on");
    EDGES("E0");
}

/* a kept channel is never cycled, not even by a watchdog */
static void t_keep_recycle(void)
{
    fsr_state s; reset(&s);
    fsr_keep_acquire(&s, &ops, 0);
    fsr_use(&s, &ops, 0);
    CK(fsr_recycle(&s, &ops, 0) == 1 && s.users[0] == 2 && s.enabled[0],
       "kept recycle refused, state untouched");
    EDGES("E0");
}

/* video_thread's watchdog step: the edges one
 * failed-cycle streak produces up to the escalation */
static void wd_streak(fsr_state *s, int chn, int max, int *gave_up_at)
{
    *gave_up_at = 0;
    for (int a = 1; a <= max + 1; a++) {
        int act = fsr_wd_action(s, chn, a, max);
        if (act == FSR_WD_GIVE_UP) { *gave_up_at = a; break; }
        if (act == FSR_WD_RECYCLE && (!s->enabled[chn] || fsr_recycle(s, &ops, chn) != 0)) {
            fsr_unuse(s, &ops, chn);
            fsr_use(s, &ops, chn);
        }
    }
}

static void t_wd(void)
{
    fsr_state s; int g;
    reset(&s);
    fsr_use(&s, &ops, 0);                       /* encoder */
    fsr_use(&s, &ops, 0);                       /* motion pin */
    edges[0] = 0;
    CK(fsr_wd_action(&s, 0, 1, 5) == FSR_WD_RECYCLE, "not kept: recycle");
    wd_streak(&s, 0, 5, &g);
    CK(g == 5, "gives up at the budget, got %d", g);
    EDGES("D0E0D0E0D0E0D0E0");
    CK(s.users[0] == 2 && s.enabled[0], "streak is refcount neutral");

    reset(&s);
    fsr_keep_acquire(&s, &ops, 1);
    fsr_use(&s, &ops, 1);
    edges[0] = 0;
    CK(fsr_wd_action(&s, 1, 1, 5) == FSR_WD_ENCODER, "kept: encoder only");
    CK(fsr_wd_action(&s, 1, 4, 5) == FSR_WD_ENCODER, "kept: still encoder only");
    CK(fsr_wd_action(&s, 1, 5, 5) == FSR_WD_GIVE_UP, "kept: escalates at the budget");
    wd_streak(&s, 1, 5, &g);
    CK(g == 5, "kept gives up at the budget, got %d", g);
    EDGES("");

    /* kept but EnableChn had failed: retrying the enable is no edge on a
     * running channel */
    reset(&s);
    fail_enable = 1;
    fsr_keep_acquire(&s, &ops, 0);
    fsr_use(&s, &ops, 0);
    fail_enable = 0;
    CK(fsr_wd_action(&s, 0, 1, 5) == FSR_WD_RECYCLE, "kept, off: retry the enable");
    CK(fsr_recycle(&s, &ops, 0) == 1, "...but never via a D+E");
    fsr_unuse(&s, &ops, 0);
    fsr_use(&s, &ops, 0);
    EDGES("E0");
    CK(fsr_wd_action(&s, 0, 2, 5) == FSR_WD_ENCODER, "kept, now on: encoder only");
}

#ifdef FSR_EXPECT_AUTO
/* per SoC/stack: with general.fs_keepalive = -1 the watchdogs never cycle a
 * FrameSource where the keepalive is on by default */
static void t_wd_platform(void)
{
    fsr_state s; int g;
    reset(&s);
    int keep = fs_keepalive_resolve(-1, FS_KEEPALIVE_AUTO);
    for (int c = 0; c < 2; c++) {
        if (keep) fsr_keep_acquire(&s, &ops, c);
        fsr_use(&s, &ops, c);
    }
    edges[0] = 0;
    wd_streak(&s, 0, 5, &g);
    CK(g == 5, "escalates at the budget, got %d", g);
    if (keep) EDGES("");
    else      EDGES("D0E0D0E0D0E0D0E0");
}
#endif

/* teardown: keep released first, no leak warning, exactly one disable each,
 * and a restart (ing_stop -> ing_start) starts from zero */
static void t_keep_teardown(void)
{
    fsr_state s; reset(&s);
    int leaked;
    fsr_keep_acquire(&s, &ops, 0);
    fsr_keep_acquire(&s, &ops, 1);
    fsr_use(&s, &ops, 0); fsr_unuse(&s, &ops, 0);   /* threads joined */
    CK(fsr_teardown(&s, &ops, 0, &leaked) == 0 && leaked == 0, "chn0 no leak");
    CK(fsr_teardown(&s, &ops, 1, &leaked) == 0 && leaked == 0, "chn1 no leak");
    CK(!fsr_kept(&s, 0) && !fsr_kept(&s, 1), "keep cleared");
    EDGES("E0E1D0D1");
    CK(fsr_keep_acquire(&s, &ops, 0) == FSR_EDGE, "restart acquires afresh");

    reset(&s);
    fsr_keep_acquire(&s, &ops, 2);
    fsr_use(&s, &ops, 2);                         /* a holder that leaked */
    fsr_teardown(&s, &ops, 2, &leaked);
    CK(leaked == 1, "a real leak is still reported");
}

static void t_resolve(void)
{
    CK(fs_keepalive_resolve(-1, 1) == 1 && fs_keepalive_resolve(-1, 0) == 0, "auto follows the platform");
    CK(fs_keepalive_resolve(0, 1) == 0, "0 overrides auto on");
    CK(fs_keepalive_resolve(1, 0) == 1, "1 overrides auto off");
}

int main(void)
{
#ifdef FSR_EXPECT_AUTO
    CK(FS_KEEPALIVE_AUTO == FSR_EXPECT_AUTO, "FS_KEEPALIVE_AUTO = %d, want %d",
       FS_KEEPALIVE_AUTO, FSR_EXPECT_AUTO);
    CK(fs_keepalive_resolve(-1, FS_KEEPALIVE_AUTO) == FSR_EXPECT_AUTO, "auto resolution");
    CK(fs_keepalive_resolve(0, FS_KEEPALIVE_AUTO) == 0, "explicit 0");
    CK(fs_keepalive_resolve(1, FS_KEEPALIVE_AUTO) == 1, "explicit 1");
#endif
    t_refcount();
    t_enable_retry();
    t_recycle();
    t_kick_idle();
    t_kick_then_consumer();
    t_kick_running();
    t_kick_repeat();
    t_kick_concurrent();
    t_no_disable_enable_pair();
    t_teardown();
    t_keep_consumers();
    t_keep_t41_repro();
    t_keep_kick();
    t_keep_enable_fail();
    t_keep_recycle();
    t_keep_teardown();
    t_resolve();
    t_wd();
#ifdef FSR_EXPECT_AUTO
    t_wd_platform();
    printf("test_fs_ref %-7s auto=%d: %d/%d checks passed\n", FSR_LABEL,
           FS_KEEPALIVE_AUTO, checks - failures, checks);
#else
    printf("test_fs_ref: %d/%d checks passed\n", checks - failures, checks);
#endif
    return failures ? 1 : 0;
}
