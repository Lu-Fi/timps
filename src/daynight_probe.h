/* daynight_probe.h - the day/night PROBE SCHEDULE as one pure function.
 *
 * Design-notes section 5 ("The destination"): every stuck-mode incident in
 * this subsystem's record lives in probe scheduling, not in the switching
 * machinery. Scheduling used to be seven pieces of loop-local state
 * (verify{}, brighten_since_ms/brighten_armed, probe_backoff,
 * probe_fail_smooth, last_phys_probe_ms) consulted by four interleaved
 * if-blocks inside dn_thread(), which is why the only way to ask "what would
 * this build do with THAT evidence" was to build a camera-shaped harness and
 * wait. Here it is instead ONE function over ONE typed evidence struct, with
 * no clock read, no I/O, no globals and no writes to its input:
 *
 *     dn_probe_plan p = dn_next_probe(&evidence);
 *
 * The win is not fewer lines. It is that a pure function is directly
 * unit-testable and, more importantly, PROPERTY-testable - see
 * tests/dn-probe-props.c, which asserts on every evaluation:
 *
 *   MONOTONICITY. For evidence e1, e2 identical except that e2 is strictly
 *   brighter (lower smoothed gain - the metric is inverted, high gain = dark):
 *        dn_next_probe(e2).in_ms <= dn_next_probe(e1).in_ms
 *   Brighter evidence must never buy a LATER correction.
 *
 * That single property is violated by three of this file's historical
 * incidents - 0f5fc80, 14a1d61 and the 2026-08-14 cam-vorne dawn, where an
 * AMBIGUOUS (dead-zone, i.e. brighter) revert bought the same 4 h schedule a
 * confirmed-night revert does. Three incidents, one assertion, checked by
 * construction on every build instead of re-derived by hand per incident.
 *
 * `in_ms` is the whole point of the return value: the earliest time at which
 * this evidence can produce a PHYSICAL probe (an audible IR-cut drive).
 * 0 = now (act == DN_PROBE_FIRE), DN_PROBE_NEVER = no path schedules one.
 * Every branch below sets it to the schedule that branch actually implies, so
 * the property is a statement about behaviour, not about a decorative
 * parallel computation.
 *
 * The caller (dn_thread) owns all state: it builds the evidence from its
 * locals, and writes the plan's brighten_* fields back. Nothing here mutates
 * anything. */
#ifndef DAYNIGHT_PROBE_H
#define DAYNIGHT_PROBE_H

#include <stdint.h>

/* ---- probe economy (fleet logs 2026-08-03/04, all 11 cameras) -----------
 * Every reconfirm probe is USER-VISIBLE: the board script clunks the IR-cut,
 * kills the IR LEDs and the stream shows ~7-9 s of dark colour video before
 * the revert. The hourly periodic probe alone produced 8-12 such flips per
 * camera per night ("periodische Tag/Nacht-Umschaltungen"), and the
 * sustained-brightening probe added 2-6 more on slow-ramp scenes (pre-dawn:
 * gain declines continuously, each failed probe resamples a LOWER baseline,
 * the ramp re-crosses the new bar, repeat every 10-40 min - with tangent
 * starts like 4898 vs bar 4906). Three measures, all pure scheduling:
 *  - exponential backoff: a probe that FAILS doubles the periodic interval,
 *    x1 -> x2 -> x4 (DN_PROBE_BACKOFF_MAX), bounded by
 *    max(night_reconfirm_s, DN_PROBE_BACKOFF_CAP_S); any genuine transition
 *    or a probe that STICKS resets it. See the trend qualifier below - the
 *    backoff is no longer allowed to apply blindly.
 *  - arming margin: the brightening hold only starts once the smoothed gain
 *    is clearly below the bar, never on a tangent graze.
 *  - failure ratchet: after a failed probe the next brightening probe also
 *    requires smooth gain below DN_RATCHET_MARGIN of the level that just
 *    failed - a quarter stop of NEW brightening. (It required
 *    day_gain_pct% until 2026-08-16; see DN_RATCHET_MARGIN for why borrowing
 *    the day/night discriminator for this made the bar unreachable.) */
