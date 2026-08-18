# Code Review – microstream

**Date:** 2026-07-18
**Scope:** full review of all sources under `src/` (~12,500 LOC C), build system (`Makefile`, `build.sh`)
**Target platform:** Ingenic SoCs (T31/T33/T40/T41), IMP SDK, a network-exposed streaming daemon running as root

## Overall verdict

For hand-written embedded C network code, microstream is **unusually careful and well-hardened**. Buffer bounds are checked explicitly almost everywhere, several rounds of auditing have already been completed (visible from the M-/L-/H- comments in the code), and many classic pitfalls are already closed off: constant-time auth comparisons, `/dev/urandom`-based tokens and nonces, atomic config persistence (`mkstemp`+`rename`+`fsync`), symlink/`..` protection on clip export, a correctly built producer-consumer system with a clean lifetime handshake (hub/fanqueue), and a remarkably defensive TTF parser.

**No critical memory-corruption bugs were found in the direct network input path.** The real residual risks concentrate on three themes:

1. **Availability (DoS):** socket timeouts are missing across the board, combined with small connection caps -> a cheap, persistent DoS against RTSP and HTTP.
2. **Silent continuation on security-relevant errors:** ignored return values for SRT encryption and IMP SDK calls.
3. **Unchecked value ranges** that can be set live via the (authenticated) `/control` interface — up to and including potential heap corruption in the OSD renderer.

Priority before an internet-exposed deployment: the two **HIGH DoS** items (timeouts), the **SRT passphrase** error path, and the **`font_size`/OSD canvas** bounds.

---

## Findings by severity

### HIGH

**H1 – Missing socket timeouts -> trivial, persistent slot-exhaustion DoS (RTSP)**
`rtsp.c` (control phase, ~616-635) and `net_sendall()` (`net.c:45`) block indefinitely. An unauthenticated attacker who opens `RTSP_MAX_CLIENTS` TCP connections and stays silent (or holds the TCP window at 0) occupies all slots permanently (global counter `g_nclients`, applies to RTSPS as well). RTSP is then dead for all legitimate clients.
*Fix:* set `SO_RCVTIMEO`/`SO_SNDTIMEO` on accepted fds (e.g. 30 s control, 10-15 s send), close on timeout; alternatively `poll()` with a deadline.

**H2 – Missing socket timeouts -> DoS (HTTP)**
Two spots: the `/control` body follow-up loop (`httpd.c:878-882`) has no poll deadline — unlike the header phase; and all `csend` paths via `net_sendall` lack a send timeout (open `/stream.mp4` and never read -> thread pinning). With `HTTP_MAX_CLIENTS 8`, 8 connections suffice for a complete HTTP DoS. **Reachable unauthenticated on a password-less setup** (`http_check_auth` then returns 1).
*Fix:* give the body loop the same poll deadline as the header phase; set `SO_SNDTIMEO`/`SO_RCVTIMEO` in `accept_thread`. Legitimate streaming clients read continuously and are not affected.

**H3 – SRT: error from `SRTO_PASSPHRASE` is ignored -> silently runs unencrypted**
`srt.c:355` does not check the return value of `srt_setsockflag(..., SRTO_PASSPHRASE, ...)`. libsrt requires 10-79 characters; with too short a passphrase the call fails and the listener keeps running **unencrypted and without access control**, even though the user believes the passphrase is in effect.
*Fix:* check the return value; on error, do not start the listener (`LOGE` + `return`), analogous to the bind/listen error path.

**H4 – Integer overflow / missing upper bound on OSD canvas allocation -> possible heap corruption**
`msttf_render` (`msttf.c:378-384`) computes `W`/`H` without an upper bound and allocates `malloc((size_t)W*H*4)`, while the fill loops compute in `int` (signed overflow = UB). `font_size` is taken over unchecked while parsing (`config.c:314: o->font_size=pint(val)`) and is settable **live** via `/control`. A very large value (or a corrupt font with `units_per_em=1` and large advances) leads to wraparound/underallocation and writes past the buffer.
*Fix:* hard-clamp `pixel_h` in `msttf_render` (e.g. 8..512), check `W`/`H` against a limit (e.g. 4096), compute the size in `uint64_t` (overflow -> `return -1`); clamp `font_size` in `config.c` while parsing (e.g. 8..256); use `size_t` for loop indices.

