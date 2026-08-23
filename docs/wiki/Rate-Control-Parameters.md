# Rate Control Parameters

This page documents every rate-control field the encoder HAL
(`src/hal/hal_ingenic.c`) knows about — what it does, its valid range,
which SoCs actually act on it, and what it costs. It is the detail page
behind [Rate Control and Bandwidth: T23 vs T31](Rate-Control-Bandwidth.md),
which explains *why* the classic and new-generation controllers behave so
differently; read that page first if you have not already.

All of these are `video<N>.*` keys and follow the same rules as the rest of
that section in the [Configuration Reference](Configuration-Reference.md#videon--per-stream-encoder-settings):
set in `timps.conf` or via `POST /control`, value stored and echoed back
either way. Until 2026-08-21 every one of them was persist-only (the value
never reached the running encoder before a restart). As of that date a
per-platform SUBSET applies live — see [Live vs. restart, per
SoC](#live-vs-restart-per-soc) below for exactly which keys, and
[`caps.video_live`](HTTP-Control-API.md#the-caps-object) for how a client
discovers the running build's own subset instead of hardcoding this table. Every field on
this page, live-capable or not, can also be *read back live* — see
[Readback](HTTP-Control-API.md#the-encodernrc-object--what-the-encoder-actually-holds)
below.

## Measured vs. header-derived

This matters enough to call out before the table: some of what follows was
**measured** against real hardware (`dev_notes/T23_RATECONTROL_INVESTIGATION_2026-08-21.md`),
and some is **derived from the Ingenic SDK header comments** and has not
been checked against a running encoder. The table marks each row. Two
header-derived predictions about these exact fields (a `quality_lvl` bitrate
floor and a `change_pos` savings threshold) were checked and turned out to
be wrong — see [Rate Control and Bandwidth](Rate-Control-Bandwidth.md#two-hypotheses-that-looked-right-and-were-not).
Treat every unmeasured row as a plausible reading of the header, not a
verified fact.

## The fields

| Key | Range / default | SoCs that honour it | Status | What it costs |
| --- | --- | --- | --- | --- |
| `video<N>.bitrate` | 16–50000 kbit/s | All | **Measured** — is the primary lever everywhere | Meaning differs by SoC path: on classic SoCs (T10–T30) it is written straight into the hardware ceiling (`maxBitRate`/`outBitRate`) and is a hard cap under `vbr`. On new-API SoCs (T31/C100/T40/T41) it is only the *target* (`uTargetBitRate`); the real ceiling (`uMaxBitRate`) stays at the SDK default and is not configurable through timps. A value that reads as "the cap" on a T23 is only "the target" on a T31. |
| `video<N>.rc_mode` | `cbr`\|`vbr`\|`fixqp`\|`smart`\|`capped_vbr`\|`capped_quality`, default `cbr` | All, with gaps | **Measured** (mode selection); **header-derived** (per-mode semantics beyond bandwidth) | Classic SoCs have no hardware equivalent for `capped_vbr`/`capped_quality` and fall back to `vbr` with a one-time warning. New-API SoCs currently map `smart` onto `capped_quality` with **no** warning — a known asymmetry, not yet fixed. See [Rate Control and Bandwidth](Rate-Control-Bandwidth.md) for what each mode actually buys you on each SoC generation. |
| `video<N>.qp` | 1–51 | All (only used when `rc_mode=fixqp`) | **Measured**, indirectly | Not a rate-control field in the normal sense — it disables rate control entirely and fixes QP. Useful as a diagnostic: the same T23 scene that costs ~990–2091 kbit/s under any rate-control mode costs 278 kbit/s at a fixed QP of 42, proving the encoder can see and react to the scene even when the rate controller chooses not to. |
| `video<N>.min_qp` | 1–51, default 20 | All — classic SoCs via the rate-control struct, new-API SoCs via a separate `IMP_Encoder_SetChnQpBounds` call after channel registration | **Measured** (T31, 2026-08-22) | A raised QP *floor* is a bitrate *ceiling* — counter-intuitive on first read, but a floor on how coarse the quantizer may get is a cap on how far the encoder can compress, so it can only spend more bits, never fewer, once the target itself is out of reach. Live-differential proof on a T31 substream: raising `min_qp` 20 → 40 cut the delivered bitrate 229 → 25 kbps (0.11×) with the `bitrate` target untouched — the bound genuinely constrains the encoder, it is not merely echoed back by `GET /control`. |
| `video<N>.max_qp` | 1–51, default 45 | Same as `min_qp` | **Measured** (T31, 2026-08-22) | The mirror image of `min_qp`: a lowered QP *ceiling* forbids the encoder from degrading enough to hit a starved bitrate target, so it binds only once the target is already below what the scene needs. Proof needs a starved target first — at a common 34 kbps target on the same T31 substream, ceiling 28 delivered 97 kbps while ceiling 51 delivered 14 kbps (6.93× apart), i.e. the tighter ceiling forced the encoder to spend far more than the unreachable target asked for. |
| `video<N>.quality_lvl` | 0–7, default 2 | Classic SoCs only (T10–T30), `vbr`/`smart` modes only. No effect on T31/C100/T40/T41 — logged once and ignored. | **Measured** | Moves the T23's whole operating point down: 1709 kbit/s at level 2 vs. 1243 at level 7 on the same scene (−27%; −41% against the shipped `cbr` default of 2091). This is a **uniform** shift, not scene adaptation — the within-level spread stays at 0.2–4% at every level, same as under `cbr`. It is not a hard bitrate floor either, despite what the header formula suggests — see the refuted hypothesis on the companion page. |
| `video<N>.change_pos` | 50–100, default 80 | Classic SoCs only, `vbr`/`smart` modes only. No effect on new-API SoCs. | **Measured** | No measurable effect on T23 at any tested value (80 / 65 / 50 all landed within 1251–1264 kbit/s at `quality_lvl=7`). Despite the header's description ("qp is adjusted once bitrate exceeds this percentage of the target"), do not expect tuning this to change anything until it has been re-verified on other SoCs or with `IMP_Encoder_GetChnAttrRcMode()` readback. |
| `video<N>.i_bias_lvl` | −3–3, default 0 | Classic SoCs (`vbr`+`cbr` modes) and T31/C100 (all modes, via `IMP_Encoder_SetChnQpIPDelta`, wired 2026-08-21). No effect on T40/T41 — their SDKs have no equivalent call, logged once and ignored. | **Measured** on T31/C100, `cbr` (2026-08-22): confirmed null; measured non-effect on T40/T41 | Header describes it as an I-frame QP bias. The value reaches the encoder correctly — `encoder.<n>.rc.ip_delta` echoes the configured value 1:1, in scale and sign, proving the pass-through itself is right — but a bracketed `-3/+3/-3` sweep on a T31 substream under `cbr` found no bitstream effect beyond measurement noise (mean keyframe size within ~0.1% across the swing, versus a 15% effect threshold): `SetChnQpIPDelta` is a no-op in `cbr` on this SoC. Not a timps bug — the value timps sends is the value the encoder reports back — but it means configuring this field on a T31/C100 `cbr` stream buys nothing today. Unmeasured under `vbr`/`fixqp` on T31/C100, and unmeasured on classic SoCs in either mode. |
| `video<N>.fluc_lvl` | 0–4, default 0 | Classic SoCs, H.265 channels only (exposed as a config key 2026-08-21 — was hardcoded at 0 before). No effect on new-API SoCs. | **Unmeasured** | "Bitrate fluctuation relative to the average" per the header — the H.265 counterpart of `i_bias_lvl`. Always restart-bound (see [Live vs. restart](#live-vs-restart-per-soc) below) — it is inherently an H.265-only field, and the classic live-apply path only ever covers H.264 channels. |
| *(hardcoded, classic path only)* `staticTime` | fixed at 2 (seconds) | Classic SoCs | Header-derived | Rate-control statistics window. No documented valid range; not yet exposed as a config key. |
| *(hardcoded)* `frmQPStep` | fixed at 3 | Classic SoCs | Header-derived | Per-frame QP step limit. No documented range. |
| *(hardcoded)* `gopQPStep` | fixed at 15 | Classic SoCs | Header-derived | Per-GOP QP step limit. No documented range. |
| *(hardcoded)* `gopRelation` | fixed at 0 | Classic SoCs (H.264 struct) | Header-derived | No documented range or effect description. |

The new-API SDK structs (T31/C100/T40/T41) have no equivalents at all for
`quality_lvl`, `change_pos`, or the four remaining hardcoded fields above —
a structural difference between the two encoder generations (see
[Platform & SDK Support](Platform-SDK-Support.md#two-encoder-api-generations)),
not an oversight in what timps exposes. `i_bias_lvl` used to be in that same
list; T31/C100 gained a real equivalent (`iIPDelta`) on 2026-08-21, T40/T41
did not and stay in it.

## Live vs. restart, per SoC

Added 2026-08-21 (`src/enc_caps.h`, commits `c4e434f`/`443584e`/`22832f1`):
a per-platform subset of the fields above now reaches a **running** encoder
channel without a daemon restart. This table is a static platform ceiling —
the per-request truth (a listed key can still fall back to restart if its
channel isn't running, or the IMP call itself is rejected) is the
`deferred`/`deferred_keys` grading in the `POST /control` reply, and the
running build's own list is `caps.video_live` — see
[HTTP Control API](HTTP-Control-API.md#the-caps-object). Query that instead
of hardcoding this table into a client.

| Key | Classic H.264 (T10–T30, T23) | Classic H.265 (T21/T30) | T31 / C100 | T40 | T41 |
| --- | --- | --- | --- | --- | --- |
| `bitrate` | Live | Restart | Live | Live | Live |
| `rc_mode` | Live (full rc-struct re-fill in one call) | Restart | Restart — no direct mode setter | Restart | Restart — SDK has no rc-mode setter at all |
| `qp` (fixqp initial QP) | Live under `fixqp` only, Restart otherwise (see below) | Restart | Restart (see below) | Restart (see below) | Restart |
| `min_qp` / `max_qp` | Live | Restart | Live | Live | Live |
| `quality_lvl` / `change_pos` | Live | Restart | No effect | No effect | No effect |
| `i_bias_lvl` | Live | Restart | Live | No effect (no `SetChnQpIPDelta`) | No effect (no `SetChnQpIPDelta`) |
| `fluc_lvl` | Restart-only (H.265-only field; classic live-apply is H.264-only) | Restart | No effect | No effect | No effect |

`qp` was listed Live on T31/C100/T40 until 2026-08-22 and is not. Measured
on cam-garage (T31X, substream in `fixqp`): the live POST is graded
`deferred:0`, `encoder.<n>.rc.qp` echoes the new value — and the encoded
bitstream does not move, while the same QP pair applied at boot spans 6.4×.
`IMP_Encoder_SetChnAttrRcMode` stores `attrFixQp.iInitialQP` where the next
`Get` reads it back and never re-programs the running channel, so the key is
now graded restart-bound, which is what it always was. T40 shares that code
path and is graded the same way without a measurement of its own.
`IMP_Encoder_SetChnQp()` (T31 1.1.5+ and C100 headers only, "takes effect in
the next frame") is the candidate for restoring a real live `qp` there and
needs a bitstream measurement before it is advertised.

The classic H.264 path had the same class of bug, closed 2026-08-22: its
live-apply re-fills the *whole* rate-control union via
`IMP_Encoder_SetChnAttrRcMode` regardless of which key was actually posted,
so the call "succeeds" and gets graded live even when the specific field
posted has no effect in the encoder's *current* mode — `qp` only feeds
`attrFixQp.iInitialQP`, a union member the encoder ignores outside `fixqp`.
Measured on a T23N camera: `video1.qp` posted under `cbr` was graded
`deferred:0` (claimed live) with no observable bitstream effect. Fixed by
gating the classic `qp` branch on the channel's current `rc_mode`, the same
outside-its-native-mode check the new-API side already had — `qp` is now
correctly graded restart-bound outside `fixqp` on classic SoCs too. This
also benefits T20X, which shares the classic code path. Unlike the new-API
side, no candidate replacement call is needed here: the classic
`SetChnAttrRcMode` call genuinely does reprogram the running channel — it
just needs to run *in* `fixqp` for the `qp` field within it to matter, which
the gate now correctly requires.

H.264-only on the classic path because `IMP_Encoder_SetChnAttrRcMode`'s
full-struct re-fill is only proven safe for the H.264 union layout; an
H.265 channel on T21/T30 stays restart-bound no matter which key changed
(graded per-request at runtime, not expressible as a static per-key
capability — hence no `ENC_LIVE_KEYS` distinction for it). The host
simulator has no live path at all — `caps.video_live` is always empty
there.

**Hardware verification status**: wired and cross-build clean on every
platform (T23, T23N, T20X, T31, T31X, T23+`USE_SW_ROTATE`; T40/T41
compile-checked, not link-tested — their vendor libs need an fp64 toolchain
this repo does not ship). As of 2026-08-22, real hardware measurement on a
T31 fleet has confirmed: `bitrate`/`min_qp`/`max_qp` genuinely bind on the
live path (differential bitstream proofs, see the table above); the
`i_bias_lvl`↔`ip_delta` pass-through is correct in scale and sign but a
confirmed no-op on the bitstream under `cbr`; and `qp`'s live/restart
grading is now honest on both encoder-API generations (classic and
new-API), closing the exact same "whole call succeeds, so it's graded live
regardless of whether the specific field posted has any effect in the
current mode" gap on each side independently. The "takes effect at next
IDR" latency claim itself has not been independently timed (the
measurements above establish that live changes take effect, not exactly
how many frames later) — that and `i_bias_lvl` under `vbr`/`fixqp` remain
open (tracked in `dev_notes/TODO.md`).

## What was unexplained, and how it was settled

The T23's low-activity operating point sat around 990 kbit/s in the
measured setup and would not go lower under any combination of `rc_mode`,
`quality_lvl`, or `change_pos` — while the same scene, same camera, costs
278 kbit/s at a fixed QP of 42. Neither the `quality_lvl` floor formula
nor the `change_pos` threshold accounted for the gap (both were checked
and ruled out — see
[Rate Control and Bandwidth](Rate-Control-Bandwidth.md#two-hypotheses-that-looked-right-and-were-not)).

Sweeping `min_qp` settled it: 20 -> 1743 kbit/s, 30 -> 1181, 38 -> 235 —
below the fixed-QP reference. The T23 controller is quality-seeking, not
rate-seeking: it picks the best quality `min_qp` allows and the bitrate
follows as a consequence. `min_qp`/`max_qp` predate this investigation
and were never the mystery — they were simply left at their defaults
(20/45) throughout the earlier tests, which is why the operating point
looked fixed.

### Readback

The remaining question — whether timps's writes reach the encoder
unaltered at all — is answered by `GET /control`'s
[`encoder.<n>.rc` object](HTTP-Control-API.md#the-encodernrc-object--what-the-encoder-actually-holds),
which reads the attributes back live via `IMP_Encoder_GetChnAttrRcMode`
and reports them separately from the configured `video<N>.*` block. Diff
the two if a setting ever looks like it is not taking effect.

## Illustrations (pending — placeholders only)

This section is meant to show four frames pulled directly from the H.264
stream at different rate-control settings, so the visible quality
difference is judged from what the encoder actually produced — **not** from
a JPEG snapshot, which comes from timps's separate piggyback JPEG encoder
(see [Streaming Protocols](Streaming-Protocols.md)) and would not reflect
the H.264 rate controller's behaviour at all.

**No images are embedded yet.** Comparable frames already exist from the
2026-08-21 investigation, but they were captured in a child's bedroom for
an internal technical comparison, not cleared for publication in a public
wiki. The motif needs to be agreed with the user and, if needed, re-shot
before anything goes in below — placeholders only until then.

Four frames are needed, one per setting, so the same scene can be compared
across the operating points discussed above. For each, a 1:1-pixel crop is
the part that actually shows quantisation — a downscaled full frame hides
it — but pairing the crop with the corresponding full frame (for context on
where in the picture the crop was taken from) is worth doing if the capture
session allows it:

> **[PLACEHOLDER 1 — `rc_mode=cbr`, shipped default, ~2091 kbit/s]**
> A single frame from the `video0` H.264 stream while running the default
> `cbr` configuration (`quality_lvl` is not read under `cbr`, so this is the
> baseline). Show a 1:1-pixel 640×360 centre crop of the frame — not the
> full 1920×1080 downscaled — so fine detail and compression artefacts are
> visible at native resolution. Caption should name the setting and the
> measured mean bitrate for this run.

> **[PLACEHOLDER 2 — `rc_mode=vbr`, `quality_lvl=5`, ~1381 kbit/s]**
> Same scene, same crop region as Placeholder 1, so the two can be
> flipped between directly. This is the middle of the three `quality_lvl`
> points measured (2 / 5 / 7), showing a modest step down from the
> default.

> **[PLACEHOLDER 3 — `rc_mode=vbr`, `quality_lvl=7`, ~1243 kbit/s]**
> Same scene, same crop region again. This is the recommended operating
> point from [Rate Control and Bandwidth](Rate-Control-Bandwidth.md) (−41%
> vs. the `cbr` default). The point of placing this next to Placeholder 1 is
> for the reader to judge whether the visible quality cost matches the
> bitrate saving — the investigation notes measured only a ~7% difference in
> decoded PNG size across the `cbr` default and the two `vbr` levels (2.01 /
> 1.93 / 1.88 MB), much smaller than the bitrate change alone would suggest.

> **[PLACEHOLDER 4 — `rc_mode=fixqp`, `qp=42`, ~278 kbit/s]**
> Same scene, same crop region, at the fixed-QP diagnostic setting used
> throughout this page to prove the encoder can see scene content. Expect
> this one to show visibly heavier compression than the other three — it is
> included specifically as the "what the chip would do with no rate control
> at all" reference point, not as a usable setting.

Once approved, each placeholder above should be replaced with a normal
Markdown image reference and its descriptive paragraph kept as a caption
underneath.

A natural follow-up, tracked separately in `dev_notes/TODO.md` ("Screenshots
for the wiki"), is a matching set from a T31 camera at the same nominal
settings, to make the generational difference from
[Rate Control and Bandwidth](Rate-Control-Bandwidth.md) visible rather than
just measured. The two cameras in the current investigation look at
different rooms, so such a set would show different scenes — that has to be
stated in the caption rather than implied as a like-for-like picture
comparison; only the bitrate figures are comparable across the two SoCs, not
the frames.

### Proposed image location and convention

The wiki has not needed images before this page, so there is no existing
convention to follow. Proposed: store images under
`docs/wiki/images/<page-slug>/`, e.g.
`docs/wiki/images/rate-control-parameters/cbr-quality-lvl-2.png`, and
reference them with a normal relative Markdown image link
(`` ![cbr, quality_lvl 2, 2091 kbit/s](images/rate-control-parameters/cbr-quality-lvl-2.png) ``).

Reasoning: images live next to the page that uses them, under a
page-named subdirectory, so a page's assets are easy to find, easy to
remove if the page is ever deleted, and never collide with another page's
files with the same descriptive name (several pages will plausibly want a
file named `cbr.png`). Relative links keep working whether this directory
is browsed inside the main repo or synced to a GitHub wiki checkout, which
mirrors `docs/wiki/` at its own root.

## See also

- [Rate Control and Bandwidth: T23 vs T31](Rate-Control-Bandwidth.md) — the
  practical argument this page's detail supports.
- [Configuration Reference](Configuration-Reference.md#videon--per-stream-encoder-settings) —
  the general `video<N>.*` key reference (type/default/range/live-status
  conventions used above).
- [Platform & SDK Support](Platform-SDK-Support.md#two-encoder-api-generations) —
  the classic-vs-new-API encoder split these fields are organized around.
- `dev_notes/T23_RATECONTROL_INVESTIGATION_2026-08-21.md` — full
  measurement data and method for the 2026-08-21 T23 investigation
  (`quality_lvl`, `change_pos`, the `min_qp` sweep that settled the
  quality-seeking-vs-rate-seeking question on that SoC).
- `dev_notes/TODO.md` — open follow-up work: the "takes effect at next IDR"
  latency has not been independently timed; `i_bias_lvl` is unmeasured under
  `vbr`/`fixqp` on T31/C100 and unmeasured on every classic SoC in either
  mode; and nothing above has been measured yet on T20X or T40/T41 hardware
  (compile/link status only).
