# Independent Code Review – timps

**Date:** 2026-08-07
**Scope:** complete `src/` tree (~17,700 LOC C) @ `main` (HEAD `074e8f5`, v1.7.8), focused on all changes since the last audit sweep (2026-07-26, `ad0364a`), especially the **unreviewed ~65 commits from 2026-07-27 through 2026-08-06** (speaker.c, backchannel.c, Opus RTSP, adaptive-drop, HTTP digest, rotate hardening, daynight probe economy) as well as the **same-day** commits `10a192a`/`074e8f5` (C11 data-race hardening, v1.7.8).
**Basis:** three independent static passes (1. C11 data-race sweep across all worker threads, 2. fresh-eyes review of the new code `073f652..HEAD`, 3. re-verification of all prior findings from `CODE_REVIEW.md`, `CODE_AUDIT_2026-07-18.md`, `SECURITY_AUDIT_2026-07-23.md`) plus a dedicated deep review of the data-race commit `10a192a` and a build/regression run (`make sim`, `make test-auth`).

As with the prior audits: the commits from 2026-08-06 originate from the same agent that commissioned this review — they were therefore independently re-verified with full skepticism, not just rubber-stamped.

---

## Overall verdict

The current code state remains **very good**. **Not a single regression** compared to the earlier audits; several old residual items (N1, N2, N4, N6, M5 remainder) have since been **closed**. The large new body of code since late July (speaker/backchannel, Opus, adaptive-drop, HTTP digest, buffer growth, rotate refusals) is **memory- and auth-clean** — no CRITICAL, HIGH, or memory-relevant MEDIUM finding.

The most notable new finding concerns, of all things, the most recent commit: the hardening commit `10a192a`, described as a "complete C11 data-race sweep," is **incomplete**. What it does touch (audio.mute `_Atomic`, daynight snapshot, OSD `.enabled`) is **correctly implemented** — but the same race class continues to exist unchanged in record.c, timelapse.c, the status accessors, and the GET /control path (details under A1). In practice this is benign on MIPS32 with aligned loads; formally it remains UB, and the "complete sweep" claim in the commit message does not hold.

Builds: `make sim` compiles **warning-free** under `-Wall -Wextra` (host, C11); `make test-auth` runs **4/4 PASS** (RTSP fail-closed confirmed). The cross build also uses `-std=c11`, so `_Atomic` is safe on the target as well.

---

## 1. Verification of prior findings (status in current code)

All line numbers re-verified against HEAD `074e8f5` (mechanism checked, not old lines taken on faith). **No regression.**

