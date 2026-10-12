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
| T40 | `include/T40/1.3.1/en` (matches the libimp 1.3.1 thingino ships; the older 1.2.0 header lacks `IMPEncoderStream.isVI`, so `IMP_Encoder_GetStream` would overrun a struct one word too short — `video_thread()` has a compile-time tripwire against this) |
| T41 | `include/T41/1.2.6/en` (matches the libimp 1.2.6 thingino ships) |
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
| Highlight depress, sinter/temper NR, max analog/digital gain, manual WB, ISP-level hflip/vflip | All **except** T40/T41 (`ISP_NEW_TUNING_API`); sinter NR (since 1.9.33) and temper NR (since v1.10.0) on T41 with OpenIMP |
| AE compensation | All except T21 and T40; T41 with OpenIMP (since 1.9.33) |
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
| No rotation primitive | *(none)* | T30, C100; T10, T20, T21 with the vendor libimp | `rotation` accepts only `0`; any 90/180/270 request is coerced to 0 with a warning. |
| Software 90/270 (opt-in) | `ROT_HAS_SW_90` | T23 (needs `USE_SW_ROTATE=1`) | CPU NV12 transpose per frame + libimp's unbound `IMP_Encoder_YuvEncode` path. Real CPU cost on the single core, H.264-only, software text-OSD only (no logos/privacy covers on the rotated stream), and the [motion grid](Motion-Detection.md#the-t23-software-rotation-coordinate-caveat) needs an explicit coordinate-mapping fix since IVS itself still sees the pre-rotation frame. |
| FrameSource software rotate | `ROT_HAS_FS_ROTATE` | T31; T10, T20, T21 with OpenIMP | `IMP_FrameSource_SetChnRotate` — rotation happens inside libimp, not visible to the rest of timps as "software," but still not true hardware. Hardware OSD/privacy overlays work on rotated 90/270 streams (fixed 2026-07-23) but are confined to a top-band region on non-square rotated streams due to an OSD coordinate range-check quirk (vendor libimp only). On T10/T20/T21 (OpenIMP only, since v1.10.0, not yet tested on hardware): sub streams only, 16-aligned, at most 704x576; `video0` and anything larger start unrotated. Needs an OpenIMP with T1x rotation (commit `6b7f5ae`, branch `claude/t1x-rotation`, not in OpenIMP `next` or any thingino `ciao` OpenIMP pin); timps probes `SetChnRotate` with a 0x0 picture at start and brings the stream up unrotated when the OpenIMP stubs (`9cab219`, returns success without rotating) or refuses (`6857610`, `0c02f45`, `636b6ac`) it. |
| True hardware I2D rotate | `ROT_HAS_HW_I2D` | T40, T41 | The dedicated I2D hardware block. Full framerate, all resolutions. With the vendor libimp timps applies the same top-band OSD/privacy clamp on 90/270 as on T31 (`video.<N>.rotate_path` = `band`; whether this libimp needs it is not verified on hardware); OpenIMP and `180` draw over the whole frame. The only tier where `rotation=180` is a real, distinct, **per-channel** capability (elsewhere, 180 was removed as a redundant, incorrectly-per-stream-modeled *global* ISP flip — use `image.hflip`+`image.vflip` instead, which is always available independent of `USE_ROTATE`). |

`ROT_HAS_90` (gating whether 90/270 are accepted at all) is defined
whenever any of the three apply-path macros above is active, or on the
host sim. Rotation direction conventions are **not** uniform across
platforms (T23 and T40/T41: 90° = clockwise; T31 and T10/T20/T21: 90° = counter-clockwise,
kept that way specifically for numeric compatibility with prudynt-t's raw
`rotTo90` enum values) — see `docs/rotation.md` for the full rationale and
on-device test history. Since v1.10.2 `GET /control` reports the path of a
running stream in `video.<N>.rotate_path` (`off`, `sw`, `band`, `hw`).

## ROI and FrontCrop tiers

Since v1.10.0, encoder ROI (`roi<S>.<N>.*`) and ISP FrontCrop
(`image.fcrop_*`) carry a per-SoC tier in `src/roi_caps.h`, reported as
`caps.roi.tier` / `caps.fcrop.tier`: **effective** = device-tested,
**experimental** = the call exists and returns 0 but the effect is unverified
(or known absent), **unsupported** = refused. Both setters are bound weak, so a
libimp without the symbol reports unsupported at run time. The OpenIMP tiers
come from device tests by the OpenIMP maintainer (OpenIMP d03ca24, open-tx-isp
3cf9bdac), not from timps itself.

