# Day/night - retiring `learn` and hardcoding eight fields

**Date:** 2026-08-22 · **Status:** shipped (`d97f76b`, CHANGELOG Unreleased)
**Relation to the earlier notes:** this does not supersede
`DAYNIGHT_REDESIGN_2026-08-17.md` or `DAYNIGHT_DECISION_2026-08-17.md` - the
automaton those two describe is unchanged. It removes config surface around
it. Note that the decision note **predates** this change by five days and its
"What the morning design loses" section still says "`daynight.learn` stays as
an option that can be switched off": that sentence is now wrong, and this
document is the record of why.

---

## What changed

Two groups, two different reasons.

**Removed outright:** `daynight.learn`, `daynight.state_path`, and the whole
`dn_learn_*` subsystem behind them (load, add, the daily "learned:" line, the
state file).

**Turned into fixed constants** in `src/daynight.h`, still readable in
`GET /control` but no longer settable from config or `POST /control`:

| key | constant | value |
|---|---|---|
| `probe_jump_pct` | `DN_PROBE_JUMP_PCT` | 50 |
| `probe_settle_s` | `DN_PROBE_SETTLE_S` | 8 |
| `ref_delay_s` | `DN_REF_DELAY_S` | 30 |
| `ir_ratio_night` | `DN_IR_RATIO_NIGHT` | 2.0 |
| `ir_ratio_day` | `DN_IR_RATIO_DAY` | 2.0 |
| `ir_min_headroom` | `DN_IR_MIN_HEADROOM` | 8 |
| `boot_settle_s` | `DN_BOOT_SETTLE_S` | 5 |
| `transition_s` | `DN_TRANSITION_S` | 5 |

Every value equals the field's old `config.c` default, so no camera's
behaviour changed at the moment of the commit.

---

## Why `learn` went

`learn` existed for one failure: a camera whose configured `day_gain` is
unreachable for its scene stays in night forever, because every probe fails
its verdict. Three cameras sat in night that way on 2026-08-16. The mechanism
recorded each confirmed day's lowest exposure and, with `learn=1`, let the
median of the last eight raise the effective `day_gain`.

It could not raise it far enough for the cameras that needed it. Its own
safety clamp - never raise past `night_gain/2` - caps a raised threshold at
2048 under the affected cameras' `night_gain` of 4096, and the numbers those
cameras actually demand are above that. From
`private/fleet/camera-fleet.md`:

| camera | figure | where it comes from |
|---|---|---|
| cam-sz (Schlafzimmer) | 3238, then 2712 | failed morning verifies, "Live-Verify-Messungen nach Umstellung", 2026-08-17 |
| cam-db (Dachboden) | 2528 | same table, same morning |
| cam-wohn-ofen | 3025 | the 10:40 snapshot table - its exposure index while in NIGHT, over `day_gain` 768 "deutlich" |

**A correction worth carrying forward:** `d97f76b`'s own commit message, the
CHANGELOG entry, and the fleet document's "Konsequenz für die Schwellwerte"
paragraph all attribute the span "2528-3238" to cam-sz/cam-wohn-ofen. That is
sloppy: 3238 is cam-sz and 2528 is **cam-db**, from the failed-verify table;
cam-wohn-ofen never appears in that table at all (it was in the fast
day->night bounce-back group) and its own figure is **3025**, measured in the
snapshot table. The argument is unaffected - 2528, 3025 and 3238 are all above
the 2048 ceiling, and it is the ceiling that decides - but the three cameras
should not be quoted as if two of them shared one measurement.

A mechanism that cannot fix the incident it was written for is not worth its
config surface, its state file, or its daily log line.
`daynight.diagnose_thresholds` stays and covers the same failure honestly: it
names the value `day_gain` would have to exceed, and leaves raising it (and
`night_gain` with it, which is the part `learn` structurally could not do) to
a deliberate per-room decision.

---

## Why the other eight went

The same argument `DN_TREND_PCT` was already hardcoded on: a config key nobody
ever needed to change per camera is not a config key.

- `ir_ratio_night` / `ir_ratio_day` are thresholds on `r = D(illuminator off)
  / D(illuminator on)`, a **dimensionless** quantity. Absolute exposure spans
  a factor of 63 across this fleet at one instant, which is exactly why the
  ratio was chosen in the first place (`DAYNIGHT_DECISION_2026-08-17.md`); the
  ratio does not inherit that spread. The twelve-camera dusk-to-dawn campaign
  of 2026-08-19 (37-62 probe pairs per camera) put the darkest genuine night
  with AE headroom at r=2.38 and the dimmest confirmed-lit room at r=1.50, and
  anything in 1.8..2.2 produced identical verdicts across the whole campaign.
  Both sit at 2.0, deliberately equal.
- `ir_min_headroom` is the AE reserve below which that ratio means nothing (a
  railed meter cannot respond to the illuminator going off and returns r ≈ 1).
  Measured on a pitch-dark outbuilding: r=1.14 at 1 unit of reserve.
- `ref_delay_s`, `boot_settle_s`, `transition_s` are settle-time floors - AE
  convergence and IR-LED warm-up after a switch or a boot. Every camera's AE
  settles on the same order of seconds.
- `probe_jump_pct` and `probe_settle_s` are the probe economy's own trigger
  bar and verdict delay, swept fleet-wide, never per installation.

They live in `src/daynight.h` rather than `daynight.c` because `control.c`'s
status JSON and `config.c`'s grace-period warning both need the numbers: one
header, one copy, nothing to drift.

---

## Compatibility

A `timps.conf` that still sets one of the ten keys is parsed and the line
ignored, rather than landing in the generic "unknown key" warning.
`learn`/`state_path` warn unconditionally; the eight constants warn only when
the configured value **differs** from the constant, so a config that never
touched one of them is silently unaffected.

`POST /control` is asymmetric here and it is worth knowing before writing a
test against it: one of these keys **alone** is `422 unknown_fields`, but
mixed into a body with valid keys the request is `200` and the key is
**silently dropped**. Two things broke on exactly that asymmetry and were
fixed in the follow-up commits:

- `scripts/dn-scenarios/24-pegged-boot-poisoned-reference.json` set
  `ref_delay_s=5` to make the reference anchor come due while the meter was
  still railed. With the override ignored the anchor moved from t≈19 to t≈46,
  past the scenario's own AE repair at t=30, and the `night reference
  deferred` branch stopped being reached - the scenario stayed green while
  testing nothing. The repair now steps at t=90.
- `scripts/timps-qa.sh` round-tripped seven of the eight as live-settable
  fields and forced `transition_s=2` in the F-08 transition block. The
  round-trips became nine hard FAILs against a current daemon; the
  `transition_s` POST became a no-op that the restore check could not see,
  because both sides read 5. Both replaced: the eight are asserted read-only
  against the constants, and the transition block runs at the real 5 s dwell.

The general lesson: when a key stops being settable, grep for it in
`scripts/` as well as `src/`. A test that configures a value the daemon
ignores does not fail - it quietly stops testing.
