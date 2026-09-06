# Session Summary — 2026-09-06 evening/night: fleet incidents, 8 fixes

**Date:** 2026-09-06 evening through 2026-09-07 00:30
**Trigger:** a post-rollout syslog review, followed by a live investigation of
two T20 cameras that went down during the session and had to be recovered
via USB (Ingenic Cloner / thingino-dfu). Several code fixes came out of the
recovery work itself, not just the original review.

Commits (in order): `8937d00`, `348af01`, `8022ca7`, `e31afcf`, `c9fa77e`,
`879d467`, `9d3bbcf`, `a14c59d`. All on `main`, all pushed.

---

## 1. `hal: don't double-disable idle framesources at teardown` (`8937d00`)

**Symptom:** `[HAL_ING] teardown: 2 IMP call(s) failed (FrameSource_DisableChn
rc=-1)` fired on 100% of clean shutdowns, fleet-wide — a warning that cried
wolf on every restart, defeating the diagnostic it was added for.

**Root cause:** `ing_stop()` unconditionally called `IMP_FrameSource_DisableChn`
on every video/JPEG channel, but the producer threads' own exit path
(`fs_unuse()`) had usually already disabled an idle channel (no subscriber).
A second disable on an already-idle channel is rejected by libimp
(`rc=-1`, "FrameSource N do not enable") — harmless, but noisy.

**Fix:** new `fs_teardown(chn)` next to `fs_use`/`fs_unuse`, gated on the
existing `g_fs_enabled[]` refcount (the single source of truth for hardware
state elsewhere in the file) — only calls `DisableChn` when the channel is
actually enabled, and resets the refcount for the in-process start-retry
path (`ing_stop → ing_init → ing_start`). Verified live on cam-garage: 3×
`S95timps restart`, zero false warnings, both RTSP streams recovered
normally each time.

## 2. `audio: elect backchannel owner only on real audio packets` (`348af01`)

**Bug:** `bc_elect_locked()` ran *before* RTP header/payload-type validation
in `bc_feed_rtp()`, so any packet from the current owner — including a
muxed RTCP RR or a malformed datagram — re-stamped the staleness timer. A
client holding the backchannel open with keepalives but never actually
talking would block the 10 s staleness re-election forever.

**Fix:** validate first (touches no shared state), elect under the lock only
on confirmed audio. Also fixed as doc drift (no code change): `audio.talk_ws`
and `audio.spk_enabled` are actually live (read at next `/talk` request /
next AO open respectively), not restart-only as previously documented.

**Verified clean in the same pass** (adversarial audio-subsystem review):
lock ordering, `g_ai_up` TOCTOU handling, AO/AI error-path unwinds, AEC live-
toggle safety (disassembled `libimp.so` to confirm the vendor SDK actually
handshakes with its own record thread for AEC, unlike the documented
HPF/AGC/NS restart-only hazard).

## 3. `build: pin T40/T41 headers to the libimp versions thingino ships` (`8022ca7`)

Thingino links libimp **1.3.1** for T40 and **1.2.6** for T41, but the
Makefile pinned the **1.2.0** header set. T40 1.2.0's `IMPEncoderStream`
lacks the trailing `bool isVI` field 1.3.1 added, so `IMP_Encoder_GetStream`
in `video_thread()` wrote one word past the struct on every frame — the same
bug class as the earlier T23 `fcrop` header mismatch. Not currently hit by
any deployed camera (no T40/T41 in the fleet yet) — fixed pre-emptively.
Added a `(void)st.isVI` compile-time tripwire so a future header
regression fails the build instead of corrupting memory silently.

## 4. Day/Night: IMP exposure as ground truth (`e31afcf`, `9d3bbcf`)

**Claim in the code that turned out to be wrong:** a comment in `daynight.c`
asserted there was no IMP accessor for the sensor's maximum integration
time on classic-tuning SoCs, so cameras that don't publish it via `/proc`
(both fleet T20 boards) fall back to a session high-water-mark estimate.
`IMP_ISP_Tuning_GetExpr` does publish it (`it_max_lines`), on every classic
platform, identical struct layout — the comment was simply wrong.

**Step 1 (`e31afcf`):** added a parallel, decision-free read via
`hal_isp_exposure()` (new small wrapper) and appended it to the daynight
trace CSV, specifically to gather comparison data before changing any
behavior.

**Step 2, same night after live data came in (`9d3bbcf`):** on a T20 camera,
the high-water-mark ratio reads `1.0000` (falsely "full daylight") for the
first ~2.5 minutes after every restart, while the IMP-based ratio is
correct from the very first sample (`0.8350` vs `1.0000` at t=21 s, live-
measured). Once the HWM has seen one near-maximum exposure the two agree to
within 0.1%. Given the fleet had several unplanned restarts that same night,
this boot-time blind spot was live and observable, not theoretical.

**Scope:** only overrides `o->ratio` when the scrape never got a real
published maximum (`mit_real == false` — currently the two T20 cameras).
The ten T31/T23 cameras, which do publish a real maximum, are untouched;
Fable's earlier cross-check found no disagreement there worth acting on.

## 5. `image: add ae_it_max_us` + boot-time fix (`c9fa77e`, `a14c59d`)

