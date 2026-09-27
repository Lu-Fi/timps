/* test_timelapse_prune.c - host-only unit test for timelapse.c's retention
 * cutoff (prune_cutoff(), opened for test via -DTL_PRUNE_TEST).
 *
 * The bug this guards: the cutoff was time(NULL)-(time_t)days*86400 in the
 * target's 32-bit time_t, so a large timelapse.keep_days wrapped it into the
 * future and every hourly prune deleted the whole archive. The property
 * checked is target-width independent: a cutoff is either skipped or lies in
 * [0, now] - so it always fits a 32-bit time_t when now does.
 *
 * Built and run by `make test-timelapse-prune`.
 */
#include <stdio.h>
#include <stdint.h>
#include <limits.h>
#include <time.h>

extern int tl_prune_cutoff_test(int64_t now, int days, time_t *cutoff);

static int g_fail;

static void check(int64_t now, int days, int want_prune, int64_t want_cut)
{
    time_t c = (time_t)-1;
    int r = tl_prune_cutoff_test(now, days, &c);
    if (r != want_prune || (r && ((int64_t)c != want_cut || c < 0 || (int64_t)c > now))) {
        fprintf(stderr, "FAIL now=%lld days=%d: r=%d cutoff=%lld (want r=%d cutoff=%lld)\n",
                (long long)now, days, r, (long long)c, want_prune, (long long)want_cut);
        g_fail = 1;
    }
}

int main(void)
{
    const int64_t now = 1790467201;   /* 2026-09-27, fits int32 */

    check(now, 7, 1, now - 7 * 86400);
    check(now, 3650, 1, now - 3650LL * 86400);
    check(now, 0, 0, 0);
    check(now, -1, 0, 0);
    /* the wrap band from the audit, and beyond */
    check(now, 45579, 0, 0);
    check(now, 49710, 0, 0);
    check(now, 24856, 0, 0);
    check(now, INT_MAX, 0, 0);
    /* clock not yet NTP-synced: nothing is old enough, never prune */
    check(3600, 7, 0, 0);
    check(0, 7, 0, 0);

    for (int d = 1; d < INT_MAX - 997; d += 997) {
        time_t c;
        if (tl_prune_cutoff_test(now, d, &c) && (c < 0 || (int64_t)c > now || (int64_t)c > INT32_MAX)) {
            fprintf(stderr, "FAIL sweep days=%d cutoff=%lld\n", d, (long long)c);
            g_fail = 1;
            break;
        }
    }

    if (g_fail) { fprintf(stderr, "FAILED\n"); return 1; }
    printf("all timelapse prune-cutoff tests passed\n");
    return 0;
}
