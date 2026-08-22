# Open items

Working list. Newest block first; each entry says what is established and what
is still guesswork, so nobody has to re-derive it.

Background for the encoder block:
`dev_notes/T23_RATECONTROL_INVESTIGATION_2026-08-21.md`.

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

**Proposed minimal fix (not applied — sketch only):** give `hub_get_fps()`
the same staleness guard `hub_get_bitrate()` already has, i.e. in
`src/hub.c`:

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

## `{bitrateN}` placeholder is missing (per-channel bitrate OSD text)

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
  succeeds. New API: `qp` under non-fixqp reports "deferred" although a
  restart will not make it effective either. Documented in code, no lie in
  `deferred_keys` (the key DID reach the encoder), but "no effect in this
  mode" is not expressible. Accepted as-is.
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
- **`i_bias_lvl` on T31** via `SetChnQpIPDelta` — not just that the call
  returns 0, but whether the keyframe size actually moves. Sweep -3/0/+3 and
  measure I-frame windows. Also read `encoder.<n>.rc.ip_delta` after each
  write: classic iBiasLvl is a dimensionless level, iIPDelta a QP delta —
  the readback tells whether the 1:1 pass-through lands as sent, the
  I-frame sizes tell whether the sign convention matches the classic one
  (audit: not decidable from the headers).
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

### Report ignored unknown fields in POST responses

Measured against the simulator 2026-08-21: a POST containing only unknown keys
returns 422 `unknown_fields`, but a POST mixing a valid key with an unknown one
returns `ok:true, accepted:1` and drops the unknown key silently.
`{"quality_lvl":7,"quality_level":5}` therefore looks successful.

Add an `ignored:[...]` array without changing what gets applied. Same failure
class as the `min_qp` gap and the missing status-JSON fields.

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
