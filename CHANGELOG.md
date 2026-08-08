# Changelog

All notable changes to timps are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/), and the project aims to follow
semantic versioning.

## [Unreleased]

### Added
- **Fatal-signal handler for SIGSEGV/SIGBUS/SIGFPE/SIGABRT (`main.c`)**, closing
  the gap flagged by a crash-surface research pass: timpsd links closed-source
  Ingenic vendor libraries (`libimp.so`, `libaudioProcess.so`) with documented
  crash modes already noted elsewhere in this codebase's own hardening
  comments (a `libaudioProcess.so` UAF on channel-teardown races, a
  `libimp`/encoder div-by-zero SIGFPE from a bad QP, ...). Previously a hit
  took the whole process down silently — only SIGINT/SIGTERM/SIGPIPE had
  handlers, and there is no core-dump story on this embedded target, so a
  vendor-library crash left literally zero diagnostic trail. The new handler
  installs via `sigaltstack()` (so it still runs on a stack-overflow fault)
  and, on any of the four signals, writes to both stderr and a new
  `/run/timps.crash` flat file (same `/run` convention as the singleton lock
  and `/control` token file — survives the dying process for a respawn
  supervisor to pick up, since the init script normally backgrounds timpsd
  and its stderr is otherwise discarded): the signal number, `siginfo_t`'s
  faulting address, the faulting PC (MIPS o32 `uc_mcontext.pc` — confirmed
  against this project's actual T31/T23 cross-toolchain `sys/ucontext.h`,
  not assumed), and a `/proc/self/maps` dump plus a classification of
  whether the fault address falls inside `libimp.so`, `libaudioProcess.so`,
  or timps's own code — the single most useful post-mortem fact for triage.
  Entirely hand-rolled on `open`/`read`/`write`/`close`/`signal`/`raise`/
  `sigprocmask` (no malloc, no `LOGx` — `log.c`'s `log_printf()` uses
  `vsnprintf`/a `pthread_mutex`/`syslog()`, none async-signal-safe). Never
  attempts to continue past the fault: restores the signal's default
  disposition, unblocks it, and re-raises so the kernel does its normal
  thing; `_exit()` is a last-resort fallback that should never execute.
  Verified with a standalone test harness (real SIGSEGV/SIGFPE/SIGABRT
  triggers) confirming the handler fires, logs correctly, classifies the
  fault address, and the process actually terminates (128+signum exit,
  core dumped) rather than looping or hanging; cross-compiled `main.c`
  clean against both the real T31 and T23 toolchains to confirm the MIPS
  `ucontext_t` field access. Adds ~2.8 KB `.text` and ~80 KB `.bss` (the
  altstack + a static `/proc/self/maps` scratch buffer — RAM only, not
  flash-resident) to the target binary.
- **`_Static_assert(MOTION_MAX_CELLS <= MOTION_STATUS_MAX, ...)` in
  `imp_motion.c`**: `MOTION_MAX_CELLS` is taken straight from whichever IMP
  SDK header a build compiles against (52 on most, 4 on the old T10/T20
  3.9.0 SDK), while every status array in `imp_motion.h` is fixed at
  `MOTION_STATUS_MAX` (64) slots. A future SDK bump that raised
  `IMP_IVS_MOVE_MAX_ROI_CNT` past 64 would have silently turned the
  existing runtime clamps into an out-of-bounds write instead of failing to
  build; this makes that a compile error instead.

### Changed
- **`GET /control`'s `caps.image`/`caps.audio` no longer hand-list field
  names in a second place either** (the caps-list half of the follow-on
  architecture finding flagged alongside the `F_CTRL` consolidation above):
  `control.c`'s `IMG_CAPS[]`/`AUD_CAPS[]` arrays re-typed every `image.*`/
  `audio.*` key name a third time, under their own copy of `isp_caps.h`'s/
  `audio_caps.h`'s `#ifdef` platform gates — the same "typed three times,
  one gets forgotten" shape as the field-name duplication `F_CTRL` just
  fixed, just in the capability-advertisement layer instead of the
  POST-apply layer. Added a second flag, `F_CAP`, to `cfg_field` (alongside
  `F_NOGET`/`F_ATOMIC`/`F_CTRL`): `config.c`'s `image_fields[]`/
  `audio_fields[]` tables now carry it directly, gated by the exact same
  `ISP_HAS_*`/`AUDIO_HAS_*`/`USE_PLAY`/`USE_BACKCHANNEL` macros the old
  arrays used, and `control_get_json()` builds `caps.image`/`caps.audio` by
  walking `cfg_fields_image()`/`cfg_fields_audio()` for entries carrying
  **both** `F_CTRL` and `F_CAP`, instead of the removed `IMG_CAPS[]`/
  `AUD_CAPS[]` arrays.
  - `F_CAP` is a **separate axis from `F_CTRL`, not a refinement of it**:
    every `image.*`/`audio.*` field is `F_CTRL` (POST-able) on every build,
    but `F_CAP` is compile-time platform/feature gated. For `image.*` it is
    a pure hardware-capability gate (identical to `isp_apply_image()`'s own
    guards in `hal_ingenic.c`). For `audio.*` it is narrower still: several
    fully POST-able, fully persisted fields (`codec`, `samplerate`,
    `bitrate`, `channels`, `enabled`, `force_stereo`, `spk_enabled`,
    `backchannel`/`_codec`/`_rate`, `high_pass`, `agc`, `ns`,
    `agc_target_dbfs`, `agc_compression_db`) deliberately never get `F_CAP`
    on **any** platform — not because the hardware lacks them, but because
    they are restart-required or persist-only (libimp runs several of them
    on its own vendor thread and frees state unlocked, so a live toggle
    would race it), matching the old `AUD_CAPS[]` array's explicit
    exclusions exactly.
  - Verified by building a standalone harness against `config.c`'s
    `cfg_fields_image()`/`cfg_fields_audio()` accessors under every
    `PLATFORM_T10`/`T21`/`T23`/`T31`/`T40`/`T41` macro (with and without
    `USE_PLAY`) and diffing the emitted name **set** against the old
    hand-written arrays' `#ifdef` conditions by hand for each platform:
    identical for every platform tested. The only difference found is
    cosmetic — `caps.audio`'s JSON array now emits `mute` before
    `spk_volume`/`spk_gain`/`aec` (table order) instead of after (the old
    array's order); the array is a membership list, not positionally
    consumed, so this reorders nothing observable. `make sim` builds clean
    with the new `F_CAP`-gated tables.
  - **Left alone, deliberately**: `src/hal/hal_ingenic.c`'s `isp_apply_image`/
    `ai_apply_key` strcmp dispatch chains still hand-list field names a
    third(now fourth) way, to route each one to its actual
    `IMP_ISP_Tuning_Set*`/`IMP_AI_Set*` call. Unifying that would need a
    real per-field dispatch mechanism (function pointers or tags covering
    genuinely different SDK calls and value transforms per field), which is
    a materially bigger, riskier change than relocating an existing,
    already-correct set of `#ifdef` conditions — not something to invent
    and land unreviewed in the same pass as this consolidation. The
    duplication risk there is unchanged: a future new image/audio field
    still needs a human to remember to wire it into `hal_ingenic.c` by
    hand.

- **`src/control.c`'s `/control` POST handling no longer hand-lists field
  names in a second place** (architecture-audit catch): 11 hand-written
  string arrays (`IMG`, `AUD_LIVE`, `AUD_REST`, `OSD`, `OSD_GLOBAL_KEYS`,
  `VID_REST`, `SENSOR`, `DN_KEYS`, `MOTION_KEYS`, `MOTION_RESTART_KEYS`,
  `REC_KEYS`, `TL_KEYS`, `PRIV_KEYS`) re-listed ~100 field names already
  present in `src/config.c`'s `cfg_field` tables — the confirmed root cause
  of the "nine orphaned fields" bug fixed just above (a field added to
  config.c's table but forgotten in control.c's matching array silently
  never applied over HTTP). Added one flag, `F_CTRL`, to `cfg_field`
  (alongside the existing `F_NOGET`/`F_ATOMIC`); config.c's tables are now
  the single source of truth for "is this field POST-able", exposed to
  control.c via eleven `cfg_fields_*()` accessors (image/audio/sensor/osd/
  osd_item/motion/record/timelapse/daynight/video/privacy). control.c's new
  generic `apply_ctrl_fields()` walks a section's table and applies any
  entry present in the POSTed JSON with `F_CTRL` set, reusing
  `timps_apply_setting()` exactly as before — the live-apply funnel itself
  is untouched.
  - `F_CTRL` is a deliberate, mandatory-per-field **security allowlist**,
    not a walk-everything default: fields the arrays never exposed keep no
    `F_CTRL` and stay silently unreachable via POST, matching prior
    behavior exactly — `motion.on_motion`/`cooldown_ms` (fork()+execlp()
    hook / re-exec floor), `daynight.switch_cmd`/`isp_path` (exec'd
    command / scraped proc path), every `rtsp.*`/`http.*` credential and
    token, the `videoN.imp_chn`/`jpeg`/`jpeg_quality`/`jpeg_fps`/`jpeg_chn`
    internal channel-wiring fields, and the OSD item's `logo`/`logo_w`/
    `logo_h`/`font_path` (never in the old per-item `OSD[]` array either).
  - Genuinely special cases stayed hand-written, unchanged: `speaker.play`/
    `stop` (transient actions), `record.active`/`record.clip` (actions),
    the legacy `force_mode` alias, `daynight.mode`'s reject-on-garbage
    validation (deliberately stricter than `config_apply_kv`'s own
    coerce-to-sensor-and-persist), and the nested/indexed structural loops
    for `osd`/`osd<S>`/`video`/`privacy` (each loop now calls the generic
    per-field walker once it has located the right sub-object/sub-table,
    rather than the top-level dispatch itself being generic across nesting
    shapes). The `osd.enabled`/`motion.enabled`/`daynight.enabled` and
    `daynight.time_night_start`/`time_day_start` hand-written special cases
    were removed as genuinely redundant (their truthy-string pre-conversion
    duplicated what `pbool()`/`copystr()` already do) rather than kept.
  - Verified against `timpsd-sim`: representative fields from each of the
    11 old arrays still apply/persist/round-trip identically; all 13
    fields fixed by the immediately-preceding "nine orphaned entries + osd
    item type" commit still work; a nested/indexed case (`osd0.0.type`)
    still applies; and every security-excluded field above still returns
    `{"ok":true}` with **no** `[CTRL] set` log line and **no** config-file
    write, confirmed both by daemon log inspection and a `GET /control`
    diff. Cross-compiled clean for T31 against real Ingenic headers/libs
    (zero new warnings); the resulting binary is 1312 bytes smaller
    (`.text` -940 B, `.data` -372 B, `.bss` unchanged) than before this
    change on an otherwise-identical build.

### Fixed
- **Boot-order gap left over from the 7th rotation-effective-dims site fix
  (`cb4c7de`)**: independent review of that fix found it correct but
  incomplete. `imp_osd.c`'s `osd_rotated()` asks the hub for a stream's
  ACTUAL post-refusal dims to tell a genuinely-applied 90/270 rotation apart
  from one refused by the T23 SW-rotate / T31 FS-rotate safe-envelope check,
  falling back to the raw (pre-refusal) computation only if the hub isn't
  populated yet for that stream — documented as "shouldn't happen in
  practice" because the OSD refresh thread only starts after every stream's
  `hub_set_video_params()` call. True for the refresh thread, but
  `hal_ingenic.c`'s `ing_start()` also calls `imp_osd_setup()` itself
  **before** that same stream's `hub_set_video_params()` call, and
  `imp_osd_setup()` performs the first OSD text/logo/cover render
  synchronously. So on every boot the hub-not-populated-yet fallback fired
  for that first render, and on a *refused* rotation the boot-time render
  used the pre-refusal (wrong) `rotated`/`hlim` — the exact oversized-bound
  scenario `cb4c7de` targeted — for one render pass. Text self-heals via the
  OSD updater (~1 s later); privacy covers are pre-clamped to width/height
  regardless (harmless); a boot-configured **logo** with `logo_h` in
  `(height, width]`, though, would pass the defeated H5 size check in
  `setup_logo()` and never get re-checked until a later `/control` write.
  Fixed by reordering `ing_start()` (`src/hal/hal_ingenic.c`) so
  `hub_set_video_params(i,...)` for a stream is published as soon as its
  effective (post-refusal) dims are known — right after
  `ms_vstream_eff_dims()` — instead of after `imp_osd_setup()` and its
  binds; `imp_osd_setup()`'s first render for that stream now always sees
  the correct, final `osd_rotated()` answer instead of the stale fallback.
  Confirmed safe: `hub_set_video_params()` is a pure mutex-protected struct
  store with no dependency on OSD/bind state, the published dims are
  already final at the new call site (computed post rotation-refusal
  retarget), and nothing reads the hub for that stream until after
  `ing_start()` returns. No behavior change for the accept path (dims are
  identical either way) or for text/covers (already self-healing/harmless);
  only the boot-logo edge case is closed. `imp_osd.c`'s `osd_rotated()`
  comment updated to match. Cross-compiled clean (zero new warnings) for
  T31 (`USE_ROTATE=1`) and T23 (`USE_ROTATE=1 USE_SW_ROTATE=1`) against
  their real Ingenic headers/libs.
- **Nine `cfg_field` table entries were silently unreachable via `POST
  /control`** (audit catch, not intentional design like their neighbors):
  each existed as a real, validated, clampable entry in `src/config.c` and
  was correctly settable by hand-editing `/etc/timps.conf`, but the
  hand-written per-section JSON handlers in `src/control.c` were never
  extended to cover them — a POST returned `{"ok":true}` with zero effect
  (no `[CTRL] set` log line, no config-file write, no live apply). None of
  the nine carry an `F_NOGET`/exclusion marker, unlike the fields that
  *are* deliberately excluded in the same tables (`motion.on_motion`,
  `motion.cooldown_ms`, `daynight.switch_cmd`, `daynight.isp_path` — all
  stay file-only, unchanged by this fix).
  - `osd.monitor_stream`/`font_path`/`vars_file`/`supersample`/`hinting`:
    added to the `osd` section's master-switch handler (same
    restart-required class as the already-POST-able `osd.enabled` —
    `imp_osd_setup()` only builds the OSD groups once at startup) and to
    `GET /control`'s `osd` object.
  - `osd<S>.<N>.type` (text vs. logo): added to the per-item POST key list
    (it was already GET-readable and every sibling field — `text`/`x`/`y`/
    `font_size`/`color`/etc. — was already POST-able). Persists and is
    pushed through the existing live-reapply call, but `imp_osd_apply()`'s
    render dispatch is fixed at region-creation time, so switching an
    existing item's type needs a restart to actually change what's drawn
    — same restart-required class as `enabled`.
  - `daynight.transition_s`/`interval_ms`/`threshold_low`/`threshold_high`/
    `hysteresis`: added to the `daynight` section's key list and to its
    `GET`/`/events` JSON — same live class as every other numeric
    `daynight.*` key (the detection thread polls `g_cfg` directly, no HAL
    call involved).
  - `motion.hold_ms`/`skip_frames`: added to the `motion` section's key
    list and to its `GET`/`/events` JSON. Unlike their already-POST-able
    neighbors (`enabled`/`cols`/`rows`/`sensitivity`/`monitor_stream`,
    which trigger a live IVS grid rebuild), these two only feed
    `IMP_IVS_MoveParam`/`g_hold_ms` at grid *create* time
    (`imp_motion.c`), so a POST persists and takes effect at the next
    such rebuild or a daemon restart, not immediately — documented as
    such rather than force-added to the live-rebuild path.
  - Verified end-to-end against `timpsd-sim`: for one field per section
    (`osd.hinting`, `daynight.transition_s`, `motion.skip_frames`), POSTed
    the value, confirmed the `[CTRL] set <key> = <value>` log line, the
    `CONFIG persisted ... setting(s)` write to the conf file, and the
    clamped/persisted value round-tripping back through `GET /control`;
    also re-verified `daynight.switch_cmd`/`motion.cooldown_ms` remain
    silently unreachable (no log line, no config-file write) and that an
    out-of-range `daynight.hysteresis` POST still clamps to its
    documented bound. `docs/wiki/Configuration-Reference.md` and
    `docs/wiki/HTTP-Control-API.md` updated to match (File-only →
    Restart-only/Live per field, per the corrected POST-able-key lists).