#ifndef DN_PROBE_BACKOFF_MAX
#define DN_PROBE_BACKOFF_MAX 4        /* interval multiplier cap (1h->2h->4h) */
#endif
#ifndef DN_PROBE_BACKOFF_CAP_S
#define DN_PROBE_BACKOFF_CAP_S 14400  /* absolute backoff ceiling (4 h) */
#endif
#ifndef DN_BRIGHTEN_MARGIN
#define DN_BRIGHTEN_MARGIN 0.97f      /* hold arms only clearly below the bar */
#endif
/* RATCHET MARGIN (kinder-links 2026-08-16). The failure ratchet used to be
 * day_gain_pct% of the frozen anchor, i.e. it borrowed the DAY/NIGHT
 * DISCRIMINATOR as its "another full trigger-worth of NEW brightening" bar.
 * Those are two different questions and only one of them is about pipelines:
 *  - day_gain_pct (60%) answers "is this the day pipeline or the night
 *    pipeline?". 60% is 0.74 stops, and it is right for that job - an IR-cut
 *    transition moves total_gain by whole orders of magnitude (the fleet logs
 *    show 10944 -> 2102, 4096 -> 1619).
 *  - the ratchet answers "is this NEW evidence, distinguishable from the
 *    evidence that already failed?". That is a noise-and-quantisation
 *    question, and 0.74 stops is wildly more than it needs.
 * The cost of conflating them, measured: on kinder-links the anchor latched at
 * 1653 - the room's ORDINARY resting night level, which is where a failed
 * reconfirm always latches it - so the ratchet demanded 992, i.e. analog gain
 * 62 against a scene living between 85 and 100. The bar sat BELOW THE WHOLE
 * SCENE's nightly range, so the sustained-brightening path was dead for the
 * rest of the night for any indoor-light-sized event, and turning the room
 * light on moved nothing. dn_bar_reachable() cannot catch this: 992 clears the
 * 256 floor comfortably. Physically reachable, scene-wise unreachable.
 *
 * One quarter stop (2^-0.25) instead. Justification, not taste:
 *  - it is ~5x the DN_BRIGHTEN_MARGIN noise bar (3% ~ 1.4 analog units ~ one
 *    sensor gain step), so it still cannot be crossed by AGC hunting;
 *  - it is 8 analog units, larger than any jitter in the fleet traces;
 *  - it still makes the incidents the ratchet exists for unrepeatable, which
 *    is the actual requirement. Both are same-LEVEL re-fires, not brighter
 *    ones: the pre-dawn tangent re-cross (gain 4898 vs a resampled bar 4906,
 *    a 0.16% margin) and the cam-wyze-pan dawn dip (floor ~820 against an
 *    anchor latched at ~820) are blocked by any margin at all, and remain
 *    blocked at 0.84 (they would need 689).
 * So this is strictly a widening of what counts as NEW evidence, bounded well
 * away from noise, and the arming margin + the fresh above->below edge still
 * gate the hold behind it. */
#ifndef DN_RATCHET_MARGIN
#define DN_RATCHET_MARGIN 0.84f       /* one quarter stop of NEW brightening */
#endif
/* STALE-REFERENCE RELEASE (kinder-links 2026-08-16, second order).
 *
 * brighten_ref freezes the baseline so a brightening cannot erode the bar it
 * is measured against. Frozen indefinitely, though, it reintroduces exactly
 * the pre-dawn probe volley fad4f40 removed: on a monotone descent the bar
 * stands still, the ramp walks into it, the probe fails, the baseline
 * replants lower, the reference re-freezes there, and the ramp walks into the
 * NEXT one. Measured on corpus scenario 09 (the cam-vorne dawn) when the
 * freeze was first added without this: six sustained-brightening pairs down
 * one ramp, 14 board switches against a budget of 9. The chasing baseline had
 * been suppressing that volley all along - accidentally, and at the cost of
 * suppressing every genuine light-on too.
 *
 * So the freeze needs a release, and the discriminator is already in the two
 * numbers: how far the frozen reference has been allowed to LEAD the live
 * baseline. A step contaminates it by a couple of percent; a ramp slow enough
 * for the baseline to track separates them by a lot, because the baseline has
 * been chasing for the whole descent. Measured, same two scenarios:
 *      corpus 10, a 23% step:      ref 2396 vs baseline 2343  = 2.3%
 *      corpus 09, a dawn ramp:     ref 3920 vs baseline 3398  = 15.4%
 *                                  ...and 14.8 / 10.9 / 10.1 / 10.4 / 11.7%
 *                                  at the five successive holds after it.
 * Derivation rather than curve-fitting: a legitimate step has to survive its
 * approach plus one confirm window. The baseline closes (1 - e^(-T/tau)) of
 * the gap, tau = 500 ms / DN_BASELINE_ALPHA = 250 s, and the gap is at most
 * (100-pct)/2 = 22.4% of the reference; for T = 60 s (a 30 s approach and the
 * 30 s confirm) that is 21.3% of 22.4% = 4.8%. A ramp the baseline can track
 * is >= 10% by the time it reaches the bar. 6% sits between the two with 1.25x
 * over the derived requirement and 1.7x under the measured ramp floor.
 *
 * Deliberately expressed as a DIVERGENCE and not as a timer, which it is
 * otherwise equivalent to: the baseline's drift rate is fixed, so "how far has
 * the reference led" and "how long has it been frozen" are the same
 * measurement - and the divergence form needs no second clock field and keeps
 * dn_next_probe() reading now_ms and nothing else. It is also the honest
 * statement of the limit: the fast path is for STEPS. A light that ramps up
 * over several minutes exceeds the lead, releases the reference and is handled
 * by the periodic reconfirm instead, which is the right division of labour -
 * see corpus scenario 11. */
