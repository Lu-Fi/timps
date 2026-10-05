# FrameSource keepalive and the debounced latch kick — 2026-10

Two changes, one reason: on the open-stack T23 and on the T41, a FrameSource
`DisableChn`/`EnableChn` edge while the sensor streams can hang the whole SoC,
and timps produced such edges all day long with nobody watching.

1. **All SoCs:** the ISP latch kick (`fs_kick_chn0()`) no longer drops its chn0
   reference right after its 500 ms wait. The reference goes through the same
   2 s idle debounce as an idle encoder. When chn0 already runs, the kick only
   holds it on and no longer waits.
2. **`general.fs_keepalive`** (`-1` auto / `0` off / `1` on, default `-1`,
   restart-only): `ing_start()` takes one reference on every video FrameSource
   and holds it until `ing_stop()`. The running daemon then never disables a
   FrameSource. Encoders stay on-demand. **Auto = on for `USE_OPENIMP` builds
   on T23 and T41, off everywhere else.**

Code: `src/hal/fs_ref.h` (state machine, host-tested by `make test-fs-ref`),
`src/hal/hal_ingenic.c` (`fs_use`/`fs_unuse`/`fs_kick_chn0`/`fs_kick_reap`/
`fs_keep_acquire`/`fs_teardown`, the keepalive block in `ing_start()`, the JPEG
cold-start path), `src/config.c` (`general.fs_keepalive`), `src/control.c`
(`caps.framesource`).

The design that led here was written from the code first (scratch document
`fs0-wake-design.md`, proposals A/B/C, recommendation "A on the open stack
T23/T41 plus a kick through the normal debounce on all SoCs"). This note keeps
what is needed to understand and revisit the decision.

---

## 1. The problem

### 1.1 What hangs

* **T41 (open stack):** reproduced **3 of 3** times: one FrameSource channel
  was idle-stopped and restarted within ~200 ms while the other channel kept
  running, and the SoC hung hard (no kernel log, no watchdog-recovered
  process; power cycle).
* **T23 (OpenIMP + open-tx-isp), cam-B:** hung during a wake sequence of the
  kind described in 1.2.
* A fix in the ISP/FrameSource driver is being worked on elsewhere. Until it
  ships (and probably afterwards, see section 6), userspace must not produce
  the pattern.

The common factor is an edge on one FrameSource channel while the sensor and
at least one other channel stream. The hang has not been reproduced on a vendor
libimp build; whether vendor T41 is affected is **unknown** (no vendor T41 has
been tested).

### 1.2 Evidence: the wake sequence with nobody connected

Source: the central syslog collector (syslog-ng → Loki on host `docker`),
which stamps on receipt; the stamps below are the kernel uptime of cam-B (T23,
OpenIMP, timelapse every 60 s, no RTSP/HTTP client, default `motion.*`). They
were taken from the incident report that started this work and were **not
re-read for this note** (no camera or log access during the implementation).

| uptime [s] | what timps did | FS0 |
| --- | --- | --- |
| ~3170 | start; boot kick at the end of `ing_start()` | E, 500 ms, **D at once** |
| 3171.5 | first timelapse shot (`timelapse.c` shoots right after start) | **E a few ms after that D**, D ~2-4 s later |
| 3178 | day/night boot measurement: switch to day → `color` hook → `POST /control` → kick | E/D |
| 3186 | verdict night: switch → kick | E/D |
| 3194, 3203 | the two post-switch re-asserts (`DN_REASSERT_MS` = 8 s) → kick each | E/D, E/D |
| 3214 | probe → day: switch → kick | E/D |
| 3222, 3231 | re-asserts → kick each | E/D, E/D |
| every 60 s | timelapse shot | E/D |

About 9 enable/disable pairs in ~70 s on a camera nobody looked at. The second
row is exactly the T41 repro pattern: a disable followed within milliseconds
by an enable.

### 1.3 Root-cause chain (why timps produced edges at all)

1. **On-demand FrameSource.** `fs_use()`/`fs_unuse()` refcount every
   FrameSource; the last `fs_unuse()` calls `DisableChn`. That is the design
   behind timps' near-zero idle CPU (on vendor stacks a live FrameSource pumps
   the bound pipeline even with the encoder stopped; ~19 % idle CPU was
   measured long ago, a mixed upper bound including poll loops and the OSD
   updater, SoC unknown, before OpenIMP).
2. **The ISP latches `running_mode` and flip only while chn0 delivers
   frames.** Root-caused on cam-L (T23/sc2336, vendor stack) 2026-08-01 and
   2026-08-09: a `SetISPRunningMode` with chn0 idle returns 0 and is never
   applied, even while chn1 streams. Hence the **kick**: after such a Set,
   enable chn0, run 500 ms, disable.