- **`osd.hinting` autohinter could erase thin stems instead of sharpening
  them** (adversarial review catch on the autohinting feature added above,
  fixed on the second attempt after the first repair was itself caught in
  re-review): the feature as first landed snapped each stem-side edge
  independently to `floorf(mid + 0.5)` (nearest pixel column/row). For a
  stem thinner than 1px in device pixels - which is exactly what the shipped
  `/usr/share/fonts/default.ttf` (correctly identified below as UbuntuMono
  **Regular**, not "Roboto Bold" as originally claimed above) produces for
  its ~0.98px-wide vertical strokes at the 12px OSD default - the two edges'
  independently-rounded columns could land on the very same integer (one
  edge at -0.49px displacement, the other at +0.49px, both floor to the same
  column), collapsing the stem to zero width, and a zero-width path renders
  as nothing: 'I' lost its entire vertical stem at 12px, "IlIlIl" at 8px
  lost stems wholesale - the exact failure the feature was meant to fix,
  made worse instead. Root cause: only tested against a Bold-weight
  substitute font, whose stems are wide enough that this collision is
  structurally unreachable, hiding the bug. A first repair attempt (sort
  the candidate edges by pre-snap position, push apart ADJACENT pairs whose
  snaps collided) was geometrically wrong and never shipped past review:
  sorted adjacency doesn't establish that two edges bound the same stroke,
  so it forced apart edges that were legitimately collinear (the aligned
  caps of H/U/N/M's twin stems, split segments of one stem side - new
  regressions at sizes that had been fine), mis-paired stems whose sides
  interleave with bar/bowl edges in sort order (57/89 sub-pixel stem pairs
  in the shipped font still collapsed, e.g. 'b' at 8px lost its left stem),
  and tore '+''s crossbar onto different rows at 11-13px.
  The final fix (`src/hal/msttf.c`: `autohint_glyph()`/`resolve_snaps()`,
  replacing the per-contour pass) pairs edges by actual stroke geometry
  instead. Candidates are collected across the WHOLE glyph (a bowl wall is
  bounded by one outer-contour and one counter-contour edge) together with
  their traversal direction and perpendicular extent, and two edges form a
  stroke pair only if ALL of: opposite traversal direction (a consistently
  wound outline walks the two sides of one stroke in opposite directions),
  overlapping perpendicular extents (they face each other along the stroke),
  and ink between them (even-odd midpoint test on the pre-snap outline,
  same fill rule as the rasterizer - rejects sub-pixel counters/gaps, whose
  bounding edges also satisfy the first two conditions). Only such pairs
  whose independent snaps collided are touched: the lower edge keeps its
  snap, the upper is pushed out by the pair's own measured pre-snap width
  rounded (floored at 1px). Edges at near-equal positions are recognized as
  already merged (never separated), and if a push moves one segment of a
  split edge line its near-equal siblings follow (keeps '+'-style split
  crossbars on one row). Everything the collision pass doesn't touch keeps
  its plain independent snap bit-for-bit. For width>=1px, `floor(x+0.5)`
  and `floor(x+w+0.5)` are always different (a half-open interval of
  length>=1 contains an integer boundary), so thick stems never enter the
  collision branch at all. No font- or size-specific constants: widths and
  separations are measured per pair at render time; the only epsilons are a
  0.05px float-identity tolerance and a 0.25px degenerate-overlap gate in
  device pixel space.
  Re-verified against the actual shipped font (byte-identical via md5 to
  `package/thingino-fonts/files/UbuntuMono-Regular2.ttf`, TTF `name` table
  family "UbuntuMono" subfamily "Regular") plus 5 other fonts spanning
  Regular-to-Black weights (Roboto Regular/Bold/Black, DejaVu Sans
  Regular/Bold) via an ink-verified white-box sweep (even-odd inside test
  against the pre-hint outline, so only pairs with real ink between them
  count) over every ASCII glyph at every size 8-40px plus 48/64/96/128/192/
  256px: 4568-5274 ink-verified stroke pairs per font, 0 collapse to zero
  width (89/89 pairs that collided under independent snapping in the
  shipped font are preserved; 70-71/70-71 and 4/4, 2/2, 0/0 in the others),
  0 collinear splits torn apart, 0 thin strokes over-widened. Regression
  gate at thick sizes: rendering all 94 ASCII glyphs at 32/48/64/96/128px
  through the real `msttf_render()` path, the new pairing logic is
  byte-for-byte identical to plain independent snapping (the pre-repair
  semantics) in all 470 renders - and byte-identical to the ORIGINAL
  feature commit for H/U/N/K at every thick size and every glyph at
  32/48px except where that commit's own in-place snapping mutated later
  edges' inputs mid-pass (a defect the collect-then-apply restructure
  removed). Spot-verified renders: 'I'@12px full stem, "WYZE-KINDERZIMMER"
  @12px and "IlIlIl"@8px keep every stroke, 'b'/'K'/'P'/'d'/'g'/'p'/'q'/
  'u'@8px and 'B'/'D'/'K'/'R'/'h'@10px all keep their stems (alpha-coverage
  on/off ratios 0.87-1.41, no collapse signature), '+'@11-13px crossbar
  symmetric on every row. Fuzzing: 300k ASan+UBSan iterations of a direct
  degenerate-input driver against the new pairing code (0/1/odd candidate
  counts, all-identical positions, .5-boundary straddles, +/-1e9
  coordinates, capacity-overflow contours) plus the 1200-mutant malformed-
  font corpus (bit flips/truncation/zero-fill/0xFF-fill of 3 seed fonts)
  with hinting forced on: 0 crashes/UB.
- **Double-instance ISP collision hardening**, root-caused after a real
  incident: a manually-launched foreground `/tmp/timpsd` test build was still
  running when `/etc/init.d/S95timps restart` fired in another shell.
  busybox's `start-stop-daemon -S -x /usr/bin/timpsd` matches "already
  running" by executable PATH, not by process identity, so it never saw the
  `/tmp/timpsd` process and started a second `/usr/bin/timpsd`. That second
  process's `IMP_ISP_Open`/sensor-init reset the shared ISP kernel driver
  state out from under the first, destroying its FrameSource channels; the
  first process then spun in its encoder watchdog's recovery cycle forever
  (every attempt "succeeded" per `StartRecvPic` but the hardware was
  genuinely gone) producing zero video for 2+ minutes with no way to
  self-recover and no mechanism to hand off to init supervision. Two
  independent fixes:
  - `src/main.c`: an exclusive `flock()` on `/run/timps.lock`, taken before
    any ISP/HAL initialization. A second instance that loses the race logs a
    clear fatal error and exits immediately, never touching the shared
    hardware in the first place - this closes the actual race, independent
    of how the double-start happens. The fd is opened `O_CLOEXEC` (Fable
    review catch): `imp_motion.c`'s `on_motion` hook double-forks a detached
    grandchild that `execlp()`s a long-lived clip-capture/upload script -
    without `O_CLOEXEC` that grandchild would inherit the lock fd and keep it
    held after timpsd itself exits, permanently blocking the next instance
    from ever acquiring it.
  - `src/hal/hal_ingenic.c` (`video_thread`): the encoder watchdog's forced
    recovery cycle (`fs_unuse()`/`fs_use()`/`StartRecvPic`) now tracks
    consecutive cycles that never actually yielded a frame (via
    `IMP_Encoder_GetStream`, not just a "successful" `StartRecvPic`). After
    `MS_VIDEO_WATCHDOG_MAX_RECOVERIES` (default 5, ~25 s of total dead time)
    it logs FATAL and raises `SIGTERM` on itself so the existing orderly
    shutdown path exits the process, instead of retrying forever - this
    firmware has no process supervisor/respawn beyond a console getty, so the
    camera stays down until a manual/scheduled restart, but that is still
    strictly better than the prior silent, unbounded zero-video hang.
    `jpeg_thread` had the identical infinite-retry gap; it now gives up on
    just its own channel after the same number of failed cycles
    (`MS_JPEG_WATCHDOG_MAX_RECOVERIES`), mirroring how `audio_thread` already
    disables itself alone rather than taking the whole process down for a
    non-primary stream.
- **Piggyback JPEG (`/snapshot.jpg`, `/stream.mjpeg`) broke when a 90/270
  rotation was refused by the SW-rotate safe envelope.** On a T23 build with
  `USE_ROTATE`/`USE_SW_ROTATE`, requesting a rotation that exceeds the safe
  envelope (e.g. a full-res `1920x1080@25` main stream) is correctly refused
  by `sw_rot_start()` — the video/RTSP path falls back to running UNROTATED.
  But `jpeg_attach()` re-read the *raw* `cfg->video[vi]` (rotation still 90)
  instead of the caller's post-refusal effective config, so it sized the JPEG
  encoder channel from the rotated dims (e.g. `1080x1920`). That width is not
  16-aligned, so `IMP_Encoder_CreateChn` failed ("JPEG CreateChn N failed")
  and `/snapshot.jpg?chn=N` returned the literal `no frame` on every affected
  channel, even though video streamed fine. `jpeg_attach()` now takes the
  effective `const ms_vstream_cfg *` the caller actually brought the video
  channel up with, so the JPEG channel always shares the video channel's true
  post-refusal dimensions. This also covers the analogous T31
  (`ROT_HAS_FS_ROTATE`) FS-rotate fallback path, which had the same defect.
  Pre-existing bug (introduced with the JPEG-piggyback + rotation plumbing in
  July; unrelated to the recent C11-hardening / frame-pool changes).
- **IVS motion detection also used raw rotated dims after a refused 90/270
  rotation** — the sibling defect flagged during the review of the JPEG fix
  above, same root cause: `imp_motion.c`'s `imp_motion_start()` re-derived the
  monitored stream's geometry from the raw `cfg->video[mon]` instead of the
  effective post-refusal config. When the monitored stream's 90/270 request
  was refused by a safe-envelope check and the stream came up UNROTATED, the
  config still said 90, so on T31 (`ROT_HAS_FS_ROTATE`) IVS was created with
  swapped `frameInfo` dims (e.g. `1080x1920` against a framesource really
  delivering `1920x1080`), and on T23 (`ROT_HAS_SW_90`) the ROI grid was
  inverse-rotation-mapped for a rotation that was not happening — grid cells
  (and the privacy-mask exclusion, which is compared in displayed-frame space)
  landing transposed/mirrored relative to the frame the user actually sees.
  Fix follows the `jpeg_attach()` precedent: `ing_start()` now records the
  rotation it ACTUALLY applied per stream (`g_eff_rot[]`; the refusal decision
  only exists in `hal_ingenic.c`, and `videoN.rotation` is restart-only so the
  boot-time record stays valid), and `motion_sync()` hands
  `imp_motion_start()` an effective copy of the monitored stream's config
  built from `g_cfg_boot.video[mon]` + that recorded rotation. Sourcing the
  geometry from `g_cfg_boot` (per the `config.h` WHY block) also stops a live
  `/control` write to restart-only `videoN.*` geometry from leaking into a
  later motion-grid rebuild against the still-running old pipeline. Verified:
  `make sim` clean; `gcc -fsyntax-only -Wall -Wextra` of `hal_ingenic.c` +
  `imp_motion.c` against the vendored SDK headers for T31 (1.1.6, with and
  without `USE_ROTATE`) and T23 (1.3.0, with and without
  `MS_ENABLE_SW_ROTATE`). Needs an on-device check on a T31 with
  `motion.enabled=1` and a refused rotation (e.g. `1920x1080@25` + rotation
  90) confirming the IVS grid tracks the unrotated frame.