**H5 – OSD region coordinates not clamped to frame size -> SDK-dependent OOB in compositing**
`imp_osd.c` (~191/211/244): if the rendered OSD bitmap is wider/taller than the stream (large `font_size` on a substream, or `logo_w/logo_h` larger than the stream size), `ox=0` is set, but `rect.p1.x = x+w-1` ends up outside the frame. On several T-SoCs, IMP_OSD then writes past the image boundary. `setup_cover` clamps correctly — `refresh_text`/the logo path does not.
*Fix:* clamp `w`/`h` to the frame size before `SetRgnAttr` (discard or scale the region if it does not fit).

**H6 – IMP SDK return values consistently unchecked -> silent failures**
`hal_ingenic.c` (among others 498-499, 551, 823-826, 1305-1320): `IMP_FrameSource_SetChnAttr`, `IMP_Encoder_RegisterChn/CreateGroup`, `IMP_System_Bind`, most `IMP_ISP_*` calls in `isp_init`, `Start/StopRecvPic`. On failure the pipeline appears to keep running but never delivers frames — exactly the class of error that is handled deliberately in the audio path.
*Fix:* check at least the bind/register/create chain in `ing_start`/`jpeg_setup` and `return -1` with teardown on error.

### MEDIUM

**M1 – TLS handshake without timeout** (`tls.c:92-97`). Blocking fd; a client that stays silent after the TCP connect hangs the thread forever (compounds H1/H2). *Fix:* `SO_RCVTIMEO` before `ms_tls_accept()`, or `mbedtls_ssl_conf_read_timeout()` + `mbedtls_net_recv_timeout`.

**M2 – No TLS minimum version** (`tls.c:59-63`). `MBEDTLS_SSL_PRESET_DEFAULT` still permits TLS 1.0/1.1 under mbedTLS 2.x. *Fix:* `mbedtls_ssl_conf_min_version(..., TLS 1.2)`.

**M3 – Use-after-free on the TLS context during shutdown** (`rtsp.c:752-757`). `rtsp_stop()` waits only a bounded 500 ms for `g_nclients==0` and then calls `ms_tls_ctx_free()`; detached client threads (which, due to missing timeouts, can hang indefinitely) keep referencing `conf`/`cert`/`drbg` afterward. *Fix:* real synchronization (refcount) or joinable threads; at minimum, tie the bounded sleep to a hard `shutdown()` of the client fds.

**M4 – SRT `SRTO_STREAMID` on the listener is ineffective as access control** (`srt.c:353`). Streamid is caller-side; the streamid of accepted sockets is never checked. *Fix:* register `srt_listen_callback()` and compare the incoming streamid against `cfg->srt.streamid`.

**M5 – Digest auth: `realm` and `uri` from the client are unchecked** (`auth.c:52-92`). The client-supplied `realm` flows into HA1 instead of enforcing `AUTH_REALM`; `uri` is never compared against the request URI (an RFC 2617 requirement); no `qop`/`cnonce`/`nc`. Practical impact is limited (nonce is per-connection random, renewed on every 401), but the uri check costs nothing. *Fix:* require `strcmp(realm, AUTH_REALM)` and `strcmp(uri, request_uri)`.

**M6 – Predictable random values from `rand()` (seed `time^pid`)** (`main.c:84`, `rtsp.c:366`, `rtp.c:41-43`). Session ID, SSRC, start sequence number, ts_base are predictable -> lowers the bar for off-path RTP injection over the UDP transport. Tokens/digest nonces have already been switched to urandom. *Fix:* use `getrandom()`/`auth_gen_token()` for these as well.

**M7 – Recorder: no `fsync`, `fclose` unchecked -> data loss on power failure** (`record.c:281-287`). `fclose(w_fp)` may fail to write the last flush (SD full/removed), and the return value is ignored; `fsync()` is never called -> up to `segment_s` seconds of recording can be lost from the page cache. *Fix:* check the `fclose` return value; periodically `fflush`+`fsync(fileno(w_fp))` (e.g. every 5-10 s / N fragments).

