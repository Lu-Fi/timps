# Open items

Working list. Newest block first; each entry says what is established and what
is still guesswork, so nobody has to re-derive it.

## RESOLVED (diagnostics), OPEN (root cause acceptable, not acted on): cam-vorne AU-drop during 2h drift QA

Found 2026-08-23 in a `timps-qa.sh --profile drift` run against cam-vorne
(T23N, 1080p `cbr` 2000 kbit/s): one throttled `AU exceeds max buffer` line
(need=1116108, max=1048576 `MS_AU_BUF_MAX`). Root-caused via the T23
rate-control investigation: the classic-API controller is quality-seeking,
not rate-seeking (min_qp=20 default), so a complex-scene IDR at 1080p
legitimately reaches ~1 MB regardless of the configured bitrate target -
this can happen on ANY T23 board at 1080p, not something specific to
cam-vorne's config (verified stock defaults). The no-force-IDR design
(commit `8128201`, a real prior stall incident on the same board family) is
correct and untouched; actual recovery is simply the next scheduled IDR
(<= `videoN.gop` frames, ~2s at the default gop=50/25fps) - NOT the
`fanqueue_take_dropped_key`/`hub_request_idr` path the old code comment
claimed, which only fires on consumer-queue evictions and never sees a
producer-side drop. Comment corrected.

**Fixed**: the exact count was previously unobservable (log throttled to
every 20th event) - `encoder.<n>.au_drops` in `/control` now exposes the
true cumulative count per channel (`77e8ad1`).

**Deliberately not changed**: `MS_AU_BUF_MAX` (a single global 1 MiB
constant, not resolution/bitrate-aware) was NOT raised - the rationale
against a bigger buffer costing RAM is stale since the P-01 pool rework,
but a >1MB packet entering a client fanqueue (`FQ_MAX_BYTES`=2MB) would
evict nearly a full queue per push, and that trade-off needs hardware
validation before touching it. Follow-up once `au_drops` has real fleet
data: if it climbs steadily rather than staying near-zero, the two levers
are a T23-specific `-DMS_AU_BUF_MAX` bump or raising `video0.min_qp` (the
proven T23 lever from the 2026-08-21 investigation) - pick whichever the
actual drop rate justifies, not preemptively.

## PARTIALLY HARDWARE-VERIFIED (v1.9.3): seven review findings (A1, A3, A4, B1, B2, B5, B7)

Implemented 2026-08-23 on worktree branch `agent-fixes-a2492577`, on top of
`worktree-agent-ad3cd67597597d1e1`, merged to `main` as `5df5f05`. One commit
per finding so they can be cherry-picked individually. Builds clean (`make
sim`, `make test-config` 12/12, `make test-auth` 4/4).

**Normal-operation hardware verification, same day**: flashed to cam-garage
and cam-vorne (v1.9.3), full `timps-qa.sh --profile standard` run against
both - 0 FAIL on either (144/1 warn, 142/3 warn; warnings pre-existing and
already understood: motion-gated recorder with motion off, one ffmpeg
warning, expected fps degradation at the 8-client load ceiling). This
confirms nothing here broke ordinary operation.

**Still NOT verified**: A1 and B1's actual escalation/alarm paths. Neither
run above deliberately drove the daemon into 10 failed starts the way
cam-kinder-rechts' `--test-encoder` stress test did for the retry-cap fix
(2026-08-22) - a clean QA pass says these two boards work normally, not that
the reboot escalation or the bring-up teardown deadline fire correctly under
real fault conditions. That still needs the same kind of deliberate
reproduction cam-kinder-rechts got. B7's fail-closed TLS path is likewise
unexercised against a real broken cert on hardware.

- **A1** (`main.c`): the escalation branch rebooted even when the one-shot
  marker could not be written. On an unwritable `/etc` that is a reboot
  every ~8 min forever. Now marker-write failure means give up WITHOUT the
  reboot.