- **The identical raw-vs-effective-rotation-dims bug (see the `jpeg_attach()`
  fix and the IVS fix directly above, commits `8cc8987` and `efe94b9`) was
  still live in every OTHER consumer of a stream's post-rotation geometry**:
  `record.c`'s fMP4 track (`tkhd`) dims (both the continuous recorder's
  `seg_open()` and the on-demand clip muxer in `record_clip()`),
  `mp4/httpd.c`'s live-MP4 mux dims in `stream_mp4()` AND its `/events` SSE
  `stats` payload's `video[].width/height`, `rtsp.c`'s SDP `a=framesize`
  attribute in `gen_sdp()`, and `/control` GET's `eff_width`/`eff_height`
  fields all called `ms_vstream_eff_dims()` on the RAW configured rotation
  instead of the ACTUAL post-refusal geometry - so on any T23 SW-rotate /
  T31 FS-rotate safe-envelope refusal (the common case at typical
  main-stream resolutions/framerates - confirmed on kinder-links and
  Galayou, both flashed with rotation enabled tonight), a recorded MP4, a
  live-MP4 stream, the `/events` stats feed, RTSP SDP negotiation, and the
  `/control` status could all advertise swapped W/H for a stream that is
  actually running unrotated. Root cause: `hal_ingenic.c` already tracks the
  real per-stream rotation (`g_eff_rot[]`) and already pushes the
  correspondingly correct effective width/height into the hub via
  `hub_set_video_params()` (both the SW-rotate accept path and the bound
  FS-rotate/no-rotate path push from the post-refusal `v`), but nothing
  could read that back out. Added `hub_get_video_params(int src, int
  *vcodec, int *w, int *h, int *fps)` (`src/hub.c`/`hub.h`) as that missing
  getter; all five originally-flagged call sites plus the SSE stats block
  now call it first and only fall back to the raw `ms_vstream_eff_dims()`
  computation when the hub hasn't been populated yet (stream never started
  - the getter returns 0). `/control`'s GET needed one extra bit of care:
  its `eff_width`/`eff_height` describe the live (persist-only, possibly
  POSTed-but-not-yet-restarted) config, not necessarily the boot config, so
  it only trusts the hub when the live rotation/width/height still match
  `g_cfg_boot` (i.e. nothing pending); otherwise it keeps the raw preview
  swap, preserving `scripts/timps-qa.sh --test-rotation`'s existing
  pending-config-preview contract. `ms_vstream_eff_dims()` itself is
  untouched and still used for its original purpose inside
  `hal_ingenic.c`'s pre-refusal setup (`sw_rot_start()`'s envelope checks,
  `enc_create()`, `fs_create()`) where the raw computation is exactly what's
  needed before the accept/refuse decision has been made. Verified: `make
  sim` clean (`hal_sim.c` never refuses rotation, so its raw-computed hub
  push already always equals the effective dims - no change needed there
  for consistency); cross-compiled `hal_ingenic.c`/`record.c`/`mp4/httpd.c`/
  `rtsp.c`/`control.c`/`hub.c` clean (`-Wall -Wextra`, zero warnings) against
  the real vendored SDK headers for T31 1.1.6 (`USE_ROTATE=1`) and T23 1.3.0
  (`USE_ROTATE=1 USE_SW_ROTATE=1`), using tonight's already-built
  `wuuk_y0510_t31x...`/`galayou_y4_t23n...` cross toolchains (link step not
  attempted - the vendor `libimp.a`/`libalog.a`/`libsysutils.a` stubs aren't
  vendored in this checkout, only the headers). Not verified on real
  hardware: kinder-links (192.168.10.124) and Galayou (192.168.15.129) were
  both reachable tonight, but a live redeploy onto production cameras mid-
  incident was judged too risky to rush safely - this relies on header-level
  type-checking plus manual tracing of both the SW-rotate and FS-rotate
  refusal paths instead.
- **OSD read live `videoN.rotation` instead of the boot snapshot** (audit A2):
  `imp_osd.c` read `g_cfg.video[si].rotation` in the text/logo/privacy placement
  paths. `videoN.rotation` is restart-only, so a `/control` write to it would
  immediately re-place the OSD for a rotation the running encoder is not
  producing (overlay wrong until restart). All rotation reads now use
  `g_cfg_boot`, which also removes them from the C11 data-race class below.
- **`imp_osd.c` was the 7th site of the raw-vs-effective-rotation-dims bug**
  (see `jpeg_attach()`/IVS/hub-getter fixes above, commits `8cc8987`,
  `efe94b9`, `9f71b41` - found by independent review of the latter): the
  rotated-IPU-OSD-path gate in `refresh_text()`, `setup_logo()`,
  `setup_cover()` and `osd_rot_place()` (text/logo/privacy placement, plus the
  H5 oversize check and `osd_even_pad()`) all decided `rotated` from the RAW
  `g_cfg_boot.video[si].rotation`, not whether rotation was actually applied.
  On a refused 90/270 (T23 SW-rotate / T31 FS-rotate safe-envelope refusal -
  stream runs unrotated, OSD still active on the bound path), `rotated` was
  wrongly computed `true`: the H5 size check used `hlim = s->width` (the
  narrower, rotated-frame limit) against a bitmap sized for the real, wider
  unrotated frame, defeating the very check whose own comment warns IMP_OSD
  "writes past the frame buffer" on several SoCs - plus needless even-padding
  and top-band placement clamping. Fixed by a new `osd_rotated(s)` helper,
  used at all four sites, that asks `hub_get_video_params()` for the dims the
  HAL is ACTUALLY running and compares them against the raw configured
  (unrotated) dims: swapped relative to raw = rotation genuinely applied,
  unswapped despite a 90/270 request = refused. Falls back to the raw
  `ms_vstream_eff_dims()` computation only if the hub hasn't been populated
  yet for that stream (matches the other sites' fallback; in practice
  unreachable here since `ing_start()` calls `hub_set_video_params()` for
  every stream before starting the OSD updater thread). Verified: `imp_osd.c`
  is not part of `make sim` (confirmed via the Makefile's `SIM_SRC` list, same
  as `hal_ingenic.c`); cross-compiled clean (`-Wall -Wextra`, only pre-existing
  unrelated warnings) against the real vendored SDK headers for T31 1.1.6
  (`USE_ROTATE=1`) and T23 1.3.0 (`USE_ROTATE=1 USE_SW_ROTATE=1`) using
  tonight's already-built cross toolchains; traced the refused-rotation +
  active-OSD scenario by hand, confirming `osd_rotated()` now returns `false`
  and every site behaves as the non-rotated (identity) case.
- **`sensor.*` numerics were POST-able and persisted but unclamped** (config
  audit F-01, high severity): `sensor.i2c_addr/fps/width/height` had no clamp,
  so a garbage `/control` POST (e.g. `sensor.width=70000`) survived a reboot and
  could feed the ISP init a value that crash-loops the camera. Now clamped like
  `videoN.*` (`fps` 0–120, `width`/`height` 0–8192, `i2c_addr` 0–0x7F; `0` still
  means "auto").
