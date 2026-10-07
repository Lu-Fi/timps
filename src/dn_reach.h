#ifndef MS_DN_REACH_H
#define MS_DN_REACH_H
/* The reachable-maximum night rule. `s > night_gain` needs an exposure index
 * the camera can produce at all: with the digital gains unused and the
 * integration-time ratio at most 1, the index cannot exceed the published
 * analog cap, 256 * 2^(mag/32). Measured on a Wyze Pan v1 (T20/jxf22,
 * OpenIMP), one night in 2026-10: cap 128 = 4096, the AE stopped there with
 * the ISP digital gain at 0 and the integration time at 87.6 %, so the index
 * sat at 3587 all night against the default night_gain 4096 and the camera
 * stayed in day. Only where the cap proves night_gain out of reach does a
 * plateau rule stand in for it; everywhere else nothing here changes a
 * decision. Pure, so scripts/test_dn_reach.c runs the code the thread runs. */
#include <math.h>

#define DNR_CAP_PCT   98     /* analog gain at >= this % of its published max */
#define DNR_PLAT_PCT  95     /* index within this % of its plateau at the cap */
/* the plateau bar must clear day_gain by this much, or the next probe reads
 * the same scene below day_gain and undoes the switch */
#define DNR_HYST      1.5f

typedef struct {
    int   ag, mag;       /* SENSOR analog gain / its MAX, log2 units (32 = one
                          * stop); <0 = not in the dump */
    int   dg, idg;       /* SENSOR / ISP digital gain, ditto */
    float s;             /* smoothed exposure index */
    int   stable;        /* stable_n reached: the AE has settled */
    float day_gain, night_gain;   /* night_gain as configured */
} dnr_in;

typedef struct {
    float plat;          /* highest settled index of this stretch at the cap, 0 = none */
    int   mag;
    float fired_bar;     /* bar the rule last switched on, 0 = never this session */
} dnr_state;

static inline float dnr_cap_gain(int mag)
{
    return 256.0f * exp2f((float)mag / 32.0f);
}

static inline int dnr_unreachable(int mag, float night_gain)
{
    return mag > 0 && dnr_cap_gain(mag) <= night_gain;
}

static inline int dnr_at_cap(const dnr_in *in)
{
    return in->mag > 0 && in->ag >= 0 && in->idg == 0 && in->dg <= 0 &&
           in->ag * 100 >= in->mag * DNR_CAP_PCT;
}

static inline float dnr_bar(const dnr_state *st)
{
    return st->plat * (float)DNR_PLAT_PCT / 100.0f;
}

/* One day-mode tick: 1 = this sample is dark the way `s > night_gain` would
 * be. The plateau is per stretch at the cap, not per session, so one
 * transient cannot raise it for good. */
static inline int dnr_dark(dnr_state *st, const dnr_in *in)
{
    if (!dnr_unreachable(in->mag, in->night_gain) || !dnr_at_cap(in) ||
        !(in->s > 0.0f)) {
        st->plat = 0.0f;
        return 0;
    }
    if (st->mag != in->mag) { st->mag = in->mag; st->plat = 0.0f; }
    if (in->stable && in->s > st->plat) st->plat = in->s;
    float bar = dnr_bar(st);
    return st->plat > 0.0f && bar > in->day_gain * DNR_HYST && in->s >= bar;
}

/* The night threshold the cost vetoes project against once the rule has
 * switched: against an unreachable night_gain they never fire, and a ratio
 * verdict would chase the plateau rule all night. */
static inline float dnr_night_eff(const dnr_state *st, float night_gain,
                                  float day_gain)
{
    if (!(st->fired_bar > 0.0f) || !dnr_unreachable(st->mag, night_gain))
        return night_gain;
    return (st->fired_bar > day_gain * DNR_HYST && st->fired_bar < night_gain)
         ? st->fired_bar : night_gain;
}
#endif
