# Image rotation in timps

Config key: `videoN.rotation`, values `0 | 90 | 270` (plus `180` on T40/T41 —
see the note below). **90 = clockwise on
T23 and T40/T41. On T31 specifically, 90 rotates counter-clockwise** (kept for
consistency with prudynt-t/raptor's raw `rotation=1|2` config values, see
"Notes on direction" below) — but **on T31, neither 90 nor 270 currently
produce a working hardware OSD/privacy overlay** when the stream is
downscaled from the sensor; see "Known limitation" below.

> **`rotation=180` is platform-nuanced.** On every classic-API SoC
> (T10/T20/T21/T23/T30/T31/C100) it only ever meant a *global* ISP Hflip+Vflip —
> visually identical to, and made redundant by, the always-available
> `image.hflip=1` + `image.vflip=1`, but falsely modelled as per-stream (setting
> it on one stream silently flipped every other enabled stream too). It was
> **removed** on those SoCs: a `rotation=180` request there is coerced to 0 with
> a warning, exactly like an unsupported 90/270 request — **for a 180° flip set
> `image.hflip=1` and `image.vflip=1`** (independent of `USE_ROTATE`, always
> available; note it is a global flip).
> **On T40/T41, `rotation=180` is retained** — there it is a genuine
> *per-channel* hardware I2D flip (flip+mirror) scoped to just the requesting
> video stream, something the global `image.hflip`/`image.vflip` cannot do on
> that platform (they flip every channel at the sensor/ISP). So on T40/T41 `180`
> is a real, distinct capability, gated on `ROT_HAS_HW_I2D`.
Rotation is **restart-required** — it is applied when the pipeline is built, not
live. Downstream (encoder, RTSP SDP, fMP4/MP4 recorder, OSD, snapshots) all use
the post-rotation dimensions via one helper (`ms_vstream_eff_dims`), so a
rotated 1920×1080 stream is advertised and recorded as 1080×1920.

## Enabling it (build option / menuconfig)

