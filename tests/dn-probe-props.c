/* dn-probe-props.c - property test for dn_next_probe() (design-notes s.5).
 *
 * The reason the probe schedule was collapsed into one pure function is this
 * file. A scenario in scripts/dn-scenarios/ can only show that ONE evidence
 * TRAJECTORY produces an acceptable outcome; it cannot show that the schedule
 * is well-behaved as a function, because a running machine only ever visits
 * the evidence its own decisions produce. The counterfactual - "what would
 * this build have done had the scene been slightly brighter at that instant"
 * - is exactly what every one of the twelve stuck-mode incidents turned on,
 * and it is unreachable from a replay. It IS reachable from a pure function.
 *
 * THE PROPERTY
 *
 *   Monotonicity. For evidence e1, e2 identical except that e2 is strictly
 *   brighter (lower smoothed gain - the metric is INVERTED, high gain = dark
 *   scene):
 *                 dn_next_probe(e2).in_ms <= dn_next_probe(e1).in_ms
 *
 *   In words: brighter evidence must never buy a LATER correction. It is
 *   allowed to buy an earlier one, or the same one. Never a later one.
 *
 * This is not an abstract nicety. It is the shared shape of three of the
 * subsystem's open incidents:
 *
 *   0f5fc80  - the baseline drifted toward raw gain and the brightening bar
 *              was derived from the baseline, so noise ratcheted both
 *              together and a brighter reading raised its own bar.
 *   14a1d61  - the baseline drifts toward smooth_tg and the skip gate's
 *              "solidly night" bar is derived from the baseline, so over
 *              hours the bar chased a day-level gain downward and "still deep
 *              in night" stayed true: brighter evidence, later probe.
 *   2026-08-14 - an AMBIGUOUS (dead-zone, i.e. brighter) revert was accounted
 *              identically to a confirmed-night revert and bought the same x4
 *              backoff, i.e. a 4 h schedule. Brighter evidence, later probe,
 *              four hours of daylight in IR mode.
 *
 * Three incidents, one assertion, checked on every build instead of
 * re-derived by hand after each one.
 *
 * AND ONE THE ASSERTION ABOVE CANNOT SEE (kinder-links 2026-08-16)
 *
 * Monotonicity and the reconfirm bound are both statements about ONE evidence
 * snapshot. That is exactly where the trend suspension hid a bug for two days:
 * as a predicate over the instantaneous smooth_tg it satisfied both, on every
 * one of ~10^6 points, while still handing a four-hour deadline straight back
 * the moment a room dimmed again after brightening. Purity bought
 * counterfactual REACH; it did not by itself buy temporal claims. Hence
 * PROPERTY 3 (assert_dip_monotone), stated over the SEQUENCE:
 *
 *   evidence once measured is never worth less later. For e1, e2 identical
 *   except that e2 records a strictly brighter minimum since the frozen
 *   anchor - the CURRENT readings equal:
 *                 dn_next_probe(e2).in_ms <= dn_next_probe(e1).in_ms
 *
 * Note it is vacuous against a build that simply ignores min_smooth_since_probe
 * (both plans come out identical), which is honest about its limits: the
 * discriminating checks for that build are the two kinder-links cases asserted
 * by name at the bottom of main(). A general property cannot notice a field
 * nobody reads; a named incident can.
 *
 * WHAT "IDENTICAL EXCEPT BRIGHTER" MEANS, HONESTLY
 *
 * The property is only as strong as the definition of "brighter", so it is
 * spelled out rather than assumed:
 *
 *  - smooth_tg is scaled DOWN. That is the scene getting brighter.
 *  - night_baseline is held FIXED. This is the load-bearing choice. The
 *    baseline is not an independent measurement, it is a slow EMA that CHASES
 *    smooth_tg (DN_BASELINE_ALPHA), and 14a1d61 is precisely the bug where
 *    letting the bar follow the gain down made brighter evidence look
 *    unchanged. Holding it fixed is what makes the sweep able to see that.
 *    The sweep separately varies the baseline across its own axis, so the
 *    chased configurations are covered too - as SEPARATE base points, each of
 *    which must independently satisfy the property.
 *  - probe_fail_smooth is held FIXED, because it is frozen at a measurement
 *    by construction (generator D's rule). A test that let the frozen anchor
 *    move with the scene would be testing a machine nobody wrote.
 *  - every clock and every scheduling counter is held FIXED. Only the light
 *    changes.
 *
 * COVERAGE
 *
 * The sweep is exhaustive over a cartesian product of realistic values rather
 * than random: ~10^5 base evidence points, each compared against 7 strictly
 * brighter variants, so ~10^6 ordered pairs per run, deterministic and
 * reproducible. The value sets are drawn from the incident record (the gains
 * in the corpus scenarios and the commit messages: 256/262/284/315/708/820/
 * 1002/2425/4813/9024/10856/16000) plus the structural edges that the
 * unsatisfiable-bar work identified (at, just under and just over the 256
 * sensor floor).
 *
 * A violation prints the full evidence of BOTH points and both plans, because
 * "monotonicity failed somewhere" is not actionable and the whole argument
 * for the collapse is that this class of bug becomes actionable.
 *
 * Build/run:  make dn-props && ./dn-probe-props
 * The replay harness runs it as corpus entry 00 (scripts/dn-replay.py --all).
 */