#ifndef DN_HOLD_REF_LEAD
#define DN_HOLD_REF_LEAD 1.06f        /* frozen ref may lead the baseline 6% */
#endif
/* 60000 -> 30000 (2026-08-12, "switching feels sluggish"): with the direct
 * adaptive night->day switch removed (b4a54f0), this hold IS the entire
 * night->day latency for the everyday "a light came on" event - the smoothed
 * gain crosses the bar within ~2-11 s of the step (DN_SMOOTH_ALPHA=0.1 at a
 * 500 ms tick), so the confirm window was ~85% of the wait. The hold does two
 * jobs and only one of them needs 60 s worth of anything:
 *  - reject transients (don't clunk the IR-cut for passing headlights). Cost
 *    of halving: a brightening lasting 30..60 s now buys one probe pair where
 *    it used to buy none. That is BOUNDED, not open-ended - probe_fail_smooth
 *    latches on the failed probe and the ratchet then demands another full
 *    day_gain_pct%-worth of NEW brightening, so it stays at most ONE pair per
 *    night entry exactly as at 60 s.
 *  - let smooth_tg converge to the dip floor before the ratchet latches on it
 *    (see the dawn analysis at the night->day decision). This is the load-
 *    bearing one, and it is satisfied with room to spare: the EMA residual is
 *    0.9^N, so 30 s (60 ticks) is 99.8% converged vs 99.9998% at 60 s. Traced
 *    against the cam-wyze-pan dawn numbers the ratchet would latch at 838
 *    instead of ~820 - a 2% difference in a bar that sits at 60% of it.
 * Below ~20 s that second job starts to erode (98.5% at 40 ticks), so 30 s is
 * the floor this reasoning supports, not an arbitrary halving. */
#ifndef DN_BRIGHTEN_CONFIRM_MS
#define DN_BRIGHTEN_CONFIRM_MS 30000  /* sustained-brightening probe confirm */
#endif

/* ---- unsatisfiable-bar guard (design-notes generator C, 2026-08-14) ------
 * Several bars here are derived as a FRACTION of a previously measured gain
 * (day_gain_pct% of the night baseline; day_gain_pct% of a failed probe's
 * latched level; the brightening probe bar halfway between them). total_gain
 * has a hard physical floor at 256 - the IMP [24.8] linear scale's 1.0x, i.e.
 * log2 gain units 0, below which no sensor register can go - so any derived
 * bar that lands under it is not "strict", it is UNSATISFIABLE: the
 * comparison it gates can never come true and the whole path it guards is
 * silently dead. That is the terminal state of the ratchet's own recursion,
 * reached on cam-vorne 2026-08-14 after four probes. Rule: any bar derived
 * from a measurement is checked against the measurement's physical range at
 * the moment it is latched, and an unsatisfiable one is warned about. */
#ifndef DN_GAIN_FLOOR
#define DN_GAIN_FLOOR 256.0f     /* [24.8] linear 1.0x - hard sensor floor */
#endif
static inline int dn_bar_reachable(float bar)
{
    return bar >= DN_GAIN_FLOOR;
}

/* The value the sustained-brightening hold ACTUALLY compares smooth_tg
 * against, given a rest reference. One definition, used both by the schedule
 * itself and by the generator-C guard at the baseline plant/drift, because
 * having those two derive it separately is precisely how the guard came to be
 * checking a different number from the gate it was guarding:
 *
 * Schlafzimmer (192.168.241.170) 2026-08-16, ~10:03-11:28. dn_bar_check() was
 * handed the NOMINAL bar, baseline*(100+pct)/200, while the gate is that bar
 * times DN_BRIGHTEN_MARGIN. At baseline 326 the nominal bar is 260.8 - over
 * the 256 floor, so the guard stayed silent - and the operative gate is
 * 252.98, under it, so the brightening path was dead. An hour wrong-mode in
 * daylight with zero diagnostic output, ended by a human raising
 * total_gain_day_threshold to 700 by hand. The silent dead band is exactly
 * baseline in [320.0, 329.9): wide enough to land in, narrow enough that four
 * previous incidents missed it. Note the guard was RIGHT either side of it -
 * at baseline 315 after a restart the nominal bar was 252 and it fired - which
 * is what made the failure look like a camera-specific quirk rather than an
 * off-by-one-multiplication.
 *
 * Rule this encodes, beyond the arithmetic: a guard must be handed the value
 * the guarded comparison uses, not a value derived alongside it. */
static inline float dn_hold_gate(float ref, int day_gain_pct)
{
    return ref * (100.0f + (float)day_gain_pct) / 200.0f * DN_BRIGHTEN_MARGIN;
}

/* what the plan asks the caller to do this tick */
enum {
    DN_PROBE_NONE = 0,   /* nothing due (or an oscillation freeze holds it) */
    DN_PROBE_FIRE,       /* drive the board NOW - one audible IR-cut pair */
    DN_PROBE_SKIP        /* the deadline came due but the evidence does not
                          * justify spending a click: re-arm, no board drive */
};

/* which of the three schedules won, in priority order (ties go to the lower
 * number, matching the order the old interleaved if-blocks ran in) */
enum {
    DN_PATH_NONE = 0,
    DN_PATH_PERIODIC,    /* periodic / adopted-night reconfirm deadline */
    DN_PATH_BRIGHTEN,    /* sustained-brightening hold */
    DN_PATH_PREBASELINE  /* pre-baseline day-trigger probe */
};

/* "no path schedules a physical probe from this evidence" */
#define DN_PROBE_NEVER INT64_MAX

/* Everything the schedule is allowed to look at. Deliberately a VALUE type
 * with no pointers: the whole input to a decision fits in one struct that a
 * test can build literally, which is what makes the property test possible.
 * Gains are IMP [24.8] linear (256 = 1x, INVERTED: high = dark); <=0 means
 * "not available", never "zero light". */
