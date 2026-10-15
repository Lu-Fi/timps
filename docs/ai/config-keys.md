# timps configuration keys — complete reference

**Applies to timps v1.10.2 (draft, source: `dev/1.10.2`, 2026-10-12).**

A statement marked **since vX** is new in that release — on an older camera
the *previous* behaviour is the one to describe. Changes in the
`[Unreleased]` 1.10.2 draft of `CHANGELOG.md` are marked **since v1.10.2**.

Authoritative source: `src/config.c` (the `cfg_field` tables and
`config_defaults()`), `src/config.h` (struct field sizes and doctrine),
`src/control.c` (what is reachable over HTTP), `src/hal/hal_ingenic.c`
(`ing_control()` — what applies live), and `src/*_caps.h` (platform gating).
Where an existing doc contradicts the code, the code wins here.

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
  (they are not truncated into a bogus second key). **since v1.9.28** a line with an
  embedded NUL byte is dropped the same way, and both warnings name only the
  key, never a prefix of the (possibly secret) value.
* Unknown keys log `unknown key <name>` at WARN and are ignored. **since v1.9.28** that
  includes a leftover `osd.vars_file` line (§8).
* **Later lines win.** A duplicate key later in the file overrides the earlier
  one.
* Integer keys are decimal; hex needs an explicit `0x` (`0x37`). A leading
  zero is just decimal (`08` = 8, `010` = 10; up to v1.9.32 `strtol` base 0
  read these as octal). Hex-typed keys (colours, `0xAARRGGBB`) keep the C
  notation (`strtoul` base 0).
* Booleans: `1`, `true`, `on`, `yes` are true (case-insensitive). Anything else
  is false — including typos. Two keys warn loudly about this (see `F_SECVAL`
  below).

The file path is `/etc/timps.conf` unless `timpsd -c <path>` says otherwise
(`src/main.c`). The other arguments: `-v` (DEBUG logging, **not** the
version), `-h`, and **since v1.10.0** `-V`/`--version` and
`--check-libs` (checks only whether the loaded libimp fits the build, reads no
config; exit 0, 78 flavour mismatch, 79 missing IMP function, 80 could not
check — see
`docs/ai/troubleshooting.md` §1.11). Any other argument exits 2.

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
| **capability-gated** | The HAL only issues the IMP call where the `*_caps.h` matrix says the SDK has it. Elsewhere a `POST /control` does not apply and does **not persist** the key (reply `"unsupported"`, 422 `not_supported_on_soc` if nothing else was accepted); a value already in the config file is read and silently skipped. `caps.image`/`caps.audio` list the capabilities. |

`F_CTRL` is a per-field **security allowlist**, not a default. Unreachable over
HTTP, verified against `control_apply_json()` (`src/control.c`) and the
`cfg_field` tables:

* **Whole sections `control_apply_json()` never walks:** `http.*`, `rtsp.*`,
  `jpeg.*`, `events.*`, `webrtc.*`, `srt.*`, `sim.*`. The POST surface only
  knows `image`, `audio`, `general`, `daynight`, `osd`/`osd<S>`, `video`,
  `privacy`, `roi` (**since v1.10.0**), `sensor`, `motion`, `record`,
  `timelapse`.
* **Individual keys inside a walked section that carry no `F_CTRL`:**
  `general.loglevel`, `general.imp_polling_timeout`, `general.osd_pool_size`,
  `general.fs_keepalive` (only `general.debug_modules` is POST-able);
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
  A failed config write (read-only or full flash) is counted there too, and the
  next POST, even of the same value, retries it.
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
* The write is atomic (`mkstemp` + `fsync` + `rename` + directory `fsync`).
  **since v1.9.28** the rewritten file keeps the old file's mode (a hand-applied
  `chmod 600` survives) and a newly created one is `0600`; before, every
  rewrite forced 0644. A read error on the old file aborts the rewrite entirely rather
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
| `general.fs_keepalive` | int or word | `-1` | -1..1 | file-only, restart | **since v1.10.0.** `-1` or `auto`, `0` or `off`/`false`/`no`, `1` or `on`/`true`/`yes`; numbers outside -1..1 are clamped, anything else logs a warning and stays auto (`-1`). On: `ing_start()` holds every video FrameSource (and the dedicated `jpeg.*` one) enabled until the daemon stops: no FrameSource is disabled at runtime except at start/stop (and on a kept channel whose `EnableChn` failed, which is retried); encoders stay on-demand. **since v1.10.1** this includes the video watchdog, which only restarts the encoder. **Auto resolves to on only for `USE_OPENIMP` builds on T23 and T41** (the stacks where a FrameSource disable/enable while the sensor streams can hang the SoC), off on every vendor-libimp build and on T10/T20/T21/T30/T31/T40 with OpenIMP. The resolved value is `caps.framesource.keepalive` in `GET /control` (`caps.framesource.cfg` is the raw value) and the start-up line `framesource keepalive on|off (general.fs_keepalive=N[, auto])`. See pitfalls below and `dev_notes/FS_KEEPALIVE_2026-10.md`. |
| `general.syslog` | bool | `1` | — | file-only, restart | **Not a stored field** — handled as a side effect in `set_kv()` (`log_set_syslog()`). Never appears in `GET /control` and cannot be POSTed. `0` = stdout only, no `logread`. |
| `general.trace` | int (bitmask) | `0` | unclamped | file-only, restart | Developer send-pipeline tracing. Bits: `1` per-AU gap/age/send, `2` per-write timing, `4` fanqueue backlog, `8` periodic summary, `15` = all. **Only does anything in a `USE_TRACE=1` build**; parsed and ignored otherwise. Side-effecting key like `general.syslog`: no table entry, no `F_CTRL`, not in `?fields=1`. |
| `general.trace_ms` | int | `250` | unclamped | file-only, restart | Report threshold in ms for `general.trace`. Takes effect in file order, so list it **before** `general.trace` if you want the startup banner to print the right threshold (cosmetic only). |

Pitfalls
* `general.loglevel = 3` on a flash-backed syslog camera grows the log fast; use
  `general.debug_modules` for one subsystem instead.
* `general.trace` on a normal firmware build is accepted, persisted and inert —
  there is no warning.
* **`general.fs_keepalive` words.** `auto`, `on`/`true`/`yes` and
  `off`/`false`/`no` are understood (case-insensitive). Any other word logs
  `general.fs_keepalive = '<value>' is not a valid value, using -1 (auto)` and
  keeps the auto default. Development builds between 1.9.33 and this change
  read every word as `0` (off). Check `caps.framesource` after a restart.
* **What `general.fs_keepalive = 1` costs** (why auto stays off outside
  open-stack T23/T41): the FrameSource pools are held permanently instead of
  only while someone watches — about 6.3 MB rmem for a 1080p FS0 (2 × 3 133 440 B,
  from a T23 log) plus ~0.7 MB for a 640×360 FS1 (estimate). The **peak** rmem
  is unchanged (a camera with a client on every stream holds the same pools).
  CPU, OSD blending, DDR bandwidth, heat and power of a FrameSource that runs
  without an encoder consumer are **unmeasured** on every SoC. On T10/T20/T21/
  T30/T31/T40 and on vendor builds `1` is an explicit opt-in on that basis.
* **What is still on-demand with the keepalive on:** the encoders
  (`video chnN idle` still appears 2 s after the last client), and
  SW-rotate streams (T23 `USE_SW_ROTATE`, 90/270), which are exempt — the log
  says `framesource N: no keepalive (software-rotate stream)`.
