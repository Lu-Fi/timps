/* test_fs_ref.c - host-only test for the FrameSource refcount and the chn0
 * latch-kick hold (src/hal/fs_ref.h). IMP Enable/DisableChn are stubs that
 * record every hardware edge. Build/run: make test-fs-ref */
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

int main(void)
{
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
    printf("test_fs_ref: %d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
