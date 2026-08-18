# Adversarial Review – P-01 (Frame Pool / `hub_publish_take`) + P-02 (`ms_stopgate`)

**Date:** 2026-08-07
**Subject:** an uncommitted diff in worktree `perf-p01-p02` (branch `perf/p01-p02-frame-copy`, base = `main` @ `68d6ec6`, 13 files, +540/-142), Opus's implementation of P-01 and P-02 from `PERFORMANCE_AUDIT_2026-08-07.md`.
**Method:** a full reading of the diff; a manual refcount trace of ALL producer call sites (the 0-subscriber and N-subscriber cases, all error paths); leftover greps (`hub_publish`, `usleep`, `g_stop`, raw `free()` on packets); an own `make sim` (warning-free) **and** an own ASan+UBSan host build under multi-client load (RTSP TCP+UDP x3, fMP4, MJPEG, 60x snapshot hammer, 10x reconnect churn, continuous AND motion recording with a 3 s pre-roll), plus three measured SIGTERM shutdowns. Claims were not taken on faith, but reproduced.

---

## Overall Verdict

**Cleared for the dedicated cam-A soak (T31).** No blocking finding. The refcount bookkeeping is correct at all 10 producer call sites, both audit invariants (the 0-subscriber skip, the `vparam_update` ordering) hold up under deep inspection, and the stopgate conversion is race-free across all four threads. Four new findings, all LOW/INFO, none soak-blocking — but R-01 (pool ceiling vs. real IDR sizes) explicitly belongs on the **soak measurement list**.

Independent reproduction: `make sim` warning-free; the ASan+UBSan run under full load shows **0 leaks / 0 UAF / 0 UB**; shutdown latencies measured independently: **0.28 s idle, 0.28 s mid-stream, 0.20 s in motion mode under load** — matching Opus's order of magnitude (0.14-0.34 s) exactly, far under the 3 s watchdog.

**Not validatable via the cam-A soak** (see the T23 section below): the `ROT_HAS_SW_90` software-rotate path. It is **byte-identically untouched** in the diff (no hunk between hal_ingenic.c:1185-1744) and stays on the unchanged copy API — the residual risk is a pure compile risk, not a logic risk. Opus's claimed T23 `-fsyntax-only` run could **not** be reproduced here for lack of a toolchain → an open item before any T23 rollout.

---

## P-01 – Verdict per Claim

| Claim | Verdict | Evidence |
| --- | --- | --- |
| `pkt_new()` unchanged, all existing sinks untouched | ✅ **CONFIRMED** | frame.c: identical malloc+memcpy, only `cap=len?len:1`, `pool=NULL`, `pnext=NULL` added. `pool==NULL` ⇒ `pkt_unref()` frees exactly as before. Grep across all of `src/`: **no** raw `free()` on an `ms_pkt` outside frame.c; fanqueue (push-on-closed, drop-oldest, GOP forward-drop), the record ring, rtsp/srt/httpd/timelapse all use `pkt_unref` exclusively — pool-agnostic, correctly so. |
| Shared helper, no behavioral drift | ✅ **CONFIRMED — genuinely shared** | `hub_publish()` AND `hub_publish_take()` both call the same `hub_prepare_locked()` (vparam/fps/kbps update + subscriber snapshot + raising `g_pushing`, all under `s->lock`) and the same `hub_finish_push()`. No duplicated code that could drift apart. |
| `vparam_update` ordering preserved | ✅ **CONFIRMED** | The take path passes `p->data/p->len` to `hub_prepare_locked` – at this point the producer holds the sole reference (no push has happened yet), so it is exactly the same bytes at the same point in the sequence as in the copy path. The video-only gate is unchanged. |
| 0-subscriber invariant (audit invariant a) | ✅ **CONFIRMED** | `hub_publish_take` at `nsub_snap==0`: `pkt_unref(p)` ⇒ last ref ⇒ returned to the pool (no free, no malloc). The idle-stop debounce window is borrow+return of the **same** buffer. Caveat for buffers >96 KB → R-01. |
| Call-site count "10 instead of 11" | ✅ **CONFIRMED — 10 is correct** | Own grep on `main`: exactly **10** producer sites (hal_ingenic 7: video 1159, sw-rot video 1488, sw-rot JPEG 1519, JPEG 1874, audio 2394/2417/2425; hal_sim 3: 107/142/160). The "11" in the earlier Opus report was a miscount. Disposition in the branch: **4 converted** (video+JPEG hardware, video+JPEG sim), **6 deliberately left on the copy path** (2x T23 sw-rot, 4x audio). No site mishandles the other API's semantics. |
| Pool sizing 4 x 96 KB, ratcheted release | ✅ Values confirmed (`HUB_POOL_MAX_FREE=4`, `HUB_POOL_KEEP_CAP=96*1024`, both overridable via `#ifndef`), logic correct (`nfree<max_free && cap<=keep_cap` under `pool->lock`). **But → R-01.** |
| ASan run with no findings | ✅ **reproduced independently** (see the overall verdict), including the motion pre-roll ring (75 pinned packets ⇒ the pool-overflow-to-free branch demonstrably exercised). |