- **A3** (`config.c`): `key = "value" # comment` kept both the quotes and
  the comment. The unquote step now finds the real closing quote (searched
  from the right, so `write_kv_line`'s unescaped-quote round trip survives)
  and cuts the comment behind it. New `make test-config` harness.
- **A4** (`config.c`): the atomic rewrite never checked `ferror()` on the
  read side, so a bad flash block silently committed a truncated config.
  Now it aborts the rewrite and leaves the original alone.
- **B1** (`main.c`): the bring-up retry loop's own `g_hal->stop()` had no
  deadline - a wedged `IMP_System_Exit()` hung the process before the retry
  cap or the escalation could fire. Now bounded by
  `MS_STARTUP_STOP_ALARM_S` (20 s, deliberately not the 3 s shutdown value)
  with a siglongjmp-based guillotine that returns control to the loop
  instead of `_exit()`ing it.
- **B2** (`main.c`): `unlink()` of the marker now warns on anything but
  `ENOENT`.
- **B5** (`hal_ingenic.c`): the audio thread's exit-path AI disable now
  holds `g_ai_lock`, like the other disable site.
- **B7** (`httpd.c`): `http.https=1` with a failed TLS context no longer
  falls back to plaintext on the same port - it refuses to bind.

Still open on these:
1. Hardware verification of A1 and B1. Both are reboot/alarm behaviour that
   cannot be exercised off-device. The host runs used a fault-injected sim
   backend (start() fails, stop() blocks forever) plus `-D` overrides for
   the marker path and the deadline; that proves the control flow, not the
   vendor interaction. What to watch on a real board: a start() failure
   loop must still reach exactly ONE reboot, and a `chattr`/read-only
   `/etc` must produce "giving up WITHOUT the escalation reboot" and stay
   down.
2. B1's abandoned-teardown policy is a judgement call worth a second
   opinion: an abandoned `stop()` does NOT retry, it goes straight to the
   one-shot escalation. Reasoning is in the commit message (init()/start()
   have no deadline of their own, so retrying on a half-torn-down libimp
   risks a second, unbounded hang). If a board ever turns out to have a
   stop() that is slow-but-honest past 20 s, this trades a retry for a
   reboot - raise `MS_STARTUP_STOP_ALARM_S` rather than removing the
   deadline.
3. B7 has never run against real mbedTLS: the host has none, so the
   fail-closed decision was driven through a stub `tls.c`. Worth one
   deliberate test on a camera with a deliberately corrupted
   `http.tls_cert` - expected: no listener on the http port, RTSP
   unaffected, one LOGE naming the cert.
4. B7 leaves one asymmetry on purpose: a build WITHOUT `USE_TLS` and
   `http.https=1` still serves plaintext (now at LOGE instead of silently),
   because the operator cannot fix that without a different binary. If the
   fleet standardises on `USE_TLS=1`, revisit and make that case fail
   closed too.
5. B5's race is not reproducible off hardware; the fix is by inspection
   against the documented invariant at the other `g_ai_lock` call site.

## RESOLVED: cinnado_d1_t31l "idle" CPU was Frigate, not a bug

Found 2026-08-23 via `timps-qa.sh`'s "expected near-idle - possible busy-wait
loop" check on cam-kinder-rechts (24.1%, WARN). Cross-checked two sibling
`cinnado_d1_t31l_sc2336_atbm6031` boards (cam-db 16.6%, cam-sz 36.3%) against
one different board (cam-garage, 0.0%) and initially read this as a
board-family characteristic worth investigating. User confirmed same day:
these boards are pulled continuously by Frigate (NVR), which QA's own
client-tracking has no visibility into - "zero clients" only meant "zero
clients from this QA run's perspective," not zero real load. Not a timpsd
bug, not board-specific; closed. Leaves a minor QA-script sharpness note (not
acted on): the idle-CPU check does not verify zero clients are ACTUALLY
connected before declaring a baseline "idle", it just assumes so between its
own test phases.

## HARDWARE-VERIFIED (fleet-wide, v1.9.2/v1.9.3): boot MEASURES day/night instead of trusting the persisted value

Implemented 2026-08-22 on worktree branch `worktree-agent-boot-measure`,
merged to `main` as `50cc113`. Design and the shape of the change are in
CHANGELOG [1.9.3] ("Day/night boot now MEASURES before it decides") and
`docs/wiki/Day-Night.md` section "Boot"; the original ask is preserved
verbatim below. Deployed to all 12 fleet cameras 2026-08-22/23.

**Hardware-verified, both halves**: the boot-time measurement itself was
confirmed across all 7 cameras rolled out that first evening (dark-time
boot -> correct `night` decision -> `/sbin/daynight night` actually run,
`/run/thingino/daynight_mode`=night, IR-cut/LEDs physically correct - no
manual re-assert needed, unlike the pre-fix behaviour that started this
whole redesign). The RUNTIME transition logic was independently confirmed
by a genuine dawn crossing during the same session: cam-vorne's own silent
brightness probes (r=1.00-1.95, room light not IR reflection) correctly
detected first light and switched to day with an ISP-confirmed re-assert,
unprompted - real evidence the daynight state machine still works after
the boot-sequence rewrite, not just the new code path in isolation.
Summary of what shipped:

- Boot no longer adopts `running_mode`. It waits for AE convergence
  (unchanged bounds), runs ONE ordinary probe into the day pipeline - the
  same `dn_switch`, `DN_PROBE_SETTLE_S` verdict and readback gate a runtime
  probe uses, deliberately not a second measurement mechanism - reads it
  against `day_gain`, and asserts the answer with `switch_cmd`.
- Cost: one `switch_cmd` invocation for a day answer (the probe's own drive
  IS the assertion), two for a night answer. Exactly one assertion of the
  DECIDED mode per boot, always.
- `boot_probe=0` now opts out of the measurement, not the assertion. The
  railed-AE (`headroom == 0`) override still forces a measurement, and with
  `boot_probe=1` the boot probe IS the real transition that case needed, so
  it is subsumed rather than special-cased.
- Fallback: no usable exposure reading within `DN_STABLE_MAX_MS` -> adopt
  the persisted value, still assert it once, `WRN` saying it is a fallback.
- Three deliberate non-behaviours, each a scar: the boot probe never takes
  the silent route (its premise - "our illuminator was on" - is unverified
  at boot, and in a dark room the ratio lands on "pegged, therefore night",
  the right mode with no assertion, which is exactly how the desync below
  went unnoticed); it never arms the running_mode re-assert (that is the
  "overwritten twice, eight seconds apart" incident); and it CHARGES
  `probe_min_gap_s` like any other probe (exempting it was tried and
  scenario 02 reproduced incident f8a7b21 - see the commit message and
  CHANGELOG; an earlier revision of this entry said the opposite).

Still open on this:
1. Hardware verification. Nobody has watched a real camera do this. The
   two that matter are a dark-time reboot (must end with `daynight night`
   having run, `/run/thingino/daynight_mode` = night, LEDs on) and a
   daylight reboot (must end in day with ONE `switch_cmd day`, no revert).
2. The `S95timps restart` question from the original ask is still not
   answered, only sidestepped: the new sequence does the same thing on a
   restart as on a power-on boot, because it does not need to know which it
   is. That is a decision, not an oversight - a restart's optics are just as
   unknown as a boot's - but it means a restart now costs one to two
   `switch_cmd` calls where it used to cost none, which is worth watching on
   a camera that gets restarted often (QA `--test-encoder` restarts it
   repeatedly).
3. Stuck-ISP boot: with a board whose ISP does not follow the switch
   (scenario 27's defect class), the boot probe now drags the readback
   enforcement machinery in at t=0, costing three `switch_cmd` calls instead
   of zero. Bounded by `DN_VERIFY_MAX_CYCLES` and it REPAIRS the stuck ISP,
   but it is a real change in a real pathology.
4. The corpus needed real retiming in four places (19, 25, 26, 27), not just
   re-budgeting, because the boot sequence used to SUPPLY setup those
   scenarios depended on - most sharply in 26, where the boot silent probe
   was what measured `filter_cost`, and without it the scenario stayed green
   while its own phase-2 mechanism was never reached. Each carries a dated
   note. A reviewer should read those four notes specifically and decide
   whether the retimes preserve the incident each scenario encodes.
5. `03-noisy-night` is flaky on harness timing (6 switches against a budget
   of 5 on two runs including a pre-change baseline, 4 on a third). Not
   caused by this change, but it means a single green corpus run is not
   proof; run it twice.

Original ask, unchanged:

User feedback 2026-08-22, prompted directly by the IR-desync entry below:
trusting the persisted `running_mode` at boot is fine for a reboot minutes or
hours after the last real reading, but has no real justification for a
camera that was powered off for a long time - the extreme case raised was
"what if it sat in a drawer for a year". A year-old (or even day-old, for a
camera near a window with fast-changing light) persisted value is a guess
with no more authority than a random default; there is no principled reason
to commit the physical hardware to it before checking reality.

Current behaviour (`daynight.c` ~line 1181 onward, see the entry below for
the ISP-vs-switch_cmd split) is a hybrid: it commits `running_mode` to the
ISP immediately from the persisted value, and only OPTIONALLY schedules a
verifying probe afterward (`boot_probe`, on by default) - so the wrong
guess is live, briefly, before anything checks it, and per the entry below
the physical IR/ircut hardware may never get corrected at all if the probe
happens to CONFIRM the stale guess rather than overturn it (nothing forces a
`switch_cmd` call when the decision doesn't change).

The user's position is the boot sequence should be inverted: measure first
(a real exposure/gain reading against the day/night thresholds, the same
kind of measurement `boot_probe` already knows how to take), decide from
that measurement, and only THEN commit `running_mode` to the ISP and
unconditionally call `switch_cmd` once to physically assert it - never
trusting the persisted value as anything more than a hint for which probe
path to prefer (e.g. still using it to pick day vs. night silent-probe
framing where that matters for probe safety, per the existing "AE railed"
boot special-case at ~line 1208).

Deliberately not implemented tonight. This is a real behavioural change to
a subsystem that was reworked and reviewed earlier THIS SAME DAY (the
learning-removal consolidation, see `DAYNIGHT_CONSOLIDATION_2026-08-22.md`)
and has already produced two real incidents THIS EVENING on top of that
(the restart-crash entry below, and the IR-desync entry below it) - both
found only because tonight happened to force restarts/reboots into it.
Rushing a further change into the same subsystem at the same sitting, with
no daylight hours left to observe a real transition before shipping it,
is exactly the kind of pressure that produced the "switch to day was
overwritten twice, eight seconds apart" incident the CURRENT boot logic's
own comment (~line 1188) is scar tissue from. Needs: a clean design pass
(what exactly triggers the always-call-switch_cmd - does it also fire on
every plain `S95timps restart`, not just power-on boot, and is that
distinction even knowable/desirable), then the same read-review-test cycle
the rest of today's daynight work went through, not a same-night patch.

## RESOLVED (hardware-verified fleet-wide): a dark-time reboot leaves the physical IR/ircut desynced from software

Fixed 2026-08-22 by the boot-measures-first change above (same worktree
branch), via option (a) below made unconditional: boot now always ends in
exactly one `switch_cmd` call asserting the mode it decided, whether or not
that mode differs from the persisted one. Confirmed on hardware 2026-08-22/23
across the whole fleet rollout: every camera that booted after dark correctly
ran `daynight night` on its own, no manual `/sbin/daynight night` needed on
any of them (unlike the original 5-camera incident this entry documents).
Option (b) (`/sbin/daynight`'s own startup asserting the persisted mode) is
still worth doing in `thingino-firmware-LuFi` and is NOT made redundant by
this: it would also cover the window before timps starts, and cameras not
running timps at all. Original report follows.



Found 2026-08-22, same evening as the restart-crash entry below and likely to
recur with it: `daynight.c`'s boot path (~line 1181, "Push image.running_mode
into the ISP once... NOT switch_cmd") deliberately skips calling
`switch_cmd` at boot, on the documented reasoning that the physical board is
already in the state matching the persisted `running_mode` and a filter
movement per boot is a real mechanical cost. That assumption holds for a
reboot that happens in daylight or shortly after one, but not for a reboot
that happens after dark with no live day/night transition since: the board
script (`/sbin/daynight`, resolved via `switch_cmd = daynight`) keeps its own
notion of physical state in `/run/thingino/daynight_mode` (tmpfs, reset to
"day" on every reboot, and only ever written by an actual `daynight
day|night` invocation - see its `[ -f "$MODE_FILE" ] || echo "day" >
"$MODE_FILE"` and the IR-CUT/IR-LED GPIO logic further down). If timps boots
straight into a persisted "night" `running_mode` and no dusk/dawn transition
happens afterward, `/sbin/daynight` is never invoked, so `daynight_mode`
stays "day" and the physical IR-cut filter and 850/940nm IR LEDs stay in
their day (LED off) position indefinitely - the ISP is correctly night-tuned
(color/exposure), but there is no IR light to see anything by.

Confirmed on the fleet 2026-08-22 ~22:30: exactly the 5 cameras rebooted
after dark that evening (cam-db, cam-schuppen, cam-sz, cam-wintergarten,
cam-kinder-rechts) had an empty/missing `daynight_mode`; the 7 cameras that
had been running continuously since before dusk all correctly showed
"night". Fixed by hand for tonight (`ssh <ip> /sbin/daynight night` on the 5)
- this is not a software fix, just a physical re-assert. Left unfixed, it
would have silently self-corrected at tomorrow's dusk transition, since that
IS a real transition and does call `switch_cmd`.

Not fixed here because a real fix needs a decision, not just code: either
(a) boot should compare `running_mode` against `/sbin/daynight status` (or
equivalent) and call `switch_cmd` once if they disagree, accepting the
mechanical cost is exactly what the boot path was written to avoid, or (b)
`/sbin/daynight`'s own S06ircut/S10daynightd startup should assert the
persisted mode itself rather than defaulting to "day" and waiting to be
told - that lives in `thingino-firmware-LuFi`, outside timps. Either fix
belongs in that repo's review queue, not slipped in unreviewed here.

Background for the encoder block:
`dev_notes/T23_RATECONTROL_INVESTIGATION_2026-08-21.md`.

## RESOLVED (hardware-verified): a real restart could leave timpsd down (T31, mem-constrained boards)

Root cause found 2026-08-22 (static analysis + the saved qa2-*.log evidence;
fix landed in `main.c`, merged to main as `366917c`): the bring-up treated
init and start failures asymmetrically - `g_hal->init()` retried forever, one
`g_hal->start()` failure exited the process permanently. After a restart the
old instance's rmem is released late (4 s+ even after a clean teardown; worse
when the 3 s shutdown alarm guillotines `g_hal->stop()`), so on 22 MB-rmem
T31 boards init's small allocs eventually pass while start's big contiguous
`Codec_Encode_Create` alloc still fails -> unguarded exit, camera dark. The
QA logs show exactly this two-phase death on all 5 cameras: section 16 found
timpsd still "alive" with 2 threads/0 listeners (= parked in the init retry
loop, all services down), and it was gone later (the one-shot start exit).
cam-garage self-recovered because its pool drained before start ran. Fix:
start failure now unwinds via `g_hal->stop()` and re-enters the retry loop;
plus the shutdown path re-arms the alarm before `g_hal->stop()` so IMP
teardown gets the full 3 s budget. See CHANGELOG [Unreleased].

**Hardware-verified same evening on cam-kinder-rechts** (T31,
`--test-encoder`'s exact restore-restart step that killed it before): the
fix worked as designed - the daemon stayed alive and logged `HAL start
failed - unwinding and retrying in 60s` every ~60 s instead of vanishing.
It did NOT self-recover within ~9 minutes of retrying, though - the rmem
carve-out on this board apparently sometimes needs a real reboot to clear,
not just time, so a real `reboot` was used to restore it, same as the
original 5. The fix's actual guarantee is narrower than "the camera
recovers itself": it guarantees the daemon stays alive and visibly
retrying/logging instead of silently dying, which is what makes the
following bound possible/meaningful.

**Follow-up, also landed same evening**: retrying `start()` forever, per the
above, can mean retrying forever for real (9+ minutes and still going in the
verification run) - a process that LOOKS alive but never serves a frame is
its own failure mode, so `start()` failures are now capped at
`MS_STARTUP_MAX_START_FAILS = 10` (a `#define` next to the retry loop in
`main.c`), after which the daemon gives up loudly (`LOGE` + exit) instead of
spinning indefinitely - the same "exit rather than hang forever, a human or
scheduler restarts it" convention `hal_ingenic.c`'s
`MS_VIDEO_WATCHDOG_MAX_RECOVERIES` already uses for the video watchdog.
`init()` failures are NOT capped (unchanged, still forever) - a
misconfiguration is meant to wait for a human to fix the config, which is a
different class of failure than a resource-drain race. Not
hardware-verified on its own (would need a board whose rmem never clears,
which is hard to arrange on purpose) - reviewed by reading the existing
watchdog precedent and confirmed by `make sim`.

**Second follow-up, same evening**: user feedback on the cap above - retries
alone weren't what fixed cam-kinder-rechts, a real `reboot` was, every time
this incident happened tonight. So after `MS_STARTUP_MAX_START_FAILS` is
reached, `main.c` now escalates to exactly ONE real reboot (`sync()` +
`reboot(RB_AUTOBOOT)`) before giving up for good - "for good" meaning a
persistent marker file (`/etc/timps-startup-reboot.flag`, real flash, not
`/run` tmpfs, so it survives the reboot) records that this incident already
got its one shot. If the SAME incident is still failing 10 more times after
that reboot, the marker is still there, so it gives up permanently instead
of rebooting again - no boot loop. The marker is deleted the moment
`start()` next succeeds, so an unrelated FUTURE incident (even far in the
future) gets its own fresh one-shot reboot rather than inheriting a
permanently spent one.

**Hardware-verified the same night**, deliberately reproducing the exact
stuck state on cam-kinder-rechts again (same `--test-encoder` restore-restart
that caused the original incident): `start_fails` counted cleanly 1/10
through 9/10 over ~8 minutes (each log line matching `HAL start failed
(N/10) - unwinding and retrying in 60s`), SSH went briefly unreachable right
at attempt 10 (the escalation reboot firing), and `/control` was back to 200
~15s later with no manual intervention. Confirmed after: `timpsd` running
under a fresh PID, `/etc/timps-startup-reboot.flag` gone (cleared by the
successful `start()`, as designed), RTSP sessions and the encoder serving
normally, daynight's own boot-verify probe running as expected. The full
cap-then-escalate-then-clear cycle worked end to end on the first real test.

Found 2026-08-22 during the post-rollout fleet QA (`--test-encoder`, which
deliberately forces a real restart to make a restart-bound `rc_mode` change
effective - see RC6b's "rc_mode=vbr is restart-bound here, restarting for
real" in `timps-qa.sh`). On 5 of 12 fleet cameras (all `cinnado_d1_t31l` /
`wuuk_y0510_t31x`, i.e. the T31 boards with 37-38 MB total RAM: cam-db,
cam-schuppen, cam-wintergarten, cam-sz, cam-kinder-rechts) the restart's
encoder re-init hit a repeating `E/Encoder: encoder_init failed` loop (dozens
of attempts over ~20-30s), then on cam-garage's occurrence self-recovered
(matches the earlier 2026-08-22 "did not come back after the final
restore-restart" QA finding on cam-garage, which turned out to be slow, not
dead), but on these 5 the daemon gave up entirely: `E/Alloc Manager: allocMem
mem_manager->alloc is failed`, `E/IMP Alloc APIs: g_alloc.alloc_mem failed`,
`Codec_Encode_Create failed`, then no live `timpsd` process at all - `/control`
and RTSP both fully down, not self-healing.

Kernel command line on the affected boards after a real *reboot* correctly
shows `rmem=22M@0x2a00000` (the normal reserved encoder pool, not the
temporary `rmem=0M` used mid-OTA-upload to free RAM for the transfer) - so
this is not the OTA memory remap leaking into the boot config. The pattern
(fails only after a same-process *restart*, a full power-cycle reboot always
recovers cleanly) points at the IMP SDK's rmem-backed allocator not being
cleanly reusable across a soft process restart on these lower-RAM boards -
plausibly the previous instance's allocations in the physical rmem carve-out
are not fully released before the new instance probes it, and 22 MB has much
less slack to absorb that than it would on a bigger board. Needs a hardware
investigation (does the failing instance's `Alloc Manager` dump - it prints
one, captured in logread - show any `owner=` entries that should have been
freed by the restart path already?) before attempting a fix. Until then,
**avoid `--test-encoder` runs that hit a real restart on T31 boards in
unattended/fleet contexts** - all 5 affected cameras were manually recovered
via SSH `reboot` this session; nothing in the field would have done that on
its own.

## Day/night: code-verified config/status reference (read-only audit 2026-08-22)

Occasion: in a support conversation the `/control` status key `day_trigger`
was mistaken for a settable config field. It is not - it is a read-only
display value (the computed path-C probe bar, see the status table below).
This section is the lookup table that was missing, verified line by line
against `src/daynight.c` (2050 lines, HEAD `6e49752`). Nothing was changed;
three small findings are listed at the end.

### Which document is authoritative where

The redesign doc alone is NOT sufficient to read the current code. The chain:

1. `DAYNIGHT_REDESIGN_2026-08-17.md` - the four-path automaton (A/B/C/D),
   exposure index, boot, learning. Its §12 covers only what changed during
   the 2026-08-17 implementation itself.
2. `DAYNIGHT_DECISION_2026-08-17.md` - supersedes the night-to-day half:
   adds the **silent probe** (`irprobe_cmd`, `ir_ratio_*`, `ir_min_headroom`,
   headroom/"railed is an answer") and **path T** (trend trigger, fixed
   constants `DN_TREND_*`). Its "What shipped - 2026-08-18" section is the
   place to look first when that note and the code disagree.
3. `CHANGELOG.md` 1.9.1/1.9.2 (2026-08-21) - the third wave: reference
   lowering by silent-probe proof, clip protection (`dn_clipped`), ISP
   readback verify/enforce cycle, boot "AE pegged" forced cycle,
   `filter_cost` projection, silent-probe retirement after 2 failures,
   operator probe request (`POST /control {"daynight":{"probe":1}}`).
4. `DAYNIGHT_CONSOLIDATION_2026-08-22.md` - no behaviour change, but the
   config surface shrank by ten keys: `learn`/`state_path` are gone and eight
   fields became `DN_*` constants in `daynight.h`. Read it before trusting
   any `daynight.*` key named in documents 1-3; note in particular that
   document 2 still says "`daynight.learn` stays as an option that can be
   switched off", which document 4 reverses.

`src/daynight_probe.h` (732 lines) no longer exists - dropped, not merged;
the probe plan is now a handful of `if`s inside `dn_thread()`.

### The five trigger paths (the redesign's four, plus T from the decision note)

- **A - day->night, direct measurement** (no probe). In DAY, smoothed
  exposure `s > night_gain` held for `day_confirm_s` -> switch to night
  (daynight.c:1628-1660). Reset whenever `s` drops back below `night_gain`.
- **C - spontaneous jump** (night->day trigger). In NIGHT,
  `s < ref * probe_jump_pct/100` held for `probe_confirm_s` -> ask for a
  probe (1698-1704). `ref` is the PROVEN night level; it moves only on proof:
  anchored after night entry (1672-1693), ratcheted up by a failed audible
  probe (1874-1885), lowered by a silent probe that proved night below the
  bar (1449-1460, 1508-1519).
- **T - trend** (night->day trigger, dawn ramps path C cannot see).
  `ema_fast < 75% of ema_slow` (tau 3/60 min, compile-time `DN_TREND_*`)
  held for `probe_confirm_s` -> probe (1726-1741). Armed ONLY while
  `irprobe_cmd` is set and not retired.
- **B - heartbeat** (safety net, the only bound on a wrong night). Every
  `heartbeat_s` in night a probe fires, UNLESS the scene is flat (sustained
  min not 10% below `ref`, `DN_MOVED_MARGIN`) AND path C is sighted
  (`dn_c_sighted`) AND less than `heartbeat_max_s` has passed since the last
  audible probe - then it defers by another `heartbeat_s` (1743-1771).
- **D - boot**. After `boot_settle_s` + AE settled: persisted day -> verdict
  from the honest reading, zero clicks (1381-1391); persisted night -> boot
  probe if `boot_probe=1` or the AE is railed with 0 reserve, else the first
  heartbeat fires immediately (1352-1380).

All night->day triggers (C/T/B/D and operator request) funnel into ONE
escalation (1774-1846): silent probe first (illuminator off, ratio verdict,
no click, not rate-limited), audible IR-cut probe only when the ratio could
not answer - and the audible one alone is rate-limited by `probe_min_gap_s`.
The audible verdict is binary against `day_gain` (1594-1617).

### Config field reference (all daynight.* keys from src/config.c)

**Stale as of the 2026-08-22 config consolidation** (`learn`/`state_path`
removed outright, eight more fields hardcoded - see
`dev_notes/DAYNIGHT_CONSOLIDATION_2026-08-22.md` and `CHANGELOG.md`
Unreleased).
Line numbers below predate that change (they were verified against HEAD
`6e49752`, ~2050 lines; `daynight.c` is shorter now that the learning
subsystem is gone) and have NOT been re-walked line by line - treat them as
approximate, re-grep before trusting a specific line.

| field | path | what it does (daynight.c) | higher value means |
|---|---|---|---|
| `enabled` | all | 0 = thread measures but forces nothing (1121) | - |
| `mode` | all | `auto` vs `schedule` (calendar-only, 1288) | - |
| `time_night_start`/`time_day_start` | B, schedule | fixed local window (609-619); wins over sun | - |
| `sun_latitude`/`sun_longitude`, `sun_*_offset_min` | B, schedule | sun calendar (627-732); heartbeat dawn pull-in (1896-1898) | - |
| `day_gain` (alias `total_gain_day_threshold`) | probe verdict (C/T/B/D) | audible-probe verdict `s < bar` (1600), boot-day confirm (1383); `bar` is just `dn->day_gain` now that `dn_day_bar()`/`learn` are gone | day confirmed more easily = faster to colour (false day self-corrects via A) |
| `night_gain` (alias `total_gain_night_threshold`) | A | day->night threshold (1628); also silent-probe day projection cap (1495) | stays in day/colour longer |
| `day_confirm_s` | A | hold time above `night_gain` (1631) | more cautious day->night |
| `probe_min_gap_s` | audible probe | ONLY rate limit on audible clicks (1821-1822); floor 60 s | fewer clicks, slower retry after a failed probe |
| `probe_confirm_s` | C, T, B | hold time for C (1702) and T (1739); size of the tumbling window feeding the heartbeat deferral evidence (1170) | needs longer-held evidence = fewer false probes, slower reaction |
| `heartbeat_s` | B | base interval (1749), defer step (1756), reschedule after night entry (1895) and every silent night verdict (1427, 1462, 1521, 1544) | wrong night checked less often |
| `heartbeat_max_s` | B | hard cap on deferral, measured since the last AUDIBLE probe (1753-1755) | flat scenes probed less often |
| `boot_probe` | D | 1 = probe a persisted night at boot (1374) | - |
| `interval_ms` | all | tick (1088), default 2000 | slower sampling everywhere |
| `diagnose_thresholds` | diagnostics | arms the 3-failed-probes warning (885) | - |
| `switch_cmd`/`isp_path`/`trace_path`/`irprobe_cmd` | plumbing | board script (1834), scrape source (311), trace recorder (901), silent probe (1793); all F_NOGET, not POSTable | - |

**Removed outright (2026-08-22):** `learn`, `state_path` - the learning
subsystem's own safety clamp (`night_gain/2`) could not raise `day_gain` far
enough for the cameras that actually needed it
(`private/fleet/camera-fleet.md`); `diagnose_thresholds` above is what is
left, naming the value to raise by hand instead of raising it automatically.

**Hardcoded, no longer POST-able or config-file-settable (2026-08-22):**
`probe_jump_pct` (50), `probe_settle_s` (8), `ref_delay_s` (30),
`ir_ratio_night` (2.0), `ir_ratio_day` (2.0), `ir_min_headroom` (8),
`boot_settle_s` (5), `transition_s` (5) - every camera measured wanted the
same value, so these are now the `DN_*` constants in `src/daynight.h`
instead of `ms_daynight_cfg` struct fields. Still visible read-only in
`GET /control`'s `daynight` status object for diagnostics. A config file
that still sets one of these to something other than its fixed value gets
one warning naming the value now in effect.

No dead config: every field in the table is read by the current code.
`day_trigger` is NOT in this table because it is not config - see below.

### /control status keys ("daynight":{...}) vs the automaton's internals

Read-only, computed per tick (control.c:849-935, daynight.c:1917-1927):

| status key | internal | meaning |
|---|---|---|
| `night_baseline` | `ref` | the PROVEN night level; -1 outside night. Legacy key name - it is NOT the old drifting baseline |
| `day_trigger` | `ref * DN_PROBE_JUMP_PCT/100` | the path-C probe bar; -1 outside night. Legacy key name - it has nothing to do with `day_gain`; `DN_PROBE_JUMP_PCT` is a fixed constant as of 2026-08-22 (was the config field `probe_jump_pct`), so this was never settable and still is not |
| `exposure` | `sm.d` | the RAW exposure index D of the last sample (not the EMA `s` - the trace logs both) |
| `total_gain` | `sm.gain` | gain half only, for continuity with old plots |
| `brightness` | `sm.bright` | thingino formula, display only, no longer a decision path |
| `isp_desync` | debounced `cur != sm.isp` | -1 unknown / 0 in sync / 1 standing |

Everything else in the object is a config echo (the eight fixed `DN_*`
constants included - see the field reference above). No caveat on `day_gain`
any more: the 2026-08-22 consolidation removed `learn`, so the echoed value
is always the one actually applied.

### Deviations from the redesign doc not recorded in its §12

All verified formulas that DO match the draft: the jump trigger (1698), the
EMA/`stable_n` shape (1161-1163), the ref anchor rule (1672), the learn
clamp `clamp(max(day_gain, median*1.6), day_gain, night_gain/2)` (773-785),
atomic <=1/h persistence with silent discard of unknown versions (787-844),
the 3-failed-probes diagnostic (§12.6, 881-896). Deviations:

- `next_heartbeat()` shape: the draft schedules `heartbeat_max_s` as one long
  appointment; the code defers in `heartbeat_s` steps with a hard cap of
  `heartbeat_max_s` since the last audible probe (1750-1771). Same ceiling,
  different mechanism; only the flatness-evidence half is in §12.2.
- `c_sighted()` formula: draft `ref > DN_GAIN_FLOOR*1.5`; code
  `ref * probe_jump_pct/100 > DN_GAIN_FLOOR` (449) - i.e. "the bar itself is
  above the floor", strictly more correct. On the current fleet the
  integration-time ratio is readable everywhere, so it is effectively always
  true and the deferral is bounded by `heartbeat_max_s` alone.
- The draft's calendar gate "and c_sighted()" is dropped: the dawn pull-in
  only ever shortens the interval (dn_secs_to_dawn, 707-732), so gating it
  was pointless. Benign.
- `boot_probe auto|always|never` shipped as plain 0/1 (config.c:993), plus
  the unconditional "AE railed with 0 reserve" forced cycle (1359-1373).
- `state_path` default is non-empty (`/etc/timps-daynight.state`); the
  learning gate is the separate `learn` flag (§12.3), not path emptiness.
- Everything else that differs (silent probe, path T, ref lowering, clip
  guards, verify cycle) is documented in the decision note / CHANGELOG, not
  in §12 - by design, see the chain above.

### Findings (real but minor - nothing changed, listed for triage)

1. **A failed illuminator re-light is not retried.** The verdict path turns
   the LEDs back on with `dn_irprobe(dn->irprobe_cmd, 1)` and ignores the
   return value (daynight.c:1405); `ir_fails` counts only the "off"
   direction (1812). Worse: disabling daynight mid-silent-probe
   (1121-1136) or stopping the thread clears `ir_verdict_at` without
   re-lighting, so a camera can be left with its illuminator off until the
   next probe or switch (up to `heartbeat_s`). Real switches are safe -
   `switch_cmd` re-establishes the lights.
2. **The calendar dawn pull-in survives only until the first reschedule.**
   `hb_at` is pulled toward sunrise only on night ENTRY (1896-1898); every
   later reschedule - heartbeat deferral (1756) and all four silent-probe
   night verdicts (1427, 1462, 1521, 1544) - sets a plain
   `now + heartbeat_s` and drops the sunrise appointment. Bounded by
   `heartbeat_s`, and paths C/T usually catch dawn first, but it deviates
   from the redesign's §4.4 intent ("an appointment exactly at sunrise").
3. **`g_probe_req` is sticky across states.** A probe requested while in
   day, mid-probe, or while `enabled=0` is consumed only by the night
   branch (1734), so it can fire a surprise silent probe hours later at the
   next night entry. `/control` acks it (`g_acc++`) either way.

## OSD config WebUI: "Apply to both" scope + restart on adding a field

Investigated 2026-08-21 against `package/timps/files/www/a/streamer-osd.js`,
`streamer-osd0.html`/`streamer-osd1.html`, `timps-api.js` (WebUI repo) and
`src/control.c`, `src/hal/imp_osd.c`, `src/config.c`, `src/config.h` (timps
repo, read-only — two other agents were live-editing/investigating
`hal_ingenic.c`/`hub.c`/`hub.h`/`enc_caps.h`/`osd_vars.c` for an unrelated
fps/bitrate issue at the time; nothing in those files was touched here).

### 1. "Apply to both streams" silently stops applying to both — FIXED (WebUI-only)

Root cause found and fixed, no daemon change needed. The wire protocol was
always correct: `sendItem()` in `streamer-osd.js` builds
`{osd0:{"N":{...}}, osd1:{"N":{...}}}` for scope `"both"`, and
`control_apply_json()` (`src/control.c:677-692`) applies each `osdS.N.*`
section independently and lively via `imp_osd_apply(s, item)`
(`src/hal/imp_osd.c:615`) — verified by reading both sides end to end,
including `find_obj()`'s exact-name matching (`src/control.c:181`), which
rules out `"osd"`/`"osd0"`/`"osd1"` colliding with each other.

The bug was that the "Apply to" dropdown's value was **never actually
recorded** — `buildCard()` re-derived it from scratch on every render purely
via `itemsIdentical()` (byte-identical `enabled/text/x/y/font_size/color/
transparency/outline/outline_color` on both streams). Every full re-render
(a plain page load, returning to the tab, or — the common case — this same
page's own `/events` "config" SSE echo of the edit it just made, see
`onConfigEvent`, which only skips the reload while an input/select/textarea
has focus) re-ran that check and, since `x`/`y` are in the identity set and
main/substream almost always run at different resolutions, `itemsIdentical()`
was essentially never true in practice. Result: the user picks "Both
streams", edits one field (it correctly lands on both), then the very next
reload flips the dropdown back to "This stream" with no warning — every
subsequent field the user edits on that card silently stops reaching the
other stream, exactly matching "apply to both doesn't work".

Fix applied in `package/timps/files/www/a/streamer-osd.js`: added a
module-level `scopeChoice` map (item index -> last scope the user explicitly
picked via the dropdown), written in the `.osd-scope` `change` listener and
consulted first in `buildCard()`, ahead of the `itemsIdentical()` auto-detect
default. Cleared for an item when its card is removed (`.osd-remove`
handler), so a slot reused later starts from the auto-detect default again
instead of inheriting a stale pick. No server-side change; `src/control.c`
and `src/hal/imp_osd.c` were only read, not modified.

**Still to verify on hardware** (no camera access from this session): open a
stream's OSD page, add/edit an overlay, set its scope to "Both streams",
change two or three fields in sequence (including one that forces a
reload — e.g. tab away and back, or wait for the SSE echo with focus
outside any input) and confirm the dropdown now stays on "Both streams" and
every field after the first still reaches the other stream's `osdX.N.*`
config.

### 2. Restart needed to add a new OSD field — confirmed daemon behavior, not a WebUI bug

The master OSD switch needing a restart was already known
(`osd.enabled`, config-only, `imp_osd_setup()` builds the groups once at
startup — `src/config.c` osd_fields comment, `src/control.c:1071`). The
narrower question was why enabling a single **new/previously-disabled**
overlay item also needs one, since `osdS.N.enabled` is not in `/control`'s
`"restart"` list.

Confirmed from `src/hal/imp_osd.c`:

- `MS_MAX_OSD` is 8 (`src/config.h:9`), `MS_MAX_VSTREAM` is 2
  (`src/config.h:8`) — matches the WebUI's `MAX_OSD = 8` / one page per
  stream.
- `imp_osd_setup()` (`src/hal/imp_osd.c:417`) creates the OSD group
  unconditionally (once osd.enabled or any privacy region is wanted), but
  loops `for (i=0;i<MS_MAX_OSD;i++)` and does `if (!it->enabled) continue;`
  **before** calling `IMP_OSD_CreateRgn()` (line 458-463) — i.e. it only
  allocates an IMP region for items that were already enabled at boot. This
  is asymmetric with privacy cover masks, which the same function
  pre-creates for **all** `MS_MAX_PRIVACY` slots regardless of `enabled`,
  explicitly "so masks can be toggled/moved LIVE without a restart" (line
  498-500 comment).
- `imp_osd_apply()` (`src/hal/imp_osd.c:615`, the function `/control` calls
  live for every `osdS.N.*` POST) checks `rg->rgn<0` and, if so, logs
  "no region on stream %d (disabled at startup) - enable persisted, applies
  on restart" and returns without showing the item (line 626-631). The
  source comment above it says this outright: "an item that was disabled at
  startup has no region ... enabling it live is refused and takes effect
  after a restart."

So this is a genuine **daemon** limitation, not the WebUI being overly
cautious: an item that was off when the streamer last started truly has no
IMP region to show, no matter what `/control` POST is sent, until
`imp_osd_setup()` runs again on the next restart. The WebUI's per-item
"Needs restart" badge (`bootEnabled[S][item]`, captured from the boot-time
GET) tracks exactly this condition and is accurate; `streamer-osd0.html`'s
intro text already documents it correctly ("enabling an overlay that was off
when the streamer last started ... take[s] effect after a streamer restart,
flagged per overlay"). No WebUI change made or needed for this point.

One asymmetry worth a future daemon-side look (not fixed here, out of scope
and `imp_osd.c` was off-limits this session): OSD items could in principle
be pre-created for all `MS_MAX_OSD` slots the same way privacy masks are, to
let a fresh overlay go live without a restart. Also noted: `/control`'s
generic `"restart":[...]` list (`src/control.c:1071`) does not list
`osdS.N.enabled`, but nothing in this WebUI currently reads that generic
list (checked — no `restart` consumer found under
`package/timps/files/www/`), so today that omission has no visible effect.

## OSD `{fps}`/`{bitrate}`: cam-kinder-rechts showing 13 instead of ~25

**PARTLY CLOSED 2026-08-22.** Two separate things live in this entry and only
one of them is settled.

*The magnitude question is answered and was never this bug*: the ffprobe
counts and the driver work below traced the ~13.5 fps to
`isp_ch0_pre_dequeue_time`, fixed and verified on cam-kinder-rechts (see
"RESOLVED: T31+sc2336 fps ceiling fixed" further down).

*The asymmetry the investigation turned up is now fixed*: `hub_get_fps()` has
the same 2 s staleness guard `hub_get_bitrate()` has carried since `eda8302`,
so an idle channel reports 0 instead of a frozen `mfps`. Demonstrated on the
host with the `osd_vars.c` + `hub.c` harness from the `{bitrateN}` item: after
3 s of silence `{bitrate0}` read 0 while `{fps0}` still read 31.0, and with
the guard both read 0.

**What is still NOT established**: whether that asymmetry is what
cam-kinder-rechts was showing. The suggested quick check - what `{bitrate}`
read at the same moment as the 13 - was never run, and the fix was applied
because a frozen fps reading on an idle channel is wrong on its own terms, not
because it was confirmed as the cause. Nobody should read this as "the field
report is explained". If a frozen `{fps}` is ever seen again, it is a new
observation, not a regression of this.

### Original entry

Investigated 2026-08-21 against `src/hub.c`/`src/hub.h` (read-only — another
agent is live-editing `hal_ingenic.c`/`hub.c`/`hub.h`/`enc_caps.h` for the
rate-control work; this was analysis only, nothing here was applied).

**Established from source:**

- `hub_get_fps(src)` (`src/hub.c`) returns `s->mfps`, a real 1-second running
  average of frames actually handed to `hub_publish()`/`hub_publish_take()`
  for that hub source — computed in `hub_prepare_locked()` regardless of
  subscriber count (so it is not "viewer-side" and not gated by who is
  watching).
- `hub_get_bitrate(src)` computes the identical kind of window (`mkbps`) but
  additionally checks staleness: `(ms_now_us() - s->bwin < 2000000) ?
  s->mkbps : 0.0`, i.e. it reports 0 once the source has been idle >2 s
  ("on-demand encoder stopped" — comment in `hub.c` and spelled out
  explicitly in the commit that added it, `eda8302`: *"The getter reports 0
  when the last window is stale (>2s) so an idle on-demand stream shows 0
  rather than a frozen rate."*).
- **`hub_get_fps()` has no equivalent staleness guard.** It always returns
  the last computed `mfps`, however old. This is a provable asymmetry
  against `hub_get_bitrate()`, which was deliberately fixed for exactly this
  failure mode one commit later in the same file.
- Video encoding is on-demand (`hal_ingenic.c`, `video_thread()`): a stream
  with no subscribers and no `vc->active` is stopped
  (`IMP_Encoder_StopRecvPic`) after a 2 s idle debounce
  (`MS_IDLE_STOP_US`). `osd.monitor_stream` defaults to channel 0
  (`config.c:307`); `{fps}`/`{bitrate}` always read that one channel
  regardless of which OSD layer they're printed on (see the second item
  below).
- `imp_chn` (the IMP encoder channel number `hub_publish_take()` uses as hub
  `src`) defaults to the stream index (`config.c:276: v->imp_chn=i`), so
  under the reported config (no `imp_chn` override) channel 0 ↔ hub source 0
  ↔ `video0` — no index-mismatch found there.
- No clean fraction relates 13 to 25 or 30 (not 25/2, not 30·13/25), which
  argues against a deterministic frame-skip/rounding bug in the measurement
  itself and fits a **frozen, arbitrary transient value** better than a
  systematic rate-conversion bug.

**Working hypothesis (not confirmed on hardware):** `video0` on
cam-kinder-rechts had no active subscriber (or was between the last
subscriber leaving and the idle-stop happening) when the OSD read 13. The
displayed 13 is very plausibly a *stale* `mfps` frozen from some earlier,
atypical 1 s window (e.g. right after `StartRecvPic`/a forced-recovery cycle,
or right before `StopRecvPic`), not a live measurement of a continuously
running 25 fps stream — because `hub_get_fps()` never expires it, unlike
`hub_get_bitrate()`.

**Quick, no-code-needed check:** what did the *bitrate* half of the OSD
(`{fps}/{bitrate}`) show at the same moment? If it was `0`, that alone
confirms channel 0 was idle (`hub_get_bitrate()`'s 2 s staleness check would
have already zeroed it) while `{fps}` kept showing a leftover number — direct
proof of this exact asymmetry, no further hardware access needed.

**Still to check on hardware (cam-kinder-rechts) if the bitrate reading above
isn't already conclusive:**

- Whether `video0` currently has any subscriber (RTSP/HTTP client open on the
  main stream specifically, not `video1`) at the moment `{fps}` reads low.
  `GET /control`'s `"encoder"` block (`hal_enc_stats`, keyed by stream index)
  should show channel 0 present/absent and its `work_done`/`cur_packs`
  advancing or static across two polls a few seconds apart.
- Actively open a viewer on `video0` (not a substream) for 10+ s and watch
  whether `{fps}` climbs toward ~25 while watched, then re-check a few
  seconds after closing the viewer to see if it freezes again at whatever
  the last value was — that would be the clean live confirmation of the
  "frozen on idle" theory.

**Proposed minimal fix (APPLIED 2026-08-22, see the status note at the top of
this entry):** give `hub_get_fps()` the same staleness guard
`hub_get_bitrate()` already has, i.e. in `src/hub.c`:

```c
double hub_get_fps(int src)
{
    hub_source *s = hub_get(src); if(!s) return 0.0;
    double fps;
    pthread_mutex_lock(&s->lock);
    fps = (ms_now_us() - s->fwin < 2000000) ? s->mfps : 0.0;
    pthread_mutex_unlock(&s->lock);
    return fps;
}
```

This would make an idle channel show `{fps}` = 0 (matching `{bitrate}` = 0)
instead of a misleading frozen number, but it does **not** by itself explain
why the *live* number, if `video0` really is continuously watched, would sit
at 13 instead of ~25 — that branch of the investigation still needs the
hardware check above before touching any code. `hub.c`/`hub.h` are currently
being live-edited by another agent for the rate-control work, so this fix (if
confirmed) should land after that lands, to avoid a merge fight.

### Root-cause investigation in the buildroot tree (2026-08-21) — ISP clock, not sensor

The RTSP frame counts (ffprobe, independent of the daemon) supersede the
stale-OSD theory above for the *magnitude* question: all six cinnado
T31L+sc2336 cameras really deliver ~13.5 fps (135-136 frames/10 s), while
T23N+sc2336 (25.0) and T31X+sc4336p (24.7) deliver full rate. Findings from
`.ciao-wt` (read-only analysis, nothing applied):

**Established from the tree:**

- The sc2336 kernel driver is NOT a per-platform vendor blob: it is built
  from source in the fetched `ingenic-sdk` checkout
  (`output/ciao/<cam>/build/ingenic-sdk-c5cbd61.../3.10.14/sensor-src/{t23,t31}/sc2336.c`).
  The t23 and t31 variants differ only in whitespace/cosmetics; the register
  init tables are byte-identical (single 1080p linear mode, HTS 0x8ca=2250,
  VTS 0x5a0=1440, 81 MHz SCLK, same `SENSOR_VERSION "H20210805a"`). No
  WDR/HDR/DOL mode exists in either driver, so the
  "double-exposure halves the rate" hypothesis is ruled out for this
  firmware.
- IQ tuning bins differ per SoC as expected
  (`/usr/share/sensor/sc2336-t23.bin` 176288 B md5 aecf0163…,
  `sc2336-t31.bin` 159736 B md5 68388420…) but IQ bins carry ISP tuning
  (AE/AWB/CCM), not the sensor readout mode — the readout mode comes solely
  from the (identical) driver init table.
- **The defconfigs differ far more than previously noted** (the earlier
  "only shvflip" comparison was wrong). The decisive difference is the ISP
  core clock passed to the tx-isp module at load
  (`target/etc/modules.d/20-isp`):
  - T23N cinnado: `tx_isp_t23 ... isp_clk=200000000 isp_clka=600000000` -> full rate
  - T31X wuuk: `tx_isp_t31 isp_clk=200000000` -> full rate
  - **T31L cinnado: `tx_isp_t31 isp_ch0_pre_dequeue_time=24 ... ` with NO
    `isp_clk` -> driver default `isp_clk = 100000000` (100 MHz,
    `3.10.14/isp/t31/tx-isp-debug.c:11`; the ISP core itself is the vendor
    archive `libt31-firmware-540.a`, ISP_FW_VER 1.1.6, and takes its clock
    from this shim).**
- Sibling T31L+sc2336 profiles in `configs/cameras/` DO raise the clock:
  gncc_gc2 220 MHz, litokam_c1 220 MHz, hugolog_e5 220 MHz (with a comment
  block showing the stock vendor parameters `isp_clk=220000000`,
  `pre_dequeue_time=20`, `valid_lines=540`), galayou_y4_atbm6032 150 MHz,
  wansview_g6 125 MHz. `cinnado_d1_t31l_sc2336_atbm6031` and
  `wansview_w7_t31l` are the odd ones out with no `BR2_ISP_CLK_*` at all.
- timps sets the sensor to its proc-reported max (30 fps,
  `src/config.c:1747`, `IMP_ISP_Tuning_SetSensorFPS(30,1)` in
  `src/hal/hal_ingenic.c:895`) and the encoder to FRC 30 -> 25 (matches the
  logged `inFrmRate.frmRateNum=30 ... setFrmRate.frmRateNum=25`). If the
  100 MHz ISP can only push ~16.2 fps of 1080p, the encoder's 25/30 drop
  pattern turns that into exactly 16.2 * 5/6 = 13.5 fps — the measured
  number. (This arithmetic fits, but the 16.2 figure itself is inference,
  not yet measured.)

**Remains conjecture until measured on hardware:**

- That the ISP really delivers ~16.2 fps at 100 MHz (vs. dropping half).
- Whether `isp_ch0_pre_dequeue_time=24` contributes (the working 220 MHz
  profiles use 20 + `valid_lines=540`); `/proc/jz/isp/isp-fs` has a
  dedicated `ch0_pre_dequeue_drop` counter to check this.
- The single `sc2336 stream on` without `stream off` on T31 could not be
  decided from the tree: both platforms run the same timps on-demand logic,
  so it is either a dmesg ring-buffer artifact or a difference in viewer
  pattern (e.g. a continuously-pulling client on that camera), not a
  platform mechanism found in the source.

**Discriminating measurements (on one affected camera, read-only):**

1. ISP delivery rate before the encoder: snapshot
   `grep -E 'buf_qcnt|losted|pre_dequeue' /proc/jz/isp/isp-fs`, wait 10 s,
   snapshot again; delta(buf_qcnt)/10 = true ISP fps. ~16 confirms the
   clock theory; ~30 moves the blame past the ISP.
2. `grep -iE 'fps|wdr' /proc/jz/isp/isp-m0` — reports "ISP OUTPUT FPS" and
   WDR mode from the vendor core.
3. Live sensor timing over i2c (safe, read-only, bus owned by kernel hence
   `-f`): `i2ctransfer -f -y 0 w2@0x30 0x32 0x0c r2` (HTS, expect 08 ca)
   and `i2ctransfer -f -y 0 w2@0x30 0x32 0x0e r2` (VTS: 04 b0 = 30 fps,
   05 a0 = 25 fps; ~0a 6a would mean the sensor itself is slowed — not
   expected). Do NOT use `sinfo dump` for this: it pulses the sensor reset
   line and would kill the running stream.
4. The decisive experiment (one test camera, reversible):
   `sed -i 's/^tx_isp_t31 /tx_isp_t31 isp_clk=200000000 /' /etc/modules.d/20-isp`
   + reboot, then re-run the ffprobe count. If it lands at ~25, the tree fix
   is one line in
   `configs/cameras/cinnado_d1_t31l_sc2336_atbm6031/..._defconfig`:
   add `BR2_ISP_CLK_220MHZ=y` (parity with the other T31L+sc2336 profiles;
   200 MHz also plausible) — deliberately NOT applied, per-fleet decision
   pending.

## RESOLVED: `{bitrateN}` placeholder was missing (per-channel bitrate OSD text)

**Implemented 2026-08-22** exactly as sketched below - the branch went into
`resolve()` right after the plain `{bitrate}` one, mirroring the `{fps}` ->
`{fpsN}` order rather than being wedged in before it: `strcmp("bitrate9",
"bitrate")` never matches, so there was no shadowing to avoid and the
mirrored order reads better. No `hub.c`/`hub.h` change was needed, as
predicted. Placeholder lists updated in `src/hal/osd_vars.h`,
`docs/wiki/Configuration-Reference.md` (two lists, both of which were also
missing plain `{bitrate}`), `timps.conf.example` and `README.md`.

Verified on the host, not on a camera: `osd_vars.c` is not in the sim's source
list, so it was exercised through a throwaway harness linking the real
`osd_vars.c` + `hub.c` and publishing two channels at different frame sizes.
`{fps0}/{fps1}` = 31.0/16.0 and `{bitrate0}/{bitrate1}` = 2478/256 - per
channel, distinct, and `{bitrate}` kept reading the `osd.monitor_stream`
global. `{bitrate9}` (>= MS_MAX_VSTREAM) renders 0 and `{bitrateX}` falls
through to the vars-file lookup, both as intended. The OSD-text smoke test on
a real camera below is still worth doing but nothing about it is in doubt.

### Original entry

Investigated 2026-08-21, read-only against `src/hal/osd_vars.c`, `src/hub.c`,
`src/hub.h` and `git log`/`git show` on the relevant commits.

**Established:**

- `{fps}`/`{bitrate}` (no number) both read `osd.monitor_stream` (default
  channel 0) via two globals `g_fps`/`g_bitrate` in `osd_vars.c`, filled once
  a second from the OSD updater threads — same value burned into every OSD
  layer's text regardless of which stream that layer belongs to (confirmed:
  `osd0.1.text` and `osd1.1.text` both showing `{fps}/{bitrate}` render
  identically).
- `{fpsN}` (`osd_vars.c:180-183`, commit `a94379b`, 2026-08-01) is a genuine
  per-channel diagnostic: `hub_get_fps(ch)` already takes the channel/hub
  source as an argument, so wiring it per-N was a ~10-line, purely additive
  change reusing the existing rolling window — no changes to `hub.c`/`hub.h`
  were needed.
- **`hub_get_bitrate(int src)` (`src/hub.c`, added earlier the same week in
  `eda8302`, 2026-07-28) is *already* per-channel** — it takes a hub source
  exactly like `hub_get_fps()` does. There is no technical blocker: nothing
  about the bitrate measurement is global-only or unsplittable per channel.
- The `a94379b` commit message only talks about fps ("cheap: reuses the
  existing `hub_get_fps()` rolling window") and never mentions bitrate. There
  is no commit, comment, or design note anywhere arguing `{bitrateN}` was
  considered and deliberately left out. This reads as a **plain gap/oversight
  in the `{fpsN}` commit** (added the per-channel fps placeholder, forgot the
  bitrate counterpart that the code already supports), not an intentional
  "no-number = main-stream alias" design.
- `{fps}`/`{bitrate}` (no number) themselves are not a deliberate "primary
  stream" alias either as far as the history shows — `{fps}` predates
  multi-stream OSD support in this file and `{bitrate}` was bolted on next to
  it later at `osd.monitor_stream` for symmetry, before `{fpsN}` introduced
  the real per-channel idea. So the no-number forms look like a leftover
  default rather than a designed alias, but per-channel OSD text (osd0 vs
  osd1) existed before `{fpsN}`/`{bitrateN}` did, which is exactly the
  scenario that motivated question 1 above (osd1 wants osd1's own numbers,
  not channel 0's).

**Proposed minimal fix (not applied — sketch only), in
`src/hal/osd_vars.c`'s `resolve()`, mirroring the existing `{fpsN}` branch
one-to-one:**

```c
/* {bitrateN}: measured encoder OUTPUT bitrate of video stream N specifically,
 * independent of osd.monitor_stream. Mirrors {fpsN}. */
else if (!strncmp(name,"bitrate",7) && name[7]>='0' && name[7]<='9' && name[8]==0){
    int ch = name[7]-'0';
    snprintf(out,outsz,"%.0f", ch<MS_MAX_VSTREAM ? hub_get_bitrate(ch) : 0.0);
}
```

placed next to the existing `{fpsN}` branch (before the plain `{bitrate}`
check so it doesn't get shadowed — same ordering issue does not actually
arise here since `strcmp` vs `strncmp` don't collide, but keep it adjacent to
`{fpsN}` for readability). No `hub.c`/`hub.h` change needed —
`hub_get_bitrate()` already does the right thing per channel. Update the
placeholder list comment in `src/hal/osd_vars.h` alongside it
(`{hostname} {ip} {mac} {fps} {fpsN} {bitrate} {uptime}` → add `{bitrateN}`).

Not applied: `hub.c`/`hub.h` are currently being live-edited by another agent;
`osd_vars.c`/`osd_vars.h` are not touched by that work, so this one could
actually land independently once reviewed — flagging it here rather than
doing it opportunistically since the task was analysis-only.

**Nothing to verify on hardware for this one** — it is a straightforward,
low-risk additive placeholder mirroring code that already works
(`{fpsN}`/`hub_get_bitrate()` both already proven in production), but it
hasn't been written yet so it should still get the normal OSD-text smoke test
after implementation (POST an `osd*.*.text` containing `{bitrateN}` for both
streams, confirm the rendered text differs and matches each stream's own
`GET /control` `encoder.N` figures).

## Follow-ups from the 2026-08-21 rate-control implementation

### RESOLVED: `video<N>.qp` was graded "applied live" under fixqp and is not (T31)

**Fixed 2026-08-22 by fix candidate 1**: `qp` is out of `ENC_LIVE_KEYS` for
T31/C100 **and T40** (T40 shares the `ENC_HAS_SETRCMODE` branch and is graded
the same way rather than optimistically, still without a measurement of its
own). The POST reply now defers `video<N>.qp` on every new-API SoC, which is
what was true all along. `caps.video_live` on T31/C100 is
`bitrate,min_qp,max_qp,i_bias_lvl`; T40 and T41 now carry the identical set,
so `timps-qa.sh`'s RC1 set-shape table collapsed to three cases and reports
`T40/T41`. RC6/RC6b self-gate on `rc_live_has qp` and are inert on the new
API from here; the classic path is unchanged. The dead `qp` branch in
`rc_live_apply` is kept and marked unreachable, since it is the record of what
`SetChnAttrRcMode` actually does. Verified: `make sim` clean, T31 and T23
cross-syntax clean, macro expansion checked per platform. **Not re-measured on
hardware** - the change only removes a claim, so there is nothing new for the
bitstream to confirm.

**Correction to candidate 2 below: it is NOT a dead end.**
`IMP_Encoder_SetChnQp(int encChn, int iQP)` is declared in
`include/T31/1.1.5{,.2}/en`, `include/T31/1.1.6/en` (the set the T31 build
uses) and `include/C100/2.1.0/en`, documented as *"Setting the rate control
property QP dynamically ... will reset the QP of the next frame, and the set
QP will take effect in the next frame"*, H264 and H265, channel must exist.
Not present in T40/T41 1.2.0/zh, nor in T31 1.1.1/1.1.2. Deliberately NOT
wired: header presence is not proof libimp exports it, and re-advertising `qp`
live on the strength of a doc comment would recreate the same broken promise
this entry is about. To land it: call it in `rc_live_apply`'s `qp` branch,
re-add `qp` to `ENC_LIVE_KEYS` for T31/C100 only, and re-run the RC6b
bitstream measurement on cam-garage - if the two QP values do not span like
the boot path does, revert and leave the key restart-bound.

**2026-08-22, classic path (T23N) closed too**: "the classic path is
unchanged" above turned out to be the remaining gap. RC6 (which pins the
same deferred-outside-fixqp/live-inside-fixqp contract already enforced on
the new API) FAILed on cam-vorne: `video1.qp` posted under `rc_mode=cbr`
graded `deferred:0` even though classic `rc_live_apply` ignores `k` entirely
and just re-fills the whole `IMPEncoderAttrRcMode` union from `g_cfg` -
`SetChnAttrRcMode` succeeds regardless of which field the caller cared about,
so a fixqp-only field reported "live" while doing nothing observable. Fixed
in `rc_live_apply`'s classic branch: `qp` now returns 0 (persisted, applies
on restart) when `v->rc_mode!=MS_RC_FIXQP`, before the union re-fill. Verified
on hardware (cam-vorne, T23N): RC6 now reads "correctly DEFERRED under
rc_mode=cbr". Also fixes T20X (wyze/wyze-pan), which shares the classic
branch. This narrows the "grading is honest per call, not per effect" note
below - that acceptance no longer covers `qp` on classic, only the other
keys it names (`quality_lvl`, `bitrate` under a non-native mode).

The original report follows.

#### Original report (2026-08-22)

**Measured on cam-garage (T31X/sc4336p) 2026-08-22, substream in `fixqp`,
daemon pid pinned across the whole sequence.** Found by the new `timps-qa.sh`
RC6b check, which is why that check exists.

The live path is inert while claiming success:

| route | `qp` | delivered | mean keyframe |
| --- | --- | --- | --- |
| boot (`enc_create`) | 25 | 108 kbit/s | 19624 B |
| boot (`enc_create`) | 42 | 17 kbit/s | 2161 B |
| live POST | 25 | 40 kbit/s | 7793 B |
| live POST | 42 | 40 kbit/s | 7801 B |
| live POST 42 → 25, straight after a boot at 42 | 25 | 21 kbit/s | 3742 B |

The boot path spans 6.4× across the same QP pair, so the encoder and the scene
are both perfectly capable of expressing it. The live path does not move at
all — and from a fresh boot at 42, a live POST of 25 leaves the stream where
42 put it, not where a boot at 25 would.

Meanwhile the POST reply says `deferred:0` and `encoder.1.rc.qp` echoes the
new value exactly. So `IMP_Encoder_SetChnAttrRcMode` stores `attrFixQp.
iInitialQP` where the next `GetChnAttrRcMode` reads it back, and never
re-programs the running channel. `rc_live_apply`'s RMW (hal_ingenic.c, the
`ENC_HAS_SETRCMODE` `qp` branch) is doing exactly what it says; the SDK call is
not.

This is the "accepted, persisted, faithfully echoed, and silently ignored"
class again (340fb1f, ff28ee2, f003655, 0a8bb9f, 6ec766e, dd2221f, 51bf052,
30ecc74) — with the twist that the readback is complicit, so only a bitstream
measurement can see it. It is also a broken promise in the API contract:
`deferred:0` means "reached the running pipeline", and a UI acting on it tells
the user no restart is needed when one is.

**Fix candidates**, in order of honesty:
1. Drop `qp` from `ENC_LIVE_KEYS` for T31/C100 (and check T40, same call) in
   `src/enc_caps.h`, so it is graded restart-bound and the reply stops lying.
   Costs nothing real: nobody can use the live path today anyway.
2. Find a call that does re-program a running fixqp channel. Nothing obvious in
   the T31 headers; `SetChnAttrRcMode` is the only rc-attr setter.
   *(Wrong - `IMP_Encoder_SetChnQp` exists on T31/C100; see the correction at
   the top of this entry.)*
Option 1 unless 2 turns something up. **Done 2026-08-22 (option 1).**

**Not reproduced elsewhere yet**: T40 uses the same `ENC_HAS_SETRCMODE` branch
and is untested; T41 has no setter at all so it is already restart-bound;
classic (T10–T30) goes through the full-union re-fill, a different code path.
Note the same shape was measured for `i_bias_lvl` on this SoC (accepted,
echoed via `ip_delta`, no bitstream effect) — see the entry under "Hardware
verification". Two of T31's five `caps.video_live` keys are therefore inert.
The other three (`bitrate`, `min_qp`, `max_qp`) were measured to genuinely
reach the encoder in the same session.


### Wiki pages are slightly stale after the implementation

`docs/wiki/Rate-Control-Parameters.md` still says "the five still-hardcoded
classic-path fields" — `fluc_lvl` is a config key now (0..4, H265 only), and
live application + the `encoder.<n>.rc` readback + `caps.video_live` /
`deferred_keys` exist. Deliberately NOT edited alongside the implementation to
avoid clobbering the parallel wiki work; needs one correction pass once the
hardware verification has confirmed the behaviour worth documenting.

### frmQPStep / gopQPStep / gopRelation / staticTime stay hardcoded

Confirmed during implementation, not just carried over: no vendored header
documents a range for any of them, and nothing new was learned that would
justify picking bounds. Revisit only with a documented domain or a
measurement. (`gopRelation` is a bool and COULD be exposed trivially, but a
knob with unknown semantics and no measurement is still a trap.)

### Adversarial review of the rate-control rework (2026-08-21 audit)

Scope: `c4e434f..c5d2de5` + the WebUI commits, checked against the vendored
SDK headers, the simulator and cross-builds. Confirmed findings, smallest
first; comment-level corrections were applied in the same pass.

- **T21/T30 headers allow `SetChnAttrRcMode` for H265.** Only T23 (en+zh)
  says "H264 only"; T21/T30 say "H264 and H265". The live path refuses every
  classic H265 stream — over-conservative on T21/T30, correct on T23.
  Comments in `enc_caps.h`/`hal_ingenic.c` corrected to say so; the refusal
  itself stays until hardware confirms (no T21/T30 H265 stream in the fleet).
- **`quality_lvl` range/semantics flip under smart.** Classic headers: VBR
  qualityLvl 0..7, LOWER = better; Smart qualityLvl 0..6, HIGHER = better,
  default 4. Config clamps a static 0..7 and the WebUI tooltip states the
  VBR reading. Under `rc_mode=smart` a posted 7 is out of the documented
  domain and the tooltip's direction is wrong. Fix would be mode-dependent
  clamping + tooltip split — behavior change, deliberately not applied here.
- **Grading is honest per call, not per effect.** Classic: a live write to a
  key absent from the active mode's union member (`quality_lvl` under cbr,
  `bitrate` under fixqp) reports "applied live" because the whole-union call
  succeeds. Accepted as-is for these - `qp` used to be in this bucket too but
  was pulled out and gated (2026-08-22, see the RESOLVED entry above): it is
  the one key whose live-vs-restart claim was actually being asserted by RC6
  and hardware-measured, not just accepted as an architectural limit.
- **`min_qp`/`max_qp` have no cross-field clamp** (SDK: minQp range
  [0..maxQp]). min_qp=50+max_qp=20 passes config and reaches the SDK, which
  then rejects (graded honestly at runtime). Pre-existing, not from this
  rework.
- **`SetChnBitRate` stores raw values** (T31 libimp disassembly: args land
  unconverted in the channel state; for CBR the lib forces max:=target).
  Consistent with the header's "bit/s" and the HAL's kbps*1000. Whether
  `SetDefaultParam`'s `uTargetBitRate` uses the SAME unit is not decidable
  statically — see the hardware check below.
- **Readback vs. live-apply concurrency inside libimp.** GET `/control`
  (`hal_enc_rc_read` -> `GetChnAttrRcMode`) takes no lock against a POST
  worker's `SetChnAttrRcMode`/`SetChnBitRate` (`apply_mu` covers only
  POST-vs-POST). No timps data race (g_cfg writes stay under
  `config_str_lock`, `rc_live_apply` reads on the same thread), but Get-vs-Set
  concurrently inside libimp is not a vendor-sample pattern. Worst plausible
  case: a torn union in the readback. If hardware shows inconsistent
  `encoder.<n>.rc` values during writes, serialize the two with a small
  mutex.
- **GET `/control` reads the videoN ints lock-free** while POST writes them
  under `config_str_lock` — formally the C11 race the config.h rule exists
  for. Pre-existing for every videoN int; the live keys just change more
  often now.
- **Verified clean:** sim contract exact (`deferred_keys` = changed-and-not-
  live keys, clamped values in `applied`, unchanged re-posts deferred:0,
  per-channel key names); `fluc_lvl` present in BOTH status-JSON branches
  (`!USE_ROTATE` verified live on the sim); single response builder in
  httpd.c; `enc_caps.h` matches the SDK call surface on all five platforms
  (T41: no rc-mode setter, T40: no QpIPDelta — both confirmed in the
  headers); new-API struct/field names, int16 casts and
  `attrCappedQuality`=typedef-of-`attrCappedVbr` all match; classic fill
  field-for-field correct incl. H264/H265 CBR layout differences; WebUI
  gating and `deferred_keys` handling consistent with the server (minor: it
  leaves `i_bias_lvl`/`fluc_lvl`/QP bounds enabled under FIXQP where they do
  nothing).
- **Builds:** T23/T31/C100 compile+link clean. T40/T41 compile clean; the
  final link needs the xburst2 toolchain (not on this machine — vendor libs
  are FP64), so run `./build.sh timps T40|T41` where it is available.

## Hardware verification (mine — the agents only have the simulator)

Both agents work without camera access. Everything they build has to be checked
against real hardware afterwards, on both SoC generations:
cam-kinder-links (T23) and cam-kinder-rechts (T31).

What to check:

- **Readback vs. written.** For every rate-control value, compare what
  `/control` reports as configured against what `GetChnAttrRcMode` reports as
  held. This is the whole point of the readback — we have never verified that
  our writes arrive unaltered.
- **Per channel.** ch0 and ch1 must stay independent. Test with genuinely
  different settings running at once (ch0 vbr/2000, ch1 cbr/384 is the natural
  case, it is what the fleet already runs).
- **Live application really is live.** A change takes effect with no daemon
  restart, the readback confirms it, and the stream does not break. Then the
  inverse: on T41 there is no setter at all, so anything claiming to be live
  there is a bug — untestable on our fleet, we have no T41.
- **The new static keys** parse from timps.conf, persist, and survive a
  restart with the same values.
- **Both warnings fire where they should**: the `smart` -> `capped_quality`
  substitution on the T31, and the reworded new-API warning. And, just as
  important, that they do *not* fire on the T23.
- **`i_bias_lvl` on T31** via `SetChnQpIPDelta` — **measured 2026-08-22 on
  cam-garage (T31X/sc4336p), and the answer is that the call does nothing to
  the bitstream.** Transfer: settled and correct. `encoder.<n>.rc.ip_delta`
  echoes the posted value 1:1, in scale and sign, live and after a restart —
  the 443584e pass-through lands as sent, so nothing to fix there. Semantics:
  no effect. Live path, three interleaved −3/+3 pairs, 15 s captures, stable
  scene at the configured 384 kbps: mean keyframe 26254/26695/26646 B at −3
  against 26687/26701/26700 B at +3 — 1.7 % spread, no direction, delivered
  rate 224/225/226 kbps either way. Boot path (value applied by `enc_create`
  after `RegisterChn`, daemon restarted between halves), two interleaved
  pairs: 13855/20549 B at −3 against 14942/23034 B at +3 — the +3 half larger
  both times, but keyframes drifted 26k → 14k across the session with the
  light, so 8–12 % is inside the scene drift and is not a result. Boot and
  live use the identical SDK call on the identical channel.
  Conclusion: `IMP_Encoder_SetChnQpIPDelta` is accepted, faithfully echoed,
  and inert under `cbr` on this SoC. That is an SDK-side null, not a timps
  bug — which is why the QA verdict is INFO, not FAIL. **Still open:** whether
  it does anything under a non-`cbr` mode (rc_mode is restart-bound on the new
  API, so a `vbr`/`smart` sweep needs a restart per half and was not run), and
  whether C100 — the other SoC whose SDK ships the call — behaves the same.
  The sweep itself is now repeatable as `timps-qa.sh` RC5b under
  `--test-encoder`; point it at any T31/C100 camera to re-check. It measures
  `-3 / +3 / -3` and requires the effect to beat the drift the two `-3` legs
  measure between them — an unbracketed A/B pair run at dusk on cam-garage
  reported a 41.9 % "effect" with the opposite sign, purely from the light
  changing, against 0.1 % from three controlled runs earlier the same day.
  Keyframe size is not stable enough on a real camera to compare across two
  captures without an in-run noise estimate.
- **`bitrate` unit on T31 (new API).** Boot with videoN.bitrate=2000, note
  `encoder.<n>.rc.bitrate`. Then POST the same 2000 live. If the readback
  jumps by x1000, `SetDefaultParam` and `SetChnBitRate` disagree on the
  unit and the boot path underfeeds by 1000 (or the live path overfeeds);
  if it is unchanged, both take bit/s-equivalent storage and the kbps*1000
  conversion is right. Cross-check `ave_bitrate` against a real stream
  measurement.
- **T23 live rc actually applies.** POST `video0.bitrate` (and `min_qp`)
  on cam-kinder-links without restart; `deferred_keys` must be empty for
  them, the readback and the measured stream must follow within one GOP.
- **`flucLvl`** is H265-only and the T23 SDK has no H265 at all, so it cannot
  be tested on the T23. Check whether any fleet camera can exercise it; if not,
  say so rather than claiming it works.

Currently running: `min_qp` sweep (20 -> 30 -> 38) on cam-kinder-links under
vbr with quality_lvl back at 2, so min_qp is the only variable. Tests whether
the controller is quality-seeking and where its operating qp actually sits.

## Screenshots for the wiki

Needed for the parameter documentation, and they have to be produced
deliberately rather than reused from the technical comparison.

**Motif: to be agreed between the user and me before anything is captured.**
Candidate is the cat-tree crop from the 2026-08-21 frames — anonymous enough,
enough texture and fine structure to show quantisation artefacts. Not decided
yet. The existing children's-room frames are not for publication.

Requirements:

- Frames from the **H.264 stream**, not JPEG snapshots — those come from a
  separate encoder and would show nothing about rate control.
- **Both SoCs**: the same settings on T23 (cam-kinder-links) and T31
  (cam-kinder-rechts), so the reader can see the generational difference and
  not just read about it.
- One frame per setting: cbr baseline, vbr at quality_lvl 2 / 5 / 7, and fixqp
  42 as the "what the scene actually costs" reference. Full frame plus a 1:1
  centre crop each — the crop is where quantisation is visible; the scaled-down
  full frame hides it.
- Capture each SoC's series in one uninterrupted run so lighting is comparable
  within it. Note the measured bitrate next to each image.

The two cameras look at different rooms, so the T23 and T31 series will not
show the same scene. Say that in the caption instead of implying a like-for-like
comparison — the bitrate figures are comparable, the pictures are not.

`docs/wiki/Rate-Control-Parameters.md` already has four labelled placeholders
(T23: cbr baseline, vbr/5, vbr/7, fixqp 42) and a proposed image location
(`docs/wiki/images/<page-slug>/`) waiting for this material — drop the images
in and swap the placeholders for real `![]()` references once the motif is
agreed. The T31 companion set from this section is noted there as a tracked
follow-up, not yet placeholdered.

## Encoder

### Consider unifying videoN.bitrate semantics via SetChnBitRate

The per-SoC meaning (classic ceiling vs new-API target) is now DOCUMENTED
(timps.conf.example, commit 9acfcbe) and both values are observable via the
`encoder.<n>.rc` readback. Deliberately not unified: that would change fielded
new-API behaviour on header-only knowledge. Once the T31 readback shows what
`uMaxBitRate` actually defaults to (and in which unit), decide whether
`IMP_Encoder_SetChnBitRate(chn, target, max)` at bring-up (bit/s!) is worth
it.

### uMaxPSNR / eRcOptions / uMaxPictureSize stay read-only

Decision 2026-08-21: readable via `encoder.<n>.rc` (max_psnr / rc_options /
max_picture_size), no setters. `uMaxPSNR` is the knob `capped_quality` is
named after, but everything known about these fields is header-derived and
the investigation refuted two header-derived hypotheses already. Revisit with
readback data from a real T31.

## Control API

### RESOLVED: report ignored unknown fields in POST responses

Measured against the simulator 2026-08-21: a POST containing only unknown keys
returns 422 `unknown_fields`, but a POST mixing a valid key with an unknown one
returns `ok:true, accepted:1` and drops the unknown key silently.
`{"quality_lvl":7,"quality_level":5}` therefore looks successful.

Add an `ignored:[...]` array without changing what gets applied. Same failure
class as the `min_qp` gap and the missing status-JSON fields.

**Done 2026-08-22.** The apply path could not be extended to do this: it walks
the field TABLE and looks each name up in the body, so a name the tables do not
know is structurally invisible to it (the point control.h already made). So
`ign_scan()` walks the body in the other direction and tests each member
against the SAME `tbl[i].name` + `F_CTRL` rule `apply_ctrl_fields()` accepts on
- one call next to each section's apply call, never a second hand-written name
list, so a field that stops being applied starts being reported in the same
edit. Names are `ms_json_esc`-escaped (they are client data - a POSTed
`"bright\"ness\\"` comes back correctly escaped, verified), and
`ignored_truncated` flags a short list.

Deliberately narrow so what it says is always true: object- and array-valued
members are skipped (`get_val()` refuses those too, so they are subsections,
not fields - this is also what keeps `{"video":{"0":{...}}}`'s index members and
the legacy flat top-level form from being reported), only the scanned object's
own level is walked, and sections with keys handled outside the table
(`daynight.mode`/`probe`, `record.active`/`clip`/`seconds`, `speaker.play`/
`stop`) pass those names as `extra` so they are not falsely reported.
Documented in `docs/wiki/HTTP-Control-API.md`'s reply table, including the
limits: an unknown top-level SECTION, an out-of-range stream/item index and an
object-valued member are not listed, so `ignored:[]` is not a promise that
every name in the body was understood.

Verified against the sim: the TODO's own example now answers
`ignored:["video1.quality_level"]` with `accepted:1` unchanged;
`{"image":{"brightness":140,"brihtness":9}}` names `image.brihtness`; a
fully-known body and the legacy flat form both report `ignored:[]` (no false
positives); `{"daynight":{"mode":"sensor","probe":0,...}}` reports neither;
`{"motion":{"on_motion":"/bin/sh",...}}` reports it, which is correct - it is
present and not applied; 40 unknown keys overflow the buffer and set
`ignored_truncated` with the JSON still valid. `scripts/timps-qa.sh` 8e gained
case 9 for the mixed body and the empty-list negative half; both were run
against the sim and pass.

## Fleet

### Decide the backchannel overlay policy

`audio.backchannel` defaults to 0 (`config.c:295`) and no user overlay sets it,
so every full flash turns two-way audio off. It was only on before because an
earlier runtime setting had persisted. Restored on cam-garage 2026-08-21 and
verified by acoustic loopback at 30.6 dB, but it will be lost at the next
flash.

Making it durable means `audio.backchannel = 1` in the user overlay
`timps.conf` — a privacy-relevant change, so it needs the user's decision:
garage only, whole fleet, or not at all.

### Give the wyze cam2 a real hostname

192.168.10.107 still reports the factory hostname `ing-wyze-cam2-3737` and
appears in Loki under the older, stale `ing-wyze-cam2-8071`. Set a proper
hostname in the user profile so it survives flashing, then confirm the
collector picks up the new label.

### Bring the fleet back to one version

Ten cameras run the v1.9.2 release; cam-kinder-links and cam-kinder-rechts run
`v1.9.2-1-g8f3c84c` with the three new rate-control keys. Fine while the T23
work is in progress, not permanent.

Build recipe, learned the hard way 2026-08-21: `make rebuild-timps` **plus a
separate pack run**, verified by extracting timpsd from the squashfs. A plain
`make` repacks fresh images around the previous binary, and even
`rebuild-timps && make` packs before the install lands. See
`.ciao-wt/docs/ota-rootfs-hang-and-partition-layout.md`.

### Prove the T23 boot repair on cam-vorne

The railed-boot repair (treat a pegged AE reading at boot as a transition, not
a re-assert) has never run on real hardware in the dark. cam-vorne is a T23 and
the natural candidate. Reboot after nightfall and check it does not anchor its
reference to a clipped reading — `dn_clipped()` should suppress the anchor and
the daynight log should show the deferred verdict.

## Done

- **Live + static rate control, incl. WebUI** — implemented 2026-08-21
  (timps commits c4e434f..9acfcbe, firmware webui commit 4397ddc7b). In
  order: (1) `encoder.<n>.rc` readback via `IMP_Encoder_GetChnAttrRcMode`,
  per channel, separate from the configured block — including the previously
  unreadable new-API attrs (uMaxBitRate/iIPDelta/iPBDelta/eRcOptions/
  uMaxPictureSize/uMaxPSNR). (2) `videoN.fluc_lvl` (0..4, H265 only; the
  other four literals stay hardcoded, no documented ranges). (3) smart ->
  capped_quality warns once; the 8f3c84c warning split into "no equivalent
  field" vs "SDK lacks SetChnQpIPDelta"; `i_bias_lvl` wired on T31/C100 at
  bring-up. (4) live apply per channel: classic = whole-union
  `SetChnAttrRcMode` re-fill (H264 only, incl. live rc_mode switch), T31/
  C100 = SetChnBitRate + SetChnQpBounds + SetChnQpIPDelta + fixqp-qp RMW,
  T40 same minus i_bias, T41 bitrate + QP bounds only. `hub_control()` now
  reports live vs persisted; `caps.video_live` advertises the platform set;
  POST replies carry `deferred`/`deferred_keys` (runtime truth). (5) WebUI:
  rc fields on both stream pages, gated per mode/codec/SoC with the reason
  in the tooltip, per-save live/restart toast from `deferred_keys`, and an
  "Encoder holds" readback line. Verified on the simulator (grading, clamps,
  per-channel isolation, browser test of both pages); T23/T23-swrot/T31
  cross-builds clean, T40/T41 compile clean (no fp64 toolchain to link).
  **All IMP runtime behaviour still needs the hardware pass** — see
  "Hardware verification" above.
- **Smart qualityLvl semantics** — settled by measurement 2026-08-21. Under
  `smart`, quality_lvl 2 gives 1745 kbit/s and 7 gives 1265, same direction as
  vbr. The en T23 Smart text claiming 0..6 with the opposite direction is a
  translation error. Also settled that `smart` brings no scene adaptation on
  T23: within-level spread 0.2-0.5% against 46.7% at a fixed qp.
- **Wiki: why T23 needs far more bandwidth than T31** — written 2026-08-21 as
  `docs/wiki/Rate-Control-Bandwidth.md`, linked from `Home.md`. Carries the
  within-level-spread argument, the practical `quality_lvl` mitigation, and
  both refuted hypotheses.
- **Wiki: document the rate-control parameters** — written 2026-08-21 as
  `docs/wiki/Rate-Control-Parameters.md`, linked from `Home.md`. Every field
  (`bitrate`, `rc_mode`, `qp`, `min_qp`, `max_qp`, `quality_lvl`, `change_pos`,
  `i_bias_lvl`, plus the five still-hardcoded classic-path fields) with range,
  per-SoC support, and a measured-vs-header-derived marker. Images are not
  in yet — the page has four labelled placeholders and a proposed
  `docs/wiki/images/<page-slug>/` convention, both waiting on the motif
  decision tracked under "Screenshots for the wiki" above.

## Follow-up: ISP module parameters ruled out entirely (2026-08-21, live test on cam-kinder-rechts)

Two reboot tests, both reverted afterward:

    baseline                                        : 203 frames/15s (13.53 fps), drop ratio ~45%
    isp_clk=200000000 added                         : 203 frames/15s (13.53 fps), drop ratio ~45%
    isp_clk=220000000 + valid_lines=540 + time=20   : 203 frames/15s (13.53 fps), drop ratio ~47.5%
    (last row matches the stock-firmware reference values from hugolog_e5's defconfig comment)

Zero effect on fps or on `ch0_pre_dequeue_drop` from any combination. This
rules out the missing-isp_clk defconfig gap as the cause - it is a real
deviation from every sibling T31L+sc2336 profile, but not causally linked to
the 13.5 fps ceiling. `ISP OUTPUT FPS: 30/1` in `/proc/jz/isp/isp-m0` stayed
unchanged throughout, and WDR stayed Disabled.

Do not re-test isp_clk/pre_dequeue_time/valid_lines - closed.

Still open: what actually drops ~45-47% of frames at ch0_pre_dequeue,
independent of every ISP module parameter tried. `buf:0` and `buf:1` in
`/proc/jz/isp/isp-fs` always report identical counts, consistent with both
video streams sharing one upstream frame source - the drop happens before
that split, not per-encoder-channel. Next avenue: compare this against
cam-garage (T31 + sc4336p, full rate) at the same procfs level to see if the
working pairing shows near-zero drops there, which would confirm the drop
counter itself is the right diagnostic and shift the search to what's
upstream of it (sensor readout timing, i2c bus contention, ISP pipeline
config unrelated to these three module parameters).

## Follow-up 2: sensor itself is not throttled (i2c VTS/HTS readback, 2026-08-21)

Live register read on cam-kinder-rechts, `i2ctransfer -f -y 0 w2@0x30 0x32 0x0e r2`
(VTS) and `... 0x32 0x0c r2` (HTS):

    VTS = 0x04b0 = 1200   (init-table default was 1440 - AE lowered it, normal)
    HTS = 0x08ca = 2250   (matches the init table exactly)

    fps = SCLK / (HTS * VTS) = 81,000,000 / (2250 * 1200) = 30.0 fps

The sensor is genuinely running at 30 fps, full stop - not a reduced mode, not
a longer line time. Combined with the ISP module parameter tests above (all
negative) and `ISP OUTPUT FPS: 30/1` from `isp-m0`, the ~45-47% loss is fully
downstream of both the sensor and the ISP core's own frame production - it
happens specifically at whatever `ch0_pre_dequeue` gates, between ISP output
and encoder consumption. Neither isp_clk, pre_dequeue_time, nor valid_lines
touch it.

This is very likely a kernel/ISP-driver-level bug in the tx-isp t31 binding
for this sensor mode, not something fixable via Buildroot Kconfig. Next step
(not yet done): read the same `ch0_pre_dequeue_drop`/`buf_qcnt` counters on
cam-garage (T31 + sc4336p, confirmed full rate) to see whether that pairing
shows near-zero drops at the same procfs location - if so, the counter is the
right diagnostic and the search moves to what differs in the two sensors'
timing/interrupt behavior at the driver level, which likely requires reading
tx-isp kernel source (SDK checkout, not just the sensor driver) rather than
further Buildroot config changes.

## Incident: video<N>.buffers=3 crashed cam-kinder-rechts channel 0 (2026-08-21)

Testing whether a deeper video buffer pool (nrVBs) would reduce the
`ch0_pre_dequeue_drop` losses from the fps investigation above. Set
`video0.buffers=3` and `video1.buffers=3` via `/control` on cam-kinder-rechts,
restarted the daemon.

**Result: channel 0 (main, 1920x1080) could not initialize at all.**
`framesource 0: EnableChn failed` repeatedly, 5 forced recovery cycles never
produced a frame, timpsd exited cleanly ("camera needs a manual/scheduled
restart"). Channel 1 (sub, 640x360) was unaffected throughout - this is
specific to the larger/main channel, not `buffers` in general.

**Recovery took three attempts, because the first "fix" reintroduced the same
class of bug.** Setting `video0.buffers` back to `2` via a direct
`timps.conf` edit left the literal line `video0.buffers = 2` in the file.
`buffers_explicit` (config.h) is set whenever the key is *present* in the
file, regardless of value - it tells the HAL "trust this value as-is instead
of applying your own safety clamp (e.g. T31 non-scaled channel)". With that
clamp suppressed, channel 0 kept failing to enable on every subsequent boot
(`[chn0] does not support user memory` in the raw vendor log), including
across two full reboots, until the `video0.buffers`/`video1.buffers` lines
were removed from `timps.conf` **entirely** rather than set to `2`. After
that, channel 0 initialized normally on the next daemon start and fps
returned to the pre-incident baseline (202 frames/15s, matching the earlier
203 frames/15s within noise).

**Takeaway for anyone touching `buffers` on a T31 main channel**: writing the
default value back is not equivalent to leaving the key unset - the presence
of the key changes HAL behavior independent of the value. `buffers_explicit`
existing at all is a code smell for this exact trap; consider either dropping
the "trust it as-is" override for the known-unsafe combination, or having the
HAL log a warning when an explicit `buffers` value would suppress a clamp
that's about to matter, instead of failing silently until EnableChn rejects
it.

Camera fully recovered, config restored to the original unset state, no
buffers experiment left in place. Not revisiting the buffers>2 idea for the
fps investigation without also checking the T31 channel-0 clamp logic first.

## Likely real cause of the T31+sc2336 fps ceiling: single-buffer schedule (2026-08-21, from the buffers=3 incident + prudynt-t comparison)

The `buffers=3` crash above led straight to the probable root cause of the
whole fps investigation, already documented in timps' own source
(`hal_ingenic.c`, PLATFORM_T31 nrVBs clamp, "Confirmed on a Cinnado D1
T31L/SC2336 board 2026-07-26" - the exact board family in this fleet):

cam-kinder-rechts' video0 is 1920x1080 against a 1920x1080 sensor, so
`scale = (sw!=width)||(sh!=height)` evaluates false - the "non-scaled full-res
physical channel". The code silently clamps `nrVBs` to **1** for this case
whenever `buffers_explicit` is unset (the normal state, untouched before
tonight). That means the entire fps investigation above (203 frames/15s
baseline, the ~45-47% `ch0_pre_dequeue_drop` ratio, all the ISP-clock/
pre_dequeue-parameter tests) ran against a single-buffer framesource
schedule the whole time - not the nominal 2-buffer default assumed at the
start of the investigation. A single buffer gives the ISP nowhere to put the
next frame if the encoder falls even slightly behind, which is a far more
direct explanation for the drop ratio than anything tested so far.

**Cross-check against `prudynt-t`** (`/mnt/NVMe/git/prudynt-t/src/IMPFramesource.cpp`):
independently hit the same wall. A commented-out block there implements the
same conditional (scale only if resolution differs) with the note "That's a
great idea but it does not work as intended. Needs more investigation" -
followed by unconditionally forcing `scale = 1` regardless of whether
dimensions match. That routes every channel through the scaled multi-buffer
ring path, sidestepping the non-scaled single-buffer constraint entirely.

**Why the buffers=3 test didn't just get "clamped and still work"**: setting
`buffers_explicit` (any explicit `videoN.buffers` line, any value >1) makes
the HAL trust the value instead of applying the nrVBs=1 clamp - and the
underlying kernel driver genuinely rejects nrVBs>1 in this channel's
allocation mode ("one buffer schedule only support nrvbs = 1", visible only
in dmesg, not in any userspace-visible error - EnableChn just fails and
PollingStream spins at rc=-1 forever). There is no buffers value >1 that
works on this channel today; the constraint is in the kernel driver, not a
number to tune around.

**Proposed fix, not applied**: force `scale=1` unconditionally for
`PLATFORM_T31` non-scaled full-res channels (matching prudynt's
already-proven workaround), so the channel gets a real multi-buffer ring
instead of the single-buffer schedule. This is a source change in
`hal_ingenic.c`'s scale-decision logic (around line 960), needs a rebuild and
a flash to test - not something to try live via `/control` again tonight
given the outage above. Verification would be the same independent
`ffprobe -count_frames` measurement plus the `ch0_pre_dequeue_drop` ratio
from `/proc/jz/isp/isp-fs`, both already used throughout tonight's
investigation.

This supersedes the "next avenue" note in the ISP-module-parameters
follow-up above (comparing cam-garage's drop counters) - cam-garage's main
channel is presumably in the scaled path already (different sensor/board),
so a cross-camera procfs comparison is less informative than this scale=1
hypothesis, which is now the highest-priority thing to try.

### RESOLVED at the driver level: the gate is `isp_ch0_pre_dequeue_time`, not the scaler (2026-08-21, disassembly of tx-isp-t31.o)

The scale=1 hypothesis above is **wrong**, and adopting prudynt's workaround
would have taken channel 0 down fleet-wide. Established by disassembling the
exact blob this fleet runs
(`thingino output/piuma/cinnado_d1_.../build/ingenic-sdk-7b4b0f.../tx-isp-t31.o`,
function `frame_channel_unlocked_ioctl`, REQBUFS handler; string `$LC53`):

    REQBUFS on /dev/framechanN fails with "one buffer schedule only support
    nrvbs = 1" if and only if:
        isp_ch0_pre_dequeue_time > 0     (module parameter, default 0)
        && channel index == 0            (framechan0 only)
        && requested buffer count >= 2

Crop/scaler configuration is **not part of the condition**. The requested
count (from `libimp.so` `IMP_FrameSource_EnableChn`, same disassembly
session) is `attr.nrVBs + FrameDepth`; the check runs at EnableChn time
(REQBUFS), and EnableChn **does** return -1 on failure - the old comment's
"no error surfaced above the kernel log" was wrong, the incident log's
"framesource 0: EnableChn failed" was exactly this.

Why the fleet splits the way it does (`/etc/modules.d/20-isp` per board):

    cinnado d1 (all six sc2336 cams):
        tx_isp_t31 isp_ch0_pre_dequeue_time=24 isp_ch0_pre_dequeue_interrupt_process=0 isp_memopt=1 print_level=1
        (from BR2_ISP_CH0_PRE_DEQUEUE_TIME_VALUE=24 in the board defconfig)
    wuuk y0510 (cam-garage, full 24.7 fps):
        tx_isp_t31 isp_clk=200000000 print_level=1
        (no pre-dequeue parameter -> gate never active)

So the cam-garage cross-check is answered, just one level deeper than the
question was posed: whether its video0 is scaled or not is irrelevant -
its driver is simply loaded without the pre-dequeue parameter. Further
confirmation: `package/thingino-daynightd/samples/t31-proc-jz-isp-fs.txt`
(a wyze cam3 T31X) shows framesource 0 running 1920x1080 with `scaler:
disable, crop: disable` and FOUR buffers - a non-scaled chan0 multi-buffer
ring works fine on T31 when pre-dequeue is off.

The ~45-47% `ch0_pre_dequeue_drop` and the 13.5 fps ceiling are the
pre-dequeue machinery itself: chan0 is forced to a single buffer, and the
pre-dequeue worker drops every frame for which the sole buffer has not been
requeued in time.

**prudynt's `scale = 1` verdict**: does NOT bypass the gate (the kernel
never looks at the scaler) and prudynt requests nrVBs=2 on stream0 - on a
pre-dequeue board that combination fails EnableChn outright. Not adopted.
The "is 1:1 scaling lossless" question is therefore moot for this fix.

**Fix applied in timps** (`src/hal/hal_ingenic.c`, fs_create):
- new `t31_ch0_pre_dequeue_time()` reads the 0444 module parameter from
  `/sys/module/tx_isp_t31/parameters/isp_ch0_pre_dequeue_time` once (cached;
  unreadable = treated as active, stays safe).
- the nrVBs clamp condition changed from `!scale` to
  `chn == 0 && pre_dequeue active`. Same behavior as before on today's
  cinnado boards (chn0 clamped to 1); additionally closes a latent trap the
  old condition left open: a SCALED chn0 (e.g. video0 reconfigured to
  1280x720) was previously unclamped -> nrVBs=2 -> dead channel 0 on any
  pre-dequeue board. On boards without the parameter (wuuk), chn0 now gets
  the normal 2-buffer ring instead of an unnecessary single buffer.
- explicit `videoN.buffers` still overrides (now with a warning that names
  the actual failure mode), so a runtime `echo "pre_dequeue_time 0"` probe
  stays possible without a rebuild.
- boot diagnostic LOGI per T31 channel: requested WxH, sensor WxH, scale,
  final nrVBs - makes the next boot log show exactly what timps requested.

Builds verified for T23, T31, C100, T40, T41 (`./build.sh deps/timps` each);
`make sim USE_CONTROL=1` runs and the `/control?json=1` video block and
`videoN.buffers` key are unchanged.

**The actual fps lever is a firmware change, not timps**: drop
`BR2_ISP_CH0_PRE_DEQUEUE_TIME(_VALUE=24)` (and the then-pointless
`BR2_ISP_CH0_PRE_DEQUEUE_INTERRUPT_PROCESS`) from
`configs/cameras/cinnado_d1_t31l_sc2336_atbm6031/..._defconfig` in the
thingino tree, rebuild, `make ota`. With the parameter gone the kernel
accepts nrVBs=2 on chan0 and the new timps code grants it automatically.
NOT done here (out of this task's tree). Caveats to verify on hardware:
one extra 1920x1080 NV12 buffer (~3.1 MB rmem) must fit - cinnado also sets
`isp_memopt=1`, which suggests the vendor was squeezing memory; and
pre-dequeue is a latency optimization, so glass-to-glass latency may rise
slightly. Verification unchanged: `ffprobe -count_frames` + drop counters
in `/proc/jz/isp/isp-fs`.

**Boot-time `num_buffers:2` errors (open, honest status)**: the observed
dmesg errors in the first ~67 s of a clean boot cannot come from timps'
chn0 path as hypothesized. From source: the scale decision never runs
before the ISP sensor query (isp_init fills `g_isp_sensor_w/h` before any
fs_create, with a static-config fallback - both yield 1920x1080 here), the
clamp is applied before CreateChn, EnableChn re-uses the stored attr on
every recovery cycle, and the REQBUFS count is nrVBs+FrameDepth = 1+0 = 1.
There is no first-attempt window with the raw `v->buffers`. Who requested
2 buffers on framechan0 during that window is NOT identifiable from source;
no other boot-time libimp client was found in the cinnado rootfs. Next boot
with the new build will show the diagnostic LOGI line; correlate its
timestamp plus the recovery-cycle log lines against the dmesg error
timestamps (`dmesg -T`-equivalent via /proc/uptime deltas). Also worth one
`cat /sys/module/tx_isp_t31/parameters/isp_ch0_pre_dequeue_time` on
cam-kinder-rechts (expect 24) and on cam-garage (expect file absent).
The related stability question: the watchdog exits the daemon after 5
consecutive fruitless recovery cycles (~5 s of misses each at the 500 ms
polling timeout); a boot-time stall (day/night switch storm) lasting the
whole window could in principle exhaust them and take the daemon down
without any buffers override - the observed ~67 s window says the margin
is not comfortable. Worth watching, not worth code changes until the
num_buffers:2 source is identified.

**Open: does `isp_ch0_pre_dequeue_time` also explain a reported OSD/logo
corruption at the bottom of the frame?** (2026-08-22, from a side
conversation with Paul, cross-checked against
https://blog.thingino.com/ingenic-isp-tuning-parameters, not yet tested
against our own hardware). The parameter's actual function, from the
kernel module's own `MODULE_PARM_DESC` and the blog post above (both
independent of - and consistent with - the disassembly finding this
section is built on): it is a latency optimization, not merely a buffer-
count trigger. The ISP hands a frame to the encoder `isp_ch0_pre_dequeue_
time` milliseconds *before* the sensor has finished reading it out (e.g.
24 ms of "head start" on a 1080-line frame), to hide pipeline stall/avoid
dropped frames. Set too high relative to the sensor's actual readout
timing, the hand-over can start before the last rows exist, which shows
up as a torn/corrupted strip at the bottom of the image - exactly where
an OSD logo commonly sits. The blog also documents a related, not yet
investigated parameter, `isp_ch0_pre_dequeue_valid_lines` (how many rows
must be valid before a frame is eligible for early handover - plausibly
the actual safety margin against this failure mode) and its channel-1
equivalent `isp_ch1_dequeue_delay_time`.

Not yet tested: whether a previously-reported "logo broken at the bottom
of the frame" incident on this fleet is actually this mechanism, or
something unrelated (a different project/camera, or an OSD-layer bug -
see `c9081ef63`'s unrelated OSD fix for a different bug in the same
subsystem). If revisited: compare a frame capture with
`isp_ch0_pre_dequeue_time=24` (the cinnado default) against one with it
disabled (`=0`, as on the post-a7bec4c92 fix) on the same camera/scene,
and check `isp_ch0_pre_dequeue_valid_lines`'s current value/effect if the
symptom reproduces.

## Fix 868696b verified on hardware (2026-08-22, cam-kinder-rechts)

Built (rebuild-timps + separate pack, version confirmed from the packed
squashfs: v1.9.2-21-g868696b), flashed via `make ota`, PTZ preserved
(1360,157). Boot log now shows the new deterministic line instead of the
former failed-retry-then-self-correct pattern:

    chn0: fs 1920x1080 (sensor 1920x1080, scale=0) nrVBs=1

No `EnableChn failed`, no `one buffer schedule` kernel errors, no recovery
cycling - clean boot straight to streaming. `ffprobe -count_frames`: 204
frames/15s (13.6 fps), matching the pre-fix baseline (203/15s) within noise,
exactly as expected: `isp_ch0_pre_dequeue_time=24` is still active on this
board's `/etc/modules.d/20-isp`, so the fix computes the same nrVBs=1 the old
code's lucky self-correction also arrived at - it makes the clamp *correct*
(scoped to the real kernel condition, safe for a scaled chn0 too) and
removes the crash trap for explicit buffers>1, but does not by itself change
the fps ceiling.

**Fleet-wide fps fix still requires the firmware defconfig change** (remove
the four `BR2_ISP_CH0_PRE_DEQUEUE_*` lines from
`cinnado_d1_t31l_sc2336_atbm6031_defconfig`) - not applied, needs the user's
sign-off first since it touches all six cinnado boards and trades ~3.1MB
more rmem (plus `isp_memopt=1` is currently paired with this parameter,
worth understanding why before removing it) for the removed frame-drop
constraint.

## RESOLVED: T31+sc2336 fps ceiling fixed, verified on cam-kinder-rechts (2026-08-22, 01:01)

Full result, before/after on the same camera:

    before: 203-204 frames/15s (13.5 fps), ch0_pre_dequeue_drop climbing ~45-47%
    after:  373 frames/15s (24.9 fps), ch0_pre_dequeue_drop = 0

**Fix applied** (`configs/cameras/cinnado_d1_t31l_sc2336_atbm6031_defconfig`):
removed `BR2_ISP_CH0_PRE_DEQUEUE_TIME(_VALUE)` and
`BR2_ISP_CH0_PRE_DEQUEUE_INTERRUPT_PROCESS(_VALUE)` (4 lines), raised
`BR2_THINGINO_RMEM_MB` from 22 to 26 to make room for chn0's now-permitted
second video buffer (`nrVBs` went 1 -> 2 in the boot log, confirming
`868696b`'s clamp correctly stopped applying once the kernel condition it
checks - `isp_ch0_pre_dequeue_time > 0` - was no longer true). Kernel cmdline
now `mem=38M@0x0 rmem=26M@0x2600000` (was `mem=42M@0x0 rmem=22M@0x2a00000`).

**Build-system gotcha found along the way, distinct from the earlier
timps-stamp issue**: `make CAMERA=... IP=... <pkg>-dirclean <pkg>` is NOT
enough to force a package's generated files to update in `target/` - for
`ingenic-sdk` (which writes `/etc/modules.d/20-isp` via `$(GENERATE_MODULE_LOADER)`
in its install-target-cmds), the file kept its old content through two
dirclean+rebuild attempts, still timestamped from the original build. The
project's own `rebuild-<pkg>` target (`Makefile.ota`) does
`<pkg>-dirclean <pkg> <pkg>-reinstall target-finalize` - the missing
`-reinstall` and `target-finalize` steps were the actual gap. **Use
`make CAMERA=... IP=... rebuild-<pkg>` for any post-defconfig-change
package rebuild, never manual `-dirclean pkg` alone.**

**Live verification, cam-kinder-rechts, 2026-08-22 01:01**: clean boot, no
`EnableChn failed`, no `one buffer schedule` kernel errors, PTZ preserved
(1360,157), version `v1.9.2-21-g868696b`. `free -m`: 33440K total (Linux
side), 2176K free + 17324K buff/cache right after boot - tighter than
before (was ~37536K/5768K) but not exhausted. No swap. Needs sustained
observation under real load (multiple viewers, motion detection, recording)
before trusting the memory margin - a short boot-time reading doesn't prove
it holds over hours.

**Not yet done, needs the user's decision in the morning**: rolling this
defconfig change to the other five cinnado_d1_t31l_sc2336_atbm6031 cameras
in the fleet (cam-sz, cam-schuppen, cam-wohn, cam-wohn-ofen,
cam-wintergarten). Recommend watching cam-kinder-rechts for real-world
stability (a day of normal use, ideally including an overnight recording
window) before touching the other five - this is the first and only unit
running the new RMEM/buffer configuration.

## Isolated: RMEM_MB increase was unnecessary (2026-08-22, cam-kinder-rechts)

Split the two changes from the fix above and tested each build separately,
same camera:

    RMEM=26, no pre_dequeue: 373 frames/15s (24.9 fps), free mem  2176K after boot
    RMEM=22, no pre_dequeue: 375 frames/15s (25.0 fps), free mem  6232K after boot

Both hit `ch0_pre_dequeue_drop=0` and `nrVBs=2` in the boot log. The original
22MB rmem pool already had enough slack for chn0's second video buffer -
raising it to 26 bought nothing and cost 4MB off the already-tight 42MB Linux
heap for no benefit. **Recommendation revised: keep `BR2_THINGINO_RMEM_MB`
at its original value (22); only remove the four
`BR2_ISP_CH0_PRE_DEQUEUE_*` lines.** Simpler diff, no memory trade-off.

Operational note found along the way: reducing `BR2_THINGINO_RMEM_MB` via
`make ota` did not take effect on first boot after flashing - the device
came up with `rmem=0M` (a botched/incomplete memory-layout remap, visible in
the flash tool's own "Remapping memory: osmem 38M -> 64M, rmem 26M -> 0M"
log line) and timpsd never started. A second, plain reboot corrected it to
the proper `rmem=22M@0x2a00000`. The earlier 22->26 *increase* took effect
cleanly in one boot, so this looks specific to shrinking the reservation.
Not investigated further since the final recommendation no longer changes
RMEM_MB at all for this board, but worth knowing if RMEM is ever changed
elsewhere: verify the live `/proc/cmdline` after an OTA that changes it, not
just after the flash command returns.

Log-server cross-check (2026-08-22, Loki job="camera"): the *earlier* 22->26
flash last night hit the identical failure signature at 00:59-01:15 CEST
(`IMP_ISP_AddSensor failed`, `HAL init failed - retrying`, `KMEM Method:
alloc_kmem_init mmap Addr 2600000 and Size 0 error`, plus a burst of
`ipu_osd error` from the OSD compositor racing the half-initialized ISP) -
so this is not shrink-specific after all. It self-healed on its own that
time (timpsd's retry-with-backoff loop happened to succeed before hitting
its give-up limit); this morning's shrink case did not self-heal and needed
a manual reboot. Moot for the shipped fix since RMEM_MB no longer changes,
but relevant for anyone touching `BR2_THINGINO_RMEM_MB` on any board in the
future: expect the first post-flash boot to be unreliable in either
direction and verify `/proc/cmdline` + a working video stream before
trusting the flash, not just that the SSH port came back up.

The five other cinnado_d1_t31l_sc2336_atbm6031 cameras are still at the
original `BR2_THINGINO_RMEM_MB=22`, so a fleet-wide rollout of just the
pre_dequeue removal never triggers this shrink case at all.

## Candidate: raise the package-default night_gain from 4096

`night_gain` (= `total_gain_night_threshold`) has now been raised to 8000 on
four cameras: cam-sz (2026-08-21, confirmed working - switched to day
earlier the next morning, 2026-08-22), cam-wohn, cam-schuppen, and cam-vorne
(2026-08-22, all applied but not yet individually confirmed).

**Only cam-sz has a clean before/after result.** The other three are
confounded:

- **cam-schuppen has direct prior history against a clean win**: a
  2026-08-16 note in its own overlay says night_gain=8000 (paired with
  day_trigger=2500 back then, not restored here) was already tried on this
  exact camera and "may not fully close" its dead zone. Re-raising to 8000
  alone repeats a value already documented as insufficient there.
- **cam-vorne was mid-dawn-transition when set** (projected day exposure
  falling from ~992000 to ~6700 over ~40 minutes that morning, per the
  fleet-wide log review) - it may have crossed into day shortly regardless
  of the threshold change, so its outcome won't isolate the setting's effect.
- **cam-wohn** has no confounding history noted, but also no dedicated
  before/after measurement yet - it happened to be mid-transition too during
  the same log review.

Before making 8000 (or any other value) the package default in
`package/timps/files/timps.conf` / `src/config.c`'s compiled-in default:
confirm each of these four independently over a few real dawn/dusk cycles,
and settle cam-schuppen's case specifically given its contradicting history
(may need day_trigger raised too, not night_gain alone, per the 2026-08-16
note). A single confirmed camera (cam-sz) is not enough evidence for a
fleet-wide default change.
