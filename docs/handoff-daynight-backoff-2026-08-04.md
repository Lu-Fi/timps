# Handoff: daynight probe-economy fix (backoff + brightening margin/ratchet)

Date: 2026-08-04, ~08:40 CEST. Written at coordinator request before a quota
cutoff. Audience: a fresh agent with zero session memory. Ground rules that
applied all session: implement + verify in `make sim`, human reviews every
diff before anything ships; never push or touch live cameras without
explicit coordinator approval.

---

## 1. Current state of the fix (the task in flight)

**Code-complete, uncommitted, partially sim-verified.** The entire diff is
`src/daynight.c` only (+97/−10, on top of HEAD `7e99627` = v1.7.6). No config
schema changes, no new user-facing settings (per spec). Docs (wiki /
config.h) have NOT yet been updated for this change — see "next steps".

### What the diff does (all in `src/daynight.c`)

Motivation (fleet logs 2026-08-03→04, all 11 cameras, collected in
`/tmp/claude-1000/-mnt-NVMe-git-timps/361c25d1-256d-46dc-bf2f-b15d2ab9fee7/scratchpad/fleetlogs/`
— NOTE: scratchpad is session-scoped and may be gone; the analysis summary
survives in the conversation and in §2 below): the hourly
`night_reconfirm_s` probe itself was the user-visible "periodische
Tag/Nacht-Umschaltungen" (8–12 IR-cut-clunk flips per camera per night, every
probe reverting on railed day-pipeline gain 15872), plus 2–6 extra flips on
slow pre-dawn ramps from the v1.7.4 sustained-brightening probe firing
tangentially (e.g. hold starting at 4898 vs bar 4906).

Three mechanisms, pure probe-scheduling logic:

1. **Exponential backoff** — a probe that FAILS (reverts to night within
   `DN_PROBE_FAIL_WINDOW_MS` = 30 s) doubles the periodic reconfirm interval:
   multiplier ×1→×2→×4 (`DN_PROBE_BACKOFF_MAX` 4), interval bounded by
   `max(night_reconfirm_s, DN_PROBE_BACKOFF_CAP_S=14400)`. Any genuine
   transition (or a probe that sticks, via the next genuine transition)
   resets the multiplier to 1.
2. **Arming margin** — the brightening hold only starts when smoothed gain
   is below `DN_BRIGHTEN_MARGIN` (0.97) of the probe bar, never on a tangent
   graze.
3. **Failure ratchet** — after a failed probe, a NEW brightening hold
   additionally requires smoothed gain < `day_gain_pct%` of
   `probe_fail_smooth` (the smoothed night gain when the failed probe
   fired). This is what actually stops the pre-dawn volley: each failed
   probe re-samples a *lower* baseline, so a continuous ramp re-crosses the
   new bar forever; the margin alone only delays that. A real light-on step
   (20–35 % drop) passes the ratchet immediately. Cleared together with the
   backoff on any genuine transition.

Implementation landmarks (current line numbers approximate):
- New constants + rationale comment block: after `DN_ADOPT_PROBE_S`
  (~line 440): `DN_PROBE_FAIL_WINDOW_MS`, `DN_PROBE_BACKOFF_MAX`,
  `DN_PROBE_BACKOFF_CAP_S`, `DN_BRIGHTEN_MARGIN` (all `#ifndef`-overridable,
  matching house style).
- New thread-locals near `probe_day_ms`: `int probe_backoff = 1;`,
  `float probe_fail_smooth = -1.0f;` — reset in BOTH the disabled branch and
  the re-enable branch.
- Brightening hold-start gate (inside the `else if (brighten_armed)` /
  `!brighten_since_ms` branch): margin AND ratchet conditions before
  `brighten_since_ms = now_ms`.
- Main switch block (the `dn_switch(target, why)` branch): failure/reset
  accounting placed BEFORE the state resets (it must read `probe_day_ms`
  and pre-reset `smooth_tg`); logs
  `"probe confirmed genuine night (backoff x%d, brighten ratchet < %.0f)"`.
  Then the periodic re-arm applies the multiplier + cap.

### Verification status (timpsd-sim, fake-ISP-file harness)

