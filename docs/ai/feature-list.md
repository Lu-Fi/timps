# timps - feature overview

**timps** ("Tiny IMP Streamer") is a lean RTSP/fMP4/MJPEG streamer for
IP cameras with an Ingenic SoC running [thingino](https://github.com/themactep/thingino-firmware).
It is written in plain C directly on top of the vendor library `libimp` – without
live555, libconfig, libwebsockets or libschrift – and is an alternative to
prudynt-t / raptor.

* Source code: <https://github.com/Lu-Fi/timps>
* Documentation: `docs/wiki/` in the repo (Home, Architecture, Configuration-Reference,
  HTTP-Control-API, Streaming-Protocols, Day-Night, Audio, Motion-Detection,
  Recording-Timelapse, Rate-Control-\*, Building, Logging, Testing-QA,
  Platform-SDK-Support)
* Applies to timps v1.12.0 (2026-11-07);
  stripped `timpsd` about 410 KB (410 108 bytes: mipsel, T31 with
  vendor libimp, WebRTC, TLS and rotation on; state of `main` 2026-10-06).
  Statements marked **since vX** are new in that release – on a
  camera with an older version the behaviour described before applies.
* Configuration: a flat text file `/etc/timps.conf` in the format `key = value`
* A single binary (`/usr/bin/timpsd`), started via `/etc/init.d/S95timps`

---

## Streaming & protocols

* **RTSP server (own implementation, no live555)** – video + audio,
  `rtsp.enabled`, `rtsp.port` (default 554). Paths are freely selectable per stream
  (`video0.rtsp_path = /ch0`, `video1.rtsp_path = /ch1`).
* **Two video streams** (`video0.*` = main stream, `video1.*` = sub-stream), each
  H.264 or H.265 (`videoN.codec = h264 | h265`), with its own resolution, FPS,
  bitrate and rate control. H.265 not on T10/T20/T23 (there it becomes
  H.264). **T30 (since v1.12.0)**: with vendor libimp as configured, with
  OpenIMP only if the libimp exports `OpenIMP_Cap_H265`, otherwise H.264
  with a WARN. Without `videoN.width/height`, 1920x1080 or
  640x360 apply; **since v1.10.0** a smaller
  known sensor size is taken instead (a 1280x720 sensor streams 1280x720, no
  more upscaling).
* **Adjustable RTP MTU** (`rtsp.mtu`, 548–1472, default 1200) – survives
  WireGuard/OpenVPN/PPPoE paths without IP fragmentation.
* **Browser preview via fragmented MP4 (MSE)** – `http://<ip>:8880/`
  or `/stream.mp4`, including sound. Stream selectable via `http.preview_chn`.
* **Adaptive frame dropping per fMP4 client** (`http.adaptive_drop = 1`):
  a slow client alone freezes on its last frame and rejoins cleanly at the
  next keyframe – the encoder and other subscribers
  (Frigate, recording, other viewers) are unaffected.
* **JPEG & MJPEG** – `/snapshot.jpg` and `/stream.mjpeg` (`jpeg.enabled`,
  `jpeg.width/height/quality/fps/imp_chn`), optionally also as a file
  (`jpeg.snapshot_path`). **Since v1.12.0** timps with OpenIMP does not
  publish a JPEG that the library only repeats while the encoder is busy
  (at most 4 in a row, counter `jpeg_reuse` in `GET /control`).
* **JPEG at stream resolution** – an additional JPEG encoder can run alongside any
  video stream (`video0.jpeg = true`, `videoN.jpeg_quality/_fps/_chn`)
  and is fetched via `/snapshot.jpg?chn=N` or `/stream.mjpeg?chn=N` –
  without extra rmem, because the framesource is shared.
* **WebRTC / WHEP (optional, `USE_WEBRTC`)** – `POST /webrtc/whep` takes the
  browser's SDP offer and answers as an ICE-lite peer;
  DTLS-SRTP (`SRTP_AES128_CM_HMAC_SHA1_80`), H.264 video plus G.711 audio,
  `DELETE /webrtc/whep/<id>` ends the session at once. Keys: `webrtc.enabled`
  (0/1/2, default 2), `webrtc.port`, `webrtc.port_max`, `webrtc.channel` (default stream; `?chn=N` on the POST selects
  a different one per session).
  At most 4 concurrent sessions. Deliberate limits: LAN/VPN only (a
  single host candidate, no STUN/TURN, no IPv6), no Opus/AAC/H.265,
  no NACK/FEC, no congestion control.
* **SRT output (optional, `USE_SRT`)** – MPEG-TS over SRT, as listener
  (`srt.mode = listener`, default, `ffplay srt://<ip>:9000`) or as caller
  (`srt.mode = caller` + `srt.host`, for cameras behind NAT, with its own
  reconnect backoff 1 s→30 s). Further keys: `srt.port`, `srt.channel`,
  `srt.latency_ms`, `srt.streamid`, `srt.passphrase`. Link statistics (RTT,
  loss, retransmits) every 10 s in the log and under `srt` in `GET /control`.
* **On-demand encoding** – each stream (H.264/H.265, audio, JPEG/MJPEG) is
  only produced while at least one client is watching. Practically
  0 % CPU when idle. Not configurable, this is the basic behaviour.
* **FrameSource keepalive** (**since v1.10.0**) –
  `general.fs_keepalive` (`-1`/`auto`, `0`/`off`, `1`/`on`, **since v1.12.0
  default `1`**; file only,
  restart; an unknown word is read as `auto` with a warning, so a
  typo turns the keepalive **off** except on OpenIMP T23/T40/T41):
  keeps the FrameSources running from start to stop, the encoders stay on-demand.
  Auto (the default up to v1.11.1) = on only for OpenIMP builds on T23, T40
  and T41 (T23/T41: a FrameSource disable/enable while the sensor runs can
  hang the SoC; T40: without frames the AE stalls); otherwise off. Cost
  of `1`: ~6.3 MB rmem for a 1080p main stream stays allocated, CPU and
  heat are not measured. Effective value: `caps.framesource` in
  `GET /control`.

## Encoder & rate control

* **Rate control modes** `videoN.rc_mode = cbr | vbr | fixqp | smart |
  capped_vbr | capped_quality`, plus `bitrate`, `gop`, `profile` (0 Baseline,
  1 Main, 2 High), `qp`, `min_qp`, `max_qp`, `buffers`. `caps.capped_rc`
  (**since v1.12.0**) says whether `capped_*` really runs capped (`1`) or
  as VBR (`0`).
* **Classic SoC rate control (T10–T30)** additionally via `quality_lvl`
  (0–7), `change_pos` (50–100), `i_bias_lvl` (−3…3) and `fluc_lvl` (H.265 only).
  Live on H.264 streams; **since v1.12.0** on T30 also on H.265 streams
  (vendor libimp and OpenIMP; `fluc_lvl`, `fps`, `gop` and `codec` remain
  restart keys). On T21, H.265 stays restart-bound.
* **T20 macroblock rate control** `videoN.mbrc` (**since v1.12.0**, default
  `0`, restart): vendor libimp and OpenIMP otherwise have it on; at
  1080p15 it cost the streamer ~35 % instead of ~7 % CPU. Off = a somewhat coarser
  first keyframe at low bitrate.
* **Live application of part of the encoder keys** without restart, depending on SoC:
  T10–T30 `rc_mode/bitrate/qp/min_qp/max_qp/quality_lvl/change_pos/i_bias_lvl`;
  T31/C100 `bitrate/min_qp/max_qp/i_bias_lvl`; T40 `bitrate/min_qp/max_qp`;
  T41 `bitrate/min_qp/max_qp`. `qp` (fixqp) is deliberately **not** live on the new-API SoCs
  (T31/C100/T40/T41) – measured on a T31X
  (2026-08-22), the live path does not move the bitstream at all, while the
  boot path spreads by a factor of 6.4 between qp 25 and 42.
  What this build can do live is in
  `caps.video_live`; what did *not* go live per request is listed in the POST answer
  under `deferred_keys`.
* **Encoder telemetry** in `GET /control` under `encoder.<n>` – including
  `encoder.<n>.rc`: what the encoder really holds (read-back path via
  `IMP_Encoder_GetChnAttrRcMode`), separate from the configured target.
  Plus backlog counters and `queue_drops` per stream. **Since v1.11.0**
  on T40/T41 with OpenIMP additionally `encoder.<n>.telemetry` (frames, bytes,
  IDR count, fps, kbit/s, mean and maximum encode time; the WebUI preview
  shows encode time and keyframes).
* **Traceable queue overflows** (*since v1.9.19*):
  `queue_drops` counts discards in the queue of a *consumer*. A
  summary message now also names the kind at most once per 60 s per (consumer kind,
  stream) – `rec`, `rtsp`, `mp4`, `webrtc`, `srt`:
  `chn=0 rec: 12 queue overflows in the last 60s`. In addition, the
  IDR requests for error recovery are limited to 1/s **per stream** (previously
  per consumer), so several slow clients no longer load the shared encoder
  with a multiplied number of keyframes; a suppressed request is not
  lost but is made up later – unless a keyframe is published anyway
  before that, then it is dropped (so an overflow costs one
  forced IDR, not two). *Since v1.9.20* the
  hub itself counts every discard while distributing (audio too, the same for all consumers)
  – so a client that hangs and never fetches again also appears in
  `queue_drops`. RTSP, SRT and WebRTC pass every video loss on to the hub,
  instead of discarding requests within their own 1 s lockout.
  **since v1.9.28** the IDR request of a *starting* client is also subject to
  a lockout of 500 ms per stream (after which it is likewise made up), and
  RTSP `DESCRIBE` as well as the player page only request an IDR when the stream is cold
  – so a reconnect loop or a `DESCRIBE` flood can no longer
  turn every frame into a keyframe.
  No configuration key.
* **Encoder ROI (since v1.10.0)** – `roi<S>.<N>.*` (`enabled`,
  `x`, `y`, `w`, `h`, `qp`, `qp_mode`), up to 4 regions per stream with
  their own QP (absolute or delta), live, H.264 and unrotated streams only
  (on an H.265 or rotated stream: a file value gives a WARN at start, a
  POST is accepted and stored, but has no effect and logs nothing –
  only `caps.roi.streams` shows it).
  Tested on hardware (by the OpenIMP maintainer, not by timps) T10, T20
  and T21 with OpenIMP (T10: QP51 region -25 % bitrate; T21 on by default,
  `OPENIMP_T21_ROI=0` in the environment turns it off → experimental); vendor
  T10/T20/T21 experimental. **T31 with OpenIMP** via `SetChnRoiAttr`
  (tested by the OpenIMP maintainer, in timps host tests only); vendor T31
  refuses (no symbol). Vendor T23 refuses (the encoder only stores the
  region), as do vendor T30 and C100. **Since v1.12.0** T30 with OpenIMP is
  effective (family A, `IMP_Encoder_SetChnROI`, OpenIMP 03dd0ce3, tested by the
  OpenIMP maintainer). **Since v1.11.0** T40/T41 with OpenIMP
  experimental (`OpenIMP_Cap_EncRoi`, QP deltas only). **Since v1.10.1** T23 with
  OpenIMP is effective if the libimp exports `OpenIMP_Cap_T23HelixRoi`
  (OpenIMP v2026.10.11-rc1 or newer), otherwise refused. Delta -25..25 (T21
  and OpenIMP T23 only act at about -12..+13);
  an absolute QP stays absolute only as long as it is at most 25 away from the frame QP,
  otherwise it is sent as a clamped delta (otherwise invalid
  H.264). Tier and ranges are reported by `caps.roi` in `GET /control`.
* **Image rotation (optional, `USE_ROTATE`)** – `videoN.rotation = 0|90|180|270`.
  Hardware 90/270 on T40/T41 (I2D) and T31 (FrameSource). **Since v1.10.0**
  also FrameSource rotation on T10/T20/T21, only in OpenIMP builds and only with
  an OpenIMP that rotates on T1x (first commit `6b7f5ae`), sub-streams only,
  16-aligned, max. 704x576 (e.g. 640x368), even if the libimp reports more;
  not yet tested on hardware.
  timps detects an OpenIMP without rotation at start, the stream then runs
  unrotated. **Since v1.12.0** T23 and T30 also rotate via the same
  FrameSource path (`IMP_FrameSource_SetChnRotate`), only in OpenIMP builds
  (OpenIMP `next` `17082ed3` or newer); OpenIMP reports the limits via
  `OpenIMP_FS_RotateLimits` (currently T23 and T30 1280x720, alignment 16).
  OSD, privacy masks and motion detection stay on the rotated
  stream. The former timps-own software path for T23 and its switch
  `USE_SW_ROTATE` are removed; T23 with vendor libimp does not rotate.
  **Caution T23**: the old software path rotated 90 clockwise, the
  FrameSource path rotates 90 counter-clockwise – a T23 with
  `rotation = 90` now needs `270` (and vice versa).
  True per-channel 180° rotation only on T40/T41; otherwise use
  `image.hflip` + `image.vflip` instead. **Since v1.11.0** `GET /control` reports per
  stream `video.<N>.rotate_path`: `off`, `band`
  (vendor libimp 90/270: OSD/privacy only in the top `eff_width`
  rows) or `hw` (whole image).

## Image & ISP tuning

* Full ISP tuning via `image.*`, all settable live and persistent:
  `brightness`, `contrast`, `saturation`, `sharpness`, `hue`, `hflip`, `vflip`,
  `anti_flicker`, `running_mode` (day/night pipeline), `ae_compensation`,
  `max_again`, `max_dgain`, `sinter_strength` / `temper_strength` (spatial /
  temporal noise reduction), `dpc_strength`, `defog_strength`,
  `drc_strength`, `highlight_depress`, `backlight_compensation`,
  `core_wb_mode`, `wb_rgain`, `wb_bgain`. `wb_rgain`/`wb_bgain` are **since
  v1.10.0** limited per SoC (0..255 on T10/T20/T30, neutral 128;
  0..4095 on T21/T23/T31, neutral 256). On T41 with OpenIMP there are
  `ae_compensation` and `sinter_strength` (since v1.9.33) as well as
  `temper_strength` (**since v1.10.0**). **Since v1.12.0** T40 has
  `ae_compensation` (AE target as on T41) and `max_again` (upper limit of the
  analogue AE gain, 1024 × 2^(value/32)), with vendor and
  OpenIMP libimp.
* **T30 ISP (since v1.12.0, vendor and OpenIMP libimp)**:
  `sinter_strength`, `temper_strength` and `dpc_strength` map 0..255 onto
  the T30's 50..150 % (128 = 100 %, the vendor image; previously 128 was
  written as 128 %), `dpc_strength` via `SetDPStrength`;
  `drc_strength` via `Get/SetRawDRC` (0 = off, 128 = the values read before the first
  change, otherwise manual strength); `defog_strength` via
  `SetAntiFogAttr` (up to 128 off, 129–170 weak, 171–212 medium, from 213
  strong). `colorfx` only with OpenIMP; `scene` does not exist on T30 (the call
  has no effect there), `hue` neither. *Since v1.9.21* `GET /control` reports
  under `image.wb_live` the currently effective WB gains (in auto mode the
  AWB result) – switching to manual with them does not change the image.