- **Completed the C11 data-race sweep started in v1.7.8** (audit A1 / config
  audit F-02/F-03): several background threads and the `GET /control` path still
  read live-mutable `g_cfg.*` ints/enums/floats lock-free while the `/control`
  connection thread mutates them under `config_str_lock()`. Fixed with the
  daynight per-iteration whole-section snapshot pattern (or a point-of-use
  lock+copy where a loop snapshot doesn't fit):
  - `record.c`: `rec_thread` now snapshots the whole `record` section once per
    pass and threads it through `want_run`/`want_write`/`motion_recent`/
    `seg_open`/`prune_free`; `record_clip` and `record_get_status` snapshot the
    live ints under the lock. (Strings were already correctly locked.)
  - `timelapse.c`: same per-pass snapshot for `enabled/channel/interval_s/
    keep_days`; `timelapse_get_status` reads its ints under the lock.
  - `rtsp/speaker.c` (`ao_ensure`) and `hal_ingenic.c` (`hal_ao_open` AEC):
    cold reads of `spk_enabled/spk_volume/spk_gain` and `audio.aec` now under
    the lock.
  - `rtsp/rtsp.c`: SDP/PLAY reads of `audio.bitrate_kbps/samplerate/channels`
    now under the lock (video fields already used `g_cfg_boot`).
  - Status/GET accessors: `daynight_get_status`/`daynight_sun_status` (incl. the
    tearable `sun_latitude/longitude` floats), the sim `motion_get_status` stub,
    and the numeric parts of `control_get_json`/`control_daynight_json` plus
    `motion.monitor_stream` now snapshot under the lock.
  - **Concurrent POSTs are now serialized**: `control_apply_json` runs its whole
    apply-and-notify body under a single mutex so two simultaneous POSTs can no
    longer interleave a partially-applied config (`hub_control()` still runs
    after each field's `config_str_unlock()` as before).
- **Doc drift** (config audit F-10): `record.post_roll_s` range is 1–300, not
  0–300 (wiki + QA spec fixed); `general.osd_pool_size` is in KB, not bytes
  (wiki fixed); `timps.conf.example` `osd_pool_size` now shows the real default
  (1024).

### Changed
- **Clamp the remaining unclamped live/persist config fields** (config audit
  F-09): `image.running_mode` 0–1, `image.anti_flicker` 0–2, `image.core_wb_mode`
  0–1, `audio.samplerate` 8000–96000.
- **`videoN.qp` and `videoN.max_gop` marked reserved / no-effect** (config audit
  F-04): both are parsed, clamped, persisted and echoed for compatibility but no
  HAL consumes them — the encoder's keyframe interval comes from `videoN.gop`
  (`rcAttr.maxGop = v->gop`) and there is no separate init/fixed-QP wiring (the
  pre-T31 attribute path is CBR-only). Wiring them up cleanly would need a
  rate-control redesign that cannot be hardware-verified here, so they are
  documented as reserved like `motion.roi_*`, with a one-shot warning when a
  non-zero value is set. Use `videoN.min_qp`/`max_qp` + `videoN.rc_mode` for
  quality control.
- **Removed the `cmfc`/`cmf2` CMAF brands from the fMP4 `ftyp`** (audit A3):
  CMAF (ISO/IEC 23000-19) requires one track per file, but this muxer always
  writes combined video+audio into a single `moov`, so advertising CMAF
  conformance was wrong for the A/V case (the strict validators the brands were
  meant to satisfy would flag it). Browsers ignore compatible-brands, so this
  was cosmetic; reverting the v1.7.8 addition.
- **Halve `FQ_MAX_BYTES` on the small-RAM SoCs** (perf audit P-08): T10/T20/T21
  builds now compile with `-DFQ_MAX_BYTES=1048576` (2 MB → 1 MB per queue), so
  the all-stalled worst case (8 HTTP + 8 RTSP + 8 SRT) is ~24 MB instead of
  ~48 MB. Added a `PLATFORM_CFLAGS` hook to the Makefile for the affected
  platforms.
- **RTSP play-loop syscall trimming** (perf audit P-03/P-04): `stream_loop` now
  reads `ms_now_us()` once per iteration and reuses it (no vDSO on this MIPS
  target — each call was a real syscall, 3–5×/iteration), and gates the
  nonblocking control-socket poll to ~50 ms instead of once per media frame
  (backchannel sessions still poll every iteration so interleaved speaker audio
  is not delayed). Teardown latency stays well under the RTSP keepalive window.

### Performance
- **Single-copy frame publish via `hub_publish_take()` + a per-source packet
  pool** (perf audit P-01, previously deferred): every published frame used to
  be copied twice - once when `video_thread`/`jpeg_thread` assemble the IMP
  packs into a scratch buffer (necessary: Annex-B start-code fix-up across
  scattered packs), then a SECOND time when `hub_publish()` -> `pkt_new()`
  malloc'd and copied the whole access unit into a refcounted `ms_pkt` (plus a
  `free` after the last `unref`). At 25 fps x 2 streams that was ~50 malloc/free
  pairs/s and up to ~1.5 MB/s of redundant memcpy, and frame-sized heap churn
  (4 KB-400 KB) is the classic fragmentation source on the 32 MB SoCs.
  - New `frame.c` primitives: an `ms_pkt` now carries `cap`/`pool` fields; a
    tiny per-source recycling `pkt_pool` (`pkt_pool_get`/`pkt_pool_init`, and a
    pool-aware `pkt_unref` that returns the buffer to the pool instead of
    `free`ing it on the last reference). `pkt_new()` (copy constructor) is
    unchanged, so every existing sink (fanqueue, the recorder pre-roll ring,
    RTSP/SRT/HTTP) is byte-for-byte unaffected.
  - New `hub_pkt_get(src, cap)` + `hub_publish_take(src, pkt, ...)`: the
    producer assembles the AU DIRECTLY into a pooled buffer and hands ownership
    to the hub - no second copy. `hub_publish()` and `hub_publish_take()` share
    one under-lock helper, so the two invariants are provably preserved: (a) the
    0-subscriber skip does NOT cause a per-frame malloc/free - a 0-sub publish
    just returns the buffer to the pool (borrow+return of the same buffer while
    idle); (b) `vparam_update()` still reads the AU under `s->lock` at the same
    point in the sequence, before any push/hand-off.
  - Pools are process-lifetime statics (a slow subscriber can hold a packet long
    after the producer stopped), sized `HUB_POOL_MAX_FREE=4` idle buffers with a
    `HUB_POOL_KEEP_CAP=96 KB` ceiling so a one-off large IDR is freed rather than
    pinned idle (idle pool memory bounded to ~384 KB/source). Beyond the pool
    (recorder pre-roll pinning many frames, or a slow client) it falls back to
    malloc/free - never worse than before.
  - Converted producers: the hardware `video_thread` and dedicated `jpeg_thread`
    (`hal_ingenic.c`) and both `make sim` producers (`hal_sim.c` video + JPEG,
    so the new path is actually exercised host-side). Left on the copy path
    deliberately: the three audio producers (tiny `__thread`-static frames whose
    per-frame copy is negligible and whose no-heap-churn design is intentional)
    and the opt-in T23 software-rotate video/JPEG path (`ROT_HAS_SW_90`,
    untestable here and flagged as extra risk by the audit). `hub_publish()`
    stays a first-class API for those.
  - Verified: clean `make sim`; an ASan+UBSan host build driven with concurrent
    multi-client RTSP (TCP+UDP) fan-out, snapshot hammering and motion recording
    (pre-roll ring pinning pooled packets) reported zero leaks / use-after-free /
    UB across subscribe/publish/unsubscribe churn and shutdown; and
    `hal_ingenic.c` type-checks clean against both the T31 headers (converted
    paths) and the T23 headers (untouched sw-rotate path). Still wants the
    on-hardware soak the audit called for before fleet rollout.
- **Idle-wakeup consolidation via a stop-condvar** (perf audit P-02, previously
  deferred): the periodic worker threads stayed responsive to shutdown by
  slice-sleeping (usleep in 100-300 ms chunks, re-checking a stop flag each
  chunk), waking ~25x/s in aggregate even fully idle. New reusable `ms_stopgate`
  primitive (`util.c`, same CLOCK_MONOTONIC condvar pattern as `fanqueue`/
  `events`): a worker now blocks on one `pthread_cond_timedwait` per real
  interval and wakes immediately on stop.
  - Converted: `daynight.c` (200 ms slices -> one wait per detection interval),
    `record.c` and `timelapse.c` (disabled-idle poll 300 ms -> 1 s, only bounds
    how fast a `/control` enable is noticed from the fully-idle state; motion/
    continuous recording is unaffected - it runs in the `fanqueue_pop` path),
    and the `imp_osd.c` updater (ten 100 ms slices -> one 1 s wait; a plain
    stop-gate, NOT the `hub_set_activity_cb` wake the audit floated, since that
    couples the OSD updater into the untestable HAL on-demand path for only a
    sub-second first-connect timestamp-freshness nicety). Idle wakeups drop from
    ~25/s to <5/s.
  - `main.c`'s `while (g_run) sleep(1)` is intentionally left as-is: `sleep()`
    already returns immediately on the shutdown signal (EINTR), and a condvar
    cannot be signalled from an async signal handler.
  - Shutdown safety (the highest priority for this item): `ms_stopgate_stop()`
    sets the flag and broadcasts under the SAME mutex the waiter tests its
    predicate under, and the waiter checks the predicate both before and inside
    the wait loop - so a stop requested before OR during the wait can never be
    missed. Traced by hand for all four threads and verified live: idle,
    recording and mid-stream SIGTERM all shut down in 0.14-0.34 s (well under
    the 3 s watchdog `alarm()`), ASan-clean.

### Added
- **Opt-in geometric autohinting for the OSD TrueType rasterizer** (`osd.hinting`,
  default `0` = off), addressing a research finding that substream OSD text
  (12px default, `src/config.c`) looks visibly less crisp than the main
  stream: `msttf.c`'s from-scratch `glyf` rasterizer scales outlines by a
  plain float factor with 2x2 supersampled coverage AA but never interpreted
  the font's embedded TrueType hint bytecode, so unhinted glyphs land at
  inconsistent sub-pixel positions and show uneven stroke widths between
  characters at small sizes - a real, well-known font-rendering problem, not
  imagined, and confirmed present in the shipped `/usr/share/fonts/default.ttf`
  (UbuntuMono Regular - corrected below; originally misidentified as "Roboto
  Bold" in this entry). Considered two fixes: (a) a true TrueType instruction
  interpreter (stack-based VM executing the font's own hint bytecode -
  genuinely correct, but real interpreter-writing work with a real
  correctness/security surface for bytecode running on-device with no
  sandboxing) vs (b) a lightweight geometric autohinter (snap long,
  near-vertical/near-horizontal flattened outline edges - typical letter
  stems/serifs - to the pixel grid before rasterizing, skipping bytecode
  execution entirely). Chose (b): this is a from-scratch, deliberately minimal
  rasterizer in a security/stability-sensitive embedded daemon, not a
  general-purpose font library, and a modest, safely-bounded improvement beats
  a more "authentic" but real bytecode-execution surface. `msttf_set_hinting()`
  (mirrors the existing `msttf_set_ss()` pattern) gates a new autohint pass
  (now `autohint_glyph()`; see the stem-collapse fix above for the final
  algorithm) that runs after `parse_glyph()` flattens a glyph's
  contours to device-pixel-space polylines: any edge longer than ~2px and
  within ~11 degrees of vertical/horizontal has both endpoints forced to a
  common rounded pixel column/row. A length gate excludes the short chords
  `quad()` emits per flattened bezier (8/curve), so round glyphs ('O', 'o')
  are provably untouched (verified bit-identical alpha-sum before/after in
  testing below) while stems ('l', 'I', '1', 'i') snap cleanly. Purely
  geometric, no bytecode execution, no new attack surface. Wired up as
  `osd.hinting` (bool, default 0) next to the existing `osd.supersample` in
  `config.c`'s `osd_fields`/`ms_osd_cfg` (not `general.*`: it's the same
  category of global TTF-rasterizer tunable as supersample, and unlike
  `general.*` the `osd.*` section is readable via `GET /control`). Same
  File-only/restart-only handling as `supersample`: applied once via
  `msttf_set_hinting()` in `imp_osd_setup()`, which only runs at startup, so
  it needs a restart to take effect - no live-apply plumbing added. Default
  off: zero behavior/output change for any existing install. Verified: `make
  sim` clean; a standalone host-side test (`msttf.c` has no external
  dependencies, so it links directly with a small driver) rendered strings at
  12px with hinting off/on against a real glyf-outline TTF (DejaVu Sans Bold -
  the shipped font's actual identity, UbuntuMono Regular, wasn't confirmed at
  the time and a Bold-weight substitute was used instead; see the follow-up
  fix below for why testing only a Bold weight hid a real bug) confirming: no
  crash, no all-zero/degenerate output, canvas size stable within 2px, alpha
  coverage within a 0.5-2x sane band; round glyphs ("OoOo0") bit-identical
  alpha sum on vs off; a repeated-stem string ("IlIlIlIl") showed edge
  "fuzziness" (partial-alpha-coverage variance at each stem's boundary
  columns, the direct signature of sub-pixel-phase inconsistency) drop from
  0.038 to exactly 0.0 with hinting on, i.e. every stem instance became fully
  sharp-edged regardless of its accumulated sub-pixel phase; multi-word test
  strings at 12px and 32px stayed fully legible with no garbling in both
  modes. Caveats for review: this is a heuristic approximation, not real
  hinting - it does not reproduce the font's authored hint intent and cannot
  guarantee exact stem-width preservation the way a bytecode interpreter
  would; the angle/length thresholds were tuned against one real font at OSD
  text sizes (8-32px) and are untested against other TTF files if a user
  swaps `osd.font_path`; `imp_osd.c`/`config.c`/`config.h` changes (the
  `msttf_set_hinting()` call site and the new `hinting` field) could not be
  cross-compiled in this pass (no vendored IMP SDK headers available in the
  sandbox) and still need an on-device or cross-toolchain build check.
- **QA coverage for previously-untested live-settable fields** (config audit
  F-08, `scripts/timps-qa.sh` section 8b): the daynight TIME/SUN path
  (`daynight.mode` + `time_night_start`/`time_day_start`/`sun_latitude`/
  `sun_longitude`/`sun_sunrise_offset_min`/`sun_sunset_offset_min`), the speaker
  keys `audio.spk_volume`/`spk_gain`/`aec` (gated on `caps.audio`, skipped
  cleanly when no AO pipeline is compiled in), and the path-traversal-sensitive
  live strings `record.dir`/`timelapse.dir`.

### Documentation
- **`timps.conf.example` brought back in sync with the code** (config audit
  F-05/F-06/F-07): added the 12 missing `daynight.*` keys (`mode`, the
  `time_*`/`sun_*` keys, `boot_settle_s`/`boot_settle_max_s`/`boot_stable_pct`,
  `night_reconfirm_s`, `probe_max_skip_s`) with example values and comments;
  corrected `http.adaptive_drop` (default is `1`, hardware-verified, not the
  stale "EXPERIMENTAL, default 0" text); fixed the audio/speaker block (there IS
  an AO pipeline on USE_PLAY/USE_BACKCHANNEL builds — `spk_enabled` gates the
  physical speaker and `spk_volume`/`spk_gain` are live), added the missing
  `audio.aec` key, and added `opus` to the `audio.codec` list.
- Fixed the stale doctrine comment in `config.c` that still claimed live int
  reads "need no lock" (it contradicted the correct `config.h` doctrine and was
  the likely root cause of the recurring F-02/F-03 class), and the stale
  `control.c` comment that described `on_motion` as `system()` when it is
  `fork()+execlp()`.

### Deferred (audited, not done this pass)
- **Perf P-01 and P-02 are now done** (see the Performance section above) - the
  dedicated session with a "Garage" (Wuuk/T31-class) camera earmarked for the
  soak test made it reasonable to attempt them. The T23 software-rotate publish
  path and the three audio producers were deliberately left on the copy path
  (rationale in the Performance section); those remain available follow-ups if a
  T23 with rotation is ever soak-tested.
- **SDK feature-gap items #5 (`GetChnEvalInfo`) and #6 (exposure ceiling)** were
  skipped: both are new IMP features that cannot be verified without live
  hardware.

### Changed
- **De-duplicated JPEG source selection + cold-wake grab** between
  `mp4/httpd.c` (`/snapshot.jpg`, `/stream.mjpeg`) and `timelapse.c`.
  `timelapse.c` hand-mirrored `httpd.c`'s `jpeg_src_from_path()` and
  `snapshot_jpg()`'s two-phase piggyback-wake grab ever since timelapse
  gained just-in-time subscription (`32ac430`, explicitly "ported
  snapshot_jpg's pattern") - and every later fix to that logic had to be
  remembered twice: `26afbee` (gethostname hardening) and `51d9325` (read
  restart-only `videoN.*` from the boot snapshot) both touched both files.
  Both are now `hub_pick_jpeg_src(cfg, chn, strict)` and
  `hub_grab_jpeg(src, wait_ms, busy)` in `hub.c`/`hub.h` - hub-source-index
  logic, so it lives with the rest of the hub source abstraction rather than
  a new standalone file. `httpd.c` now does only HTTP response framing
  around the grabbed packet; `timelapse.c` does only the file write.
  Preserved a real behavioral difference between the two callers as an
  explicit `strict` parameter: an explicit `/snapshot.jpg?chn=N` never falls
  back to a different channel (404s instead), while timelapse's configured
  channel always falls through to the dedicated `jpeg.*` channel / any
  piggyback stream, same as before. Also preserved `snapshot_jpg()`'s
  distinct 503 "busy" (subscribe failed - source full) vs. "no frame"
  (grab timed out) responses via an optional `busy` out-param, since
  `timelapse.c`'s copy never made that distinction and a naive merge would
  have silently collapsed it for HTTP callers. No functional change to
  either call site; verified with `make sim` and against `timpsd-sim`
  (both a live timelapse capture cycle and `/snapshot.jpg`,
  `/snapshot.jpg?chn=0`, an invalid `?chn=1`, and `/stream.mjpeg?chn=0`).

## [1.7.8] - 2026-08-06

### Fixed
- **C11 data races on runtime-mutable config**: several live-settable fields
  were read by a different thread than the `/control` writer without any
  synchronization against it — a data race regardless of whether the plain
  int read ever visibly "tears".
  - `audio.mute`: the per-frame audio worker read it lock-free on every
    captured frame. It's now `_Atomic int`, and a new `F_ATOMIC` field flag in
    `config.c` routes `field_set()`/`field_get()` through `atomic_store`/
    `atomic_load` instead of a plain assignment.
  - **daynight thread**: was reading individual `g_cfg.daynight.*` fields
    (and `image.running_mode`) lock-free once per ~500 ms poll. It now
    snapshots the whole `ms_daynight_cfg` struct plus `running_mode` under
    `config_str_lock()` once per iteration — the same whole-struct pattern
    `imp_osd.c` already used — and threads that local snapshot through
    `dn_day_trigger()`/`dn_status_update()`/`dn_switch()` instead of each
    re-reading global state.
  - **OSD `.enabled`**: the updater loop checked it lock-free before calling
    `refresh_text()`; the check now happens inside `refresh_text()`, after its
    own under-lock item snapshot.
- **`aac_asc()`** now logs a warning when handed a samplerate outside the
  standard AAC table instead of silently falling back to the 16 kHz index.

### Added
- **fMP4 CMAF brands** (`cmfc`, `cmf2`) added to the `ftyp` compatible-brands
  list for stricter CMAF validators (Bento4, some HLS/DASH tooling).
- **QA script** (`scripts/timps-qa.sh`):
  - New section 8c: an SSH-based round-trip test of the `osd.vars_file`
    custom-placeholder mechanism.
  - Section 8b: added the missing `probe_max_skip_s` live-setting coverage,
    and a new persist-clamp regression test (`ov_clamp_test`) that POSTs
    out-of-range values and asserts the read-back is the clamped boundary,
    not the raw input.

### Documentation
- Documented the `osd.vars_file` custom-OSD-placeholder mechanism
  (Configuration Reference), including its non-atomic-write concurrency
  caveat and the atomic-replace mitigation.
- Documented that empty `rtsp.user`/`http.user` credentials (the shipped
  default) leave the media endpoints open to anyone on the network while
  `/control`/`/events` stay loopback-gated (Configuration Reference, HTTP
  /control API Reference, `timps.conf.example`).
- Documented a deferred RTP/RTCP timestamp overflow after ~2.8-3 years of
  continuous uptime ("L13", Streaming Protocols § Known limitations).

## [1.7.7] - 2026-08-05

### Fixed
- **Day/night reconfirm probes were themselves the visible "periodische
  Tag/Nacht-Umschaltungen."** Every probe switch clunks the IR-cut relay,
  kills the IR LEDs and shows ~7–9s of dark colour video before reverting —
  so on a genuinely dark, unchanging night the hourly `night_reconfirm_s`
  probe flapped 8–12× per camera per night while learning nothing, and on a
  slow pre-dawn ramp the v1.7.4 sustained-brightening probe added another
  2–6 (each failed probe re-sampled a *lower* baseline, so the continuously
  declining gain kept re-crossing the freshly-lowered bar every 10–40 min to
  sunrise; fleet logs all 11 cameras 2026-08-03/04). Three pure
  probe-scheduling measures in `daynight.c`, no config/schema change:
  - **exponential backoff** — a probe that fails (reverts within 30s,
    `DN_PROBE_FAIL_WINDOW_MS`) doubles the periodic interval ×1→×2→×4
    (`DN_PROBE_BACKOFF_MAX`), bounded by `max(night_reconfirm_s,
    DN_PROBE_BACKOFF_CAP_S=4h)`; a camera keeps its first-hour self-healing
    probe but then stops clunking hourly. Any genuine transition (or a probe
    that sticks in day) resets the multiplier.
  - **brightening arming margin** (`DN_BRIGHTEN_MARGIN`, 0.97) — the hold
    starts only clearly below the probe bar, never on a tangent graze (fleet
    logs showed holds starting 0.2% under the bar).
  - **failure ratchet** — after a failed probe, a new brightening hold must
    additionally undercut `day_gain_pct`% of the level that just failed (a
    whole further trigger-worth of new brightening), so a slow ramp can no
    longer re-fire on a drifted bar; a real light-on step (20–35% gain drop)
    still passes immediately. Latched on failure, cleared on any genuine
    transition.

  Verified in `timpsd-sim` (fake-ISP harness): backoff intervals stretch
  15→30→60→60s (×2,×4,cap) under constant darkness; a clean 66% step still
  brightening-probes and sticks in day on the first try; a forced probe
  failure latches `backoff x2, ratchet < N`; a below-margin/above-ratchet
  gain produces no new hold (the volley cycle); and a gain that undercuts the
  ratchet is allowed a fresh probe again.
- **The periodic reconfirm probe still physically clunked the IR-cut on a
  schedule even when nothing had changed.** Backoff cut the *frequency* of the
  probe but not its *invasiveness*: on a camera sitting in genuine, unchanging
  darkness (cam-wyze, closet, 2026-08-04: "das klacken der IR blende nervt …
  nachts andauernd") every backed-off probe still drove the board's IR-cut
  relay — an audible mechanical click — only to read railed night gain and
  revert. The periodic probe is now **gated on passive evidence**: before it
  fires, the smoothed night gain is compared against the same probe bar the
  sustained-brightening hold uses; if the gain is still solidly deep in night
  (≥ `DN_BRIGHTEN_MARGIN` of the bar), the physical switch is **skipped** — no
  `dn_switch`, no IR-cut click — and the probe silently re-arms on the same
  backoff schedule. This is not weaker self-healing: a *false* night latch
  (actually daytime behind an engaged IR pipeline) reads low gain, which is
  exactly the evidence that fires the probe; only a genuinely-dark scene, where
  a probe could only fail, is skipped. The first probe after each night entry
  still always fires (so the stuck-forever class stays covered within the first
  interval), and a `DN_PROBE_MAX_SKIP_S` outer bound (12h) forces a probe
  regardless of gain once that long has passed since the last *actual* physical
  probe — the trust-nothing safety net for a permanently-flat reading that
  evidence alone can never clear. Net effect under permanent darkness: at most
  ~2 physical clicks/day (vs up to 6/day at the 4h backoff cap before). Verified
  in `timpsd-sim`: constant deep-night darkness fires one first probe then skips
  every subsequent scheduled probe (zero further `switching to day`); and with
  a lowered outer bound the skips interleave with a forced probe every bound
  period, proving the safety net is never silently disabled.
- **IR-reflection feedback loop could flip day/night every few seconds
  indefinitely.** A camera mounted very close (~30 cm) to a reflective object
  hits a *physical* loop the probe-economy logic above cannot see, because it
  happens on the PRIMARY threshold crossings, not on a probe: night → IR LED
  on → the LED reflects intensely off the close object → AGC gain reads very
  low ("bright") → genuine night→day crossing → IR LED off + colour pipeline →
  but it is actually still dark → gain rails back up → genuine day→night
  crossing → IR LED on again → repeat, clunking the IR-cut every few seconds.
  Added a general **oscillation breaker** in `daynight.c` (a backstop for ANY
  fast day/night oscillation, not IR-specific detection): it counts *genuine*
  (non-probe) mode flips in a rolling window (`DN_OSC_WINDOW_MS`, 60s) and, if
  `DN_OSC_FLIPS` (3) of them land inside it, logs one warning
  (`possible IR-reflection feedback loop detected (N flips in Ms) - camera may
  be mounted too close to a reflective object; freezing in <mode> for Ts`) and
  FREEZES the last-decided mode for `DN_OSC_FREEZE_MS` (10 min), suppressing
  both switches and probes so the loop cannot continue; after the cooldown it
  resumes and re-detects if the condition persists. Probe fire/revert flips are
  deliberately NOT counted — a reconfirm/brightening probe cycle is a normal,
  intentional 2-flip event under the probe-economy design above and can never
  trip the breaker. These are compile-time `#ifndef`-overridable constants (like
  the other `DN_*` tunables), non-configurable at runtime by design. Verified in
  `timpsd-sim` (fake-ISP harness): a gain swing across both thresholds every ~7s
  trips the breaker on the third genuine flip, holds the mode for the whole
  cooldown while the gain keeps swinging (zero switches), then lifts and
  re-detects; and — proving no regression — the reconfirm-probe fire/fail/backoff
  scenario (reconfirm=15s, constant darkness, a probe cycle every 15–60s) never
  emits the oscillation warning, because its flips are all probe-driven.
- **`/control` persisted (and echoed) the raw pre-clamp POST value instead of
  the clamped one.** In `timps_apply_setting()` (`control.c`, the single funnel
  every `/control` key passes through) the clamped/canonical value read back
  from `g_cfg` (`config_get_kv`) was computed but used *only* for change
  detection: the live HAL call, the `/events` "config" SSE echo and the value
  written to `/etc/timps.conf` all used the raw string. Posting an out-of-range
  numeric (e.g. `daynight.probe_max_skip_s` below its 3600 floor, or
  `image.brightness` above 255) left the daemon's in-memory value correctly
  clamped — `GET /control` reads `g_cfg`, so it was always right — but wrote the
  raw out-of-range text to the config file and pushed it over SSE. The file then
  disagreed with reality indefinitely (a reboot re-clamps in memory on load but
  never rewrites the file), and other open WebUI tabs showed the bogus value.
  Not a live-safety bug (the running daemon is governed by the clamped in-memory
  struct; `ing_control` re-reads `g_cfg` and only used the raw string for a log
  line), purely a persistence/display inconsistency. Fixed by feeding the
  canonical read-back value (exactly what `GET /control` reports) to all three
  consumers. The read-back is now taken unconditionally after the write (not
  gated on the *before*-write readability flag) so the legacy `osdN.*`
  all-streams keys — unreadable while per-stream item sets have diverged, but
  re-converged by the write — also persist their clamped value.
  Edge case ruled out: keys whose clamp happens but whose value is *not*
  read-back-able (`F_NOGET` fields `jpeg_quality`/`jpeg_fps`/`logo_w`/`logo_h`,
  and clamped ints inside `noget` sections `jpeg`/`srt`/`rtsp`/`http`/`events`/
  `general`/`sim`) would keep the old raw-value behaviour — but none of those
  keys are settable through `control_apply_json`, so the residual gap is
  unreachable in practice. Verified in `timpsd-sim`: for several ranged keys the
  `GET /control` read-back, the on-disk config bytes and the `/events` config
  push now all agree on the clamped value; QA section 8b (every live-settings
  round-trip: image/audio/osd/privacy/motion/daynight/record/timelapse, the
  persist-only `video0.bitrate` and `audio.codec` checks) stays 15 PASS / 0
  WARN / 0 FAIL, and the unchanged-value skip, the `image.running_mode`
  re-assert-without-persist and the `motion.sensitivity` quantization skip are
  behaviourally unchanged.

### Changed
- **`daynight.probe_max_skip_s` (the passive-evidence-skip outer bound above)
  is now a live-configurable setting** instead of a compile-time-only
  `DN_PROBE_MAX_SKIP_S` constant, requested after confirming the skip fix on
  real hardware overnight. Default unchanged (43200s/12h); range 3600–604800s,
  deliberately floored at 1h in `config.c`'s validation table and clamped
  rather than accepted below it — this stays a safety net, not a switch to
  turn the self-healing check off outright.

## [1.7.6] - 2026-08-03

### Fixed
Comprehensive audit for the same bug class as the v1.7.5 day/night fix — a
thread reaching a state with (1) healthy-looking continued execution, (2)
zero log output, (3) no reachable recovery path — across the rest of the
codebase, followed by fixes for every confirmed instance:

- **Motion detection could silently stall forever with `motion.enabled`
  still reporting true.** `imp_motion.c`'s IVS poll loop treated every
  `PollingResult`/`GetResult` failure as a bare retry with no counter, no
  log, and no recovery: a driver/SDK wedge left the thread ticking at the
  poll rate while `/control` kept claiming motion was live and the last
  "active" grid snapshot froze in place — silently killing motion-triggered
  recording downstream. Added a stall watchdog (10s of consecutive misses)
  that cycles the IVS channel and surfaces a new `motion.stalled` status
  field instead of failing silently.
- **RTSP sessions could become immortal.** The idle-reap exemption keyed off
  `session.tcp`, a flag latched at SETUP time and never cleared — but
  several real transport combinations (UDP video + TCP-only backchannel, a
  transport-switch re-SETUP, a SETUP-but-never-published TCP audio track, an
  encoder wedge before the first frame) set that flag while never actually
  writing a byte over TCP, so neither the idle reaper nor `SO_SNDTIMEO` could
  ever catch an ungracefully-dead client on that session. The exemption now
  tracks real per-sink TCP write success instead of the latched transport
  choice.
- **HTTP fMP4/MJPEG and SRT streaming loops could spin forever on an
  encoder stall.** Disconnect detection was entirely data-driven (piggybacked
  on send/receive of media), so a stalled encoder combined with a TLS client
  (whose non-blocking recv can never observe an orderly close) or a client
  that vanished without a clean TCP close left the loop spinning at its poll
  cadence indefinitely, pinning a client slot. Added a 60s no-packet idle
  bound to all three loops. The `stream.mp4`/`stream.mjpeg` pre-keyframe
  discard state also only ever requested one IDR and never re-checked for a
  disconnected client; it now retries the IDR request and probes for
  disconnect every ~1s while waiting.
- **Audio watchdog could never trip on a persistently failing `GetFrame`.**
  The miss-streak counter was reset unconditionally right before calling
  `IMP_AI_GetFrame`, so a GetFrame that kept failing after a successful
  `PollingFrame` reset its own watchdog on every tick — audio died silently
  and permanently (with a busy-loop risk, since that path had no `usleep`
  either) instead of tripping the existing 500-miss teardown.
- **JPEG (snapshot/MJPEG) encoder thread had no stall watchdog at all**,
  unlike the video encoder thread, despite being able to pin the
  framesource 24/7 once wedged. Added the same PollingStream-miss-counter +
  framesource recycle cycle video_thread already had.
- **Framesource `EnableChn` failures were never retried** once the
  refcount left them at ≥1 (e.g. motion detection holding a pin) — every
  subsequent user believed the channel was enabled. `fs_use()` now retries
  the real hardware enable independent of the refcount transition, closing
  this for the common single-holder case (co-holder cases are a documented
  partial gap, tracked as a follow-up rather than rushed in without
  hardware validation).
- **Backchannel/speaker ownership had no inactivity release.** A session
  that talked once and then went quiet (but kept its RTSP connection open)
  held the backchannel decode election and the physical speaker
  indefinitely — every other client's talk audio was silently dropped and
  the play-clip queue never played. Both now re-elect/release after 10s of
  silence from the current owner.
- **SRT client threads had no liveness check at all** when the source
  stopped publishing (`if (!p) continue;`), unlike the equivalent HTTP
  path. Added the same 60s idle-stall bound.
- Lower-severity visibility/config-trap fixes found in the same pass:
  `record.post_roll_s=0` made motion-triggered recording silently record
  nothing, ever (config minimum raised to 1); `/control`'s `record` status
  now exposes whether the motion gate backing motion-mode recording is
  actually available/enabled, and whether a manual recording override is
  latched; the OSD updater thread's create-failure path now logs (matching
  every sibling thread); the day/night "ISP unreadable" idle path is now a
  visible one-shot warning instead of debug-only.

Every fix independently reviewed by a second model pass against the actual
diff (not just the diagnosis) before landing; one review finding (a
`hal_ingenic.c` comment overclaiming the framesource-recycle fix under a
co-holder) was corrected to accurately describe the remaining gap. Verified
via `make sim` and a real T31 cross-build; deployed and QA-tested on one
camera before fleet rollout.

## [1.7.5] - 2026-08-03

### Fixed
- **Day/night thread could get permanently stuck at boot with zero self-
  healing.** From `DN_UNKNOWN`, the decision silently stays put while gain
  sits inside the day/night dead-zone (300..3000 default) — by original
  design. But a camera can boot with the ISP already in a *persisted* mode
  and a dead-zone reading (e.g. a restart in daylight with a stale night
  config), and both self-healing probes (periodic reconfirm, sustained
  brightening) are gated on `cur==DN_NIGHT` — which `DN_UNKNOWN` never
  satisfies. Result: a camera could render night video (or day, in the
  inverse case) indefinitely after a reboot, with a perfectly healthy
  thread producing zero log lines, only discovered live on a T31 that
  restarted at 09:23 in broad daylight and stayed dark for hours.
  Once the boot-settle window ends still undecided, the thread now adopts
  the persisted `image.running_mode` as its internal state (the ISP is
  already running it, so nothing switches) so the normal in-mode triggers
  and probes arm. Since an adopted night is a guess rather than a
  measurement, its first day-pipeline verify probe fires within 5 minutes
  (or sooner if `night_reconfirm_s` is set lower) — once, even when
  periodic reconfirm is disabled. Pre-existing gap, not a v1.7.4
  regression; v1.7.4 only happened to be the build running when a restart
  finally landed in the dead-zone. Verified in `timpsd-sim` replaying the
  exact incident (adopts, verifies, and reaches day in ~25s versus
  indefinitely stuck before) plus a one-shot-probe-with-reconfirm-disabled
  case and two clean (non-dead-zone) boots showing zero adoption noise.

## [1.7.4] - 2026-08-03

### Fixed
- **Day/night baseline drift ratcheting into an overnight flap loop.** The
  v1.7.3 hardening's upward-only EMA baseline drift tracked raw gain ticks;
  noisy night AGC ratcheted the baseline to its noise ceiling, causing
  night↔day to flap every few minutes to every hour, all night, on real
  cameras — worse than the bug it was meant to fix. Replaced with a
  night-only smoothed gain driving a slow, symmetric baseline drift, an
  edge-armed brightening probe that disarms after a failed attempt, and an
  8s post-probe AE-stability gate so a lit room's exposure-convergence
  transient can't kill a legitimate probe. Verified against the exact
  logged flap pattern in `timpsd-sim`: zero flaps over 3 minutes where
  v1.7.3 flapped every 1-2 minutes.

## [1.7.3] - 2026-08-02

### Fixed
- **Adaptive night→day threshold too strict for a real light source.** Two
  live incidents: a basement whose only light dropped gain to 65% of a
  cleanly-sampled night baseline (never crossing `day_gain_pct`'s 60% bar),
  and a room whose baseline was sampled mid-lighting-transition
  (unrepresentatively low). The trigger is now floored at
  `total_gain_day_threshold`, the baseline drifts toward observed gain
  instead of staying fixed, and a "sustained brightening" probe forces an
  early day-pipeline recheck instead of waiting up to `night_reconfirm_s`.
  (Superseded by the fix in 1.7.4 above once this introduced its own
  regression.)
- `night_baseline`/`day_trigger` (the adaptive values currently in effect)
  are now exposed read-only in `GET /control` and the `/events` SSE push.

## [1.7.2] - 2026-08-02

### Fixed
- `boot_settle_s`/`boot_settle_max_s`/`boot_stable_pct`/`night_reconfirm_s`
  (new in 1.7.1) were live-settable but never actually appeared in the
  `GET /control` status JSON — a separate hand-written serializer had its
  own hardcoded field list, unrelated to the settings path.

## [1.7.1] - 2026-08-02

### Fixed
- **False night-mode latch surviving a reflash into broad daylight.** A
  fixed 5s post-boot settle window was too short for a cold/freshly-
  reflashed sensor's AE to converge, so a transient gain spike could
  commit straight to night regardless of real daylight and never recover.
  `boot_settle_s`/`boot_settle_max_s`/`boot_stable_pct` now wait for
  several consecutive gain readings to actually stabilize before trusting
  the first decision, and a new `night_reconfirm_s` periodically forces a
  real day-pipeline probe so an already-latched false night self-heals
  instead of requiring a manual `/control` override.

## [1.7.0] - 2026-08-02

### Added
- **HTTP Digest authentication** (RFC 7616 `qop=auth` + legacy RFC 2069)
  alongside the existing Basic auth, for both the HTTP preview endpoints
  and RTSP.
- **Read-only encoder telemetry** via `IMP_Encoder_Query`: per-channel
  queue/buffer stats (`registered`, `left_pics`, `left_stream_bytes`,
  `left_stream_frames`, `cur_packs`, `work_done`) and, on T31,
  `ave_bitrate` from `IMP_Encoder_GetChnAveBitrate`, exposed as a new
  `"encoder"` object in `GET /control`.
- **Per-client adaptive fMP4 frame-dropping** on weak links: a slow
  `/stream.mp4` client freezes on its last frame and resumes cleanly at
  the next keyframe instead of stalling every subscriber, with drop stats
  (fps/kbps/resolution/drops) visible in status. Defaulted on once
  hardware-verified.
- **Live IVS motion sensitivity** via `IMP_IVS_SetParam` (no grid rebuild
  needed for a sensitivity-only change), and opt-in AEC
  (`IMP_AI_EnableAec`) for the backchannel.
- `USE_RECORD`/`USE_TIMELAPSE` compile-time flags to shrink SD-less builds;
  `{fpsN}`/`{bitrate}` OSD placeholders for a specific stream's measured
  throughput.

### Changed
- **`rotation=180` removed on classic-API SoCs** (T10/T20/T21/T23/T30/T31/
  C100), since it's mechanically identical to `image.hflip`+`image.vflip`
  there — then **restored specifically for T40/T41**, which have a genuine
  per-channel I2D-based 180° distinct from their (global) hflip/vflip
  registers. Final `caps.rotation`: T31 `[0,90,270]`, T40/T41
  `[0,90,180,270]`, no-rotation SoCs `[0]`.
- `sendmmsg`-batched UDP video RTP per access unit, table-driven
  `/control` key lookup (~17KB smaller `.text`), explicit per-thread-type
  stack sizes, and a just-in-time timelapse hub subscription (was held
  24/7) — all throughput/footprint work with no behavior change.

### Fixed
- **T31 FS-rotate / T23 SW-rotate crash safety.** A rotation request
  outside the vendor-safe envelope (64-aligned & ≤1280x704 & ≤15fps for
  T31; 16-aligned & ≤704x576 & ≤15fps for T23) used to silently fall back
  to an oversized/misaligned software path that then failed encoder
  bring-up and took the **entire multi-stream pipeline** down — reproduced
  live via a rotation the `/control` API had itself accepted and
  persisted. Both platforms now refuse an out-of-envelope rotation and
  bring that one stream up unrotated instead; a `SetChnRotate`/
  `YuvInit` failure is likewise isolated to the affected stream rather
  than aborting the whole daemon.
- **A batch of RTSP/RTP/RTCP/SDP conformance fixes** found across several
  review rounds: `SET_PARAMETER` answered as a keepalive (200, RFC 2326
  §10.9) instead of 405; idle TCP backchannel-only sessions reaped after
  the standard timeout (previously immortal, since they never trip the
  media-write timeout other TCP sessions rely on); `Content-Length`
  request bodies actually consumed so the byte stream stays framed;
  `rtsps://` scheme stripped so TLS clients resolve the right stream;
  unsupported `Require:` feature-tags answered 551; Digest `uri=`
  verified against the real request-target; orphaned UDP sessions reaped
  at 2x the advertised timeout; `CSeq` echoed on every error response;
  `HEAD` answers `GET`'s headers with no body (RFC 7231 §4.3.2); plus
  fixes for SDP truncation/`Content-Length` mismatches, `FD_CLOEXEC` on
  accepted sockets, and several `/control` JSON-encoding hardenings
  (control-char/UTF-8 handling, `\uXXXX` decoding, failing closed instead
  of shipping truncated JSON).
- **Motion detection**: IVS grid now uses pre-rotation frame dimensions
  with a transposed grid on the T23 SW-rotate path (was building the grid
  in the wrong orientation), sensitivity changes that map to the same IVS
  level are deduped, and `cooldown_ms` is floored and persisted correctly.
- **Day/night**: a queued ISP `running_mode` change is now actively
  latched (`fs_kick_running_mode`) instead of only taking effect on the
  next unrelated encoder event; a pre-switch hysteresis window (raptor-
  style) replaced blind reassertion; a transient reading during the AE
  settle window is now ignored instead of seeding a false decision.
- **Recording/timelapse**: `record.audio` toggles the hub subscription
  live; a dropped packet (not just a missed keyframe) now requests a
  rate-limited IDR; `gethostname()` results are NUL-terminated before use
  in path templates.
- Video/JPEG AU buffers now size from the actual frame instead of a fixed
  estimate (fixes both the 1.6.1 sub-stream stall class and an analogous
  JPEG/snapshot/MJPEG stall once a scene crosses a detail threshold — see
  below), audio speaker/backchannel gating and resampling edge cases, and
  a `/control` re-POST of an unchanged `image.running_mode` now still
  re-drives the ISP (some SoCs need the write even when the value didn't
  change).

## [1.6.4] - 2026-07-29

### Added
- **`{bitrate}` OSD text placeholder.** Reports the live measured
  throughput (kbit/s) of the monitored stream, mirroring the existing
  `{fps}` placeholder's mechanism and style. Shows `0` when the encoder
  has no active consumer rather than a frozen last-seen value.

### Fixed
- **JPEG snapshot/MJPEG/WebUI preview going permanently dark once a scene
  crosses a detail threshold.** `jpeg_thread()`'s buffer starts at a
  ~0.5 byte/pixel estimate, bounded to `[MS_JPEG_BUF_MIN, MS_JPEG_BUF_MAX]`
  — same class of bug as the `[1.6.1]` AU buffer fix, but unlike a video
  frame, nothing here shrinks a JPEG scene back down once it crosses the
  estimate (e.g. daylight bringing out more detail than a dawn/dusk
  scene), so every frame overflowed and got dropped forever from that
  point on: snapshots returned "no frame", MJPEG and the WebUI preview
  went dark, while day/night switching and RTSP video kept working fine
  (different codec/buffer entirely) — easy to mistake for a day/night bug
  from the WebUI. Now sums the pack lengths before assembly and grows the
  buffer (bounded by `MS_JPEG_BUF_MAX`) to fit the real frame, same as the
  AU buffer. Verified on real hardware (Galayou Y4, T23n): `/snapshot.jpg`
  went from HTTP 503 "no frame" with continuous buffer-overflow log spam
  to a valid ~450KB daytime JPEG, no overflow since.

## [1.6.3] - 2026-07-28

### Fixed
- **1-3s browser preview lag behind the physical camera (noticeable during
  PTZ).** Two independent contributors on the encoder→browser path:
  - The embedded MSE player JS (`src/mp4/httpd.c`) only corrected its
    live-edge position once it drifted more than 6s behind, jumping back
    to just 1s behind even then. With `autoplay`, the browser starts
    playback wherever it first had enough buffered data (typically 1-3s)
    and then plays at a flat 1x forever — that initial gap never shrunk on
    its own. Replaced the dead-zone jump with active drain:
    `playbackRate` now scales 1.0→1.3x with how far behind live the
    player is, settling at a steady-state ~0.5s behind live (kept as
    jitter margin), with a hard seek reserved for a large post-stall
    drift (>4s).
  - The HTTP/fMP4 listener never set `TCP_NODELAY` (the RTSP listener
    already did) — Nagle's algorithm held small fragments until the prior
    write was ACKed, adding up to ~200ms of pure transport latency per
    fragment, compounding across every video/audio fragment sent.
  GOP size/B-frames/rate-control and the encoder polling loop were
  reviewed and ruled out: no B-frames are used (no look-ahead latency),
  the poll timeout only bounds idle-wait and never delays an
  already-produced frame, and the fanqueue has no steady-state queuing
  delay. GOP interval affects only startup/post-drop recovery, not
  in-progress PTZ framing, so it was left unchanged (shrinking it further
  would trade bandwidth/quality for no benefit here). Verified on real
  hardware (Cinnado D1 T31L x2).

## [1.6.2] - 2026-07-28

### Fixed
- **Video encoder permanent stall when a framesource enable silently
  fails.** `fs_use()` never checked `IMP_FrameSource_EnableChn()`'s return
  value, and the enable only fires on the 0→1 user-count edge. If it fails
  once — or "succeeds" without actually arming the channel, a failure
  class this file already documents twice (the AI watchdog, and the T31
  `nrVBs` case) — `video_thread()`'s `StartRecvPic` still reports success
  and `PollingStream` spins at `rc=-1` forever: the encoder never produces
  another frame. With a client still subscribed, the idle-stop debounce
  never fires, so the framesource never gets a fresh enable attempt — a
  permanent stall recoverable only by restarting the daemon. Observed live
  on a T31L main channel (`nrVBs=1`, i.e. no buffer slack) after streaming
  correctly for hours; the sub-stream (independent framesource, ≥2
  buffers) kept working the whole time. Now mirrors the existing AI
  watchdog for video: after `MS_VIDEO_WATCHDOG_ITERS` (~5s) consecutive
  `PollingStream` misses, force a real Stop/Disable/Enable/Start cycle
  instead of spinning; `EnableChn` failures are now logged instead of
  silently swallowed. Verified on real hardware (Cinnado D1 T31L): full QA
  pass (77 PASS / 0 FAIL) after the fix, including 20/20 clean TCP and
  20/20 clean UDP reconnect cycles through the previously-fragile
  enable/disable edge.

## [1.6.1] - 2026-07-28

### Fixed
- **Sub-stream permanent stall on a large (e.g. complex-scene) IDR frame.**
  `video_thread()`'s AU assembly buffer starts at a ~0.5 byte/pixel
  estimate, clamped to `[MS_AU_BUF_MIN 128KB, MS_AU_BUF_MAX 1MB]`. For a
  small sub-stream (e.g. 640x360) that estimate sits at the 128KB floor,
  which a complex-scene IDR can exceed. The overflow handler used to drop
  the oversized frame and force a fresh IDR — but an IDR is the *largest*
  frame type, so the forced replacement overflowed too, forced another
  IDR, overflowed again: a permanent self-reinforcing stall that delivered
  zero decodable video on that stream from the first oversized frame
  onward (found as "only one stream works" — main stayed fine on its
  larger 1MB cap). Now sums the pack lengths before assembly and grows the
  buffer (bounded by `MS_AU_BUF_MAX`) to fit the real frame instead of
  truncating it; the `IMP_Encoder_RequestIDR()` call on the (now
  last-resort, >1MB-AU-or-failed-realloc-only) overflow path is dropped,
  since forcing an IDR there was the actual cause of the stall — a dropped
  frame already recovers via the existing
  `fanqueue_take_dropped_key`/`hub_request_idr` path on a real client.
  Verified on real hardware (Galayou Y4, T23n): the sub stream went from
  zero video across 20+ minutes and every reconnect to streaming
  correctly (640x360 h264@25+aac, IDR ~99KB) alongside the main stream,
  with zero overflow events since boot.

## [1.6.0] - 2026-07-27

### Added
- **Native speaker output (`IMP_AO`) — timps now owns the camera speaker
  directly, no more `/bin/iac`.** New `src/rtsp/speaker.c` is the sole
  `IMP_AO` owner and arbitrates two producers: the ONVIF **backchannel**
  (live RTP → PCM, always preempts) and a **system-sound play queue**
  driven by a FIFO at `/run/timps/audio_out` taking `PLAY url=<path>
  [vol= gain= rate= format= loop= delay=]` / `STOP` lines — the same
  protocol prudynt/raptor's `/usr/sbin/play` wrapper already speaks, so
  the WiFi captive-portal prompts, the post-upgrade chime and the ESPHome
  `media_player`/TTS integration all get a working speaker on a timps
  image for free. The play queue decodes Ogg-Opus (`opusfile`, gated on
  new `USE_PLAY_OPUS` like `USE_BC_AAC` gates the AAC backchannel), WAV,
  raw PCM16 and G.711 µ/A-law. New `USE_PLAY`/`USE_PLAY_OPUS` build flags
  (off by default; `USE_BACKCHANNEL` no longer needs `/bin/iac` present at
  all — `bc_available()` is always true once built in). New
  `hal_ao_open/write/close/set_vol/set_gain` in the HAL mirror the
  existing `IMP_AI` bring-up (rate-fallback loop, lazy open/close); a new
  `src/codec/resample.c` (extracted from `backchannel.c`) is shared by
  both producers.
- **Live speaker volume/gain + WebUI-driven system-sound play.**
  `audio.spk_volume`/`spk_gain` were parsed and persisted but never
  actually reached the hardware before; now every `IMP_AO` open applies
  them, and `POST /control {"audio":{"spk_volume":..,"spk_gain":..}}`
  writes through live (`caps.audio` gains the two keys, gated on an AO
  pipeline being compiled in). `GET /control` gains
  `caps.play={available,sounds:[...]}`, enumerated from
  `/usr/share/sounds` (`.wav` always listed since the µ-law/PCM decoder
  needs no library; `.opus` only when `USE_PLAY_OPUS` is actually built,
  so the list never offers a file this exact build can't decode).
  `POST {"speaker":{"play":"<file>"}}` / `{"stop":1}` enqueues on the FIFO
  after validating the name against that directory (rejects `/`, `..`,
  non-regular files) — this is what drives the thingino WebUI's
  test-sound dropdown and live speaker volume slider.
- **Day/night: time-window and sunrise/sunset override modes.** The
  native detector could previously only decide from the ISP sensor
  (`total_gain`/brightness). `daynight.mode` (`sensor`/`time`/`sun`, a
  string token) adds two sensor-independent modes: **`time`** forces by
  the local wall clock — a fixed `[time_night_start .. time_day_start]`
  `"HH:MM"` window, wrapping past midnight (e.g. night 20:00, day 06:30).
  **`sun`** forces by today's real sunrise/sunset for
  `sun_latitude`/`sun_longitude` via the standard low-precision sunrise
  equation (pure math, UTC epoch throughout), each edge shiftable by
  `sun_sunrise_offset_min`/`sun_sunset_offset_min` (negative allowed);
  polar day/night degenerate cases fall back to permanent day/night
  instead of NaN. `sensor` stays the default and its gain/brightness
  branch is untouched; `time`/`sun` reuse the existing switch + minimum-
  dwell machinery, and `daynight.enabled=0` (manual) still suppresses
  forcing in all three modes. `GET /control` exposes the new config
  fields plus today's computed sunrise/sunset (`sun_computed_sunrise`/
  `sunset`, local `"HH:MM"`) so a UI can sanity-check the configured
  lat/long before trusting it.
- **T31(L) `nrVBs` buffer-count override** (`video.buffers`, raptor-style):
  the T31 non-scaled-channel safety clamp (see Fixed below) now only
  applies to the *default* buffer count — an explicit `buffers=` in
  `timps.conf` is trusted as-is (with a warning, since a bad value fails
  silently down in the kernel/dmesg), letting a board/sensor combination
  that doesn't hit the constraint opt out without patching code.

### Fixed
- **Play-file tail no longer cut short on normal end-of-clip.** Two
  layered bugs, both in `hal_ao_close()`'s drain path: (1) it
  unconditionally discarded the AO ring buffer (`IMP_AO_ClearChnBuf`) on
  every close, including a clip finishing normally, not just on
  stop/preempt — a `drain` flag now distinguishes the two, discarding
  immediately only on stop/preempt/backchannel-takeover. (2) the drain
  path's fixed sleep (one `MS_AI_FRM_NUM`-period ring's worth, ~0.24 s)
  assumed that was the whole story, but the IMP AO keeps its own
  playback cache on top of that ring — the real residual is ~0.7 s, so
  the fixed sleep still closed the channel ~0.5 s early (e.g.
  "Configuration portal is down" stopped after "portal"). Now uses
  `IMP_AO_FlushChnBuf`, the SDK's "wait for the last segment to finish
  playing" primitive, which blocks until the whole cache has actually
  reached the DAC regardless of depth. Verified acoustically via RTSP
  mic loopback across clips from 0.6 s to 2.7 s, before/after audible
  span matched against each source clip's real content window.
- **T31(L) `nrVBs=1` clamp scoped to `PLATFORM_T31` only.** The non-
  scaled-channel buffer-count safety clamp in `fs_create()` (shared
  across every SoC family) fired for any chip's channel requesting >1
  buffer at native sensor resolution, but the kernel constraint requiring
  it was only ever observed on T31(L) — T10/T20/T21/T23/T30/T40/T41/C100
  now keep their untouched 2-buffer default.

## [1.5.0] - 2026-07-26

### Changed
- **Default `http.port` moved 8080 → 8880.** Port 8080 clashed with the
  ONVIF daemon (`onvif_srvd`), which also listens there; whichever bound
  first won, so ONVIF could fail to start when timps grabbed 8080. timps now
  defaults to `8880`, leaving 8080 to ONVIF. The port is still configurable
  via `http.port`; the WebUI reads the live port from `/x/timps-token.cgi`,
  so browser pages follow automatically.
- **Sub-stream OSD default `font_size` 24 → 12 px.** Better fit on typical
  sub-stream resolutions; still an absolute px value, not auto-scaled — see
  `osd1.*` in `timps.conf.example`.

### Added
- **Optional image rotation** (`USE_ROTATE`/`USE_SW_ROTATE` build flags,
  `videoN.rotation` config key: `0|90|180|270`). 180° works on every SoC;
  hardware 90/270 on T31/T40/T41; software 90/270 (CPU transpose + SW
  JPEG/OSD) on T23. Restart-required; downstream (encoder, RTSP SDP,
  fMP4/MP4, OSD, snapshots) all use the post-rotation dimensions via one
  helper. Off by default, ~0.2 KB when disabled. Known limitation: on T31,
  90/270 can't carry a hardware OSD/privacy overlay (libimp IPU-OSD stride
  bug) — see `docs/rotation.md`.
- **Optional ONVIF audio backchannel** (`USE_BACKCHANNEL`/`USE_BC_AAC` build
  flags, `audio.backchannel`/`backchannel_codec`/`backchannel_rate` config
  keys, `caps.backchannel.available`). Implements ONVIF Profile T two-way
  audio: an RTSP client streams RTP audio (PCMU/PCMA pure-C, or AAC via
  libhelix-aac) to the camera; timps decodes + resamples it to PCM16 and
  pipes it to `/bin/iac -s` (thingino's `ingenic-audiodaemon`) — timps itself
  never opens `IMP_AO`, so it works identically on every SoC as long as the
  audiodaemon is installed. See `docs/backchannel.md`.
- **Optional HTTPS + SRT (compile-time gated).** New `USE_TLS` (mbedTLS) and
  `USE_SRT` (libsrt) build flags, auto-enabled by the buildroot package
  selection (`BR2_PACKAGE_MBEDTLS` / `BR2_PACKAGE_LIBSRT`) - if the lib isn't in
  the image nothing changes. `USE_TLS`: a small mbedTLS wrapper (`src/tls.c`)
  behind which the HTTP server can run **HTTPS** (`http.https` + `http.tls_cert`
  / `http.tls_key`); the httpd I/O now goes through a TLS-aware send/recv layer
  that is byte-for-byte the old plain path when `USE_TLS` is off. `USE_SRT`:
  MPEG-TS over SRT output in listener mode (`src/srt.c`, `srt.enabled`/`port`/
  `channel`/`latency_ms`/`streamid`/`passphrase`) served from the hub like the
  recorder. Config keys for RTSPS (`rtsp.tls`/`rtsp.tls_port`) are parsed and
  reserved. NOTE: the TLS and SRT code paths cannot be built in the x86 sim
  (no mbedTLS/libsrt) - the default build stays verified; the TLS/SRT paths and
  the hand-rolled TS muxer need on-device verification.
- **Local recording to SD** (`record` section + `/control` action): records one
  video stream (+AAC audio) to `<dir>/<hostname>/records/<strftime>.mp4` as
  fragmented MP4, reusing the `/stream.mp4` muxer (`src/record.c`). Modes:
  `continuous` or `motion` (pre-roll ring from the keyframe before the trigger +
  `post_roll_s` after the last motion). Segments rotate every `record.segment_s`
  at a keyframe; oldest files are pruned to keep `record.min_free_mb` free.
  `GET /control` reports a `record` status object (recording/channel/mode/bytes/
  free_mb/file) and `caps.record`; `{"record":{"active":1|0}}` is a manual
  start/stop override (the WebUI record button). thingino path defaults
  (`/mnt/mmcblk0p1`, `<host>/records/` tree). Verified end-to-end in the x86 sim
  (valid MP4 segments via ffprobe).
- **Privacy cover masks** (`privacy` section, `/control` + config): solid filled
  rectangles per video stream (`privacy<S>.<N>.{enabled,x,y,w,h,color}`, up to
  `MS_MAX_PRIVACY` per stream) that black out sensitive areas, implemented as IMP
  OSD cover regions in the per-stream OSD group. Applied LIVE (create/show/hide/
  move without a restart, as long as OSD or a privacy region was on at startup)
  and persisted. `GET /control` dumps the `privacy` tree and advertises
  `caps.privacy = {available, max_regions}`. Replaces the prudynt-era WebUI
  privacy page's dependency on the `json-prudynt.cgi` bridge. NOTE: the IMP cover
  region call in `imp_osd.c` uses the common SDK form and needs on-device
  verification against the exact `<imp/imp_osd.h>` coverData layout.
- **Token now also unlocks HTTP media viewing** (`USE_CONTROL` builds): the
  `/control` token (per-boot `http.token_file` + optional persistent
  `http.token`, same constant-time check) is accepted on `/stream.mp4`,
  `/stream.mjpeg` and `/snapshot.jpg` (incl. `?chn=N`) as `?token=` — the
  only form an `<img>`/`<video src>` can use — or `X-Timps-Token`. This lets
  the thingino WebUI previews load the streams DIRECTLY from the HTTP port
  (no on-device proxy CGIs) even with `http.user` set. Media access is now
  localhost ∨ token ∨ Basic ∨ open-when-no-user — the existing rules are
  unchanged, the token is a pure addition; it still never unlocks RTSP, and
  non-media paths (`/` player, bogus paths) are NOT unlocked by a token.
  The media endpoints also answer the CORS `OPTIONS` preflight now, and
  `/stream.mjpeg` + `/snapshot.jpg` responses carry
  `Access-Control-Allow-Origin: *` like `/stream.mp4` always did, so
  cross-origin `fetch()`es of all three work. Caveat as with `/events`: a
  query token can end up in access logs — accepted on a LAN.
- **`GET /events` SSE push stream** (`USE_CONTROL` builds): a long-lived
  `text/event-stream` that PUSHES JSON state instead of being polled —
  `event: motion` (the `/control` motion object, emitted when the active
  grid/enabled/geometry/sensitivity changed), `event: daynight` (the
  `/control` daynight object, on a mode flip or ≥1 % brightness / ≥5 % gain
  move) and a periodic `event: stats`
  (`{"uptime_s","clients","video":[{"chn","subs","fps"},…]}`, every
  `events.stats_ms`). `?stream=motion,daynight,stats` filters the types
  (default all). Same auth as `/control` (localhost / token / Basic, CORS +
  OPTIONS preflight); the token is also accepted as `?token=` because
  EventSource cannot send headers. On connect: `retry: 3000`, a
  `: connected` comment and the full current state once; afterwards
  per-connection dedup (last-sent snapshot per event type) plus a `: ping`
  keepalive (~12 s) that doubles as dead-client detection. New tiny notify
  hub `src/events.c/.h` (generation counter + `CLOCK_MONOTONIC` condvar):
  `events_notify()` is called from the IVS result thread (grid changed,
  start/stop), the day/night sampler (real changes only) and `/control`
  writes to `motion.*`/`daynight.*`/`image.running_mode`; it is a no-op stub
  without `USE_CONTROL`, so every build permutation still links. Config:
  `events.enabled` (default 1), `events.stats_ms` (default 2000, 0 = off),
  `events.max_clients` (default 8; beyond → `503`, so an /events flood
  cannot exhaust the HTTP connection threads). The status-object JSON is
  built by shared helpers (`control_motion_json`/`control_daynight_json`),
  so `/control` and `/events` emit identical shapes by construction. The
  thingino WebUI preview overlay now subscribes to `?stream=motion` (with a
  4 Hz `/control` polling fallback) instead of polling.
- **Grid motion detection (IMP_IVS)**: the single detection ROI became a
  configurable `motion.cols` × `motion.rows` GRID of IMP_IVS move-ROIs laid
  evenly over the `motion.monitor_stream` frame (integer pixel split, the last
  row/column absorbs rounding; cell index row-major = `row*cols+col`).
  `cols*rows` is clamped to the SDK's compile-time `IMP_IVS_MOVE_MAX_ROI_CNT`,
  taken from the `imp_ivs_move.h` being built against via the new
  `motion_caps.h` (`MOTION_AVAILABLE`/`MOTION_MAX_CELLS`): 52 on most SDKs,
  **4** on the old T10/T20 3.9.0 SDK (grid defaults 5×5, 2×2 on 4-cell SDKs).
  The UI sensitivity 0..255 maps to IMP's 0..4 normal-camera range (one global
  value for all cells for now). SDKs without the move API compile a no-op stub
  and report the feature unavailable. The IVS group is now explicitly bound to
  the monitor stream's FrameSource (FS→IVS, unbound on stop) and the move
  interface is released via `IMP_IVS_DestroyMoveInterface` (both were missing).
- **Live motion control + status**: `motion.enabled`/`sensitivity`/`cols`/
  `rows`/`monitor_stream` are settable via `/control` and applied LIVE — the
  HAL cleanly stops and recreates the IVS channel (move params are create-time
  attributes). `cooldown_ms`/`on_motion` stay config-file only (`on_motion`
  runs through `system()`). `GET /control` gained `caps.motion`
  (`available`, `max_cells`) and a read-only `motion` status object:
  `{"available","enabled","cols","rows","max_cells","sensitivity",
  "monitor_stream","active":[0/1,... row-major, length cols*rows],
  "last_ms"}` (`last_ms` = ms since the last motion event, -1 = never). The
  thingino WebUI polls it directly on `:8880` with the `/control` token to
  draw a live grid overlay on the preview.

- **Token auth for `/control`**: the endpoint now allows any one of localhost
  (unchanged), a valid token, or HTTP Basic (unchanged). Tokens travel as an
  `X-Timps-Token:` header (preferred) or `?token=` query parameter, are
  compared in constant time and only unlock `/control` — never the streams.
  A random 128-bit per-boot token is generated from `/dev/urandom` and
  published to `http.token_file` (default `/run/timps.token`, mode 0640,
  `""` disables) so a local privileged helper (the thingino WebUI) can hand it
  to its authenticated browser session; an optional persistent `http.token`
  secret is also accepted for remote automation and is never written to the
  token file.
- **CORS on `/control`**: `OPTIONS` preflight (204, answered before auth — a
  preflight carries no credentials) and reflection of the request's `Origin`
  (+ `Vary: Origin`, `Access-Control-Allow-Headers: X-Timps-Token,
  Content-Type`, methods, max-age) on `/control` responses, so a browser page
  on another port (WebUI on `:80`) can call `:8880/control` directly with the
  token. `Access-Control-Allow-Credentials` is deliberately never sent.

### Fixed
- Target builds now pass `-I$(IMP_INC)/imp` too: the T10/T20 3.12.0 IVS
  headers include `<imp_ivs.h>` without the `imp/` prefix and did not resolve
  with `-I$(IMP_INC)` alone.
- **Command injection hardening**: `daynight.switch_cmd` (day/night switch
  script) and `motion.on_motion` (motion-trigger script) now run via
  `fork()`+`execlp()` instead of `system()` — no shell, so a value containing
  shell metacharacters just fails to exec instead of running as injected
  commands. Both keys were already config-file-only (never settable via
  `/control`), but this closes the gap for anyone with config-file write
  access. See `dev_notes/SECURITY_AUDIT_2026-07-23.md`.
- **Value clamping**: `audio.gain` now clamps to the IMP-documented mic PGA
  range (0..31, was 0..100); `audio.volume`/`alc_gain`/`spk_volume`/
  `spk_gain` and OSD `logo_w`/`logo_h`/`outline` are clamped against their
  real IMP/rendering limits so out-of-range `/control` values can't wrap or
  blow up an allocation.
- `hal_get()`'s return value is now NULL-checked before its first use at
  startup (previously dereferenced once, in the startup log line, before the
  existing check further down).

## [1.2.0] - 2026-07-11

### Added
- **Full ISP image control via `/control`**: the `image` section now covers the
  complete tuning set — brightness, contrast, saturation, sharpness, hue, h/v
  flip, running_mode, anti-flicker, AE compensation, max analog/digital gain,
  sinter & temper (noise), DPC, defog, DRC (WDR), highlight-depress (tone),
  backlight compensation and white balance (mode + R/B gain) — applied live via
  the matching `IMP_ISP_Tuning_*` call. A compile-time per-SoC capability matrix
  (`isp_caps.h`, T10–T41 + C100) is reported as `caps.image` so a UI can grey
  out what a given SoC cannot do; unsupported keys still persist.
- **Full audio control via `/control`**: live mic volume, gain, ALC gain,
  high-pass filter, AGC (+ target level / compression), noise-suppression, and a
  **live mic mute** (`audio.mute` — captured frames are dropped before the
  encoder/hub, no restart). Capability matrix in `audio_caps.h` → `caps.audio`.
  Codec / sample-rate / bitrate / channels persist and apply on restart.
  Speaker & forced-stereo have no IMP-AO path and are reported unsupported.
- **Full encoder & sensor control** (persist + restart): `video.N` accepts the
  whole per-stream key set (codec, width, height, fps, bitrate, rc_mode, gop,
  max_gop, profile, qp, min/max_qp, rotation, buffers, enabled, rtsp_path) and a
  new `sensor` section (model, i2c_addr, fps, width, height). These never touch
  the running pipeline; `GET /control` flags them in `caps.restart` and dumps
  the current values so a UI can populate.
- **Per-stream OSD**: every video stream has its own independent overlay set
  (`osd.items[stream][item]`). Canonical keys `osd<S>.<N>.<field>` (e.g.
  `osd0.0.text`, `osd1.2.x`); legacy `osd<N>.<field>` keys still load and mirror
  onto every stream. `/control` accepts `"osd0"/"osd1"` objects (live via
  `imp_osd_apply(stream,item)`) and still the shared legacy `"osd"` object.
- **OSD text outline/stroke**: new per-item `outline` (width px, 0 = off,
  default) and `outline_color` (`0xAARRGGBB`, default black). The TTF and
  embedded-bitmap rasterizers dilate the glyph coverage and blend the stroke
  under the fill; the region grows by the outline width. `caps.osd` lists the
  new leaves.
- **Day/night measurement exposed** (`daynight_get_status()`): the detection
  thread derives the **total gain** from the isp-m0 gain fields (IMP log2 units)
  converted to the `GetTotalGain` [24.8] linear scale (256 = 1×, matching what
  prudynt/raptor report), keeps sampling in manual mode, and `GET /control`
  reports `daynight: {enabled, mode, brightness%, total_gain}` (−1 = unknown; a
  stub answers unknowns without `USE_DAYNIGHT`).
- **System log output**: timps now also logs to syslog (tag `timpsd`) so
  messages appear in `logread` (the init script backgrounds timpsd, so its
  stderr is otherwise discarded). On by default; disable with
  `general.syslog = false`.

### Changed / Fixed
- **Idle CPU** (~19 % → ~0 with no clients): on-demand now stops the
  `IMP_FrameSource` channel (not just the encoder) once a stream has no
  subscribers — an enabled FrameSource kept capturing/piping frames through the
  FS→OSD→encoder groups in the libimp worker threads. Producer threads now block
  on a condition variable instead of a poll loop, and the OSD updater only
  renders while a stream has viewers. Reactivation is immediate; the monitored
  FrameSource is pinned while motion detection is enabled.
- **`GET /control` capabilities** now report `caps.{image,audio,osd,restart}` so
  UIs can present exactly what this build/SoC supports.

## [1.1.0] - 2026-07-11

### Added
- **Live control API** (`POST`/`GET /control`, compile flag `USE_CONTROL`, on by
  default). A nested JSON blob changes settings live *and* persists the changed
  keys back to the config file (atomic tmp+rename, comments/order preserved).
  Supported: `image` (brightness, contrast, saturation, sharpness, hue, hflip,
  vflip, running_mode), `audio` (volume, gain), `osd.N` overlays, `video.N`
  bitrate (persisted only — applies on restart). The legacy flat form and
  `{"force_mode":"day"|"night"}` still work. Requests from localhost bypass
  auth; remote access requires configured HTTP/RTSP credentials.
- **Native automatic day/night** (compile flag `USE_DAYNIGHT`, on by default). A
  background thread reads ISP brightness from `/proc/jz/isp/isp-m0` and applies
  threshold + hysteresis + transition-delay logic (ported from thingino's
  `daynightd`), switching via the board's `daynight day|night` script (IR-cut /
  IR-LEDs / colour). Runtime toggle through `/control`
  (`{"daynight":{"enabled":true|false}}`). New `daynight.*` config keys
  (`enabled`, `threshold_low`, `threshold_high`, `hysteresis`, `interval_ms`,
  `transition_s`, `switch_cmd`, `isp_path`).