### Manual Refcount Verification (the core of this review)

All paths traced individually:

- **hal_ingenic video_thread (take):** `hub_pkt_get` ⇒ ref=1. (a) `need>MS_AU_BUF_MAX` ⇒ drop BEFORE the get, no packet in play. (b) get-OOM ⇒ drop+ReleaseStream, nothing borrowed. (c) defensive overflow ⇒ `pkt_unref(pk)` ✅. (d) normal ⇒ `au_is_key` reads BEFORE the handoff; after `hub_publish_take` the thread demonstrably no longer touches `pk`/`au` (the T31 bitrate block only reads `st`). Take consumes: 0-sub ⇒ unref/pool; N-sub ⇒ N x `pkt_ref` on push + 1x producer unref ⇒ balance exactly N, each sink unrefs once. ✅
- **hal_ingenic jpeg_thread (take):** identical structure; the snapshot `fwrite` reads the buffer **before** the handoff (comment and code agree). The overflow path unrefs. ✅
- **hal_sim vid/jpg (take):** get-NULL ⇒ frame skipped, state still advances (`next+=step`, `aulen=0`) — no hang, no leak. ✅
- **The 6 copy sites:** unchanged borrowed-buffer semantics, callers retain ownership. ✅
- **`hub_publish_take(src, NULL, ...)`** and an invalid `src`: no-op or unref respectively — safe. ✅
- **The one-producer-per-source invariant** (a precondition for the `g_pushing` flag) is preserved: `video_thread` XOR `sw_rot_thread` per vchan (hal_ingenic 1736/2791), one thread each for jpeg/audio.

### New Findings P-01

| # | Severity | Finding |
| --- | --- | --- |
| R-01 | 🟡 LOW (performance, not correctness) | **`HUB_POOL_KEEP_CAP=96 KB` sits BELOW the real mainstream IDR size.** hal_ingenic.c itself documents an "observed IDR peak of 256-400 KB" (1080p @ 3000 kbps, the default from `timps.conf.example`); 1080p JPEGs also regularly exceed 96 KB. Consequence: the buffer ratcheted up for the IDR is ALWAYS free()d on return ⇒ one realloc-grow + free per GOP (~1x/s), potentially per frame for JPEG sources. **Important:** the actual P-01 payoff — elimination of the second full copy — remains fully intact for IDRs (assembly happens directly into the pool buffer, handoff is zero-copy); only the *recycling* is defeated for the large frames. Churn still drops from ~25 pairs/s to ~1/GOP. Recommendation: measure RSS + fragmentation on the cam-A soak and, if needed, raise `keep_cap` for video sources (already possible via `-D`); the 384 KB idle ceiling was a deliberate 32 MB SoC trade-off and is defensible as the default. |
| R-02 | 🟡 LOW (performance) | `pkt_pool_get` grows via `realloc()` — copying up to 96 KB of **stale payload**, even though `len` is always 0 at get time and the content gets fully overwritten. `free`+`malloc` would be strictly cheaper. Cosmetic, relevant ~1x/GOP (amplifies R-01 minimally). |
| R-03 | ⚪ INFO | jpeg_thread now publishes **always** (the old `jc->active \|\| hub_active` gate is removed). Delivery semantics are demonstrably equivalent (the gate only skipped the now-eliminated malloc+copy; a 0-sub take is borrow+return), cost per idle frame: 1x `s->lock` + a pool round trip — negligible. Second: the (blocking, SD) snapshot write now moves BEFORE the publish — the MJPEG frame of the snapshot tick now arrives ~SD-latency later instead of the following frame; neutral, same thread. |

---

## P-02 – Verdict per Claim