| ID | Topic | Status |
| --- | --- | --- |
| H1 | RTSP socket timeouts | **confirmed** – `net_set_timeouts(cfd,30,15)` (rtsp.c:1376) |
| H2 | HTTP socket timeouts | **confirmed** – httpd.c:1296; body deadline poll httpd.c:1234-1250 |
| H3 | SRT passphrase error | **confirmed** – aborts, no plaintext fallback (srt.c:433-443) |
| H4 | OSD canvas clamps | **confirmed** – msttf.c:372-397; font_size 8..256 (config.c:745) |
| H5 | OSD region discard | **confirmed** – imp_osd.c:273, 310 |
| M1 | TLS handshake timeout | **confirmed** – tls.c:107 |
| M2 | TLS minimum version | **changed, equivalent** – now only `#if MBEDTLS_VERSION_MAJOR<3` (3.x enforces >=1.2 itself, the API was removed) (tls.c:64-69) |
| M3 | TLS shutdown ordering | **confirmed** – registry `shutdown()` strictly before `ms_tls_ctx_free()` (rtsp.c:1459-1485) |
| M4 | SRT streamid check | **confirmed** – srt.c:413-418, 431-432 |
| M6 | urandom instead of `rand()` | **confirmed** – rtp.c:15-27, 69-81; rtsp.c:754 |
| M7 | Recorder fsync | **confirmed** – record.c:327-352 |
| M9/M10 | OSD retire ring / item snapshot | **confirmed** – imp_osd.c:32, 214-218, 226-233 |
| M11 | Numeric clamps | **changed, equivalent** – switch to table-driven config.c (`1e586db`) carries all lo/hi clamps forward (config.c:532-587, 710-718) |
| M12 | Daynight monotonic | **confirmed** – `ms_now_us()` used throughout (daynight.c:592-593, 1197) |
| F-01 | `switch_cmd` via execlp | **confirmed** – daynight.c:185-190; **not a single** `system()`/`popen()` remains in `src/` |
| NEU-01 | `on_motion` double-fork | **confirmed** – imp_motion.c:244-262; context env from `1f10c93` set via `setenv()` in the grandchild, no shell |
| F-03/F-04 | Audio/OSD clamps | **confirmed** – config.c:509-521, 738-750 |
| F-08 | `hal_get()` NULL check | **confirmed** – main.c:109-111, before first use including a log entry |
| N1 (07-18) | Recording duration values | **FIXED** – segment_s 0..86400, pre_roll 0..60, post_roll 1..300, min_free_mb clamped (config.c:642-649) |
| F3 | daynight numerics | **confirmed** – all clamped (config.c:678-702) |
| M5 remainder | Digest uri check | **FIXED** – `strcmp(uri, req_uri)` against the actual request target (auth.c:82, 133; rtsp.c:586, httpd.c:685; commit `2b9a260`) |
| N2 (07-18) | hvcC numTemporalLayers | **FIXED** – now emits `0x0B` (vparam.c:180) |
| N4 (07-18) | RTSP transport values | **FIXED** – interleaved 0..255, client_port 0..65535, `cp==0` -> 461 (rtsp.c:816-843) |
| N5 (07-18) | /control body on timeout | **partial** – oversize/negative Content-Length now returns 413 (httpd.c:1221-1230); the timeout-truncated body is still parsed (httpd.c:1252). Residual risk unchanged, low (authenticated, range-checked). |
| N6 (07-18) | APP() accumulator UB | **FIXED** – `o<cap?buf+o:buf` pattern (control.c:865-868; httpd.c:814-816); GET /control now returns 500 instead of a truncated-JSON body (`878b940`) |
| F-02/F-12 | isp_path prefix | **open (accepted)** – still settable only via the config file (F_NOGET, not in DN_KEYS) |
| F-09 | getrandom() fallback | **open (accepted)** – urandom primary, documented weak emergency fallback (auth.c:194, 214-218) |
| F-10 | UDP ports via rand() | **open (accepted)** – rtsp.c:788, 861, not security-relevant |
| F-11 | send_resp stack buffer | **changed** – now hdr[4096] with a hard truncation-drop instead of silent truncation (rtsp.c:517, 538; `f3160cd`); still stack-based, accepted |

---

## 2. Deep review of the 2026-08-06 commits (`10a192a`, v1.7.8)

All four sub-changes independently traced through:

- **`F_ATOMIC`/`audio.mute` — correct.** Field is `_Atomic int` (config.h:74), flagged in the field table (config.c:517); `field_set()`/`field_get()` route through `atomic_store`/`atomic_load` (config.c:826-832, 876-879). All three direct readers (hal_ingenic.c:2349, hal_sim.c:141, control.c:1060) read the `_Atomic` field directly -> implicit atomic loads. No `int*` cast bypassing the field, no concurrent memcpy/struct copy of `ms_audio_cfg`; the one whole-struct copy (`config_snapshot_boot`, config.c:26) runs single-threaded before thread start. The default assignment at config.c:262 is pre-thread. The cross build uses `-std=c11` (build.sh:309) — `_Atomic` is available on the target.
- **Daynight snapshot — correct and behavior-equivalent.** The snapshot (`dncfg` + `running_mode`) is taken at the top of the loop **before every use**, under `config_str_lock()` (daynight.c:696-702); not a single remaining raw `g_cfg.daynight.*` read in the thread (verified via grep: only :588-591, :664, :699-700 remain, all under lock). The signature changes (`dn_day_trigger(dn,...)`, `dn_status_update(dn,...)`, `dn_switch(mode,why,cmd)`) were carried through at **every** call site. The probe economy logic (v1.7.7) was reviewed at the same time: backoff-cap arithmetic, oscillation ring (indices correct after reset), fail-ratchet (`probe_fail_smooth` correctly captures the pre-probe night value, since `smooth_tg` is only updated while `cur==NIGHT` and is reset only **after** it has been booked) — **no logic error found**.
- **OSD `.enabled` in `refresh_text()` — correct.** Both of the other call sites (imp_osd.c:420 setup, :570 item_apply) still check `enabled` themselves before calling -> the check inside the snapshot is a harmless double-check there; hiding the region on disable still goes through `ShowRgn` (imp_osd.c:566). **No path skips the check, no timing/behavior difference.**
- **Minor changes:** aac.c warning, vparam.c/rtsp.c comments unremarkable. **However:** the new `cmfc`/`cmf2` brands in the ftyp are technically questionable -> finding A3.
- **`fad4f40` (v1.7.7) "persist clamped values":** correct — the canonical AFTER-read under lock feeds HAL, SSE echo, and persistence consistently; the ungated AFTER-read (comment on the osdN.* convergence case) is soundly justified.

---

## 3. New findings

No CRITICAL, no HIGH. One MEDIUM (as a class), the rest LOW/INFO.

| ID | Severity | Topic | Location |
| --- | --- | --- | --- |
| A1 | Medium | C11 sweep incomplete: live-mutable ints still read lock-free | record.c, timelapse.c, imp_osd.c, speaker.c, rtsp.c, daynight.c, control.c |
| A2 | Medium | OSD reads `video[si].rotation` from **live** `g_cfg` instead of `g_cfg_boot` | imp_osd.c:202, 263, 308 |
| A3 | Low | `cmfc`/`cmf2` brands on muxed A/V fMP4 are not CMAF-conformant | fmp4.c:366-371 |
| A4 | Low | Digest nonce ring (32) can be evicted by unauthenticated 401 spam | httpd.c:619 |
| A5 | Low | Digest `uri=` strictness: compatibility risk for path-only clients | auth.c:82, 133 |
| A6 | Info | Two stale comments contradict the code | config.c:42-45; control.c:583 |

**A1 — Lock-free reads of live-mutable config ints (the actual sweep gap).** `10a192a` claims a complete data-race sweep; in fact entire classes remain (the writer is always the /control connection thread, under `config_str_lock`):
- **record.c (the largest single item):** the recorder thread reads *all* `record.*` ints lock-free on every pass — `enabled`/`mode` in `want_run()`/`want_write()` (record.c:391, 398-399), `channel`/`pre_roll_s` (:416-441), `post_roll_s` (:380, per-packet in motion mode), `segment_s` (:536-537), `min_free_mb` (:173-193), `record.audio` (:260, 469-471). The comment "re-read live every pass" (:416) even documents the pattern — it is exactly the "load hoistable out of the loop" scenario that the commit itself uses to justify the `_Atomic` conversion of `audio.mute` (config.h:74). *Failure mode:* formally UB; the compiler could, e.g., hoist `record.enabled` out of the loop and never observe a live disable.
- **timelapse.c:** analogously `channel`/`enabled` (:267-268), `interval_s` (:291), `keep_days` (:116) — the strings are correctly locked, only the ints were missed.
- **speaker.c/hal_ingenic.c:** `spk_enabled`/`spk_volume`/`spk_gain` in `ao_ensure()` (speaker.c:96-103, explicitly "Read live from g_cfg"), `audio.aec` in `hal_ao_open()` (hal_ingenic.c:3133) — cold path (per AO open).
- **rtsp.c:** `audio.bitrate_kbps`/`samplerate`/`channels` in SDP/PLAY (rtsp.c:438, 458, 938) — restart-only keys are nonetheless written into the **live** `g_cfg` by `config_apply_kv`, so the read races too; worst realistic outcome is a mixed `b=AS:` value.
- **Status accessors (GET /control / SSE threads):** `daynight_get_status`/`daynight_sun_status` (daynight.c:1267-1290, including two **float** reads `sun_latitude/longitude` — floats can also practically tear on MIPS if the compiler splits them into two accesses), `record_get_status` (record.c:582-584), `timelapse` (:333-334), the sim stub `motion_get_status` (imp_motion.c:521-529), and the entire int portion of `control_get_json` (control.c:~1037-1110).
- **Concurrent-POST class:** `control_apply_json` is **not serialized** (httpd.c:1252, one thread per connection), and `hub_control()` runs *after* `config_str_unlock()` (control.c:289/327). Two simultaneous POSTs -> the apply path of A reads `g_cfg` lock-free against the locked writes of B (hal_ingenic.c:436, 591, 2041ff, 2490-2491, 2607-2611, 2678-2708; imp_motion.c:118-152; imp_osd.c:344, 554, 595-601).