* **No blind setting**: which of these controls the given SoC really
  supports is reported by `caps.image` in `GET /control` – the WebUI greys out the rest.
  Unsupported values are stored anyway.
* **Sensor auto-detection** – `sensor.model/i2c_addr/fps/width/height` may
  be missing; timps then reads them from the kernel registry
  `/proc/jz/sensor/sensor0/`. One configuration file thus fits many
  cameras. *Since v1.9.19* the **automatically** detected
  `sensor.fps` is capped at **30** (some drivers report a rate their
  clock does not deliver – the GC2053 reports 40 for a 30 fps mode); *since
  v1.9.20* the limit is the fastest active
  `videoN.fps` if that is higher. An explicitly set value applies
  unchanged. In addition, the set rate is
  read back and logged: `sensor fps: requested N, driver holds n/d,
  set rc=R` (WARN if the driver holds something else). `videoN.fps` is only
  the rate of *this stream* and never sets the sensor rate.
* Optional cap on the AE integration time (`image.ae_it_max_us`) for
  scenes in which the automatic exposes too long.
* **ISP FrontCrop / zoom (since v1.10.0)** – `image.fcrop_enable`,
  `fcrop_x/y/w/h` in sensor pixels: the ISP delivers only this window,
  each stream scales it to its size (min. 64×64, T10 640×360, live).
  Tested (by the OpenIMP maintainer) on T10, T20 and T21 with OpenIMP (T10/T20:
  only downscaled streams, so mostly only the sub-stream; T21: streams
  that fit into the window). T23 and T31 with OpenIMP: effective and live if
  the open-tx-isp driver has the parameter `fcrop_upscale_pct` (b15ef235);
  without it T31 experimental and T23 **only at start** (a POST takes effect only
  after `/etc/init.d/S95timps restart`, needs open-tx-isp 3cf9bdac or
  newer). `caps.fcrop` reports `min_win`, `zoom_max`, `zoomable`/`zoomed` per
  stream. A window smaller than a non-downscaled stream is refused with
  `-EINVAL` (`state` `rejected`, a WARN). Vendor T23, T30,
  T40/T41, C100 and vendor T10/T20/T21 refuse. Tier and state:
  `caps.fcrop`.
