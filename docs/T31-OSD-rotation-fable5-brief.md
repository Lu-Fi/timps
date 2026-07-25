# Brief for Fable 5 — can the T31 rotated-OSD position limit be lifted?

**Ask:** We hit a hard libimp constraint that confines hardware OSD/privacy
overlays to the **top band** of a 90/270-rotated T31 stream. We fixed everything
reachable from the application; the remaining limit is inside the closed-source
`libimp`. **Question for you: by disassembling `libimp`, is there any call,
struct field, or ordering that lets an OSD overlay be placed BELOW that band
(i.e. anywhere in the full portrait) without corrupting the video?** If it is
genuinely unreachable, a definitive "no" (with the reason) is also a valid,
useful answer.

Everything below is on-device confirmed on the real camera, not theory.

---

## Environment

- SoC **Ingenic T31**, sensor SC4336P, camera `wuuk_y0510_t31x_sc4336p_ssv6158`.
- **libimp SDK `1.1.6-a6394f42`, built Dec 29 2022** (`IMP_System_Init` banner).
- App = **timps** (pure-C streamer). Rotation via
  `IMP_FrameSource_SetChnRotate(chn, rotTo90, w, h)` (software transpose inside
  libimp; pre-rotation dims; called BEFORE `IMP_FrameSource_CreateChn`).
- Pipeline: `FrameSource --Bind--> OSD group (DEV_ID_OSD) --Bind--> Encoder`.
- Test stream: pre-rotation **1280×704**, `rotation=270` → output **704×1280**
  portrait. Scaler active (2560×1440 sensor → 1280×704).

## The confirmed mechanism (what we proved)

1. **The video is composited/rotated correctly** — `VBMCreatePool` allocates the
   frame pool at the **PRE-rotation landscape** dims `1280×704`
   (`w=1280 h=704 f=842094158`, size 1351680 = 1280·704·1.5 NV12). The rotation
   to 704×1280 happens on the encoder read path.
2. **OSD composites UPRIGHT in portrait space**, at the rect coords we pass. We
   proved libimp does **not** rotate the overlay with the frame: a bitmap we
   pre-rotated 90° came out **90°-rotated** in the output (net = our rotation
   only). So the overlay's on-screen position == the rect x/y we set, in the
   704×1280 portrait space.
3. **BUT the range check uses the PRE-rotation (landscape) dims.** libimp rejects
   any overlay whose rect exceeds `picWidth×picHeight = 1280×704`. Exact logcat:
   ```
   E/OSD: osd_draw_cover_pic(617): invalid param p0(594,1240)-p1(693,1269),
          (w,h)=(1280,704), You should keep the smallest coordinates within
          the picture range!!!
   E/OSD: ipu: ipu_osd error ret = -1
   ```
   `(594,1240)` is a logo at portrait y=1240; `1240 > 704` → rejected. The
   rejected region is **invisible AND floods one `ipu_osd error` per frame**,
   which poisons the whole OSD group pass (blanks every overlay in the group).
4. Net constraint: on the 704×1280 portrait, OSD/privacy only work for
   `y + h ≤ picHeight (=704)` — i.e. the **top ~704 rows**. Everything else is
   the app's problem to clamp/discard.

### Why the obvious app-side fixes don't work

- **Raise `picHeight` to 1280** so the check passes → **corrupts the video**
  (VBM pool becomes 1280×1280, output = green garbage). Verified on-device.
- **Feed the OSD group the post-rotation dims (704×1280)** directly → libimp
  rejects with the same "invalid param" (the group is clamped to the frame).