| SoC | ROI vendor | ROI OpenIMP | FrontCrop vendor | FrontCrop OpenIMP |
| --- | --- | --- | --- | --- |
| T10 | experimental | **effective** | unsupported | **effective**, live |
| T20 | experimental | **effective** | unsupported | **effective**, live (downscaled streams only) |
| T21 | experimental (no effect seen) | **effective** (`OPENIMP_T21_ROI=0` turns it off → experimental) | unsupported | **effective**, live |
| T23 | unsupported (encoder only stores the region) | **effective** when the libimp exports `OpenIMP_Cap_T23HelixRoi` (OpenIMP v2026.10.11-rc1+, since v1.10.1), otherwise unsupported; `OPENIMP_T23_ROI=0` → unsupported | unsupported (untested) | **effective**, live with the driver's `fcrop_upscale_pct` parameter (open-tx-isp b15ef235); otherwise experimental, **start-only** (needs 3cf9bdac+) |
| T30, C100 | unsupported | — | unsupported | — |
| T31 | unsupported (no ROI symbol in 1.1.x) | **effective** (`SetChnRoiAttr`, needs OpenIMP d03ca24 + fec3e69; `OPENIMP_T31_ROI=0` → unsupported) | experimental, live (no zoom without open-tx-isp `fcrop_upscale_pct`) | **effective**, live with the driver's `fcrop_upscale_pct` parameter (open-tx-isp 27dc8be4+); otherwise experimental, live |
| T40, T41 | unsupported (vendor T41 takes deltas only; not device-tested) | unsupported (T41 experimental, off in libimp) | unsupported | unsupported |

T23/T31 FrontCrop with OpenIMP: timps checks at start for
`/sys/module/tx_isp_t23|t31/parameters/fcrop_upscale_pct`, which only drivers
with the T23 crop-hang fix (open-tx-isp 164225e5/25b1d8c7) or the T31 guard
(27dc8be4) have. Without it, T23 stays start-only (older drivers hung the
pipeline on a crop change): a POST is persisted and applied at the next
`/etc/init.d/S95timps restart`. A window smaller than a stream that is not
scaled down is refused with `-EINVAL` (one WARN, `caps.fcrop.state` =
`rejected`). Since v1.10.2 `image.fcrop_upscale_pct` (live) writes that
parameter, so the scaler may upscale a window down to half the stream size;
the ceiling `caps.fcrop.upscale_max` is 100 on both (device-tested; T31 at
150 hard-hangs until the hardware watchdog resets the camera) and -1 on every
other SoC/libimp.

## Day/night colour statistics

The colour trigger of v1.10.2 (`daynight.color_pct`) reads per-zone AWB
statistics: `GetAwbStatistics` on T40/T41, `GetAwbZone` on T23/T31/C100, and
on T10/T20 with OpenIMP a tuning ioctl on libimp's `/dev/video0` fd. With
OpenIMP on T23/T31 and T40/T41 only where the libimp exports
`OpenIMP_Cap_AwbZone` / `OpenIMP_Cap_AwbStatistics`. Vendor T10/T20/T21, T21
with OpenIMP and T30 have none (`daynight.color_trigger` = `unavailable`).
Device-tested on T40 vendor and T20 OpenIMP only.

ROI QP ranges: delta -25..25 everywhere (-26 gave invalid H.264 on T20); T21
and OpenIMP T23 honour only about -12..+13 (`caps.roi.qp_delta_eff`), positive deltas are
also capped by `max_qp`. An absolute QP is sent as a clamped delta when it is
more than 25 from a possible picture QP (`caps.roi.abs_window`). T10/T20 crop
only downscaled (sub) streams; a main stream at sensor size cannot zoom
there (`caps.fcrop.zoomable`).

Raising a tier after a device test is one line in `src/roi_caps.h` plus its
`ROI_MATRIX` entry in the Makefile (`make test-roi`).

## FrameSource keepalive default

`general.fs_keepalive = -1` (the default) resolves per build, from
`FS_KEEPALIVE_AUTO` in `src/hal/fs_ref.h`:

| Build | Auto resolves to | Why |
| --- | --- | --- |
| T23 with `USE_OPENIMP` | **on** | FrameSource disable/enable edges while streaming hung a T23 on the open stack. |
| T41 with `USE_OPENIMP` | **on** | Reproduced hang (3/3): one channel stopped and restarted within ~200 ms while the other ran. |
| T10, T20, T21, T30, T31, T40 with `USE_OPENIMP` | off | No hang known; idle cost of a 24/7 FrameSource unmeasured on these (mostly small-RAM) SoCs. |
| Every vendor-libimp build (incl. T23, T41) | off | Same; the vendor T41 has not been tested for the fault. |

`general.fs_keepalive = 1` turns it on anywhere (opt-in), `0` turns it off
anywhere. See [Architecture](Architecture.md#framesource-keepalive-generalfs_keepalive).

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