3. **The kick disabled at once.** `fs_kick_chn0()` was
   `fs_use(0); usleep(500 ms); fs_unuse(0);` — the 2 s debounce that every
   encoder thread applies before its own `fs_unuse()` was bypassed. With chn0
   idle that `fs_unuse()` was a real `DisableChn`, and at start-up the next
   event was the first timelapse shot: an `EnableChn` a few ms later.
4. **Re-asserts multiply the kicks.** Every day/night switch and every probe
   is followed by two re-asserts 8 s apart (`daynight.c`, defence against a
   lost hook POST); each one is a `POST image.running_mode` and therefore a
   kick. A switch is three kicks, a probe with its revert about six. Over a
   day: ~20-25 pairs from day/night.
5. **Timelapse dominates.** Each shot wakes the piggyback JPEG encoder on
   video0, i.e. FS0, and the debounce disables it 2 s after the grab:
   **1440 enable/disable pairs per day** at `timelapse.interval_s = 60`
   (`jpeg.snapshot_path` does the same with its fixed 60 s cadence). External
   snapshot pollers (Home Assistant, agents) add an unknown number.
6. **Motion pins FS1.** Default `motion.monitor_stream = 1` keeps FS1 enabled
   permanently while FS0 toggles — the "other channel streams" half of the
   T41 pattern is the default configuration.

### 1.4 How prudynt and raptor do it

Neither has on-demand FrameSources; both set `running_mode` directly.

* **prudynt-t** (local clone `559d912`): `src/VideoWorker.cpp:228` enables the
  FrameSource and `:234` calls `IMP_Encoder_StartRecvPic` once when the worker
  starts; `:254`/`:258` stop the encoder and disable the FrameSource only when
  the worker ends. `IMP_ISP_Tuning_SetISPRunningMode` is called directly
  (`src/IMPSystem.cpp:134`, `:249`). FrameSource **and** encoder run
  permanently.
* **raptor** (`31df3f2`): `rvd/rvd_frame_loop.c:231-232` starts every stream at
  start-up (`rvd_stream_start` → `fs_enable_channel` + `enc_start`,
  `rvd/rvd_pipeline.c:1432-1433`), `:290-291` stops them only at exit
  (`rvd_stream_stop` → `fs_disable_channel`, `rvd_pipeline.c:1359`). Only the
  JPEG encoder is on-demand. Day/night (`ric`) polls exposure over the rvd
  socket and sets the running mode directly; no wake is needed because the
  FrameSource always runs.

Both therefore never produce runtime FrameSource edges, and neither exercises
the state timps' keepalive creates on the open stack (FrameSource on, video
encoder channel stopped) — see 5.2.

---

## 2. Change 1: the kick goes through the debounce (all SoCs)

`fsr_kick_begin()` in `src/hal/fs_ref.h`:

* **chn0 idle:** take one "kick" reference (a real `EnableChn` edge, which as
  before re-applies hflip/vflip/running_mode in `fs_edge_relatch()`), wait
  500 ms (unchanged), and **keep** the reference for another
  `MS_IDLE_STOP_US` (2 s). It is released by `fs_kick_reap()`, which every idle
  producer loop (video, sw-rot, jpeg) calls before blocking in `act_wait()`;
  those loops wake at least once a second. So chn0 is disabled 2-3 s after the
  kick instead of immediately.
* **chn0 already running:** take (or extend) the same hold, **no edge and no
  wait**. The brief asked for "do nothing" here. A pure no-op would reopen a
  small hole: if the only holder is an encoder thread in its own 2 s debounce
  tail, it could disable chn0 a few ms after the Set, before the ISP latched
  it. The old code covered that by holding chn0 500 ms; the hold covers it
  without blocking the caller.
* Repeated kicks share one reference and push its deadline out; a kick that
  ends after teardown re-arms nothing.

**Why vendor SoCs stay equivalent.** The latch semantic is unchanged: after
every idle-time Set chn0 still runs at least 500 ms (now ≥ 2.5 s), the enable
edge still re-applies flip/running_mode, and the kick still blocks its caller
500 ms when it had to enable chn0. What changes is only *when* chn0 is
disabled — 2-3 s later — and that a consumer arriving inside that window finds
chn0 running instead of re-enabling it (one pair fewer). Day/night re-asserts
8 s apart still produce one pair each on a camera with no consumer. The
callers (`ing_control()` running_mode, the batched flip kick in
`ing_control_commit()`, the boot kick at the end of `ing_start()`) are
untouched. The /control and day/night threads no longer block 500 ms when chn0
already streams.

