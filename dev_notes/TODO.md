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
  measure I-frame windows.
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
