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

### Wiki: why T23 needs far more bandwidth than T31

Shorter than the investigation note and aimed at someone choosing or operating
a camera. Core point: the classic controller (T10..T30) targets a rate, the new
one (T31/C100/T40/T41) reacts to scene content.

Measured on two cameras, same sensor, same settings, comparable light:
within-level spread 37-42% on T31 against 0.2-4% on T23 across cbr, vbr and
smart — while the same T23 at a fixed qp spreads 30-47%, so the chip sees the
change and the controller erases it. Means 2091 against 1149 kbit/s.

Say what can be done: `quality_lvl` lowers the T23 operating point to 1243
(-41%), uniformly, in busy scenes too — not scene adaptation. And name the two
refuted hypotheses so nobody re-derives them from the header.

### Wiki: document the rate-control parameters with example images

Every value: what it does, range, which SoCs honour it, what it costs.
Illustrated with frames from the H.264 stream — **not** JPEG snapshots, those
come from a separate encoder and would show nothing.

Material captured on cam-kinder-links 2026-08-21: cbr/quality_lvl 2 (2091
kbit/s), vbr/5 (1381), vbr/7 (1243), fixqp 42 (278), each with a 1:1 640x360
centre crop.

**The motif has to be approved by the user before anything goes into the wiki.**
The existing frames are from a children's room and were captured for a
technical comparison, not for publication. Ask which scene to use, re-shoot if
needed.

Depends on the T23/T31 wiki entry above.

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
