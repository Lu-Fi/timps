# timps – Performance & Resource Audit 2026-08-07

**Date:** 2026-08-07
**Scope:** exclusively performance, memory footprint, CPU/syscall load, thread/wakeup budget, and binary size. **Not** a security/correctness audit (those run in parallel, see `CODE_AUDIT_2026-07-18.md` / `SECURITY_AUDIT_2026-07-23.md`).
**Basis:** static analysis of the complete `src/` tree (~19,900 LOC of C) on `main`, at v1.7.7. No live measurement; cost estimates are derived from code structure, allocation patterns, and known platform costs (MIPS 3.10 **without vDSO** → every `clock_gettime`/`recv` is a real syscall at ~2-5 us each, cf. the comment at rtsp.c:87-94).

---

## Overall Verdict

For an embedded daemon, the code is **already unusually thoroughly optimized**. Practically every "classic" finding one would expect from such an audit has already been addressed and is documented in the code as an earlier optimization pass:

- **Zero-copy fan-out** via refcounted packets (`frame.c`), publish-skip at 0 subscribers (hub.c:247), a byte budget instead of just a per-queue slot cap (fanqueue.c:14-16, S2).
- **Syscall batching**: TCP-interleaved RTP in a single `write` (rtsp.c:148-165), UDP RTP via `sendmmsg` batching (P3, rtsp.c:87-143), SRT TS in 1316-byte blocks instead of individual 188-byte sends (srt.c:130-141), the recorder via buffered `FILE*` writes with asynchronous `sync_file_range` (record.c:326-340).
- **On-demand pipelines**: encoder/framesource/audio block idle on a condvar (`act_wait`, hal_ingenic.c:280-300), the OSD updater rasterizes only when consumers exist (imp_osd.c:458-469), `/events` is condvar-driven rather than polled (events.c).
- **Allocation hygiene**: AU/JPEG buffers grow on demand rather than being pre-sized for the worst case (hal_ingenic.c:979-987), persistent fragment scratch buffers with soft-shrink (record.c:298-299, fmp4.c:450, `ms_buf_reset`), a font cache instead of repeated parsing (imp_osd.c:87-121), a `/proc` reader with a 1 s TTL cache (osd_vars.c:63-143), throttled ISP scraping (P2, daynight.c:389-391).
- **Explicit thread stacks** (S3, util.h:55-74) instead of the 8 MB RLIMIT default; the binary built with `-Os -ffunction-sections -Wl,--gc-sections` + strip (build.sh:299-352), all heavy dependencies (TLS/SRT/FAAC/Opus/TTF) compile-gated.

The real measured baseline (QA soak 2026-07-18: ~5 MB RSS stable, fps-stable with up to 4 clients) confirms this. **There is no more "low-hanging" major win left.** The remaining items below are incremental: one medium structural candidate (P-01), one wakeup-budget item (P-02), and a handful of small syscall/copy trimmers. None of this blocks production use on T10/T20-class hardware.

---

## Findings

