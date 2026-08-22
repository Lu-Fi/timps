# HTTP /control API Reference

`/control` and `/events` are compiled only into `USE_CONTROL` builds
(default on — see [Building](Building.md)). They live in `src/control.c`
(JSON parsing/serialization + apply logic) and `src/events.c`
(notification plumbing), wired into the HTTP server in `src/mp4/httpd.c`.
No JSON library is used — both request parsing and response building are
hand-rolled targeted scanning, consistent with the rest of the codebase's
minimal-dependency philosophy.

## Authentication and access control

`/control`, `/events`, and the HTTP media endpoints (`/stream.mp4`,
`/stream.mjpeg`, `/snapshot.jpg`, including their `?chn=N` forms) share
one access-control gate (`http_check_auth`/`http_check_token` in
`httpd.c`). A request is allowed if **any one** of the following holds;
otherwise it gets `401`/`403`:

1. **Loopback bypass** — the peer address is `127.0.0.0/8`. This is what
   lets an on-device WebUI always reach the streamer without a password;
   it replaces prudynt's separate "web UI auth key" mechanism.
2. **A valid token** — either the random **per-boot token**
   (`g_ctl_token`, generated at every `timpsd` start and published to
   `http.token_file`, default `/run/timps.token`, mode 0640, for local
   privileged readers like the thingino WebUI) or the optional persistent
   **`http.token`** config secret (for remote automation; this one is
   *never* written to the token file). Sent as an `X-Timps-Token: <token>`
   header (preferred) or `?token=<token>` query parameter — the query form
   exists because `<img>`/`<video src>`/`EventSource` cannot set custom
   headers, at the cost of the token potentially ending up in
   proxy/access logs (accepted as fine on a LAN). **The token never
   unlocks RTSP.**
