# timps configuration keys — complete reference

**Applies to timps v1.9.20 (source: `main`, 2026-09-24).**

A statement marked **since v1.9.21 (unreleased)** is already in the source but
not in any tagged release yet — on a v1.9.20 camera the *previous* behaviour is
the one to describe.

Authoritative source: `src/config.c` (the `cfg_field` tables and
`config_defaults()`), `src/config.h` (struct field sizes and doctrine),
`src/control.c` (what is reachable over HTTP), `src/hal/hal_ingenic.c`
(`ing_control()` — what applies live), and `src/*_caps.h` (platform gating).
Where an existing doc contradicts the code, the code wins here and the
disagreement is listed in `docs/ai/_doc-drift.md`.

Overview / architecture doc: `docs/ai/reference.md`.

---

## 1. How to read this file

### 1.1 Config file format

`/etc/timps.conf` is a flat `key = value` file (`config_load()` in
`src/config.c`). Rules, all verified in code:

* One key per line. Leading/trailing whitespace is trimmed.
* A line whose first non-blank character is `#` or `;` is a comment.
* A line without `=` is silently skipped.
* An **inline** `# comment` is cut from the value when the `#` is at the start
  of the value or preceded by a space/tab **and** the value is not quoted.
* A value may be quoted with `"` or `'`. One leading and one matching trailing
  quote are stripped; the closing quote is searched from the **right**, and a
  quote only counts as closing if nothing but blanks or an inline `# comment`
  follows it. An opening quote with no closing one logs a warning and the value
  is kept verbatim, quotes included.
* Physical lines longer than 510 characters are dropped entirely with a warning
  (they are not truncated into a bogus second key).
* Unknown keys log `unknown key <name>` at WARN and are ignored.
* **Later lines win.** A duplicate key later in the file overrides the earlier
  one.
* Values are parsed with `strtol(v, NULL, 0)` for integers, so `0x37` (hex) and
  `055` (octal) are accepted wherever an integer is.
* Booleans: `1`, `true`, `on`, `yes` are true (case-insensitive). Anything else
  is false — including typos. Two keys warn loudly about this (see `F_SECVAL`
  below).

The file path is `/etc/timps.conf` unless `timpsd -c <path>` says otherwise
(`src/main.c`).

### 1.2 Column meanings

| Column | Meaning |
| --- | --- |
| **Key** | Exact config-file spelling. Indexed forms use `<N>`/`<S>` placeholders documented per section. |
| **Type** | `bool`, `int`, `float`, `string[n]` (n = buffer size incl. NUL), or an enum with its accepted tokens. |
| **Default** | The value `config_defaults()` sets before the file is read. |
| **Range** | The clamp applied at parse time (`lo`/`hi` in the `cfg_field` table). "unclamped" means the table entry has `lo == hi == 0`, so the raw parsed value is stored. |
| **Apply** | See 1.3. |
| **Notes** | Platform gating, interactions, pitfalls. |

Out-of-range values are **clamped, not rejected**. The clamped value is what is
stored, read back by `GET /control`, echoed in a POST reply and persisted — so a
POST of `999` to a `0..255` field succeeds and the camera reports `255`.

### 1.3 Apply modes

