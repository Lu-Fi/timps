/* test_stopgate.c - host-only test for the stop gate's kick (src/util.c),
 * which wakes the day/night thread when a consumer arrives during Idle-Dark.
 * Build/run: make test-stopgate */
#include <stdio.h>
#include "util.h"

static int failures, checks;

#define CK(cond, ...) do { checks++; if (!(cond)) { failures++; \
    printf("  FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void t_stopgate_kick(void)
{
    ms_stopgate g;
    ms_stopgate_init(&g);
    int64_t t0 = ms_now_us();
    ms_stopgate_kick(&g);
    CK(ms_stopgate_wait(&g, 30) == 0, "plain wait ignores a kick");
    CK(ms_now_us() - t0 >= 25000, "and sleeps its interval");
    t0 = ms_now_us();
    CK(ms_stopgate_wait_kick(&g, 5000) == 2, "a pending kick ends wait_kick");
    CK(ms_now_us() - t0 < 1000000, "at once");
    CK(ms_stopgate_wait_kick(&g, 20) == 0, "the kick was consumed");
    ms_stopgate_kick(&g);
    ms_stopgate_stop(&g);
    CK(ms_stopgate_wait_kick(&g, 5000) == 1, "stop wins over a kick");
    CK(ms_stopgate_wait(&g, 5000) == 1, "stopped gate");
}

int main(void)
{
    t_stopgate_kick();
    printf("test_stopgate: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