| ID | Sev | Topic | File:Line | Estimated Impact | Effort |
|----|-----|-------|-------------|--------------------|---------|
| P-01 | 🟠 MEDIUM | Duplicate frame copy + malloc/free per published frame | hal_ingenic.c:1122-1139 → hub.c:249 → frame.c:7-17 | ~0.5-1.5 MB/s of memcpy + ~50-100 malloc/free/s saved per active stream; roughly 1-3% CPU on T21-class, less heap fragmentation on 32 MB SoCs | medium |
| P-02 | 🟠 MEDIUM | Idle wakeup budget of ~25/s due to slice sleeps instead of a condvar stop | imp_osd.c:513, daynight.c:289-296, record.c:454, timelapse.c:273/287, main.c:167 | ~20 wakeups/s (~40 context switches/s) removable while idle | small-medium |
| P-03 | 🟡 LOW | Multiple `clock_gettime` syscalls per play-loop iteration (no vDSO!) | rtsp.c:1042-1117 (`ms_now_us` 3-5x/iteration), analogous in httpd.c stream loops | ~100-200 syscalls/s at 2 clients at 25 fps each → ~0.5-1 ms CPU/s | small |
| P-04 | 🟡 LOW | Control-socket poll (`MSG_DONTWAIT` recv) every iteration = 1 EAGAIN syscall per media frame | rtsp.c:1128 | ~40-50 syscalls/s per RTSP client avoidable (gate at ~50 ms) | small |
| P-05 | 🟡 LOW | TS packetization copies the PES payload byte-by-byte instead of via `memcpy` | srt.c:263-265 | ~0.5 MB/s/client byte-by-byte on MIPS ~= <1% CPU; ~4-8x faster with memcpy | small |
| P-06 | 🟡 LOW | MJPEG multipart: 2 `send()` calls per frame (part header + JPEG) | httpd.c:580-581 | 1 syscall/frame saved (~10-25/s per MJPEG client) via a contiguous buffer/`writev` | small |
| P-07 | ℹ️ INFO | UDP video session: 23.5 KB `rtp_batch` (16x1472) + batch intermediate copy | rtsp.c:95-101, 190 | ~24 KB heap per UDP video client; a deliberate trade for ~170 syscalls saved per IDR – **leave as is** | – |
| P-08 | ℹ️ INFO | Fanqueue worst-case pinning of 2 MB per stalled client | fanqueue.c:14-16 | 8 HTTP + 8 RTSP + 8 SRT stalled = theoretically ~48 MB; on T10/T20/T21 builds `-DFQ_MAX_BYTES=1048576` (and possibly halving QCAP) is recommended | trivial (build flag already exists) |
| P-09 | ℹ️ INFO | `config_str_lock` cadence after the daynight/OSD hardening (a review question) | daynight.c:696-701, imp_osd.c:230-233 | 2 locks/s (284-byte copy) + <=16 locks/s (428-byte copies); hold time sub-us, contention only during /control writes. **A generation counter is not worthwhile** | – |
| P-10 | ℹ️ INFO | TTF re-rasterization 1x/s per second-resolution text item | imp_osd.c:222-294, msttf.c | Already gated (osd_needed, change detection, supersample=2). Minute resolution in the template makes it essentially free – a documentation note, not a code fix | – |
| P-11 | ℹ️ INFO | Thread inventory: ~13-16 base threads (full build) + 1/client, connection stacks 256 KB of VA | util.h:72-74, various | RSS share is small (stacks are lazily faulted in); an mbedTLS context of ~30-40 KB per TLS connection is the real per-client cost item | – |
| P-12 | ℹ️ INFO | Binary size | build.sh:299-352 | `-Os` + gc-sections + strip + feature gates: no discernible bloat source; the table-less CRC32 (srt.c:108-117) deliberately trades 1 KB of flash for cycles, only on PSI paths | – |

---

### P-01 – Frame publish: second full copy + heap churn per frame

The hottest remaining path. Today, per encoded frame:

1. `video_thread` assembles the IMP packs into the persistent AU buffer (`memcpy`, hal_ingenic.c:1137) – **necessary** (start-code fixup, scattered packs).
2. `hub_publish` → `pkt_new` **mallocs and copies the entire AU again** (frame.c:7-17), plus a `free` after the last `unref`.

At 25 fps x 2 streams + 25 audio frames/s, this amounts to ~75 malloc/free pairs/s with active clients and up to ~1.5 MB/s of additional memcpy bandwidth – on a 32 MB SoC, on top of that, sustained heap churn in the frame size class (4 KB...400 KB), the classic fragmentation source for uClibc allocators.

**Proposal:** a `hub_publish_take()` variant that takes ownership of an already-refcounted buffer: the producer assembles the AU directly into an `ms_pkt` (or into a buffer recycled from a small per-source pool) instead of into `au[]`. Watch out for two existing invariants: (a) the 0-subscriber skip (hub.c:247) must not turn into a per-frame malloc at 0 subscribers – the producer buffer must stay reusable when nobody is listening; (b) `vparam_update` reads the AU under `s->lock` before the push. Hence "medium" rather than "small": an API change to hub plus 3 producer paths (video, JPEG, audio). The gain is the largest single item still available.