typedef struct {
    int64_t now_ms;              /* monotonic ms (the ONLY clock input) */

    /* --- config snapshot (already taken under the config lock) --- */
    int   day_gain_pct;          /* adaptive regime iff 0 < pct < 100 */
    int   night_reconfirm_s;     /* periodic reconfirm interval, 0 = off */
    int   probe_max_skip_s;      /* outer bound on evidence-skipped probes */
    int   transition_s;          /* minimum dwell between board drives */
    float day_threshold;         /* total_gain_day_threshold */

    /* --- measurement --- */
    float smooth_tg;             /* night-only EMA of total_gain */
    float night_baseline;        /* adaptive baseline - DRIFTS, see below */

    /* --- frozen references (generator D: an "is there evidence of change?"
     * test must be anchored on a frozen measurement, never on a value some
     * other mechanism updates). probe_fail_smooth is the smoothed night gain
     * at the instant the last physical probe checked and found genuine
     * night; unlike night_baseline nothing drifts it. */
    float probe_fail_smooth;     /* <=0 = no failed probe outstanding */
    /* The lowest SUSTAINED smoothed gain since that anchor was frozen - i.e.
     * the brightest the scene has been, for at least DN_BRIGHTEN_CONFIRM_MS
     * together, at any point since a probe last measured night. The caller
     * maintains it and restarts it with the night session, exactly where
     * probe_fail_smooth is re-frozen.
     *
     * "Sustained" is load-bearing and was not there in the first version
     * (35af4c9), which took a plain running minimum of smooth_tg. That is an
     * ORDER STATISTIC, and unlike every other quantity in this file it has no
     * noise rejection at all: the expected depth of a running minimum grows
     * without bound in the number of samples, so on a perfectly static scene
     * it descends forever until it crosses the 0.97 anchor bar. Worse, the
     * number of samples is set by the reconfirm interval, which is the very
     * thing the backoff lengthens - so the error is self-reinforcing in the
     * wrong direction: longer backoff, more samples, deeper spurious minimum,
     * suspension fires, backoff defeated. Reproduced on a STATIC 3500 gain
     * with realistic stochastic AGC noise and a real 3600 s reconfirm: hourly
     * probes all night, 7 board switches against a budget of 4, and the log
     * lines were structurally identical to the ones corpus 10 asserts as
     * PROOF THE FIX WORKS. The header comment below even predicted the
     * mechanism and then dismissed it, on the strength of a corpus whose
     * noise model is a bounded deterministic sinusoid and therefore cannot
     * exhibit it - see noise_factor() in scripts/dn-replay.py, now stochastic,
     * and corpus scenario 12, which exists to fail on exactly this.
     *
     * The fix keeps the debounce shape the rest of the file already uses: a
     * reading only enters the minimum once the gain has held there for
     * DN_BRIGHTEN_CONFIRM_MS, so an AGC trough a few seconds wide cannot
     * latch anything while corpus 10's ~300 s dip latches with 10x margin.
     * Durability is unchanged - once latched it stays until the anchor is
     * re-frozen - so property 3 says exactly what it said before.
     *
     * Why a minimum and not the current reading (kinder-links 2026-08-16):
     * the trend test below and the skip gate's anchor override are both
     * "has the premise of this backoff been contradicted by MEASUREMENT?".
     * Asking that of the instantaneous gain makes the answer un-askable
     * again the moment the scene dims back, so a brightening that came and
     * went leaves no trace and buys nothing. Observed: 23:21:08 the gain hit
     * 1599 against a 1653 anchor, the suspension fired and pulled the
     * reconfirm from 03:12 in to 00:12 - and then the room dimmed on its own,
     * the predicate went false, and the deadline snapped back out to 03:12.
     * The evidence had existed; the schedule simply forgot it. A running
     * minimum cannot forget, and it is still a MEASUREMENT rather than
     * scheduler state, so generator D's rule (anchor an is-there-evidence
     * test on a frozen measurement) is satisfied by construction.
     *
     * <=0 = nothing sustained since the anchor. */
    float min_smooth_since_probe;

    /* --- scheduling state --- */
    int     backoff;             /* current periodic-interval multiplier */
    int     verify_armed;        /* a NIGHT verify deadline is armed */
    int64_t verify_from_ms;      /* when that deadline was armed ... */
    int64_t verify_at_ms;        /* ... and when it comes due */
    int64_t last_phys_probe_ms;  /* last actual board drive (0 = none yet) */
    int64_t last_switch_ms;      /* last mode switch (dwell clock) */
    int64_t brighten_since_ms;   /* when the brightening hold started (0=no) */
    int     brighten_armed;      /* fresh above-bar -> below-bar edge seen */
    /* FROZEN baseline the hold's bar is derived from - snapshotted on the last
     * tick the scene was still AT or above that bar, i.e. the room's resting
     * level as of the last moment it was not brightening. <=0 = none yet, use
     * the live baseline.
     *
     * Why this cannot be night_baseline (kinder-links 2026-08-16, and it is
     * generator D exactly): night_baseline drifts toward smooth_tg every tick,
     * and the hold's bar is derived FROM it, so the instant a light comes on
     * the bar starts converging on the very reading it exists to detect. The
     * gap it has to close is only (100-pct)/2 = 22.4%, and DN_BASELINE_ALPHA
     * closes that in ~25 s of a real step - against a DN_BRIGHTEN_CONFIRM_MS
     * of 30 s. The debounce loses the race by construction. Traced on the
     * corpus 10 replay, tick by tick: a 23% brightening left the margin test
     * 2.2% short at the moment the step completed and falling from there, so
     * it never opened on any tick; 25 s later the bar had dropped THROUGH
     * smooth_tg, which takes the `>= bar` branch and re-arms - so a
     * permanently brighter room reads as "nothing has changed" from then on,
     * and the fast path is not merely slow, it is unreachable for any step
     * whose size is comparable to the bar itself.
     *
     * Freezing the reference is the same move 19dcd74 made for the unverified
     * day (day_verify_ref anchors "still brightening" on the reading that
     * armed the deadline), and it is what generator D's rule demands: the
     * question "is the scene brighter than its resting level?" has to be asked
     * against what the resting level WAS, not against a number that is busy
     * becoming the answer. */
    float   brighten_ref;
    int64_t osc_freeze_until_ms; /* oscillation-breaker cooldown (0 = none) */
} dn_evidence;