3. **HTTP Basic or Digest credentials** — `http.user`/`http.pass` (falling
   back to `rtsp.user`/`rtsp.pass` if unset). The 401 challenge offers
   **Digest** first (RFC 7616 `qop="auth"`, plus legacy RFC 2069 no-qop
   support, with a tracked nonce ring — see
   [Streaming Protocols](Streaming-Protocols.md#authentication-http)) then
   **Basic**.

### Empty credentials (the shipped default) — media is open, control is not

When **no credentials are configured** (both `http.user` and `rtsp.user`
empty, the shipped default), gate #3 has nothing to check, so the generic
auth gate passes **every** request. This is deliberately **not** symmetric
across endpoints:

- **Media endpoints** (`/stream.mp4`, `/stream.mjpeg`, `/snapshot.jpg`,
  incl. `?chn=N`) — reachable by **anyone on the network, with no
  authentication**. The RTSP video/audio stream behaves the same way
  (RTSP auth is off while `rtsp.user` is empty). This is by design: an
  unconfigured camera streams on the LAN out of the box.
- **`/control` and `/events`** — carry an **extra loopback-only gate**:
  when no credentials are set, a **non-loopback** request is refused with
  `403` (`if (!c->local && !tok_ok && !user[0])` in `httpd.c`). So config
  and event state can never be read or changed from off-device unless you
  either configure credentials or present a valid token — even though the
  media is open.

To require authentication for the media too, set `rtsp.user`/`rtsp.pass`
and/or `http.user`/`http.pass`. See the SECURITY block in
`timps.conf.example` and
[Configuration Reference](Configuration-Reference.md).

CORS: the three media endpoints send `Access-Control-Allow-Origin: *`
unconditionally (safe because their auth never relies on ambient browser
credentials); `/control` and `/events` instead **reflect** the request's
`Origin:` header (with `Vary: Origin`, allow-listing the `X-Timps-Token`
header, no `Access-Control-Allow-Credentials`) so a WebUI served from a
different port can call `/control` directly. An `OPTIONS` preflight is
answered `204 No Content` before any auth check runs.

## `GET /control` — status + capabilities

Returns the entire current in-memory configuration and read-only status as
one JSON document. Because it's a config **snapshot**, this is also how a
client discovers, in one shot, which live-editable settings this specific
build/platform actually supports.

### Top-level shape

```json
{
  "version": "v1.8.5-51-g5b2105f",
  "caps": { "image": [...], "audio": [...], "osd": [...], "restart": [...],
            "rtsp_max_clients": 8, "http_max_clients": 8,
            "events_max_clients": 8,
            "motion": {...}, "privacy": {...}, "rotation": [...],
            "record": {...}, "backchannel": {...}, "play": {...},
            "timelapse": {...} },
  "image": { ... }, "audio": { ... }, "sensor": { ... },
  "video": { "0": { ... }, "1": { ... } },
  "osd": { "enabled": 1 }, "osd0": { "0": {...}, ... }, "osd1": { ... },
  "privacy": { "0": { "0": {...}, ... }, "1": { ... } },
  "daynight": { ... }, "motion": { ... }, "encoder": { "0": {...}, "1": {...} },
  "record": { ... }, "timelapse": { ... },
  "srt": { "available": 0 }, "tls": { "available": 0 }
}
```

### The `caps` object

`caps` exists so a WebUI can grey out controls this exact build/SoC
cannot actually apply, instead of hardcoding a feature matrix client-side:

| `caps.*` field | Meaning |
| --- | --- |
| `caps.image` | Array of `image.*` leaf keys the HAL wires **live** on this platform (from `src/isp_caps.h`'s per-SoC macros — e.g. `hue` only appears on T23/T31/T40/T41/C100). Unlisted `image.*` keys are still accepted/persisted, just have no live effect. |
| `caps.audio` | Array of `audio.*` leaf keys applied **live** (`volume`, `gain`, `mute` always; `alc_gain` only where `AUDIO_HAS_ALC_GAIN`; `spk_volume`/`spk_gain`/`aec` only when a speaker pipeline — `USE_PLAY` or `USE_BACKCHANNEL` — is compiled in). Deliberately excludes `high_pass`/`agc`/`agc_target_dbfs`/`agc_compression_db`/`ns` even though they're numeric-looking live candidates: libimp runs those on its own vendor record thread and frees state unlocked, so a live toggle would race that thread — they are restart-only by design. |
| `caps.osd` | The per-item OSD leaf keys `/control` accepts and applies live (`text x y font_size color transparency outline outline_color`). Per-item `enabled` is deliberately **not** in this list — see the [Configuration Reference](Configuration-Reference.md#osdsn--per-stream-osd-overlay-items) note on why enabling a boot-disabled item is restart-only. |
| `caps.restart` | Sections the WebUI should conservatively label restart-required: `["video", "sensor", "osd.enabled"]`. `caps.video_live` below is the per-key exception carved out of the `video` entry here — a listed `video_live` key applies live even though its section is named in `caps.restart`. |
| `caps.video_live` | Array of `video<N>.*` leaf keys (e.g. `bitrate`, `min_qp`, `max_qp`, `qp`, `i_bias_lvl`, plus the full classic-SoC set on T10–T30/T23 — see [Rate Control Parameters](Rate-Control-Parameters.md#live-vs-restart-per-soc)) this build's SoC can apply to a **running** encoder channel, from `src/enc_caps.h`. Empty on the host simulator and on channels with no live encoder. A listed key can still fall back to restart at runtime (channel not running, a classic H.265 stream, or the IMP call itself was rejected) — the POST reply's `deferred`/`deferred_keys` is the per-request truth; this array is only the platform's static ceiling. |
| `caps.rtsp_max_clients` | Concurrent RTSP sessions this build accepts before refusing further ones. Compile-time (`RTSP_MAX_CLIENTS`, default 8, `-D` overridable per board — low-RAM boards are built with `-DRTSP_MAX_CLIENTS=4`), so **two cameras reporting the same `version` can differ here**. |
| `caps.http_max_clients` | Same for concurrent HTTP connections (`HTTP_MAX_CLIENTS`, default 8, `-D` overridable). Past it the listener answers `503` with body `busy`. Note that `/stream.mp4`, `/stream.mjpeg` and each `/events` subscriber hold a connection for their whole lifetime, so a single WebUI tab can occupy several. |
| `caps.events_max_clients` | Concurrent `/events` (SSE) subscribers before `503 busy`. Unlike the two above this is a **config** key (`events.max_clients`, default 8), so it is per-camera, not per-build. |
| `caps.motion` | `{"available":0\|1, "max_cells":N}` — whether this build/SDK has the `IMP_IVS` move API, and the compile-time cell budget (`IMP_IVS_MOVE_MAX_ROI_CNT`, 52 on most SDKs, 4 on the old T10/T20 3.9.0 SDK). |
| `caps.privacy` | `{"available":0\|1, "max_regions":N}` — `available` reflects whether an OSD group actually exists on **any** stream (it only does if OSD or a privacy region was enabled at boot), not a hardcoded 1. |
| `caps.rotation` | (Only present in `USE_ROTATE` builds.) The ascending array of rotation values this SoC's build can actually apply, e.g. `[0]`, `[0,90,270]`, or `[0,90,180,270]` on T40/T41. See [Platform & SDK Support](Platform-SDK-Support.md). |
| `caps.record` / `caps.timelapse` | `{"available":0\|1}` per `USE_RECORD`/`USE_TIMELAPSE`. |
| `caps.backchannel` | `{"available":<bc_available()>}` — whether the backchannel was actually configured at boot (restart-only master switch — see [Audio](Audio.md)). |
| `caps.play` | `{"available":0\|1, "sounds":[...]}` — the play queue, with `sounds` live-enumerated from `/usr/share/sounds` (`.wav`/`.ulaw` always; `.opus` only when `USE_PLAY_OPUS` was actually compiled in, capped at 96 entries to bound the JSON response size). |

### Build-feature discovery — `*.available`

The fleet does not run one binary. The thingino firmware package and the
standalone `build.sh` binary are compiled with **different `USE_*` sets**
(the firmware package links mbedTLS; the standalone build typically does
not), and `version` — a `git describe` string — is identical across
them. So "does this camera speak HTTPS?" is not answerable from the
version, the config file, or anything else a client can read remotely.

Every optional feature therefore reports an `available` flag, and a
client should branch on **that**, never on a version comparison:

| Where | Feature | Emitted when off |
| --- | --- | --- |
| `caps.record` / `caps.timelapse` / `caps.play` / `caps.backchannel` / `caps.motion` | `USE_RECORD` / `USE_TIMELAPSE` / `USE_PLAY` / `USE_BACKCHANNEL` / IVS move API | `{"available":0}` |
| `caps.rotation` | `USE_ROTATE` | key absent entirely |
| `srt` (top level) | `USE_SRT` | `{"available":0}` |
| `tls` (top level) | `USE_TLS` | `{"available":0}` |

`srt` and `tls` sit at the top level rather than under `caps` because,
when the feature *is* compiled in, they also carry the runtime settings
needed to dial it:

```json
"srt": {"available":1,"enabled":1,"port":9000,"channel":0}
"tls": {"available":1,"https":1,"rtsps":1,"rtsps_port":322}
```

When the build lacks the feature, **only** `{"available":0}` is emitted.
That is deliberate for `tls` in particular: `http.https`/`rtsp.tls` may
well be `1` in `timps.conf` on a non-TLS build — that mismatch is exactly
what logs `RTSPS requested but built without USE_TLS` at startup, where
no HTTP client ever sees it — but no listener was opened, so echoing the
requested flags would invite a client to dial a port nothing is bound to.
`"available":0` means *ignore any TLS configuration you may have seen
elsewhere; this binary cannot serve it.*

### The `encoder` object — read-only encoder telemetry

`GET /control` also carries a top-level `"encoder"` object with one entry
per video channel that currently has a live encoder: `{"0":{...},
"1":{...}}`. This is a read-only diagnostics addition — there is no
matching `/control` POST surface, and no new config keys. Each entry
comes straight from `IMP_Encoder_Query` (available on all 9 platforms):

```json
"encoder": {
  "0": {"registered":1,"left_pics":0,"left_stream_bytes":0,
        "left_stream_frames":0,"cur_packs":1,"work_done":1,
        "ave_bitrate":3012.4}
}
```

| Field | Meaning |
| --- | --- |
| `registered` | Whether the channel is registered to its encode group. |
| `left_pics` | Images still queued to encode. |
| `left_stream_bytes` / `left_stream_frames` | Bytes/frames still sitting in the stream buffer, unread. |
| `cur_packs` | Stream packets making up the current frame. |
| `work_done` | `0` = still running, `1` = not running. |
| `ave_bitrate` | **T31 only**, and only once at least one frame has flowed: the running average bitrate from `IMP_Encoder_GetChnAveBitrate` (a T31-exclusive call that needs the just-fetched stream buffer, so it's computed and cached by the encode thread itself rather than queried directly from the `/control` handler, which would otherwise steal packets from the streaming loop). |

A channel whose query fails — a disabled stream, the T23 SW-rotate path
(which has no bound encoder channel/group at all), or the host
simulation backend — is **omitted** from the object entirely rather than
reported with misleading zeros.

### The `encoder.<n>.rc` object — what the encoder actually holds

Each `encoder.<n>` entry can carry an additional `"rc"` sub-object,
read live via `IMP_Encoder_GetChnAttrRcMode` — a *second* source of truth
next to the `video<N>.*` block, which only reports what was written. The
two exist so a written value can be diffed against what the encoder held
after the fact; see
[Rate Control Parameters](Rate-Control-Parameters.md#what-was-unexplained-and-how-it-was-settled) for why
that gap has mattered in practice (two SDK-header-derived assumptions
about T23 rate control turned out wrong until this readback existed).

```json
"encoder": {
  "0": {"registered":1, "left_pics":0, "...": "...",
        "rc": {"rc_mode":"vbr", "bitrate":2000, "max_bitrate":2000,
               "min_qp":20, "max_qp":45, "quality_lvl":7,
               "change_pos":80, "i_bias_lvl":0}}
}
```

Fields reuse the `video<N>.*` names where they mean the same thing.
Only fields the current mode/API actually carries are present — a
`cbr` channel never shows `quality_lvl`, a classic-SoC channel never
shows `ip_delta`/`pb_delta`/`rc_options`/`max_picture_size`/`max_psnr`.
On the new-generation API (T31/C100/T40/T41) those last five are the
attributes timps itself never writes (left at the vendor SDK default);
this readback is the first place their values are visible at all — as
raw SDK numbers, units unverified. If `hal_enc_rc_read()` fails (no
live channel, unqueryable state), the whole `rc` key is omitted, same
as the parent `encoder.<n>` entry's own omission rule above.


### Request/response examples

Get full status:

```sh
curl http://127.0.0.1:8880/control
```

Change a **live** setting (image brightness) and read it back:

```sh
curl -X POST http://127.0.0.1:8880/control -d '{"image":{"brightness":140}}'
curl http://127.0.0.1:8880/control | jq .image.brightness
# -> 140 (applied immediately via IMP_ISP_Tuning_SetBrightness; persisted to timps.conf)
```

Change a **restart-only** setting (encoder bitrate) — it persists and is
echoed back, but the running encoder keeps its current bitrate until the
next restart:

```sh
curl -X POST http://127.0.0.1:8880/control -d '{"video":{"0":{"bitrate":3500}}}'
curl http://127.0.0.1:8880/control | jq .video."0".bitrate
# -> 3500 (in the config; the live stream is unaffected until restart)
```

Using a token instead of Basic auth (from a browser context that can't
send `Authorization`, e.g. `<img>`):

```sh
curl "http://127.0.0.1:8880/snapshot.jpg?token=$(cat /run/timps.token)" -o snap.jpg
```

## `POST /control` — apply settings

Takes a nested JSON body; every recognized setting is:

1. flattened to its config-file key (`image.brightness`,
   `osd0.0.text`, `video0.bitrate`, ...),
2. applied to the in-memory config (`config_apply_kv`),
3. **change-detected** (before/after comparison; a no-op re-POST is
   skipped so a client that re-sends the same value every few seconds
   can't hammer the ISP or rewrite flash — with one deliberate exception:
   `image.running_mode` always re-drives the ISP even when unchanged,
   because it's a hardware-sync command whose actual latched state can
   drift from the config model — see [Day/Night](Day-Night.md)),
4. applied live via `hub_control()` → the HAL (when a live-apply path
   exists for that key),
5. pushed to any other open `/events` subscribers as a `config` event,
6. and finally, all changed keys from the whole request are written back
   to the config file in **one** batched, atomic `config_write_keys()`
   call.

### JSON shape

Nested per-section objects, matching the config-file section prefixes:

```json
{
  "image": {"brightness":140,"contrast":128,"hue":128,"hflip":0,"running_mode":1},
  "audio": {"volume":90,"gain":30,"mute":false,
            "codec":"aac","samplerate":16000,"channels":1,"bitrate":32},
  "speaker": {"play":"chime_1.wav"},
  "osd":   {"enabled":1},
  "osd0":  {"0":{"enabled":1,"text":"%Y-%m-%d %H:%M:%S","x":10,"y":10,
                 "font_size":32,"color":"0xFFFFFFFF",
                 "outline":1,"outline_color":"0xFF000000"},
            "3":{"enabled":0}},
  "osd1":  {"0":{"text":"sub cam"}},
  "video": {"0":{"bitrate":3500},"1":{"bitrate":600}},
  "privacy": {"0":{"0":{"enabled":1,"x":0,"y":0,"w":200,"h":100,"color":"0xFF000000"}}},
  "sensor": {"model":"gc2053","i2c_addr":55,"fps":25,"width":1920,"height":1080},
  "daynight": {"mode":"sun","sun_latitude":52.52,"sun_longitude":13.40},
  "motion": {"enabled":1,"sensitivity":128,"cols":5,"rows":5},
  "record": {"active":1},
  "timelapse": {"interval_s":120}
}
```

Every field is optional; unknown keys are ignored; the legacy flat form
(`{"brightness":140,"running_mode":1}` or `{"force_mode":"night"|"day"}`)
still works and maps onto `image.*`.

### Section-by-section behavior

See [Configuration Reference](Configuration-Reference.md) for the
authoritative per-key live/restart table; this is the request-shape
summary:

| JSON section | Maps to | Live-apply behavior |
| --- | --- | --- |
| `image` | `image.*` | Every key accepted and live-applied where the SoC supports it (`caps.image`). |
| `audio` | `audio.*` | `volume`/`gain`/`alc_gain`/`mute`/`spk_volume`/`spk_gain` live; the rest (codec/samplerate/channels/bitrate/high_pass/agc/ns/force_stereo/spk_enabled/backchannel*) persist-only. |
| `speaker` | not persisted | `{"play":"<file>"}` enqueues a system sound on the play FIFO (validated against `/usr/share/sounds`, no `/` or `..`); `{"stop":1}` stops it. Transient action, `USE_PLAY` only — see [Audio](Audio.md). |
| `daynight` | `daynight.*` + `{"probe":1}` | `enabled`/`mode`/`time_night_start`/`time_day_start`/the threshold, probe, heartbeat, boot and sun-offset numerics plus `interval_ms`/`diagnose_thresholds` are all live (the detection thread polls `g_cfg` directly rather than being pushed through a HAL call); `mode` is validated against `auto`/`schedule` (legacy `sensor`/`time`/`sun` still accepted) before being applied. Eight further numerics — `probe_jump_pct`, `probe_settle_s`, `ref_delay_s`, `ir_ratio_night`, `ir_ratio_day`, `ir_min_headroom`, `boot_settle_s`, `transition_s` — are fixed internal constants as of the 2026-08-22 config consolidation: still readable in the status object for diagnostics, but no longer POST-able. `switch_cmd`/`isp_path`/`trace_path`/`irprobe_cmd` are deliberately **not** POST-able (exec'd command / paths the daemon writes as root, config-file only). `probe` is a command, like `record.clip`: it arms one silent IR probe for the next tick and is rejected (not silently ignored) on a camera with no `daynight.irprobe_cmd` configured or whose silent probe has retired itself for the session — see [Day/Night](Day-Night.md#the-silent-probe). |
| `osd` (legacy shared form) | `osd.enabled`/`monitor_stream`/`font_path`/`vars_file`/`supersample`/`hinting` + `osdN.*` mirrored onto every stream | These osd.* globals are looked for only in the JSON span *before* the first nested item object, so an item's own keys (e.g. an item's `enabled`) are never mistaken for them. All five are config-only (restart-required), same as `osd.enabled`. |
| `osd0`/`osd1` (canonical per-stream form) | `osd<S>.<N>.*` | Applied live via `imp_osd_apply()` for items that already had a region at startup, **except** `type` (text vs. logo): the live re-render dispatch is fixed at region-creation time, so changing an existing item's type persists but needs a restart to actually change what's drawn. `logo`/`logo_w`/`logo_h`/`font_path` (per-item override) are persist-only and not GET-readable. |
| `video` | `video<N>.*` | Persist-only by default (the encoder/FrameSource is never reconfigured live) **except** `rtsp_path` (always live) and the `caps.video_live` subset (2026-08-21 — bitrate/QP/rc knobs, per SoC; see [Rate Control Parameters](Rate-Control-Parameters.md#live-vs-restart-per-soc)), which reaches the running encoder channel through the calls listed there. Geometry/codec/identity keys (`width`/`height`/`fps`/`codec`/`profile`/`buffers`) and anything `caps.video_live` doesn't list stay restart-bound. |
| `privacy` | `privacy<S>.<N>.*` | Live (create/show/hide/move) as long as an OSD group exists on that stream. |
| `sensor` | `sensor.*` | Persist-only; applied at the next ISP init. |
| `motion` | `motion.enabled/sensitivity/cols/rows/monitor_stream` | All live — the HAL stops and recreates the whole IVS grid on any of these (a single request's several motion keys are batched into **one** rebuild via `hub_control_commit()`, not one rebuild per key). `hold_ms`/`skip_frames` are also POST-able (persist + echo) but only feed the grid/hold logic at the next such rebuild or a restart, not immediately. `cooldown_ms`/`on_motion` are deliberately **not** POST-able (config-file only). |
| `record` | `record.*` + `{"active":1\|0}` + `{"clip":"...","seconds":N}` | Config keys apply on the recorder's next loop pass (no restart); `active` is an immediate manual start/stop override; `clip`/`seconds` triggers an independent one-shot on-demand fMP4 capture, not persisted. |
| `timelapse` | `timelapse.*` | Applied on the timelapse thread's next loop pass, no restart. |

### Response body and status codes

Every `POST /control` answers `application/json` with the same body
shape, whatever the status:

```json
{"ok":true,"accepted":2,"changed":1,"rejected":0,"not_persisted":0,
 "deferred":0,"deferred_keys":[],
 "applied":{"image.brightness":"255"}}
```

| Field | Meaning |
| --- | --- |
| `ok` | `true` only when at least one known field was applied. |
| `accepted` | Known fields applied, **including** no-op rewrites of the value a field already held — re-posting the current value is a success, not a silent failure. Clamped writes count here too: clamping is the documented contract, not an error. Also counts *commands* that were carried out (`record.clip`, `daynight.probe`), which never go through the settings path at all. |
| `changed` | The subset that actually differed and was persisted. |
| `rejected` | Known fields whose **value** was refused (`null`, `undefined`, or an empty string on a non-string field), plus commands that were understood and failed (`record.clip` to an unwritable path). |
| `not_persisted` | Of `accepted`, the number that were applied live but **not** written to `/etc/timps.conf` because the request changed more keys than the 48-slot persist list holds. Those values are live now and gone after the next reboot; a caller changing many keys at once should split the request or re-`GET` to confirm what survived. |
| `deferred` / `deferred_keys` | (2026-08-21) Of `changed`, the `video<N>.*`/`sensor.*` keys that were persisted but did **not** reach the running pipeline this request — `deferred` is always the exact count, `deferred_keys` lists them (subject to `deferred_truncated`, same overflow contract as `applied`/`truncated`). A key absent from `deferred_keys` after a successful `changed` count on a `video`/`sensor` field DID apply live — see `caps.video_live` and [Rate Control Parameters](Rate-Control-Parameters.md#live-vs-restart-per-soc). Every other section's keys are graded by the [section-by-section table](#section-by-section-behavior) above instead; `deferred` never lists them. |
| `applied` | Per-key echo of the **effective** value wherever it differs from what was posted — i.e. after clamping. This is how a caller that posted `999` learns it got `255`, without re-`GET`ting the document. |
| `truncated` | Present (`true`) only if more keys changed than the 512-byte echo holds; fall back to a `GET`. |
| `reason` | Present only on the error answers below — the machine-readable discriminator, so a client never has to infer the case from the status line. |

| Status | `reason` | Meaning | What the client should do |
| --- | --- | --- | --- |
| `200 OK` | — | At least one known field was applied (or one command carried out). A partial request — some fields applied, others rejected — is a `200`; check `rejected`. | Nothing. Read `applied` for clamped values. |
| `400 Bad Request` | `not_json` | The body was not a JSON object at all (garbage, empty, truncated before the first `{`). | Fix the caller — this is a client bug. |
| `422 Unprocessable Content` | `unknown_fields` | It parsed, but carried **no field this build knows**: a typo, the wrong section, or a key gated out of this binary. Nothing was applied. | Check spelling — and check the `*.available` flags above, because the key may simply not exist in *this* build. Retrying the identical body will never succeed. |
| `409 Conflict` | `values_rejected` | It parsed and every field in it **was** known, but every one of them was refused: bad values, or a command that failed. Nothing was applied. | The key names were right; re-send with valid **values**. |
| `413 Payload Too Large` | — | `Content-Length` negative, or larger than the request buffer. | Split the request. |
| `503 Service Unavailable` | `oom` | The daemon could not allocate to service the request. | Retry later; not a client error. |

`422` and `409` were **one code until now**, and they are opposite
instructions: `422` says *your key names are wrong for this binary*,
`409` says *your key names were right and your values were not*. A client
that retried the first unchanged would loop forever; a client that went
hunting for a missing build feature on the second would be chasing
nothing.

**Compatibility note.** `422` deliberately kept the *unknown-field*
meaning rather than the (semantically tidier) value-rejection one,
because that is what the installed base already asserts: thingino's
`timps-selftest.sh` probes an unknown key and fails the camera on
anything but `422`, and the WebUI's `timps-api.js` prints its "no setting
in this request is known to this timps build" message on a `422` with
`rejected == 0`. Moving *that* case would have turned every fielded
selftest red. The value-rejection case moved instead; the only casualty
is the `rejected > 0` branch of that same WebUI message, which degrades
to a generic "HTTP 409" line until the WebUI is updated. Clients keying
off `res.ok` or on `2xx` are unaffected — both cases were, and remain,
non-`2xx`.

## `GET /events` — Server-Sent Events push stream

An alternative to polling `GET /control`: a long-lived
`text/event-stream` connection that pushes JSON the moment relevant state
changes. Same access-control rules as `/control` (loopback/token/Basic or
Digest), same CORS handling. `events.enabled=0` makes the endpoint answer
`404`; `events.max_clients` (default 8) caps concurrent subscribers below
the general HTTP client limit — beyond it the endpoint answers `503`
with body `busy` (a `HEAD` request does not count against this limit and
never enters the streaming loop).

```sh
curl -N http://127.0.0.1:8880/events                                    # everything
curl -N "http://127.0.0.1:8880/events?stream=motion,stats&token=$(cat /run/timps.token)"
```

`?stream=motion,daynight,stats,config` selects a subset of event types
(default: all four). Browsers use the query-string token form because
`EventSource` cannot set custom headers.

### Wire format

On connect: `retry: 3000` (tells `EventSource` to reconnect after 3s if
dropped), then a `: connected` comment line. Every event frame is
`event: <type>\ndata: <json>\n\n`, capped at 1280 bytes — an oversized
payload is **dropped entirely** (never truncated, so as not to poison the
stream framing for the client's parser) and logged as a warning. A
`: ping` comment line is sent roughly every 12 seconds of otherwise-quiet
connection, both to detect a dead client (a failed write ends the
connection) and to keep intermediate proxies from timing it out.

Each connection deduplicates independently against what *it* last sent —
producers (the IVS result thread, the day/night sampler, `/control`
writes) wake subscribers through a shared condition variable, so push
latency is just the producer's own sampling rate, never HTTP polling.

### Event types

| `event:` | Pushed when | `data:` payload | Delivery semantics |
| --- | --- | --- | --- |
| `motion` | A grid transition occurred, or enabled/geometry/sensitivity changed | Identical shape to `/control`'s `"motion"` object (grid + `active[]` + `last_ms`) | **Lossless, queue-driven**: every real transition is captured in a bounded 32-entry snapshot ring with a per-connection cursor, so two transitions between two samples are never collapsed into one (which plain level-sampling would do, since IVS clears `retRoi` on the very next processed frame). A cursor that falls too far behind is jumped forward to the oldest retained snapshot rather than blocking the producer. |
| `daynight` | Mode flipped, or brightness moved ≥1%, or gain moved ≥5% relative (or ≥8 absolute near zero) | Identical shape to `/control`'s `"daynight"` object | Level-sampled with a per-connection dedup threshold matching the producer's own event-worthy-change filter in `daynight.c`, so brightness/gain jitter every sample doesn't spam the stream. |
| `stats` | Every `events.stats_ms` (default 2000ms; `0` disables) | `{"uptime_s":N,"clients":N,"video":[{"chn":0,"subs":N,"fps":F,"kbps":F,"width":N,"height":N,"codec":"h264","drop_frames":N,"drop_bytes":N},...]}` | Periodic tick. `video[]` only lists streams enabled **at boot** (`g_cfg_boot`), so the reported geometry/codec always matches what the fps/kbps numbers were actually measured on. |
| `config` | Another client's `/control` POST changed a setting | `{"key":"<key>","value":"<value>"}`, or `{"resync":true}` once if this connection fell behind a bounded coalescing table and may have missed an update | A small fixed 24-slot table (sized so one bulk image-tuning POST fits in a single push) coalesces rapid repeated changes to the same key into one entry; a genuinely new key evicts the globally-oldest slot when full and flags lapped subscribers to re-`GET /control` instead of silently missing the update. |

The thingino WebUI's preview overlay subscribes to `?stream=motion` and
falls back to 4Hz `/control` polling if `/events` is unavailable.
