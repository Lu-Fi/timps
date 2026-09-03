# Platform & SDK Support

timps supports nine Ingenic SoC families: **T10, T20, T21, T23, T30, T31,
T40, T41, C100**. This page summarizes the cross-platform structure the
codebase is built around; it deliberately does not duplicate the
exhaustive per-function availability matrix already maintained in
`docs/sdk-feature-gaps.md`, or the rotation deep-dive in
`docs/rotation.md` — read those two documents directly for full depth.

## SDK headers per platform

The vendored Ingenic IMP headers (from the
[gtxaspec/ingenic-headers](https://github.com/gtxaspec/ingenic-headers)
git submodule under `include/`) are pinned per platform in the
`Makefile`:

| Platform | Header path |
| --- | --- |
| T31 | `include/T31/1.1.6/en` |
| C100 | `include/C100/2.1.0/en` |
| T21 | `include/T21/1.0.33/zh` |
| T23 | `include/T23/1.3.0/en` (must be ≥1.1.2 — earlier headers are missing a struct member that, combined with a newer libimp, makes the framesource silently deliver no frames; `fs_create()` has a compile-time tripwire against this mismatch) |
| T30 | `include/T30/1.0.5/zh` |
| T40 | `include/T40/1.2.0/zh` |
| T41 | `include/T41/1.2.0/zh` |
| T20, T10 | `include/T20/3.12.0/zh` (T10 has no distinct header set and builds against T20's) |

## Two encoder-API generations

`src/hal/hal_ingenic.c` branches its encoder setup between two SDK
generations:

- **New generation** — `IMP_Encoder_SetDefaultParam`-based configuration:
  **T31, C100, T40, T41**.
- **Old generation** — manual `IMPEncoderChnAttr`/profile/rate-control
  struct assembly: **T10, T20, T21, T23, T30**.

Every encoder-facing feature decision in the codebase (and in
`docs/sdk-feature-gaps.md`'s roadmap of unused SDK capabilities) has to
account for this split.

## ISP tuning API split

Separately, the **ISP tuning** call signatures diverge on just two
platforms: `ISP_NEW_TUNING_API` (`src/isp_caps.h`) is defined only for
**T40/T41**, which use a reworked API taking an `IMPVI_NUM` + pointer
argument form for many tuning calls, and route flips through
`SetHVFLIP` instead of separate `SetISPHflip`/`SetISPVflip` calls. Every
other platform (including the old-generation-encoder ones) uses the
classic tuning API.

## ISP tuning capability matrix (`image.*`)

`src/isp_caps.h` is the single source of truth both `hal_ingenic.c`
(which calls guard which SDK function) and `/control`'s `caps.image` list
consult — see [Configuration Reference](Configuration-Reference.md#image--isp-tuning)
for the full per-key table. Summary:

| Capability | Platforms |
| --- | --- |
| Brightness/contrast/saturation/sharpness, anti-flicker, running-mode, sensor FPS | All SoCs |
| Hue (`SetBcshHue`) | T23, T31, T40, T41, C100 |
| Backlight comp / defog / DPC strength | T23, T31, C100 |
| DRC / WDR strength | T21, T23, T31, C100 |
| Highlight depress, sinter/temper NR, max analog/digital gain, manual WB, ISP-level hflip/vflip | All **except** T40/T41 (`ISP_NEW_TUNING_API`) |
| AE compensation | All except T21 and T40/T41 |
| AE average luminance (`GetAeLuma`, secondary day/night metric) | T21, T23, T31, C100 |
| Real sensor-attr readback (`GetSensorAttr`) | T23, T31, T32, T33, T40, T41, C100 (absent on T10/T20/T21/T30) |

A build with no `PLATFORM_*` macro defined at all (the host `make sim`
build) enables every capability, so the WebUI can be fully exercised
against `timpsd-sim`.

## Audio capability matrix (`audio.*`)

From `src/audio_caps.h`: every SoC supports `IMP_AI_SetVol`/`SetGain`,
`EnableHpf`/`DisableHpf`, `EnableAgc`/`DisableAgc`, and
`EnableNs`/`DisableNs`. **Analog PGA gain** (`IMP_AI_SetAlcGain`,
`audio.alc_gain`) is restricted to **T21, T31, C100** (T10 shares T20's
headers, which also lack it, so T10 has no ALC either). See
[Audio](Audio.md) for how this interacts with the native speaker
pipeline.

## Motion detection capability (`motion.*`)

From `src/motion_caps.h`: whether `IMP_IVS` move detection exists, and
the maximum grid cell budget, come directly from whichever SDK header set
a platform's build links against (`IMP_IVS_MOVE_MAX_ROI_CNT`) — 52 cells
on essentially every SDK version timps builds against today, with the one
documented historical exception being the old T10/T20 3.9.0 SDK (4
cells), which the current Makefile no longer selects (it pins T20 3.12.0).
See [Motion Detection](Motion-Detection.md).

## Rotation capability tiers

Image rotation (`videoN.rotation`, `USE_ROTATE`, off by default) has the
most platform-fragmented capability story in the codebase — full detail
lives in `docs/rotation.md`; the tiers are:

| Tier | Macro | Platforms | Mechanism |
| --- | --- | --- | --- |
| No rotation primitive | *(none)* | T10, T20, T21, T30, C100 | `rotation` accepts only `0`; any 90/180/270 request is coerced to 0 with a warning. |
| Software 90/270 (opt-in) | `ROT_HAS_SW_90` | T23 (needs `USE_SW_ROTATE=1`) | CPU NV12 transpose per frame + libimp's unbound `IMP_Encoder_YuvEncode` path. Real CPU cost on the single core, H.264-only, software text-OSD only (no logos/privacy covers on the rotated stream), and the [motion grid](Motion-Detection.md#the-t23-software-rotation-coordinate-caveat) needs an explicit coordinate-mapping fix since IVS itself still sees the pre-rotation frame. |
| FrameSource software rotate | `ROT_HAS_FS_ROTATE` | T31 | `IMP_FrameSource_SetChnRotate` — rotation happens inside libimp, not visible to the rest of timps as "software," but still not true hardware. Hardware OSD/privacy overlays work on rotated 90/270 streams (fixed 2026-07-23) but are confined to a top-band region on non-square rotated streams due to an OSD coordinate range-check quirk. |
| True hardware I2D rotate | `ROT_HAS_HW_I2D` | T40, T41 | The dedicated I2D hardware block. Full framerate, all resolutions, no OSD/privacy limitations. The only tier where `rotation=180` is a real, distinct, **per-channel** capability (elsewhere, 180 was removed as a redundant, incorrectly-per-stream-modeled *global* ISP flip — use `image.hflip`+`image.vflip` instead, which is always available independent of `USE_ROTATE`). |

`ROT_HAS_90` (gating whether 90/270 are accepted at all) is defined
whenever any of the three apply-path macros above is active, or on the
host sim. Rotation direction conventions are **not** uniform across
platforms (T23 and T40/T41: 90° = clockwise; T31: 90° = counter-clockwise,
kept that way specifically for numeric compatibility with prudynt-t's raw
`rotTo90` enum values) — see `docs/rotation.md` for the full rationale and
on-device test history.

## Where to go deeper

- **`docs/sdk-feature-gaps.md`** — an independent, function-by-function
  audit of every `IMP_*` SDK declaration across all nine platforms against
  what timps actually calls today, with a fleet-weighted prioritized list
  of unused capabilities (live encoder bitrate/GOP/QP reconfiguration,
  backchannel echo cancellation, live IVS sensitivity without a full
  rebuild, AE zone weighting, ROI-based encoding, ePTZ, WDR, privacy
  mosaic, and more) — useful reading before proposing a new feature that
  touches the SDK directly.
- **`docs/rotation.md`** — the full rotation story: exact per-SoC
  resolution/framerate constraints, the T31 hardware-OSD-on-rotated-stream
  investigation trail, direction-convention history, and how the other
  thingino streamers (prudynt-t, raptor, strero) compare.