| Mode | Meaning |
| --- | --- |
| **live** | A `POST /control` changes the running pipeline immediately (`ing_control()` returns 1). |
| **restart** | The key is POST-able and persists, but the running daemon keeps its old behaviour until `/etc/init.d/S95timps restart`. |
| **file-only** | The `cfg_field` entry has no `F_CTRL` flag (or the whole section is not wired into `control_apply_json`). The key can **only** be changed by editing `/etc/timps.conf` and restarting. A POST carrying it is silently skipped (it shows up in the reply's `ignored` array). |
| **capability-gated** | The key parses and persists on every build/SoC, but the HAL only issues the corresponding IMP call where the `*_caps.h` matrix says the SDK has it. Elsewhere it is stored, echoed, and does nothing. |

`F_CTRL` is a per-field **security allowlist**, not a default. Unreachable over
HTTP, verified against `control_apply_json()` (`src/control.c`) and the
`cfg_field` tables:

* **Whole sections `control_apply_json()` never walks:** `http.*`, `rtsp.*`,
  `jpeg.*`, `events.*`, `webrtc.*`, `srt.*`, `sim.*`. The POST surface only
  knows `image`, `audio`, `general`, `daynight`, `osd`/`osd<S>`, `video`,
  `privacy`, `sensor`, `motion`, `record`, `timelapse`.
* **Individual keys inside a walked section that carry no `F_CTRL`:**
  `general.loglevel`, `general.imp_polling_timeout`, `general.osd_pool_size`
  (only `general.debug_modules` is POST-able);
  `video<N>.imp_chn`/`jpeg`/`jpeg_quality`/`jpeg_fps`/`jpeg_chn`;
  `osd<S>.<N>.logo`/`logo_w`/`logo_h`/`font_path`;
  `motion.on_motion`, `motion.cooldown_ms`;
  `daynight.switch_cmd`, `daynight.isp_path`, `daynight.irprobe_cmd`,
  `daynight.trace_path`.
* `general.syslog`, `general.trace` and `general.trace_ms` have no table entry
  at all (side effects in `set_kv()`), so they are file-only too.
* `daynight.mode` is the one exception in the other direction: no `F_CTRL`, but
  hand-validated in `control.c`, so a POST *does* reach it (and an unknown
  token is rejected rather than coerced).

A key in the second group **does** show up in the POST reply's `ignored` array
— `ign_note()` reports any member of a walked object that lacks `F_CTRL`. So
`{"motion":{"on_motion":"/x"}}` answers `200` with
`"ignored":["motion.on_motion"]`.

`F_NOGET` marks fields `GET /control` never reads back. `F_SECVAL` (only
`http.https` and `rtsp.tls`) makes an unparseable value log a loud WARN, because
for those two keys a silent fall-through to `0` means *plaintext*.

### 1.4 How to change a key

**By file (works for every key):**

```sh
vi /etc/timps.conf
/etc/init.d/S95timps restart
```

**By HTTP (only `F_CTRL` keys):** `POST /control` on the HTTP port
(`http.port`, default 8880) with a nested JSON body. The section names and the
nesting are defined in `src/control.h`:

```json
{ "image":  {"brightness":140,"running_mode":1},
  "audio":  {"volume":90,"mute":0},
  "video":  {"0":{"bitrate":3500},"1":{"bitrate":600}},
  "sensor": {"fps":25},
  "osd":    {"enabled":1},
  "osd0":   {"0":{"text":"%Y-%m-%d %H:%M:%S","x":10}},
  "osd1":   {"0":{"font_size":12}},
  "privacy": {"0":{"0":{"enabled":1,"x":100,"y":80,"w":320,"h":180}}},
  "motion": {"enabled":1,"sensitivity":160},
  "record": {"enabled":1,"mode":"motion"},
  "timelapse": {"interval_s":60},
  "daynight": {"day_gain":900},
  "general":{"debug_modules":"DAYNIGHT"} }
```

Notes on the POST surface:

* The JSON section name maps to the config prefix, but **not uniformly** —
  note that OSD items and privacy masks nest differently:
  * `{"image":{…}}` → `image.*`
  * `{"video":{"<N>":{…}}}` → `video<N>.*`
  * `{"osd<S>":{"<N>":{…}}}` → `osd<S>.<N>.*` — the **stream index is part of
    the section name** (`"osd0"`, `"osd1"`).
  * `{"privacy":{"<S>":{"<N>":{…}}}}` → `privacy<S>.<N>.*` — the section is the
    bare word `privacy` and **both** indices are nested inside it. There is no
    `"privacy0"` section: `control_apply_json()` looks up the literal name
    `privacy` and then walks stream then region. A body using `"privacy0"` is
    an unknown top-level section — it is **not** applied and, because only
    known sections are scanned, it is **not** listed under `ignored` either.
    It answers `422 unknown_fields` if it was the only thing in the body.
  * `{"osd":{"0":{...}}}` is the **legacy** shared form and writes the item
    onto **every** stream.
* A legacy flat top-level form still works for image keys only
  (`{"brightness":140}`) plus `{"force_mode":"night"|"day"}` →
  `image.running_mode`.
* **POSTs persist.** `control_apply_json()` collects every *changed* key and
  calls `config_write_keys(g_cfg_path, ...)` at the end of the request. Values
  that did not change are not rewritten (deliberate flash-wear avoidance).
* At most **48** changed keys persist per request (`CTRL_MAX_CHG`). Beyond that
  the value is live but unsaved and the reply's `not_persisted` counter says so.
* The reply reports `accepted`, `changed`, `rejected`, `not_persisted`,
  `deferred`/`deferred_keys` (changed keys that did **not** reach the running
  pipeline: `video*`/`sensor.*` graded per request, plus — **since v1.9.20** — every restart-only `audio.*`/`osd.*` global; on v1.9.19 only
  `video*`/`sensor.*` were ever listed), `applied` (effective, post-clamp values) and
  `ignored` (field names inside a known section that this build did not apply).
* `null`, `undefined` and (for non-string fields) `""` are rejected. An empty
  string on a **string** field means "clear this".
* Non-config commands that ride the same endpoint and are **not** persisted:
  `{"record":{"active":1|0}}`, `{"record":{"clip":"/path.mp4","seconds":6}}`,
  `{"daynight":{"probe":1}}`, `{"speaker":{"play":"x.wav"}}` /
  `{"speaker":{"stop":1}}`.
* `GET /control` dumps the current values plus a `caps` object;
  `GET /control?fields=1` dumps the authoritative inventory of every `F_CTRL`
  field grouped by section — that endpoint is generated from the same tables, so
  it cannot drift.

### 1.5 Does timpsd rewrite /etc/timps.conf?

Yes, on every `POST /control` that changed at least one key — but it is a
**merge, not a regeneration** (`config_write_keys()` in `src/config.c`):

* Existing `key = ...` lines are replaced **in place**; file order is preserved.
* Whole-line comments, blank lines and lines timpsd does not recognise are
  copied through untouched.
* Keys not already present are appended at the end.
* Later duplicate lines of a replaced key are dropped.
* A line spelled with a legacy **alias** is matched by canonical name and
  replaced in place (so the file does not accumulate a stale alias line next to
  the new canonical one).
* **An inline `# comment` on a rewritten key's line is lost**, because the whole
  line is regenerated as `key = value`. Comments on their own lines survive.
* Values are re-quoted when needed (empty, or containing space, `#`, `"`, `'`,
  `;`); control characters are folded to spaces.
* The write is atomic (`mkstemp` + `fsync` + `rename` + directory `fsync`),
  mode 0644. A read error on the old file aborts the rewrite entirely rather
  than committing a truncated copy.

### 1.6 Platform names

`PLATFORM` values the `Makefile` has a branch for: **T10, T20, T21, T23,
T30, T31, T40, T41, C100**. There is **no validation**: the `IMP_INC` selection
ends in an `else` branch that falls back to the **T31** headers, so an unknown
or misspelled `PLATFORM` builds silently against T31 headers while
`-DPLATFORM_<X>` matches no `*_caps.h` condition. `T32`, `T33` and `A1` are **not** selectable
PLATFORM values in this tree — a `T32`/`T33` mention exists only inside
`isp_caps.h`'s `ISP_HAS_SENSOR_ATTR` condition and is unreachable; `A1` does not
appear at all. A build with no `PLATFORM_*` macro (the x86 host sim) enables
every capability so the WebUI can be exercised.

---

## 2. `general.*`

Section is readable via `GET /control`; only `debug_modules` is POST-able.

| Key | Type | Default | Range | Apply | Notes |
| --- | --- | --- | --- | --- | --- |
| `general.loglevel` | int | `2` | unclamped | file-only, restart | `0`=error `1`=warn `2`=info `3`=debug. `config_apply_kv()` calls `log_set_level()` on it, but it has no `F_CTRL`, so only the file path reaches it. |
| `general.debug_modules` | string[64] | `""` | — | **live** | Comma-separated module names forced to DEBUG regardless of `loglevel`, e.g. `HAL_ING,DAYNIGHT`. Live on purpose: restarting to enable debugging destroys the state you are trying to observe. |
| `general.imp_polling_timeout` | int | `500` | 10..10000 | file-only, restart | ms, `IMP_*_PollingStream` timeout. `0` would make the video thread spin and the watchdog `SIGTERM` the daemon seconds after the first client — hence the floor of 10. |
| `general.osd_pool_size` | int | `1024` | 1..1024 | file-only, restart | KB of IMP OSD pool. 1024 is the T-series maximum; larger is rejected by libimp anyway. |
| `general.syslog` | bool | `1` | — | file-only, restart | **Not a stored field** — handled as a side effect in `set_kv()` (`log_set_syslog()`). Never appears in `GET /control` and cannot be POSTed. `0` = stdout only, no `logread`. |
| `general.trace` | int (bitmask) | `0` | unclamped | file-only, restart | Developer send-pipeline tracing. Bits: `1` per-AU gap/age/send, `2` per-write timing, `4` fanqueue backlog, `8` periodic summary, `15` = all. **Only does anything in a `USE_TRACE=1` build**; parsed and ignored otherwise. Side-effecting key like `general.syslog`: no table entry, no `F_CTRL`, not in `?fields=1`. |
| `general.trace_ms` | int | `250` | unclamped | file-only, restart | Report threshold in ms for `general.trace`. Takes effect in file order, so list it **before** `general.trace` if you want the startup banner to print the right threshold (cosmetic only). |

Pitfalls
* `general.loglevel = 3` on a flash-backed syslog camera grows the log fast; use
  `general.debug_modules` for one subsystem instead.
* `general.trace` on a normal firmware build is accepted, persisted and inert —
  there is no warning.

---

## 3. `sensor.*`

All five keys are `F_CTRL` (POST-able) but **restart** — they are consumed at
the next ISP init only. `ing_control()` logs `persisted, applies on restart` and
the POST reply lists them under `deferred`.

| Key | Type | Default | Range | Apply | Notes |
| --- | --- | --- | --- | --- | --- |
| `sensor.model` | string[64] | *(unset)* → autodetect | — | restart | Sensor driver name, e.g. `gc2053`. |
| `sensor.i2c_addr` | int | *(unset)* → autodetect | 0..0x7F | restart | Alias `sensor.i2c_address`. Hex (`0x37`) accepted. |
| `sensor.fps` | int | *(unset)* → autodetect, **capped** | 0..120 | restart | `0` = auto. **since v1.9.19:** the autodetected value is capped at **30** (`SENSOR_AUTO_FPS_CAP` in `src/config.c`); **since v1.9.20** the cap is raised to the fastest *enabled* `videoN.fps` when that is higher, so a stream configured above 30 still gets its frames. An explicit value is used as given. Before v1.9.19 the driver's `max_fps` was taken verbatim. |
| `sensor.width` | int | *(unset)* → autodetect | 0..8192 | restart | `0` = auto. |
| `sensor.height` | int | *(unset)* → autodetect | 0..8192 | restart | `0` = auto. |

`config_sensor_finalize()` runs once after load and before the HAL starts:

1. **`model` and `i2c_addr` are overridden by the kernel sensor registry** when
   it is readable (`/proc/jz/sensor/name` flat layout, or
   `/proc/jz/sensor/sensor0/name` nested). A config value that disagrees logs a
   warning and is discarded — deliberately, because telling `IMP_ISP_AddSensor`
   the wrong sensor makes the kernel module work from a zeroed attribute table
   and divide by zero (SIGFPE in the kernel).
2. `width`/`height`/`fps` are **config-first**: only a `0`/unset value is filled
   from the registry (`width`, `height`, `max_fps`, then `fps`).
   **since v1.9.19:** a registry `fps` above the cap is capped and logged
   (`sensor.fps: driver max_fps=%ld, auto capped to 30 (set sensor.fps to
   override)`). The cap is **30**, or — **since v1.9.20** — the
   fastest enabled `videoN.fps` if that is higher, and the log names the
   registry key the value came from (`driver max_fps=…` or `driver fps=…`).
   Some drivers advertise a rate their clock cannot deliver — the GC2053
   reports `max_fps = 40` on a 30 fps mode. The cap applies to the
   *autodetected* value only; a configured `sensor.fps = 50` still goes to the
   ISP unchanged.
3. Remaining unset values get fallbacks: `model=gc2053`, `i2c_addr=0x37`,
   `width`/`height`/`fps` from `video0.*`, final safety net `1920x1080 @25`.

Pitfalls
* `/proc/jz/sensor` is reported not to exist on T40/T41 (and it never does on
  the host sim), so autodetect silently does nothing there and only config +
  fallback apply. **(unverified — kernel side; `config_sensor_finalize()` has
  no `PLATFORM` gate and probes the path on every build.)**
* **`sensor.fps` vs `videoN.fps`:** `sensor.fps` is the sensor/ISP frame rate;
  `videoN.fps` is the per-encoder-channel rate. Setting `videoN.fps` above
  `sensor.fps` cannot create frames — you get the sensor rate. Setting it below
  makes the HAL drop/re-time to that channel's rate. `videoN.fps` never reaches
  the sensor driver at all, so raising it is not a way to raise the capture
  rate.
* **The sensor driver can refuse or clamp the rate, and until v1.9.19 nothing
  said so.** **since v1.9.19** the `IMP_ISP_Tuning_SetSensorFPS`
  call is read back and logged once per bring-up as
  `sensor fps: requested N, driver holds n/d, set rc=R` (INFO when they agree,
  **WARN** when the return code is non-zero or the driver holds a different
  rate). That line, not the echoed config value, is what the sensor is actually
  running at. The readback uses `IMP_ISP_Tuning_GetSensorFPS`, which every SDK
  header set timps builds against declares, T10 included; the line degrades
  to `readback unavailable` only when the driver's getter itself fails.
* Editing `sensor.model` to "fix" a wrong-looking camera is almost always
  wrong — the loaded `.ko` decides, not the config.

---

## 4. `image.*` (ISP tuning)

Every `image.*` key is `F_CTRL` and **live** (applied by `isp_apply_image()`
under `g_isp_lock` on each POST). Every key is also **capability-gated**: a key
whose `ISP_HAS_*` macro is undefined on this SoC is still parsed, clamped,
persisted and echoed, but the IMP call is never issued and the log says
`image.<k> unsupported on this platform (persisted only)`. That line is a
**LOGD**, i.e. invisible at the default `general.loglevel = 2` — add
`general.debug_modules = HAL_ING` to see it. The `caps.image`
array of `GET /control` lists exactly the supported subset, and is the reliable
way to check.

| Key | Type | Default | Range | Supported on | Notes |
| --- | --- | --- | --- | --- | --- |
| `image.brightness` | int | `128` | 0..255 | all | 128 = neutral. |
| `image.contrast` | int | `128` | 0..255 | all | |
| `image.saturation` | int | `128` | 0..255 | all | |
| `image.sharpness` | int | `128` | 0..255 | all | |
| `image.hue` | int | `128` | 0..255 | T23 T31 T40 T41 C100 | `ISP_HAS_HUE`. Inert on T10/T20/T21/T30. |
| `image.vflip` | bool | `0` | — | all | Global ISP flip (every channel). |
| `image.hflip` | bool | `0` | — | all | Global ISP flip (every channel). |
| `image.running_mode` | int | `0` | 0..1 | all | `0` day, `1` night (IR-cut/mono pipeline). |
| `image.anti_flicker` | int | `2` | 0..2 | all | `0` off, `1` 50 Hz, `2` 60 Hz. |
| `image.ae_compensation` | int | `128` | 0..255 | T10 T20 T23 T30 T31 C100 | `ISP_HAS_AECOMP` — **absent on T21** and on T40/T41. |
| `image.max_again` | int | `160` | 0..255 | T10 T20 T21 T23 T30 T31 C100 | `ISP_HAS_GAINS`; absent on T40/T41. |
| `image.max_dgain` | int | `80` | 0..255 | T10 T20 T21 T23 T30 T31 C100 | `ISP_HAS_GAINS`. |
| `image.sinter_strength` | int | `128` | 0..255 | T10 T20 T21 T23 T30 T31 C100 | `ISP_HAS_NR`, spatial NR. |
| `image.temper_strength` | int | `128` | 0..255 | T10 T20 T21 T23 T30 T31 C100 | `ISP_HAS_NR`, temporal NR. |
| `image.dpc_strength` | int | `128` | 0..255 | T23 T31 C100 | `ISP_HAS_DPC`. |
| `image.defog_strength` | int | `128` | 0..255 | T23 T31 C100 | `ISP_HAS_DEFOG`. |
| `image.drc_strength` | int | `128` | 0..255 | T21 T23 T31 C100 | `ISP_HAS_DRC` (WDR). |
| `image.highlight_depress` | int | `0` | **0..10** | T10 T20 T21 T23 T30 T31 C100 | `ISP_HAS_HILIGHT`. Note the 0..10 domain, not 0..255. |
| `image.backlight_compensation` | int | `0` | **0..10** | T23 T31 C100 | `ISP_HAS_BACKLIGHT`. |
| `image.core_wb_mode` | int | `0` | 0..1 | T10 T20 T21 T23 T30 T31 C100 | `ISP_HAS_WB`. `0` = auto WB, `1` = manual (then `wb_rgain`/`wb_bgain` apply). |
| `image.wb_rgain` | int | `0` | 0..65535 | T10 T20 T21 T23 T30 T31 C100 | Only meaningful with `core_wb_mode=1`. |
| `image.wb_bgain` | int | `0` | 0..65535 | T10 T20 T21 T23 T30 T31 C100 | Only meaningful with `core_wb_mode=1`. **since v1.9.21 (unreleased)** start value: read-only `image.wb_live.rgain`/`.bgain` in `GET /control` = gains AWB applies now. |
| `image.ae_it_max_us` | int | `0` | 0..1000000 | T10 T20 T21 T23 T30 T31 C100 | `ISP_HAS_AE_IT_MAX` (T23/T31/C100) or `ISP_HAS_AE_IT_RANGE` (T10/T20/T21/T30). Inert on T40/T41. `0` = leave the sensor mode's own AE maximum alone. |

Prose and pitfalls

* **`running_mode` is a command, not just a value.** A `POST` of the value it
  already has is normally deduplicated, but `image.running_mode` is
  re-asserted to the ISP anyway (without re-persisting), because a
  `SetISPRunningMode` issued mid-AE-ramp at dusk is accepted and does not always
  latch. The HAL also kicks FrameSource channel 0 after the change, because the
  ISP only latches a running-mode change while chn0 delivers frames, and
  re-asserts `hflip`/`vflip` afterwards (a mode switch can reset them on some
  driver combinations).
* **Do not force `running_mode` when the scene is genuinely dark.** Setting it
  as an ISP-only override desynchronises the ISP from the board's own IR-cut and
  illuminator state and produces a purple/IR-tinted image. Use the day/night
  automaton (`daynight.*`) or the board script instead.
* **`hflip`/`vflip` need a chn0 kick too.** A flip applied while chn0 is idle
  (boot, or a POST with nobody watching) is coalesced into a single kick at the
  end of the request. On T21 the ISP samples the Bayer pattern at sensor probe
  time, so a flip that changes the pattern may need a full restart to look
  right.
* **`ae_it_max_us` has three documented surprises** (measured on T31X/sc4336p):
  1. The write only takes while chn0 is actually delivering frames to a
     consumer. An apply into an idle pipeline is accepted, echoed back by
     `GetExpr`, and silently ignored by the sensor. The HAL re-applies and
     verifies it from the encode threads' frame path, so a persisted value takes
     hold roughly within ~30 s of the first real client after boot.
  2. Within one daemon lifetime the cap only ratchets **down**. Raising it
     again, or setting `0`, needs a restart.
  3. It **moves the day/night exposure index**: capping the AE's maximum
     integration time makes the AE answer a shortfall with gain instead, so a
     camera running this key needs its `daynight.day_gain`/`night_gain`
     re-checked rather than inherited.
* `image.hflip=1` + `image.vflip=1` is the supported way to get a 180° picture
  on every SoC except T40/T41 (see `videoN.rotation`).
* Unit note: `ae_it_max_us` is **microseconds**, deliberately not sensor lines —
  the HAL converts with the SDK's `one_line_expr_in_us` and clamps into the
  sensor's real range at apply time.
---

## 5. `video<N>.*` (encoder streams)

**Index range:** `N` is `0` or `1` only. `MS_MAX_VSTREAM = 2` (`src/config.h`), and
the parser matches the literal prefixes `video0.` and `video1.` — `video2.*`
is an unknown key.

Over `POST /control` the section is `{"video":{"0":{...},"1":{...}}}`.

Keys `enabled` … `rtsp_path` (20 keys) are `F_CTRL`. `imp_chn`, `jpeg`,
`jpeg_quality`, `jpeg_fps`, `jpeg_chn` are `F_NOGET` and **file-only** —
internal channel wiring, deliberately not exposed over HTTP.

| Key | Type | Default (video0 / video1) | Range | Apply | Notes |
| --- | --- | --- | --- | --- | --- |
| `enabled` | bool | `1` / `1` | — | restart | Restart-only in the strongest sense: all consumers read `g_cfg_boot`, so a live-enabled stream is "servable" but has no publisher and the client just hangs. |
| `codec` | enum | `h264` / `h264` | `h264`, `h265` (`hevc` = `h265`); anything else → `h264` | restart | **Coerced to `h264` with a warning on T10, T20 and T23.** T10/T20 have no H.265 encoder; T23's SDK marks every H.265 rc struct unsupported, so `IMP_Encoder_CreateChn` fails. The coercion is what keeps that out of the bring-up path: since 1.9.3 a `start()` failure is no longer fatal — it retries up to `MS_STARTUP_MAX_START_FAILS` (10) times and then escalates to one reboot (`src/main.c`), so an uncoerced H.265 on T23 would be a reboot loop rather than a clean exit. |
| `width` | int | `1920` / `640` | 64..4096 | restart | |
| `height` | int | `1080` / `360` | 64..4096 | restart | |
| `fps` | int | `25` / `25` | 1..120 | restart | Per-channel rate — **this stream only**. It does not set the sensor rate (that is `sensor.fps`) and cannot exceed what `sensor.fps` delivers. Documented explicitly **since v1.9.19**; the behaviour is unchanged. |
| `bitrate` | int | `3000` / `512` | 16..50000 | **live** on every Ingenic SoC (see below) | **kbps.** |
| `rc_mode` | enum | `cbr` / `cbr` | `cbr`, `vbr`, `fixqp`, `smart`, `capped_vbr`, `capped_quality`; anything else → `cbr` | live on classic SoCs, restart on T31/C100/T40/T41 | Alias: `mode`. |
| `gop` | int | `50` / `50` | 1..1000 | restart | The real keyframe interval (`rcAttr.maxGop` / `gopAttr.uGopLength`). |
| `max_gop` | int | `60` / `60` | 1..1000 | **no effect** | **RESERVED and IGNORED.** No HAL consumes it; the keyframe interval comes from `gop`. A non-zero value logs one WARN per session. Parsed, clamped, persisted and echoed purely for compatibility. |
| `profile` | int | `2` / `2` | 0..2 | restart | `0` baseline, `1` main, `2` high. |
| `qp` | int | `35` / `35` | 1..51 | live on classic SoCs only | **Only consumed when `rc_mode = fixqp`** (it becomes `iInitialQP` / `attrH264FixQp.qp`). Under CBR/VBR/smart/capped_* it has no consumer at all — use `min_qp`/`max_qp` there. Deliberately **not** live on T31/C100/T40/T41: `SetChnAttrRcMode` writes it into the struct the next Get reads back but never re-programs the running channel (measured on T31X). |
| `min_qp` | int | `20` / `20` | 1..51 | **live** everywhere | |
| `max_qp` | int | `45` / `45` | 1..51 | **live** everywhere | |
| `quality_lvl` | int | `2` / `2` | 0..7 | live on classic SoCs; **no effect** on T31/C100/T40/T41 | VBR/Smart: `minBitRate = bitrate * quality[lvl]`. No new-API equivalent exists anywhere; the HAL warns once if it deviates from the default. |
| `change_pos` | int | `80` / `80` | 50..100 | live on classic SoCs; **no effect** on T31/C100/T40/T41 | VBR/Smart: % of bitrate above which QP is raised. |
| `i_bias_lvl` | int | `0` / `0` | -3..3 | live on classic SoCs and T31/C100; **no effect at all** on T40/T41 | VBR+CBR I-frame QP bias. `ENC_HAS_QPIPDELTA` is defined for **T31/C100 only** (`src/hal/hal_ingenic.c`): T40 and T41 have no `IMP_Encoder_SetChnQpIPDelta` and no new-API struct field, so the value is parsed, clamped, persisted, echoed and **ignored** (one WARN per session). A restart does not help. |
| `fluc_lvl` | int | `0` / `0` | 0..4 | **never live anywhere**; classic-SoC restart only | **H.265 only** — the H.264 rc structs have no `flucLvl` field. It is written into `attrH265Vbr`/`attrH265Cbr` in `classic_rc_fill()` and nowhere else, so: no effect on T31/C100/T40/T41 (no new-API equivalent), and inert on T10/T20/T23 because `codec = h265` is coerced away there. On T21/T30 it reaches the struct at channel creation, but classic H.265 channels are restart-bound (the classic `SetChnAttrRcMode` is H.264-only), so it never applies live. |
| `rotation` | enum/int | `0` / `0` | `0`, `90`, `270`, plus `180` on T40/T41; legacy `1`→90, `2`→270 | restart | See the prose below. Unsupported values coerce to `0` with a warning. |
| `buffers` | int | `2` / `2` | 1..8 | restart | IMP `nrVBs`. Setting it explicitly also sets an internal `buffers_explicit` flag, so the T31 safety clamp trusts your value instead of overriding it. The clamp gate is exactly `chn == 0 && isp_ch0_pre_dequeue_time != 0` (unreadable counts as active) — **scaled or not**; the older "non-scaled channel" theory was superseded in 2026-08. With the flag set the HAL warns and leaves `nrVBs` alone, which is why an explicit `buffers = 2` is *not* the same as omitting the line. |
| `rtsp_path` | string[64] | `/ch0` / `/ch1` | — | **live** | The one `videoN.*` key that is genuinely live — a DESCRIBE re-matches it on every request, and it is read from the live `g_cfg`, not the boot snapshot. **Since v1.9.20** the POST reply no longer lists it under `deferred` (v1.9.19 did, although the change was already live). Not in `caps.video_live`, which is the rate-control list only. |
| `imp_chn` | int | `0` / `1` | 0..8 | **file-only**, restart | IMP encoder channel index. libimp's own bound is `chn < 9`; above `MS_FS_MAXCHN` the frame source silently returns nothing — no video, no diagnostic. Must be unique across all encoders. |
| `jpeg` | bool | `1` / `1` | — | **file-only**, restart | Alias `jpeg_enabled`. Piggyback JPEG encoder in the same encoder group, sharing this stream's FrameSource (no extra rmem) at this stream's resolution. |
| `jpeg_quality` | int | `75` / `75` | 1..100 | **file-only**, restart | |
| `jpeg_fps` | int | `5` / `5` | 1..120 | **file-only**, restart | Max snapshot/MJPEG publish rate. |
| `jpeg_chn` | int | `3` / `4` | 0..8 | **file-only**, restart | `MS_MAX_VSTREAM + 1 + N`. Must be unique. |

### Live vs restart for `videoN.*`

`GET /control` reports the conservative `caps.restart: ["video","sensor",…]`
plus a per-build `caps.video_live` list (`src/enc_caps.h`). `rtsp_path` is the
other live exception; it is not a rate-control key, so it is not in
`caps.video_live`.

| Platform | `video_live` keys |
| --- | --- |
| T10 T20 T21 T23 T30 (classic rc API) | `rc_mode`, `bitrate`, `qp`, `min_qp`, `max_qp`, `quality_lvl`, `change_pos`, `i_bias_lvl` |
| T31, C100 | `bitrate`, `min_qp`, `max_qp`, `i_bias_lvl` |
| T40 | `bitrate`, `min_qp`, `max_qp` |
| T41 | `bitrate`, `min_qp`, `max_qp` |
| host sim | *(none)* |

A listed key can still fall back to restart at runtime (channel not running, a
classic-API H.265 channel, a rejected IMP call). The per-request truth is the
POST reply's `deferred`/`deferred_keys`, not this list. A live rate-control
change takes effect at the next IDR/GOP, not instantly.

On the classic SoCs a live rate-control key re-fills the whole rc union. **Since
v1.9.20** `codec` and `fluc_lvl` in that re-fill come from the
boot snapshot, i.e. from what the channel was actually built with. On v1.9.19 a
POSTed-but-not-yet-restarted `codec` leaked into it: after `codec = h265` on a
running H.264 stream, every later live `bitrate` was refused as "H265"; the
reverse sent an H.264 union to a running H.265 channel. Only T21 and T30 could
hit this — T10, T20 and T23 coerce `codec` to `h264` at parse time.

### `rotation` in detail (`src/rotate_caps.h`)

* The whole feature is a **build-time opt-in**: without `USE_ROTATE`
  (`BR2_PACKAGE_TIMPS_ROTATE`, default `n`) no `ROT_HAS_*` macro is defined and
  `prot()` coerces every non-zero rotation to `0`. `GET /control` then omits the
  `caps.rotation` key entirely.
* With `USE_ROTATE`, 90/270 need a real transpose path:
  * **T40/T41** — hardware I2D (`ROT_HAS_HW_I2D`).
  * **T31** — libimp FrameSource rotate (`ROT_HAS_FS_ROTATE`).
  * **T23** — software NV12 transpose, only with the extra opt-in
    `BR2_PACKAGE_TIMPS_SW_ROTATE` (`-DMS_ENABLE_SW_ROTATE`). Costs real CPU on
    the single-core T23, is H.264 only, and loses hardware OSD/privacy on the
    rotated stream.
  * **T10 T20 T21 T30 C100** — no path at all; 90/270 coerce to `0`.
* **`180` is not a rotation on most SoCs.** On every classic-API SoC it was only
  a global ISP Hflip+Vflip, falsely modelled as per-stream, and was removed —
  it coerces to `0` with a log line telling you to use
  `image.hflip=1` + `image.vflip=1`. On **T40/T41 only** it is a genuine
  per-channel hardware I2D rotate, which the global flips cannot replicate.
* A 90/270 rotation **swaps the effective width/height** for every downstream
  consumer (encoder attrs, OSD, hub, muxers, JPEG piggyback).
* Legacy `rotation = 1|2` (the raw libimp `rotTo90` enum, as used in
  prudynt/raptor configs) maps to 90/270 and round-trips to the same physical
  direction.

### `videoN.*` pitfalls

* **`bitrate` means different things per rc mode and per SoC.** Under `cbr` it
  is the target the encoder holds; under `vbr`/`smart` it is the ceiling and
  `quality_lvl` sets the floor (classic SoCs only); under `fixqp` it is ignored
  and `qp` decides. On T31/C100/T40/T41 the classic knobs that would shape it
  (`quality_lvl`, `change_pos`, `fluc_lvl`) do nothing.
* **`qp` under CBR does nothing.** A very common mistake: use `min_qp`/`max_qp`.
* **`max_gop` does nothing.** Use `gop`.
* **`fluc_lvl` does nothing on an H.264 stream, and never applies live on any
  SoC** — see its row above.
* Enabling a boot-disabled stream over `/control` reports success and leaves a
  hanging client — you must restart.
* `videoN.fps` above `sensor.fps` is silently capped by the sensor — and
  `videoN.fps` is not a way to raise `sensor.fps`. Compare the requested rate
  with the `sensor fps: requested N, driver holds n/d` line
  (**since v1.9.19**) before blaming the encoder.
* Two streams sharing an `imp_chn` or `jpeg_chn` is a silent failure mode: no
  video and no clear diagnostic.

---

## 6. `audio.*`

Every `audio.*` key is `F_CTRL` (POST-able). The **live vs restart** split is
listed per key below; the restart keys carry `F_RESTART` in `audio_fields[]`,
and **since v1.9.20** they are listed in `caps.restart` as
`audio.<key>` and a changed one comes back in the POST reply's `deferred_keys`. An audio key the SoC does not have logs
`audio.<k> unsupported on this platform (persisted only)` — like the `image.*`
twin this is a **LOGD**, invisible at `general.loglevel = 2`; use `caps.audio`
from `GET /control` instead.

| Key | Type | Default | Range | Apply | Notes |
| --- | --- | --- | --- | --- | --- |
| `audio.enabled` | bool | `1` | — | restart | Master AI capture switch. |
| `audio.codec` | enum | `aac` | `aac`, `pcmu`/`g711u`/`ulaw`, `pcma`/`g711a`/`alaw`, `opus` (USE_STREAM_OPUS builds only), `none`/`off`. **Any unrecognised word silently becomes `aac`.** | restart | `aac` needs `USE_FAAC` (`BR2_PACKAGE_TIMPS_FAAC`, default y). `opus` is only recognised in a `USE_STREAM_OPUS` build; elsewhere it falls through to `aac`. |
| `audio.codec2` | enum | `pcmu` on `USE_WEBRTC` builds, else `none` | `pcmu`/`g711u`/`ulaw`/`on`/`true`/`yes`/`1` → PCMU; `none`/`off`/`false`/`no`/`0`/empty → none. **Anything else warns and becomes `none`** (deliberately *not* the `aac` fallback `codec` uses). | restart | Optional second encode of the same PCM, published on a separate hub source and consumed **only by WebRTC** (whose SRTP audio path carries G.711 and nothing else). Ignored when `codec` is already G.711. |
| `audio.samplerate` | int | `16000` | 8000..96000 | restart | |
| `audio.channels` | int | `1` | 1..2 | restart | `1` mono (native), `2` = simulated stereo (mono mic duplicated to L=R, **AAC only**). Anything else would put a bogus channel count in the AAC ASC / SDP / fMP4 `stsd`. |
| `audio.bitrate` | int | `32` | 8..320 | restart | kbps, AAC only. |
| `audio.volume` | int | `80` | 0..100 | **live** | `IMP_AI_SetVol`. |
| `audio.gain` | int | `25` | 0..31 | **live** | `IMP_AI_SetGain`. |
| `audio.high_pass` | bool | `1` | — | restart | HPF. Restart-required **by necessity**: libimp runs HPF/AGC/NS on its own internal record thread and `IMP_AI_Disable*` frees that module state with no lock — toggling it live races the vendor thread into a use-after-free inside `libaudioProcess.so`. |
| `audio.agc` | bool | `0` | — | restart | Same UAF reason as `high_pass`. |
| `audio.ns` | int | `0` | 0..3 | restart | `0` off, `1..3` = level. Same UAF reason. |
| `audio.alc_gain` | int | `0` | 0..7 | **live**, capability-gated | `IMP_AI_SetAlcGain` analog PGA. **Only on T21, T31, C100** (`AUDIO_HAS_ALC_GAIN`). A T10 build compiles against T20 headers, so T10 has no ALC either. |
| `audio.agc_target_dbfs` | int | `10` | 0..31 | restart | AGC `TargetLevelDbfs`. |
| `audio.agc_compression_db` | int | `0` | 0..90 | restart | AGC `CompressionGaindB`. |
| `audio.mute` | bool | `0` | — | **live** | The only `_Atomic` config field: the per-frame audio worker reads it lock-free. `1` = captured frames are dropped before the encoder/hub, so **no** client gets audio. |
| `audio.force_stereo` | bool | `0` | — | restart | Persist-only companion to `channels`; read at the next audio init. |
| `audio.spk_enabled` | bool | `1` | — | takes effect at the **next AO open** | Master gate for the physical speaker; `0` keeps the AO closed, so neither the backchannel nor local playback makes any sound. A session already holding the speaker keeps it. |
| `audio.spk_volume` | int | `80` | 0..100 | **live** if a play/backchannel session holds the speaker; otherwise applied at the next AO open | Needs `USE_PLAY` or `USE_BACKCHANNEL`, else persist-only. |
| `audio.spk_gain` | int | `25` | 0..100 | same as `spk_volume` | |
| `audio.backchannel` | bool | `0` | — | restart | ONVIF audio backchannel (client → speaker). Needs `USE_BACKCHANNEL`. The pipeline is configured once at boot and RTSP gates on that boot state, so a live change does nothing until restart. |
| `audio.backchannel_codec` | enum | `pcmu` (0) | `pcmu`→0, `pcma`→1, `aac`→2, or a number clamped 0..2 | restart | `aac` additionally needs `USE_BC_AAC`. **Reads back as a number**, not a word. |
| `audio.backchannel_rate` | int | `16000` | 8000..48000 | restart | Speaker sample rate. |
| `audio.aec` | bool | `0` | — | applied at the **next AO open** | Acoustic echo cancellation (`IMP_AI_EnableAec`). A live POST persists and returns "not live". Needs `USE_PLAY`/`USE_BACKCHANNEL`; otherwise persist-only. Opt-in because quality/latency varies per SoC/mic/speaker pairing. |
| `audio.talk_ws` | tri-state int | `0` | 0..2 | **live** for new requests | Browser-microphone backchannel over a WebSocket at `/talk` on the HTTP port. `0` off; `1` on with **TLS required** (`/talk` answers 426 on a plaintext port); `2` on with TLS preferred but a plain `ws://` upgrade accepted. Legacy `true`/`on`/`yes` parse as the **strict** `1`. Needs `USE_BC_WS` (which implies `USE_BACKCHANNEL` + `USE_CONTROL`). |

### `audio.*` pitfalls

* **`audio.codec` swallows typos.** `audio.codec = ac3` is stored as `aac`
  without a warning. Verify with `GET /control`.
* **`audio.codec2` is the opposite** — an unrecognised word warns and disables.
* `audio.channels = 2` only does anything with `codec = aac`.
* `high_pass`/`agc`/`ns` look like live controls in a UI and are not — they are
  deliberately absent from `caps.audio` for exactly that reason. `caps.audio`
  only ever lists `volume`, `gain`, `mute`, plus `alc_gain` (T21/T31/C100) and
  `spk_volume`/`spk_gain`/`aec` (USE_PLAY/USE_BACKCHANNEL builds).
* `audio.mute = 1` is often mistaken for a bug report ("no audio on any
  client") — check it first.
* `audio.talk_ws = 1` on a build without TLS can never be satisfied: `/talk`
  always answers 426.
* `audio.aec` without also enabling the speaker path does nothing.

---

## 7. `jpeg.*` (dedicated MJPEG/snapshot encoder)

The whole section is **file-only**: no key carries `F_CTRL`, and the section is
marked `noget`, so `GET /control` never echoes it either.

| Key | Type | Default | Range | Apply | Notes |
| --- | --- | --- | --- | --- | --- |
| `jpeg.enabled` | bool | **`0`** | — | file-only, restart | Off by default. The per-stream piggyback encoders (`videoN.jpeg`, default on) normally cover snapshots and the WebUI thumbnail; this dedicated channel is the old "stream2". |
| `jpeg.width` | int | `640` | 64..4096 | file-only, restart | |
| `jpeg.height` | int | `360` | 64..4096 | file-only, restart | |
| `jpeg.quality` | int | `75` | 1..100 | file-only, restart | |
| `jpeg.fps` | int | `5` | 1..120 | file-only, restart | Max MJPEG frame rate. |
| `jpeg.imp_chn` | int | `2` | 0..8 | file-only, restart | Must not collide with `videoN.imp_chn` (0, 1) or `videoN.jpeg_chn` (3, 4). |
| `jpeg.snapshot_path` | string[128] | `""` | — | file-only, restart | Periodic file snapshot; `""` = none. |

Pitfall: `timps.conf.example` shows `jpeg.enabled = 1`; the **compiled default
is `0`**. The example says so in a comment, but the two do not agree at a
glance.

Source selection (`hub_pick_jpeg_src`): a piggyback source is preferred when
that stream was **boot**-enabled and has `videoN.jpeg` on; otherwise `jpeg.*`;
otherwise the first boot-enabled stream with a piggyback encoder.
---

## 8. `osd.*` (globals)

All six keys are `F_CTRL` (POST-able). `enabled`, `font_path`, `supersample`
and `hinting` are **restart** (`F_RESTART`): `imp_osd_setup()` reads them once at
startup. `monitor_stream` and `vars_file` are **live**: the OSD thread re-reads
them on every text refresh (about once a second).

**Since v1.9.20** the API says so: the four restart keys are in
`caps.restart` and come back in the POST reply's `deferred_keys`, and
`ing_control()` logs `persisted, applies on restart` only for them. v1.9.19
listed only `osd.enabled` in `caps.restart`, never reported an `osd.*` key as
deferred, and logged "applies on restart" for all six — including the two that
already applied live.

| Key | Type | Default | Range | Apply | Notes |
| --- | --- | --- | --- | --- | --- |
| `osd.enabled` | bool | `1` | — | restart | Master switch. Listed in `caps.restart`. |
| `osd.monitor_stream` | int | `0` | **unclamped** | **live** | Which stream's measured rate feeds the `{fps}`/`{bitrate}` placeholders. Out of range is not rejected — the lookup just returns `0.0`, so `{fps}` prints `0.0`. (Contrast `motion.monitor_stream`, which is `T_CHAN` and coerces to 0.) |
| `osd.font_path` | string[128] | `/usr/share/fonts/default.ttf` | — | restart | Default TTF for text items. Empty = built-in bitmap font. |
| `osd.vars_file` | string[128] | `/tmp/timps_osd.vars` | — | **live** | Extra placeholder source: `name=value` lines, looked up for any `{name}` the built-ins do not resolve. |
| `osd.supersample` | int | `2` | 1..4 | restart | TTF rasterizer AA samples per axis per pixel. Cost is roughly quadratic (4 → 16 samples/px). `2` is visually indistinguishable from `4` at OSD sizes and roughly halves rasterizer CPU. |
| `osd.hinting` | bool | `1` | — | restart | Lightweight geometric autohint (snaps stem-like outline edges to the pixel grid at small sizes). **Not** a TrueType bytecode interpreter. **The Kconfig help text is wrong** — `BR2_PACKAGE_TIMPS_OSD_HINTING`'s help says the runtime key defaults to "0 (off) either way"; `config_defaults()` sets `hinting = 1`. The code wins. |

Pitfalls
* **`osd.hinting` does nothing unless the build has `USE_OSD_HINTING`**
  (`BR2_PACKAGE_TIMPS_OSD_HINTING`, default **n**). It is still parsed, clamped,
  persisted and echoed. `config.c` logs one WARN per session when a non-zero
  value is set on such a build: *"osd.hinting is stored but has no effect in
  this build"*.
* `osd.supersample` and `osd.hinting` are read once by `imp_osd_setup()`; the
  WebUI cannot change them live even though they are POST-able.

---

## 9. OSD items — `osd<S>.<N>.*` (canonical) and `osd<N>.*` (legacy)

**Index ranges:** `S` = video stream, `0..MS_MAX_VSTREAM-1` = **0..1**.
`N` = item index, `0..MS_MAX_OSD-1` = **0..7**. Each stream has its own
independent set of 8 items.

**Legacy form `osd<N>.<field>`** (no stream digit) is still parsed and writes the
item onto **every** stream. It reads back only while all streams still agree on
the value; once they diverge, `GET /control` reports the legacy key as unknown
(so a legacy write always applies rather than being falsely deduplicated).

Over `POST /control` the canonical form is `{"osd0":{"0":{...}}}` /
`{"osd1":{"3":{...}}}`; the legacy shared form is `{"osd":{"0":{...}}}`.

| Key | Type | Default | Range | Apply | Notes |
| --- | --- | --- | --- | --- | --- |
| `enabled` | bool | item 0–3 `1`, items 4–7 `0` | — | **restart** | POST-able, but an item that was **disabled at boot has no IMP region**, so enabling it live is a silent no-op. Deliberately excluded from `caps.osd` for that reason. |
| `type` | enum | `text` (item 3: `logo`) | `logo` → logo, **anything else** → `text` | **restart in practice** | POST-able and persisted, and the item is re-applied — but the region's text-vs-logo render dispatch (`rg->is_text`) is fixed when the region is created at startup, so a live text↔logo switch keeps rendering the old kind until restart. |
| `text` | string[128] | see layout below | — | **live** | strftime `%` sequences plus `{placeholder}`s. |
| `logo` | string[128] | item 3: `/usr/share/images/thingino_100x30.bgra`, else `""` | — | **file-only** | Alias `logo_path`. Raw BGRA file. |
| `logo_w` | int | item 3: `100`, else `0` | 0..4096 | **file-only** | Alias `logo_width`. |
| `logo_h` | int | item 3: `30`, else `0` | 0..4096 | **file-only** | Alias `logo_height`. |
| `x` | int | see layout | **unclamped** | live | `0` = centred horizontally; positive = px from the left; negative = px from the right. |
| `y` | int | see layout | **unclamped** | live | `0` = centred vertically; positive = from the top; negative = from the bottom. |
| `font_size` | int | `32` on stream 0, `12` on stream 1 | **8..128** | live | Absolute pixels, no per-stream auto-scaling. Ceiling lowered from 256 to 128 because a 255-char item at 256 px could transiently allocate a ~14 MB canvas. |
| `color` | hex | `0xFFFFFFFF` | — | live | `0xAARRGGBB`. Alias `font_color`. Reads back as `0x%08X`. |
| `transparency` | int | `255` | 0..255 | live | Group alpha. Clamped so e.g. `300` does not wrap to 44 while the config echoes 300. |
| `outline` | int | `1` | **0..64** | live | Text outline/stroke width in px, `0` = off. Alias `stroke`. |
| `outline_color` | hex | `0xFF000000` | — | live | Alias `stroke_color`. |
| `font_path` | string[128] | `""` | — | **file-only** | Optional per-item TTF override. |

**Default layout** (identical on every stream):

| Item | enabled | type | x | y | content |
| --- | --- | --- | --- | --- | --- |
| 0 | 1 | text | `10` | `10` | `%Y-%m-%d %H:%M:%S` (top-left) |
| 1 | 1 | text | `0` | `10` | `{hostname}` (top-centre) |
| 2 | 1 | text | `-10` | `10` | `{uptime}` (top-right) |
| 3 | 1 | logo | `-10` | `-10` | `thingino_100x30.bgra` (bottom-right) |
| 4–7 | 0 | text | `10` | `10` | *(empty)* |

**Built-in `{placeholder}`s** (`src/hal/osd_vars.c`): `{hostname}`, `{ip}`,
`{mac}`, `{fps}`, `{fps0}`/`{fps1}`, `{bitrate}`, `{bitrate0}`/`{bitrate1}`,
`{uptime}`, `{net}` (= `{tx}`), `{cpu}`, `{mem}`, `{clients}`. Anything else is
looked up in `osd.vars_file`; an unresolved name expands to the empty string.
`{fps}`/`{bitrate}` follow `osd.monitor_stream`; the numbered forms are per
stream and independent of it. A channel idle for more than ~2 s reports `0`.

Pitfalls
* **Enabling an item live does not work** — save and restart.
* A `#` in OSD text survives a `/control` write (the writer quotes the value),
  but a hand-edited unquoted `text = Kamera #2` line loses everything from the
  `#`. Quote it.
* `logo`, `logo_w`, `logo_h` and the per-item `font_path` are file-only; the
  WebUI item editor cannot set them.
* Very large `font_size` values are clamped to 128, silently.

---

## 10. Privacy masks — `privacy<S>.<N>.*`

**Index ranges:** `S` = stream `0..1` (`MS_MAX_VSTREAM`), `N` = region `0..3`
(`MS_MAX_PRIVACY = 4`). Four regions per stream.

Every key is `F_CTRL` and **live** — the IMP OSD cover region is created,
shown, hidden or moved at runtime.

**POST shape** (`control_apply_json()` looks up the literal section name
`privacy`, then the stream index, then the region index):

```json
{"privacy": {"0": {"1": {"enabled":1,"x":100,"y":80,"w":320,"h":180}}}}
```

`{"privacy0":{"1":{…}}}` does **not** work — it is an unknown top-level
section, silently unapplied and absent from the reply's `ignored` list. This is
the one place where the JSON nesting differs from the OSD-item convention
(`{"osd0":{"1":{…}}}`), so it is an easy mistake to make.

| Key | Type | Default | Range | Apply | Notes |
| --- | --- | --- | --- | --- | --- |
| `enabled` | bool | `0` | — | live | |
| `x` | int | `0` | unclamped | live | Pixels in that stream's frame. |
| `y` | int | `0` | unclamped | live | |
| `w` | int | `0` | unclamped | live | Alias `width`. |
| `h` | int | `0` | unclamped | live | Alias `height`. |
| `color` | hex | `0xFF000000` (opaque black) | — | live | `0xAARRGGBB` fill. Alias `fill_color`. |

Caveat that matters in support: `caps.privacy.available` is **0** when no IMP
OSD group exists in the running pipeline. `imp_osd_setup()` builds a group per
stream only when OSD **or** a privacy region was enabled **at boot**. On a
camera booted with OSD off and all privacy regions off, a privacy POST persists
and changes nothing visible until restart.

A privacy region on the motion-monitored stream is excluded from the IVS grid
(they share the FrameSource, and the cover would otherwise trip motion), so
changing one rebuilds the motion grid live. Dragging a rectangle posts
`x`+`y`+`w`+`h` in one request and the rebuild is batched to once per request.

---

## 11. `motion.*`

`MOTION_AVAILABLE` and the cell budget come from the SDK header actually built
against (`src/motion_caps.h`): `MOTION_MAX_CELLS = IMP_IVS_MOVE_MAX_ROI_CNT`,
which is **52** on T10/T20 3.12.0, T21, T23, T30, T31, T40, T41 and C100, but
only **4** on the old T10/T20 3.9.0 SDK. A build whose SDK has no IVS move
support reports `caps.motion.available = 0` and the feature is a stub.

| Key | Type | Default | Range | Apply | Notes |
| --- | --- | --- | --- | --- | --- |
| `motion.enabled` | bool | `0` | — | **live** | Rebuilds the IVS grid. |
| `motion.monitor_stream` | int | `0` | `T_CHAN`: out of `0..1` **coerces to 0** | **live** | |
| `motion.sensitivity` | int | `128` | 0..255 | **live** | Mapped to the SDK's 0..4 as `v*4/255`. A change that maps to the same level skips the grid rebuild but **is** still persisted. |
| `motion.cols` | int | `5` (see below) | ≥1, and `cols*rows ≤ MOTION_CELL_LIMIT` | **live** | Clamped against the *current* other axis, never the other way round, so re-applying the same pair is idempotent. |
| `motion.rows` | int | `5` (see below) | same | **live** | |
| `motion.cooldown_ms` | int | `5000` | **250..INT_MAX** | **file-only** | Minimum gap between motion events. **Not POST-able by design** — it is the floor that bounds how often the `on_motion` hook can be re-exec'd. `0` is no longer accepted. |
| `motion.hold_ms` | int | `800` | 0..INT_MAX | **live** | Keep a cell "active" this long after its last hit, so asynchronous `/events`/`/control` readers reliably observe single-frame motion. `0` = no hold. Takes effect through a grid re-sync. |
| `motion.skip_frames` | int | `5` | 1..INT_MAX | **live** | `IMP_IVS_MoveParam.skipFrameCnt` — analyse every Nth frame. Higher = cheaper but more latency. Takes effect through a grid re-sync. |
| `motion.on_motion` | string[128] | `""` | — | **file-only**, `F_NOGET` | Program run on motion via **`posix_spawn()`** with the value as the literal path (`src/hal/imp_motion.c`) — **not** a shell command line, **no arguments**, and **no `PATH` search**, because `posix_spawn` execs with `execve`. A bare command name therefore always fails: uClibc-ng's `__spawni` `_exit(127)`s, which surfaces as `on_motion '<cmd>' cannot be executed - is the script installed and executable?`. **Use an absolute path.** Never POST-able and never read back: it is an exec primitive. Read from `g_cfg` per event, so a file edit + restart is what applies it. |
| `motion.roi_x` | int | `0` | unclamped | **deprecated, ignored** | Legacy single-ROI keys, replaced by the cell grid. Still parsed and persisted; a non-zero value logs one WARN per session and nothing consumes them. |
| `motion.roi_y` | int | `0` | unclamped | **deprecated, ignored** | |
| `motion.roi_w` | int | `0` | unclamped | **deprecated, ignored** | |
| `motion.roi_h` | int | `0` | unclamped | **deprecated, ignored** | |

**Default grid** depends on the SDK: `5x5` where `MOTION_MAX_CELLS >= 25`,
`2x2` where it is ≥4 (i.e. T10/T20 on the 3.9.0 SDK), `1x1` otherwise.

Pitfalls
* `motion.cooldown_ms` and `motion.on_motion` cannot be set from the WebUI or
  any HTTP client. Edit the file.
* A `cols`/`rows` pair whose product exceeds the SDK budget is silently reduced
  — read back what you got.
* On the thingino package, `motion.on_motion` is shipped pointing at
  `/usr/sbin/timps-motion`, which is installed unconditionally for that reason.
* The IVS grid rebuild is batched to once per `/control` request, so a form POST
  carrying `cols`+`rows`+`sensitivity`+`monitor_stream` rebuilds once.

---

## 12. `record.*`

Requires `USE_RECORD` (`BR2_PACKAGE_TIMPS_RECORD`, default y); otherwise
`caps.record.available = 0` and the keys just persist.

Every key is `F_CTRL` and the running recorder reads them **live** — no restart.

| Key | Type | Default | Range | Apply | Notes |
| --- | --- | --- | --- | --- | --- |
| `record.enabled` | bool | `0` | — | live | Also gates the on-boot start. |
| `record.channel` | int | `0` | `T_CHAN`: out of `0..1` → 0 | live | |
| `record.mode` | enum | `1` (motion) | `motion` → 1, `continuous` → 0, or a raw number | live | **Reads back as a number**, not a word. |
| `record.dir` | string[128] | `/mnt/mmcblk0p1` | — | live | Segments land under `<dir>/<hostname>/records/`. |
| `record.name` | string[96] | `%Y%m%d/%H/%Y%m%dT%H%M%S` | — | live | strftime path template. |
| `record.segment_s` | int | `60` | 0..86400 | live | `0` = single file, no rotation. Alias `record.segment`. |
| `record.pre_roll_s` | int | `3` | 0..60 | live | Motion mode: buffered seconds kept before the trigger. Alias `record.pre_roll`. |
| `record.post_roll_s` | int | `10` | **1..300** | live | Alias `record.post_roll`. **Floor is 1, not 0, deliberately:** motion-triggered recording gates on `last_ms < post_roll_s*1000`, which at 0 is never true even for the triggering event, so `mode=motion` would record nothing at all with `enabled:true` and no warning. |
| `record.min_free_mb` | int | `200` | 0..1048576 | live | Delete oldest segments until at least this much is free. |
| `record.audio` | bool | `1` | — | live | Mux audio into the recording when available. |

Non-config commands on the same section (not persisted):
`{"record":{"active":1|0}}` = manual start/stop override (omit or `<0` returns
to config mode); `{"record":{"clip":"/tmp/x.mp4","seconds":6}}` = on-demand
fMP4 clip, blocking for roughly `seconds`.

---

## 13. `timelapse.*`

Requires `USE_TIMELAPSE` (`BR2_PACKAGE_TIMPS_TIMELAPSE`, default y). Every key is
`F_CTRL` and read **live** by the timelapse thread.

| Key | Type | Default | Range | Apply | Notes |
| --- | --- | --- | --- | --- | --- |
| `timelapse.enabled` | bool | `0` | — | live | Also gates the on-boot start. |
| `timelapse.channel` | int | `0` | `T_CHAN`: out of `0..1` → 0 | live | The stream whose piggyback JPEG encoder is captured; falls back to the dedicated `jpeg.*` channel. |
| `timelapse.dir` | string[128] | `/mnt/mmcblk0p1` | — | live | Shots land under `<dir>/<hostname>/timelapses/`. |
| `timelapse.name` | string[96] | `%Y%m%d/%H/%Y%m%dT%H%M%S` | — | live | strftime template, `.jpg` appended. |
| `timelapse.interval_s` | int | `60` | 1..INT_MAX | live | Alias `timelapse.interval`. |
| `timelapse.keep_days` | int | `7` | 0..INT_MAX | live | `0` = keep forever. |

Pitfall: a `timelapse.channel` pointing at a stream whose `videoN.jpeg` is off
(or that was boot-disabled) falls back rather than failing loudly.

---

## 14. `daynight.*`

Requires `USE_DAYNIGHT` (`BR2_PACKAGE_TIMPS_DAYNIGHT`, default y). The keys are
always parsed so a config with `daynight.*` loads warning-free even on a build
without the thread; there they simply persist.

Every key is `F_CTRL` (live — the detection thread re-reads `g_cfg` on its next
tick) **except**:
* `daynight.mode` — POST-able, but hand-validated in `control.c` rather than by
  the generic walker (so it carries no `F_CTRL`); an unrecognised token over
  HTTP is **rejected with a warning** instead of being coerced.
* `daynight.switch_cmd`, `daynight.isp_path`, `daynight.irprobe_cmd`,
  `daynight.trace_path` — **file-only** and `F_NOGET`. These name commands the
  daemon execs and paths it writes as root; keeping them off the POST surface is
  the arbitrary-file-write / arbitrary-exec boundary.

| Key | Type | Default | Range | Apply | Notes |
| --- | --- | --- | --- | --- | --- |
| `daynight.enabled` | bool | `1` | — | live | `0` = manual mode; the thread idles and nothing forces the ISP mode. |
| `daynight.mode` | enum | `auto` | `auto`, `schedule`; legacy `sensor`→auto, `time`/`sun`→schedule. From the **file**, an unknown token logs a warning and falls back to `auto`; from **POST** it is rejected. | live (hand-validated) | `auto` = full automaton (measurement + probes, calendar optional). `schedule` = the calendar decides outright: no sensor, no probes, two IR-cut clicks a day. Only matters when `enabled=1`. |
| `daynight.time_night_start` | string[6] | `""` | `"HH:MM"` local, **unvalidated** | live | Night at/after this time. Empty = that edge unset. |
| `daynight.time_day_start` | string[6] | `""` | `"HH:MM"` local, **unvalidated** | live | Day at/after this time. The window may wrap past midnight. |
| `daynight.sun_latitude` | float | `0.0` | -90..90 | live | Degrees, +N/−S. |
| `daynight.sun_longitude` | float | `0.0` | -180..180 | live | Degrees, +E/−W. |
| `daynight.sun_sunrise_offset_min` | int | `0` | -1440..1440 | live | Minutes added to sunrise before switching to day. |
| `daynight.sun_sunset_offset_min` | int | `0` | -1440..1440 | live | Minutes added to sunset before switching to night. |
| `daynight.day_gain` | float | `768` | 1..1000000 | live | **Alias `total_gain_day_threshold`.** Exposure index below this (on the day pipeline) = day. Units are the IMP `[24.8]` linear gain scale, `256` = 1.0×, so 768 = 3×. |
| `daynight.night_gain` | float | `4096` | 1..1000000 | live | **Alias `total_gain_night_threshold`.** Exposure index above this (while in day) = night. 4096 = 16×. |
| `daynight.day_confirm_s` | int | `30` | 1..3600 | live | How long the index must stay above `night_gain` before day→night. |
| `daynight.probe_min_gap_s` | int | `600` | **60**..86400 | live | No two probes closer than this. **The only bound on the audible IR-cut click rate** — a safety net, not something to switch off. |
| `daynight.probe_confirm_s` | int | `15` | 1..3600 | live | How long the probe trigger condition must hold. |
| `daynight.heartbeat_s` | int | `14400` (4 h) | 300..604800 | live | Flat re-probe interval while the scene is moving. Deliberately not a multiplying backoff. |
| `daynight.heartbeat_max_s` | int | `43200` (12 h) | 300..604800 | live | Interval once the scene demonstrably has not moved since the last probe. The automaton takes the smaller of the two when the scene is moving. |
| `daynight.boot_probe` | int | `1` | 0..1 | takes effect at the next boot | `1` = **every** boot measures before deciding, regardless of the persisted mode: boot into the day pipeline, read against `day_gain`, then assert the result on the board once. `0` = adopt the persisted mode without measuring — **except** when the AE is railed (zero reserve), where it measures anyway. Boot asserts the mode it ends up with on the board either way. |
| `daynight.interval_ms` | int | `2000` | 100..60000 | live | Sample interval. The exposure index needs a `/proc` scrape per tick (integration time has no IMP API). |
| `daynight.diagnose_thresholds` | int | `0` | 0..1 | live | When probes keep failing and the best day-pipeline reading of that excursion was still clear of `day_gain`, warn that the threshold is unreachable for this scene and name the value to raise it above. It needs **`DN_DIAG_FAILS` = 3 consecutive failed probes** before it fires, and then warns **once per daemon session** (`diag_warned` in `src/daynight.c`) — not once per probe. Off by default anyway, because on a camera that genuinely never sees day it is a WARN nobody asked for. |
| `daynight.history_s` | int | `0` | 0..172800 (48 h) | live | In-RAM decision-history ring for the WebUI tuning graph, in seconds of retention. `0` = off; nothing is allocated until a sample is pushed. One 16-byte sample per period, so the 48 h ceiling costs ~270 KiB. |
| `daynight.switch_cmd` | string[64] | `daynight` | — | **file-only**, `F_NOGET` | Board script, run as `<cmd> day\|night` via `vfork()`+`execlp()` (no shell; `fork()` on v1.9.19). **Unlike `motion.on_motion`, this one *does* search `PATH`** — `execlp`, not `execve` — which is why the bare default `daynight` works. Same for `daynight.irprobe_cmd`. |
| `daynight.isp_path` | string[128] | `/proc/jz/isp/isp-m0` | — | **file-only**, `F_NOGET` | ISP exposure proc file that is scraped. |
| `daynight.irprobe_cmd` | string[64] | `timps-irprobe` | — | **file-only**, `F_NOGET` | Run as `<cmd> on\|off`. Empty disables the **silent** probe entirely and every night→day question falls back to the audible IR-cut probe. |
| `daynight.trace_path` | string[128] | `""` | — | **file-only**, `F_NOGET` | Opt-in CSV decision-trace recorder, one line per N samples. Size-capped and rotated once, so bounded at 2× the cap. **Must live on tmpfs** (`/tmp`, `/run`) — a LOGW reminds you if the path does not look like tmpfs. |

### How the decision works (needed to answer tuning questions)

The metric is the **exposure index**, not bare gain:

```
D = total_gain * (integration_time / max_integration_time)
```

Higher = darker. In a dark scene the AE has the integration time railed at max,
so `D == total_gain` and both thresholds keep their historic meaning. In a
bright scene the gain rails at its `256` (1.0×) floor and the AE shortens the
exposure instead — which bare gain cannot see at all. When the integration-time
fields are unreadable, `D` degrades to `total_gain`.

Both thresholds are only ever evaluated on the **day** pipeline, the only
optical path that reports ambient light honestly. Night-pipeline readings are
used exclusively as a relative change detector.

The **silent probe** briefly switches the IR illuminator off (not the IR-cut
motor, so no click) and compares:

```
r = D(illuminator off) / D(illuminator on)
```

`r >> 1` = the illuminator was doing the work → genuinely night. `r ≈ 1` = the
room supplies the light → day, whatever the absolute level. This exists because
genuine daylight measured across a twelve-camera fleet at one instant spanned a
factor of **63**, so no absolute day threshold can be right everywhere.

### Retired `daynight.*` keys

These are **parsed and ignored with one warning each** (not reported as unknown
keys), because each names a mechanism that no longer exists:

| Retired key | What the warning says to use instead |
| --- | --- |
| `daynight.day_gain_pct` | The adaptive night baseline is gone; night→day is probe-mediated and the probe trigger is a fixed internal constant. |
| `daynight.baseline_delay_s` | Became the fixed constant `DN_REF_DELAY_S`. |
| `daynight.boot_settle_max_s` | The settle wait is bounded internally now. |
| `daynight.boot_stable_pct` | The settle wait is gated on the reading having stopped moving; no tunable. |
| `daynight.night_reconfirm_s` | Replaced by `daynight.heartbeat_s` / `heartbeat_max_s`. |
| `daynight.probe_max_skip_s` | The probe skip it bounded no longer exists. |
| `daynight.threshold_low` | The brightness fallback is no longer a decision path — use `day_gain`/`night_gain`. |
| `daynight.threshold_high` | Same. |
| `daynight.hysteresis` | The brightness fallback is no longer a decision path. |
| `daynight.learn` | The learning subsystem is gone; `diagnose_thresholds` reports an unreachable `day_gain` without touching it. |
| `daynight.state_path` | Only used by `daynight.learn`. |

A second group became **fixed internal constants** whose value equals the old
default. They warn **only when the configured value differs** from the constant
(so a config that never touched them is silent):

`daynight.probe_jump_pct`, `daynight.probe_settle_s`, `daynight.ref_delay_s`,
`daynight.ir_min_headroom`, `daynight.boot_settle_s`, `daynight.transition_s`,
`daynight.ir_ratio_night`, `daynight.ir_ratio_day`.

Their effective values are still readable in the `daynight` status object of
`GET /control` — they are simply no longer per-camera settings.

### `daynight.*` pitfalls

* **`day_gain` too low is the classic failure.** `300` (1.17×) asserted "day
  only when the day pipeline needs essentially no gain", which indoors is never
  true — a normally-lit room in daytime needs 2–3×, so the comparison could
  never come true and the camera sat in night mode in daylight. The current
  default is 768 (3×). Genuinely dim rooms (measured up to 9.79×) need an
  explicit per-camera override; turn on `diagnose_thresholds` to be told the
  value to use.
* **`image.ae_it_max_us` moves the index** — a camera running it needs its
  `day_gain`/`night_gain` re-checked rather than inherited.
* `daynight.enabled = 0` does **not** force a mode; it just stops the automaton.
  Use `image.running_mode` for a manual mode.
* Setting `time_night_start`/`time_day_start` **or** a non-zero lat/lon switches
  the calendar source on even in `auto` mode (an explicit time window wins over
  lat/lon). In `auto` the calendar only schedules probes; in `schedule` it
  decides outright.
* `trace_path` on flash (not tmpfs) wears the flash; that is why it warns.
* `{"daynight":{"probe":1}}` over `/control` requests one silent probe on the
  next tick; it is **rejected** if `irprobe_cmd` is empty, so "nothing happened"
  is distinguishable from "the probe ran and found nothing".
---

## 15. `rtsp.*`

Whole section is **file-only** (`noget` + no `F_CTRL`): not POST-able, not
echoed by `GET /control`. Changes need a restart.

| Key | Type | Default | Range | Apply | Notes |
| --- | --- | --- | --- | --- | --- |
| `rtsp.enabled` | bool | `1` | — | file-only, restart | |
| `rtsp.port` | int | `554` | 1..65535 | file-only, restart | |
| `rtsp.mtu` | int | `1200` | **548..1472** | file-only, restart | Max RTP packet size (header + payload) for UDP packetization. 1200 leaves room for WireGuard/OpenVPN/PPPoE/IPv6 tunnel overhead; raise to 1400 for LAN-only setups. |
| `rtsp.user` | string[64] | `""` | — | file-only, restart | Alias `rtsp.username`. **Empty = RTSP is open, no authentication.** Non-empty enables Digest auth. |
| `rtsp.pass` | string[64] | `""` | — | file-only, restart | Alias `rtsp.password`. |
| `rtsp.tls` | bool | `0` | — | file-only, restart | Alias `rtsp.tls_enabled`. `1` = additionally run an RTSPS listener. Needs `USE_TLS`. **`F_SECVAL`**: a value that parses as neither an on/off word nor an in-range number logs a loud WARN, because the fall-through to `0` is plaintext. |
| `rtsp.tls_port` | int | `322` | 1..65535 | file-only, restart | |

Stream URLs come from `videoN.rtsp_path` (`/ch0`, `/ch1` by default), which is
the one live-matched video key.

---

## 16. `http.*` (fMP4 preview, MJPEG, snapshots, `/control`, `/events`, `/talk`)

Whole section is **file-only** (`noget` + no `F_CTRL`). The credential and token
keys are deliberately unreachable over the very endpoint they protect.

| Key | Type | Default | Range | Apply | Notes |
| --- | --- | --- | --- | --- | --- |
| `http.enabled` | bool | `1` | — | file-only, restart | |
| `http.port` | int | `8880` | 1..65535 | file-only, restart | |
| `http.preview_chn` | int | `1` | **unclamped** | file-only, restart | Default video stream for the preview page. Out-of-range or boot-disabled falls back to `0`. A `?chn=N` query parameter overrides it per request. |
| `http.adaptive_drop` | bool | `1` | — | file-only, restart | Per-client adaptive frame dropping on `/stream.mp4`: a client whose own queue backs up freezes on its last frame and resumes at the next natural keyframe instead of decoding a corrupt headless GOP. Purely per-client — never touches the shared encoder or other subscribers. |
| `http.user` | string[64] | `""` | — | file-only, restart | Alias `http.username`. **Empty = fall back to `rtsp.user`/`rtsp.pass`**, so an empty `rtsp.user` too means the HTTP port is open. |
| `http.pass` | string[64] | `""` | — | file-only, restart | Alias `http.password`. |
| `http.token` | string[64] | `""` | — | file-only, restart | Optional **persistent** remote secret, accepted as a valid `?token=`, for automation. Never written to `token_file`. |
| `http.token_file` | string[128] | `/run/timps.token` | — | file-only, restart | Where the random per-boot token is published for local privileged readers. `""` = do not write. The configured `http.token` secret is never put here. |
| `http.https` | tri-state int | `0` | 0..2 | file-only, restart | Alias `http.tls`. `0` = plain HTTP only. `1` = **both** schemes on the same port, sniffed per connection (`0x16` = a TLS handshake record; every HTTP method starts with a letter). `2` = TLS only, plaintext refused with 426. Legacy `true`/`on`/`yes` parse as `1`. **`F_SECVAL`.** Needs `USE_TLS`. |
| `http.tls_cert` | string[128] | `/etc/ssl/certs/timps.crt` | — | file-only, restart | PEM certificate. |
| `http.tls_key` | string[128] | `/etc/ssl/private/timps.key` | — | file-only, restart | PEM private key. |

Important behaviours

* **Note the widening in v1.9.11:** `http.https = 1` used to mean TLS-only. It
  now means "both". An operator who wants the old strict behaviour must say
  `2`. This is the opposite polarity to `audio.talk_ws`, where `1` is the
  *stricter* value.
* **TLS fails closed.** If the cert/key cannot be loaded, the HTTP listener is
  not bound at all, so `http.https = 2` without a usable cert takes the whole
  HTTP port down.
* Cert sharing with the web UI is arranged at boot by the init script, not by
  the defaults — see §22.8. A browser trusts a self-signed cert per *origin*
  (scheme+host+**port**), and Safari gives `fetch()`/XHR no click-through at
  all, so a second cert on `:8880` makes the preview fail with a bare
  "Load failed" for anyone who only ever trusted the web UI's.
* **The `/mjpeg` busybox proxy no longer exists.** The preview reaches
  `:8880/stream.mp4` and `:8880/stream.mjpeg` directly. `/onvif/image.cgi` is a
  symlink onto `x/ch0.jpg`, which *reads* `http.port` and `http.https` out of
  `/etc/timps.conf` — so a non-default port is fine, but its `case` has no arm
  for `http.https = 2` and falls back to plain `http`, which the daemon then
  `426`s. (The comment still in the shipped `files/timps.conf` about two
  loopback proxies in `/etc/httpd.conf` is stale.)
* What genuinely hardcodes `http://127.0.0.1:8880` is **`send2common`**
  (its `copy_photo` snapshot fetch); **`timps-motion`** reads `http.port` but
  hardcodes the `http://` scheme. Both break on `http.https = 2`. See
  `docs/ai/troubleshooting.md` §8.4 for the full list of which shipped scripts
  accept `2` and which do not.

---

## 17. `events.*` (GET /events SSE push stream)

Whole section is **file-only**; requires `USE_CONTROL`.

| Key | Type | Default | Range | Apply | Notes |
| --- | --- | --- | --- | --- | --- |
| `events.enabled` | bool | `1` | — | file-only, restart | `0` = the endpoint answers 404. |
| `events.stats_ms` | int | `2000` | **unclamped** | file-only, restart | Period of the `stats` event in ms. `0` = no stats events. |
| `events.max_clients` | int | `8` | **unclamped** | file-only, restart | Concurrent `/events` connections; above this the server answers 503. A value `<= 0` falls back to the compiled `EVENTS_MAX_CLIENTS_DEF`. |

---

## 18. `webrtc.*` (WHEP endpoint)

The section only exists in a `USE_WEBRTC` build (`BR2_PACKAGE_TIMPS_WEBRTC`);
otherwise the keys are unknown and log `unknown key`. Whole section is
**file-only** — a live cert/identity swap would strand sessions that already
published a fingerprint in an SDP answer.

| Key | Type | Default | Range | Apply | Notes |
| --- | --- | --- | --- | --- | --- |
| `webrtc.enabled` | tri-state int | `2` | 0..2 | file-only, restart | `0` = `/webrtc/whep` answers 404. `1` = on, TLS required for the signalling POST where the HTTP port has it. `2` = on and accept a plaintext POST too. Default is `2` on purpose: most cameras have no http→https redirect, so `1` would silently 426 the feature it enables. Legacy `true`/`on`/`yes` parse as `1`. |
| `webrtc.port` | int | `0` | 0..65535 | file-only, restart | UDP media port. `0` = ephemeral. |
| `webrtc.port_max` | int | `0` | 0..65535 | file-only, restart | Top of the media port range. `0` = `webrtc.port + WEBRTC_MAX_SESSIONS - 1` (one port per session slot; `WEBRTC_MAX_SESSIONS` is 4). |
| `webrtc.channel` | int | `0` | **0..1** (`MS_MAX_VSTREAM-1`) | file-only, restart | Which video stream to send. |

WebRTC reuses `http.tls_cert`/`http.tls_key` for the DTLS identity, so a camera
with no usable cert has `/webrtc/whep` disabled outright. Media is H.264 over
SRTP plus G.711 from `audio.codec2` — no Opus, no H.265, no NACK/FEC/congestion
control, no IPv6 or NAT traversal. LAN/VPN only.

---

## 19. `srt.*` (MPEG-TS over SRT)

Requires `USE_SRT` (`BR2_PACKAGE_TIMPS_SRT`, default **n**). Whole section is
**file-only**.

| Key | Type | Default | Range | Apply | Notes |
| --- | --- | --- | --- | --- | --- |
| `srt.enabled` | bool | `0` | — | file-only, restart | Off by default because the SRT listener is **unauthenticated** unless a passphrase is set. |
| `srt.port` | int | `9000` | 1..65535 | file-only, restart | Listener: local bind port. Caller: remote port. |
| `srt.channel` | int | `0` | **unclamped** | file-only, restart | Video stream to serve. Out of range, or a boot-disabled stream, falls back to `0`. |
| `srt.latency_ms` | int | `120` | **unclamped** | file-only, restart | Alias `srt.latency`. SRT receive/peer latency. |
| `srt.mode` | string[16] | `listener` | `listener`, `caller`; anything else → listener (validated in `srt.c`, not by the config table) | file-only, restart | `caller` dials out — for cameras behind NAT or on unreliable uplinks. |
| `srt.host` | string[64] | `""` | — | file-only, restart | Caller mode: remote host/address. |
| `srt.streamid` | string[64] | `""` | — | file-only, restart | Listener: **required** STREAMID. Caller: STREAMID to present. |
| `srt.passphrase` | string[64] | `""` | — | file-only, restart | Optional AES passphrase; `""` = none. |

---

## 20. `sim.*` (host simulator only)

Whole section is **file-only** and only meaningful in a `make sim` / host build
with no Ingenic HAL. Feeds the simulated encoders from files.

| Key | Type | Default | Notes |
| --- | --- | --- | --- |
| `sim.video0` | string[256] | `""` | e.g. `/tmp/ch0.h264` |
| `sim.video1` | string[256] | `""` | e.g. `/tmp/ch1.h264` |
| `sim.audio` | string[256] | `""` | e.g. `/tmp/aud.aac` |
| `sim.jpeg` | string[256] | `""` | e.g. `/tmp/snap.jpg` |

---

## 21. Aliases and legacy spellings accepted by the loader

Both spellings are accepted on the config-file path **and** on the
`POST /control` path (`apply_ctrl_fields()` matches `.alias` too). The canonical
name wins when a body carries both, and what is applied, persisted and echoed is
always the canonical one. `config_write_keys()` replaces an alias line in place
rather than appending the canonical name next to it.

| Legacy / alternate spelling | Canonical key |
| --- | --- |
| `sensor.i2c_address` | `sensor.i2c_addr` |
| `rtsp.username` | `rtsp.user` |
| `rtsp.password` | `rtsp.pass` |
| `rtsp.tls_enabled` | `rtsp.tls` |
| `http.username` | `http.user` |
| `http.password` | `http.pass` |
| `http.tls` | `http.https` |
| `http.cert` | `http.tls_cert` |
| `http.key` | `http.tls_key` |
| `srt.latency` | `srt.latency_ms` |
| `record.segment` | `record.segment_s` |
| `record.pre_roll` | `record.pre_roll_s` |
| `record.post_roll` | `record.post_roll_s` |
| `timelapse.interval` | `timelapse.interval_s` |
| `daynight.total_gain_day_threshold` | `daynight.day_gain` |
| `daynight.total_gain_night_threshold` | `daynight.night_gain` |
| `video<N>.mode` | `video<N>.rc_mode` |
| `video<N>.jpeg_enabled` | `video<N>.jpeg` |
| `osd<S>.<N>.logo_path` | `osd<S>.<N>.logo` |
| `osd<S>.<N>.logo_width` | `osd<S>.<N>.logo_w` |
| `osd<S>.<N>.logo_height` | `osd<S>.<N>.logo_h` |
| `osd<S>.<N>.font_color` | `osd<S>.<N>.color` |
| `osd<S>.<N>.stroke` | `osd<S>.<N>.outline` |
| `osd<S>.<N>.stroke_color` | `osd<S>.<N>.outline_color` |
| `privacy<S>.<N>.width` | `privacy<S>.<N>.w` |
| `privacy<S>.<N>.height` | `privacy<S>.<N>.h` |
| `privacy<S>.<N>.fill_color` | `privacy<S>.<N>.color` |

Other legacy forms that are **not** aliases but are still accepted:

* `osd<N>.<field>` (no stream digit) — the pre-per-stream OSD item form; writes
  item `N` on **every** stream.
* `daynight.mode = sensor` → `auto`; `= time` or `= sun` → `schedule`.
* `video<N>.rotation = 1` → 90, `= 2` → 270 (the raw libimp `rotTo90` enum used
  by prudynt/raptor configs).
* `audio.codec`: `g711u`/`ulaw` = `pcmu`, `g711a`/`alaw` = `pcma`, `hevc` =
  `h265` for `video<N>.codec`, `off` = `none`.
* `record.mode`: `motion` = 1, `continuous` = 0.
* Legacy POST-only forms: a flat top-level `{"brightness":140}` maps to
  `image.*`, and `{"force_mode":"night"|"day"}` maps to `image.running_mode`.
---

## 22. Build-time options (thingino firmware Kconfig)

Source: `/home/lfiebach/thingino-firmware-LuFi/package/timps/Config.in` and
`package/timps/timps.mk`. **Every symbol is `bool`** — there are no string or int
symbols. A config key can be perfectly valid and still do nothing because the
feature behind it was not compiled in, which is the single most common cause of
"I set it and nothing happened".

### 22.1 Master symbol

| Symbol | Prompt | Default | Depends / selects | Effect when off |
| --- | --- | --- | --- | --- |
| `BR2_PACKAGE_TIMPS` | "timps" | *(none → n)* | `depends on !BR2_SOC_FAMILY = "a1"` **and** `BR2_PACKAGE_THINGINO_STREAMER_TIMPS`. Selects `BR2_PACKAGE_INGENIC_LIB` (+ `…_LIBIMP` unless `OPENIMP`, `…_LIBSYSUTILS` unless the neo variant), `BR2_PACKAGE_THINGINO_FONTS`, `…_LOGO`, `…_SEND2`, `…_AGENT`, and `INGENIC_MUSL`/`INGENIC_UCLIBC` per toolchain. | No `timpsd`, no `/etc/timps.conf`, no `S95timps` — the package is absent. |

**The `a1` SoC family is explicitly excluded** by that `depends on`, which is why
`A1` never appears as a platform anywhere in timps.

All symbols in 22.2 are inside `if BR2_PACKAGE_TIMPS`; the two in 22.5 are not.

### 22.2 Feature symbols

Every one maps to `USE_<X>=$(if $(BR2_PACKAGE_TIMPS_<X>),1,0)` in
`TIMPS_BUILD_CMDS`, which the upstream `Makefile` turns into `-DUSE_<X>`.

| Symbol | Default | Depends / selects | Define | Compiles in | Symptom when OFF |
| --- | --- | --- | --- | --- | --- |
| `BR2_PACKAGE_TIMPS_FAAC` | **y** | selects `BR2_PACKAGE_FAAC` | `-DUSE_FAAC` | Software AAC encode | Only G.711 audio. `audio.codec = aac` cannot be satisfied, so the fMP4 browser preview and SRT have no usable audio track. |
| `BR2_PACKAGE_TIMPS_STREAM_OPUS` | **n** | selects `BR2_PACKAGE_OPUS` | `-DUSE_STREAM_OPUS` | RTP/RTSP Opus encoder (RFC 7587), signalled `opus/48000/2` | `opus` is **not an accepted `audio.codec` token** — it falls through the parser to `aac`, silently. |
| `BR2_PACKAGE_TIMPS_CONTROL` | **y** | — | `-DUSE_CONTROL` | `/control` JSON API, `/events` SSE, the token machinery | `/control` and `/events` gone (404); **no WebUI plugin installed at all**; no motors UI. The whole `TIMPS_INSTALL_SEND2` hook drops out, so `send2common`, `telegram-cam-register` and the `send2*` tools are not installed. **`/usr/sbin/timps-motion` still installs unconditionally**, so the motion bridge keeps running and its `record.clip` POST to `/control` fails every time (it logs once per boot and sends the notification without video). `/talk` would 401 any browser (hence `BC_WS`'s dependency). ~15 KB smaller. |
| `BR2_PACKAGE_TIMPS_DAYNIGHT` | **y** | — | `-DUSE_DAYNIGHT` | The detection thread; installs `/usr/sbin/color`, `/usr/sbin/daynight`, `/etc/init.d/S06ircut`; removes daynightd's autostart scripts | No automatic day/night. `daynight.*` keys parse and persist and do nothing. daynightd's scripts are **not** removed. (`/usr/sbin/ircut` and `/usr/sbin/light` install unconditionally.) |
| `BR2_PACKAGE_TIMPS_RECORD` | **y** | — | `-DUSE_RECORD` | fMP4 segment recording **and** the on-demand clip capture used by send2/Telegram motion videos | `GET /control` reports `record.available = 0`, the WebUI hides record controls, Telegram motion *videos* stop working. ~11 KB saved. |
| `BR2_PACKAGE_TIMPS_TIMELAPSE` | **y** | — | `-DUSE_TIMELAPSE` | Periodic JPEG snapshots + pruning | `timelapse.available = 0`, WebUI hides the page. ~4 KB saved. |
| `BR2_PACKAGE_TIMPS_TLS` | **y** | selects `BR2_PACKAGE_MBEDTLS` | `-DUSE_TLS` | `src/tls.c`: HTTPS on the HTTP port + RTSPS. Installs `/usr/bin/generate-timps-tls-certs.sh`. With `BR2_PACKAGE_THINGINO_UHTTPD_TLS=y` it also uncomments `http.https = 1` in the shipped `/etc/timps.conf`. | No HTTPS, no RTSPS. `BR2_PACKAGE_TIMPS_WEBRTC` becomes unselectable. `audio.talk_ws = 1` can never be satisfied. No cert generator, so `S95timps`'s fallback silently does nothing. |
| `BR2_PACKAGE_TIMPS_WEBRTC` | **y** | depends on `TIMPS_TLS` **and** `TIMPS_CONTROL`; selects `BR2_PACKAGE_MBEDTLS_DTLS_SRTP` | `-DUSE_WEBRTC` (forces `USE_TLS`/`USE_CONTROL`) | `POST /webrtc/whep` ICE-lite + DTLS-SRTP, H.264 + G.711, max 4 sessions | `/webrtc/whep` not served (404); the `webrtc.*` keys are unknown. |
| `BR2_PACKAGE_TIMPS_SRT` | **n** *(no `default` line at all)* | selects `BR2_PACKAGE_LIBSRT` | `-DUSE_SRT` | MPEG-TS over SRT | No SRT listener/caller; `srt.*` keys are inert. |
| `BR2_PACKAGE_TIMPS_OSD_HINTING` | **n** | — | `-DUSE_OSD_HINTING` | Geometric OSD-text autohinter in the TTF rasterizer (~2.1 KB) | `osd.hinting` is the textbook "parses but does nothing": accepted, clamped, persisted, echoed, no visual change. `config.c` logs one WARN per session saying so. |
| `BR2_PACKAGE_TIMPS_ROTATE` | **n** | — | `-DUSE_ROTATE` | The whole rotation feature | **Byte-identical build.** `videoN.rotation` coerces every non-zero value to `0`; `caps.rotation` is omitted. |
| `BR2_PACKAGE_TIMPS_SW_ROTATE` | **n** | depends on `TIMPS_ROTATE` | **`-DMS_ENABLE_SW_ROTATE`** (the one symbol whose define does not follow the `USE_*` pattern) | Software NV12 transpose on T23 (`ROT_HAS_SW_90`, gated `PLATFORM_T23 && MS_ENABLE_SW_ROTATE`) | On T23, 90/270 has no path and coerces to `0`. Irrelevant on other SoCs. |
| `BR2_PACKAGE_TIMPS_BACKCHANNEL` | **n** | — | `-DUSE_BACKCHANNEL` | ONVIF audio backchannel via native `IMP_AO` (no external `/bin/iac`); G.711 decode is pure C. Uncomments `audio.backchannel = 1` in the shipped config. | No talk-back; `audio.backchannel` inert; `caps.backchannel.available = 0`. |
| `BR2_PACKAGE_TIMPS_BC_AAC` | **n** | depends on `TIMPS_BACKCHANNEL`; selects `BR2_PACKAGE_LIBHELIX_AAC` | `-DUSE_BC_AAC` (forces `USE_BACKCHANNEL`) | AAC backchannel decode | Backchannel accepts G.711 only; an ONVIF client offering `mpeg4-generic` AAC is rejected. |
| `BR2_PACKAGE_TIMPS_BC_WS` | **y if `BR2_PACKAGE_TIMPS_TLS`**, else n | depends on `TIMPS_BACKCHANNEL` **and** `TIMPS_CONTROL` | `-DUSE_BC_WS` (forces both) | `/talk` WebSocket: browser mic → G.711 in an AudioWorklet → the same `IMP_AO` path (~8 KB, no new library). Uncomments `audio.talk_ws = 1` (only the strict value 1, never 2). | `wss://<cam>:<http_port>/talk` not served; the backchannel is reachable only from an ONVIF client or NVR. |
| `BR2_PACKAGE_TIMPS_PLAY` | **n** | — | `-DUSE_PLAY` | Play-FIFO at `/run/timps/audio_out`, WAV + raw PCM16; installs `/usr/sbin/play` | No `/usr/sbin/play`. The WiFi captive-portal prompts, the post-upgrade chime and the Home Assistant ESPHome media_player/TTS integration are silent no-ops. |
| `BR2_PACKAGE_TIMPS_PLAY_OPUS` | **n** | depends on `TIMPS_PLAY`; selects `BR2_PACKAGE_OPUSFILE` | `-DUSE_PLAY_OPUS` (forces `USE_PLAY`) | Ogg-Opus decode in the play queue | Play queue is WAV/PCM only; thingino's shipped `/usr/share/sounds/*.opus` will not play. |

### 22.3 Audio backchannel preset (a Kconfig `choice`)

Prompt "Audio backchannel preset", default `…_MANUAL`. None of these adds a
define; they only `select` the symbols above.

| Symbol | Selects |
| --- | --- |
| `BR2_PACKAGE_TIMPS_AUDIO_PRESET_MANUAL` *(default)* | nothing |
| `BR2_PACKAGE_TIMPS_AUDIO_PRESET_FULL` | `TIMPS_BACKCHANNEL`, `TIMPS_BC_AAC`, `TIMPS_PLAY`, `TIMPS_PLAY_OPUS`, `THINGINO_SOUNDS`, `THINGINO_SOUNDS_FORMAT_OPUS` (~395 KB of codec libraries — too big for a ~5 MB T20 rootfs) |
| `BR2_PACKAGE_TIMPS_AUDIO_PRESET_MINIMAL` | `TIMPS_BACKCHANNEL`, `TIMPS_PLAY`, `THINGINO_SOUNDS` (no extra codec libraries; confirmed on a ~5 MB T20 rootfs) |

### 22.4 Non-Kconfig build knob

`TIMPS_TRACE` is **deliberately not a Kconfig symbol**. `timps.mk` maps
`USE_TRACE=$(if $(filter 1,$(TIMPS_TRACE)),1,0)` → `-DUSE_TRACE`, which compiles
`src/trace.c`. Arm it only with a make-variable override
(`... TIMPS_TRACE=1 rebuild-timps`). Without it, `general.trace` and
`general.trace_ms` parse and do nothing.

### 22.5 Install-only symbols (outside `if BR2_PACKAGE_TIMPS`)

| Symbol | Default | Effect | When off |
| --- | --- | --- | --- |
| `BR2_PACKAGE_TIMPS_DIAG_TOOLS` | **n** | Installs `timps-dn-isp-log`, `timps-logcat-ship`, `/usr/bin/timps-selftest` (~9 KB of shell). No define. | Those three helpers are absent. Nothing on the camera calls them anyway. **`/usr/bin/timps-irprobe` installs unconditionally**, not under this symbol — so `daynight.irprobe_cmd`'s default works without it. |
| `BR2_PACKAGE_TIMPS_DROP_LIBSTDCPP` | *(none → n)* | A target-finalize hook that readelf-scans every `.so*`/executable for a `NEEDED` on libstdc++ and removes `usr/lib/libstdc++.so*` (2130 KB) only if nothing links it. | libstdc++ stays in the image. **Known gotcha:** the `.mk` block implementing it is nested inside the `BR2_PACKAGE_THINGINO_WEBUI` + `BR2_PACKAGE_TIMPS_CONTROL` guard, so on a build without the WebUI or with `CONTROL=n` the option is settable in menuconfig and silently does nothing. |

### 22.6 Other compile flags

Non-`USE_*` defines added by `timps.mk`:
`-D_GNU_SOURCE` (always), `-DPLATFORM_<SOC_FAMILY uppercased>` (always),
`-DKERNEL_VERSION_4` (kernel 4.4.94), `-DLIBC_GLIBC` / `-DLIBC_UCLIBC` per
toolchain (there is **no** `-DLIBC_MUSL`). The upstream `Makefile` adds
`-DHAL_INGENIC`, `-DPLATFORM_$(PLATFORM)`, the per-platform `PLATFORM_CFLAGS`
and `-DMS_VERSION='"<version>"'` — the last of which is what `GET /control`
reports as `version`, derived from `git describe` when an override srcdir is
set, so it catches stale-build drift.

**Build hardening is dropped by the package.** The upstream `Makefile` defines
`CFLAGS ?=`/`LDFLAGS ?=` including `HARDEN_CFLAGS`/`HARDEN_LDFLAGS`
(`-fstack-protector-strong`, `-D_FORTIFY_SOURCE=2`, `-Wl,-z,relro -Wl,-z,now
-Wl,-z,noexecstack`, gated by `HARDEN=1`/`FORTIFY=1`). `timps.mk` passes
`CFLAGS="$(TIMPS_CFLAGS)"` and `LDFLAGS="$(TARGET_LDFLAGS) …"` **explicitly** on
the make command line, which overrides a `?=` default outright — so none of
those hardening flags reach a Buildroot build unless Buildroot's own
`TARGET_CFLAGS`/`TARGET_LDFLAGS` already carry them (via `BR2_SSP_*`,
`BR2_FORTIFY_SOURCE_*`, `BR2_RELRO_*`). **(unverified — whether this firmware's
toolchain config actually supplies them was not checked; read the
`BR2_SSP_`/`BR2_RELRO_`/`BR2_FORTIFY_SOURCE_` symbols in the generated
`.config`.)** `HARDEN=0`/`FORTIFY=0` are therefore not the lever here; the
Buildroot-side symbols are.

Implications the upstream `Makefile` applies no matter what the package passes:
`USE_SW_ROTATE=1 ⇒ USE_ROTATE`; `USE_BC_AAC=1 ⇒ USE_BACKCHANNEL`;
`USE_PLAY_OPUS=1 ⇒ USE_PLAY`; `USE_BC_WS=1 ⇒ USE_BACKCHANNEL + USE_CONTROL`;
`USE_WEBRTC=1 ⇒ USE_TLS + USE_CONTROL`;
`USE_BACKCHANNEL=1 or USE_PLAY=1 ⇒ USE_AUDIO_OUT`.

### 22.7 What the firmware does to `/etc/timps.conf`

* **At build time** `TIMPS_INSTALL_TARGET_CMDS` runs `sed` over the shipped
  `package/timps/files/timps.conf`: it sets `sensor.model` from
  `BR2_SENSOR_1_NAME`, sets `sensor.i2c_addr = 0x31` for `gc5603`, and
  **uncomments** `http.https = 1` (if `TIMPS_TLS` + `THINGINO_UHTTPD_TLS`),
  `audio.backchannel = 1` (if `TIMPS_BACKCHANNEL`) and `audio.talk_ws = 1` (if
  `TIMPS_BC_WS`). `webrtc.enabled` is deliberately left commented and relies on
  the binary's own default of `2`.
* The shipped template also sets `motion.on_motion = /usr/sbin/timps-motion`,
  which is why that script installs unconditionally.
* **At boot, `S95timps` does not create or rewrite `/etc/timps.conf` at all.**
  It only *reads* `http.https`, `rtsp.tls`, `webrtc.enabled`, `http.tls_cert`
  and `http.tls_key` inside `ensure_tls_certs()`. It cannot repair a deleted
  config file.

### 22.8 `S95timps` behaviour

* Config path is hardcoded: `CONF=/etc/timps.conf`, passed as
  `timpsd -c /etc/timps.conf`. PID file `/var/run/timpsd.pid`.
* Commands: **`start | stop | restart | status`**. There is **no `reload`** —
  live reconfiguration is `/control`'s job, and anything restart-required needs
  a full `S95timps restart`.
* `stop` and `restart` wait for the old process to go (`wait_stop()`), because
  IMP/rmem teardown takes a while and starting a new instance too early makes
  ISP init fail. The loop is **50 iterations of
  `usleep 100000 2>/dev/null || sleep 1`** — so ~5 s where busybox `usleep`
  exists, and **50 s where it does not**.
* **`stop` deletes the pidfile unconditionally**, even when `wait_stop` timed
  out and printed `timpsd still stopping...`. `restart`'s second `wait_stop`
  then returns immediately (no pidfile = "gone"), so `start` can land on top of
  a still-exiting instance and the singleton `flock` becomes the only guard —
  and it refuses the *new* process, leaving the camera dark. See
  `docs/ai/troubleshooting.md` §11.6.
* `ensure_tls_certs()` runs before `start`, in this order:
  1. Proceed only if `http.https` is `1`/`2`/`true`/`yes`/`on`, **or**
     `rtsp.tls` is exactly `1`, **or** `webrtc.enabled` is `1` or `2`. (Note the
     asymmetry: `http.https` accepts word forms, `rtsp.tls` is an exact `"1"`
     string compare.)
  2. If either `http.tls_cert` or `http.tls_key` is empty, do nothing.
  3. If both files already exist and are non-empty, do nothing — an
     operator-supplied cert is never second-guessed.
  4. Otherwise, if both configured paths differ from the uhttpd pair and
     `/etc/ssl/certs/uhttpd.crt` + `/etc/ssl/private/uhttpd.key` exist,
     **symlink** the configured paths to them (so `:443` and the preview port
     present the same certificate and one browser trust decision covers both).
     A symlink, not a copy, because `S02ssl` regenerates uhttpd's cert whenever
     it is missing and a copy would go stale.
     **If either `ln -sf` fails**, *both* paths are removed again (a half-made
     link would leave timpsd with a cert and no key), `logger -t timps` records
     `TLS: could not link /etc/ssl/certs/uhttpd.crt, generating our own`, and
     the step falls through to 5.
  5. Otherwise run `/usr/bin/generate-timps-tls-certs.sh <crt> <key>` if it is
     installed (it is only installed by `BR2_PACKAGE_TIMPS_TLS`); if it is not
     installed the function simply returns and timpsd starts with no cert. A
     generator failure logs `TLS cert generation failed: <output>`, again via
     `logger -t timps` — so these two lines live in `logread`, not in timpsd's
     own log stream.

### 22.9 Shipped configuration vs compiled defaults

`package/timps/files/timps.conf` is **not** a copy of `config_defaults()`, and
it is **not** `timps.conf.example` either. Three different files, three
different values for several keys. What the shipped template changes:

| Key | Shipped `/etc/timps.conf` | Compiled default | Why it matters |
| --- | --- | --- | --- |
| `general.osd_pool_size` | `1000` | `1024` | Harmless, but a `GET /control` read-back of 1000 is the file, not a clamp. |
| `general.debug_modules` | `daynight` | `""` | **DAYNIGHT logs at DEBUG on every shipped camera.** With `general.loglevel = 2` this is the baseline log volume an operator sees — the per-probe day/night lines are not a leftover from someone debugging. |
| `rtsp.user` / `rtsp.pass` | `thingino` / `thingino` | `""` / `""` (= open) | The "empty credentials" behaviour described for `rtsp.*`/`http.*` is **not** the shipped state. |
| `http.user` / `http.pass` | `thingino` / `thingino` | `""` / `""` | Same. Default credentials, fleet-wide. |
| `sensor.model`, `sensor.i2c_addr` | `gc2053` / `0x37`, then `sed`-adapted per board | *(unset → autodetect)* | `TIMPS_INSTALL_TARGET_CMDS` rewrites `sensor.model` from `BR2_SENSOR_1_NAME`, and forces `i2c_addr = 0x31` for `gc5603`. |
| `motion.on_motion` | `/usr/sbin/timps-motion` | `""` | Which is why that script installs unconditionally (§22.2). |
| `http.https`, `audio.backchannel`, `audio.talk_ws` | commented, **uncommented by the build** when the matching Kconfig symbol is on | `0`, `0`, `0` | `http.https = 1` needs `TIMPS_TLS` **and** `THINGINO_UHTTPD_TLS`; `audio.talk_ws` is only ever uncommented as `1`, never `2`. |
| `record.enabled` | `0`, active line | `0` | Same value, but present in the file. |
| `srt.enabled` | `0` | `0` | — |

And where the repo's own `timps.conf.example` disagrees with the shipped
template (it is a documentation file, not something installed — see §14 #2 of
the troubleshooting doc):

| Key | `timps.conf.example` | Shipped template |
| --- | --- | --- |
| `srt.enabled` | `1`, active (with a comment saying the compiled default is 0) | `0` |
| `general.osd_pool_size` | `1024` | `1000` |
| `general.debug_modules` | commented out | `daynight`, active |
| `rtsp.user` / `http.user` | active but **empty** | `thingino` |
| `audio.talk_ws` | `0`, active | commented `= 1` (uncommented by the build) |
| `record.enabled` | commented out | `0`, active |
| `motion.on_motion` | commented `/etc/scripts/on_motion.sh` | `/usr/sbin/timps-motion`, active |
| `http.adaptive_drop`, `motion.skip_frames` | present | absent |
| `sensor.fps/width/height` | active | commented out |
| `jpeg.*`, `video*.*`, `osd*.*`, `daynight.*`, `timelapse.*`, `privacy*.*` | fully enumerated | absent — the shipped template relies on the compiled defaults |

**Practical consequence:** never answer "what is this camera set to?" from
either file. Read `GET /control`.

### 22.10 Other conditional installs

Beyond the feature symbols in §22.2, these files land (or do not) on rules of
their own, all in `timps.mk`:

| File | Rule |
| --- | --- |
| `/usr/libexec/agent/adapter.sh` (from `files/agent-adapter`) | **Unconditional**, inside `TIMPS_INSTALL_TARGET_CMDS`; overwrites the null fallback thingino-agent installs. |
| `/etc/init.d/S48webui-config` | `TIMPS_INSTALL_WEBUI_CONFIG_FIX`, only when `BR2_PACKAGE_THINGINO_WEBUI=y`. |
| `/etc/init.d/S96onvif_discovery` | `TIMPS_INSTALL_ONVIF_DISCOVERY`, a **target-finalize hook that probes the target** — it installs only if `usr/sbin/wsd_simple_server`, `var/www/onvif/onvif.cgi` or an existing `S96onvif_discovery` is present. It is deliberately **not** gated on `BR2_PACKAGE_THINGINO_ONVIF`, and on an ONVIF-free image nothing is shipped. |
| `rm -f /etc/init.d/*daynightd /etc/init.d/*dusk2dawn` | `TIMPS_DISABLE_DAYNIGHTD`, only when `BR2_PACKAGE_TIMPS_DAYNIGHT=y`. A glob, because a 2.0.0 rename (`S97daynightd` → `S10daynightd`) broke the old literal name. |
| Stock-WebUI purge | `TIMPS_PURGE_STOCK_WEBUI`, a finalize hook appended inside the `THINGINO_WEBUI` + `TIMPS_CONTROL` guard. |
| Five forked WebUI files re-applied | `TIMPS_REAPPLY_WEBUI_OVERLAY`, **prepended** to `TARGET_FINALIZE_HOOKS` so it wins the per-package merge. |
| motors UI (`config-motors.html`, `json-motor*.cgi`, …) | `TIMPS_REAPPLY_MOTORS_UI`, only when `BR2_PACKAGE_THINGINO_MOTORS` **and** `BR2_PACKAGE_THINGINO_STREAMER_TIMPS` are both `y`; `json-motor-token.cgi` additionally needs `BR2_PACKAGE_THINGINO_MOTORS_WS=y`. |
| `/usr/sbin/{daynight,ircut,light}`, `S06ircut` | `TIMPS_INSTALL_DAYNIGHT_SCRIPTS`, from the **daynightd package's** `files/`; `ircut` and `light` unconditionally, `daynight` and `S06ircut` under `BR2_PACKAGE_TIMPS_DAYNIGHT`. |

One documentation disagreement worth knowing: the upstream `Makefile`'s comment
next to `USE_WEBRTC` says the runtime key is "gated at runtime by
`webrtc.enabled` in timps.conf (default 0)". `Config.in` and
`config_defaults()` both say **2**. The code wins.

---

## 23. Coverage check

The key inventory in this file was verified mechanically, not by hand. A script
(`/tmp/timps-keycheck.py`, outside the repo) parses `src/config.c` directly:

1. It extracts every `static const cfg_field <name>_fields[] = { … }` table and
   every `F(...)` / `FS(...)` entry inside it (name + alias), mapping each table
   to its section prefix from `g_sections[]` — including the three indexed
   tables (`video_fields` → `video<N>.`, `osd_item_fields` → `osd<S>.<N>.`,
   `privacy_fields` → `privacy<S>.<N>.`).
2. It adds the keys handled by explicit code in `set_kv()` outside the tables:
   `general.syslog`, `general.trace`, `general.trace_ms`, the four deprecated
   `motion.roi_*`, and the retired/now-constant `daynight.*` names extracted
   from the `gone[]`, `gone_i[]` and `gone_f[]` arrays.
3. It extracts every `key = value` line from `timps.conf.example`, including
   commented-out ones.
4. It checks each key appears literally in this file, or matches a documented
   index pattern (`video<N>`/`videoN`/`video0`, `osd<S>.<N>`/`osdS.N`/`osd0.0`,
   `privacy<S>.<N>` …, or a leaf name in that section's table).

**Result: 225 canonical config keys documented, 0 missing.** The breakdown:

* **199** from the `cfg_field` tables — `sensor` 5, `image` 23, `audio` 24,
  `jpeg` 7, `rtsp` 7, `http` 11, `webrtc` 4, `events` 3, `general` 4, `sim` 4,
  `srt` 8, `osd` 6, `motion` 9, `record` 10, `timelapse` 6, `daynight` 23,
  `video<N>` 25, `osd<S>.<N>` 14, `privacy<S>.<N>` 6.
* **7** handled by explicit code in `set_kv()` outside the tables:
  `general.syslog`, `general.trace`, `general.trace_ms`, `motion.roi_x/y/w/h`.
* **19** retired or now-constant `daynight.*` names that the loader still
  recognises and warns about (11 removed, 8 turned into fixed constants).

Plus **27 aliases** (§21), all present, and **244 key occurrences extracted from
`timps.conf.example`** (active and commented out), all covered — 0 missing.

Counting notes:
* Indexed keys are counted **once per pattern**, not per index: the 25
  `video<N>.*` keys, 14 `osd<S>.<N>.*` keys and 6 `privacy<S>.<N>.*` keys are
  one entry each. Expanded per index the file would describe
  25×2 + 14×(2 streams × 8 items + 8 legacy) + 6×(2×4) = far more lines for no
  extra information.
* `webrtc.*` (4 keys) only exists in a `USE_WEBRTC` build; it is counted because
  the table exists in the source.
* Limits used for the index ranges, all from `src/config.h`:
  `MS_MAX_VSTREAM = 2`, `MS_MAX_OSD = 8`, `MS_MAX_PRIVACY = 4`,
  `MS_MAX_STR = 64`; and from `src/motion_caps.h`, `MOTION_MAX_CELLS` =
  `IMP_IVS_MOVE_MAX_ROI_CNT` (52, or 4 on the T10/T20 3.9.0 SDK).

---

## 24. Per-SoC differences that change what a key does

The `*_caps.h` matrices in §4, §5 and §5's `video_live` table cover which keys
apply. These are the *behavioural* differences behind them — the ones that turn
into support questions.

| SoC(s) | Difference | Where |
| --- | --- | --- |
| **T40, T41** | `ISP_NEW_TUNING_API`: no `GetTotalGain`, no `ISP_HAS_EXPR` (no `GetExpr`/integration-time readback), and no `ISP_HAS_AELUMA`. `hal_isp_total_gain()` and `hal_isp_ae_luma()` return −1, so **day/night runs on the `/proc` scrape alone** — no IMP cross-check, and the exposure index degrades to whatever the dump publishes. | `src/isp_caps.h`, `src/hal/hal_ingenic.c` |
| **T21, T23, T31, C100** | The only SoCs with `IMP_ISP_Tuning_GetAeLuma` (`ISP_HAS_AELUMA`), day/night's secondary photosensing metric. | `src/isp_caps.h` |
| **T20** (old SDK) | The ISP dump is `/proc/jz/isp/isp_info`, not `isp-m0` — `isp-m0` does not exist in that SDK and never will. `daynight.c` carries an explicit fallback list, so `daynight.isp_path` does not have to be changed, but the `is not readable, using … instead` warning is expected there. | `src/daynight.c` |
| **T10** | `jpeg.quality` / `videoN.jpeg_quality` are **never applied** — custom quantization tables are known to degrade JPEG quality on T10, so the SDK default is kept and one WARN is logged. | `src/hal/hal_ingenic.c` |
| **T10, T20, T21, T30** | No `ISP_HAS_SENSOR_ATTR` (`GetSensorAttr`), so the framesource input geometry comes **only** from `sensor.width`/`sensor.height`. A sensor driver that reports `0x0` cannot be compensated for there. | `src/isp_caps.h` |
| **T31 only** | `encoder.<n>.ave_bitrate` appears in `GET /control?stats=1` (`IMP_Encoder_GetChnAveBitrate`). Elsewhere the key is simply absent, not zero. | `src/hal/hal.h`, `src/control.c` |
| **T41** | No `IMP_Encoder_SetChnAttrRcMode` at all (`ENC_HAS_SETRCMODE` undefined) — only `bitrate` and the QP bounds can be touched live. T40 **does** attempt the live call. | `src/hal/hal_ingenic.c` |
| **T40, T41** | Flips go through `IMP_ISP_Tuning_SetHVFLIP` instead of `SetISPHflip`/`SetISPVflip`, and `g_sensor.rst_gpio`/`pwdn_gpio`/`power_gpio` are forced to `-1` (the fields are plain `int` in that SDK). | `src/hal/hal_ingenic.c` |
| **T31/C100 vs T40/T41** | `ENC_HAS_QPIPDELTA` (hence a working `videoN.i_bias_lvl`) is T31/C100 only. | `src/hal/hal_ingenic.c` |
| **T31** | The `isp_ch0_pre_dequeue_time` one-buffer gate on framechan0 — see `videoN.buffers` in §5. | `src/hal/hal_ingenic.c` |
