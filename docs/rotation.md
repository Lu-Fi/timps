# Image rotation in timps

Config key: `videoN.rotation`, values `0 | 90 | 180 | 270`. **90 = clockwise.**
Rotation is **restart-required** — it is applied when the pipeline is built, not
live. Downstream (encoder, RTSP SDP, fMP4/MP4 recorder, OSD, snapshots) all use
the post-rotation dimensions via one helper (`ms_vstream_eff_dims`), so a
rotated 1920×1080 stream is advertised and recorded as 1080×1920.

## Enabling it (build option / menuconfig)

Rotation is **off by default** and selectable like `USE_CONTROL`/`USE_TLS`/
`USE_FAAC` — it costs nothing when disabled (all code `#ifdef`'d out).

| menuconfig option | make flag | what it adds |
|---|---|---|
| `BR2_PACKAGE_TIMPS_ROTATE` | `USE_ROTATE=1` | 180° everywhere + hardware 90/270 on the SoCs that have it (T31, T40, T41) |
| `BR2_PACKAGE_TIMPS_SW_ROTATE` (needs `TIMPS_ROTATE`) | `USE_SW_ROTATE=1` | the **software** 90/270 path for T23 (CPU transpose + software JPEG + software OSD) — the large/CPU-heavy part |

## What it costs

Binary (code, measured as object `.text`; MIPS target ≈ 1.2–1.4× these host figures):

- `USE_ROTATE` alone: **~0.2 KB** for 180° on a given SoC; the hardware 90/270
  apply adds only a few hundred bytes more on T31/T40/T41. Negligible.
- `USE_SW_ROTATE` (T23): **~7 KB** total (CPU NV12 transpose `nv12_rot.o` ≈ 0.6 KB
  + the software encode/JPEG/OSD path in the HAL ≈ 6.2 KB).

Runtime memory — **only the T23 software path** allocates extra; it scales with
the rotated resolution. For a 720×1280 stream, roughly:

- rotated NV12 bounce buffer ≈ 1.4 MB (rmem)
- encoder output buffer ≈ 0.9 MB (heap)
- JPEG buffer (if `videoN.jpeg` on) ≈ 1.0 MB (heap)
- FrameSource depth-2 pool at source dims ≈ 2.8 MB (rmem)
- → **~6 MB of buffers** for one 720×1280 rotated stream, plus **~1 CPU core**.

180° and the hardware 90/270 paths (T31/T40/T41) add **no measurable runtime
memory or CPU** — the rotation happens in the ISP/I2D hardware.

## What works on which camera (SoC)

| SoC | 180° | 90/270° | Mechanism for 90/270 | Constraints |
|-----|------|---------|----------------------|-------------|
| T10, T20, T21, T30, C100 | ✅ | ❌ | none (no rotate primitive in libimp) | 90/270 in config is coerced to 0 with a warning |
| **T23** | ✅ | ✅ *(opt-in)* | CPU NV12 transpose → unbound software H.264 encoder (`IMP_Encoder_YuvEncode`, SDK 1.3.0) | build with `USE_SW_ROTATE=1`; **H.264 only**; substream-class res (~≤704×576 @ ≤15 fps); real CPU cost on the single core; **software text-OSD only** (no logos, no privacy covers); snapshot + MJPEG work via the standalone JPEG encoder, but it needs the rotated width (= source height) to be a multiple of 32 and height a multiple of 8 (use e.g. 1280×704 → 704×1280, not 1280×720); motion grid stays pre-rotation |
| **T31** | ✅ | ✅ | `IMP_FrameSource_SetChnRotate` (software inside libimp) | libimp SDK ≥ 1.1.6; 64-px-aligned resolution; **≤1280×704 @ ≤15 fps** recommended; extra rmem; not combinable with encoder soft-zoom; **hardware OSD/privacy do NOT work on 90/270-rotated streams** (see limitation below) |
| **T40, T41** | ✅ | ✅ | true **hardware** I2D rotate (`IMPFSI2DAttr` + `IMP_FrameSource_SetI2dAttr`) | full frame rate, all resolutions, OSD + privacy keep working. `rotate_angle` units await on-device confirmation (degrees) |

180° is realised as ISP Hflip+Vflip, **XOR-composed** with the live
`image.hflip`/`image.vflip` keys (180 + a manual flip flips once, not cancels).
On T40/T41 the 180 is done per-channel in the I2D unit instead.

### Known limitation: no OSD/privacy on 90/270-rotated T31 streams

On **T31**, a 90/270-rotated stream **cannot carry a hardware OSD or privacy
mask** (verified on-device). The video itself is clean; the overlay renders as
scattered pixels ("dots"). Root cause: `IMP_FrameSource_SetChnRotate` produces a
704×1280 frame, but the bound hardware IPU-OSD miscomputes that buffer's stride
(the encoder reads it correctly, the OSD does not) — a libimp-internal issue.

It is **not** fixable from timps' config/setup, and a software-OSD fallback (as
on T23) is impossible on T31: its libimp exports no unbound encode API
(`IMP_Encoder_YuvEncode`/`SendFrame` are T23-SDK-1.3.0-only), so there is no
software step between the hardware rotate and the hardware encoder to render an
overlay into. 180° rotation is unaffected (OSD works). T40/T41 (I2D) are
unaffected (OSD + privacy keep working).

**Workaround:** put the OSD on a non-rotated stream (e.g. the sub-stream `ch1`)
and keep the rotated main stream overlay-free; or use 180° instead of 90/270.

## How the other thingino streamers handle rotation (for reference)

- **prudynt-t**: 90/270 on **T31 only** (`IMP_FrameSource_SetChnRotate`,
  runtime `dlsym`-probed); no 180 rotation key (uses hflip/vflip); **no I2D** →
  T40/T41 get no rotation. Config `90 → CCW`.
- **raptor**: its HAL *implements* T31 `SetChnRotate` and T32/T33/T40/T41 I2D,
  but the daemon **never calls it** — rotation is dead code; only hflip/vflip
  are exposed.
- **strero**: no rotation at all (ONVIF even advertises `Rotation=false`).

So timps offers strictly more: 180 everywhere, hardware 90/270 on T40/T41
(which prudynt cannot do), T31 90/270, and the T23 software path that no other
thingino streamer has.

## Notes on direction / vendor semantics

- **90 = clockwise** in timps (`rotTo90 = 2` on T31). prudynt/raptor map their
  "90" to CCW (`rotTo90 = 1`) — so timps rotates the opposite way for the same
  config value. Deliberate (matches the common "rotate clockwise" UI).
- T31 `IMP_FrameSource_SetChnRotate` takes **pre-rotation** width/height and
  must be called **before** `CreateChn`; only the encoder gets swapped dims.
- T40/T41 `rotate_angle` units are undocumented in every SDK header; timps and
  raptor-hal both use plain degrees (90/180/270).