Visible: `framesource 0 disabled (idle)` follows a kick 2-3 s later instead of
500 ms later; at start-up the boot kick and the first timelapse shot share one
enable.

---

## 3. Change 2: `general.fs_keepalive`

### 3.1 Behaviour

* `ing_start()`, after all video channels, the audio thread and the dedicated
  JPEG channel exist and **before** `motion_sync()` and the boot kick, resolves
  `general.fs_keepalive` and, when on, calls `fs_keep_acquire()` for every
  video stream's FrameSource in stream order (FS0 first) and for the dedicated
  `jpeg.*` channel's FrameSource. One enable edge per channel per start, before
  any client can connect (RTSP/HTTP start after `start()` returns).
* Every other holder — encoder threads, the JPEG piggyback, the motion pin,
  the latch kick — becomes a plain reference bump. With chn0 kept, the kick
  returns immediately (no hold, no wait).
* `fs_teardown()` drops the keepalive reference (and a pending kick hold)
  before it checks for leaked references and disables, so every path that
  tears a channel down (`ing_stop()`, the bring-up unwind, `sw_rot_teardown()`,
  the in-process start retry in `main.c`) releases it in the right order and
  without the `reference(s) still held at teardown` warning.
* Encoders stay on-demand exactly as before (`StartRecvPic` on the first
  consumer, `StopRecvPic` 2 s after the last), so encoder CPU is still only
  spent while someone consumes.
* The log says `framesource keepalive on (general.fs_keepalive=-1, auto)` (or
  `off`, or the explicit value) once per start. `GET /control` reports
  `caps.framesource = {"keepalive": <resolved 0|1>, "cfg": <-1|0|1>}`.

### 3.2 Exemptions and special paths

* **SW-rotate streams (T23 `USE_SW_ROTATE`, 90/270):** not kept. That path is
  unbound: its thread pulls frames itself with `GetFrame` after setting a frame
  depth of 2 on every enable. An enabled FrameSource with a depth set and no
  reader has never been run on that stack; whether it stalls, overwrites or
  logs is unknown. Such a stream keeps its old on-demand edges (logged as
  `framesource N: no keepalive (software-rotate stream)`). This leaves an edge
  source on T23 cameras with a rotated stream; revisit after measuring it.
* **Streams with `videoN.enabled = 0`:** never created, nothing to keep.
* **Motion pin:** unchanged, a reference bump under the keepalive.
* **JPEG cold-start recycle** (`MS_JPEG_COLD_STALL_US`, the open-stack T23 JPEG
  channel that stays empty until its FrameSource is cycled): when the
  FrameSource is kept, the recycle is an **encoder restart only**
  (`StopRecvPic`/`StartRecvPic`), never `fs_recycle()`. Log:
  `recycling the encoder (framesource kept alive)`. This is the minimal safe
  variant: it never creates the edge the keepalive exists to avoid. If the
  encoder still delivers nothing, the existing miss watchdog runs (its
  `fs_unuse()`/`fs_use()` pair is a reference bump under the keepalive, so it
  is an encoder restart too) and after `MS_JPEG_WATCHDOG_MAX_RECOVERIES` it
  gives up on that JPEG channel only; video is unaffected. Whether the
  "empty until recycled" fault happens at all with a warm FrameSource is
  unknown (M5).
* **Video watchdog** (encoder dead for ~5 s with a client attached): still
  calls `fs_recycle()`, a real Disable+Enable. It is an error path on an
  already broken pipeline whose next step is the give-up exit and reboot; not
  changed. This is the one remaining runtime edge.
* **Start/stop:** one enable per kept channel at start, one disable at stop.
  At stop chn0 is disabled while chn1 still runs (teardown order) — the same
  as any shutdown with clients connected before this change.

### 3.3 Day/night

`src/daynight.c` needs no change. In practice, with chn0 kept:

* every `running_mode` POST, switch and re-assert finds chn0 running: no kick
  edge, no 500 ms block in the day/night thread, the Set latches within a
  frame or two (assumption, M3);
* the ISP readback (`/proc/jz/isp/isp-m0` "ISP Runing Mode") should confirm a
  switch on the next tick, so the readback gate (`DN_VERIFY_MS`) should not
  force cycles and `readback_untrusted` should stay 0 — as it did whenever a
  client held chn0;