Rotation is **off by default** and selectable like `USE_CONTROL`/`USE_TLS`/
`USE_FAAC` — it costs nothing when disabled (all code `#ifdef`'d out).

| menuconfig option | make flag | what it adds |
|---|---|---|
| `BR2_PACKAGE_TIMPS_ROTATE` | `USE_ROTATE=1` | real 90/270 transpose on the SoCs that have it (T31, T40, T41; T23 via `SW_ROTATE`), plus genuine per-channel 180° on T40/T41 (I2D). No effect on SoCs without a 90/270 path — for a 180° flip there use `image.hflip`+`image.vflip` instead |
| `BR2_PACKAGE_TIMPS_SW_ROTATE` (needs `TIMPS_ROTATE`) | `USE_SW_ROTATE=1` | the **software** 90/270 path for T23 (CPU transpose + software JPEG + software OSD) — the large/CPU-heavy part |

## What it costs

Binary (code, measured as object `.text`; MIPS target ≈ 1.2–1.4× these host figures):

- `USE_ROTATE` alone: the config/caps plumbing plus the hardware 90/270 apply is
  only a few hundred bytes on T31/T40/T41. Negligible. (On a SoC with no 90/270
  path it adds nothing usable — `rotation` accepts no non-zero value there.)
- `USE_SW_ROTATE` (T23): **~7 KB** total (CPU NV12 transpose `nv12_rot.o` ≈ 0.6 KB
  + the software encode/JPEG/OSD path in the HAL ≈ 6.2 KB).

Runtime memory — **only the T23 software path** allocates extra; it scales with
the rotated resolution. For a 720×1280 stream, roughly:

- rotated NV12 bounce buffer ≈ 1.4 MB (rmem)
- encoder output buffer ≈ 0.9 MB (heap)
- JPEG buffer (if `videoN.jpeg` on) ≈ 1.0 MB (heap)
- FrameSource depth-2 pool at source dims ≈ 2.8 MB (rmem)
- → **~6 MB of buffers** for one 720×1280 rotated stream, plus **~1 CPU core**.

The hardware 90/270 paths (T31/T40/T41) add **no measurable runtime memory or
CPU** — the rotation happens in the ISP/I2D hardware.

## What works on which camera (SoC)

(The 90/270 columns below are the real transpose. The 180° column shows where
`rotation=180` is a genuine *per-channel* capability: **only T40/T41** (I2D
flip+mirror). On every other SoC `rotation=180` is removed/coerced to 0 — use
the global `image.hflip`+`image.vflip` there instead.)

| SoC | 180° | 90/270° | Mechanism for 90/270 | Constraints |
|-----|------|---------|----------------------|-------------|
| T10, T20, T21, T30, C100 | ❌ (use `image.hflip`+`vflip`) | ❌ | none (no rotate primitive in libimp) | 90/270 in config is coerced to 0 with a warning; `rotation` accepts no non-zero value (180 also coerces to 0) |
| **T23** | ❌ (use `image.hflip`+`vflip`) | ✅ *(opt-in)* | CPU NV12 transpose → unbound software H.264 encoder (`IMP_Encoder_YuvEncode`, SDK 1.3.0) | build with `USE_SW_ROTATE=1`; **H.264 only**; substream-class res (~≤704×576 @ ≤15 fps); real CPU cost on the single core; **software text-OSD only** (no logos, no privacy covers); snapshot + MJPEG work via the standalone JPEG encoder, but it needs the rotated width (= source height) to be a multiple of 32 and height a multiple of 8 (use e.g. 1280×704 → 704×1280, not 1280×720); motion grid stays pre-rotation |
| **T31** | ❌ (use `image.hflip`+`vflip`) | ✅ | `IMP_FrameSource_SetChnRotate` (software inside libimp) | libimp SDK ≥ 1.1.6; 64-px-aligned resolution; **≤1280×704 @ ≤15 fps** recommended; extra rmem; not combinable with encoder soft-zoom; **hardware OSD/privacy DO work on 90/270** (fixed 2026-07-23) — on a **non-square** rotated stream overlays are confined to the top `picHeight` band (see note below); square streams = anywhere |
| **T40, T41** | ✅ **per-channel** (I2D flip+mirror) | ✅ | true **hardware** I2D rotate (`IMPFSI2DAttr` + `IMP_FrameSource_SetI2dAttr`) | full frame rate, all resolutions, OSD + privacy keep working. 180 flips only the requesting channel (unlike the global `image.hflip`/`vflip`). `rotate_angle` units await on-device confirmation (degrees) |

Note: without `USE_ROTATE` (the default), all rotation code compiles out and
every 90/180/270 request coerces to 0 — the build is byte-identical to
no-rotation.

### OSD/privacy on 90/270-rotated T31 streams: works, top-band limit (fixed 2026-07-23)

Hardware OSD text, logos and privacy covers **work** on a 90/270-rotated T31
stream. Two things were needed (both in `src/hal/`), and one hardware limit
remains.

**Fix 1 — FrameSource picWidth (`hal_ingenic.c`).** For a rotated channel the FS
`chnAttr` must stay at the **pre-rotation (landscape) geometry** — scaler AND
picWidth (only the *encoder* gets the post-rotation portrait dims via
`ms_vstream_eff_dims`). Swapping the FS `picWidth` to the portrait dim gives the
OSD compositor the wrong stride → the classic "scattered dots". prudynt-t marks
that exact swap `// Breaks OSD` in `IMPFramesource.cpp`.

**Fix 2 — even geometry + top-band clamp (`imp_osd.c`, rotated streams only).**
The IPU OSD compositor needs **even** region width/height/origin (an odd region
fails the per-frame composite with `ipu: error ipu start ret=-1` and blanks the
whole group), so overlays are padded/aligned to even. And libimp **range-checks
OSD coordinates against the pre-rotation (landscape) dims** `picWidth×picHeight`
(logcat: `osd_draw_cover_pic ... keep within picture range`), so on a portrait
`704×1280` the usable OSD area is the **top `picHeight` (=704) rows**; overlays
configured lower are clamped up (with a one-time startup `LOGW`).

**The remaining hardware limit.** Raising `picHeight` to the portrait height to
lift the range check corrupts the video pool (VBM is allocated at the landscape
dims) → garbage video. And the frame rotation does **not** rotate overlays
(they composite upright in portrait space, verified on-device), so there is no
coordinate transform that reaches the bottom of a tall portrait. Net:

- **square rotated stream (≤704×704):** OSD/privacy work **anywhere**.
- **non-square portrait (e.g. 704×1280):** OSD/privacy work in the **top ~704
  px**; lower overlays are clamped into that band.
- T40/T41 (I2D 90/270): unaffected, OSD works everywhere.

The **non-rotated path is byte-for-byte unchanged** — all of the above (even
alignment, clamp, warning) is gated on `rotation ∈ {90,270}`.

**If full-height OSD on a non-square rotated stream is required:** put the OSD on
the non-rotated sub-stream `ch1`, or use a square main stream.

## How the other thingino streamers handle rotation (for reference)

- **prudynt-t**: 90/270 on **T31 only** (`IMP_FrameSource_SetChnRotate`,
  runtime `dlsym`-probed); no 180 rotation key (uses hflip/vflip); **no I2D** →
  T40/T41 get no rotation. Its `stream0.rotation` config key is **not
  degrees** — it's the raw libimp `rotTo90` enum (`validateInt2`, range 0-2),
  passed straight through with no CW/CCW translation. Two theories for "OSD
  works in prudynt-t" were tested on-device (2026-07-21) and both look
  unlikely on our test camera: (a) CCW is the "good" enum value — falsified
  earlier, CCW is broken here too; (b) the scaler being inactive (`scale=0`)
  is what makes it work — also unsupported: with the scaler genuinely
  disabled, OSD was *still* broken using timps' own FrameSource setup, and
  prudynt-t's own exact FrameSource attrs (which additionally swap
  `scaler.outwidth/outheight` to post-rotation dims) corrupt the *entire
  frame*, not just OSD, whenever the scaler is active — cleanly reproduced.
  Current best guess: prudynt-t's report is specific to a different
  camera/SDK build than the one used here, or was never directly verified
  under matching conditions.