* **Crop upscaling (since v1.11.0)** – `image.fcrop_upscale_pct`
  (default -1 = leave the driver value, live, only T23/T31 with OpenIMP): the
  scaler may enlarge the window per axis by this many percent, so at 100
  a window down to half the stream size. Upper limit
  `caps.fcrop.upscale_max` = 100 (tested on the device; T31 hangs hard at
  150). If the value is lowered and the active window no longer fits,
  the crop falls back to the full sensor. Elsewhere a POST answers
  `"unsupported"` (104).

* **Crop on T40/T41 (since v1.11.0)** – with OpenIMP via the ISP AutoZoom
  (`OpenIMP_Cap_AutoZoom`), experimental, without upscaling. T41 live up
  to the driver's limit (1920x1080 with current open-tx-isp, otherwise
  768x432), larger streams briefly restart their framesource; T40 live up to 4K.
* **DRC/DPC/defog on T41 (since v1.11.0)** – `image.drc_strength`,
  `image.dpc_strength` and `image.defog_strength` with OpenIMP
  (ISP module shares 2/3/4, 128 = vendor image), live.
* **Test pattern (since v1.11.0)** – with OpenIMP (`OpenIMP_Cap_TestPattern`)
  `{"testpattern":{"mode":0..4}}` and buttons on the WebUI preview:
  static pattern, moving square, motion test with evaluation (hits,
  false alarms, delay), sensor test pattern through the whole ISP; **since
  v1.11.1** mode 5, a still image in the middle of the ISP (after sharpening, T23 with
  OpenIMP c5fedf2a+, `caps.testpattern.isp_mid`). The WebUI shows all of this as a
  pipeline banner sensor → ISP → encoder → IVS. Day/night stays
  on day meanwhile.

## Day/Night (automatic day/night switching)

Replaces thingino's standalone `daynightd` (the firmware package build
disables `S97daynightd` when `USE_DAYNIGHT` is on).