*Assessment:* all formally UB, practically benign on MIPS32 (aligned 32-bit loads) and in some cases latent for years — hence MEDIUM for the class, not HIGH. *Fix recommendation:* (1) record/timelapse: take a section snapshot under lock once per pass (the daynight pattern), or make the 4-5 loop-gate fields `_Atomic`; (2) a single mutex around `control_apply_json` collapses the entire concurrent-POST class; (3) speaker/AEC/SDP/status accessors: a short lock+copy each. The likely root cause is the **stale comment at config.c:42-45** ("Ints are not covered ... no tearing to worry about"), which directly contradicts the newer, correct comment at config.h:472-476 (-> A6).

**A2 — OSD uses live rotation instead of the boot snapshot.** `osd_rot_place()`/`refresh_text()`/the logo path read `g_cfg.video[si].rotation` (imp_osd.c:202, 263, 308) from the **live** config. `videoN.rotation` is restart-only (`VID_REST`) — which is exactly why `g_cfg_boot` was introduced (commit `288047a`). This is a double defect: (a) the same race class as A1, (b) *functionally*: a `/control` write to `videoN.rotation` re-places/re-pads the OSD starting with the next text update, for a rotation the running encoder does **not** actually produce — the overlay sits misplaced until restart. *Fix:* switch the three sites to `g_cfg_boot` (fixes the race and the desync at the same time).

**A3 — CMAF brands on a multi-track fMP4.** `10a192a` adds `cmfc`/`cmf2` to the compatible-brands of the ftyp (fmp4.c:366-371), with the stated goal of satisfying strict validators (Bento4). But CMAF (ISO/IEC 23000-19) requires **one track per CMAF file**; this muxer writes a video **and** an audio trak into the same movie (fmp4.c:376-377). The brand claim is therefore incorrect for the A/V case — exactly the strict validators it targets would flag it. Browsers ignore compatible-brands, so this is purely cosmetic. *Fix:* only set the brands when exactly one track is active (video-only streams would be conformant), or remove them again.

**A4 — Nonce ring eviction (reachable unauthenticated).** Every 401 response (including failed Basic attempts) mints and stores a new digest nonce in the 32-entry ring (httpd.c:619). More than 32 unauthenticated requests rotate the ring and evict pending legitimate challenges -> an honest digest client gets `stale=true` and needs an extra round trip. Self-healing, no bypass, Basic unaffected — an inherent property of a bounded ring, noted for awareness only.

**A5 — Digest uri strictness.** The new `strcmp(uri, req_uri)` (auth.c:82, 133) correctly closes cross-URI replay, but rejects clients that (against RFC, but common in practice) send only the path instead of the full `rtsp://...` URI in `uri=`; `extract_url` also silently truncates at 512. A compatibility, not a security, risk — check here first if interop problems arise with exotic NVRs.