- **raptor**: its HAL *implements* T31 `SetChnRotate` and T32/T33/T40/T41 I2D,
  but the daemon **never calls it** — rotation is dead code; only hflip/vflip
  are exposed.
- **strero**: no rotation at all (ONVIF even advertises `Rotation=false`).

So timps offers strictly more: hardware 90/270 on T40/T41 (which prudynt cannot
do) plus genuine per-channel 180° there (I2D flip+mirror, not the global sensor
flip), T31 90/270, and the T23 software path that no other thingino streamer
has. (On the classic-API SoCs — like prudynt-t — a 180° flip is done via the
global `image.hflip`/`image.vflip`, not via the `rotation` key.)

## Notes on direction / vendor semantics

- **T23 and T40/T41: 90 = clockwise.** **T31: 90 = counter-clockwise**
  (`rotTo90 = 1`), 270 = clockwise (`rotTo90 = 2`) — deliberately the opposite
  of the other two SoCs, and the opposite of timps' own earlier convention.
  Changed 2026-07-21 to numerically match prudynt-t/raptor's raw
  `rotation=1|2` config values (see below), on the theory that CCW was the
  one direction with a working hardware-OSD path on this SDK. **That theory
  did not survive on-device testing** (see "Known limitation" above): OSD is
  currently broken in both directions on our test camera, so this direction
  choice no longer has an OSD-based justification — it's kept purely for the
  prudynt/raptor config-value compatibility described below.
- prudynt-t and raptor don't have a CW/CCW convention to compare against in
  the first place: their `rotation` config key **is** the raw libimp
  `rotTo90` enum (0/1/2, see prudynt-t's `Config.cpp: validateInt2`), passed
  straight through with no degrees-to-direction translation at all. timps'
  legacy `rotation=1|2` compatibility inputs (`config.c: prot()`) are kept
  numerically aligned with that raw enum (1→90→rotTo90 1, 2→270→rotTo90 2),
  so a `prudynt.cfg`-style `rotation=1` or `rotation=2` value carries over to
  the same physical direction in timps.
- T31 `IMP_FrameSource_SetChnRotate` takes **pre-rotation** width/height and
  must be called **before** `CreateChn`; only the encoder gets swapped dims.
- T40/T41 `rotate_angle` units are undocumented in every SDK header; timps uses
  plain degrees for the 90/270 rotate; raptor-hal uses plain degrees too. 180 on
  T40/T41 does not use `rotate_angle` at all — it is `flip_enable`+`mirr_enable`
  on the per-channel I2D attr.