* **The measured quantity is the exposure index**, not the plain gain:
  `D = total_gain × (integration time / max. integration time)` – higher =
  darker. This keeps the measurement meaningful even when `total_gain`
  sits at the floor at 256 (1×).
* **Asymmetric approach (since v1.10.1 the simple core)**: day→night is
  an honest measurement (IR-cut filter closed, illuminator off): above
  `night_gain` or AE at its limit for `day_confirm_s` (30 s, very dark 6 s).
  At night the camera **looks**: IR illuminator off for 8 s (without a click), every
  45 min and at once when the IR-lit level is `jump_pct` (20 %) below the last look
  for 20 s. If the scene is bright enough without IR, it **checks**
  with the day image (one click): below `day_gain` it stays day, otherwise back
  (second click); the next check then needs a darker look and
  waits 1, 2, 4 … min. **Since v1.11.0** light that comes slowly
  (dawn with daylight IR, a fireplace) only asks again when the k of the
  failed check predicts day; sudden light still asks at once.
* **Colour trigger (since v1.11.0)** – `daynight.color_pct` (default 3, 0 = off,
  0..50, live): if the colour of the IR-lit image (global R/G
  or B/G of the AWB statistics, read every 5 s) changes by this many percent for 20 s
  compared with the last look, the camera looks at once. This way even a
  weak lamp that hardly brightens the IR image ends the night in about 40 s
  instead of only at the next 45 min look. Colour looks at most every 5 min,
  the interval doubles up to 45 min; never a click alone. Needs a
  switchable IR LED and AWB statistics in the libimp (T40/T41, T23/T31/C100
  vendor, T10/T20 OpenIMP, OpenIMP T23/T31/T40/T41 only with
  `OpenIMP_Cap_AwbZone`/`_AwbStatistics`); status `daynight.color_trigger`
  (`on`/`off`/`unavailable`) and `color_dev_pct`. Tested on the device only
  on T40 (vendor) and T20 (OpenIMP).
* **Check only close to day (since v1.12.0)** – a look only asks for
  a check with the day image if `k × dark` is at most 4 × `day_gain`.
  Previously colour looks at dusk already asked at 19 ×
  `day_gain` and each cost a pair of clicks. `daynight.day_trigger` now reports
  the threshold actually used and can therefore be below `day_gain`.
* **Darker, but a different colour (since v1.12.0)** – if the IR-lit
  image gets darker while its colour stays shifted by more than
  `color_pct` for 20 s (a lamp goes on while the infrared of other
  cameras in the room goes off), the camera starts a colour look. It takes the darker
  exposure value as reference and keeps the colour reference for
  the look. Previously such a camera stayed in night until the 6 h cycle.
* **Dawn (since v1.12.0)** – a look whose lit level
  already sits at the threshold (lit × 0.9 ≥ threshold) is skipped
  (`look (<why>) skipped`), the IR illuminator stays on; timer and
  requested looks always run. Light without a 10 s step above 15 % counts as
  creeping and is judged with the step threshold and the k of the last
  failed check, also for colour looks; near the threshold
  the camera also looks without a `jump_pct` step (`look (near)`). A lamp
  right after a check no longer becomes the night reference. Fleet calculation
  over 121 mornings: failed checks 178 → 94, day on average 5 min
  later, about half of the morning looks are skipped.