#include "daynight_probe.h"

#include <stdio.h>
#include <string.h>

static long g_pairs, g_bases, g_viol;

static const char *act_name(int a)
{
    return a == DN_PROBE_FIRE ? "FIRE"
         : a == DN_PROBE_SKIP ? "SKIP" : "none";
}

static const char *path_name(int p)
{
    return p == DN_PATH_PERIODIC    ? "periodic"
         : p == DN_PATH_BRIGHTEN    ? "brighten"
         : p == DN_PATH_PREBASELINE ? "pre-baseline" : "-";
}

static void show(const char *tag, const dn_evidence *e, const dn_probe_plan *p)
{
    char in[32];
    if (p->in_ms == DN_PROBE_NEVER) snprintf(in, sizeof in, "NEVER");
    else snprintf(in, sizeof in, "%lldms", (long long)p->in_ms);
    printf("  %s: smooth_tg=%.1f baseline=%.1f fail_smooth=%.1f backoff=%d\n"
           "      pct=%d reconfirm=%ds max_skip=%ds transition=%ds "
           "day_thr=%.0f\n"
           "      now=%lld verify(armed=%d from=%lld at=%lld) "
           "last_phys=%lld last_sw=%lld\n"
           "      brighten(armed=%d since=%lld) osc_freeze=%lld\n"
           "   -> in_ms=%s act=%s path=%s eff_backoff=%d pulled_in=%d "
           "anchor_override=%d\n",
           tag, (double)e->smooth_tg, (double)e->night_baseline,
           (double)e->probe_fail_smooth, e->backoff,
           e->day_gain_pct, e->night_reconfirm_s, e->probe_max_skip_s,
           e->transition_s, (double)e->day_threshold,
           (long long)e->now_ms, e->verify_armed,
           (long long)e->verify_from_ms, (long long)e->verify_at_ms,
           (long long)e->last_phys_probe_ms, (long long)e->last_switch_ms,
           e->brighten_armed, (long long)e->brighten_since_ms,
           (long long)e->osc_freeze_until_ms,
           in, act_name(p->act), path_name(p->path), p->eff_backoff,
           p->trend_pulled_in, p->anchor_override);
}

/* PROPERTY 2 - the bound the backoff is not allowed to erode.
 *
 * Monotonicity alone is necessary but not sufficient, and the 2026-08-14
 * incident shows why: a schedule that is uniformly four hours late is
 * perfectly monotone. The design notes state the missing half as the
 * restart-equivalence bound - "T = night_reconfirm_s is the natural bound,
 * one reconfirm interval, which is precisely the guarantee night_reconfirm_s
 * was introduced (b3eec71) to provide, and which probe_backoff x the skip
 * gate has been quietly eroding ever since."
 *
 * So: once the FROZEN anchor says the scene is measurably brighter than the
 * night a physical probe actually measured, the correction may not land later
 * than one un-backed-off reconfirm interval after the deadline was armed. The
 * backoff may still stretch the schedule of a scene that has NOT changed -
 * that is what it is for - but it may not outlive the evidence that
 * contradicts its premise.
 *
 * Asserted only on self-consistent evidence (a deadline no later than the
 * machine's own arming rule would produce) and never while an oscillation
 * freeze is active, since that legitimately outranks every probe. */