typedef struct {
    int         act;             /* DN_PROBE_NONE / _FIRE / _SKIP */
    int         path;            /* DN_PATH_* that owns in_ms */
    const char *why;             /* fire reason (stable literal), else NULL */
    int64_t     in_ms;           /* earliest possible physical probe; 0 = now,
                                  * DN_PROBE_NEVER = none scheduled. THE
                                  * quantity the monotonicity property is
                                  * stated over. */
    int64_t     rearm_at_ms;     /* DN_PROBE_SKIP: new deadline */

    /* state the caller writes back (the hold is carried between ticks, but
     * it is computed here so the whole schedule stays in one place) */
    int     brighten_armed;
    int64_t brighten_since_ms;
    float   brighten_ref;        /* frozen baseline the hold's bar comes from */
    int     brighten_started;    /* the hold started on THIS evaluation */

    /* derived, for log lines only - never re-derive these at the call site */
    float   probe_bar;           /* baseline-relative "solidly night" bar */
    float   hold_bar;            /* the hold's bar, off the FROZEN reference;
                                  * equals probe_bar until the scene leaves it */
    float   ratchet_bar;         /* frozen-anchor brightening bar (<0 = none) */
    int     anchor_override;     /* the frozen anchor beat the drifting bar */
    int     trend_pulled_in;     /* the backoff was suspended, see below */
    int     eff_backoff;         /* the multiplier actually applied */
    int64_t periodic_due_ms;     /* the deadline after any pull-in */
    int     frozen;              /* an oscillation freeze suppressed a fire */
} dn_probe_plan;

/* the periodic interval at a given multiplier, in seconds */
static inline int64_t dn_reconfirm_iv_s(const dn_evidence *e, int backoff)
{
    int64_t iv  = (int64_t)e->night_reconfirm_s * (backoff > 0 ? backoff : 1);
    int64_t cap = e->night_reconfirm_s > DN_PROBE_BACKOFF_CAP_S
                ? (int64_t)e->night_reconfirm_s
                : (int64_t)DN_PROBE_BACKOFF_CAP_S;
    if (iv > cap) iv = cap;
    return iv;
}

/* TREND-AWARE BACKOFF (design-notes generator E / section 8 item 6).
 *
 * probe_backoff was level-blind: it doubled the reconfirm interval on every
 * failed probe with no regard for which way the scene was moving, and
 * "doubling the reconfirm interval in the middle of a monotone descent is
 * exactly backwards". On cam-vorne 2026-08-14 the backoff hit its x4 cap at
 * 05:58 WHILE the gain was falling through three orders of magnitude, and
 * that cap is what turned a bad revert into a four-hour outage; on Schuppen
 * 2026-08-13 the same cap put the recovering probe 4 h out while the baseline
 * chased the (day-level) gain down.
 *
 * The reference point the test was missing is already in the struct and is
 * already frozen: probe_fail_smooth, the smoothed gain at the instant a
 * physical probe last MEASURED genuine night. If the scene has since moved
 * measurably brighter than that - the very same DN_BRIGHTEN_MARGIN comparison
 * the skip gate's ratchet-anchor override uses (14a1d61) - then the premise
 * the backoff rests on ("this darkness is confirmed, stop clunking hourly")
 * has been contradicted by measurement, and the multiplier is SUSPENDED for
 * as long as that holds.
 *
 * Deliberately a suspension, not a new ladder (design-notes section 7: do not
 * add escalation timers). Its effects are bounded on both sides:
 *  - it can only ever SHORTEN a deadline, never lengthen one, and never below
 *    night_reconfirm_s - i.e. it restores exactly the guarantee b3eec71
 *    introduced night_reconfirm_s to provide and which the backoff has been
 *    quietly eroding ever since. It cannot manufacture an extra probe beyond
 *    the un-backed-off schedule.
 *  - in the case the backoff exists for it costs nothing: a genuinely dark
 *    closet sits AT or above its own probe_fail_smooth, so falling is false
 *    and the multiplier applies unchanged.
 *
 *    This claim was WRONG for one day (35af4c9 -> the same-day follow-up) and
 *    the way it was wrong is worth keeping. The comment that stood here
 *    correctly identified that a running minimum is an extreme-value
 *    statistic which a static-but-noisy scene can drag under the 3% bar, and
 *    then waved it off as bounded-and-anyway-unmeasured, citing a corpus that
 *    was structurally incapable of measuring it (bounded deterministic
 *    sinusoid noise; a window-dependent statistic needs stochastic noise to
 *    falsify). It was not bounded: on a static 3500 scene with realistic
 *    stochastic noise the suspension fired every hour, all night, defeating
 *    the backoff completely. Naming a risk is not the same as bounding it,
 *    and "the tests are green" is worth nothing when the tests cannot express
 *    the risk. min_smooth_since_probe now requires DN_BRIGHTEN_CONFIRM_MS of
 *    dwell before latching, corpus 12 is the null hypothesis that fails if it
 *    ever regresses, and the claim above is once again true as written.
 * Note the two bars compose rather than duplicate: falling (< 0.97x anchor)
 * is by construction the negation of the skip gate's "solidly night"
 * (>= 0.97x anchor), so a pulled-in deadline always finds the gate open - the
 * suspension can never pull a deadline in only to skip it. Both now read the
 * same min_smooth_since_probe, so they still compose exactly.
 *
 * 2026-08-16: the test reads min_smooth_since_probe, not smooth_tg. As first
 * written this was a predicate over the CURRENT reading, recomputed every
 * tick, so it un-fired as readily as it fired: on kinder-links it suspended
 * an x4 backoff at 23:21:08 (gain 1599 vs anchor 1653) and then handed the
 * four hours straight back when the room dimmed again minutes later, long
 * before the deadline it had pulled in. Over a single evidence snapshot the
 * monotonicity property still held - which is why the property test passed -
 * because the defect lived BETWEEN evaluations: brighter evidence had no
 * durable effect. Keying it on the minimum since the anchor was frozen makes
 * "the scene has been measurably brighter than confirmed night" a fact about
 * the interval rather than about this tick, which is what the sentence
 * always meant. Bounds are unchanged: still only ever shortens, still never
 * below night_reconfirm_s, still zero clicks on a scene that never brightens
 * (its minimum simply never goes below the anchor). */