### P-02 – Idle wakeup consolidation

Stop responsiveness today is bought throughout via slice sleeps: OSD at 100 ms (10 wakeups/s, **even fully idle**, just to notice `osd_needed()` within 100 ms – that is an extra 4 mutex pairs per wake), daynight in 200 ms slices (~5/s at a 500 ms interval), recorder/timelapse at 300 ms each (~3.3/s even when disabled). Total while idle: ~25 wakeups/s, ~50 context switches/s on the core that timpsd shares with the ISP/encoder firmware. Not a measured hotspot, but unnecessary: the same stop latency is delivered by a `pthread_cond_timedwait`-based stop condvar (the pattern already exists in `fanqueue`/`events`) at **one** wakeup per genuine interval; the OSD idle case can use the existing `hub_set_activity_cb` as a wake signal instead of polling. Benefit: idle wakeups from ~25/s down to <5/s, noticeable more in power/scheduling behavior than in %CPU.

### P-03/P-04 – Syscall trimmers in the RTSP play loop

Without vDSO, every `ms_now_us()` costs a real syscall. `stream_loop` calls it several times per iteration (drop check, SR check, activity), and additionally polls the control socket non-blocking every iteration – with video running, that is ~1 EAGAIN `recv` plus 3-5 `clock_gettime` calls per client and frame. A `now` cached once per iteration and a time-gated control poll (every ~50 ms instead of every iteration; TEARDOWN latency stays <50 ms) together save roughly 100-150 syscalls/s per active client for a two-line change. Small, but practically free.

---

## Explicitly checked, **no** action needed

- **`snprintf` in hot loops:** none found. Formatting exists only in request/setup paths, OSD expansion (1x/s, cached), and log lines; RTP/TS/fMP4 headers are built binary via `wr_be*`.
- **`/control` GET JSON:** an 18 KB heap buffer + a one-time build per request (httpd.c:1187-1204) – irrelevant at WebUI rates; continuous polling is obsolete anyway thanks to the SSE push architecture (events.c, condvar).
- **O(n^2) patterns:** none. All client/subscriber lists are flat arrays of <=16 entries, iterations linear; fanqueue drops are amortized O(1).
- **Recorder I/O:** buffered `fwrite` + `REC_SYNC_US` fflush + asynchronous `sync_file_range`, fsync only at segment end – correctly sized, no need for coalescing.
- **fMP4/HTTP stream path:** one `csend` per completed fragment (httpd.c:415), a persistent scratch buffer (fmp4.c:450-475) – already batched.
- **Daynight:** gain via the IMP API 2x/s (cheap), `/proc` scrape throttled to 5 s (P2), a 284-byte struct snapshot per poll under lock (see P-09) – the probe-economy changes from v1.7.7 have no measurable runtime cost.
- **Stack/TLS budgets:** the audio worker uses ~12 KB `__thread` + 8 KB `acc[]` (documented, hal_ingenic.c:2377-2384), RTP packet buffers ~1.5 KB/frame on connection stacks (256 KB) – all within the S3 budgets; `rtsp.c`'s `hdr[3072]` remains the known F-11 nit.

## Recommended Order

1. **P-08** – set the build flag for small SoCs (1 line in the platform build config, immediate worst-case RAM gain).
2. **P-03 + P-04** – syscall trimmers (a few lines each, risk-free, measurable via `strace -c`).
3. **P-01** – `hub_publish_take()`/buffer pool, verify with `make sim` + a QA soak (RSS + %CPU before/after).
4. **P-02** – stop-condvar consolidation, together with P-01 in one refactor window.
5. P-05/P-06 as capacity allows.

None of these items is urgent; in its current state, the daemon is resource-viable across all target platforms.
