# Day/Night Detection – Enumeration of Alternatives (Design Exploration)

**Date:** 2026-08-07
**Code baseline:** `074e8f5` (v1.7.8), `src/daynight.c` = 1322 lines
**Character:** pure option-space mapping, **no recommendation, no code, no patch.** Basis for a possible later redesign decision by a human.
**Verification:** against the actual code (`src/daynight.c` read in full), `src/hal/hal.h` (available signal APIs), `docs/sdk-feature-gaps.md` (platform availability matrix across all 9 SoCs), `docs/wiki/Configuration-Reference.md` (daynight keys), and the CHANGELOG history v1.7.3-v1.7.8 (why each hardening step was introduced).

---

## 0. Starting Point: What Exists Today and WHY

The current state is a background thread (`dn_thread`) that polls at `interval_ms` (default 500 ms) and knows three operating modes:

- **`sensor`** (default): primarily **ISP total_gain** via `hal_isp_total_gain()` (IMP `GetTotalGain`), fallback = `/proc/jz/isp/isp-m0` scrape (integration time + gains + brightness). High gain = dark = night. The wide dead-zone gap `total_gain_day_threshold`..`night_threshold` (300..3000) is the hysteresis.
- **`time`**: pure wall clock, no sensor, **no probe**.
- **`sun`**: sun-position calculation (NOAA/Meeus) from lat/lon + offsets, no sensor, **no probe**.

All the complexity in `sensor` mode exists because of **one** basic physical fact that has to be kept in mind constantly when comparing the alternatives:

> **The `sensor` mode's only ambient-light reading (gain/luma) is measured THROUGH the currently active optical+tuning pipeline.** In the night state, the IR-cut filter is removed, the IR LEDs are on, and the night AE table is active. A dark room therefore looks "bright" to the sensor (IR light reaches the sensor). The gain reading on the night path is thus **not a reliable proxy for "is it actually day"**.

This necessarily leads to the **probe**: to find out whether it's really day, the camera must briefly be switched physically into the day configuration (IR-cut swung in, IR LEDs off, color pipeline), read a genuine daylight value, and switch back if needed. **This physical probe click (mechanical IR-cut relay, audible; plus ~7-9 s of washed-out color video) is the central source of user annoyance.** The entire machinery — exponential backoff, brightening margin, failure ratchet, passive-evidence skip, the `probe_max_skip_s` outer bound, oscillation breaker, baseline drift — exists solely to minimize the **frequency** of this click **without** losing the self-healing property (if the camera gets stuck in the wrong mode, it corrects itself). Every fix was driven by real fleet incidents (v1.7.3 flap loop, v1.7.4 probe economy, v1.7.5 dead-zone adoption + oscillation breaker, v1.7.6 silent limbo, v1.7.8 `probe_max_skip_s` made configurable).

The memory footprint is already **tiny**: a few scalars plus three small ring buffers (`hist[10]`, `settle_hist[6]`, `osc_hist[3]` floats). There is **no** large buffer to reclaim. The 1322 lines are almost entirely logic plus extensive incident comments, **not** data structures.

---

## Part A — Alternative MEASUREMENT SIGNALS (replacements for the ISP total_gain reading itself)

This section lists **only the decision signal**, independent of the control algorithm (Part B). Core question for each: *Is the signal downstream of the IR pipeline (→ still needs a probe) or independent of it (→ probe-free possible)?*

### A1 — ISP total_gain (current state, reference)
- **Signal:** `hal_isp_total_gain()` / `/proc` scrape of the gains.
- **Platform:** IMP API on all except T40/T41 + Sim; there the `/proc` scrape fallback kicks in. Effectively usable fleet-wide.
- **Pipeline-dependent?** YES. → Probe unavoidable for night→day.
- **Complexity/RAM/CPU:** Reference. Very cheap (one API call/tick, scrape throttled to `DN_SCRAPE_MS`).
- **Weakness:** AGC noise in the dark (IR AGC hunting) → is the root of the entire smoothing/ratchet apparatus.

### A2 — AGC/exposure time (integration time) instead of gain
- **Signal:** `SENSOR Integration Time` / `Max Integration Time` from the same `/proc` scrape (today used only for the brightness fallback). Idea: **exposure time saturates at its maximum in darkness BEFORE gain ramps up** – the combined "total exposure" (integration time x gain) is a more monotonic light proxy that is **less noisy** at the dark end than gain alone.
- **Platform:** integration time is broadly available in the `/proc` dump; the new tuning APIs (`GetIntegrationTime`) are, per the feature gaps doc, present on older chips (T20/T21/T30).
- **Pipeline-dependent?** YES (same optics/AE as A1). → Probe remains.
- **Complexity/RAM/CPU:** ~the same; possibly **less** smoothing code needed because the signal is quieter. Could reduce `DN_SMOOTH_ALPHA`/ratchet overhead.
- **Loss:** nothing fundamental; does NOT resolve the probe question. A marginal but honest robustness gain at the noise source.