- **Live OSD apply** (`imp_osd_apply`): OSD overlay changes made through
  `/control` are re-rendered on the running streams.
- `config_get_kv()` — read a config value back as a normalized string
  (used for change detection).

### Fixed / Hardened
- **`/control` change detection**: a value that does not actually change is no
  longer re-applied to the ISP nor rewritten to the config file. This stops
  clients that poll and re-post the same value every few seconds from hammering
  the ISP and, worse, rewriting the config on flash over and over.
- **`/control` input validation**: invalid values (`null`, `undefined`, empty)
  are rejected instead of being stored and parsed to `0`.
- **Config-injection defense**: persisted values are stripped of control
  characters and double quotes before being written to the flat config file.

### Build
- `USE_DAYNIGHT` added to the Makefile (target and host-sim recipes); both
  `USE_CONTROL` and `USE_DAYNIGHT` default on and can be disabled independently
  (`USE_CONTROL=0` / `USE_DAYNIGHT=0`), compiling the feature out entirely.

## [1.0.0]

### Added
- Initial import: Tiny IMP Streamer — pure-C RTSP + fragmented-MP4 + JPEG/MJPEG
  streamer for Ingenic SoC cameras, built straight on the vendor `libimp`
  (no live555 / libconfig / libwebsockets / libschrift). On-demand encoding,
  TrueType OSD, motion detection, RTSP-Digest / HTTP-Basic auth. Ingenic
  headers via the `ingenic-headers` submodule.

[1.2.0]: https://github.com/Lu-Fi/timps/compare/v1.1.0...v1.2.0
[1.1.0]: https://github.com/Lu-Fi/timps/compare/v1.0.0...v1.1.0
[1.0.0]: https://github.com/Lu-Fi/timps/releases/tag/v1.0.0