* **if** AE statistics only update while frames flow (unknown, M1), the
  automaton now measures live values instead of the value frozen at the last
  wake: smoothing/`stable_n`, the silent probe ratio (`r ~ 1` on frozen values)
  and the dusk/dawn paths A/C/T work without a client. The fleet mostly did not
  see the frozen case because Frigate holds ch0 on most cameras.
* the boot measurement and probes no longer depend on a 500 ms kick having
  converged AE.

The replay corpus (`scripts/dn-replay.py`) runs against the sim, which has no
FrameSource; it is unchanged by construction and was re-run as a regression
(38/38).

### 3.4 Default rule and why

`-1` resolves to **on** exactly when the build defines `USE_OPENIMP` and
`PLATFORM_T23` or `PLATFORM_T41` (`FS_KEEPALIVE_AUTO` in `fs_ref.h`), **off**
everywhere else: vendor libimp builds on every SoC, and T10/T20/T21/T30/T31
(and T40) with OpenIMP.

* **On where it is needed.** Those are the two stacks with a reproduced or
  observed hang. The goal there is to make the pattern unreachable, not rarer;
  only a permanently held FrameSource does that.
* **Off where the price is unmeasured.** On small-RAM SoCs (T10/T20/T21/T30
  and the T31 variants with 40-64 MB) the cost of a FrameSource that runs 24/7
  — CPU in libimp's worker threads, DDR bandwidth, heat, power, and the rmem a
  pool keeps instead of releasing — has never been measured, and those
  platforms show no hang. Changing their idle behaviour without data would
  trade a known-good state for an unknown one.
* **Opt-in elsewhere:** `general.fs_keepalive = 1` in `/etc/timps.conf` plus
  `/etc/init.d/S95timps restart`. On any SoC other than open-stack T23/T41 this
  is an explicit opt-in with the costs in section 4 unmeasured; it is the
  right experiment on a camera that shows the hang signature or whose
  day/night must work without a client (M1).
* **Off on T23/T41:** `general.fs_keepalive = 0` restores the on-demand
  behaviour of change 1 (still with the debounced kick).

The key is file-only (no `F_CTRL`): it is read once in `ing_start()`, and a
live toggle would itself create the edges it exists to prevent.

---

## 4. Costs

| | Value | Status |
| --- | --- | --- |
| rmem held for FS0 | 6 266 880 B (2 × 3 133 440 B, `nrVBs` 2, 1920×1080 NV12) | from a cam-B/cam-02 log line (`VBMCreatePool ... sizeimage=3133440`, `dev_notes/TODO.md`); now held permanently instead of only while someone consumes |
| rmem held for FS1 | ~0.7 MB (640×360 NV12 × 2 = 691 200 B) | design estimate, not logged |
| Peak rmem | unchanged | a camera with a client on both streams holds the same pools today |
| Pool create/destroy | once per start instead of once per wake | removes the vendor-named rmem fragmentation risk of repeated pool create/destroy |
| CPU | **unmeasured** | the historical "~19 % → ~0" is a mixed vendor-stack upper bound, not the FrameSource share on the open stack |
| OSD | **unknown** | if the OSD group blends every frame while the encoder is stopped, that is the measured OSD load (~8-9 points/core on T31) |
| DDR | ~78 MB/s computed (1080p25 NV12 writes for FS0) | arithmetic only |
| Heat / power | **unmeasured** | |

What is gained besides the hang: snapshots and timelapse shots come from a warm
pipeline (no cold-start recycle, AE converged, mode latched), `running_mode` and
flip POSTs answer without the 500 ms block, and day/night may measure without a
client.

---

## 5. Risks

1. **"FrameSource on, encoder channel stopped" is a new steady state on the
   open stack.** Neither prudynt nor raptor runs it (both keep their video
   encoders started). If OpenIMP back-pressures a bound FrameSource whose
   encoder channel is stopped, the first client after a long idle might see a
   stall, visible as `PollingStream idle` / `encoder dead ... forcing a
   framesource disable/enable cycle` (and that recovery is an edge).
   Unknown; M12.
2. **AE in that state** — whether the ISP updates AE statistics with frames
   flowing into a stopped encoder is unknown (M1). The `ae_it_max_us`
   precedent: a write into a pipeline that is enabled but not delivering is
   ignored (`hal_ingenic.c`, measured on T31). The supervisor is still driven
   by real encoded frames, so it is not made worse (M4).
3. **Start-up enables are still edges.** FS1 is enabled while FS0 already runs,
   once per start. That existed before; whether a single enable can trigger the
   driver fault, or only fast stop/restart sequences, is unknown.
4. **SW-rotate streams keep their edges** (3.2).
5. **The video watchdog recycle remains** as the one runtime edge, error path
   only (3.2).