**M8 – Resource leak / inconsistent state in `ing_start`/`ing_stop` error paths** (`hal_ingenic.c:1298-1332`, 1394-1431). If `fs_create`/`enc_create` fails for stream i>0, `ing_start` returns `-1` without tearing down the FrameSources/encoders/groups/threads already created; `g_nv` only counts slots with a successfully started thread, so previously created IMP channels are skipped (leaked) in `ing_stop`. *Fix:* decouple channel teardown from thread existence; free cleanly on error (or make `ing_stop` idempotent and call it).

**M9 – OSD `retired` double-buffering does not reliably prevent use-after-free** (`imp_osd.c:158-201`). Only exactly one old buffer is held back; without a real "buffer no longer being read" signal from the IMP pipeline, `SetRgnAttr` can, under load, set a new buffer while the hardware is still reading the one just released. *Fix:* increase ring depth (2-3) or tie it to frame latency.

**M10 – Config fields read lock-free in `refresh_text` while `/control` writes** (`imp_osd.c:161ff`). Only `it->text` is snapshotted under `config_str_lock`; `font_size/x/y/color/outline/enabled` are read lock-free -> inconsistent combinations are possible (relevant in conjunction with H4). *Fix:* take the entire item snapshot under `config_str_lock`.

**M11 – Config parser: missing range validation of numeric values** (`config.c:278-301` among others). `width/height/fps/gop/qp/bitrate/.../rtsp.port/http.port` go through `pint()` (strtol without error checking, "abc"->0) unchecked; inconsistent, since `motion.*`/`record.channel`/`osd.supersample` are properly clamped. Persistable via `/control` -> a bad value can permanently break the stream (HAL init fail -> `main` returns 1 -> crash loop under respawn). *Fix:* clamp analogous to `motion.*` (fps 1..sensor-max, qp 1..51, port 1..65535).

**M12 – Day/night logic uses the wall clock instead of CLOCK_MONOTONIC** (`daynight.c:286`, 302-305). `time(NULL)*1000` for dwell/baseline; an NTP step-back after boot makes deltas negative -> switching blocks, while a step-forward defeats the minimum dwell time. The rest of the code already uses monotonic time. *Fix:* `ms_now_us()/1000`.

**M13 – `config_get_kv` does not cover `record.*` -> dedup ineffective, flash wear** (`config.c:623-838`). control.c sets `record.*` via `/control`, but change detection always reports them as "unknown" -> every POST to the record page rewrites `/etc/timps.conf` (jffs2 wear that the dedup is supposed to prevent). *Fix:* add a `record.*` branch to `config_get_kv`.

**M14 – Build: no hardening flags** (`Makefile:93-95`, `build.sh:273-283`). No `-D_FORTIFY_SOURCE=2`, no `-fstack-protector-strong`, no `-Wl,-z,relro,-z,now`. `-no-pie` is forced by non-PIC vendor archives (understandable). For a network service running as root, compiler defense-in-depth is missing; `-Wno-stringop-truncation` also globally suppresses a useful warning class. *Fix:* enable SSP + FORTIFY + RELRO at least for musl builds.

**M15 – Toolchain download without integrity check** (`build.sh:85-104`). The cross-toolchain tarball is downloaded from GitHub and executed without SHA256/signature verification — unlike the otherwise exemplary commit-pinned repos. Supply-chain risk. *Fix:* pin a SHA256 and verify it after download.

**M16 – msttf: scanline intersections hard-capped at 128** (`msttf.c:437`). Further crossings are discarded; with an odd number of retained crossings, even-odd fill fails, producing an incorrectly rendered scanline for dense contours (not a memory error). *Fix:* grow `xint` dynamically, or round `nx` down to an even number.

### LOW (selection)