| Claim | Verdict | Evidence |
| --- | --- | --- |
| Follows the fanqueue/events pattern | ✅ **CONFIRMED** | `ms_stopgate` uses exactly the `condattr`+`CLOCK_MONOTONIC` construction from events.c:22-26/fanqueue.c:27-29 (NTP-step-proof). |
| Freedom from stop races | ✅ **CONFIRMED** | `ms_stopgate_stop()`: `lock → stop=1 → broadcast → unlock` — the **same** mutex as the waiter's predicate check. `ms_stopgate_wait()`: the predicate is checked before the first wait (the already-stopped case) AND inside the loop (spurious wake/timeout). A window-free construction; for all 4 threads, `!ms_stopgate_stopped()` additionally holds at the top of the loop. No shutdown-hang window found. |
| Shutdown latencies | ✅ **measured independently:** 0.28 s idle / 0.28 s mid-stream / 0.20 s under motion load (ASan build, hence conservative). Fully corroborates Opus's 0.14-0.34 s. |
| record/timelapse: the 1 s poll affects ONLY the idle-enable case | ✅ **CONFIRMED** | record.c: the 1 s wait sits exclusively in the `!want_run()` branch (recorder disabled, no subscription, no pre-roll to lose); an ongoing recording ticks via `fanqueue_pop(&q,200)` with `want_write()` **per packet** — write/motion cadence unchanged. timelapse.c: the capture timestamp is pinned to `next_us`, the wait is `min(left, 1 s)` — interval fidelity unchanged, only the disabled-poll period moves from 300 ms to 1 s. |
| imp_osd: only idle wakeups changed | ✅ **CONFIRMED** | Old loop: render + 10x100 ms ⇒ 1 s cadence while active. New: render + 1x1000 ms ⇒ identical 1 s cadence while active, 1/10 the wakeups. The only loss: the sub-second refresh on a client connecting from idle (timestamp stale by up to ~1 s, cosmetic) — cleanly documented as a trade-off in the code, coupling to `hub_set_activity_cb` deliberately and understandably rejected. |
| main.c correctly left untouched | ✅ **CONFIRMED** | `while (g_run) sleep(1)` + signal handler: `pthread_cond_broadcast` is not async-signal-safe (POSIX), a handler must NOT wake the gate; `sleep()` returns immediately on a signal with EINTR — the existing loop is already the correct construction. Opus's reasoning holds. |

### Edge Cases Checked

- **`record_clip` (control thread) reads `g_gate`:** (1) recorder never started ⇒ the `!g_rc` guard returns before the loop. (2) thread-create failure ⇒ `record_start` explicitly calls `ms_stopgate_stop` — the gate is stopped, `record_clip` does not spin forever (equivalent to the old `g_run=0`). The timelapse error path is analogous. Cleanly thought through. ✅
- **`dn_sleep` in inner branches** (daynight 754/790/935/1226): returns immediately after a stop, every branch ends in a `continue` back to the loop-head check — no spin, exactly the old slice semantics. ✅
- **Leftover grep:** no more `usleep` in the 4 converted files, no `g_stop`/old-`g_run`-flag relic. ✅

### New Findings P-02

| # | Severity | Finding |
| --- | --- | --- |
| R-04 | 🟡 LOW (portability) | `ms_stopgate_init()` gets called again on restart (`record_start` after `record_stop`, likewise daynight/imp_osd/timelapse) on an already-initialized mutex/cond — undefined by the letter of POSIX (re-init without `destroy`). Practically harmless on NPTL/uClibc-ng (static objects, demonstrably no waiters at init time — the join has already completed — and no resource leak), and the header documents "safe to call again on restart" as a deliberate decision. A cleaner approach would be an `ms_stopgate_rearm()` that only sets `stop=0` under the lock. Not a soak blocker. |

---

## What the cam-A Soak Does NOT Cover (open before a T23 rollout)

1. **The T23 `ROT_HAS_SW_90` path:** untouched in the diff (0 hunks in 1185-1744, both sites 1488/1519 still on the unchanged copy API) — logic risk ~= 0. BUT: hal_ingenic.c was substantially edited; Opus's claimed `-fsyntax-only` run against the T23 headers was **not reproduced** here (no toolchain in the worktree). ⇒ Before any T23 build: run `./build.sh` or `-fsyntax-only` against the T23 headers once, independently.
2. **The R-01 measurement:** real IDR sizes vs. the 96 KB ceiling at hardware bitrates — soak checklist: RSS trend, `logread` for `dropping frame`/OOM warnings (must never fire), ideally one heap-churn before/after comparison.
3. **The uClibc allocator:** the ASan run is glibc/x86 — only the soak shows the uClibc allocator's fragmentation behavior under the new 1/GOP pattern.
4. **Audio paths** are unconverted (deliberately) — no soak requirement beyond normal operation.

## Soak Recommendation

cam-A (T31), standard QA (`scripts/timps-qa.sh --profile soak`) plus: >=48 h with active recording (motion + pre-roll, pins pool packets), periodic RSS + `dropping frame` grep. Abort criterion: any `AU exceeds max buffer`/`no memory for AU packet` message, or RSS drift >10%.
