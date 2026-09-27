/* test_daynight_sun.c - host-only unit test for daynight.c's sunrise/sunset
 * day-selection (dn_sun_times(), opened for test via -DDN_SUN_TEST).
 *
 * The bug this guards: the calendar day used to compute today's sun times was
 * chosen by flooring to UTC midnight, which falls in the evening/afternoon
 * local time for any location far enough west of Greenwich (or morning far
 * enough east) - so the schedule adopted TOMORROW's sunset hours before it
 * happened, and switched to night at UTC midnight instead of at the real
 * sunset. Reported from Vancouver (UTC-7), where UTC midnight is 17:00 PDT.
 *
 * Built and run by `make test-daynight-sun`.
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

extern int dn_sun_times_test(float lat, float lon, time_t now,
                              time_t *sr, time_t *ss);

static int g_fail;

static void check_today(const char *what, float lat, float lon, time_t now)
{
    time_t sr = 0, ss = 0;
    int r = dn_sun_times_test(lat, lon, now, &sr, &ss);
    if (r != 0) {
        fprintf(stderr, "FAIL %s: polar day/night (r=%d), expected a normal day\n", what, r);
        g_fail = 1;
        return;
    }
    /* today's sunrise must already be in the past, and sunset still within a
     * few hours - the bug instead handed back TOMORROW's pair (sunrise ~14 h
     * in the future, sunset ~26 h out) at this exact instant. */
    if (sr > now) {
        fprintf(stderr, "FAIL %s: sunrise %lds in the FUTURE (tomorrow's calendar day, "
                         "the UTC-midnight bug)\n", what, (long)(sr - now));
        g_fail = 1;
    }
    long until_ss = (long)(ss - now);
    if (until_ss < 0 || until_ss > 6 * 3600) {
        fprintf(stderr, "FAIL %s: sunset %lds away (expected a few hours)\n", what, until_ss);
        g_fail = 1;
    }
    if (!g_fail) printf("ok %s: sunrise %lds ago, sunset in %lds\n",
                        what, (long)(now - sr), until_ss);
}

int main(void)
{
    /* Vancouver, BC, 2026-09-26 17:00:01 PDT = 2026-09-27 00:00:01 UTC - the
     * exact instant from the field report, one second after UTC midnight. */
    check_today("Vancouver just after UTC midnight", 49.2827f, -123.121f, 1790467201);

    /* Same instant, one second earlier (2026-09-26 23:59:59 UTC / 16:59:59
     * PDT): must agree with the case above - the day must not flip within
     * this same local evening. */
    check_today("Vancouver just before UTC midnight", 49.2827f, -123.121f, 1790467199);

    if (g_fail) { fprintf(stderr, "FAILED\n"); return 1; }
    printf("all daynight sun tests passed\n");
    return 0;
}