- **L1 – RTSP client cap check-then-act is not atomic** (`rtsp.c:669`): the cap can be exceeded by 1. Fix: `__sync_add_and_fetch` before accepting.
- **L2 – `send_resp()` does not clamp `n` to the buffer size** (`rtsp.c:273`): latent, currently unreachable; clamp `n` after snprintf.
- **L3 – RTP: sink errors in `emit()` are discarded** (`rtp.c`): remaining NALs are still packetized despite a dead client (wasted CPU).
- **L4 – SRT double `srt_close(g_ls)`** (`srt.c:393` vs. 413): race on socket-ID reuse.
- **L5 – `record_clip` `fclose` unchecked** (`record.c:545`): a truncated clip is reported as success.
- **L6 – `seg_open` fwrite error leaves a stub file behind** (`record.c:230`): add `unlink(path)` in the error path.
- **L7 – TLS-buffered data invisible to `poll()`** (`httpd.c:757`, TLS builds only): check `mbedtls_ssl_get_bytes_avail` before poll.
- **L8 – Signal handler installed only after service start** (`main.c:112`): SIGINT/SIGTERM during IMP init -> abort without HAL teardown. Register the handler before `init()`.
- **L9 – `config_load` silently truncates lines >511 characters** (`config.c:868`): detect a missing `\n`, discard the rest + `LOGW`.
- **L10 – `record.name`/`timelapse.name`/`*.dir` via `/control` without `..` check** (record.c:192, timelapse.c:126): an authenticated user can write outside the records tree (no privilege escalation, but a hardening gap — adopt the `..` check used in `record_clip`).
- **L11 – `hub->nsub` read for logging after the lock is released** (`hub.c:161/183`): benign race.
- **L12 – `ms_base64` declaration outside the include guard** (`util.h:51`): harmless, but an oversight.
- **L13 – `strftime` with a partially user-controlled format** (`osd_vars.c:200`): not a memory error, but unexpected output; consider whitelisting `%` conversions.
- **L14 – `fmp4` `mfhd` seq (32-bit) / `a_timescale<<16`** (fmp4.c:369/261): only relevant after >2 years of continuous connection or >65535 Hz; document/clamp.
- **L15 – `hal_sim` does not cover piggyback JPEG** (hal_sim.c:199): test backend not on par with `hal_ingenic`.

---

## Explicitly checked and found OK

- **md5.c** – a correct, clean MD5 implementation.
- **Auth core** – constant-time comparisons, urandom tokens/nonces, correct buffer sizes; `/control` and `/events` are behind a global auth gate + "local only" when no user is configured.
- **HTTP parsing** – bounded `sscanf`, CR/LF stripping (no response splitting), truncation guards; **no path traversal** (the server does not serve files from the filesystem).
- **fMP4 box structure** – box-size patching guarded against OOM subtrees, esds/trun/trex/tfhd correct, tfdt cleanly 64-bit, `aac_adts_strip` bounds-safe.
- **hub/fanqueue/frame** – the publish/unsubscribe handshake prevents use-after-free, refcounting is consistent, overflow behavior is well thought out (drop-oldest + GOP pruning + IDR request), producer never blocks.
- **NAL/vparam/aac/g711** – bounds-safe, defensive Exp-Golomb reader.
- **config_write_keys** – atomic (mkstemp+rename, fsync on file and directory, writer mutex) — exemplary for jffs2.
- **msttf bounds checks** – consistently defensive against corrupt fonts (loca/glyf/cmap bounds, recursion limit on composite glyphs, OOM handling). Exception: see H4/M16.
- **record_clip** – path validation (`/tmp/` prefix + `..` filter), seconds clamp, serialization via trylock.
- **`on_motion`/`switch_cmd` (`system()`)** – deliberately not settable via `/control` -> no injection vector, as long as the config file is trusted.

---

## Recommended fix order

1. **H1, H2, M1** – socket/handshake timeouts (`SO_RCVTIMEO`/`SO_SNDTIMEO` + body poll deadline). Highest impact for the least effort.
2. **H3, M4** – handle the SRT passphrase error, check streamid server-side.
3. **H4, H5, M10, M11** – clamp value ranges (`font_size`, config numerics), bound the OSD region to the frame size, snapshot items under lock.
4. **H6, M8** – check IMP return values and clean up error paths properly.
5. **M7** – `fsync`/`fclose` checking in the recorder (data integrity).
6. **M14, M15** – build hardening + toolchain integrity.
7. Remaining MEDIUM/LOW items as capacity allows.