- **Pre-rotate the overlay bitmap + transform coords into landscape** → the
  overlay then appears **rotated** and at the wrong place, because (per #2) the
  frame does not rotate overlays; the composite uses the rect coords verbatim.

So the check and the composite disagree on orientation: **composite = portrait
(704×1280), range-check = landscape (1280×704)** — and we can't reach the value
the range-check reads without breaking the video pool.

## What ships today (all on-device clean, 0 IPU errors)

- **Square rotated (≤704×704):** OSD/privacy anywhere. `picHeight == picWidth`
  so the check never bites.
- **Non-square portrait (704×1280):** OSD/privacy work in the **top 704 band**;
  overlays configured lower are clamped up, taller-than-band ones discarded
  (with a startup warning). Video unaffected.
- **180°:** unaffected (no W/H swap → no mismatch).
- App code: `src/hal/hal_ingenic.c` (FS chnAttr stays pre-rotation landscape —
  the *encoder* carries post-rotation dims) + `src/hal/imp_osd.c` (even-align +
  top-band clamp, gated on rotation). Full trail: `T31-OSD-rotation-handoff.md`.

## The precise questions for the disassembly

Concrete entry points (from `docs/T31-OSD-disasm-findings.md`, same libimp):
`OSD_Draw_Layer_Cover_Pic` (~`0xBFECC`), `osd_update_left` (~`0xC1BF4`),
`VBMGetFrame` (~`0x1FE70`), `on_framesource_group_data_update` (~`0x9A654`);
the IPU-descriptor W/H swap at ~`0xC05C8`, the CW/CCW marker check at ~`0xC0274`.
The runtime error is `osd_draw_cover_pic(617)` → `ipu_osd error`.

1. **Where does the range-check `(w,h)` come from?** In the function that logs
   "keep the smallest coordinates within the picture range" (`osd_draw_cover_pic`
   / `OSD_Draw_Layer_Cover_Pic`), which struct field feeds the `(1280,704)` it
   compares against — the FrameSource chnAttr `picWidth/picHeight`, the OSD
   group's own attr, or a field on the frame object? Is there a **separate,
   settable OSD-group dimension** (some `IMP_OSD_Set*`/attr) that this check
   uses, independent of the FS `picHeight`?
2. **Is the composite target stride/height independent of the check?** i.e. does
   the actual pixel write use the post-rotation 704×1280 layout while only the
   *validation* uses the landscape dims? If the check is simply reading the wrong
   field, is there an API path (or a benign attr write) that makes it read the
   post-rotation dims — so a `y=1240` overlay validates AND draws correctly?
3. **Any rotation-aware OSD path?** The prior disasm noted a `rotTo90==1` (CCW)
   branch (`osd_update_left`, IPU-descriptor swap `0xC05C8`). Does any code path
   make `OSD_Draw_Layer_Cover_Pic` swap the check dims for a rotated channel, and
   can it be triggered from the public API (e.g. a specific bind order, a group
   attr, or `SetChnRotate` argument convention)?
4. **Alternative composite point.** Is there any exported way to composite the
   OSD **after** the rotation (post-rotation portrait frame) — e.g. a second
   FrameSource pass, `IMP_Encoder_SetFisheyeEnableStatus`, an unbound encode
   (`IMP_Encoder_YuvEncode`/`SendFrame` — believed T23-SDK-1.3.0 only, please
   confirm for this T31 build), or an IVS/OSD ordering trick?
5. If none of the above: **confirm the limit is structural** (check hard-wired to
   the pre-rotation frame dims, no reachable override) so we can close it as a
   documented SDK limitation with confidence.

## Reproduce

```
# on a T31/SC4336P camera with timps built (USE_ROTATE):
scripts/deploy.sh --build --no-run
scripts/osd-rot-test.sh scripts/camera-portrait-nojpeg.conf   # logo at y=1240 -> clamped/reject path
# logcat on the camera shows the osd_draw_cover_pic range-check error verbatim.
```

Full evidence + every dead-end we already ruled out (4-state FS permutation
matrix, scale=0, bitmap pre-rotation, picHeight=1280) is in
`docs/T31-OSD-rotation-handoff.md`. Please don't re-run those — start from the
`OSD_Draw_Layer_Cover_Pic` range-check.