New opt-in (`0` = off) config key that caps AE integration time — bounds
motion blur at night at the cost of more gain. Ships default-off: capping
exposure shifts daynight's exposure index, so per-camera threshold
recalibration is needed before it's safe to turn on anywhere.

**Original limitation, discovered the same day:** reliable as a **live**
control (a `/control` POST while a client streams takes effect on the next
frame) but not as a **boot-time** setting — `isp_ae_it_max_latch()` tried to
fake an active pipeline via `fs_use()` + `usleep()`, which enables the
framesource but doesn't make anything actually consume frames from it; the
SDK silently ignored the write. Measured: even a real client streaming for
a full minute *after* the boot-time attempt left the sensor at its default
max.

**Fix (`a14c59d`):** moved the (re-)apply into the real frame-delivery path.
`ae_it_max_on_frame()` runs from `video_thread`/`jpeg_thread`/`sw_rot_thread`
right after each thread actually pulls and publishes a frame — genuine
consumption, not a simulated one. The slow path (`ae_it_max_check()`, every
~15 frames, serialized on the existing `g_isp_lock`) reads back via
`hal_isp_exposure()` and re-applies if the cap isn't holding; backs off to a
5-minute poll after 6 failed attempts so a SoC that never honours the cap
costs one log line, not a spin. Also newly covers a previously-undocumented
gap: the cap can be silently lost on a framesource idle→active recycle, not
just at boot — the same `ae_it_max_arm()` hook now fires on that edge too.

**New, separately-discovered quirk (documented, not fixed):** within one
daemon lifetime the cap only ratchets *down* — once the SDK has capped the
reported maximum, raising the target again is rejected as "above the
maximum", and `0` (disable) has no restore call. Only a process restart
clears it fully.

**Measured on cam-garage (T31X/sc4336p), real night scene:**

| | before fix | after fix |
|---|---|---|
| boot-time, no client yet | 1496 lines (uncapped — bug) | 1496 lines (cap not yet confirmed) |
| boot-time, ~45s in with a real streaming client | 1496 lines (still uncapped) | **545 lines (capped, confirmed)** |
| live `/control` POST | worked (unaffected baseline) | still works, no regression |

**Binary cost, cross-compiled T31, measured directly (not estimated):**
whole feature (both commits) is **+1984 .text / +28 .data / +32 .bss**
(308927→310911 / 7124→7152 / 189932→189964 bytes) — about 0.65% of the
310 KB binary.

**Real-world caveat found while demoing this:** on a camera whose gain is
already near its ceiling before the cap is applied (cam-garage's analog
gain was 126/127 even uncapped), there's no headroom left to trade shorter
exposure for more gain — the image just gets darker, not just noisier. The
usual "less blur, more grain" framing assumes gain headroom that isn't
always there.

## 6. QA script: don't let stale tmpfs artifacts cause a false OOM diagnosis (`879d467`)

A QA-induced camera crash was initially attributed by the QA script itself
to a live-DSP-toggle use-after-free (the same class this session's audio
review had specifically verified as *fixed*). The actual dmesg was a plain
kernel OOM-kill, not a signal death. Root cause: five earlier QA runs
against the same camera (a T23 with ~38 MB total RAM) were launched without
`--ssh`, so the `record.clip` test's cleanup — which lives in the SSH-only
branch — never ran, leaving five 2 MB test clips in tmpfs. A sixth clip plus
normal daemon RSS was enough to push the whole system over the edge; the
kernel OOM-killer took the largest RSS process, which happened to be
timpsd. No leak in timpsd itself — verified by fuzzing `audio.agc` with 20
rapid concurrent `/control` writes on a healthy camera: ~300 KB transient
RSS, freed correctly, no leak.

**Fix:** preflight now reports tmpfs headroom and clears stale
`/tmp/timps_qa_*.mp4` from earlier runs; the clip test no longer POSTs a
clip at all without `--ssh` (it can't clean up after itself either way);
and the OOM-grading logic now distinguishes "already dead before the
stress ran", "OOM-killed during stress" (kernel dmesg says so) and a real
UAF, instead of assuming the worst label unconditionally.

**Known gap this surfaced, not fixed:** timpsd has no watchdog/respawn
mechanism at all — `/etc/inittab` has no respawn line, `S95timps` is
start/stop only, the only watchdog process on these boards is the hardware
`/dev/watchdog` (kernel-hang only). If timpsd dies for *any* reason it stays
dead until a human notices. Deliberately left as-is (a design decision, not
a bug) — noted here in case it comes up again.

---

## Fleet-firmware-side lesson (not a timps change, noted here for the
record since it caused two of this session's outages)

`thingino-firmware`'s `ota-rootfs` target only ever writes the rootfs+data
partitions, never the kernel. It is only safe when the device was already
fully flashed (kernel included) from the *same* build lineage recently — if
the on-device kernel predates a kernel-config-relevant merge (in this case,
`CONFIG_XFRM` and friends dropped in an upstream merge from several weeks
earlier) while the freshly-built rootfs's kernel modules assume the new
config, the mismatch is silent until the module (in this case the WiFi
driver) corrupts memory on first use — the box never re-associates with
WiFi again, no error visible over the network. Always confirm the target
was fully (`ota`, not `ota-rootfs`) flashed at least once since the last
kernel-affecting merge before trusting a rootfs-only update.