static void assert_reconfirm_bound(const dn_evidence *e)
{
    if (!e->verify_armed || e->verify_from_ms <= 0 ||
        e->night_reconfirm_s <= 0)
        return;
    if (e->probe_fail_smooth <= 0.0f || e->min_smooth_since_probe <= 0.0f)
        return;                                  /* no frozen anchor to read */
    if (e->min_smooth_since_probe >= e->probe_fail_smooth * DN_BRIGHTEN_MARGIN)
        return;                       /* not measurably brighter: no promise */
    if (e->osc_freeze_until_ms && e->now_ms < e->osc_freeze_until_ms)
        return;                                  /* the freeze outranks this */
    if (e->verify_at_ms >
        e->verify_from_ms + dn_reconfirm_iv_s(e, e->backoff) * 1000)
        return;               /* deadline the machine would not have produced */

    int64_t bound = e->verify_from_ms + dn_reconfirm_iv_s(e, 1) * 1000;
    dn_probe_plan p = dn_next_probe(e);
    g_pairs++;
    if (p.in_ms == 0)
        return;                                    /* correcting right now */
    if (p.in_ms != DN_PROBE_NEVER && e->now_ms + p.in_ms <= bound)
        return;
    if (++g_viol <= 10) {
        printf("VIOLATION %ld: the x%d backoff outlived the evidence against "
               "it - gain %.0f is below %.0f%% of the frozen anchor %.0f, so "
               "the correction was promised by t=%lld (one reconfirm interval "
               "after arming) but is scheduled for t=%lld\n",
               g_viol, e->backoff, (double)e->min_smooth_since_probe,
               (double)(DN_BRIGHTEN_MARGIN * 100.0f),
               (double)e->probe_fail_smooth, (long long)bound,
               p.in_ms == DN_PROBE_NEVER ? -1LL
                                         : (long long)(e->now_ms + p.in_ms));
        show("evidence", e, &p);
        printf("\n");
    }
}

/* PROPERTY 3 - evidence of a PAST brightening may not be forgotten.
 * (kinder-links 2026-08-16, the defect this file could not see.)
 *
 * Properties 1 and 2 are both statements about ONE evidence snapshot, and
 * that is exactly where the trend suspension hid its bug for two days: as a
 * predicate over the instantaneous smooth_tg it satisfied both, on every
 * snapshot, while still handing a four-hour deadline straight back the moment
 * a room dimmed again after brightening. The property that was missing is the
 * one that spans snapshots:
 *
 *   For evidence e1, e2 identical except that e2 records a strictly BRIGHTER
 *   minimum since the frozen anchor (min_smooth_since_probe lower):
 *        dn_next_probe(e2).in_ms <= dn_next_probe(e1).in_ms
 *
 * i.e. "the scene reached X at some point since a probe last measured night"
 * must be worth at least as much as "the scene never got brighter than its
 * current reading" - even when the CURRENT readings are identical. Note this
 * is not implied by property 1: there the reading moves and the history moves
 * with it, here the reading is pinned and only the history differs, which is
 * precisely the counterfactual a trajectory can never present. */
static void assert_dip_monotone(const dn_evidence *e1, const dn_evidence *e2)
{
    if (e2->min_smooth_since_probe >= e1->min_smooth_since_probe) return;
    dn_probe_plan p1 = dn_next_probe(e1);
    dn_probe_plan p2 = dn_next_probe(e2);
    g_pairs++;
    if (p2.in_ms <= p1.in_ms) return;
    if (++g_viol <= 10) {
        printf("VIOLATION %ld: a past brightening bought a LATER probe - the "
               "schedule forgot evidence it had already been shown\n", g_viol);
        show("no dip  ", e1, &p1);
        show("dipped  ", e2, &p2);
        printf("\n");
    }
}

/* the one assertion: e2 is e1 with a strictly lower smoothed gain */
static void assert_monotone(const dn_evidence *e1, const dn_evidence *e2)
{
    dn_probe_plan p1 = dn_next_probe(e1);
    dn_probe_plan p2 = dn_next_probe(e2);
    g_pairs++;
    if (p2.in_ms <= p1.in_ms) return;
    if (++g_viol <= 10) {
        printf("VIOLATION %ld: brighter evidence bought a LATER probe\n",
               g_viol);
        show("dimmer  ", e1, &p1);
        show("brighter", e2, &p2);
        printf("\n");
    }
}