static inline int dn_trend_falling(const dn_evidence *e)
{
    return e->probe_fail_smooth > 0.0f && e->min_smooth_since_probe > 0.0f &&
           e->min_smooth_since_probe < e->probe_fail_smooth * DN_BRIGHTEN_MARGIN;
}

/* The schedule. Pure: same evidence in, same plan out, always. */
static inline dn_probe_plan dn_next_probe(const dn_evidence *e)
{
    dn_probe_plan p;
    p.act = DN_PROBE_NONE;
    p.path = DN_PATH_NONE;
    p.why = 0;
    p.in_ms = DN_PROBE_NEVER;
    p.rearm_at_ms = 0;
    p.brighten_armed    = e->brighten_armed;
    p.brighten_since_ms = e->brighten_since_ms;
    p.brighten_ref      = e->brighten_ref;
    p.brighten_started  = 0;
    p.probe_bar   = 0.0f;
    p.hold_bar    = 0.0f;
    p.ratchet_bar = -1.0f;
    p.anchor_override = 0;
    p.trend_pulled_in = 0;
    p.eff_backoff = e->backoff > 0 ? e->backoff : 1;
    p.periodic_due_ms = 0;
    p.frozen = 0;

    const int adaptive = e->day_gain_pct > 0 && e->day_gain_pct < 100;
    /* the "solidly night" / brightening bar: halfway between day_gain_pct%
     * and 100% of the baseline. 0 when there is no baseline to derive from. */
    if (e->night_baseline > 0.0f)
        p.probe_bar = e->night_baseline *
                      (100.0f + (float)e->day_gain_pct) / 200.0f;
    /* day_gain_pct still gates whether a ratchet is MEANINGFUL here (it is a
     * regime test - the paths that consume the bar all require the adaptive
     * regime), but the bar itself is no longer derived from it. */
    if (e->probe_fail_smooth > 0.0f && e->day_gain_pct > 0)
        p.ratchet_bar = e->probe_fail_smooth * DN_RATCHET_MARGIN;

    /* ---- 1. periodic / adopted-night reconfirm ------------------------ */
    if (dn_trend_falling(e)) p.eff_backoff = 1;
    if (e->verify_armed && e->verify_at_ms > 0) {
        p.periodic_due_ms = e->verify_at_ms;
        if (p.eff_backoff < (e->backoff > 0 ? e->backoff : 1) &&
            e->verify_from_ms > 0) {
            int64_t pulled = e->verify_from_ms +
                             dn_reconfirm_iv_s(e, p.eff_backoff) * 1000;
            if (pulled < p.periodic_due_ms) {
                p.periodic_due_ms = pulled;
                p.trend_pulled_in = 1;
            }
        }
        if (e->now_ms >= p.periodic_due_ms) {
            /* Passive-evidence skip gate (cam-wyze closet, 2026-08-04: "das
             * klacken der IR blende nervt ... nachts andauernd"). The backoff
             * cut the FREQUENCY of the periodic probe, but every firing still
             * physically drives a mechanical relay. On a camera sitting in
             * unchanging darkness a blind scheduled probe accomplishes
             * nothing but that clunk. So only fire when there is passive
             * reason to suspect the state changed - the same signal the
             * brightening hold uses. A FALSE night latch (actually daytime
             * behind an engaged IR pipeline) reads LOW gain, which is exactly
             * the evidence that opens this gate; only a genuinely-dark scene,
             * where a probe could only fail anyway, is skipped. */
            int can_judge = e->night_baseline > 0.0f && e->smooth_tg > 0.0f;
            int solidly_night = can_judge &&
                e->smooth_tg >= p.probe_bar * DN_BRIGHTEN_MARGIN;
            /* Baseline-drift / failure-ratchet reinforcement loop (Schuppen
             * 2026-08-13, 14a1d61). night_baseline drifts toward smooth_tg
             * every tick, and probe_bar is derived FROM it, so over hours of
             * night dwell the bar chases whatever the gain currently reads -
             * even a genuinely DAY-level gain if the mode is wrongly still
             * night - and "still deep in night" stays true indefinitely. Two
             * independent verify-before-trusting mechanisms ended up
             * validating each other instead of either being the other's
             * escape hatch. probe_fail_smooth is immune to the chase: it is
             * frozen at a measurement. While a ratchet is outstanding, ALSO
             * require the gain not to have moved meaningfully brighter than
             * that anchor. */
            /* Read the MINIMUM since the anchor was frozen, for the same
             * reason dn_trend_falling() does (kinder-links 2026-08-16): the
             * deadline is a single instant, so testing the instantaneous
             * gain there throws away every brightening that happened between
             * deadlines. On the incident night that is the difference
             * between skipping (gain 1650 vs a 1603 bar, 3% short, re-arm
             * +4 h) and firing on the 1599 the scene had actually reached. */
            if (solidly_night && e->probe_fail_smooth > 0.0f) {
                solidly_night = !(e->min_smooth_since_probe > 0.0f &&
                                  e->min_smooth_since_probe <
                                      e->probe_fail_smooth * DN_BRIGHTEN_MARGIN);
                p.anchor_override = !solidly_night;
            }
            /* the safety net that survives a permanently-flat gain (a truly
             * stuck reading evidence alone can never clear): once
             * probe_max_skip_s has passed since the last ACTUAL physical
             * probe, fire regardless - "trust nothing, double-check". The
             * very first probe of a session always fires. */
            int outer_bound_due = e->last_phys_probe_ms == 0 ||
                (e->now_ms - e->last_phys_probe_ms >=
                 (int64_t)e->probe_max_skip_s * 1000);
            if (solidly_night && !outer_bound_due) {
                int64_t iv = dn_reconfirm_iv_s(e, p.eff_backoff);
                p.act = DN_PROBE_SKIP;
                p.rearm_at_ms = e->now_ms + iv * 1000;
                p.in_ms = iv * 1000;
            } else {
                p.act = DN_PROBE_FIRE;
                p.why = "periodic reconfirm probe";
                p.in_ms = 0;
            }
        } else {
            p.in_ms = p.periodic_due_ms - e->now_ms;
        }
        p.path = DN_PATH_PERIODIC;
    }

    /* ---- 2. sustained-brightening hold -------------------------------- */
    /* Gain held below the halfway point between day_gain_pct% and 100% of the
     * baseline for DN_BRIGHTEN_CONFIRM_MS: a real light came on but not
     * enough to cross the strict adaptive bar, so probe the day pipeline and
     * let ITS calibrated thresholds decide. The hold only arms on a fresh
     * above-bar -> below-bar EDGE of the smoothed gain, so after a failed
     * probe the scene sits below the bar DISARMED until it genuinely darkens
     * again - identical darkness can never re-fire it.
     *
     * The bar comes off brighten_ref, a FROZEN snapshot of the baseline taken
     * on the last tick the scene was still at or above it, never off the live
     * drifting baseline - see brighten_ref in dn_evidence for the measured
     * reason. Note what this does NOT relax: the arming edge, the 30 s
     * confirm, the failure ratchet, the transition_s dwell and the oscillation
     * breaker are all untouched, so a passing headlight is rejected exactly as
     * before (a 2 s blip barely moves a 5 s-tau EMA, and could not hold for
     * 30 s if it did). What changes is only that a sustained brightening stops
     * being erased by the reference it is measured against. */
    if (adaptive && e->night_baseline > 0.0f && e->smooth_tg > 0.0f) {
        int64_t hold_in = DN_PROBE_NEVER;
        /* The reference is the HIGHER of the frozen rest level and the live
         * baseline. The max is not cosmetic - without it the schedule is not
         * monotone, and the property test finds it: a frozen ref BELOW the
         * live baseline would hand a brighter scene a lower bar than a dimmer
         * one gets, i.e. brighter evidence buying a later probe, the exact
         * shape of 0f5fc80/14a1d61. It is also the physically right rule: if
         * the baseline has risen above the frozen rest level the room has
         * genuinely got darker since, and that newer, darker rest level is the
         * honest thing to measure a brightening against. Freezing only ever
         * protects the reference from being dragged DOWN by the event it is
         * supposed to detect. */
        float   hold_ref = e->night_baseline;
        if (p.brighten_ref > hold_ref) {
            hold_ref = p.brighten_ref;
            /* ...but only while it is still a RECENT rest level. Once it leads
             * the live baseline by more than DN_HOLD_REF_LEAD the scene has
             * been below it long enough for the baseline to have moved on:
             * that is a ramp, not a step, and holding the old reference would
             * walk a probe volley down it. Fall back to the live baseline,
             * which is what suppresses the descent. */
            if (hold_ref > e->night_baseline * DN_HOLD_REF_LEAD)
                hold_ref = e->night_baseline;
        }
        /* The reference tracks the live baseline only while the scene is AT
         * REST - within DN_BRIGHTEN_MARGIN of it - and freezes the moment the
         * scene is measurably brighter than that. Freezing at the BAR instead
         * (the obvious place) is far too late: the bar is 22.4% away, so by
         * the time the scene reaches it the descent has been under way for
         * tens of seconds and the baseline has been chasing it the whole time.
         * Measured on the corpus 10 replay: freezing at the bar captured 2353
         * where the true pre-event rest level was 2397, and that 1.8% of
         * contamination was itself enough to keep the margin shut. The 3% band
         * is the same "measurably brighter" notion the trend test and the
         * anchor override use, and it is wide enough that AGC noise cannot
         * freeze the reference spuriously - and harmless if it briefly does,
         * since the next at-rest tick resumes tracking. */
        if (e->smooth_tg >= hold_ref * DN_BRIGHTEN_MARGIN) {
            p.brighten_ref = e->night_baseline;
            hold_ref       = e->night_baseline;
        }
        p.hold_bar = hold_ref * (100.0f + (float)e->day_gain_pct) / 200.0f;
        if (e->smooth_tg >= p.hold_bar) {
            p.brighten_armed    = 1;
            p.brighten_since_ms = 0;
        } else if (p.brighten_armed) {
            if (!p.brighten_since_ms) {
                /* hold-start gates: the arming MARGIN (clearly below the bar,
                 * never a tangent graze - fleet logs show holds starting 0.2%
                 * under) and the failure RATCHET (after a failed probe,
                 * another full trigger-worth of NEW brightening below the
                 * level that already failed, or a slow ramp re-fires every
                 * time it re-crosses the freshly resampled baseline's bar). */
                if (e->smooth_tg < dn_hold_gate(hold_ref, e->day_gain_pct) &&
                    (p.ratchet_bar < 0.0f || e->smooth_tg < p.ratchet_bar)) {
                    p.brighten_since_ms = e->now_ms;
                    p.brighten_started  = 1;
                    hold_in = DN_BRIGHTEN_CONFIRM_MS;
                }
            } else {
                int64_t ready = p.brighten_since_ms +
                                (int64_t)DN_BRIGHTEN_CONFIRM_MS;
                int64_t dwell = e->last_switch_ms +
                                (int64_t)e->transition_s * 1000;
                if (ready < dwell) ready = dwell;   /* both gates, ANDed */
                hold_in = ready > e->now_ms ? ready - e->now_ms : 0;
            }
        }
        if (hold_in < p.in_ms) {
            p.in_ms = hold_in;
            p.path  = DN_PATH_BRIGHTEN;
            if (hold_in == 0) {
                p.act = DN_PROBE_FIRE;
                p.why = "sustained brightening probe";
            }
        }
    }

    /* ---- 3. pre-baseline day-trigger probe ---------------------------- */
    /* Before the night baseline has been planted, a gain reading under the
     * static day threshold must not full-switch to day - through the night/IR
     * pipeline it is ambiguous between "lights came on" and "strong IR return
     * in darkness" (cam-wyze-pan rests at ~256 under IR vs the 300 threshold;
     * the resulting instant full switches re-tripped the oscillation breaker
     * forever). Fire the PROBE machinery instead: a genuine lights-on sticks
     * within seconds, darkness reverts cheaply and arms the ratchet, which
     * blocks an identical re-fire - the loop terminates after one pair. */
    if (e->night_baseline <= 0.0f && e->day_gain_pct > 0 &&
        e->smooth_tg > 0.0f && e->smooth_tg < e->day_threshold &&
        (p.ratchet_bar < 0.0f || e->smooth_tg < p.ratchet_bar)) {
        int64_t dwell = e->last_switch_ms + (int64_t)e->transition_s * 1000;
        int64_t in = dwell > e->now_ms ? dwell - e->now_ms : 0;
        if (in < p.in_ms) {
            p.in_ms = in;
            p.path  = DN_PATH_PREBASELINE;
            if (in == 0) {
                p.act = DN_PROBE_FIRE;
                p.why = "pre-baseline day-trigger probe";
            }
        }
    }

    /* ---- 4. oscillation-breaker cooldown ------------------------------ */
    /* While frozen, suppress probes too: a probe would flip the very mode the
     * freeze is holding and restart the loop. This is a floor on in_ms rather
     * than a special case, so the property above still describes the truth -
     * no physical probe can happen before the freeze lifts, whatever the
     * evidence says. A SKIP still re-arms (it drives nothing). */
    if (e->osc_freeze_until_ms && e->now_ms < e->osc_freeze_until_ms) {
        int64_t left = e->osc_freeze_until_ms - e->now_ms;
        if (p.act == DN_PROBE_FIRE) {
            p.act = DN_PROBE_NONE;
            p.frozen = 1;
        }
        if (p.in_ms < left) p.in_ms = left;
    }
    /* a hold that started on a tick where a DIFFERENT path fires is
     * bookkeeping the caller is about to reset anyway - do not claim it
     * started, so the log line stays one-per-real-hold */
    if (p.act == DN_PROBE_FIRE && p.path != DN_PATH_BRIGHTEN)
        p.brighten_started = 0;
    return p;
}

#endif /* DAYNIGHT_PROBE_H */