Harness: sim reads gain from a fake `daynight.isp_path` text file; write
`SENSOR analog gain : N` (log2 units, 32/stop → tg = 256·2^(N/32)).
Configs used: scratchpad `dn-sim3.conf` (reconfirm=20 s, mid-band harness),
`dn-sim4.conf` (reconfirm=0), `dn-v1.conf` (reconfirm=15 s, ports 18881/18555).
All are derived from `scripts/camera.conf` with high ports + fake isp_path +
short settle/baseline timers (`interval_ms=200`, `transition_s=2`,
`baseline_delay_s=3`). IMPORTANT sim-math caveat: at 200 ms ticks the
baseline drift absorbs holds with smooth/baseline ratio > ~0.6875 (at the
real 500 ms default it is ~0.76) — pick test ratios ≤ 0.66 or holds
dissolve by design and the probe never fires (this cost me two test
iterations; it is correct behavior, not a bug).

- **PASSED — V1 backoff stretching** (the hourly-volley replay): constant
  darkness, reconfirm=15 s, every probe fails. Log shows probes spaced
  15→30→60→60→60 s with `backoff x2, x4, x4, x4...` — exact expected
  1h→2h→4h-capped shape scaled down.
- **PASSED — V3 clean first-try** step brightening 3298→2186 (66 %): margin
  did not block, hold ran 60 s, probe fired, stuck in day (mode 0).
- **PASSED — failure detection + backoff latch**: forced probe failure
  (railed 57k during probe) logs `backoff x2, brighten ratchet < 1202` and
  reverts correctly.
- **PASSED — ratchet block**: after the failure, 90 s at gain 2233 (below
  margin-bar, above ratchet) produced NO new hold (pre-fix this was the
  10–40-min volley cycle).
- **NOT YET CLEANLY DEMONSTRATED — ratchet PASS edge** (hold allowed again
  once gain undercuts the ratchet). Two attempts failed on test
  choreography, not code: (a) chose 1244 vs actual ratchet 1202 (still
  blocked — which itself confirmed the block), (b) compressed
  `baseline_delay_s=3` let the railed 57k pollute the post-revert baseline
  (smoothed 57k→ mixed ≈35k), which legitimately fired the NORMAL day
  trigger and reset the ratchet (correct behavior; real config uses 30 s
  delay so this pollution is a sim-timing artifact only).
  A worked-out window for a clean demonstration exists — see §3.

---

## 2. Session context: the full daynight saga (v1.7.1 → v1.7.6 + tonight)

`CHANGELOG.md` covers v1.7.0–v1.7.6 (commits `d3feab3` backfill, `97f9106`,
`b1a5a08`, `e54e380`, `7e99627`). Short chain, with what each fixed:

- v1.7.1/2: adaptive boot-settle + periodic `night_reconfirm_s` probe; new
  fields exposed in GET /control.
- **Unrelated but session-relevant:** the cam-K magenta incident was NOT
  a daynight/SDK bug — an interrupted `timps-qa.sh` run stranded manual WB
  rgain/bgain=32767 in flash config; QA script hardened (commit `98d99ed`,
  interruption-safe restore trap + WB probe values now 1024=unity). The
  ingenic-sdk bump e4313e0→7b4b0f4 is a single T23 sc2336p IQ-bin revert —
  exonerated. Also: the cam-I "blue/purple" image was *IR light
  rendered through the color pipeline* (ISP forced day via /control while
  board ircut/IR LEDs stayed in night — split-brain; lesson: manual day/night
  overrides must go through the board `daynight`/`color` script, not raw
  `image.running_mode`).
- v1.7.3 (`97f9106`): adaptive-threshold hardening v1 — trigger floor,
  upward-only baseline drift, sustained-brightening probe; plus
  `night_baseline`/`day_trigger` in status JSON + WebUI photosensing page
  extensions (firmware repo `package/timps/files/www/config-photosensing.*`).
  **Regressed:** upward-only drift ratcheted on AGC noise → overnight flap
  loop.
- v1.7.4 (`b1a5a08`): flap fix — night-only smoothed gain, symmetric slow
  baseline drift, edge-armed brightening, post-probe revert stability gate
  (`DN_PROBE_SETTLE_MS`).
- v1.7.5 (`e54e380`): dead-zone adoption — a boot landing with gain inside
  the day..night dead-zone stayed DN_UNKNOWN forever (silent, no
  self-healing possible since probes were gated on cur==NIGHT); now adopts
  the persisted mode after boot settle, with an early verify probe
  (`DN_ADOPT_PROBE_S=300`, one-shot even when reconfirm disabled).
- v1.7.6 (`7e99627`): the silent-limbo audit sweep (motion IVS starvation,
  immortal RTSP sessions, httpd discard spin, SRT liveness, audio watchdog
  self-reset, jpeg watchdog, fs enable retry, backchannel) — from the
  4-agent fleet-wide audit.
