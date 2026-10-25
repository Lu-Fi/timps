# timps `/control` error codes

**Applies to timps v1.11.1.** All of it is
**since v1.10.1**; older builds have no `errors` object, no
`rotation_state` and no `caps.err_v`. 1.11.0 adds no code (`caps.err_v`
stays `1`).

Source: `src/err_codes.h` (the numbers), `src/control.c` (where a POST
reports them), `src/hal/fs_rot_env.h` (the rotation envelopes). The WebUI
text for each code is in thingino-firmware
`package/timps/files/www/a/timps-errors.js`.

## Where codes appear

| Place | Shape | Meaning |
| --- | --- | --- |
| `POST /control` reply | `"errors":{"video1.rotation":204,"image.bogus":102}` | One entry per key that was refused, not applied, or will not take effect at the next start. **Present only when there is at least one entry**; a clean POST has no `errors` key at all. `errors_truncated: true` = the object overran its 512-byte buffer (the other lists keep their own counts). |
| `GET /control` → `video.<N>.rotation_state` | int | `0` = the stream runs as configured at the **last start** (also when it is not rotated). Anything else = the `2xx` code why it runs **unrotated** although `videoN.rotation` asks for 90/270. Only in `USE_ROTATE` builds (like `eff_width`). Its companion `video.<N>.rotate_path` (**since v1.11.0**, `off`/`sw`/`hw`/`band`) says how a running stream is rotated, not why. |
| `GET /control` → `caps.err_v` | int | Version of this table, currently `1`. A client that meets a code it has no text for, or an `err_v` above the one it knows, should show the number. |

`errors` sits next to `rejected`, `ignored`, `unsupported` and
`deferred_keys`; it does not change any count or the HTTP status. The status
rules are unchanged: 200 when at least one known key was applied, 409 when
all were refused, 422 when none was known.

Rotation is restart-only. So a `202`..`206` in a POST reply is a **prediction**:
the value is stored and persisted (counted in `accepted`, listed in
`deferred_keys`) because a later POST may still fix the geometry, but the next
start will bring the stream up unrotated for this reason. After the restart,
`rotation_state` is the truth. The prediction is made only when the request
touched that stream's `rotation`, `width`, `height`, `fps` or `enabled`, so an
unrelated `bitrate` change does not repeat it.

## Codes

| Code | Name | Meaning | Typical fix |
| --- | --- | --- | --- |
| 101 | `ERR_VALUE_INVALID` | Value refused: `null`/`undefined`, empty string on a non-string key, or a `videoN.rotation` that is not a rotation at all (e.g. `45`, stored as `0`). | Send a valid value. `videoN.rotation`: `0`, `90`, `270` (`180` on T40/T41). |
| 102 | `ERR_KEY_UNKNOWN` | Key not known to this build (typo, wrong section, compiled out). Also listed in `ignored`. | Check spelling against `GET /control?fields=1`; check `caps` for the feature. |
| 103 | `ERR_KEY_FILE_ONLY` | Key exists but cannot be set over `/control` (e.g. `videoN.imp_chn`, `videoN.jpeg_quality`, `general.loglevel`, `motion.on_motion`). Also listed in `ignored`. Like `ignored`, only sections `/control` handles are scanned: an `rtsp`/`http` object is not reported at all. | Edit `/etc/timps.conf`, then `/etc/init.d/S95timps restart`. |
| 104 | `ERR_KEY_NOHW` | Feature absent on this SoC. Also listed in `unsupported`. **Since v1.11.0** also `image.fcrop_upscale_pct` where `caps.fcrop.upscale_max` is -1 (not T23/T31 OpenIMP, or a driver without the `fcrop_upscale_pct` parameter; decided at run time). | None; the key has no effect on this camera. |
| 201 | `ERR_ROT_UNSUPPORTED` | The rotation value has no apply path in this build/SoC and was stored as `0`: `USE_ROTATE` off (`BR2_PACKAGE_TIMPS_ROTATE`), T30/C100, T23 without `USE_SW_ROTATE`, T10/T20/T21 with the vendor libimp, or `180` anywhere but T40/T41. | `caps.rotation` lists what this build can do. For a 180° picture use `image.hflip=1` + `image.vflip=1`. |
| 202 | `ERR_ROT_MAIN` | T10/T20/T21 (OpenIMP) rotate only the sub stream; `video0` is refused. | Rotate `video1`, keep `video0.rotation = 0`. |
| 203 | `ERR_ROT_ALIGN` | Width/height not aligned. T31: both multiples of **64** (`1280x704`, `640x384`). T10/T20/T21: multiples of **16** (`640x368`, not `640x360`). T23 (SW rotate): the source **height** (= encoded width) a multiple of 16. | Change `videoN.width`/`videoN.height`. |
| 204 | `ERR_ROT_SIZE` | Picture too large. T31: at most 1280x704 pixels. T10/T20/T21 and T23: at most 704x576 pixels. | Lower `videoN.width`/`videoN.height`. |
| 205 | `ERR_ROT_FPS` | Frame rate too high. T31 and T23: at most 15 fps (`videoN.fps`). T10/T20/T21 have no fps limit. | `videoN.fps` ≤ 15. |
| 206 | `ERR_ROT_LIB` | T10/T20/T21: the libimp cannot rotate - no `IMP_FrameSource_SetChnRotate`, or the OpenIMP stub that accepts it without rotating (thingino `ciao` pin `9cab219`). In a POST only after a start has seen it. | OpenIMP with T1x rotation (`6b7f5ae` or later). |
| 207 | `ERR_ROT_REFUSED` | libimp refused the rotation at start (only in `rotation_state`): T31 `SetChnRotate` failed, a T10/T20/T21 OpenIMP refused the geometry (`6857610`, `0c02f45`, `636b6ac` refuse every 90/270), or T23 `IMP_Encoder_YuvInit` failed. | Read the `video<N>:` WARN line in `logread`; try a smaller, aligned sub-stream geometry; on T1x upgrade OpenIMP. |
| 208 | `ERR_ROT_SMALL` | T23 SW rotate: the source height (= encoded width) is below 256. | `videoN.height` ≥ 256. |

Reserved, no codes yet: **3xx** FrontCrop (`image.fcrop_*`, see
`caps.fcrop.state`), **4xx** encoder ROI (see `caps.roi.rejected`), **5xx**
sensor. Codes are never renumbered; a new code bumps `caps.err_v`.

## Example

```sh
curl -s -X POST http://127.0.0.1:8880/control -d '{"video":{"1":{"rotation":90}}}'
```

On a T20 with OpenIMP and a 640x360 sub stream:

```json
{"ok":true,"accepted":1,"changed":1,"rejected":0,"not_persisted":0,"pending":0,
 "deferred":1,"deferred_keys":["video1.rotation"],"ignored":[],"unsupported":[],
 "errors":{"video1.rotation":203},"applied":{}}
```

Answer: stored, but after a restart the sub stream still runs unrotated
because 360 is not a multiple of 16; set `video1.height = 368` (or `576`),
then `/etc/init.d/S95timps restart`, and check `video.1.rotation_state` is `0`.