### A3 — AE luma (ISP target brightness)
- **Signal:** `hal_isp_ae_luma()` (raptor's `ae_luma`).
- **Platform:** **only T21/T23/T31/C100** (`IMP_ISP_Tuning_GetAeLuma`); missing on T20/T10/T30/T40/T41 → **not fleet-wide**.
- **Pipeline-dependent?** YES – and worse: AE luma is **regulated to a setpoint** by the AE loop, so in steady state it is ~constant regardless of ambient light. As a **primary signal it is worse** than gain; at best usable as a secondary confirmation.
- **Conclusion:** not a candidate as a primary signal; probe remains.

### A4 — Frame luma / luma histogram from an actual video frame
- **Signal:** `IMP_FrameSource_SnapFrame` (per the feature gaps doc **available on all 9 platforms**) → grab a real YUV frame, compute a luma histogram. Richer than the single AE luma scalar (distinguishes "uniformly dark" from "bright with dark regions").
- **Platform:** universal (SnapFrame everywhere).
- **Pipeline-dependent?** YES – the frame is the output of the same pipeline. In night mode it is already mono/IR-illuminated; a histogram derived from it is just as poor a daylight proxy as A1. → Probe remains for night→day.
- **Complexity/RAM/CPU:** **significantly MORE** – frame grab (large buffer, VBM/rmem cost), scanning the Y plane (per-tick CPU over tens of thousands of pixels), 256-bin histogram. The first genuine increase in memory usage across the whole comparison.
- **Loss/gain:** no probe advantage; expensive. Only sensible as an add-on feature (see A6).

### A5 — IR-cut relay feedback (hardware feedback pin)
- **Signal:** a GPIO that reads back the **physical IR-cut filter position**.
- **Platform:** **does not exist in the timps HAL.** Mode switching today is fire-and-forget via an external board script (`switch_cmd day|night`); there is no sense line and no HAL function for it. Thingino boards drive the IR-cut via GPIO but generally do not expose a readback. → practically **unavailable**.
- **What it would solve:** only the question "is the filter where I think it is?" (could replace the `DN_REASSERT` safety net) – **NOT** the question "is it day outside?". So it does not solve the probe problem at all.
- **Conclusion:** irrelevant to the core question; and unavailable on the hardware side to begin with.

### A6 — Color saturation / chroma energy of the frame
- **Signal:** SnapFrame → measure U/V chroma energy. In genuine daylight the color pipeline produces real color (high chroma); in an actually dark, IR-illuminated scene, the image is nearly grayscale even on the day path (chroma ≈ 0).
- **Platform:** universal (SnapFrame).
- **Pipeline-dependent?** PARTIALLY. Useful mainly for the **day→night direction** (detects a "washed-out/colorless day image" → should be night). **Useless for night→day**: in night mode the stream is mono anyway, chroma is always ~0 – "night pipeline in daylight" cannot be distinguished from "night pipeline in the dark" via chroma. → night→day probe remains.
- **Complexity/RAM/CPU:** MORE (frame grab + UV scan), like A4.
- **Loss:** no full probe replacement; only a half-signal.

### A7 — Independent ambient-light sensor (LDR/ALS on ADC/I2C)
- **Signal:** a separate photoresistor/ALS whose reading does **NOT** sit behind the IR pipeline. Many Thingino cameras have an LDR on an ADC/GPIO pin that the board's own daynight scripts read.
- **Platform:** **board-dependent, NOT universal** – no uniform HAL access today; an ADC read would have to be implemented per board. Not possible at all on boards without an LDR.
- **Pipeline-dependent?** **NO** – this is the crucial point: a pipeline-independent ambient-light signal.
- **Probe?** **Eliminated entirely** – ambient light is measured directly, independent of the current mode. This removes the entire backoff/ratchet/skip machinery.
- **Complexity/RAM/CPU:** the decision core becomes **much simpler** (a plain Schmitt trigger on the LDR value); in exchange, new HAL code is needed per board plus LDR-to-threshold calibration.
- **Loss:** no more self-healing/oscillation special cases needed (the LDR does not lie about IR). BUT: availability is not guaranteed → could only be deployed as a **preferred signal WITH a gain fallback**, never as a sole fleet-wide replacement.

### A8 — External time/sun source as a "signal" (no sensor)
- **Signal:** wall clock (`time`) or sun position (`sun`) – both already implemented.
- **Pipeline-dependent?** N/A – **entirely sensor-independent.**
- **Probe?** **Never needed.** Self-healing comes automatically from the next time/sun edge.
- **Loss:** blind to actual light – a windowless interior/cloud cover/interior artificial light/obstruction are all ignored. (Details under B1/B2/B3.)

### A9 — "Shadow read": briefly turn off the IR LEDs only, do NOT move the IR-cut
- **Signal:** not a new sensor signal, but a **cheaper probe substitute**. If the board can switch the IR LEDs independently of the mechanical IR-cut, only the IR illumination is killed briefly (while staying on the night path): in a genuinely dark room, the gain keeps climbing (confirms darkness); in an actually bright room, it stays moderate (visible light is present).
- **Platform:** **board-dependent** – requires separate LED control in the `switch_cmd` script.
- **Probe?** Replaces the **audible relay click** with silent LED switching – the click disappears, the brief image disturbance (darker image for 1-2 s) remains.
- **Complexity:** ~the same as today, but an additional board-script contract is needed.
- **Loss:** no robustness loss; potentially the **biggest usability gain** for the smallest rework – but only where the hardware/script cooperates.

---

## Part B — Alternative ARCHITECTURES / ALGORITHMS

### B1 — Sun position only (`sun`), sensor fully off
- **Core idea:** the astronomical calculation decides alone; no sensor, no probe. Already exists.
- **Signal:** A8 (sun position).
- **Probe?** Never.
- **Code:** **massively less** – the entire sensor block, baseline, probes, oscillation, boot-settle would go away (only `dn_sun_times`/`dn_sun_target` + the switch machinery remain). Roughly <200 instead of 1322 lines.
- **RAM/CPU:** minimal (one calculation/tick, no `/proc`, no IMP call).
- **Loss:** a windowless interior/room, dense cloud cover, an obstructed camera, an interior room with artificial light → all get it wrong. No reacting to actual light. Self-healing is trivial (the clock).

### B2 — Time of day only (`time`)
- Like B1, even simpler (no astronomical math), but without seasonal adjustment. Same losses, no probe.

### B3 — Sun/time as PRIMARY, sensor only as a bounded correction (hybrid)
- **Core idea:** the sun/time schedule determines the base state. The sensor may only correct **within a window**: e.g., "force night if the room goes dark during the scheduled day" or fine-tune near twilight. Day is **always** re-entered at the scheduled sunrise edge, regardless of what the sensor says.
- **Signal:** A8 primary + A1 secondary.
- **Probe?** **No periodic reconfirmation probe needed** – the clock provides the self-healing: an incorrect night latch resolves itself at the latest at the next sun edge, without ever probing. A probe would at most be optional for a faster day correction in the middle of the night (rare).
- **Code:** **significantly less** than today – backoff/ratchet/skip/`probe_max_skip_s` largely fall away because the clock solves the "stuck forever" problem. Baseline/oscillation are only relevant within the narrow correction window.
- **RAM/CPU:** ~same as today or lower.
- **Loss:** no longer reacts fully freely to arbitrary light (e.g., a room that is darkened at 14:00 and lit again at 15:00 → may be sluggish within the narrow window). A windowless interior without meaningful geocoordinates remains problematic. **The most attractive low-probe architecture that largely retains sensor reactivity.**

### B4 — Fixed dual threshold / Schmitt trigger, NO adaptive baseline, NO probe
- **Core idea:** what `daynight.c` was before all the hardening: fixed `day`/`night` gain thresholds, a wide dead zone, no baseline drift, no probe, no smoothing apparatus.
- **Signal:** A1 (or A2).
- **Probe?** **None.**
- **Code:** **drastically less** (roughly 200-300 lines).
- **RAM/CPU:** minimal.
- **Loss – substantial and concrete:** (1) the `day_gain_pct` adaptive baseline is gone → rooms with weak artificial light that never drops below the fixed threshold stay stuck in night forever (exactly the two incidents from 2026-08-02). (2) **No self-healing** – a night latch that, because of the pipeline problem (A0), never gets below the day threshold stays night forever (the dead-zone/silent-limbo class returns). (3) IR-reflection oscillation unchecked. Answers Q2 with "yes, without a probe" – but at the price of the robustness that was bought through incidents.

### B5 — Control-theoretic estimator (low-pass/Kalman on log-light) instead of threshold+hysteresis
- **Core idea:** replace the ad-hoc smoothing zoo (EMA `smooth_tg`, baseline drift, boot-settle stability window, hysteresis candidate) with **one** principled estimator that maintains an ambient-light estimate with variance and switches when the estimate crosses a threshold with confidence.
- **Signal:** A1/A2 (the gain reading, unchanged).
- **Probe?** **Remains** – the estimator does not change the measurement problem (A0), only the filtering.
- **Code:** the decision **core** could become more compact/elegant (boot-settle + smoothing + hysteresis unified in one filter). BUT: once the self-healing probe + oscillation breaker + passive skip are bolted back on (they remain necessary), the net LOC savings are a wash. Risk: the special cases tuned precisely to past incidents get lost in the "cleaner" model and have to be fought for again.
- **RAM/CPU:** ~the same (everything is already float anyway; MIPS without an FPU is not a blocker, since the current code already floats).
- **Loss:** potential regression risk in the fine details; no functional gain.

### B6 — Frame-content classifier (chroma + luma histogram + gain, hand-tuned)
- **Core idea:** combine a SnapFrame thumbnail + several features (A4+A6) into a more robust day/night classification.
- **Signal:** A4+A6 (+A1).
- **Probe?** **Remains** for night→day (the A0/A6 argument).
- **Code/RAM/CPU:** **MORE** on every axis (frame grab + multi-feature per tick).
- **Loss/gain:** no probe advantage at higher cost. Not worthwhile.

### B7 — Delegation to the OS (thingino `daynightd` / board sensor)
- **Core idea:** timps does no detection of its own, it only consumes the mode set by the OS.
- **Probe?** None from timps' point of view (shifted elsewhere).
- **Code:** the least in timps.
- **Loss:** timps loses integration/control and duplicates exactly what the native port moved away from (the `daynightd` formula was deliberately brought in-house). This is delegation rather than an algorithm.

---

## Part C — Direct Answer to the Two Guiding Questions

### Question 1: Is there an alternative that MEANINGFULLY SIMPLIFIES the code or SIGNIFICANTLY SAVES MEMORY, without losing much real functionality?

**Memory: practically no – there is nothing to save.** The current footprint is already negligible (a few scalars plus three tiny ring buffers, `hist[10]`/`settle_hist[6]`/`osc_hist[3]`). No candidate saves meaningful RAM; the frame-based signals (A4/A6/B6) are the only ones that would even **increase** consumption (SnapFrame buffers). "Significantly saving memory" is simply a non-goal given the real current state.

**Code size: yes, but only by giving up robustness bought through incidents.** B4 (fixed Schmitt trigger) and B1/B2 (sun/time only) are dramatically shorter – but sacrifice self-healing, dim-room recovery, and oscillation protection, i.e. exactly the functionality the lines exist for. **The only candidate that genuinely simplifies AND loses little real functionality is B3 (sun/time primary, sensor as a bounded correction):** the clock takes over self-healing, which lets backoff/ratchet/skip/`probe_max_skip_s` largely fall away – at the price of reduced free light reactivity and the need for meaningful geodata. B5 (Kalman/low-pass) makes the core more elegant, but is net a LOC wash and purely a refactor without a functional gain.

### Question 2: Is there an alternative that works WITHOUT any probe interval (no periodic forced physical mode switch)?

**Yes – but only if the decision signal comes from a source that is NOT downstream of the IR pipeline.** That is the crux: `sensor` mode necessarily needs the probe because its only signal (gain/luma, likewise frame luma/histogram/chroma) is measured through the active night optics, and a dark room looks "bright" within it. Probe-free options:

1. **Sun/time** (B1/B2, already implemented) – zero probe, but blind to actual light (windowless interior/cloud cover/interior spaces).
2. **Sun/time primary + sensor correction** (B3) – **the most attractive probe-free architecture that retains sensor reactivity:** an incorrect latch resolves itself at the next sun edge without ever probing; a room that goes dark can be forced directly to night.
3. **Independent ambient-light sensor (LDR/ALS, A7)** – the cleanest solution: a pipeline-independent signal → no probe, no backoff apparatus – but **not available fleet-wide** (board-dependent, no HAL access today), so conceivable only as a preferred signal with a gain fallback.
4. **Fixed dual threshold (B4)** – no probe, but loses self-healing and dim-room recovery.

**Not achievable probe-free**, on the other hand, are all purely image-/gain-based approaches (A1/A2/A4/A6, B5/B6): they all measure through the same pipeline and cannot distinguish "night pipeline in daylight" from "night pipeline in the dark" without physically switching to the day path. The cheapest compromise without a real signal change is **A9 (shadow read: briefly turn off the IR LEDs only)** – this eliminates the audible relay click (not the brief image disturbance) and only requires separate LED control on the board side.