* **Reference only from a steady image (since v1.11.0)** – a look while the
  lit level is still moving (a lamp goes on, a neighbouring camera's look)
  sets no reference; the next steady level does that (at most 60 s).
* **Vendor T21 (since v1.11.0)** no longer reads `daynight.isp_path`: the
  driver corrupted the kernel heap while reading. A detection thread that
  stops ticking reports itself once as an error in the log.
* **Guarantee**: after `heartbeat_max_s` (18 h) without a day-image measurement at the latest,
  a check is made. Cameras without a switchable IR LED check when the
  night level halves and every `heartbeat_s` (4 h).
* **Thresholds**: `daynight.day_gain` (default 768 = 3×, below it is day) and
  `daynight.night_gain` (default 4096 = 16×, above it is night). Old names
  `total_gain_day_threshold`/`total_gain_night_threshold` still work.
  `probe_min_gap_s`, `probe_confirm_s`, `boot_probe` and
  `diagnose_thresholds` are retired since v1.10.1 (one warning per key).
* **Calendar only for `schedule`**: `daynight.mode = auto | schedule`, a fixed
  time window (`time_night_start`/`time_day_start`) or real sunrise/
  sunset from `sun_latitude`/`sun_longitude` with offsets decides only in
  `schedule` mode.
* **Switching via board script**: `daynight.switch_cmd` (default `daynight`)
  is called as `<cmd> day|night` via `fork()+execlp()` (never `system()`).
  The script drives the IR-cut and IR LEDs and reports the mode back via
  `POST /control {"image":{"running_mode":0|1}}`.
* **Diagnostics**: every check logs `check (…): night, exposure N` – N is the
  day-image value for `day_gain`; `daynight.trace_path` writes (on tmpfs only!)
  a CSV decision trace; `daynight.history_s` (default 0 = off, max. 48 h)
  keeps a RAM series that the WebUI tuning curve fetches via
  `GET /control?dn_history=1`. If someone changes the ISP mode from outside,
  the status reports `isp_desync` until the core switches the next time.
* **Idle-Dark (since v1.10.0, default off)** –
  `daynight.ir_idle_off = 1`: at night, when for `daynight.ir_idle_linger_s`
  (60 s, 0..3600) no stream, snapshot, recorder, timelapse and no
  motion detection needs a frame, timps turns off **only the IR LED**;
  IR-cut and ISP stay in night, nothing clicks. Meanwhile the AE keeps measuring
  the ambient light without the LED (a look that lasts): if the value is
  below the threshold for 30 s, it checks with the day image (one click, nobody is
  watching). The first new viewer gets the LED at once (kick, not only at the
  next tick).
  Snapshots right after that wait up to ~3.5 s (max. 5 s) for the AE, streams
  are dark for the first 1-4 s. Requirements: FrameSource keepalive
  (since v1.12.0 default `1`; auto only OpenIMP T23/T40/T41), mode `auto`, `timps-irprobe` with the verbs
  `idle`/`state` (firmware from 2026-10), no white light in
  `daynight.controls`. Why off by default: at 850 nm the glow going out shows
  that nobody is watching or recording. Status `daynight.ir_idle` (`reason`: `disabled`,
  `schedule`, `keepalive`, `no_illuminator`, `white_light`, `hook`), in the
  WebUI as the card "IR light when idle" on the Day/Night page. If
  `timpsd` dies while the LED is dark (SIGKILL, OOM, crash), nothing restarts it;
  but the hook's `idle` (firmware from `3af7d99e4`) turns the LED back on by itself after
  at most ~15 min – every re-assert (8 s, 16 s, then every
  10 min) extends this deadline, and if the board has meanwhile gone to day, it stays
  off. Choose `ir_idle_linger_s` longer than `timelapse.interval_s`, otherwise
  every capture turns the LED on once.
* Manual operation at any time via `daynight.enabled = 0` plus
  `image.running_mode` or the WebUI toggle.

## Motion detection

* **Grid of IMP_IVS move ROIs** over the frame of `motion.monitor_stream`;
  `motion.cols` × `motion.rows` cells report motion individually – the WebUI builds
  a live overlay on the preview from this.
* `motion.enabled`, `motion.sensitivity` (0–255, mapped onto the SDK levels 0–4),
  `motion.hold_ms` (afterglow of a cell, default 800),
  `motion.skip_frames` (every Nth frame, default 5),
  `motion.cooldown_ms` (default 5000). `hold_ms` and `skip_frames` can be
  POSTed and are applied live via the batched IVS grid resync
  at the end of the request; `cooldown_ms` and `on_motion` are deliberately
  settable only in the configuration file.
* **Hook** `motion.on_motion` (thingino default `/usr/sbin/timps-motion`) is
  started rate-limited and without a shell via `posix_spawn()` – deliberately settable only from
  the configuration file, not via HTTP.
* The SDK's cell budget (`IMP_IVS_MOVE_MAX_ROI_CNT`, usually 52) is reported as
  `caps.motion.max_cells`; the status contains `active[]`, `last_ms` and a
  `stalled` flag when IVS stops delivering results.
* **Push instead of polling**: every grid change goes out losslessly via
  `GET /events?stream=motion` (32-entry ring with a cursor per connection).

## Recording & timelapse

* **Local recording** (`USE_RECORD`) as fragmented MP4 on the SD card:
  `record.enabled`, `record.channel`, `record.mode = continuous | motion`,
  `record.dir`, `record.name` (strftime), `record.segment_s`,
  `record.pre_roll_s`, `record.post_roll_s`, `record.min_free_mb`,
  `record.audio`. Stored under `<dir>/<hostname>/records/<name>.mp4`,
  segment changes always at a keyframe. **since v1.9.28**: a `record.dir` or
  `timelapse.dir` on the root filesystem (typically: an SD mount point without a
  card) is refused instead of writing into the flash (`/tmp` stays
  allowed); the pre-roll keeps the keyframe at which its window starts
  (previously it often collapsed to almost nothing); files with a timestamp before
  NTP synchronisation (~1970) count as the newest when cleaning up,
  instead of being deleted first.
* **Clean cut instead of decoder garbage after a queue overflow**
  (*since v1.9.19*): once the recording queue has
  overflowed, all still pending P-frames refer to frames that are
  not in the file. Up to v1.9.18 they were written anyway (up to
  a whole GOP of visible residue after the gap); now the segment pauses and
  resumes only at the next keyframe – the same logic as
  `http.adaptive_drop` for the fMP4 clients. The gap becomes somewhat
  longer, but the image after it is clean; in `motion` mode the
  pre-roll ring is also discarded. Audio pauses too. An overflow that only hit **audio**
  triggers none of this – the GOP is intact.
* **Honest storage management**: old segments are pruned until
  `min_free_mb` is free – if the value is unreachable for the card,
  the recorder refuses to record and gives the reason in `record.last_error`, instead of deleting all
  existing recordings and missing the target anyway.
  The status also contains `write_errors`, `manual_off` and
  `motion_gate_enabled`.
* **Manual start/stop** via `POST /control {"record":{"active":1|0}}`
  (the WebUI's record button) as well as a **single clip on request**
  (`{"record":{"clip":"/tmp/x.mp4","seconds":6}}`) – this is the way
  send2/Telegram fetch motion videos. **since v1.9.28** a running
  clip no longer blocks other `POST /control`; a second clip meanwhile is
  refused (`rejected`).
* **Timelapse** (`USE_TIMELAPSE`): periodic JPEGs to
  `<dir>/<hostname>/timelapses/<name>.jpg`, `timelapse.enabled/channel/dir/
  name/interval_s/keep_days`, written atomically (tmp + rename), older images
  are pruned after `keep_days` (0–3650, **since v1.9.28**; a huge value
  previously deleted the whole archive on 32-bit `time_t`). All keys live via
  `/control`.

## Audio

* **Capture codecs** `audio.codec = aac | pcmu | pcma | opus | none`
  (AAC via libfaac, Opus via `USE_STREAM_OPUS`, RFC 7587).
  `audio.samplerate` (8000–96000, default 16000), `audio.bitrate`,
  `audio.channels` (1 mono, 2 = simulated dual-mono stereo, AAC only).
* **Second G.711 encode for WebRTC** – `audio.codec2 = pcmu` encodes the same
  PCM a second time onto its own hub source, so that a WHEP session gets sound
  while RTSP, fMP4 preview and recording stay on the AAC primary stream.
* **Microphone chain live**: `audio.volume`, `audio.gain`, `audio.alc_gain`
  (analogue PGA, T21/T31/C100 only), `audio.mute`. Effective only at the next start
  (despite being POST-able): `high_pass` (default on), `agc`,
  `agc_target_dbfs`, `agc_compression_db`, `ns` – libimp would otherwise pull its
  own record thread out from under itself.
* **Speaker natively via IMP_AO** – timps owns the output device itself,
  no external `/bin/iac`. `audio.spk_enabled`, `audio.spk_volume`,
  `audio.spk_gain`.
* **ONVIF audio backchannel** (`USE_BACKCHANNEL`): an RTSP client speaks onto
  the camera speaker. `audio.backchannel`, `audio.backchannel_codec =
  pcmu | pcma | aac` (AAC needs `USE_BC_AAC` / libhelix-aac),
  `audio.backchannel_rate`. Exactly one speaker at a time.
* **Browser intercom `/talk`** (`USE_BC_WS`): RFC 6455 WebSocket,
  browser microphone → camera speaker, G.711 µ-law in 20 ms frames.
  `audio.talk_ws` has three values: 0 off, 1 only over TLS (`wss://`),
  2 also over `ws://`.
* **Echo cancellation** `audio.aec = 1` (takes effect at the next AO open, only
  useful when microphone and speaker run at the same time).
* **System sound queue** (`USE_PLAY`): FIFO at `/run/timps/audio_out` with
  the same `PLAY`/`STOP` protocol that prudynt/raptor's `/usr/sbin/play`
  speaks – Wi-Fi captive portal announcements, update chime, Home Assistant TTS.
  WAV, raw PCM16 and G.711 always; Ogg Opus with `USE_PLAY_OPUS`.
  Playback via `POST /control {"speaker":{"play":"chime_1.wav"}}`, the
  available files are listed in `caps.play.sounds`.

## OSD & privacy

* **Own TrueType rasterizer** (no libschrift), up to 8 overlays
  **per video stream**: keys `osd<S>.<N>.<field>` (e.g. `osd0.0.text`,
  `osd1.2.x`), old `osd<N>.<field>` keys still apply to all streams.
* Item types `text` and `logo`; fields `enabled`, `text`, `x`, `y`,
  `font_size` (absolute pixels), `color`/`transparency` (0xAARRGGBB),
  `outline` + `outline_color` (readability outline), `logo`/`logo_w`/`logo_h`,
  optional font per item.
* **Positioning**: 0 = centred on the axis, positive = distance from
  left/top, negative = from right/bottom.
* **Placeholders**: strftime tokens plus `{hostname} {ip} {mac} {fps} {bitrate}
  {uptime}`, stream-specific `{fpsN}`/`{bitrateN}`, as well as any custom
  `{name}` from a file with a compiled-in path (`/tmp/timps_osd.vars`) that
  a script can write - the path is **since v1.9.28** **not** configurable (not even
  via the config file): it used to be (`osd.vars_file`), but a
  settable path could point at `timps.conf` itself and render its
  credentials into the video via a `{placeholder}`.
* Global OSD keys: `osd.enabled`, `osd.monitor_stream`, `osd.font_path`,
  `osd.supersample` (anti-aliasing 1–4, default 2), `osd.hinting`
  (geometric autohinting for small text, runtime default 1, but only takes effect
  if `BR2_PACKAGE_TIMPS_OSD_HINTING` compiled the pass in).
  `enabled`, `font_path`, `supersample` and `hinting` can be set via `/control`,
  but take effect only after a restart; `monitor_stream` takes effect
  immediately (since v1.9.20 the API also reports it that way: `caps.restart` and
  `deferred_keys`).
* **Privacy masks**: up to 4 rectangles per stream
  (`privacy<S>.<N>.{enabled,x,y,w,h,color}`), as IMP OSD cover regions,
  movable live. Availability is reported by `caps.privacy`.

## Network & security

* **RTSP digest auth** (`rtsp.user`/`rtsp.pass`) and **HTTP digest/basic**
  (`http.user`/`http.pass`, falls back to the RTSP credentials) – own
  MD5 implementation, no OpenSSL. **since v1.9.28**: RTSP closes a
  connection after 5 rejected logins and one that has not reached `PLAY`
  after 60 s; HTTP digest keeps at most 4 nonces per client IP, so that
  a flood from one address does not push out other clients' challenges.
* **Important about the delivered state**: as long as *both* user names are empty,
  the media endpoints (RTSP stream, `/snapshot.jpg`, `/stream.mjpeg`,
  `/stream.mp4`) are reachable on the network without authentication. `/control` and
  `/events` are excepted: without configured credentials they answer
  every non-loopback client with `403`. The thingino package ships
  `thingino`/`thingino` as the preset.
* **Token auth** for `/control`, `/events` and the HTTP media endpoints:
  a random **per-boot token** in `http.token_file` (default
  `/run/timps.token`, mode 0640, so that the local WebUI can read it) and
  optionally a permanent secret `http.token` for remote automation.
  Passed as an `X-Timps-Token:` header or `?token=` (for `<img>`, `<video>`
  and `EventSource`, which cannot set headers). A token never unlocks
  RTSP.
* **TLS (optional, `USE_TLS`, mbedTLS)** – `http.https` has three values:
  0 plaintext only, **1 = both schemes on the same port** (classification
  by the first byte of the connection: 0x16 = TLS handshake), 2 = TLS only
  (plaintext gets `426 Upgrade Required`). Plus `http.tls_cert`/`http.tls_key`
  and optionally RTSPS via `rtsp.tls` / `rtsp.tls_port` (default 322).
  If the certificate is missing, the listener is not bound at all – never
  silently downgraded to plaintext. Session tickets (RFC 5077)
  are on, the ticket key rotates every 12 h.
* **Certificate sharing with the WebUI**: on images with the WebUI, `S95timps` links
  to its `uhttpd` certificate, so that `:443` and `:8880` show the same certificate
  – otherwise the browser would have to trust a self-signed certificate per port separately,
  which is not possible at all for `fetch()` subresources (Safari!).
* **CORS**: media endpoints send `Access-Control-Allow-Origin: *` when
  credentials are set, a valid token comes along or the client is
  local (open camera without a token: no CORS, no PNA permission);
  `/control` and `/events` instead mirror the `Origin:` header
  (with `Vary: Origin`, without credentials).
* **CSRF protection** (**since v1.9.28**): a `POST` authenticated via basic/digest
  whose `Origin` host does not match the `Host` header gets
  `403 bad origin` – browsers also send stored HTTP credentials with
  foreign form POSTs. Token, loopback and requests entirely without
  `Origin` (curl, scripts) are not affected.
* **SRT without a passphrase** is open to everyone, `srt.streamid` is plaintext;
  **since v1.9.28** timps warns at start when RTSP/HTTP credentials are set
  at the same time.
* **Config file**: a rewrite keeps the file permissions **since v1.9.28** (a
  `chmod 600 /etc/timps.conf` persists; previously every POST forced 0644).
* Configurable client limits are reported instead of a client finding them
  by trial: `caps.rtsp_max_clients`, `caps.http_max_clients`,
  `events.max_clients`.

## API & control

* **`GET /control`** – one JSON document with the complete current state:
  `version` (git describe of the binary), `caps`, `image`, `audio`, `sensor`,
  `video`, `osd`/`osd0`/`osd1`, `privacy`, `daynight`, `motion`, `encoder`,
  `queue_drops`, `record`, `timelapse`, `srt`, `tls`, `last_errors`.
* **`caps` object** – what *this* binary can do on *this* SoC:
  `caps.image`/`caps.audio` (list of the really supported controls),
  `caps.osd`, `caps.restart`, `caps.video_live`, `caps.motion`,
  `caps.privacy`, `caps.rotation`, `caps.record`, `caps.timelapse`,
  `caps.backchannel` (incl. `talk_ws`), `caps.play`, `caps.webrtc`.
  A UI should branch on this, not on version numbers.
* **`POST /control`** – nested JSON per section; every recognised value
  is applied live (where possible) **and** written back to `/etc/timps.conf`
  (only the changed keys, atomically via tmp+rename). The answer always has the
  same shape: `accepted`, `changed`, `rejected`, `not_persisted`,
  `deferred`/`deferred_keys`, `ignored` and `applied` (echo of the *effective*,
  i.e. possibly clamped values). Error cases are distinguishable:
  `400 not_json`, `422 unknown_fields` (this build does not know the key),
  `409 values_rejected` (keys right, values wrong).
* **Sub-endpoints**: `GET /control?fields=1` (inventory of all POST-able
  fields, generated directly from the same tables the POST uses),
  `?stats=1` (lean number set for the stats card instead of the 8 KB document;
  **since v1.11.0** with `system`: load, total/available memory, CPU % of the
  machine and of `timpsd` as well as its RSS),
  `?dn_history=1[&last=N|&since=S][&max=N]` (day/night series with cursor paging),
  `?clients=1` (connected clients: IP, protocol, stream, connection time,
  data rate, total transferred and camera latency (capture to send)
  per client, user agent – e.g. Frigate,
  VLC, ffmpeg, browser). **Since v1.9.26** also in the log
  (module `CLIENT`): one line on connect with the user agent, one on disconnect
  with duration and bytes.
* **`GET /events`** – server-sent events instead of polling:
  `?stream=motion,daynight,stats,config`. `motion` and `daynight` send the full state once on
  connect, `stats` ticks every `events.stats_ms`
  (default 2000 ms), `config` reports changes by other clients.
  Keepalive ping every ~12 s, `events.max_clients` (default 8). Open only
  **one** `/events` connection with all types per page: browsers allow 6
  HTTP/1.1 connections per host:port, after that `/control` requests hang.
* **Alias tolerance**: old key spellings (`video0.mode`, `record.segment`,
  `daynight.total_gain_day_threshold`, `osd0.0.stroke`, the flat
  `{"force_mode":"night"}`) are still accepted and mapped to the
  canonical name.
* **Retained error state**: `last_errors` in `GET /control` keeps the last
  WARN/ERROR per module – the camera's 64 KB syslog ring otherwise recycles itself
  before anyone looks.

## WebUI integration (thingino)

* timps is a **plugin of the thingino WebUI**: `timps.webui.json` ends up under
  `/var/www/a/plugins/`, the pages and scripts are installed into the
  WebUI tree during the firmware build (about 45 files).
* Own or replaced pages: `preview.html` (native MSE/fMP4 player with
  WebRTC option, motion overlay, talk button, control bar),
  `streamer-video.html` (encoders of both streams as tabs, live/restart marking,
  restart bar, live values), `streamer-overlays.html` (tabs "Text & logo" and
  "Privacy masks" for both streams, plus "ROI" and "Crop / zoom", position by anchor/pixel, dragging or arrow keys),
  `recordings.html` (tabs Clips/Settings), `timelapse-player.html` (tabs Player/Settings),
  `streamer-config/-image/-sensor.html`, `config-audio.html`, `config-motion.html`,
  `config-photosensing.html` (incl. Idle-Dark card with status chip),
  `tool-sensor-data.html` (day/night tuning curve).
  The old addresses `streamer-main/-substream/-osd0/-osd1.html`, `config-privacy.html`,
  `tool-record.html` and `tool-timelapse.html` redirect to the new pages.
* The previews fetch `/stream.mjpeg?chn=N` and `/snapshot.jpg?chn=N` directly
  from the timps port (with `?token=`), without a proxy CGI.
* **Pages for 1.11.0**: field `color_pct` on the Day/Night page; switch and
  slider for crop upscaling, locked crop sliders without zoom and
  hidden tabs the SoC cannot do; one click corrects the
  geometry of a refused rotation; the overlay editors work in the
  rotated image and mark what a rotated stream cannot show;
  separate CPU and memory tiles with the timps share; one shared
  `/events` connection per page.
* Bridge CGIs under `/var/www/x/` (`timps-imp.cgi`, `timps-token.cgi`,
  `json-heartbeat.cgi`, `json-recordings.cgi`, `json-timelapse.cgi` …);
  ONVIF snapshot URLs (`/onvif/image.cgi`, `image1.cgi`) point to timps.
* **WebRTC capability check in the browser**: `preview.html` rebuilds the answer
  timps would send and tests it against a throwaway `RTCPeerConnection` –
  if the browser cannot handle it (Firefox only accepts Baseline), the
  option appears disabled with a reason instead of a black player.

## PTZ & motors

timps itself does not drive motors – pan/tilt comes from
`thingino-motors` in the thingino ecosystem. What the timps package contributes is the **interface**:
joystick overlay in the preview, `config-motors.html` and the related
CGIs are installed from the timps package when `thingino-motors` and timps
are selected together, and on builds with `BR2_PACKAGE_THINGINO_MOTORS_WS`
the WebSocket variant (instead of CGI polling) is enabled.

## Platform support

* **SoCs**: T10, T20, T21, T23, T30, T31, T40, T41, C100.
* **Two encoder API generations**: new (`IMP_Encoder_SetDefaultParam`) on
  T31/C100/T40/T41, classic (manual `IMPEncoderChnAttr` structure) on
  T10/T20/T21/T23/T30. This explains the different meaning of
  `videoN.bitrate`: classic a hard upper limit, new a target value.
* **ISP tuning API** differs only on T40/T41 (`ISP_NEW_TUNING_API`), which
  drops some `image.*` controls there – see `caps.image`.
* **Audio**: `IMP_AI_SetAlcGain` (`audio.alc_gain`) only on T21/T31/C100.
* **Rotation**: hardware I2D on T40/T41, FrameSource rotate on T31 and,
  only with OpenIMP, on T10/T20/T21 (since v1.10.0, sub-streams only) as well as
  T23/T30 (since v1.12.0); not at all on C100 and with vendor libimp on
  T10/T20/T21/T23/T30.
* **AWB statistics for the colour trigger (since v1.11.0)**: T40/T41, T23/T31/C100,
  T10/T20 with OpenIMP; not on vendor T10/T20/T21, T21 with OpenIMP and
  T30.
* **Host simulation** without hardware: `make sim` builds `timpsd-sim`, which feeds files
  instead of the ISP (`sim.video0`, `sim.audio`, …) – handy for
  WebUI and API work at the desk.

## Operation, logging & diagnostics

* **Tiered logging**: `general.loglevel` (0 err … 3 debug) plus
  `general.debug_modules` – a list of names (MAIN, CONFIG, CTRL, DAYNIGHT,
  HAL_ING, HTTP, HUB, MOTION, OSD, REC, RTSP, SRT, TL, TLS, AAC …) that raises individual
  modules to DEBUG without making everything else noisy. Switchable live via `/control`,
  which is decisive when troubleshooting. `general.syslog = 1`
  additionally writes to `logread`.
* **Version checkable remotely**: `GET /control` returns `version` as the
  git-describe string of the running binary – exactly the test that separates "firmware
  flashed" from "new binary really running".
* **QA script** `scripts/timps-qa.sh --cam <ip> [--profile quick|standard|load|
  soak|drift]`: automated run over streams, rate control,
  motion, audio, recording, day/night and field inventory, including an
  HTML report (`scripts/qa_html_report.py`).
* **Day/night tools**: `dn-isp-probe.sh`, `dn-isp-report.py`,
  `dn-replay.py` and **since v1.10.1** a behaviour corpus of families ×
  camera profiles with measured values (`scripts/dn-families/`, `make
  test-dn-replay`, bounds only: wrong-mode time, switches, light on/off,
  no flapping), plus models of thingino-daynightd and raptor for
  comparison (`others.py`, `compare.py`).
* **libimp check (since v1.10.0)**: a `timpsd` built for the
  other libimp stack (vendor vs. OpenIMP) exits with 78/79 and a reason
  instead of crashing; `timpsd --check-libs` checks only that (80 = could not
  check), thingino's `S95timps` does not start the daemon on 78/79.
* **Optional diagnostic scripts in the image** (`BR2_PACKAGE_TIMPS_DIAG_TOOLS`,
  default off): `timps-selftest`, `timps-logcat-ship`, `timps-dn-isp-log`.
* Host tests for delicate components: `make test-srtp`, `test-stun`, `test-fmp4`,
  `test-config`, `test-fanqueue`, `test-hub-pool`, `test-hub-idr`,
  `test-auth`, `test-clients`, `test-daynight-sun`, as well as **since v1.9.28**
  `test-timelapse-prune` and `test-record-ring`; **since
  v1.10.0** `test-roi`, `test-hub-hold`, `test-libimp-check`, **since
  v1.10.1** `test-stopgate` and `test-dn-replay`
  and the firmware script tests `scripts/test_irprobe.sh`,
  `scripts/test_s95timps_preflight.sh`, `scripts/test_webui_idle.js`.

## Build options (thingino / Buildroot)

Package: `package/timps` – selected via *Streamer Packages → Streamer → timps*
(`BR2_PACKAGE_THINGINO_STREAMER_TIMPS`). Each Kconfig option is mapped onto a
`USE_*` macro.

| Kconfig | Switch | Default | Effect |
| --- | --- | --- | --- |
| `BR2_PACKAGE_TIMPS_FAAC` | `USE_FAAC` | y | AAC audio via libfaac |
| `BR2_PACKAGE_TIMPS_CONTROL` | `USE_CONTROL` | y | `/control` + `/events` (~15 KB) |
| `BR2_PACKAGE_TIMPS_DAYNIGHT` | `USE_DAYNIGHT` | y | native day/night detection |
| `BR2_PACKAGE_TIMPS_RECORD` | `USE_RECORD` | y | SD recording (~11 KB) |
| `BR2_PACKAGE_TIMPS_TIMELAPSE` | `USE_TIMELAPSE` | y | Timelapse (~4 KB) |
| `BR2_PACKAGE_TIMPS_TLS` | `USE_TLS` | y | HTTPS + RTSPS (mbedTLS) |
| `BR2_PACKAGE_TIMPS_WEBRTC` | `USE_WEBRTC` | y | WHEP endpoint (needs TLS + CONTROL) |
| `BR2_PACKAGE_TIMPS_SRT` | `USE_SRT` | n | MPEG-TS over SRT (libsrt) |
| `BR2_PACKAGE_TIMPS_STREAM_OPUS` | `USE_STREAM_OPUS` | n | Opus as `audio.codec` (libopus) |
| `BR2_PACKAGE_TIMPS_BACKCHANNEL` | `USE_BACKCHANNEL` | n | ONVIF backchannel |
| `BR2_PACKAGE_TIMPS_BC_AAC` | `USE_BC_AAC` | n | AAC on the backchannel (libhelix-aac) |
| `BR2_PACKAGE_TIMPS_BC_WS` | `USE_BC_WS` | y on TLS builds | `/talk` for browsers (~8 KB) |
| `BR2_PACKAGE_TIMPS_PLAY` | `USE_PLAY` | n | System sound queue + `/usr/sbin/play` |
| `BR2_PACKAGE_TIMPS_PLAY_OPUS` | `USE_PLAY_OPUS` | n | Ogg Opus in the play queue (opusfile) |
| `BR2_PACKAGE_TIMPS_ROTATE` | `USE_ROTATE` | n | Image rotation |
| `BR2_PACKAGE_TIMPS_OSD_HINTING` | `USE_OSD_HINTING` | n | OSD autohinting (~2 KB) |

Plus the combined option **"Audio backchannel preset"** (MANUAL / FULL / MINIMAL),
which switches backchannel, play queue and the format of the thingino sounds
together consistently – FULL fits T31 boards, MINIMAL also fits a T20 with
~5 MB rootfs.

Direct cross-compiling without a firmware tree is also possible:
`make PLATFORM=T31 CROSS_COMPILE=mipsel-linux-` (the IMP headers come as a
git submodule from `gtxaspec/ingenic-headers`).

## Where to find what

This file is a feature overview, not a lookup table. For details:

* **`docs/ai/reference.md`** – the overall reference for the
  support assistant: build gating, endpoints, authentication, `/control`,
  day/night, troubleshooting.
* **`docs/ai/config-keys.md`** – every configuration key individually, checked against
  `src/config.c`: default, value range, live or restart. Where
  this file and `config-keys.md` differ,
  `config-keys.md` applies; where either differs from the source, the source applies.
* **`docs/ai/troubleshooting.md`** – log dictionary, status and error codes,
  typical misconfigurations and a list of plausible-sounding but
  wrong answers.
