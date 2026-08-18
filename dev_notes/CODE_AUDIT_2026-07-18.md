# Independent Code Review – microstream / timps

**Date:** 2026-07-18
**Scope:** security, stability, memory/load optimization, and A/V stream conformance
**Basis:** complete `src/` (~13,400 LOC C), three independent static audits, verification against `git log`, plus ffprobe/ffmpeg analysis of real QA recordings (`timps-qa-20260718-165812`, a test camera on the LAN)

This review is independent of the existing `CODE_REVIEW.md`. It verifies that review's findings against the **current** code state and adds an actual stream-conformance check.

---

## Overall verdict

The current code state is **very good**. Practically all HIGH and MEDIUM findings documented in the earlier `CODE_REVIEW.md` have since been fixed (hardening commits `1418af5`, `5ea745d`, `63761c9`, among others). Three independent audits (network, core/memory, codec/HAL) confirm the fixes at concrete line numbers. Two findings reported as "new/critical" turned out on re-verification to be **false positives** (see below) — notable because one of them had claimed a root command-injection vulnerability.

The streams produced are **conformant**: all recordings (RTSP main/sub, HTTP fMP4) decode with **0 video and 0 audio errors**, A/V is in sync, timestamps are monotonic, and the GOP is regular.

Only **minor** residual items (LOW) remain, with no critical open security or memory-corruption bugs.

---

## 1. Verification of prior findings (status in current code)

| ID | Topic | Status |
| --- | --- | --- |
| H1 | RTSP socket timeouts | **fixed** – `net_set_timeouts(cfd,30,15)` (rtsp.c:725), `net.c:30-43` |
| H2 | HTTP socket timeouts | **fixed** – `net_set_timeouts` (httpd.c:996), body loop with 5s poll (httpd.c:934-950) |
| H3 | SRT passphrase error ignored | **fixed** – return value checked, no longer starts unencrypted (srt.c:398-403) |
| H4 | OSD canvas integer overflow | **fixed** – `pixel_h` clamped 8..512, W/H <=4096, size computed in double with SIZE_MAX guard (msttf.c:372-397); `font_size` via `pint_cl(8,256)` (config.c:335) |
| H5 | OSD region not clamped | **fixed** – bitmap discarded when larger than stream size (imp_osd.c:215-247) |
| H6 | IMP SDK return values unchecked | **fixed** – StartRecvPic/GetStream/Bind checked (hal_ingenic.c:685-747, 1512-1527) |
| M1 | TLS handshake without timeout | **fixed** – `SO_RCVTIMEO` before handshake (tls.c:105-110) |
| M2 | No TLS minimum version | **fixed** – minimum TLS 1.2 (tls.c:64-69) |
| M3 | UAF on TLS context during shutdown | **fixed** – fd registry, `shutdown()` before `ms_tls_ctx_free` (rtsp.c:822-832) |
| M4 | SRT streamid unchecked server-side | **fixed** – `listen_cb` rejects mismatches (srt.c:372-392) |
| M5 | Digest realm/uri unchecked | **partial** – replay risk closed via server-nonce binding (auth.c:74); literal realm/uri check (RFC 2617) still open. **No auth-bypass hole.** |
| M6 | Predictable `rand()` | **fixed** – urandom for SSRC/seq/ts_base/session ID (rtp.c:15-27, rtsp.c:403) |
| M7 | Recorder without fsync | **fixed** – fflush/fsync/fclose checked, `sync_file_range` (record.c:318-341) |
| M8 | Leak in ing_start/ing_stop error paths | **fixed** – ordered `fail:` teardown (hal_ingenic.c:1573-1619) |
| M9 | OSD retired double-buffer UAF | **fixed** – retire ring depth 3 (imp_osd.c:31, 169-175) |
| M10 | Config read lock-free in refresh_text | **fixed** – whole item snapshot taken under lock (imp_osd.c:185-240) |
| M11 | Config numerics unvalidated | **fixed** – `pint_cl()` for width/height/fps/bitrate/gop/qp/ports (config.c:300-317, 460-482) |
| M12 | Day/night wall clock instead of monotonic | **fixed** – `ms_now_us()/1000` (daynight.c:290, 308) |
| M13 | `config_get_kv` missing record.* | **fixed** – record.* branch added (config.c:839-866) |
| M16 | msttf scanline intersections capped at 128 | **fixed** – dynamically allocated `maxint` (msttf.c:447-463) |
| L1,L2,L4,L5,L6,L8,L9,L10,L11,L13,L14a | various | **fixed** (verified at current lines) |
| L14b | fmp4 mfhd seq 32-bit wrap | **intentional as-is** – documented, harmless counter wrap (fmp4.c:387-391) |

Conclusion: **all** prior H/M items have been implemented; the only "open" positions (M5 remainder, L14b) are either benign or intentional.

---

## 2. Two refuted false positives (notable)

On re-verification, two automatically reported "new" findings were **discarded** — the verification effort was worthwhile:

- **No root command injection via `/control`.** It was reported that `daynight.switch_cmd` (which flows into `system()` as root) could be set via `/control`. In fact `control.c` uses a fixed whitelist `DN_KEYS` (only four numeric thresholds); `switch_cmd`/`isp_path` can only be set from the (trusted) config file, **not** via the network interface. No injection vector.
- **hvcC chromaFormat is correct.** It was reported as `0xFC` (monochrome); the code actually emits `0xFD` (4:2:0) at the correct location (vparam.c:169). The `0xFC` line next to it is `parallelismType=0` and is correct.

---

## 3. Remaining new findings (all LOW)

- **N1 (stability/disk) — recording duration values unclamped.** `segment_s`/`pre_roll_s`/`post_roll_s`/`min_free_mb` are parsed with plain `pint()` (config.c:571-574). `segment_s <= 0` disables rotation (record.c:444), so a single segment file then grows unbounded. *Fix:* `pint_cl` analogous to the other fields (e.g. `segment_s` 1..3600).
- **N2 (conformance, cosmetic) — hvcC `numTemporalLayers=0`.** vparam.c:172 writes `0x03`; the correct value would be `0x0B` (1 temporal layer). Browsers and ffmpeg tolerate `0` (=unknown); only strict validators (Bento4, Apple HLS) flag it. Affects only H.265 metadata, not the bitstream.
- **N3 (rendering) — OSD is Latin-1 only.** msttf.c feeds the glyph lookup byte by byte; UTF-8 multibyte characters in OSD text/`{hostname}` are rasterized as individual bytes. Affects only the overlay image, **not** stream conformance.
- **N4 (robustness) — RTSP transport values unchecked.** Interleaved channels/`client_port` from `sscanf` without range checking (rtsp.c:412/427); due to truncation to `uint8_t`/`uint16_t` **memory-safe**, only a protocol robustness issue.
- **N5 (robustness) — `/control` body partially applied on timeout.** If the 5s body poll expires, the partial buffer is still parsed anyway (httpd.c:952). Since the scanner is bounds-checked and `sanitize_val`-cleaned, and the path is authenticated, the worst case is partial application — no overflow.
- **N6 (latent UB, benign) — pointer past buffer end in the `APP()`/snprintf accumulator** (control.c:541-565, httpd.c:560-563): the size argument correctly collapses to 0, nothing is written; forming the out-of-range pointer is formally UB but has no practical consequence.

---

## 4. Memory & load (from real-world QA soak run)

- **Memory:** `timpsd` RSS stays stable at ~5 MB across the client ramp (5108->5140->4904->5712->5904 kB); **no leak** observed. Frame refcounting (`frame.c`, `__sync` atomics) and the hub/fanqueue handshake are clean; every `fanqueue_pop` path correctly `pkt_unref`s.
- **Load:** fully fps-stable up to **4 concurrent clients** (min 23.5 fps, aggregate 98 fps/s). At 8 clients it degrades gracefully (6 ok / 2 rejected, min 16 fps) — no crash, no leak. On-demand encoding keeps idle CPU low.
- **No busy loops** in the recorder, timelapse, or daynight threads (all block on pop timeouts or staggered sleeps).

---

## 5. Stream conformance (independently verified with ffprobe/ffmpeg)

Checked against real recordings from the current build (RTSP main+sub, HTTP fMP4):

| Stream | Video | Audio | Decode errors |
| --- | --- | --- | --- |
| RTSP ch0 (main) | H.264 **High@L5.1**, 1920x1080, yuv420p, progressive, 25 fps | AAC-LC 16 kHz mono | **0 / 0** |
| RTSP ch1 (sub) | H.264 High@L5.1, 640x360, yuv420p, 25 fps | AAC-LC 16 kHz mono | **0 / 0** |
| HTTP fMP4 | H.264 High@L5.1, 1920x1080, 25 fps | AAC-LC 16 kHz mono | **0 / 0** |

- **GOP** regular (keyframes at frames 1,2,102,202,302,402 -> interval 100), SPS/PPS in-band, clean IDR.
- **A/V sync**: drift -0.02 s (RTSP main), -0.03 s (sub), -0.08 s (fMP4) — in sync. Timestamps monotonic (nonmono=0), no gaps >1.5 s.
- **fps** 24.8 — within 10% of nominal; realtime rate 0.98x.
- Statically confirmed correct: AAC ASC (AOT2/2-byte), `esds`, ADTS strip, `avcC` (profile/level from SPS), `trun`/`tfdt`(v1 64-bit)/`trex`/`tfhd`, NAL iteration (3-/4-byte start codes), G.711 mu-law/A-law.
- The "non-monotonic DTS" messages reported by ffmpeg during re-muxing originate from the null muxer/MKV millisecond grid of the QA recording, **not** from the transmitted bitstream (the QA timestamp parser reports nonmono=0).

**Result: all produced audio/video streams are conformant.** The only substantive conformance nit is the cosmetic HEVC `numTemporalLayers` value (N2).

---

## Recommended order (remaining items)

1. **N1** – clamp `segment_s` and other recording duration values (the only item with real operational risk).
2. **N2** – hvcC `numTemporalLayers` to `0x0B` (clean HEVC metadata for strict validators).
3. **M5 remainder** – optionally `strcmp(realm,AUTH_REALM)` + URI check (RFC 2617 form conformance).
4. **N3** – optionally UTF-8 in the OSD renderer.
5. N4-N6 as capacity allows (pure hardening).

None of these items block production deployment.