* **Remaining runtime FrameSource edges with the keepalive on:** none
  **since v1.10.1** (v1.10.0: the video watchdog's recovery cycle). The video
  watchdog, the JPEG cold-start recycle and the JPEG miss watchdog restart
  only the encoder (`chnN: encoder dead after N consecutive misses -
  restarting the encoder (framesource kept alive) to recover`,
  `recycling the encoder (framesource kept alive)`); after 5 such cycles
  without a frame the video watchdog escalates as before (exit, one-shot
  recovery reboot). Start and stop still enable/disable each kept channel once.
* **Turning it off on T23/T41** (`general.fs_keepalive = 0`) restores the
  on-demand FrameSources, with every timelapse shot, snapshot and day/night kick
  waking chn0 again — the configuration that produced the hangs.

---

## 3. `sensor.*`

The first five keys are `F_CTRL` (POST-able) but **restart** — they are consumed at
the next ISP init only. `ing_control()` logs `persisted, applies on restart` and
the POST reply lists them under `deferred`. The six board-wiring keys below
them (**since v1.10.0**) are **file-only** (no `F_CTRL`: a wrong bus
or pin over HTTP would crash-loop the ISP) and restart-applied; GET `/control`
shows them read-only in the `sensor` object.

| Key | Type | Default | Range | Apply | Notes |
| --- | --- | --- | --- | --- | --- |
| `sensor.model` | string[64] | *(unset)* → autodetect | — | restart | Sensor driver name, e.g. `gc2053`. |
| `sensor.i2c_addr` | int | *(unset)* → autodetect | 0..0x7F | restart | Alias `sensor.i2c_address`. Hex (`0x37`) accepted. |
| `sensor.fps` | int | *(unset)* → autodetect, **capped** | 0..120 | restart | `0` = auto. **since v1.9.19:** the autodetected value is capped at **30** (`SENSOR_AUTO_FPS_CAP` in `src/config.c`); **since v1.9.20** the cap is raised to the fastest *enabled* `videoN.fps` when that is higher, so a stream configured above 30 still gets its frames. An explicit value is used as given. Before v1.9.19 the driver's `max_fps` was taken verbatim. |
| `sensor.width` | int | *(unset)* → autodetect | 0..8192 | restart | `0` = auto. On T10/T20/T21/T30 the driver's `/proc/jz/sensor` size is often empty until the sensor runs; since v1.10.0 the HAL reads it again after `IMP_ISP_EnableSensor` (before the first channel) instead of keeping video0's size as the sensor size. |
| `sensor.height` | int | *(unset)* → autodetect | 0..8192 | restart | `0` = auto. |
| `sensor.i2c_adapter` | int | `-1` (unset) | -1..15 | file-only, restart | **T40/T41 only.** `IMPSensorInfo.i2c.i2c_adapter_id`. Alias `sensor.i2c_adapter_id`. Unset leaves libimp's `0`. |
| `sensor.mclk` | int | `-1` (unset) | -1..2 | file-only, restart | **T40/T41 only.** `IMPSensorMclk`: `0`=MCLK0, `1`=MCLK1, `2`=MCLK2. Unset leaves `0`. |
| `sensor.video_interface` | int | `-1` (unset) | -1..2 | file-only, restart | **T40/T41 only.** `IMPSensorVinType`: `0`=MIPI CSI0, `1`=MIPI CSI1 (T40 only: the T41 header has no CSI1, a `1` there is dropped to `-1` with one WARN), `2`=DVP. Unset leaves `0`. |
| `sensor.default_boot` | int | `-1` (unset) | -1..15 | file-only, restart | **T40/T41 only.** Driver-specific boot setting index (`default_boot`). Unset leaves `0`. |
| `sensor.rst_gpio` | int | `-1` (none) | -1..255 | file-only, restart | **T40/T41 only.** Sensor reset pin (`GPIO_PC(27)` = `2*32+27` = `91`). `-1`/unset keeps today's forced "no pin". |
| `sensor.pwdn_gpio` | int | `-1` (none) | -1..255 | file-only, restart | **T40/T41 only.** Sensor power-down pin, as `rst_gpio`. |

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

**Board wiring (T40/T41).** `isp_init()` fills `IMPSensorInfo` with only
name, I2C type/address and (T40/T41) `rst/pwdn/power_gpio = -1`; everything
else stays `0` (adapter 0, MCLK0, CSI0, boot 0). A board that wires the sensor
elsewhere fails with `IMP_ISP_AddSensor failed` and a kernel line like
`chip found @ 0x30 (i2c0) is not an sc830ai chip`. Set the wiring keys from
the board's thingino `BR2_PACKAGE_THINGINO_RAPTOR_CONF_SENSOR0_*` values. A
value that is not a number or is out of range is logged and dropped (stays
unset), never clamped. On other SoCs the keys are parsed but ignored with one
`WARN` at ISP init. The applied values are logged as
`sensor bus: i2cA mclkM vin=V boot=B rst=R pwdn=P`.

Example, Eufy S350 (T8416, T40XP, SC830AI 3840x2160 @20):

```
sensor.model = sc830ai
sensor.i2c_addr = 0x30
sensor.i2c_adapter = 1
sensor.mclk = 1
sensor.video_interface = 0
sensor.default_boot = 4
sensor.rst_gpio = 91
sensor.pwdn_gpio = -1
sensor.width = 3840
sensor.height = 2160
sensor.fps = 20
```

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
whose `ISP_HAS_*` macro is undefined on this SoC is not applied and **not
persisted** by `POST /control`: the reply lists it in `"unsupported"`, and a
body carrying only such keys answers **422** `not_supported_on_soc` with
`"ok":false`. A value for it in `/etc/timps.conf` is still read, and at start
the HAL skips it with `image.<k> unsupported on this platform (skipped)` — so
a config that moves back to a vendor libimp keeps working. That line is a
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
| `image.anti_flicker` | int | `1` (**since v1.10.0**; `2` up to v1.9.33) | 0..2 | all | `0` off, `1` 50 Hz, `2` 60 Hz. Live. Pick the mains frequency: `1` in Europe, most of Asia and Africa, Australia; `2` in most of the Americas, South Korea, Taiwan, the Philippines, Saudi Arabia, western Japan. The wrong value lets the AE step the exposure in units of the wrong half-period, which shows as horizontal bands under LED/fluorescent light (a T23 sc2336p on 50 Hz mains: row ripple 1.15 % RMS at `2`, 0.09 % at `1`). The thingino template carries the key explicitly (`2` up to v1.9.33, `1` since v1.10.0), so an upgraded camera whose `/etc/timps.conf` was ever written (a persisted `POST /control`, a hand edit: the file then lives in the overlay) keeps `2`; a factory reset, a full flash or a never-written file gets `1`. See troubleshooting §3.1. |
| `image.ae_compensation` | int | `128` | 0..255 | T10 T20 T23 T30 T31 C100, T41 with the open libimp | `ISP_HAS_AECOMP` — **absent on T21** and on T40; on T41 only with OpenIMP, **since v1.9.33** (`AeScenceAttr.AeTargetComp`; measured mean luma 8 / 57 / 98 at 40 / 128 / 230), unsupported with the vendor libimp. |
| `image.max_again` | int | `160` | 0..255 | T10 T20 T21 T23 T30 T31 C100 | `ISP_HAS_GAINS`; absent on T40/T41. |
| `image.max_dgain` | int | `80` | 0..255 | T10 T20 T21 T23 T30 T31 C100 | `ISP_HAS_GAINS`. |
| `image.sinter_strength` | int | `128` | 0..255 | T10 T20 T21 T23 T30 T31 C100, T41 with the open libimp | `ISP_HAS_NR`, spatial NR. T10/T20/T21 with the vendor libimp: accepted but no effect (the vendor firmware renormalises it); acts with the open libimp (OpenIMP; 128 = the vendor picture). **T41**: only with OpenIMP, **since v1.9.33** (Module_Ratio entry 0; image noise 488 / 351 / 216 at 0 / 128 / 255); not on T40. |
| `image.temper_strength` | int | `128` | 0..255 | T10 T20 T21 T23 T30 T31 C100, T41 with the open libimp | `ISP_HAS_NR`, temporal NR. Same OpenIMP note as `sinter_strength`. **T41**: only with OpenIMP, **since v1.10.0** (Module_Ratio entry 1; v1.9.33 left it unsupported); temporal denoise, visible only at high gain (frame-to-frame noise 9.8 at 0 vs 2.5 at 128), saturates above ~128; independent of `sinter_strength`. |
| `image.dpc_strength` | int | `128` | 0..255 | T23 T31 C100; with `USE_OPENIMP` also T10 T20 T21 | `ISP_HAS_DPC`. |
| `image.defog_strength` | int | `128` | 0..255 | T23 T31 C100; with `USE_OPENIMP` also T20 | `ISP_HAS_DEFOG`. On T20 (open stack) it is an Iridix floor, not a defog block. |
| `image.drc_strength` | int | `128` | 0..255 | T21 T23 T31 C100; with `USE_OPENIMP` also T20 | `ISP_HAS_DRC` (WDR). |
| `image.highlight_depress` | int | `0` | **0..10** | T10 T20 T21 T23 T30 T31 C100 | `ISP_HAS_HILIGHT`. Note the 0..10 domain, not 0..255. |
| `image.backlight_compensation` | int | `0` | **0..10** | T23 T31 C100 | `ISP_HAS_BACKLIGHT`. |
| `image.colorfx` | int | `0` | 0..9 (0 none, 1 B/W, 2 sepia, 3 negative, 9 vivid) | only with `USE_OPENIMP`: T10 T20 T21 T23 T31 | `ISP_HAS_COLORFX`. The vendor SDKs expose the call on some SoCs but their kernels ignore it. The open driver answers `EINVAL` for values it does not know (sepia only on T20): the HAL logs it, the value stays persisted. Not in `caps.image` on vendor builds. |
| `image.scene` | int | `0` | 0..14 (SDK `IMPISPSceneMode`: 0 auto, 2..9, 11..14; 1 and 10 invalid) | only with `USE_OPENIMP`: T10 T20 T21 T23 T31 | `ISP_HAS_SCENE`. Same notes as `image.colorfx`. |
| `image.core_wb_mode` | int | `0` | 0..9 (0..8 on T10/T20/T30) | T10 T20 T21 T23 T30 T31 C100 | `ISP_HAS_WB`. The SDK's `isp_core_wb_mode`: `0` auto, `1` manual (then `wb_rgain`/`wb_bgain` apply), `2` daylight, `3` cloudy, `4` incandescent, `5` fluorescent, `6` twilight, `7` shade, `8` warm fluorescent, `9` custom (T21/T23/T31/C100 only). Before v1.9.30 the range was 0..1, so `2..9` came back as `1`. |
| `image.wb_rgain` | int | `0` | 0..255 (T10/T20/T30, Q7: 128 = 1.0), 0..4095 (T21/T23/T31, Q8: 256 = 1.0), 0..65535 elsewhere | T10 T20 T21 T23 T30 T31 C100 | Only meaningful with `core_wb_mode=1`. The neutral (1.0) value differs per SoC. **Since v1.10.0** the value is clamped per SoC: T10/T20/T30: the kernel register is 8 bit, wider values are clamped to 255 (up to v1.9.33 they wrapped, 300 -> 44). T21/T23/T31: the register saturates at 4095 (~16x), wider values are clamped to it (up to v1.9.33 accepted up to 65535 and echoed unchanged). |
| `image.wb_bgain` | int | `0` | 0..255 (T10/T20/T30, Q7: 128 = 1.0), 0..4095 (T21/T23/T31, Q8: 256 = 1.0), 0..65535 elsewhere | T10 T20 T21 T23 T30 T31 C100 | Only meaningful with `core_wb_mode=1`. Same per-SoC limits and scale as `image.wb_rgain`. **since v1.9.21** start value: read-only `image.wb_live.rgain`/`.bgain` in `GET /control` = gains AWB applies now. |
| `image.fcrop_enable` | bool | `0` | — | see the tier table in §10a | **Since v1.10.0.** ISP FrontCrop ("zoom"): `1` = the ISP delivers only the `fcrop_x/y/w/h` window of the sensor, and every stream scales it to its own size. Live where `caps.fcrop.live` is 1; OpenIMP T23 is live when the driver has the open-tx-isp `fcrop_upscale_pct` parameter and start-only otherwise (POST persists, restart required); on vendor T23, T40/T41, T30, C100 and vendor T10/T20/T21 a POST answers `"unsupported"` and a file value is ignored with one WARN at start. Not in `caps.image` — `caps.fcrop` carries the tier. |
| `image.fcrop_x` | int | `0` | 0..8192 | as above | Left edge in sensor-sized **picture** pixels: what the streams show with `image.hflip`/`image.vflip` applied (unrotated, before any stream scaler, sensor resolution). Where the ISP crops before it flips, the HAL mirrors the window per axis: OpenIMP T21/T23/T31 both axes (`x' = sensor_w - x - w` with `image.hflip`, `y' = sensor_h - y - h` with `image.vflip`), OpenIMP T10/T20 only y (x is already picture-oriented). A live change of a mirrored flip re-applies the window (start-only drivers: after a restart); readback, log and persisted values stay picture coordinates. Vendor stacks (vendor T31 included): untested, no mirroring; OpenIMP T10/T20 mirror only y (`FCROP_FLIP_MIRROR` bitmask in `src/roi_caps.h`, bit0 x, bit1 y). The SDK struct orders **top before left** — the HAL maps `x`→`fcrop_left`, `y`→`fcrop_top`. |
| `image.fcrop_y` | int | `0` | 0..8192 | as above | Top edge, picture pixels (see `fcrop_x` for the flip). |
| `image.fcrop_w` | int | `0` | 0..8192 | as above | Window width. The HAL evens x/y/w/h, moves (never shrinks) the window inside the sensor and refuses anything under 64×64 (`state":"rejected"`, WARN). Keep the stream aspect ratio, or every stream is stretched. |
| `image.fcrop_h` | int | `0` | 0..8192 | as above | Window height. |
| `image.fcrop_upscale_pct` | int | `-1` | -1..`caps.fcrop.upscale_max` | live | **Since v1.10.2.** Sets the open-tx-isp `fcrop_upscale_pct` driver parameter (`/sys/module/tx_isp_t23\|t31/parameters/`): the scaler may upscale a FrontCrop window by this many percent per axis, so a window down to `out × 100 / (100 + p)` is accepted. `-1` = leave the driver's value. Ceiling `caps.fcrop.upscale_max`: OpenIMP T23 and T31 100 (2.0×, device-tested; T31 at 150 hard-hangs every output until the hardware watchdog resets the camera, open-tx-isp clamps at 100 from agg-38). Written at start before any SetFrontCrop and on every change; lowering it re-applies the window, and a window that no longer fits is refused (`state":"rejected"`) and the crop goes back to the full sensor, never left for the next set-format to drop. Elsewhere (vendor stacks, other SoCs, a driver without the parameter) `caps.fcrop.upscale_max` is -1, a POST answers `"unsupported"` and a file value gets one WARN at start. |
| `image.ae_it_max_us` | int | `0` | 0..1000000 | T10 T20 T21 T23 T30 T31 C100 | `ISP_HAS_AE_IT_MAX` (T23/T31/C100) or `ISP_HAS_AE_IT_RANGE` (T10/T20/T21/T30). Inert on T40/T41. `0` = no cap: touches nothing if no cap was ever written, otherwise writes the sensor mode's own (remembered, uncapped) AE maximum back (**since v1.9.30**; before that `0` needed a restart). |

Prose and pitfalls

* **`running_mode` is a command, not just a value.** A `POST` of the value it
  already has is normally deduplicated, but `image.running_mode` is
  re-asserted to the ISP anyway (without re-persisting), because a
  `SetISPRunningMode` issued mid-AE-ramp at dusk is accepted and does not always
  latch. The HAL also kicks FrameSource channel 0 after the change, because the
  ISP only latches a running-mode change while chn0 delivers frames (an idle
  chn0 runs 500 ms and then stays on for the 2 s idle debounce; a running chn0
  is only held on, no wait — **since v1.10.0**), and
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
  2. **since v1.9.30** the cap can be raised, lowered and removed live. Once a
     cap is in force `GetExpr` reports *it* as the maximum, so the HAL remembers
     the sensor mode's own, uncapped maximum from before its first write and
     compares against that; `0`, or a value at or above that maximum, writes it
     back (T23/T31/C100: `SetAe_IT_MAX(uncapped)`; T10/T20/T21/T30:
     `SetIntegrationTime(MODE_RANGE)` at the uncapped maximum — `MODE_AUTO`
     would not lift the bound on that SDK). A removal is verified from the
     frame path like a cap.
     `0` on a camera that never wrote a cap still touches nothing. Up to
     v1.9.28 the cap only ratcheted **down**; raising it or setting `0` needed
     a restart.
  3. It **moves the day/night exposure index**: capping the AE's maximum
     integration time makes the AE answer a shortfall with gain instead, so a
     camera running this key needs its `daynight.day_gain`/`night_gain`
     re-checked rather than inherited.
* `image.hflip=1` + `image.vflip=1` is the supported way to get a 180° picture
  on every SoC except T40/T41 (see `videoN.rotation`).
* **FrontCrop (`image.fcrop_*`, since v1.10.0).** One window for the
  whole ISP, in sensor pixels; the request is applied **once per POST** (after
  all four coordinates arrived) and at start after every channel exists.
  `GET /control` reports `caps.fcrop` = `{tier, live, min:64, even:1, step:2,
  sensor:[w,h], state, min_win:[w,h], zoom_max, upscale_pct, upscale_max,
  zoomable:[..], zoomed:[..]}` (`upscale_max` **since v1.10.2**, the
  ceiling of `image.fcrop_upscale_pct`, -1 unsupported); `state` is `off`, `on`, `rejected` (timps refused the
  window: below `min_win`, 64×64 or no sensor size; or SetFrontCrop returned
  `-EINVAL`/`-EOPNOTSUPP` — on T10/T20/T21 any error — i.e. no stream can
  show it, logged once with errno), `waiting` (T10/T20/T21: refused only
  because every stream that could show the window is idle - the driver counts
  an idle stream's channel as unable; applied when one starts, a snapshot
  included; disabled streams do not count, the dedicated JPEG channel
  `jpeg.*` does), `failed` (any other SDK error, logged once per window, no
  retry until the next POST) or `unsupported`. `min_win`: T31/T23 `ceil(out × 100 / (100 + p))` per axis
  for the largest stream, `p` = open-tx-isp `fcrop_upscale_pct`
  (`/sys/module/tx_isp_t31|t23/parameters/`, reported as `upscale_pct`, -1 if
  absent = no upscaling); T10/T20/T21 the smallest downscaled stream (T10 at
  least 640×360); `zoom_max` = min(sensor/min_win) per axis. `zoomable[S]`:
  the stream can show a window at all (T10/T20/T21: only streams smaller than
  the sensor, so usually only the sub stream); `zoomed[S]`: it shows the
  current one. On T10/T20/T21 a stored window that was refused or is waiting
  is applied again whenever a framesource starts (stream, snapshot or motion);
  T31 does not retry on a start (its driver judges running channels only, a
  start cannot make a window acceptable). Re-POSTing an unchanged `image.fcrop_*` value retries it
  while the state is not `on`. Device-tested (by the OpenIMP maintainer)
  on **T10, T20 and T21 with OpenIMP**, live; on **T10/T20 only the downscaled
  streams show the window** (a main stream at sensor resolution keeps the full
  picture; set it below the sensor size to zoom it), T21 shows it on every
  stream no larger than the window. **T23 and T31 with OpenIMP** are
  `effective` and live when the driver has the open-tx-isp `fcrop_upscale_pct`
  parameter (detected at start, see the T23/T31 note under the tier table).
  Without it (vendor T31 kernel, older open-tx-isp) T31 is `experimental`,
  live, and `min_win` is the largest stream, so with a main stream at sensor
  size no window smaller than the sensor is accepted (no zoom); **T23 is
  `experimental` and start-only** (`caps.fcrop.live` 0; a POST persists and is
  reported restart-required, applied at the next `/etc/init.d/S95timps
  restart`) and needs open-tx-isp 3cf9bdac or newer. Vendor T31 is
  `experimental`, live; vendor T23 is `unsupported`. Max zoom is undocumented.
  OSD, privacy masks, ROI regions and the motion grid stay in **stream-frame**
  coordinates — the scene moves under them; the HAL logs a WARN when a crop
  is applied while a privacy mask is enabled. Interaction with `hflip`/`vflip`
  and rotation is untested.
* Unit note: `ae_it_max_us` is **microseconds**, deliberately not sensor lines —
  the HAL converts with the SDK's `one_line_expr_in_us` and clamps into the
  sensor's real range at apply time. **since v1.9.30** on T10/T20/T21/T30, where
  `GetExpr` is often empty (all zero on a Wyze Cam v2, T20X/jxf23, so the key
  never worked there before), the maximum comes from `GetIntegrationTime` and
  the line time is derived from the sensor fps (1 s / (fps × maximum), a
  fraction of a percent long, so the cap errs short). If neither source gives
  a maximum and a line time, the key logs one warning per start and does
  nothing; `-v` shows the raw readings (troubleshooting §3.7).
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
| `width` | int | `1920` / `640` | 64..4096 | restart | Unset (neither `width` nor `height` given): a known sensor size that fits inside the default is used instead, so a 1280x720 sensor streams 1280x720 (**since v1.10.0**; before, 1920x1080 upscaled). |
| `height` | int | `1080` / `360` | 64..4096 | restart | See `width`. |
| `fps` | int | `25` / `25` | 1..120 | restart | Per-channel rate — **this stream only**. It does not set the sensor rate (that is `sensor.fps`) and cannot exceed what `sensor.fps` delivers. Documented explicitly **since v1.9.19**; the behaviour is unchanged. |
| `bitrate` | int | `3000` / `512` | 16..50000 | **live** on every Ingenic SoC (see below) | **kbps.** |
| `rc_mode` | enum | `cbr` / `cbr` | `cbr`, `vbr`, `fixqp`, `smart`, `capped_vbr`, `capped_quality`; anything else → `cbr` | live on classic SoCs, restart on T31/C100/T40/T41 | Alias: `mode`. |
| `gop` | int | `50` / `50` | 1..1000 | restart | The real keyframe interval (`rcAttr.maxGop` / `gopAttr.uGopLength`). |
| `max_gop` | int | `60` / `60` | 1..1000 | **no effect** | **RESERVED and IGNORED.** No HAL consumes it; the keyframe interval comes from `gop`. A non-zero value logs one WARN per session. Parsed, clamped, persisted and echoed purely for compatibility. |
| `profile` | int | `2` / `2` | 0..2 | restart | `0` baseline, `1` main, `2` high. |
| `qp` | int | `35` / `35` | 1..51 | live on classic SoCs only | **Only consumed when `rc_mode = fixqp`** (it becomes `iInitialQP` / `attrH264FixQp.qp`). Under CBR/VBR/smart/capped_* it has no consumer at all — use `min_qp`/`max_qp` there. Deliberately **not** live on T31/C100/T40/T41: `SetChnAttrRcMode` writes it into the struct the next Get reads back but never re-programs the running channel (measured on T31X). |
| `min_qp` | int | `20` / `20` | 1..51 | **live** everywhere | |
| `max_qp` | int | `45` / `45` | 1..51 | **live** everywhere | A **soft** ceiling on the classic rate control (T10/T20 family, vendor libimp and OpenIMP alike): when a GOP needs more than ~3x its bit budget the encoder lifts the ceiling to its own `new_max_qp` (51) - set only by `IMP_Encoder_SetChnRcTrigLevel`, which timps does not call. So `max_qp` holds in normal scenes, but at a starved `bitrate` the stream keeps the target rather than the quality bound. |
| `quality_lvl` | int | `2` / `2` | 0..7 | live on classic SoCs; **no effect** on T31/C100/T40/T41| VBR/Smart: `minBitRate = bitrate * quality[lvl]`. No new-API equivalent exists anywhere; the HAL warns once if it deviates from the default. |
| `change_pos` | int | `80` / `80` | 50..100 | live on classic SoCs; **no effect** on T31/C100/T40/T41| VBR/Smart: % of bitrate above which QP is raised. |
| `i_bias_lvl` | int | `0` / `0` | -3..3 | live on classic SoCs and T31/C100; **no effect at all** on T40/T41 | VBR+CBR I-frame QP bias. `ENC_HAS_QPIPDELTA` is defined for **T31/C100 only** (`src/hal/hal_ingenic.c`): T40 and T41 have no `IMP_Encoder_SetChnQpIPDelta` and no new-API struct field, so the value is parsed, clamped, persisted, echoed and **ignored** (one WARN per session). A restart does not help. |
| `fluc_lvl` | int | `0` / `0` | 0..4 | **never live anywhere**; classic-SoC restart only | **H.265 only** — the H.264 rc structs have no `flucLvl` field. It is written into `attrH265Vbr`/`attrH265Cbr` in `classic_rc_fill()` and nowhere else, so: no effect on T31/C100/T40/T41 (no new-API equivalent), and inert on T10/T20/T23 because `codec = h265` is coerced away there. On T21/T30 it reaches the struct at channel creation, but classic H.265 channels are restart-bound (the classic `SetChnAttrRcMode` is H.264-only), so it never applies live. |
| `rotation` | enum/int | `0` / `0` | `0`, `90`, `270`, plus `180` on T40/T41; legacy `1`→90, `2`→270 | restart | See the prose below. Unsupported values coerce to `0` with a warning. |
| `buffers` | int | `2` / `2` | 1..8 | restart | IMP `nrVBs`. Setting it explicitly also sets an internal `buffers_explicit` flag, so the T31 safety clamp trusts your value instead of overriding it. The clamp gate is exactly `chn == 0 && isp_ch0_pre_dequeue_time != 0` (unreadable counts as active) — **scaled or not**; the older "non-scaled channel" theory was superseded in 2026-08. With the flag set the HAL warns and leaves `nrVBs` alone, which is why an explicit `buffers = 2` is *not* the same as omitting the line. |
| `rtsp_path` | string[64] | `/ch0` / `/ch1` | — | **live** | The one `videoN.*` key that is genuinely live — a DESCRIBE re-matches it on every request, and it is read from the live `g_cfg`, not the boot snapshot. **Since v1.9.20** the POST reply no longer lists it under `deferred` (v1.9.19 did, although the change was already live). Not in `caps.video_live`, which is the rate-control list only. |
| `imp_chn` | int | `0` / `1` | 0..7 | **file-only**, restart | IMP encoder channel index. Must equal the stream index `N` (it doubles as the hub slot): any other value is reset to `N` at load with an `[ERR]` line (**since v1.9.28**; before, a wrong value silently fed another stream's or the audio hub slot). |
| `jpeg` | bool | `1` / `1` | — | **file-only**, restart | Alias `jpeg_enabled`. Piggyback JPEG encoder in the same encoder group, sharing this stream's FrameSource (no extra rmem) at this stream's resolution. |
| `jpeg_quality` | int | `75` / `75` | 1..100 | **file-only**, restart | |
| `jpeg_fps` | int | `5` / `5` | 1..120 | **file-only**, restart | Max snapshot/MJPEG publish rate. |
| `jpeg_chn` | int | `3` / `4` | 0..8 | **file-only**, restart | `MS_MAX_VSTREAM + 1 + N`. Must be unique; **since v1.9.28** a collision is moved to the lowest free channel at load with an `[ERR]` line (JPEG disabled if none is free). |

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
  * **T31** — libimp FrameSource rotate (`ROT_HAS_FS_ROTATE`). Envelope:
    width and height multiples of 64, at most 1280x704, at most 15 fps.
  * **T10 T20 T21, OpenIMP builds only** (`USE_OPENIMP`, i.e.
    `BR2_PACKAGE_OPENIMP`), **since v1.10.0** — the same FrameSource
    rotate, done in software by OpenIMP. Sub stream only: `video0` (IMP
    channel 0) is always refused.
    Width and height must be multiples of 16 (`640x368` works, `640x360` does
    not) and at most 704x576 pixels. No fps cap; 15 fps is recommended for
    the rotated sub stream (estimated 5-11% CPU at 15 fps).
    **Needs an OpenIMP with T1x rotation**: OpenIMP commit `6b7f5ae` (branch
    `claude/t1x-rotation`, 2026-10-05), not yet in OpenIMP `next` or in any
    thingino `ciao` OpenIMP pin (`9cab219` ciao, `6857610` ciao-t20/t21,
    `0c02f45` ciao-openstack, `636b6ac` ciao-t41). timps checks this at
    start: it first calls `IMP_FrameSource_SetChnRotate` with a 0x0 picture,
    which a rotating
    OpenIMP refuses; a libimp that accepts it is a stub. Outcomes, each with
    the stream starting **unrotated** and a warning:
    * OpenIMP `9cab219` (the thingino `ciao` pin) stubs the call with a plain
      success → `SetChnRotate is an OpenIMP stub - UNROTATED` (until
      v1.10.0: `libimp's SetChnRotate accepted a 0x0 picture, so it cannot
      rotate …`). Without this check (development builds between 1.9.33 and
      this fix) the stream came up with swapped encoder dims.
    * OpenIMP `6857610`, `0c02f45`, `636b6ac` refuse 90/270 →
      `SetChnRotate refused WxH`.
    * No `IMP_FrameSource_SetChnRotate` in libimp → `libimp has no
      IMP_FrameSource_SetChnRotate`.
    With a rotating OpenIMP the probe makes OpenIMP print one line on stderr,
    `[FS] SetChnRotate chN: unsupported rotTo90=M 0x0`; that is expected.
    **Not yet tested on hardware.** Vendor-libimp T10/T20/T21 builds have no path.
  * **T23** — software NV12 transpose, only with the extra opt-in
    `BR2_PACKAGE_TIMPS_SW_ROTATE` (`-DMS_ENABLE_SW_ROTATE`). Costs real CPU on
    the single-core T23, is H.264 only, and loses hardware OSD/privacy on the
    rotated stream. Envelope (`src/hal/fs_rot_env.h`): at most 704x576 pixels
    and 15 fps, the source height (= encoded width) a multiple of 16 and at
    least 256; snapshot/MJPEG on that stream also need it a multiple of 32
    (e.g. `704x384` → 384x704; `1280x704` is refused, 204).
  * **T30 C100** — no path at all; 90/270 coerce to `0`.
* Outside the envelope (T31, T10/T20/T21, T23) the stream starts **unrotated** with a
  `refusing FS-rotate ...` / `refusing SW rotate ...` warning (on T10/T20/T21 also when the OpenIMP cannot
  rotate, see above); `videoN.rotation` keeps the configured
  value, and `GET /control` shows the running geometry in
  `videoN.eff_width`/`eff_height` (unswapped). **Since v1.10.1**
  `video.<N>.rotation_state` carries the reason as a code, and a POST that
  would be refused at the next start says so in
  `"errors":{"videoN.rotation":<code>}` ([`error-codes.md`](error-codes.md)).
* **`180` is not a rotation on most SoCs.** On every classic-API SoC it was only
  a global ISP Hflip+Vflip, falsely modelled as per-stream, and was removed —
  it coerces to `0` with a log line telling you to use
  `image.hflip=1` + `image.vflip=1`. On **T40/T41 only** it is a genuine
  per-channel hardware I2D rotate, which the global flips cannot replicate.
* A 90/270 rotation **swaps the effective width/height** for every downstream
  consumer (encoder attrs, OSD, hub, muxers, JPEG piggyback).
* Overlays on a rotated stream: `osdN.<i>.x/y`, `privacyN.<i>.*` and the
  motion grid are in the **rotated**
  `eff_width`×`eff_height` frame on every path. What is drawn depends on the
  path, reported since v1.10.2 as `video.<N>.rotate_path` in `GET /control`:
  `sw` (T23) text only, no logo, no privacy covers; `band` (vendor libimp
  90/270, T31/T40/T41) only the top `eff_width` rows; `hw` (OpenIMP, and
  T40/T41 `180`) the whole frame; `off` not rotated (also a refused rotation). Encoder ROI ignores rotated streams (`caps.roi.streams[N] = "rotated"`).
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
* **since v1.9.28** channel collisions (`imp_chn` != stream index, duplicate
  `jpeg_chn`, `jpeg.imp_chn` on a video channel) are corrected at load and
  logged as
  `[ERR] config: ... collides ...` / `... must equal the stream index`.

---

## 6. `audio.*`

Every `audio.*` key is `F_CTRL` (POST-able). The **live vs restart** split is
listed per key below; the restart keys carry `F_RESTART` in `audio_fields[]`,
and **since v1.9.20** they are listed in `caps.restart` as
`audio.<key>` and a changed one comes back in the POST reply's `deferred_keys`. An audio key the SoC does not have logs
`audio.<k> unsupported on this platform` — like the `image.*`
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
| `audio.spk_volume` | int | `80` | 0..100 | **live** if a play/backchannel session holds the speaker; otherwise applied at the next AO open | Needs `USE_PLAY` or `USE_BACKCHANNEL`, else `"unsupported"` (not persisted). |
| `audio.spk_gain` | int | `25` | 0..100 | same as `spk_volume` | |
| `audio.backchannel` | bool | `0` | — | restart | ONVIF audio backchannel (client → speaker). Needs `USE_BACKCHANNEL`. The pipeline is configured once at boot and RTSP gates on that boot state, so a live change does nothing until restart. |
| `audio.backchannel_codec` | enum | `pcmu` (0) | `pcmu`→0, `pcma`→1, `aac`→2, or a number clamped 0..2 | restart | `aac` additionally needs `USE_BC_AAC`. **Reads back as a number**, not a word. |
| `audio.backchannel_rate` | int | `16000` | 8000..48000 | restart | Speaker sample rate. |
| `audio.aec` | bool | `0` | — | applied at the **next AO open** | Acoustic echo cancellation (`IMP_AI_EnableAec`). A live POST persists and returns "not live". Needs `USE_PLAY`/`USE_BACKCHANNEL`; otherwise `"unsupported"` (not persisted). Opt-in because quality/latency varies per SoC/mic/speaker pairing. |
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
| `jpeg.imp_chn` | int | `2` | 0..7 | file-only, restart | Must not collide with an enabled `videoN.imp_chn` (0, 1); a collision is moved to the lowest free channel at load with an `[ERR]` line (**since v1.9.28**). |
| `jpeg.snapshot_path` | string[128] | `""` | — | file-only, restart | Periodic file snapshot; `""` = none. |

Pitfall: `timps.conf.example` shows `jpeg.enabled = 1`; the **compiled default
is `0`**. The example says so in a comment, but the two do not agree at a
glance.

Source selection (`hub_pick_jpeg_src`): a piggyback source is preferred when
that stream was **boot**-enabled and has `videoN.jpeg` on; otherwise `jpeg.*`;
otherwise the first boot-enabled stream with a piggyback encoder.
---

## 8. `osd.*` (globals)

All keys are `F_CTRL` (POST-able). `enabled`, `font_path`, `supersample` and
`hinting` are **restart** (`F_RESTART`): `imp_osd_setup()` reads them once at
startup. `monitor_stream` is **live**: the OSD thread re-reads it on every
text refresh (about once a second).

**since v1.9.28** the `{placeholder}` source file (see below) has no config key at
all - not even file-only. It used to (`osd.vars_file`), but any settable path,
POSTable or not, could point it at `/etc/timps.conf` and render its
credentials into the video through a `{placeholder}`. The path is now fixed
in `osd_vars.c` (`OSD_VARS_FILE`, `/tmp/timps_osd.vars`).

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
| `x` | int | see layout | -8192..8192 (**since v1.9.28**; unclamped before) | live | `0` = centred horizontally; positive = px from the left; negative = px from the right. |
| `y` | int | see layout | -8192..8192 (same) | live | `0` = centred vertically; positive = from the top; negative = from the bottom. |
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
looked up in the fixed `/tmp/timps_osd.vars` (`name = value` lines, not
configurable - see §8); an unresolved name expands to the empty string.
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
| `x` | int | `0` | -8192..8192 (**since v1.9.28**; unclamped before) | live | Pixels in that stream's frame. |
| `y` | int | `0` | -8192..8192 (same) | live | |
| `w` | int | `0` | 0..8192 (same) | live | Alias `width`. |
| `h` | int | `0` | 0..8192 (same) | live | Alias `height`. |
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

## 10a. Encoder ROI — `roi<S>.<N>.*`

**Since v1.10.0.** Per-stream regions with their own QP, e.g. a
sharper face area or a coarser sky. Two SDK APIs: family A
`IMP_Encoder_SetChnROI` (T10..T23, 8 slots, corners) and family B
`IMP_Encoder_SetChnRoiAttr` (OpenIMP T31, 10 windows, x/y/w/h; regions 0..3
go to windows 0..3, 4..9 stay off, one call per apply). `S` = stream `0..1`, `N` = region `0..3` (`MS_MAX_ROI = 4`; the
SDK has 8 slots, timps uses 0..3 to keep the key table and `GET /control` at
the privacy-mask size). POST shape like privacy:

```json
{"roi": {"0": {"1": {"enabled":1,"x":640,"y":320,"w":320,"h":240,"qp":-6,"qp_mode":1}}}}
```

| Key | Type | Default | Range | Apply | Notes |
| --- | --- | --- | --- | --- | --- |
| `enabled` | bool | `0` | — | live | |
| `x` | int | `0` | 0..8192 | live | Pixels of the **encoded stream picture** (`videoN.width` × `height`). |
| `y` | int | `0` | 0..8192 | live | |
| `w` | int | `0` | 0..8192 | live | |
| `h` | int | `0` | 0..8192 | live | |
| `qp` | int | `-5` | -51..51 | live | Clamped by the HAL per mode: absolute 0..51, delta -25..25 (all SoCs). An absolute QP may go out as a delta, see below. |
| `qp_mode` | int | `1` | 0..1 | live | `0` absolute QP, `1` delta to the frame QP (SDK `bRelatedQp`). |

"live" = the stream's encode thread re-applies all its regions before the next
frame (the SDK takes it from the next frame on); an idle stream gets them when
it starts. Where `caps.roi.tier` is `unsupported` the keys are capability-gated
(POST → `"unsupported"`, a file value logs one WARN at start).

Pitfalls:

* **Corners, not sizes (family A).** The SDK `IMPRect` is two inclusive corners `p0/p1`
  (OpenIMP's header calls the same four ints x/y/width/height but reads them as
  corners). timps converts: `p0 = (x, y)`, `p1 = (x+w-1, y+h-1)`. Family B
  (T31) takes real x/y/w/h: timps takes the aligned corners and sends
  `w = min(p1.x+1, width) - p0.x` (same for h), because libimp refuses a
  window past the picture (a 1080p stream's last macroblock row ends at 1088).
* **QP ranges (measured by the OpenIMP maintainer).** Delta -25..25 on every
  SoC: -26 made the jump back at the region edge `mb_qp_delta` +26, which is
  invalid H.264 (T20: Intel VA-API refused it, ffmpeg hid it). T31/T41
  libimp take -26 but limit it to -25. T21 (and OpenIMP T23, since v1.10.1)
  honours only about -12..+13 (`caps.roi.qp_delta_eff`); positive deltas are
  also capped by `max_qp`.
* **Absolute QP → delta.** An absolute QP is valid only while
  |abs − picture QP| ≤ 25 (`caps.roi.abs_window`) for every picture QP the
  stream can have. timps assumes the picture QP is `videoN.qp` (default 35)
  under `rc_mode=fixqp` and anything in `min_qp`..`max_qp` (default 15..45)
  under the other modes. If the rule holds the QP goes out absolute
  (family B: `FIXED_QP`, which OpenIMP writes as the difference to the last
  picture QP: exact under FixQP, one picture late under CBR), otherwise as the
  delta `abs − ref`, clamped to -25..25, with `ref` = `videoN.qp` (FixQP) or
  the middle of the QP window. Example: FixQP 35, abs 5 → delta -25; CBR
  15..45, abs 15 → delta -15. The rule uses the values the running encoder
  has: a live change of `videoN.qp`, `min_qp`, `max_qp` or `rc_mode` re-maps
  the regions where that key is live (T10..T23/T30: all four; T31/C100/T40/T41:
  `min_qp`/`max_qp` only), a restart-only `rc_mode`/`qp` counts from the
  next restart.
* **Macroblocks.** The SDK truncates to 16×16 macroblocks and clips nothing;
  timps rounds the rectangle **outwards** to 16 and clamps it into the picture.
  `p1` is the last pixel of the last macroblock, so it stays inside the
  macroblock grid whether libimp treats it as inclusive or exclusive (at worst
  one macroblock short). A region fully outside the picture is turned off with a
  WARN and shows in `caps.roi.rejected[S]` (bitmask of regions). The aligned
  corners are logged at INFO.
* **H.264, unrotated streams only.** An H.265 stream (`caps.roi.streams[S]` =
  `h265`) and a 90/270 stream (`rotated`: the encoder input is the rotated frame
  and how the SDK addresses it is unverified) ignore their regions: a file
  value logs one WARN at start, a POST is answered as accepted and persisted
  but applies nothing and logs nothing - only `caps.roi.streams[S]` shows it
  (the WebUI reads it). The coordinates are meant in the picture as the viewer sees it, so a
  future rotated mode needs no new keys.
* Delta behaviour at the QP limits, the interaction with each rate-control mode
  and with OSD/privacy covers are untested. The only bitrate measurement is the
  OpenIMP maintainer's T10 test (FixQP, QP51 region: -25 %), not a timps one.

`GET /control`: `roi.<S>.<N>.*` echoes the config; `caps.roi` =
`{tier, api, regions:4, slots, align:16, qp_abs:[0,51], qp_delta:[-25,25],
qp_delta_eff:[lo,hi], abs_window:25, streams:[...], rejected:[...]}`.
`api` is `chnroi` (family A, `slots` 8) or `roiattr` (family B, `slots` 10);
`qp_delta` is what timps sends, `qp_delta_eff` what the encoder honours (T21
OpenIMP and, since v1.10.1, T23 OpenIMP `[-12,13]`, elsewhere = `qp_delta`); `streams[S]` one of `ok`, `off`,
`h265`, `rotated`, `unsupported`; `rejected[S]` a bitmask of regions outside
the picture.

### Tier table (ROI and FrontCrop, `src/roi_caps.h`)

`effective` = device-tested, `experimental` = the call exists and returns 0,
effect unverified (or known absent), `unsupported` = refused. Both setters are
bound weak: a libimp without the symbol reports `unsupported` at run time.
**Since v1.10.1** a POST of `roi<S>.<N>.*` is refused (`"unsupported"`) whenever
the running tier is `unsupported`, not only where the build lacks ROI.
The OpenIMP tiers come from device tests by the OpenIMP maintainer (OpenIMP
`claude/t31-roi` d03ca24, `claude/roi-qp-clamp` fec3e69, open-tx-isp
3cf9bdac/27dc8be4/b15ef235), not from timps' own tests.

| SoC | ROI, vendor libimp | ROI, OpenIMP | FrontCrop, vendor | FrontCrop, OpenIMP |
| --- | --- | --- | --- | --- |
| T10 | experimental | **effective** (FixQP QP51 -25 % bitrate, delta -10 x3.7) | unsupported (no symbol) | **effective** (17/17 device checks), live |
| T20 | experimental | **effective** (QP51 block visible, QP15 clean) | unsupported (no symbol) | **effective** (downscaled streams only), live |
| T21 | experimental (returns 0, no effect seen) | **effective** (on by default; libimp env `OPENIMP_T21_ROI=0` turns it off, timps then reports `experimental`) | unsupported (no symbol) | **effective** (all streams, 5 runs), live |
| T23 | unsupported (the vendor encoder only stores the region) | **since v1.10.1** runtime-detected: **effective** when the libimp exports `OpenIMP_Cap_T23HelixRoi` (OpenIMP c271c07, in v2026.10.11-rc1/agg-35+; regions programmed by the native Helix encoder, device-tested by the OpenIMP maintainer), otherwise `unsupported` (older OpenIMP drops the regions silently); libimp env `OPENIMP_T23_ROI=0` turns it off, timps then reports `unsupported` | unsupported (untested) | runtime-detected: **effective**, live with the `fcrop_upscale_pct` parameter (open-tx-isp 164225e5/25b1d8c7, in agg-34 b15ef235); without it experimental, **start-only** (`caps.fcrop.live` 0; needs 3cf9bdac+) |
| T30, C100 | unsupported (not investigated) | — | unsupported (not investigated) | — |
| T31 | unsupported (the 1.1.x libimp has no ROI symbol) | **effective** via `SetChnRoiAttr` (AVPU QP table; needs an OpenIMP with d03ca24 and the clamp fec3e69, see below; libimp env `OPENIMP_T31_ROI=0` turns it off, timps then reports `unsupported`) | experimental, live (no zoom without `fcrop_upscale_pct`) | runtime-detected: **effective**, live with the `fcrop_upscale_pct` parameter (open-tx-isp guard 27dc8be4, in agg-34 b15ef235; zoomed main + sub device-tested); without it experimental, live |
| T40, T41 | unsupported (vendor T41 `SetChnRoiAttr` takes deltas only; not device-tested) | unsupported (OpenIMP T41 ROI experimental, off by default) | unsupported (no FrontCrop; live fs crop stops T41) | unsupported |
| host sim | sim | | sim | |

Refused-key log text (`ROI_WHY`): vendor T23 "no effect on T23: the vendor
encoder only stores the region"; OpenIMP T23 without the marker "T23 ROI needs
OpenIMP v2026.10.11-rc1+ (OpenIMP_Cap_T23HelixRoi), OPENIMP_T23_ROI unset"; vendor T31 "T31 ROI needs OpenIMP (SetChnRoiAttr);
the vendor libimp has no ROI"; T40/T41 "T40/T41 SetChnRoiAttr is not
device-tested, left off". Family B is live without IDR, H.264 only, and with
no window set the encoder output is bit-identical (OpenIMP maintainer).

**T31 ROI needs a recent OpenIMP.** `IMP_Encoder_SetChnRoiAttr` for T31 is
OpenIMP d03ca24 (branch `claude/t31-roi`), the mb_qp_delta clamp fec3e69
(`claude/roi-qp-clamp`); both are in the `claude/agg-34` aggregate, not yet on
OpenIMP `main`/`next`. A firmware whose OpenIMP pin lacks them (thingino's
`package/openimp` pins `main`) reports `caps.roi.tier` = `unsupported`.

**T23/T31 FrontCrop (OpenIMP).** The tier is detected once at start from
`/sys/module/tx_isp_t23/parameters/fcrop_upscale_pct` (T31: `tx_isp_t31`),
logged as `image.fcrop: <tier>, live|start-only (<path> present|absent)`. The
parameter exists only in drivers with the T23 crop-hang fix (open-tx-isp
164225e5/25b1d8c7) or the T31 MSCA guard (27dc8be4); open-tx-isp b15ef235
(agg-34) has both. Present: `effective`, live. Every stream must be no larger
than the window or scaled down (`fcrop_upscale_pct`, default 0 = no
upscaling; **since v1.10.2** set by `image.fcrop_upscale_pct`); a smaller window is refused with `-EINVAL`, logged once and
reported as `caps.fcrop.state` = `rejected`. Absent on T23: `experimental`,
start-only, needs open-tx-isp 3cf9bdac or newer (older drivers hung the
pipeline on a crop change); the start apply runs before chn0 streams, a POST
of `image.fcrop_*` is persisted and reported as restart-required and applies
at the next `/etc/init.d/S95timps restart`. Absent on T31: `experimental`,
live.

Raising a tier after a device test is one line in `src/roi_caps.h` plus the
`ROI_MATRIX` expectation in the Makefile (`make test-roi`). Still to verify:
T10/T20/T21 vendor ROI and vendor T31 FrontCrop.

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
| `motion.monitor_stream` | int | `1` | `T_CHAN`: out of `0..1` **coerces to 0** | **live** | Stream whose frames IVS compares. Default is the sub stream (about 1/9 of the pixels of 1080p, so far less CPU); falls back to `0` when `video1` is not enabled. `GET /control` reports the configured value, not the fallback. |
| `motion.sensitivity` | int | `128` | 0..255 | **live** | Mapped to the SDK's 0..4 as `v*4/255`. A change that maps to the same level skips the grid rebuild but **is** still persisted. |
| `motion.cols` | int | `5` (see below) | ≥1, and `cols*rows ≤ MOTION_CELL_LIMIT` | **live** | Clamped against the *current* other axis, never the other way round, so re-applying the same pair is idempotent. |
| `motion.rows` | int | `5` (see below) | same | **live** | |
| `motion.cooldown_ms` | int | `5000` | **250..INT_MAX** | **file-only** | Minimum gap between motion events. **Not POST-able by design** — it is the floor that bounds how often the `on_motion` hook can be re-exec'd. `0` is no longer accepted. |
| `motion.hold_ms` | int | `800` | 0..60000 | **live** | Keep a cell "active" this long after its last hit, so asynchronous `/events`/`/control` readers reliably observe single-frame motion. `0` = no hold. Takes effect through a grid re-sync. |
| `motion.skip_frames` | int | `5` | 1..100 | **live** | `IMP_IVS_MoveParam.skipFrameCnt` — analyse every Nth frame. Higher = cheaper but more latency. Takes effect through a grid re-sync. |
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
| `record.dir` | string[128] | `/mnt/mmcblk0p1` | — | live | Segments land under `<dir>/<hostname>/records/`. **since v1.9.28** a dir on the root filesystem (e.g. the mount point with no card) is refused: one `[ERR] record.dir … is on the root filesystem (card not mounted?)` per episode, also in `record.last_error`, nothing written; `/tmp` is allowed. The check follows the nearest existing parent when the dir does not exist yet. |
| `record.name` | string[96] | `%Y%m%d/%H/%Y%m%dT%H%M%S` | — | live | strftime path template. |
| `record.segment_s` | int | `60` | 0..86400 | live | `0` = single file, no rotation. Alias `record.segment`. |
| `record.pre_roll_s` | int | `3` | 0..60 | live | Motion mode: buffered seconds kept before the trigger. Alias `record.pre_roll`. |
| `record.post_roll_s` | int | `10` | **1..300** | live | Alias `record.post_roll`. **Floor is 1, not 0, deliberately:** motion-triggered recording gates on `last_ms < post_roll_s*1000`, which at 0 is never true even for the triggering event, so `mode=motion` would record nothing at all with `enabled:true` and no warning. |
| `record.min_free_mb` | int | `200` | 0..1048576 | live | Delete oldest segments until at least this much is free. |
| `record.audio` | bool | `1` | — | live | Mux audio into the recording when available. |

Non-config commands on the same section (not persisted):
`{"record":{"active":1|0}}` = manual start/stop override (omit or `<0` returns
to config mode); `{"record":{"clip":"/tmp/x.mp4","seconds":6}}` = on-demand
fMP4 clip; the POST returns once it is written (roughly `seconds`).
**since v1.9.28** the capture runs outside the apply lock, so it no longer holds
up other POSTs for that long, and a clip requested while another is still
being written is refused and counted in `rejected`.

---

## 13. `timelapse.*`

Requires `USE_TIMELAPSE` (`BR2_PACKAGE_TIMPS_TIMELAPSE`, default y). Every key is
`F_CTRL` and read **live** by the timelapse thread.

| Key | Type | Default | Range | Apply | Notes |
| --- | --- | --- | --- | --- | --- |
| `timelapse.enabled` | bool | `0` | — | live | Also gates the on-boot start. |
| `timelapse.channel` | int | `0` | `T_CHAN`: out of `0..1` → 0 | live | The stream whose piggyback JPEG encoder is captured; falls back to the dedicated `jpeg.*` channel. |
| `timelapse.dir` | string[128] | `/mnt/mmcblk0p1` | — | live | Shots land under `<dir>/<hostname>/timelapses/`. Same root-filesystem refusal as `record.dir` (**since v1.9.28**). |
| `timelapse.name` | string[96] | `%Y%m%d/%H/%Y%m%dT%H%M%S` | — | live | strftime template, `.jpg` appended. |
| `timelapse.interval_s` | int | `60` | 1..INT_MAX | live | Alias `timelapse.interval`. |
| `timelapse.keep_days` | int | `7` | 0..3650 | live | `0` = keep forever. **since v1.9.28** clamped to 3650 (was 0..INT_MAX; a huge value overflowed the cutoff on 32-bit `time_t` and pruned everything). Files with a pre-2025 (pre-NTP) mtime are never age-pruned. |

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
| `daynight.mode` | enum | `auto` | `auto`, `schedule`; legacy `sensor`→auto, `time`/`sun`→schedule. From the **file**, an unknown token logs a warning and falls back to `auto`; from **POST** it is rejected. | live (hand-validated) | `auto` = the automaton below (the calendar is not used). `schedule` = the calendar decides outright: no sensor, no looks, two IR-cut clicks a day. Only matters when `enabled=1`. |
| `daynight.time_night_start` | string[6] | `""` | `"HH:MM"` local, **unvalidated** at parse | live | Night at/after this time. Empty = that edge unset. **since v1.9.28** a window the calendar cannot use (one edge set, a malformed value, or both edges equal) is ignored with one startup WARN, and the sun calendar is used if a location is set. |
| `daynight.time_day_start` | string[6] | `""` | `"HH:MM"` local, **unvalidated** | live | Day at/after this time. The window may wrap past midnight. |
| `daynight.sun_latitude` | float | `0.0` | -90..90 | live | Degrees, +N/−S. |
| `daynight.sun_longitude` | float | `0.0` | -180..180 | live | Degrees, +E/−W. |
| `daynight.sun_sunrise_offset_min` | int | `0` | -1440..1440 | live | Minutes added to sunrise before switching to day. |
| `daynight.sun_sunset_offset_min` | int | `0` | -1440..1440 | live | Minutes added to sunset before switching to night. |
| `daynight.day_gain` | float | `768` | 1..1000000 | live | **Alias `total_gain_day_threshold`.** A check (the day pipeline) below this = day. Units are the IMP `[24.8]` linear gain scale, `256` = 1.0×, so 768 = 3×. It also is the bar a look (the scene without the illuminator) must read below before it asks for a check, until a failed check has set a scene bar. |
| `daynight.night_gain` | float | `4096` | 1..1000000 | live | **Alias `total_gain_night_threshold`.** Exposure index above this (while in day) = night. 4096 = 16×. Where the AE cannot reach it, the AE at its limit counts as dark too (see below). |
| `daynight.day_confirm_s` | int | `30` | 1..3600 | live | How long the index must stay above `night_gain` (or the AE at its limit) before day→night. A very dark reading (≥ 2 × `night_gain`, or the AE at its limit) switches after 6 s instead (not within 8 s of the camera's own switch). |
| `daynight.jump_pct` | int | `20` | 5..70 | live | **since v1.10.1.** At night: when the illuminator-lit level falls this many percent below the level of the last look and stays there 20 s, look at once (new light). Cameras without a switchable illuminator use a fixed 50 % and check. |
| `daynight.color_pct` | int | `3` | 0..50 | live | **since v1.10.2.** At night: when the colour of the illuminator-lit picture (global R/G or B/G of the ISP's AWB statistics) moves this many percent from its value after the last look and stays there 20 s, look at once (log `look (color)`). Read every 5 s at night (after start also outside it, until the first success, at most 30 reads). Catches a weak lamp that barely moves the lit level (measured with a 30 % bulb: Eufy B/G −4 % at an unchanged exposure, Wyze Pan B/G +5.5 %; an unchanged scene stays within 0.5 %). Only starts a look (8 s dark, no click); the look decides as always. At most one colour look per 5 min, the gap doubles (10, 20 … up to 45 min) while no timer look happens in between, so a person moving in view costs at most 4 extra looks (32 s dark) in the first hour and then none beyond the 45-min rhythm. `0` = off. Needs a switchable illuminator and a libimp with per-zone AWB statistics: T40/T41 vendor, T23/T31/C100 vendor, T10/T20 OpenIMP, T23/T31 OpenIMP only with a libimp that exports `OpenIMP_Cap_AwbZone` (agg-38+), T40/T41 OpenIMP only with one that exports `OpenIMP_Cap_AwbStatistics`; elsewhere `daynight.color_trigger` reads `"unavailable"` and nothing happens. Device-tested only on T40 vendor (Eufy) and T20 OpenIMP (Wyze Pan); the other paths are unverified. |
| `daynight.heartbeat_s` | int | `14400` (4 h) | 300..604800 | live | Check interval at night for cameras **without** a switchable illuminator. Also caps the look interval (45 min) of the others. **Since v1.10.2** a camera whose colour trigger works (`daynight.color_trigger` "on") looks only every 6 h: jump and colour watch the night, the timer is a backstop. Colour-look gaps still stop at 45 min. |
| `daynight.heartbeat_max_s` | int | `43200` (12 h) | 300..604800 | live | The longest night without a day-pipeline reading: then a check runs regardless of looks and lockouts. Bounds a wrong night. |
| `daynight.interval_ms` | int | `2000` | 100..60000 | live | Sample interval. The exposure index needs a `/proc` scrape per tick (integration time has no IMP API). |
| `daynight.history_s` | int | `0` | 0..172800 (48 h) | live | In-RAM decision-history ring for the WebUI tuning graph, in seconds of retention. `0` = off; nothing is allocated until a sample is pushed. One 16-byte sample per period, so the 48 h ceiling costs ~270 KiB. |
| `daynight.ir_idle_off` | int | `0` | 0..1 | live | **since v1.10.0.** Idle-Dark: at night, once no picture consumer (video/JPEG hub subscriber: RTSP, WebRTC, fMP4, MJPEG, SRT, snapshot, timelapse, recorder; plus the motion FrameSource pin and a set `jpeg.snapshot_path`) has been present for `ir_idle_linger_s`, switch only the IR illuminator off (IR-cut and ISP stay in night) and keep reading the scene without it. **Only effective with the FrameSource keepalive** (`general.fs_keepalive`, auto = on for OpenIMP T23/T41); otherwise one WARN and `daynight.ir_idle.reason = "keepalive"`. Also needs auto mode (`daynight.mode = auto`), a working `daynight.irprobe_cmd` and a `timps-irprobe` with the `idle`/`state` verbs. What happens: the LED goes off and the scene is read without it like a look that lasts: below the bar for 30 s → a check (one click, nobody is watching); the first consumer gets the LED within one hook call (a kick, not the next tick). JPEG grabs (`/snapshot.jpg`, timelapse) right after that wake are held up to ~3.5 s (max 5 s) for the AE; streams are not held, their first 1-4 s are dark or settling. Setting it to 0 by POST, `daynight.enabled = 0`, `mode = schedule` and a stop give the LED back. Status: `daynight.ir_idle` in `GET /control` / `/events`. |
| `daynight.ir_idle_linger_s` | int | `60` | 0..3600 | live | **since v1.10.0.** How long after the last picture consumer the illuminator may go off; a consumer inside it is not an event (covers 10 s snapshot polling and DESCRIBE→PLAY). A poll slower than the linger costs one LED on/off pair and one held snapshot per poll. Keep it **longer than `timelapse.interval_s`**: with both at 60 the linger can run out on the tick before each shot, giving one LED on/off pair per shot (set e.g. 90). |
| `daynight.switch_cmd` | string[64] | `daynight` | — | **file-only**, `F_NOGET` | Board script, run as `<cmd> day\|night` via `vfork()`+`execlp()` (no shell; `fork()` on v1.9.19). **Unlike `motion.on_motion`, this one *does* search `PATH`** — `execlp`, not `execve` — which is why the bare default `daynight` works. Same for `daynight.irprobe_cmd`. |
| `daynight.isp_path` | string[128] | `/proc/jz/isp/isp-m0` | — | **file-only**, `F_NOGET` | ISP exposure proc file that is scraped. **Since v1.10.2** never read on vendor T21 (libimp 1.0.33 with the vendor tx-isp-t21): that driver corrupts the kernel heap on a proc read (an oops killed the day/night thread on a PC420); the exposure comes from IMP only (`GetTotalGain`, `GetExpr`), logged once at start. The ISP mode readback and the AE-limit test from the dump are then unavailable there. |
| `daynight.irprobe_cmd` | string[64] | `timps-irprobe` | — | **file-only**, `F_NOGET` | Run as `<cmd> on\|off` (a look) and, for Idle-Dark, `<cmd> idle\|state` (`idle`: LED off with a 15 min failsafe that the 10 min re-assert renews; `state` prints `on`/`off`). Empty: no looks; the camera checks like one without a switchable illuminator. Two failures in a row turn looks off for the session. |
| `daynight.trace_path` | string[128] | `""` | — | **file-only**, `F_NOGET` | Opt-in CSV decision-trace recorder, one line per N samples (about 32 KB/h). Past 256 KB (**since v1.10.2**, before: 1 MB) it is rotated once to `<path>.1` (the old `.1` is overwritten), so it never holds more than 512 KB of RAM. **Must live on tmpfs** (`/tmp`, `/run`) — a LOGW reminds you if the path does not look like tmpfs. |

### How the decision works (needed to answer tuning questions)

**Since v1.10.1** (simple engine; `dev_notes/dn-redesign.md` has the derivation,
the behaviour corpus and the comparison with legacy, thingino-daynightd and
raptor). The metric is the **exposure index**, not bare gain:

```
D = total_gain * (integration_time / max_integration_time)
```

Higher = darker; when the integration-time fields are unreadable `D` degrades
to `total_gain`.

* **Day → night**: `D` (the day pipeline) above `night_gain`, or the AE at its
  limit (no reserve left, or the analog gain at ≥ 98 % of its published cap
  with the integration time ≥ 80 % and no digital gain), held `day_confirm_s`;
  6 s when the reading is very dark (≥ 2 × `night_gain` or at the limit).
* **At night** the reading with the camera's own illuminator means little, so
  the camera **looks**: illuminator off for 8 s (no click), reading the scene
  without it. A look runs every 45 min (`heartbeat_s` caps it), at once when
  the lit level falls `jump_pct` below the last look for 20 s (a lamp), **since
  v1.10.2** when the lit picture's colour moves `color_pct` from the last look
  for 20 s (a weak lamp; at most every 5 min, doubling), and on
  `{"daynight":{"probe":1}}`. A lit level that rises 10 % above the
  reference and does not fall back below it for 60 s (the light of the last
  look has gone) becomes the reference, and the next look relearns the scene
  bar. **Since v1.10.2** a look taken while the lit level was still moving (a
  lamp coming on, another camera's look; a 20 s jump excepted) sets no
  reference: the next settled level does, within 60 s. The colour reference
  is retaken 8 s after every look with the AE settled; no colour look runs
  while a jump hold runs or the lit level sits above the reference, and after
  a colour look that found night, the colour back within `color_pct`/2 of its
  value before that look for 20 s is taken as the new reference, not as light.
* **Look → check**: if the reading without the illuminator is below the bar —
  `day_gain`, the jump bar for a look a jump started, or a scene bar learnt
  from a failed check — the camera switches to the day pipeline and reads it
  once (one click): below `day_gain` it stays day, otherwise back to night (a
  second click).
* **Jump bar** (**since v1.10.2**; before: always 2 × `day_gain`): every
  look→check pair teaches `k` = check reading / dark look reading (median of
  the last 5, clamped 0.7..2; the fleet reads 1.0-2.0 under lamps, 4-20 under
  other cameras' infrared). The camera also keeps the dark readings of the
  last 4 failed checks; a check that finds day drops those at or below its own
  dark reading. A jump look asks below 0.9 × the lowest kept failed dark
  reading, at most 2 × `day_gain`, at least `day_gain / k`. So the same lamp,
  TV or neighbouring infrared does not cost a click pair at every jump, while
  a brighter light still asks. Nothing failed yet (after boot): 2 × `day_gain`.
  `k` and the failures survive jumps and day, not a restart; the `look (...)`
  debug line logs `k`.
* **After a failed check** the next one needs a darker look: the bar becomes
  `dark × day_gain / day reading` (at least `0.5 × dark`, so a light that
  halves the dark reading asks again), or `0.85 × dark` when the check read the AE
  at its limit or above 4 × `night_gain` (the Eufy reads 40960 in the dark
  without a reported limit). A jump forgets the bar (new light). Checks are
  locked out for 1, 2, 4, 8, 16 min after consecutive failures, then 16 min
  each (**since v1.10.2**, before: doubling without limit); a day resets it.
* **Guarantee**: no day-pipeline reading for `heartbeat_max_s` (12 h) → a
  check, regardless of looks and lockouts.
* **Without a switchable illuminator** (`irprobe_cmd` empty or failing) the
  camera does not look: it checks when the night level halves (held 20 s) and
  every `heartbeat_s` (4 h).
* **Boot** is a check (one click; two in the dark).
* **Idle-Dark** (`ir_idle_off`) is a look that lasts while nobody watches.

Costs: 8 s of dark night video per look (0.3 % of a steady night); clicks per
day typically 2-6, worst case 28 (11 failed checks by the doubling lockout +
2 guarantee checks + 2 real switches) plus genuine light changes.

### Retired `daynight.*` keys

Parsed and **ignored** with one warning each (`… is retired and IGNORED`), so an
old config loads without "unknown key" lines:

`day_gain_pct`, `baseline_delay_s`, `boot_settle_max_s`, `boot_stable_pct`,
`night_reconfirm_s`, `probe_max_skip_s`, `threshold_low`, `threshold_high`,
`hysteresis`, `learn`, `state_path`, `probe_jump_pct`, `probe_settle_s`,
`ref_delay_s`, `ir_min_headroom`, `boot_settle_s`, `transition_s`,
`ir_ratio_night`, `ir_ratio_day`, and **since v1.10.1** `probe_min_gap_s`
(replaced by the doubling lockout), `probe_confirm_s` (fixed 20 s jump hold),
`boot_probe` (every boot checks) and `diagnose_thresholds` (the status shows
the exposure each check read). The `daynight` status object no longer echoes
the old constants.

### `daynight.*` pitfalls

* **Dim but lit rooms stay night.** Day means "the day picture needs less than
  `day_gain` amplification". A room whose day pipeline reads between
  `day_gain` and `night_gain` stays in whatever mode it is in; raise
  `day_gain` for such a camera (measured indoor rooms up to 9.79×).
* **Lamps without a jump take up to one look interval** (45 min): the Wyze Pan
  in a cellar with other IR cameras reads the same illuminator-lit level with
  and without its lamp. `{"daynight":{"probe":1}}` looks at once.
* **Other cameras' infrared** lights the scene without the illuminator but not
  the day pipeline: one failed check (two clicks) per light change.
* **Every boot in the dark costs a click pair.**
* **`image.ae_it_max_us` moves the index** — a camera running it needs its
  `day_gain`/`night_gain` re-checked rather than inherited.
* `daynight.enabled = 0` does **not** force a mode; it just stops the automaton.
  Use `image.running_mode` for a manual mode.
* The calendar (`time_*`, `sun_*`) is used by `mode = schedule` only.
* `trace_path` on flash (not tmpfs) wears the flash; that is why it warns.
* `{"daynight":{"probe":1}}` over `/control` requests one look on the next tick;
  it is **rejected** if `irprobe_cmd` is empty.
* `daynight.ir_idle_off = 1` (Idle-Dark, **since v1.10.0**, off by
  default): at night with no picture consumer for `ir_idle_linger_s` the IR
  LED is **off**; the IR-cut and the ISP stay in night. What the user sees:
  the LED (850 nm glow) goes out a minute after the last viewer, a new viewer
  gets a dark or overexposed first 1-4 s of video while the AE settles on the
  LED, and `/snapshot.jpg` answers up to ~3.5 s later than usual (it waits for
  the AE; capped at 5 s). The glow going out also tells an onlooker that
  nobody watches or records, which is why it is off by default.
  Pitfalls:
  - Cameras with a permanent consumer (Frigate/NVR, `record.enabled`,
    `motion.enabled`, `jpeg.snapshot_path`) never go idle: no effect.
  - Needs the keepalive (`general.fs_keepalive`; auto only on OpenIMP
    T23/T41): without it the AE does not run while idle.
  - Needs `timps-irprobe` with the `idle` and `state` verbs (firmware from
    2026-10 on). An older hook answers `idle` with exit 1/2: one WARN,
    `reason = "hook"`, the LED is left alone. A white light enabled in
    `daynight.controls` makes the hook refuse (exit 3): `reason =
    "white_light"`.
  - While idle-dark, a check (one click) runs when the reading without the LED
    stays below the bar for 30 s; nobody is watching.
  - To turn it off: POST `{"daynight":{"ir_idle_off":0}}`; the LED comes back
    on at once.
  - If `timpsd` dies while the LED is off, the hook's `idle` 900 s watchdog
    (renewed every 600 s) relights the LEDs.
  - Keep `ir_idle_linger_s` longer than `timelapse.interval_s` (see the key).
  `reason` in `daynight.ir_idle`: `disabled` (key 0 or `daynight.enabled =
  0`), `schedule` (`daynight.mode = schedule`), `keepalive`, `no_illuminator`
  (no `irprobe_cmd` or looks were turned off after failures),
  `white_light`, `hook` (hook without `idle` or nothing to switch).
---

## 15. `rtsp.*`

Whole section is **file-only** (`noget` + no `F_CTRL`): not POST-able, not
echoed by `GET /control`. Changes need a restart.

| Key | Type | Default | Range | Apply | Notes |
| --- | --- | --- | --- | --- | --- |
| `rtsp.enabled` | bool | `1` | — | file-only, restart | |
| `rtsp.port` | int | `554` | 1..65535 | file-only, restart | |
| `rtsp.mtu` | int | `1200` | **548..1472** | file-only, restart | Max RTP packet size (header + payload) for UDP packetization. 1200 leaves room for WireGuard/OpenVPN/PPPoE/IPv6 tunnel overhead; raise to 1400 for LAN-only setups. |
| `rtsp.user` | string[64] | `""` | — | file-only, restart | Alias `rtsp.username`. **Empty = RTSP is open, no authentication.** Non-empty enables Digest **and** Basic (the `401` offers both). |
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
| `http.allowed_hosts` | string[256] | `""` | — | file-only, restart | **since v1.10.1** Extra `Host` header names the HTTP port answers (DNS-rebinding guard, see below). Comma/space list: `cam.example.net` exact, `.example.net` that domain and its subdomains, `*` any (turns the guard off). Names only, no port. Needed only behind a reverse proxy or a DNS name that does not start with the camera's hostname. |
| `http.https` | tri-state int | `0` | 0..2 | file-only, restart | Alias `http.tls`. `0` = plain HTTP only. `1` = **both** schemes on the same port, sniffed per connection (`0x16` = a TLS handshake record; every HTTP method starts with a letter). `2` = TLS only, plaintext refused with 426. Legacy `true`/`on`/`yes` parse as `1`. **`F_SECVAL`.** Needs `USE_TLS`. |
| `http.tls_cert` | string[128] | `/etc/ssl/certs/timps.crt` | — | file-only, restart | PEM certificate. |
| `http.tls_key` | string[128] | `/etc/ssl/private/timps.key` | — | file-only, restart | PEM private key. |

Important behaviours

* **Note the widening in v1.9.11:** `http.https = 1` used to mean TLS-only. It
  now means "both". An operator who wants the old strict behaviour must say
  `2`. This is the opposite polarity to `audio.talk_ws`, where `1` is the
  *stricter* value.
* **Host gate (since v1.10.1).** Every non-loopback request whose `Host`
  header is not an IP literal, `localhost`, the camera's hostname,
  `<hostname>.<anything>` (`.local`, router DNS) or an `http.allowed_hosts`
  entry gets `421 host not allowed` (DNS rebinding: a page on a foreign name
  re-resolved to the camera). Requests without `Host` (HTTP/1.0) pass. The
  OPTIONS preflight echoes `Access-Control-Allow-Private-Network: true` only
  for a same-host `Origin` or a valid `?token=`.
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
| `webrtc.enabled` | tri-state int | `2` | 0..2 | file-only, restart | `0` = `/webrtc/whep` answers `503` (body `webrtc.enabled=0`; a 404 means the build has no WebRTC or the DTLS context failed). `1` = on, TLS required for the signalling POST where the HTTP port has it. `2` = on and accept a plaintext POST too. Default is `2` on purpose: most cameras have no http→https redirect, so `1` would silently 426 the feature it enables. Legacy `true`/`on`/`yes` parse as `1`. |
| `webrtc.port` | int | `0` | 0..65535 | file-only, restart | UDP media port. `0` = ephemeral. |
| `webrtc.port_max` | int | `0` | 0..65535 | file-only, restart | Top of the media port range. `0` = `webrtc.port + WEBRTC_MAX_SESSIONS - 1` (one port per session slot; `WEBRTC_MAX_SESSIONS` is 4). |
| `webrtc.channel` | int | `0` | **0..1** (`MS_MAX_VSTREAM-1`) | file-only, restart | Which video stream to send by default; `POST /webrtc/whep?chn=N` overrides it per session (`400` if stream N is not running or not H.264). |

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
| `srt.passphrase` | string[64] | `""` | — | file-only, restart | Optional AES passphrase (10..79 chars, else SRT refuses to run); `""` = none. **since v1.9.28** a listener with no passphrase while `rtsp.user`/`http.user` is set logs `SRT port N has NO access control - set srt.passphrase (10-79 chars)` at start: it serves video to anyone, whatever the other credentials demand. |

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
| `sensor.i2c_adapter_id` | `sensor.i2c_adapter` |
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

Source: `<thingino-firmware>/package/timps/Config.in` and
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

**Link-time optimisation (since v1.10.1).** `USE_LTO ?= 1` in the upstream
`Makefile` appends `-flto=auto` to the compile and the link step inside the
recipe, so it survives the package's own `CFLAGS`/`LDFLAGS`. `timps.mk` does
not pass it, so firmware builds get LTO (stripped `timpsd` ~6 % smaller,
measured 24-28 KB on T10-T41). `USE_LTO=0` restores the per-file build
byte for byte. Needs GCC 10+ (`-flto=auto`) and a linker with plugin support.
Weak libimp imports stay weak undefined and keep their NULL checks.

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
* **Since v1.10.0** `start` runs `timpsd --check-libs` first and
  does not start the daemon on exit 78/79 (libimp does not fit the build;
  each output line and a `timpsd not started (exit N)` line logged with
  `logger -t timps -p daemon.err`, returns 1). Any other result (80 = could
  not check, logged at `daemon.warn`) starts as before. A `timpsd` whose
  binary does not contain the string `--check-libs` is not asked at all
  (before v1.9.30 an unknown option started the daemon in the foreground).
* Commands: **`start | stop | restart | status`**. There is **no `reload`** —
  live reconfiguration is `/control`'s job, and anything restart-required needs
  a full `S95timps restart`.
* `stop` and `restart` wait for the old process to go (`wait_stop()`), because
  IMP/rmem teardown takes a while and starting a new instance too early makes
  ISP init fail. `wait_stop()` checks once whether busybox `usleep` works and
  loops **50 × 100 ms** with it or **5 × `sleep 1`** without it — ~5 s
  either way. (Firmware before 2026-09-19 looped 50 times over
  `usleep 100000 2>/dev/null || sleep 1`, i.e. 50 s without `usleep`.)
* `stop` sends `SIGKILL` when the first `wait_stop` times out (past timpsd's
  own 3 s shutdown guillotine the process is wedged; the kill releases the
  `/run/timps.lock` flock), then **removes the pidfile only once the process
  is really gone**; otherwise it prints `timpsd still stopping...` and keeps
  it, so `restart`'s second `wait_stop` still waits. (Firmware before
  2026-09-19 deleted the pidfile unconditionally, and `restart` could land on
  a still-exiting instance; see `docs/ai/troubleshooting.md` §11.6.)
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
     present the same certificate and one browser trust decision covers both)
     and log `TLS: sharing the web UI certificate (...)` via `logger -t timps`.
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
   to its section prefix from `g_sections[]` — including the four indexed
   tables (`video_fields` → `video<N>.`, `osd_item_fields` → `osd<S>.<N>.`,
   `privacy_fields` → `privacy<S>.<N>.`, `roi_fields` → `roi<S>.<N>.`).
2. It adds the keys handled by explicit code in `set_kv()` outside the tables:
   `general.syslog`, `general.trace`, `general.trace_ms`, the four deprecated
   `motion.roi_*`, and the retired/now-constant `daynight.*` names extracted
   from the `gone[]`, `gone_i[]` and `gone_f[]` arrays.
3. It extracts every `key = value` line from `timps.conf.example`, including
   commented-out ones.
4. It checks each key appears literally in this file, or matches a documented
   index pattern (`video<N>`/`videoN`/`video0`, `osd<S>.<N>`/`osdS.N`/`osd0.0`,
   `privacy<S>.<N>` …, or a leaf name in that section's table).

**Result: 247 canonical config keys documented, 0 missing.** v1.9.33 had 226
(200 table keys); v1.10.0 adds 21: `general.fs_keepalive`,
`daynight.ir_idle_off`, `daynight.ir_idle_linger_s`, the five `image.fcrop_*`,
the seven `roi<S>.<N>.*` and the six T40/T41 `sensor.*` wiring keys
(`i2c_adapter`, `mclk`, `video_interface`, `default_boot`, `rst_gpio`,
`pwdn_gpio`).
The breakdown:

* **221** from the `cfg_field` tables — `sensor` 11, `image` 30, `audio` 24,
  `jpeg` 7, `rtsp` 7, `http` 11, `webrtc` 4, `events` 3, `general` 5, `sim` 4,
  `srt` 8, `osd` 5, `motion` 9, `record` 10, `timelapse` 6, `daynight` 25,
  `video<N>` 25, `osd<S>.<N>` 14, `privacy<S>.<N>` 6, `roi<S>.<N>` 7.
* **7** handled by explicit code in `set_kv()` outside the tables:
  `general.syslog`, `general.trace`, `general.trace_ms`, `motion.roi_x/y/w/h`.
* **19** retired or now-constant `daynight.*` names that the loader still
  recognises and warns about (11 removed, 8 turned into fixed constants).

Plus **28 aliases** (§21), all present, and **260 key occurrences extracted from
`timps.conf.example`** (active and commented out), all covered — 0 missing.

Counting notes:
* Indexed keys are counted **once per pattern**, not per index: the 25
  `video<N>.*` keys, 14 `osd<S>.<N>.*` keys, 6 `privacy<S>.<N>.*` keys and
  7 `roi<S>.<N>.*` keys are one entry each. Expanded per index the file would
  describe 25×2 + 14×(2 streams × 8 items + 8 legacy) + 6×(2×4) + 7×(2×4) =
  far more lines for no extra information.
* `webrtc.*` (4 keys) only exists in a `USE_WEBRTC` build; it is counted because
  the table exists in the source.
* Limits used for the index ranges, all from `src/config.h`:
  `MS_MAX_VSTREAM = 2`, `MS_MAX_OSD = 8`, `MS_MAX_PRIVACY = 4`, `MS_MAX_ROI = 4`,
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
| **T20** (old SDK) | The ISP dump is `/proc/jz/isp/isp_info`, not `isp-m0` — `isp-m0` does not exist in that SDK and never will. `daynight.c` carries an explicit fallback list, so `daynight.isp_path` does not have to be changed, and the `is not readable, using … instead` line there is expected (INFO since v1.9.26, a WARN before that). | `src/daynight.c` |
| **T10** | `jpeg.quality` / `videoN.jpeg_quality` are **never applied** — custom quantization tables are known to degrade JPEG quality on T10, so the SDK default is kept and one WARN is logged. | `src/hal/hal_ingenic.c` |
| **T10, T20, T21, T30** | No `ISP_HAS_SENSOR_ATTR` (`GetSensorAttr`), so the framesource input geometry comes from `sensor.width`/`sensor.height` or, when those were auto, from `/proc/jz/sensor` read after `IMP_ISP_EnableSensor` (since v1.10.0). A sensor driver that reports `0x0` even then cannot be compensated for (WARN at start). | `src/isp_caps.h` |
| **T31 only** | `encoder.<n>.ave_bitrate` appears in `GET /control?stats=1` (`IMP_Encoder_GetChnAveBitrate`). Elsewhere the key is simply absent, not zero. | `src/hal/hal.h`, `src/control.c` |
| **T41** | No `IMP_Encoder_SetChnAttrRcMode` at all (`ENC_HAS_SETRCMODE` undefined) — only `bitrate` and the QP bounds can be touched live. T40 **does** attempt the live call. | `src/hal/hal_ingenic.c` |
| **T40, T41** | Flips go through `IMP_ISP_Tuning_SetHVFLIP` instead of `SetISPHflip`/`SetISPVflip`, and `g_sensor.rst_gpio`/`pwdn_gpio`/`power_gpio` are forced to `-1` (the fields are plain `int` in that SDK). | `src/hal/hal_ingenic.c` |
| **T31/C100 vs T40/T41** | `ENC_HAS_QPIPDELTA` (hence a working `videoN.i_bias_lvl`) is T31/C100 only. | `src/hal/hal_ingenic.c` |
| **T31** | The `isp_ch0_pre_dequeue_time` one-buffer gate on framechan0 — see `videoN.buffers` in §5. | `src/hal/hal_ingenic.c` |
| **all** | **Since v1.10.0** encoder ROI and ISP FrontCrop are tiered per SoC and libimp flavour (`caps.roi.tier`, `caps.fcrop.tier`): only OpenIMP T10/T20/T21 ROI and FrontCrop and OpenIMP T31 ROI (`SetChnRoiAttr`) are device-tested (by the OpenIMP maintainer). T23 has no ROI before v1.10.1; since v1.10.1 OpenIMP T23 ROI is `effective` when the libimp exports `OpenIMP_Cap_T23HelixRoi`. T23/T31 OpenIMP FrontCrop is `effective` and live when the driver has `fcrop_upscale_pct` (open-tx-isp b15ef235), otherwise experimental (T23 start-only, needs 3cf9bdac+). Table in §10a. | `src/roi_caps.h` |
| **T23, T41 with `USE_OPENIMP`** | **Since v1.10.0** `general.fs_keepalive = -1` (the default) resolves to **on**: the FrameSources run from start to stop, so `framesource N disabled (idle)` never appears and day/night kicks are no-ops. Everywhere else it resolves to off. | `src/hal/fs_ref.h` (`FS_KEEPALIVE_AUTO`) |
| **T23, T41 with `USE_OPENIMP`** | **Since v1.10.0** the only SoCs where `daynight.ir_idle_off` can become available without touching `general.fs_keepalive` (it needs the keepalive). Elsewhere it reports `reason = "keepalive"` unless the keepalive is forced on (untested there). | `src/dn_idle.h`, `src/daynight.c` |