**A6 — Stale comments.** (a) config.c:42-45 still claims int reads need no lock — a direct contradiction of config.h:472-476 and the presumed root cause of A1; (b) control.c:583 still mentions `system()` for `on_motion`, though the code has long since switched to fork/execlp. Both should be aligned with the current code.

---

## 4. Fresh-eyes result for new code (2026-07-27 -> 2026-08-06) — reviewed and clean

All specifically checked points in the new code are correctly implemented (selection, each verified at the current line):

- **speaker.c:** `rs_fit` growth strictly bounded to the worst case 8k->48k (`SPK_RS_MAX`=49152), `ms_resample` clamps to `out_cap`, `rn>0` guards at both write sites (speaker.c:137, 377); sound names validated via `sound_path()` against `/`, `..`, `S_ISREG` (control.c:60).
- **backchannel.c:** M-B1 headroom holds (cap 8192, 2048-block gate; libhelix built **without SBR**, 1024-sample constant verified); `rtp_payload_off` (CSRC/extension/padding) fully bounds-checked (backchannel.c:105).
- **rtsp.c:** Content-Length consumption clamped on both ends and bounded by H2 timeouts (rtsp.c:1161, 1317); `send_resp` hard against truncation (517, 538); `sendmmsg` batching without double-free/UAF, correct ENOSYS fallback; idle reaping keyed on actual TCP writes.
- **httpd.c digest (RFC 7616):** nc strictly increasing, TTL, request-uri binding, constant-time hex comparison, nonce table under mutex.
- **rtp.c/Opus:** MTU clamped to [548,1472], all buffers `RTP_MTU_MAX+32`; Opus: no splitting, obuf 4096 >> 1275 max, 48k clock/1920 ticks correct per RFC 7587; SDP `opus/48000/2` + `sprop-stereo=0` correct.
- **hal_ingenic.c:** AU/JPEG buffer growth is size_t-clean with a 1 MB cap, realloc failure drops rather than crashes; all rotate refusals (T23 sw, T31 FS) unwind **before** any IMP resource is allocated, or clean up in reverse order — no leak.
- **fanqueue.c byte cap / timelapse JIT subscribe:** accounting consistent on all paths, no leak/deadlock.
- **265befb (silent-limbo sweep):** no introduced OOB/UAF.
- **osd_vars `{fpsN}`:** channel index double-bounded (0-9 **and** `<MS_MAX_VSTREAM`).

---

## 5. Build & regression (host)

| Check | Result |
| --- | --- |
| `make sim` (`-std=c11 -Wall -Wextra`, USE_CONTROL/DAYNIGHT/RECORD/TIMELAPSE) | **builds warning-free** |
| `make test-auth` (sim harness, RTSP/HTTP fail-closed) | **PASS=4 FAIL=0 SKIP=2** (skips by design: loopback trust, backend 503 after auth) |

No camera flashing as part of this review (source-only); hardware verification of the v1.7.7 features was done separately, per the commit log.

---

## Recommended order

1. **A1 (record.c/timelapse.c core):** snapshot or make `_Atomic` the loop gates (`record.enabled/mode/channel`, `timelapse.enabled/interval_s`) — the only position with real mis-hoisting potential.
2. **A2:** move imp_osd.c rotation to `g_cfg_boot` — fixes an actual functional bug (live rotation edit shifts the OSD) along the way.
3. **A1 (remainder):** a mutex around `control_apply_json` (collapses the entire concurrent-POST class), then the cold single reads (speaker/aec/SDP/status) each via lock+copy; fix the comment at config.c:42-45 (A6).
4. **A3:** only set `cmfc`/`cmf2` for single-track output, or remove them.
5. **A4/A5:** as capacity allows; A5 only if real interop complaints arise.

None of these items block production deployment; from this audit's perspective, v1.7.8 can be rolled out.
