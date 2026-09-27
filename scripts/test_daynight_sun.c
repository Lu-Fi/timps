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
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "config.h"

extern int dn_sun_times_test(float lat, float lon, time_t now,
                              time_t *sr, time_t *ss);
extern int dn_cal_target_test(const ms_daynight_cfg *dn, time_t wall);
extern int64_t dn_hb_next_test(const ms_daynight_cfg *dn, int64_t now, time_t wall);

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

    /* A sunset offset pushing the edge past solar midnight: Berlin, 21 June,
     * sunset ~19:33 UTC + 240 min = ~23:33 UTC, but the solar day turns at
     * ~23:06 UTC. At 23:20 UTC it is still (shifted) day; the calendar used to
     * look at the new solar day alone and say night. */
    {
        ms_daynight_cfg dn; memset(&dn, 0, sizeof dn);
        dn.sun_latitude = 52.52f; dn.sun_longitude = 13.405f;
        dn.sun_sunset_offset_min = 240;
        int t = dn_cal_target_test(&dn, 1782084000);   /* 2026-06-21 23:20 UTC */
        if (t != 0) { fprintf(stderr, "FAIL shifted sunset across solar midnight: got %d, want day\n", t); g_fail = 1; }
        else printf("ok shifted sunset across solar midnight is still day\n");
        t = dn_cal_target_test(&dn, 1782088800);       /* 00:40 UTC: past it */
        if (t != 1) { fprintf(stderr, "FAIL after the shifted sunset: got %d, want night\n", t); g_fail = 1; }
        else printf("ok after the shifted sunset it is night\n");
    }
    /* equal HH:MM edges are not a time window: fall through to the sun */
    {
        ms_daynight_cfg dn; memset(&dn, 0, sizeof dn);
        snprintf(dn.time_night_start, sizeof dn.time_night_start, "20:00");
        snprintf(dn.time_day_start, sizeof dn.time_day_start, "20:00");
        int t = dn_cal_target_test(&dn, 1782068400);
        if (t != -1) { fprintf(stderr, "FAIL equal edges gave %d, want unknown\n", t); g_fail = 1; }
        dn.sun_latitude = 52.52f; dn.sun_longitude = 13.405f;
        t = dn_cal_target_test(&dn, 1782068400);       /* 19:00 UTC: sun up */
        if (t != 0) { fprintf(stderr, "FAIL equal edges + location gave %d, want day (sun)\n", t); g_fail = 1; }
        else printf("ok equal time edges fall through to the sun calendar\n");
    }
    /* every heartbeat re-arm is pulled in to the calendar's dawn */
    {
        setenv("TZ", "UTC", 1); tzset();
        ms_daynight_cfg dn; memset(&dn, 0, sizeof dn);
        snprintf(dn.time_night_start, sizeof dn.time_night_start, "22:00");
        snprintf(dn.time_day_start, sizeof dn.time_day_start, "06:00");
        dn.heartbeat_s = 4 * 3600;
        int64_t hb = dn_hb_next_test(&dn, 1000, 1782104400);   /* 05:00 UTC */
        if (hb != 1000 + 3600 * 1000LL) { fprintf(stderr, "FAIL heartbeat at +%llds, want dawn +3600s\n", (long long)(hb - 1000) / 1000); g_fail = 1; }
        else printf("ok heartbeat pulled in to the 06:00 day edge\n");
    }

    if (g_fail) { fprintf(stderr, "FAILED\n"); return 1; }
    printf("all daynight sun tests passed\n");
    return 0;
}
