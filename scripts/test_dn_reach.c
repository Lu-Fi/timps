/* test_dn_reach.c - host-only test for the reachable-maximum night rule
 * (src/dn_reach.h). Build/run: make test-dn-reach */
#include <stdio.h>
#include <string.h>
#include "dn_reach.h"

static int failures, checks;

#define CK(cond, ...) do { checks++; if (!(cond)) { failures++; \
    printf("  FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* the Wyze Pan v1 (T20/jxf22, OpenIMP) in its dark cellar */
static dnr_in pan(void)
{
    dnr_in in;
    memset(&in, 0, sizeof in);
    in.ag = 128; in.mag = 128; in.dg = 0; in.idg = 0;
    in.s = 3587.0f; in.stable = 1;
    in.day_gain = 768.0f; in.night_gain = 4096.0f;
    return in;
}

static void t_ceiling(void)
{
    CK(dnr_cap_gain(128) == 4096.0f, "cap 128 = %.1f", (double)dnr_cap_gain(128));
    CK(dnr_unreachable(128, 4096.0f), "4096 at cap 4096 is out of reach (strict >)");
    CK(!dnr_unreachable(128, 4095.0f), "4095 is reachable at full IT");
    CK(!dnr_unreachable(160, 4096.0f), "T31 cap 160 = 8192 reaches 4096");
    CK(!dnr_unreachable(-1, 4096.0f), "unknown cap: inactive");
    CK(!dnr_unreachable(0, 4096.0f), "cap 0: inactive");
}

static void t_mag_min(void)
{
    CK(dnr_mag_min(158, 128) == 128, "isp_info 128 under isp-m0 158");
    CK(dnr_mag_min(158, 160) == 158, "a higher isp_info does not raise it");
    CK(dnr_mag_min(158, -1) == 158, "isp_info absent");
    CK(dnr_mag_min(158, 0) == 158, "isp_info 0: unusable");
    CK(dnr_mag_min(-1, 128) == -1, "isp-m0 without a cap stays unknown");
    CK(dnr_unreachable(dnr_mag_min(158, 128), 4096.0f), "pan: 4096 out of reach");
    CK(!dnr_unreachable(158, 4096.0f), "158 alone kept the rule inert");
}

static void t_pan_plateau(void)
{
    dnr_state st = {0};
    dnr_in in = pan();
    CK(dnr_dark(&st, &in), "pan plateau is dark");
    CK(st.plat == 3587.0f, "plat %.0f", (double)st.plat);
    in.s = 3420.0f;                         /* 95.3 % of the plateau */
    CK(dnr_dark(&st, &in), "within 95 %%");
    in.s = 3400.0f;                         /* 94.8 % */
    CK(!dnr_dark(&st, &in), "below 95 %% of the plateau");
}

static void t_not_at_cap(void)
{
    dnr_state st = {0};
    dnr_in in = pan();
    in.ag = 125;                            /* 97.7 % of 128 */
    CK(!dnr_dark(&st, &in), "below 98 %% of the cap");
    in.ag = 126;
    CK(dnr_dark(&st, &in), "126 of 128 is at the cap");
    in = pan(); in.idg = 2;
    CK(!dnr_dark(&st, &in), "ISP digital gain in use");
    in = pan(); in.idg = -1;
    CK(!dnr_dark(&st, &in), "ISP digital gain unknown");
    in = pan(); in.dg = 4;
    CK(!dnr_dark(&st, &in), "sensor digital gain in use");
    in = pan(); in.mag = -1;
    CK(!dnr_dark(&st, &in), "cap unknown");
    in = pan(); in.ag = -1;
    CK(!dnr_dark(&st, &in), "analog gain unknown");
    in = pan(); in.s = -1.0f;
    CK(!dnr_dark(&st, &in), "no level");
}

static void t_reachable_untouched(void)
{
    dnr_state st = {0};
    dnr_in in = pan();
    in.night_gain = 4000.0f;                /* cap 4096 > 4000 */
    CK(!dnr_dark(&st, &in), "reachable night_gain: plain rule only");
    in = pan(); in.mag = 160; in.ag = 160; in.s = 7174.0f;
    CK(!dnr_dark(&st, &in), "T31-like cap: plain rule only");
    CK(dnr_night_eff(&st, 4096.0f, 768.0f) == 4096.0f, "no override");
}

static void t_daylight(void)
{
    dnr_state st = {0};
    dnr_in in = pan();
    in.ag = 0; in.s = 120.0f;               /* base gain */
    CK(!dnr_dark(&st, &in), "daylight");
    in = pan(); in.ag = 96; in.s = 1500.0f; /* dim room, 8x */
    CK(!dnr_dark(&st, &in), "dim room below the cap");
}

static void t_hysteresis_floor(void)
{
    dnr_state st = {0};
    dnr_in in = pan();
    in.s = 1200.0f;                         /* cap reached with IT ~29 % */
    CK(!dnr_dark(&st, &in), "bar 1140 does not clear 1.5 x day_gain");
    in = pan(); in.s = 1300.0f;
    st.plat = 0.0f;
    CK(dnr_dark(&st, &in), "bar 1235 clears 1152");
    in = pan(); in.day_gain = 3000.0f;
    st.plat = 0.0f;
    CK(!dnr_dark(&st, &in), "day_gain too close to the plateau: inactive");
}

static void t_settling(void)
{
    dnr_state st = {0};
    dnr_in in = pan();
    in.stable = 0;
    CK(!dnr_dark(&st, &in), "no plateau before the AE settles");
    CK(st.plat == 0.0f, "plat stays 0");
    in.stable = 1;
    CK(dnr_dark(&st, &in), "settled");
    in.stable = 0; in.s = 5000.0f;          /* an unsettled spike */
    dnr_dark(&st, &in);
    CK(st.plat == 3587.0f, "unsettled spike does not raise the plateau");
}

static void t_stretch_reset(void)
{
    dnr_state st = {0};
    dnr_in in = pan();
    in.s = 4000.0f;                         /* a settled transient at the cap */
    dnr_dark(&st, &in);
    in.s = 3587.0f;
    CK(!dnr_dark(&st, &in), "3587 is below 95 %% of 4000");
    in.ag = 100;                            /* the stretch at the cap ends */
    dnr_dark(&st, &in);
    CK(st.plat == 0.0f, "plateau reset off the cap");
    in = pan();
    CK(dnr_dark(&st, &in), "next stretch starts fresh");
}

static void t_night_eff(void)
{
    dnr_state st = {0};
    dnr_in in = pan();
    dnr_dark(&st, &in);
    CK(dnr_night_eff(&st, 4096.0f, 768.0f) == 4096.0f, "not fired yet");
    st.fired_bar = dnr_bar(&st);
    float e = dnr_night_eff(&st, 4096.0f, 768.0f);
    CK(e > 3407.0f && e < 3408.0f, "fired: %.2f", (double)e);
    CK(dnr_night_eff(&st, 4000.0f, 768.0f) == 4000.0f, "night_gain now reachable");
    CK(dnr_night_eff(&st, 4096.0f, 3000.0f) == 4096.0f, "day_gain raised past the bar");
}

int main(void)
{
    t_ceiling();
    t_mag_min();
    t_pan_plateau();
    t_not_at_cap();
    t_reachable_untouched();
    t_daylight();
    t_hysteresis_floor();
    t_settling();
    t_stretch_reset();
    t_night_eff();
    printf("test_dn_reach: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
