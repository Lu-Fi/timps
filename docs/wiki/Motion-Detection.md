# Motion Detection

Motion detection uses the Ingenic ISP's own hardware-adjacent motion
engine, **IMP_IVS** ("Intelligent Video System") "move" detection API —
not a software frame-difference algorithm running on the CPU. Implemented
in `src/hal/imp_motion.c`, configured under `motion.*`
(see [Configuration Reference](Configuration-Reference.md#motion--motion-detection)),
and surfaced live via `/control`'s `"motion"` object and the `/events`
`motion` SSE type (see [HTTP /control API Reference](HTTP-Control-API.md)).

## Availability

Whether `IMP_IVS` move detection exists at all, and how large a grid it
can support, is entirely determined by the vendor SDK headers the binary
was built against (`src/motion_caps.h`, auto-detected via
`__has_include(<imp/imp_ivs_move.h>)`):

- `MOTION_AVAILABLE` — 1 if this build's SDK has the move API, 0
  otherwise (feature fully compiled out as a no-op stub; `GET /control`
  reports `"available":0`).
- `MOTION_MAX_CELLS` — taken directly from the SDK's
  `IMP_IVS_MOVE_MAX_ROI_CNT` constant, **never hardcoded**: 52 on most
  SDKs (T10/T20 3.12.0, T21, T23, T30, T31, T40, T41, C100), but only
  **4** on the old T10/T20 3.9.0 SDK. `make sim` simulates the common
  52-cell case so the WebUI motion overlay can be exercised without
  hardware.

## The grid model

Motion detection is a `motion.cols` × `motion.rows` **grid** of IVS
move-ROIs, split evenly (by integer pixel boundaries) over the frame of
`motion.monitor_stream`. Each cell reports motion independently. Cell
index is **row-major**: `index = row*cols + col` — the same order IVS's
own `retRoi[]` result array uses and the same order `/control`'s
`"active"` array and the `motion` SSE event use.

`cols*rows` is clamped to `MOTION_MAX_CELLS`; `motion.cols`/`motion.rows`
each clamp against the *current* value of the other axis (never the
reverse), so re-applying the same pair via `/control` is idempotent and
doesn't trigger a spurious rebuild.

**Sensitivity mapping**: the UI-facing `motion.sensitivity` range is
0–255; IVS's native move-detection sensitivity is 0–4. The mapping is a
simple linear scale, `v*4/255`, applied uniformly to every cell (there is
no per-cell sensitivity). Because of this quantization, `/control`'s
change-detection additionally compares the *mapped* value, not just the
raw one — a sensitivity change that maps to the same IVS level (e.g. 128
→ 129) is treated as unchanged and skips the (expensive) IVS grid
rebuild.

## `monitor_stream` and FrameSource sharing

`motion.monitor_stream` selects which video stream's FrameSource feeds
the IVS grid. The HAL pins that stream's FrameSource channel active for
as long as motion detection is running (via the same refcounted
`fs_use()`/`fs_unuse()` mechanism [Architecture](Architecture.md) uses for
on-demand video/JPEG start), specifically so the framesource's normal
idle-stop debounce never disables the frames IVS needs — motion detection
keeps working even with zero RTSP/HTTP clients connected to that stream.

## Privacy-mask exclusion

Motion and an OSD privacy cover mask share the same zero-copy FrameSource
buffer, and the cover is drawn **in-place** on that buffer. If a grid
cell's rectangle intersects any *enabled* `privacy<S>.<N>` region on the
monitored stream, that cell is excluded from the IVS ROI list entirely —
otherwise an unmasked cell sitting over the cover would see raw
pre-cover/fill-color flicker and report motion permanently. Excluded
cells simply never get a `retRoi` slot and always read as inactive in
status output.

## Live rebuild behavior

`motion.enabled`, `sensitivity`, `cols`, `rows`, and `monitor_stream` are
all live-applicable via `/control`. `enabled`/`cols`/`rows`/
`monitor_stream` are creation-time attributes in the SDK — changing any
of them requires tearing down and rebuilding the entire IVS group and
move interface. To avoid rebuilding once per key in a multi-key POST
(e.g. the WebUI's motion settings page saving cols/rows/sensitivity
together), the HAL registers a batch-commit callback with the hub
(`hub_set_control_commit_cb`): these keys just flag "needs rebuild," and
the actual stop/destroy/recreate cycle runs at most **once**, after
`/control` has applied every key in the request.

**`sensitivity` alone is the one exception** and takes a cheaper fast
path: `IMP_IVS_MoveParam.sense[]` is a runtime-updatable per-ROI
parameter on all 9 platforms, so `imp_motion_set_sensitivity()` reads the
running channel's move params via `IMP_IVS_GetParam`, rewrites every
configured ROI's `sense[]` entry to the newly-mapped 0–4 level, and
pushes it back via `IMP_IVS_SetParam` — no stop/destroy/recreate at all.
If a geometry/`monitor_stream`/`enabled` change arrives in the **same**
request, the full rebuild supersedes this fast path and re-applies
sensitivity as part of it; and if the fast path itself fails for any
reason (channel not actually running, or the SDK `Get`/`SetParam` calls
report an error), the code falls back to the full rebuild rather than
leaving the channel in a half-updated state.

## `cooldown_ms`, `hold_ms`, and `skip_frames`

These three are related but distinct, and are all **config-file-only** —
none of them has a `/control` POST path:

- **`motion.skip_frames`** (default 5, floor 1) maps directly to
  `IMP_IVS_MoveParam.skipFrameCnt` — an IVS-native "analyze every Nth
  frame" knob. Higher values are cheaper but slower to detect; lower
  values are snappier but cost more CPU.
- **`motion.hold_ms`** (default 800, `0` = off) is a **status latch**,
  not a detection parameter. IVS only flags a cell `true` on the exact
  frame it moved and clears it on the very next processed frame, so an
  asynchronous reader (an `/events` subscriber, a `/control` poller)
  usually races the clear and observes all-zero even during genuine
  motion. Each cell that fires is held "active" for `hold_ms` after its
  last hit so any reader reliably sees it — every transition is *also*
  pushed into a small snapshot queue for `/events`, since level-sampling
  alone would still miss two transitions landing between two samples.
- **`motion.cooldown_ms`** (default 5000, floor 250) rate-limits how
  often the `on_motion` hook script itself can fire, independent of
  `hold_ms`. The cooldown clock is kept at file scope (not reset by an
  IVS rebuild that a config change triggers), so a live `/control` edit
  right after a real detection can't accidentally let the hook re-fire
  before the configured cooldown has actually elapsed.

## The `on_motion` hook

`motion.on_motion` (default `""` = disabled, config-file-only, and —
unlike almost every other config key — not even readable back via
`GET /control`) names a program to run when **any** cell trips. It is
invoked via `fork()` + `execlp()`, **never** `system()` — the hook has no
shell and takes no arguments, so a malicious or malformed `on_motion`
value can only fail to exec, never inject shell metacharacters. The fork
is double-forked so the actual script is reparented to init and reaped
there — motion detection is never blocked waiting on the hook's runtime.

The hook receives context via **environment variables** (avoiding the
need for the script to poll `/control` and race the `hold_ms` decay
window):

| Variable | Meaning |
| --- | --- |
| `MOTION_COLS` | Grid columns in effect. |
| `MOTION_ROWS` | Grid rows in effect. |
| `MOTION_CELLS` | A decimal `unsigned long long` bitmask — bit *N* set means row-major cell *N* fired on **this** trigger (capped to the low 64 bits). |
| `MOTION_TIME` | Unix timestamp (seconds) of the trigger. |

All four values are daemon-computed numerics with no attacker-controlled
content, so passing them via `setenv()` introduces no injection surface.

## The T23 software-rotation coordinate caveat

This is the subtlest part of the motion subsystem and is worth
understanding precisely if you're debugging a rotated T23 stream's motion
grid.

On **T31/T40/T41's hardware rotation path**, and with no rotation at all,
`IMP_FrameSource_SetChnRotate`/the I2D block rotates *inside* the
framesource, so IVS — bound downstream of the framesource — genuinely
sees the **already-rotated** frame. In that case the grid is built
straightforwardly over the post-rotation ("effective") dimensions
(`ms_vstream_eff_dims()`).

On **T23 with `USE_SW_ROTATE`** (see [Platform & SDK Support](Platform-SDK-Support.md)),
rotation instead happens entirely in **software**, downstream of
`IMP_FrameSource_GetFrame`, inside the HAL's dedicated software-rotate
encode thread. IVS is bound directly to the **pre-rotation** framesource
output and never sees a rotated frame at all — it only ever sees the raw
sensor-orientation image.

The grid the user configures (and the WebUI overlay draws) is always in
**displayed** (post-rotation) space — the space the viewer actually
watches. `imp_motion.c` reconciles this by taking an entirely different
branch when building the IVS ROI list on this path:

1. Lay the `cols`×`rows` grid out over the **displayed** frame dimensions
   (`dW = h, dH = w` — the swap of the raw sensor dims, since a 90/270
   rotation swaps width and height).
2. For each displayed cell, **inverse-map** its rectangle into the
   corresponding rectangle in the pre-rotation source frame, using the
   exact inverse of the software rotator's (`nv12_rotate90()`) forward
   transform:
   - 90° (CW): `src.x = disp.y`, `src.y = (h-1) - disp.x`
   - 270° (CCW): `src.x = (w-1) - disp.y`, `src.y = disp.x`
3. That inverse-mapped rectangle — in real, unrotated sensor coordinates
   — is what actually gets registered as the `IMP_IVS_MoveParam.roiRect`
   for IVS to watch.
4. The ROI-to-cell-index table (`g_roi_cell[]`) still records the
   **displayed** row-major cell index, so when IVS reports motion in a
   given ROI slot, it is correctly attributed to the cell the user sees
   on screen — not the cell's position in the raw sensor orientation.
5. Privacy-mask exclusion is likewise tested in **displayed** coordinates
   (matching where the user actually configured their privacy rectangles
   and consistent with the non-rotated path).

In short: on the T23 software path, IVS itself always operates on
unrotated sensor coordinates (there's no way to make it do otherwise on
this SoC/SDK combination), but the grid geometry, ROI placement, and
status reporting are all transformed so that from the outside — the
config file, the WebUI overlay, `/control`, `/events` — the grid behaves
exactly as if it were laid directly over the frame the viewer sees. This
is a deliberate, tested fix (tagged "M1" in the code) for what would
otherwise be a grid that silently watches the wrong part of the rotated
image.

See [Platform & SDK Support](Platform-SDK-Support.md) and
`docs/rotation.md` for the rest of the rotation feature's platform-by-
platform behavior.