- **Tonight (uncommitted target):** the remaining flapping IS the probe
  design cost: hourly reconfirm probes (by design, each visibly clunks
  IR-cut) + pre-dawn brightening tangent volley. The in-flight fix (§1) is
  the answer. Coordinator explicitly REVERTED the interim
  `night_reconfirm_s=10800` mitigation fleet-wide and in camera profiles —
  do not assume it is present anywhere.

Also NOT yet in changelog: this fix itself (suggest `fix(daynight): probe
economy - exponential backoff on failed reconfirm probes, brightening
arming margin + failure ratchet`, presumably v1.7.7).

---

## 3. Exact next steps to finish

1. **Finish verification — ratchet-pass edge.** Cleanest scripted scenario
   (avoids both the absorption boundary and baseline pollution and the
   normal-trigger short-circuit; all numbers precomputed for 200 ms ticks):
   - Config: `dn-sim4.conf` derivative with `baseline_delay_s = 12`
     (pollution decays ~10 %/tick; 60 ticks reduces railed 57k to noise).
   - Phase 1: dark units 118 (tg 3298), wait for baseline.
   - Phase 2: step to units 95 (tg 2004, ratio 0.61) → hold → probe fires;
     immediately write units 250 (railed 57549) → revert →
     `backoff x2, ratchet < ~1202`.
   - Phase 3: immediately write units 91 (tg 1840) as the post-revert night
     level → baseline samples ≈1840–1940 → `day_trigger ≈ 1104–1164`,
     margin-bar ≈ 1430–1505, ratchet 1202: window (day_trigger, ratchet) is
     nonempty.
   - Phase 4: write units 71 (tg 1191 — inside the window, ratio vs
     baseline ≈0.63 < 0.6875 so drift won't absorb): EXPECT hold starts
     (ratchet passes), probe fires at +60 s, day reading 1191 < 3000 →
     sticks. This is the missing PASS demonstration.
   - Sanity: assert the normal day trigger did NOT fire first (no
     `switching to day (total_gain` line before the brightening probe).
2. **Re-run the two already-passing regression scenarios once more on the
   final binary** (V1 stretch, V3 clean step) — cheap, and the earlier runs
   interleaved test-harness noise.
3. **Docs**: add the three constants + behavior to the hardening bullet
   lists in `docs/wiki/Day-Night.md` (the "Three hardenings … revised
   2026-08-03" block) and the `day_gain_pct`/`night_reconfirm_s` rows in
   `docs/wiki/Configuration-Reference.md`; extend the `config.h` comment
   above `night_reconfirm_s`. House style: WHY-comments citing the incident
   date and fleet evidence.
4. **CHANGELOG.md** entry + coordinator review of the full diff; version
   bump only when coordinator says so. No rollout, no live changes, no push
   — explicitly out of scope until review.
5. "Done" = all §3.1/3.2 scenarios pass on the final binary + docs updated
   + coordinator has the diff summary with the V1/V3/failure/block/pass
   evidence lines.

## 4. Open threads not yet closed elsewhere (do not drop)

- **Audit Tier-2/3 items** from the consolidated silent-limbo report were
  NOT all fixed in v1.7.6 (it covered Tier-1 + selected items). Still open,
  per that report: video-watchdog-vs-motion-pinned-FS alignment (V2),
  speaker/backchannel inactivity-release policy (F2 — needs a product
  decision), record.c "last motion event age" status field, OSD
  thread-create LOGE, IDR retry (H-1), manual-record-off status exposure.
  Coordinator has the ranked list; user decides what to act on.
- **task_16be228b** ("Harden daynight baseline sampling against mid-window
  light changes") — the symmetric drift in v1.7.4 largely supersedes it;
  suggest closing or repurposing after this fix ships.
- **cam-F custom thresholds** (450/4500 vs fleet 300/3000, set live by
  coordinator 2026-08-04) are still in place on 192.168.1.100; revisit
  whether they are still wanted once probe economy ships.
- The **daynight no-metric idle path** (`b<0 && tg<0`) logs at WARN now
  (v1.7.6) but still never reaches dead-zone adoption; accepted as-is, noted
  in the audit.
- Fleet log evidence for tonight's diagnosis lives only in the session
  scratchpad (`fleetlogs/*.log`) and on the cameras' logread ring buffers —
  if the next agent needs raw evidence, re-pull promptly (logread rotates).
  SSH: root@ with keys, host keys CHANGED after the v1.7.6 reflash (use a
  fresh known_hosts or per-session `UserKnownHostsFile`).
- QA runs against live cameras: use the hardened `scripts/timps-qa.sh`
  (post-`98d99ed`) only; older checkouts strand WB test values on
  interruption (that is what caused the cam-K magenta incident).

— end of handoff —