int main(void)
{
    /* gains from the incident record + the structural edges around the
     * physical floor (DN_GAIN_FLOOR = 256) that generator C turns on */
    static const float G[] = {
        255.0f, 256.0f, 257.0f, 262.0f, 268.0f, 284.0f, 300.0f, 315.0f,
        452.0f, 708.0f, 820.0f, 1002.0f, 1436.0f, 2425.0f, 4813.0f,
        9024.0f, 10856.0f, 16000.0f
    };
    static const float BL[] = { -1.0f, 256.0f, 268.0f, 315.0f, 731.0f,
                                3500.0f, 5148.0f, 10856.0f };
    static const float FS[] = { -1.0f, 256.0f, 284.0f, 315.0f, 820.0f,
                                4906.0f };
    static const int   BO[] = { 1, 2, 4 };
    static const int   PCT[] = { 0, 60, 65, 100 };
    static const int   RC[] = { 0, 900, 3600 };
    /* strictly-brighter factors: from "one AE tick" to "the lights came on" */
    static const float K[] = { 0.999f, 0.99f, 0.97f, 0.9f, 0.7f, 0.5f, 0.25f };

    const int NG = (int)(sizeof G / sizeof *G);
    const int NBL = (int)(sizeof BL / sizeof *BL);
    const int NFS = (int)(sizeof FS / sizeof *FS);
    const int NBO = (int)(sizeof BO / sizeof *BO);
    const int NPCT = (int)(sizeof PCT / sizeof *PCT);
    const int NRC = (int)(sizeof RC / sizeof *RC);
    const int NK = (int)(sizeof K / sizeof *K);

    /* Four scheduling postures, each a real situation from the record. They
     * vary the clock-relative fields together so the sweep spends its budget
     * on reachable states instead of on arbitrary timestamp noise. */
    struct posture {
        const char *name;
        int64_t now_ms, verify_from_ms, verify_at_ms;
        int     verify_armed;
        int64_t last_phys_probe_ms, last_switch_ms;
        int64_t brighten_since_ms, osc_freeze_until_ms;
        int     brighten_armed;
    } post[] = {
        /* deadline still far out, hold not started (the ordinary night) */
        { "pending",  4000000, 3000000, 10200000, 1, 3000000, 3000000,
          0, 0, 1 },
        /* deadline exactly due (the skip-gate decision point) */
        { "due",      10200000, 3000000, 10200000, 1, 3000000, 3000000,
          0, 0, 1 },
        /* deadline long past AND probe_max_skip_s exceeded (outer bound) */
        { "overdue",  60000000, 3000000, 10200000, 1, 3000000, 3000000,
          0, 0, 1 },
        /* brightening hold already running, no deadline armed */
        { "holding",  4000000, 0, 0, 0, 3000000, 3000000,
          3990000, 0, 1 },
        /* first probe of the session (last_phys_probe_ms == 0) */
        { "firstever", 4000000, 3000000, 3900000, 1, 0, 3000000,
          0, 0, 1 },
        /* oscillation-breaker freeze active */
        { "frozen",   4000000, 3000000, 3900000, 1, 3000000, 3000000,
          0, 4600000, 1 },
        /* hold disarmed (post-failed-probe, below the bar, waiting for the
         * baseline to re-converge) */
        { "disarmed", 4000000, 3000000, 10200000, 1, 3000000, 3000000,
          0, 0, 0 },
    };
    const int NP = (int)(sizeof post / sizeof *post);

    for (int ip = 0; ip < NP; ip++)
    for (int ibl = 0; ibl < NBL; ibl++)
    for (int ifs = 0; ifs < NFS; ifs++)
    for (int ibo = 0; ibo < NBO; ibo++)
    for (int ipct = 0; ipct < NPCT; ipct++)
    for (int irc = 0; irc < NRC; irc++)
    for (int ig = 0; ig < NG; ig++) {
        dn_evidence e;
        memset(&e, 0, sizeof e);
        e.now_ms             = post[ip].now_ms;
        e.verify_armed       = post[ip].verify_armed;
        e.verify_from_ms     = post[ip].verify_from_ms;
        e.verify_at_ms       = post[ip].verify_at_ms;
        e.last_phys_probe_ms = post[ip].last_phys_probe_ms;
        e.last_switch_ms     = post[ip].last_switch_ms;
        e.brighten_since_ms  = post[ip].brighten_since_ms;
        e.brighten_armed     = post[ip].brighten_armed;
        e.osc_freeze_until_ms = post[ip].osc_freeze_until_ms;
        e.day_gain_pct       = PCT[ipct];
        e.night_reconfirm_s  = RC[irc];
        e.probe_max_skip_s   = 43200;
        e.transition_s       = 30;
        e.day_threshold      = 300.0f;
        e.night_baseline     = BL[ibl];
        e.probe_fail_smooth  = FS[ifs];
        e.backoff            = BO[ibo];
        e.smooth_tg          = G[ig];
        /* base case: the current reading IS the brightest the scene has been
         * since the anchor - a night that has only ever dimmed. This is the
         * conservative posture and it reproduces the pre-2026-08-16 semantics
         * exactly, so the whole existing sweep keeps the coverage it had. The
         * dipped posture (minimum strictly below the reading) is asserted
         * separately by assert_dip_monotone() below, because it is a
         * DIFFERENT property, not a variation of this one. */
        e.min_smooth_since_probe = e.smooth_tg;
        g_bases++;

        /* The hold's frozen rest reference (2026-08-16). REF[0] = 0 is "not
         * frozen yet", which falls back to the live baseline and therefore
         * reproduces the pre-fix semantics, so the sweep keeps every bit of
         * the coverage it had; the other two are a reference frozen ABOVE and
         * BELOW the live baseline. The below-baseline case is the one that
         * matters: it is unreachable on a real trajectory (the baseline drifts
         * toward smooth_tg, so it cannot climb above a frozen rest level while
         * the scene is brighter than it) and it is exactly the counterfactual
         * that caught the missing max() in the first draft of this fix. */
        static const float REF[] = { 0.0f, 1.35f, 0.6f };
        for (int ir = 0; ir < (int)(sizeof REF / sizeof *REF); ir++) {
        e.brighten_ref = REF[ir] == 0.0f ? 0.0f
                       : (e.night_baseline > 0.0f ? e.night_baseline * REF[ir]
                                                  : 0.0f);
        assert_reconfirm_bound(&e);
        for (int ik = 0; ik < NK; ik++) {
            dn_evidence b = e;
            b.smooth_tg = e.smooth_tg * K[ik];
            /* the sensor cannot report below its physical floor, so a
             * "brighter" variant that would is not a scene the machine can
             * ever be shown - skip it rather than assert on fiction */
            if (b.smooth_tg < DN_GAIN_FLOOR) continue;
            b.min_smooth_since_probe = b.smooth_tg;
            assert_monotone(&e, &b);
            /* PROPERTY 3, same sweep: e with a PAST dip to b's level. The
             * scene reads exactly as dim as e does now, but has been as
             * bright as b at some point since the anchor was frozen. */
            dn_evidence d = e;
            d.min_smooth_since_probe = b.smooth_tg;
            assert_dip_monotone(&e, &d);
        }
        }
        e.brighten_ref = 0.0f;
    }

    /* --- the three named historical violations, as explicit cases --------
     * The sweep above already covers these shapes, but naming them means a
     * regression reports WHICH incident came back rather than a coordinate
     * in a cartesian product. */
    {
        dn_evidence e;
        memset(&e, 0, sizeof e);
        e.now_ms = 20000000; e.day_gain_pct = 60;
        e.night_reconfirm_s = 3600; e.probe_max_skip_s = 43200;
        e.transition_s = 30; e.day_threshold = 300.0f;
        e.verify_armed = 1; e.verify_from_ms = 5600000;
        e.last_phys_probe_ms = 5600000; e.last_switch_ms = 5600000;
        e.brighten_armed = 1; e.backoff = 4;

        /* 14a1d61 (Schuppen 2026-08-13): a failed probe froze the anchor at
         * 284; over the next 2.5 h the baseline chased the actual (day-level)
         * gain down to 257-266 and the baseline-derived "solidly night" bar
         * followed it, so the skip gate kept skipping. The frozen anchor is
         * what must decide: at gain 262 the scene is measurably brighter than
         * the 284 that was MEASURED to be night, so the probe must fire now -
         * regardless of where the drifting baseline has got to. */
        e.probe_fail_smooth = 284.0f;
        e.verify_at_ms = 5600000 + 14400000;
        {
            dn_evidence dim = e, bright = e;
            dim.night_baseline = 3500.0f; dim.smooth_tg = 3400.0f;
            dim.min_smooth_since_probe = 3400.0f;
            bright.night_baseline = 262.0f; bright.smooth_tg = 262.0f;
            bright.min_smooth_since_probe = 262.0f;
            /* NOTE: this pair deliberately varies the baseline too - it is
             * not an instance of the sweep's property but of the stronger
             * claim the anchor rule makes, so it is asserted directly. */
            dn_probe_plan pb = dn_next_probe(&bright);
            g_pairs++;
            if (pb.act != DN_PROBE_FIRE) {
                g_viol++;
                printf("VIOLATION (14a1d61): gain 262 is below 97%% of the "
                       "frozen anchor 284, but the schedule did not fire\n");
                show("brighter", &bright, &pb);
                printf("\n");
            }
            (void)dim;
        }

        /* 2026-08-14 (cam-vorne): the ratchet latched at 315 and the backoff
         * was at x4, putting the next reconfirm 4 h out while the gain fell
         * through three orders of magnitude. The trend suspension must make
         * the deadline land one un-backed-off reconfirm interval after it was
         * armed, not four. */
        e.probe_fail_smooth = 315.0f;
        e.verify_from_ms = 19000000;
        e.verify_at_ms = 19000000 + 4 * 3600 * 1000;   /* x4 = 4 h out */
        e.now_ms = 19000000 + 3600 * 1000 + 1000;      /* 1 h + a tick */
        e.night_baseline = 300.0f;
        {
            dn_evidence still = e, falling = e;
            still.smooth_tg   = 315.0f;   /* unchanged since the probe */
            still.min_smooth_since_probe = 315.0f;
            falling.smooth_tg = 260.0f;   /* the dawn ramp, 83% of anchor */
            falling.min_smooth_since_probe = 260.0f;
            dn_probe_plan ps = dn_next_probe(&still);
            dn_probe_plan pf = dn_next_probe(&falling);
            g_pairs++;
            if (pf.in_ms > ps.in_ms || pf.act != DN_PROBE_FIRE) {
                g_viol++;
                printf("VIOLATION (2026-08-14): a falling gain did not "
                       "collapse the x4 backoff\n");
                show("static  ", &still, &ps);
                show("falling ", &falling, &pf);
                printf("\n");
            }
            /* and the static scene must keep its backoff: the suspension may
             * not cost a click in the darkness case the backoff exists for */
            if (ps.act == DN_PROBE_FIRE) {
                g_viol++;
                printf("VIOLATION (2026-08-14): a STATIC dark scene lost its "
                       "backoff - the suspension must cost zero extra "
                       "clicks in unchanging darkness\n");
                show("static  ", &still, &ps);
                printf("\n");
            }
        }

        /* 0f5fc80 (noisy night): the bar must not follow the gain, so a
         * brighter gain against the SAME baseline must never delay. This is
         * the sweep's property restricted to the noisy-night configuration,
         * asserted by name. */
        e.probe_fail_smooth = -1.0f;
        e.night_baseline = 4906.0f;
        e.verify_from_ms = 5600000; e.verify_at_ms = 5600000 + 3600000;
        e.now_ms = 5600000 + 3600000;
        for (int i = 0; i < NG; i++) {
            for (int j = 0; j < NG; j++) {
                if (G[j] >= G[i]) continue;
                dn_evidence d = e, b = e;
                d.smooth_tg = G[i]; b.smooth_tg = G[j];
                d.min_smooth_since_probe = G[i];
                b.min_smooth_since_probe = G[j];
                assert_monotone(&d, &b);
            }
        }
    }

    /* --- kinder-links 2026-08-16, the two defects it exposed --------------
     * A room light switched off and back on in front of a deployed camera.
     * The sensor saw both edges cleanly (analog gain 85->100->86, i.e.
     * total_gain 1614 -> 2233 -> 1649, a +38%/-26% swing that held steady),
     * and the decision logic did not react to either, in any way, at all. */
    {
        dn_evidence e;
        memset(&e, 0, sizeof e);
        e.day_gain_pct = 60; e.night_reconfirm_s = 3600;
        e.probe_max_skip_s = 43200; e.transition_s = 5;
        e.day_threshold = 300.0f;
        e.brighten_armed = 1;
        e.last_switch_ms = 1000000; e.last_phys_probe_ms = 1000000;

        /* (B2) The backoff must not outlive a brightening just because the
         * scene dimmed again afterwards. The night's real numbers: the
         * anchor froze at 1653 when a reconfirm probe found genuine night and
         * took the backoff to x4 (next probe 4 h out); nine minutes later the
         * gain reached 1599 - below the 1603 bar - and the suspension pulled
         * the deadline in to one interval. Then the room dimmed on its own to
         * ~2024, the predicate went false and the four hours came back. The
         * evidence still exists; the schedule must still honour it. */
        e.probe_fail_smooth = 1653.0f;
        e.night_baseline = 2024.0f;
        e.backoff = 4;
        e.verify_armed = 1;
        e.verify_from_ms  = 2000000;                        /* probe + revert */
        e.verify_at_ms    = 2000000 + 4 * 3600 * 1000;      /* x4 = 4 h out */
        e.now_ms          = 2000000 + 3600 * 1000 + 1000;   /* 1 h + a tick */
        {
            dn_evidence forgot = e, remembers = e;
            /* both read 2024 RIGHT NOW - the room dimmed back */
            forgot.smooth_tg    = 2024.0f;
            remembers.smooth_tg = 2024.0f;
            forgot.min_smooth_since_probe    = 2024.0f;  /* never brightened */
            remembers.min_smooth_since_probe = 1599.0f;  /* it did, at 23:21 */
            dn_probe_plan pr = dn_next_probe(&remembers);
            g_pairs++;
            if (pr.act != DN_PROBE_FIRE) {
                g_viol++;
                printf("VIOLATION (kinder-links B2): the scene reached 1599 "
                       "against a frozen anchor of 1653 since the last probe, "
                       "and the x4 backoff survived it\n");
                show("remembers", &remembers, &pr);
                printf("\n");
            }
            assert_dip_monotone(&forgot, &remembers);
        }

        /* (B1) The failure ratchet must be reachable by a light source, not
         * only by a pipeline change. Same anchor 1653. Turning on a second
         * lamp takes the scene to 1300 - a 21% brightening, seven times the
         * 3% "measurably brighter" bar and far outside AGC noise. Under the
         * old day_gain_pct%-derived ratchet the bar was 992 (analog gain 62,
         * below anything this room reads at night), so the sustained-
         * brightening hold could not start no matter how long the light
         * stayed on. Baseline 2024 puts the arming bar at 1571, so the hold's
         * other two gates are satisfied and the ratchet is the one under
         * test. */
        {
            dn_evidence lamp = e;
            lamp.backoff = 1;
            lamp.verify_armed = 0; lamp.verify_from_ms = 0;
            lamp.verify_at_ms = 0;
            lamp.smooth_tg = 1300.0f;
            lamp.min_smooth_since_probe = 1300.0f;
            dn_probe_plan pl = dn_next_probe(&lamp);
            g_pairs++;
            if (!(pl.brighten_started || pl.act == DN_PROBE_FIRE)) {
                g_viol++;
                printf("VIOLATION (kinder-links B1): a 21%% brightening to "
                       "1300 against a 1653 anchor could not start a "
                       "brightening hold - ratchet bar %.0f\n",
                       (double)pl.ratchet_bar);
                show("lamp on ", &lamp, &pl);
                printf("\n");
            }
            /* and the incident the ratchet exists for must STILL be blocked:
             * the cam-wyze-pan dawn dip returns to the very level the probe
             * latched, and must not be able to re-fire the same probe. */
            dn_evidence samedip = lamp;
            samedip.probe_fail_smooth = 820.0f;
            samedip.night_baseline    = 1400.0f;
            samedip.smooth_tg = samedip.min_smooth_since_probe = 820.0f;
            dn_probe_plan ps = dn_next_probe(&samedip);
            g_pairs++;
            if (ps.brighten_started || ps.act == DN_PROBE_FIRE) {
                g_viol++;
                printf("VIOLATION (kinder-links B1): the widened ratchet let "
                       "an IDENTICAL dip re-fire - 820 against an anchor "
                       "latched at 820, bar %.0f\n", (double)ps.ratchet_bar);
                show("same dip", &samedip, &ps);
                printf("\n");
            }
        }

        /* (B3) The hold's bar must not be dragged down by the baseline chasing
         * the very brightening it exists to detect. The room rests at 2400; a
         * light takes it to 1820 (24%) and stays. Over the step and the ticks
         * after it the baseline chases down to 2300 - a 4.2% lead, the size a
         * STEP produces and comfortably inside DN_HOLD_REF_LEAD. That is
         * already enough to shut the gate: the LIVE bar is 0.8*2300 = 1840
         * with a 1785 margin, so a permanent 24% brightening reads as 2%
         * SHORT and the hold cannot start - and on the corpus 10 trajectory
         * the bar goes on to cross smooth_tg entirely, taking the `>= bar`
         * branch and re-arming, so the event is not merely missed but erased.
         * Against the frozen rest level the bar is 1920, margin 1862, and the
         * hold starts at once. */
        {
            dn_evidence chase;
            memset(&chase, 0, sizeof chase);
            chase.now_ms = 4000000; chase.day_gain_pct = 60;
            chase.night_reconfirm_s = 3600; chase.probe_max_skip_s = 43200;
            chase.transition_s = 5; chase.day_threshold = 300.0f;
            chase.last_switch_ms = 3000000; chase.last_phys_probe_ms = 3000000;
            chase.brighten_armed = 1;
            chase.probe_fail_smooth = -1.0f;
            chase.night_baseline = 2300.0f;    /* already chasing downward */
            chase.brighten_ref   = 2400.0f;    /* the pre-event rest level */
            chase.smooth_tg = chase.min_smooth_since_probe = 1820.0f;
            dn_probe_plan pc = dn_next_probe(&chase);
            g_pairs++;
            if (!pc.brighten_started) {
                g_viol++;
                printf("VIOLATION (kinder-links B3): a 24%% brightening did "
                       "not start the hold - bar %.0f off a frozen rest level "
                       "of %.0f (live baseline %.0f)\n", (double)pc.hold_bar,
                       (double)chase.brighten_ref,
                       (double)chase.night_baseline);
                show("chased  ", &chase, &pc);
                printf("\n");
            }
            /* and the blip guard the whole debounce exists for: the moment the
             * scene returns to its resting level the hold must be discarded,
             * not carried. A headlight cannot bank 29 s of confirm and spend
             * it later. */
            dn_evidence blip = chase;
            blip.brighten_since_ms = 3999000;   /* 1 s short of confirming */
            blip.smooth_tg = blip.min_smooth_since_probe = 2400.0f;
            dn_probe_plan pbl = dn_next_probe(&blip);
            g_pairs++;
            if (pbl.brighten_since_ms != 0 || pbl.act == DN_PROBE_FIRE) {
                g_viol++;
                printf("VIOLATION (kinder-links B3): a hold survived the scene "
                       "returning to its resting level - a transient could "
                       "bank confirm time\n");
                show("blip    ", &blip, &pbl);
                printf("\n");
            }
            /* ...and the other side of it (fad4f40, re-earned 2026-08-16):
             * the SAME evidence shape on a dawn RAMP must NOT start a hold,
             * or the frozen reference walks a probe volley down the descent.
             * The only thing separating the two is how far the reference has
             * been allowed to lead the live baseline - 4.2% for the step
             * above, 15.4% here, straight off the corpus 09 log. */
            dn_evidence ramp = chase;
            ramp.night_baseline = 3398.0f;
            ramp.brighten_ref   = 3920.0f;      /* 15.4% lead = a ramp */
            ramp.smooth_tg = ramp.min_smooth_since_probe = 3038.0f;
            dn_probe_plan pr2 = dn_next_probe(&ramp);
            g_pairs++;
            if (pr2.brighten_started) {
                g_viol++;
                printf("VIOLATION (kinder-links B3): a stale frozen reference "
                       "started a hold on a dawn ramp - this is fad4f40's "
                       "probe volley, ref %.0f leading baseline %.0f by "
                       "%.1f%%\n", (double)ramp.brighten_ref,
                       (double)ramp.night_baseline,
                       (double)((ramp.brighten_ref / ramp.night_baseline - 1.0f)
                                * 100.0f));
                show("ramp    ", &ramp, &pr2);
                printf("\n");
            }
            /* the max(): a stale reference BELOW the live baseline must never
             * hand a brighter scene a lower bar than a dimmer one gets */
            dn_evidence stale_dim = chase, stale_bright = chase;
            stale_dim.brighten_ref = stale_bright.brighten_ref = 1400.0f;
            stale_dim.smooth_tg = stale_dim.min_smooth_since_probe = 1500.0f;
            stale_bright.smooth_tg =
                stale_bright.min_smooth_since_probe = 1300.0f;
            assert_monotone(&stale_dim, &stale_bright);
        }
    }

    printf("dn_next_probe properties: %ld base evidence points, "
           "%ld assertions (monotonicity + reconfirm bound + dip memory), "
           "%ld violations\n", g_bases, g_pairs, g_viol);
    if (g_viol) {
        printf("RESULT: FAIL probe-properties\n");
        return 1;
    }
    printf("RESULT: PASS probe-properties\n");
    return 0;
}
