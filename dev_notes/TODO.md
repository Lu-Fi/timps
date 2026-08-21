# Open items

Working list. Newest block first; each entry says what is established and what
is still guesswork, so nobody has to re-derive it.

Background for the encoder block:
`dev_notes/T23_RATECONTROL_INVESTIGATION_2026-08-21.md`.

## Scheduled for the fable agent (start after the usage limit resets)

### Live and static setting of all rate-control values, incl. WebUI

Every rate-control value settable both statically (timps.conf / persist) and
live (no daemon restart), plus WebUI controls.

Static today: `bitrate`, `min_qp`, `max_qp`, `quality_lvl`, `change_pos`,
`i_bias_lvl`. Still hardcoded and to be added: `staticTime` (2), `frmQPStep`
(3), `gopQPStep` (15), `gopRelation` (0), `flucLvl` (0, H265 only).

Live today: nothing. All `videoN.*` keys are persist-only by design
(`control.h:44`).

SDK support, verified in the headers 2026-08-21:

| | SetChnAttrRcMode | GetChnAttrRcMode | SetChnBitRate | SetChnQpIPDelta |
|---|---|---|---|---|
| T23 | yes (H264 only) | yes | no | n/a |
| T31 | yes | yes | yes | yes |
| C100 | yes | yes | yes | yes |
| T40 | yes | yes | yes | no |
| T41 | **no** | yes | yes | no |

`SetChnAttrRcMode` takes `rcMode` plus the whole attribute union, so the entire
block goes in one call. T41 has no setter at all — everything except bitrate
stays restart-bound there.

Do the readback (below) first, so a live write can be verified against what the
encoder actually holds instead of assumed. Two header-derived hypotheses
already failed against measurement.

WebUI: show the per-SoC differences rather than silently ignoring them. A knob
that does nothing on this camera should say so.

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

### Read back the rate-control attrs via IMP_Encoder_GetChnAttrRcMode

timps calls none of the runtime rc APIs — no hits in `src/` for
`SetChnAttrRcMode`, `GetChnAttrRcMode`, `SetChnInitQP`, `SetChnMaxPictureSize`.
The readback is present on all five platforms and cannot change anything.

It is the missing diagnostic: `quality_lvl` moves the operating point (1709 ->
1243) but `change_pos` does nothing at all (1264/1264/1251 for 80/65/50), and
nothing gets the rate below ~990 although the same scene costs 278 at a fixed
qp. We have never verified that our writes arrive unaltered.

Cheaper check to run first, no code needed: sweep `min_qp` (20 -> 30 -> 38). If
the rate follows, the controller is quality-seeking and `min_qp` is the real
lever on T23 — a key we have had all along.

### Warn on the silent smart -> capped_quality substitution

`hal_ingenic.c:1123` lets `MS_RC_SMART` fall through into the
`CAPPED_QUALITY` case with no log line, while the mirror-image substitution on
the classic path (`capped_vbr`/`capped_quality` -> `vbr`) deliberately warns
once via `warned_capped`. One direction reports itself, the other does not.

### Reword the new-API warning for the three new keys

The one-shot LOGW added in `8f3c84c` says the keys "have no effect on this SoC
(new encoder API)". That implies impossibility, but
`IMP_Encoder_SetChnQpIPDelta` exists on T31 and C100 (absent on T40/T41), so
`i_bias_lvl` is merely unwired there. Distinguish "not supported by this SDK"
from "not wired up yet".

### Wire i_bias_lvl on T31/C100

Through `IMP_Encoder_SetChnQpIPDelta`, applied after `RegisterChn` the same way
`min_qp`/`max_qp` are applied via `SetChnQpBounds` (the `0a8bb9f` pattern),
non-fatal on rejection. Then narrow the warning above to the SoCs that
genuinely cannot honour it.

### Expose flucLvl for H265 on the classic path

Hardcoded to 0 in the H265 CBR/VBR/Smart fills; the last classic-path literal
with a documented domain ([0,4], "bitrate fluctuation relative to the
average"). It is the H265 counterpart to `i_bias_lvl`. Default 0 so nothing
changes untouched.

`frmQPStep` (3), `gopQPStep` (15), `gopRelation` (0) and `staticTime` (2) stay
alone for now — no documented ranges.

### Decide on the unreachable new-API rc fields

On the ENC_NEW_API path timps never touches `uMaxPSNR`, `uMaxBitRate`,
`eRcOptions` or `uMaxPictureSize`; they come from
`IMP_Encoder_SetDefaultParam` and their values are not even readable.
`uMaxPSNR` is the knob `capped_quality` is named after, so that mode is
currently indistinguishable from an unconfigurable VBR. `uMaxPictureSize` ties
into the IDR overflow warning around `MS_AU_BUF_MAX`.

Header-derived only, not verified against hardware.

### Document that videoN.bitrate means different things per SoC path

Classic: written straight into `maxBitRate`, so under VBR it is a hard ceiling.
New API: passed as `uTargetBitRate` while the real cap `uMaxBitRate` stays at
the SDK default. A user who learned the ceiling semantics on a T23 gets
something else on a T31. Either document it or unify via
`IMP_Encoder_SetChnBitRate(chn, target, max)` — note that call takes bit/s.

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