6. **Vendor T41** (if anyone builds it) is not covered by auto; whether it has
   the same fault is unknown.

## 6. Rollback

* Per camera: `general.fs_keepalive = 0` in `/etc/timps.conf`, then
  `/etc/init.d/S95timps restart`. Verify `caps.framesource.keepalive` = 0 in
  `GET /control` and `framesource keepalive off (general.fs_keepalive=0)` in
  the log.
* Change 1 has no switch; it only moves a disable 2-3 s later and is not
  expected to need one. Reverting it is a revert of its commit.
* After the driver fix: keep auto = on for the open stack as long as M1 shows
  that without a running FrameSource nothing is measured; otherwise decide the
  default again with M6/M7 data.

## 7. Alternatives considered

* **B — grace period in the refcount** (`general.fs_idle_grace_s`, disable N
  seconds after the last user, live). Cheaper in idle (FrameSource only on
  during the grace) and live-tunable, but the hang pattern stays reachable:
  grace expires, `DisableChn`, a client arrives within 200 ms, `EnableChn`. Rarer,
  not impossible, and long soaks would eventually hit it. With timelapse at 60 s
  and a grace above that it degenerates into A anyway. Without timelapse the
  day/night freshness problem stays. Kept as the fallback if M6 shows A's idle
  cost is unacceptable, with grace ≥ `timelapse.interval_s` + 15.
* **C — lazy apply plus windowed day/night measurement** (kick only if some
  FrameSource runs, latch at the next real edge; day/night requests its own
  measurement windows and marks stale samples). Rejected: it does not touch the
  dominant edge source (timelapse stays at 1440 pairs/day), measurement windows
  *add* edges (~200 pairs/day without timelapse at a 600 s window), and it
  rewrites a day/night automaton that has 38 regression scenarios and a long
  incident history, with an unknown readback semantic on open-tx-isp, a desync
  window between IR-cut and ISP mode (the purple image), and continuous-sample
  calibrated trend constants.
* **Do nothing until the driver fix.** Rejected: the fix's timeline is open,
  and the hang takes a camera off the network until someone power-cycles it.

## 8. What still has to be measured (before extending the default)

From the design's measurement list, plus M12 found while implementing. Run on
cam-B (T23) and the T41 first, supervised, with the kernel log collected
centrally.

* **M1 AE freshness.** No client, timelapse and motion off. Log every second
  `/proc/jz/isp/isp-m0` (integration time / total gain) and the `GET /control`
  day/night status while changing the light (cover the lens / lamp). Three
  states: FS0 off, FS0 on without an encoder consumer (keepalive), only FS1 on
  (motion pin). In parallel the ISP IRQ count delta in `/proc/interrupts` per
  10 s.
* **M2** `/proc` "ISP Runing Mode" after a Set without kick with FS0 off: does
  it show the requested or the latched mode?
* **M3** running_mode and hflip/vflip latch with FS0 permanently on and no
  consumer (keepalive state): `/proc` plus a look at the ch1 image in IR.
* **M4** `image.ae_it_max_us` supervisor under keepalive: still needs a real
  consumer (expected), must not get worse.
* **M5** how often `jpeg chnN: no frame ... recycling the encoder (framesource
  kept alive)` still appears (JPEG cold start with a warm FrameSource), and
  whether the channel then delivers.
* **M6** idle CPU keepalive vs on-demand with the nice-19 spinner method (not
  `/proc` ticks), OSD on and off; SoC temperature if there is a sensor; power
  with a USB meter.
* **M7** free rmem idle and with all clients: the peak must fit and should be
  unchanged.
* **M8** count of `framesource N enabled/disabled/recycled` lines per hour
  before/after, plus the kernel log for ISP/MSCA errors.
* **M9** soak ≥ 48 h on cam-B under keepalive with timelapse 60 s, day/night
  transitions, RTSP client churn on ch0 and ch1 and snapshot polling every
  10 s; plus the T41 repro (client churn on one channel while the other runs):
  under keepalive it must not produce a FrameSource edge any more.
* **M10** snapshot latency and exposure/mode of the first JPEG, keepalive vs
  on-demand, at dusk.
* **M11** (log only, now): does cam-B log `boot: silent measurement is not a
  clean night (r=1.0x)`? That would point at frozen AE during the silent probe.
* **M12** (new) time to first frame and any `PollingStream idle` / watchdog /
  recycle lines for a client that connects after ≥ 1 h of idle under the
  keepalive, ch0 and ch1, on both open-stack SoCs (risk 5.1).

Extending auto to another SoC needs at least M1, M6, M7 and M12 on that SoC.
