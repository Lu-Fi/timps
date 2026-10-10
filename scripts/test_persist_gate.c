/* test_persist_gate.c - host-only test for the /control config-write rate
 * limit (src/persist_gate.h), virtual clock. Build/run: make test-persist-gate */
#include <stdio.h>
#include "persist_gate.h"

static int failures, checks;

#define CK(cond, ...) do { checks++; if (!(cond)) { failures++; \
    printf("  FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define S 1000000LL

/* the control.c sequence: post -> maybe write; the 1 s tick writes when due */
static int writes;
static void post(pgate *g, long long now, int urgent, int ok)
{
    if (pg_post(g, now, urgent) || pg_due(g, now)) { writes++; pg_wrote(g, now, ok); }
}
static void tick(pgate *g, long long now, int ok)
{
    if (pg_due(g, now)) { writes++; pg_wrote(g, now, ok); }
}

static void t_single(void)
{
    pgate g = {0}; writes = 0;
    post(&g, 100*S, 0, 1);
    CK(writes == 1 && !g.pending, "first change after boot is written at once");
    post(&g, 200*S, 0, 1);
    CK(writes == 2, "a change after a quiet window is written at once");
}

static void t_slider(void)
{
    pgate g = {0}; writes = 0;
    long long t = 100*S;
    post(&g, t, 0, 1);                              /* leading edge */
    for (int i = 1; i <= 50; i++) {                 /* 20 posts/s for 2.5 s */
        post(&g, t + i * S / 20, 0, 1);
        if (i % 20 == 0) tick(&g, t + i * S / 20, 1);
    }
    CK(writes == 1 && g.pending, "drag inside the window: one write, rest pending");
    CK(g.due == t + PG_WINDOW_US, "due at the end of the window");
    tick(&g, t + PG_WINDOW_US - 1, 1);
    CK(writes == 1, "not before the window ends");
    tick(&g, t + PG_WINDOW_US, 1);
    CK(writes == 2 && !g.pending, "trailing write at the window end");
    /* a long drag: at most one write per window */
    writes = 0;
    long long t0 = t + PG_WINDOW_US;
    for (long long u = t0; u < t0 + 30*S; u += S/20) {
        post(&g, u, 0, 1);
        if ((u - t0) % S == 0) tick(&g, u, 1);
    }
    CK(writes <= 30*S / PG_WINDOW_US + 1, "30 s drag: %d writes", writes);
    CK(writes >= 30*S / PG_WINDOW_US - 1, "...but still writes every window: %d", writes);
}

static void t_urgent(void)
{
    pgate g = {0}; writes = 0;
    post(&g, 100*S, 0, 1);
    post(&g, 100*S + S/2, 0, 1);
    CK(writes == 1 && g.pending, "pending");
    post(&g, 100*S + S, 1, 1);
    CK(writes == 2 && !g.pending, "restart-required key flushes everything at once");
}

static void t_late_tick(void)
{
    pgate g = {0}; writes = 0;
    post(&g, 100*S, 0, 1);
    post(&g, 101*S, 0, 1);
    post(&g, 104*S, 0, 1);                          /* tick missed (busy mutex) */
    CK(writes == 2 && !g.pending, "an overdue set is written by the next POST");
}

static void t_fail(void)
{
    pgate g = {0}; writes = 0;
    post(&g, 100*S, 0, 0);
    CK(writes == 1 && g.pending && g.failing, "failed write stays pending");
    tick(&g, 100*S + PG_WINDOW_US, 0);
    CK(writes == 1, "no retry every window on a failing /etc");
    post(&g, 101*S, 0, 0);
    CK(writes == 2, "while failing, every POST tries (its reply reports it)");
    tick(&g, 101*S + PG_RETRY_US, 1);
    CK(writes == 3 && !g.pending && !g.failing, "tick retries after the backoff");
    post(&g, 101*S + PG_RETRY_US + S, 0, 1);
    CK(writes == 3 && g.pending, "recovered: rate limit again");
}

int main(void)
{
    t_single(); t_slider(); t_urgent(); t_late_tick(); t_fail();
    printf("persist gate: %d/%d passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
