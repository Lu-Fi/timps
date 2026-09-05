# Frame pool — recycling the oversized buffers (R-01, finally closed) — 2026-09-05

Closes **R-01** from `dev_notes/REVIEW_2026-08-07_p01-p02-frame-pool.md`, open
since the P-01 review with the disposition *"measure RSS + fragmentation on the
cam-A soak and, if needed, raise `keep_cap`"*. The measurement was never
recorded and `HUB_POOL_KEEP_CAP` never changed — `git log -- src/hub.c` and a
grep across `dev_notes/` show no follow-up. R-02 from the same review (the
`realloc()` on grow) *was* fixed, in the 2026-08-28 performance round; R-01 was
not.

So: genuinely still open, and left open for want of a measurement rather than
because of an objection. This note supplies the measurement — on real
hardware, which turns out to matter, because the answer is far less uniform
than either the original review or the finding that prompted this work
assumed.

---

## The problem

`pkt_unref()` returned a buffer to its source's freelist only while
`cap <= keep_cap` (96 KB). Above that it called `free()`. The comment called
such a buffer "a one-off large IDR"; it is not one-off:

| frame | typical size | how often |
|---|---|---|
| P-frame, fleet settings (1200 kbps @ 15 fps) | ~10 KB | 15/s |
| IDR, fleet settings | ~80 KB | 1/GOP |
| IDR, `timps.conf.example` default (3000 kbps @ 25 fps) | ~120 KB | 1/GOP |
| **1080p JPEG, night, measured on a fleet camera today** | **224 KB** | 5/s while a viewer is attached |
| 1080p JPEG, detail-heavy daylight (`hal_ingenic.c:106-117`, cam-L) | 800-820 KB | 5/s |

Every row from the third down was a `malloc` + `free` per frame. The JPEG rows
are the ones that hurt, and they are not exotic: `video0.jpeg` defaults to
enabled at the *video stream's* resolution (`config.c:297`), and
`timps.conf.example:208` records that **the thingino WebUI previews fetch
`/stream.mjpeg?chn=N` and `/snapshot.jpg?chn=N` directly from this port**. So
every time someone opens a camera's preview in the WebUI, that camera starts
producing 1080p JPEGs at 5 fps.

---

## What it actually costs — measured on a T31 camera

The finding that prompted this work predicted that these allocations cross
uClibc-ng's mmap threshold and cost "~0.3-1 ms per large frame". That is a
claim about the target's allocator, so it was measured there rather than
reasoned about: a small static MIPS binary (`scratchpad`, not committed) run
on the Garage T31, comparing

- **A** — `malloc(sz)` + write the whole frame + `free()` — what a >96 KB frame
  costs today, and
- **B** — write the whole frame into a buffer that is already there — what it
  costs after this change,

500 iterations each, with `/proc/self/stat` minor faults read around both
loops. The write is in both columns because the producer has to copy the
encoder packs in either way; the difference is exactly what one frame stops
paying.

| frame size | A µs | B µs | **saved µs** | A minor faults / frame | B |
|---|---|---|---|---|---|
| 80 KB | 63.7 | 51.3 | 12.4 | 0.05 | 0 |
| 120 KB | 89.3 | 74.4 | 14.9 | 0.02 | 0 |
| 192 KB | 158.0 | 134.8 | 23.1 | 0.01 | 0 |
| **256 KB** | 506.0 | 180.5 | **325.6** | **65** | 0.1 |
| 300 KB | 628.0 | 226.9 | **401.1** | 76 | 0.15 |
| 400 KB | 806.5 | 284.6 | **521.9** | 101 | 0.2 |
| 800 KB | 1568.3 | 573.9 | **994.4** | 201 | 0.4 |

### Why this had to be measured on the camera

The identical binary on the x86 host, glibc, same 500 iterations:

| frame size | A µs | B µs | saved µs | A minor faults / frame |
|---|---|---|---|---|
| 300 KB | 6.86 | 6.37 | 0.49 | 0.17 |
| 800 KB | 19.31 | 18.52 | 0.79 | 0.60 |

**Nothing.** glibc's mmap threshold is *dynamic*: after it sees a few large
blocks freed it raises the threshold and keeps them on the heap, so the
repeated allocate/free of a same-sized frame costs almost the same as reusing
one. uClibc-ng's threshold is fixed, so it never learns and pays the mmap every
frame, forever. Any host-side benchmark of this change — including `make sim` —
therefore measures approximately zero, and does so for a reason that does not
apply to the target. That is worth writing down: the 2026-08-07 review already
flagged "the ASan run is glibc/x86 - only the soak shows the uClibc allocator's
fragmentation behavior", and this is the concrete form that warning takes.

The prediction is confirmed, and the shape of it is the interesting part:
**there is a cliff between 192 KB and 256 KB**, where this uClibc-ng starts
serving the request with `mmap`. Below it a malloc+free pair costs 12-23 µs and
faults nothing. At and above it, every frame takes one minor fault *per page*
— 201 of them for an 800 KB JPEG, each one a kernel page-zeroing — and the
frame costs 2.7x what writing it costs. Recycling removes all of it.

What that adds up to per second:

- **1080p MJPEG viewer, daylight**: 5 × 994 µs = **~5 ms/s, ~0.5% of a core**,
  plus ~1000 page faults/s that stop happening.
- **1080p MJPEG viewer, night** (the 224 KB frames measured today): ~5 × 20 µs
  — small, because 224 KB happens to sit just under the mmap cliff.
- **Video IDRs at fleet settings**: nothing at all. See below.

### The honest part: today's fleet sees no change

All 11 reachable cameras run `video0.bitrate = 1200` at 15 fps. That puts an
IDR at roughly 80 KB — **under** the existing 96 KB split, so those already
recycled and still do. Minor-fault rates sampled on the live daemons (6-14/s
across cameras with 10-28 h uptime, 1.8/s on Schuppen over a 30 s window) are
flat, exactly as that predicts: at these settings nothing in the video path is
mmap-churning, and this change is a no-op for it.

The benefit is entirely in the JPEG/MJPEG path and in any camera configured
above ~1.5 Mbps. That is a narrower claim than the original finding made, and
it is the claim this change should be judged on.

---

## What changed

**`src/frame.h` / `src/frame.c`** — `pkt_pool` gains one pointer, `big`:

- `pkt_unref()` now splits at `keep_cap` instead of discarding above it. At or
  under: the freelist, bounded by `max_free`, exactly as before. Above: the
  single `big` slot. A *second* oversized buffer arriving while the slot is
  taken is freed, as before — that is what bounds the addition to one buffer
  rather than to however many large frames are in flight.
- `pkt_pool_get()` dispatches **by size**. A borrow over `keep_cap` takes
  `big` and never the freelist; a borrow at or under takes the freelist and
  never `big`. Either miss falls through to a fresh `malloc`, exactly as an
  empty freelist always did.
- New `pkt_pool_trim()` frees the `big` slot (outside the lock).

**`src/hub.c` / `src/hub.h`** — `hub_pool_trim(src)` wraps it per source. The
`HUB_POOL_KEEP_CAP` block comment is rewritten: the constant is now a
small/large *split point*, not a discard threshold.

**`src/hal/hal_ingenic.c`** — `hub_pool_trim()` on the two producer idle-stop
paths (`video_thread` and `jpeg_thread`, right after `StopRecvPic`/`fs_unuse`)
and on `jpeg_thread`'s exit epilogue, which the watchdog give-up also reaches
while the rest of the daemon keeps running.

### Why one slot, and not simply a bigger `keep_cap`

Raising `HUB_POOL_KEEP_CAP` was the review's own suggested remedy and it is the
wrong one. `keep_cap` bounds the freelist, so raising it to IDR size lets *all
four* freelist entries ratchet there — 4x the idle footprint the 96 KB ceiling
was deliberately chosen to defend on 32 MB SoCs. One slot adds at most one
buffer per source.

### Why the size-matched dispatch is not optional

This is the part the sketch for this work did not have, and it is load-bearing.
If the retained 800 KB buffer could be handed to a 10 KB P-frame, that packet
would be published with `cap` 800 KB and `len` 10 KB — and `fanqueue`'s
`FQ_MAX_BYTES` budget and `record.c`'s `RING_MAX_BYTES` both account `->len`.
The memory would be invisible to the only backstops the fan-out has. This is
not hypothetical: it is precisely the reasoning `hal_ingenic.c:2348-2403`
records for keeping the T23 SW-rotate path on the copying publish API, where
publishing a worst-case-capacity buffer would have let one stalled client pin
19-38 MiB on a board with 37 MiB of RAM.

With size-matched dispatch the invariant `cap ≈ len` survives: an over-`keep_cap`
buffer is only ever filled by an over-`keep_cap` frame. The residual overhang is
the variation between successive large frames on the same source (the slot
ratchets to the largest seen, bounded by `MS_AU_BUF_MAX` = 1 MB), and at most
one packet per source can carry it at a time.

### Interaction with today's fMP4 zero-copy change (`db64973`)

`stream_mp4()` now holds iovecs pointing into `p->data` until after the
`csendv()` completes, and only then calls `pkt_unref(p)`. Nothing about that
changes here: this change does not move *when* the last unref happens, only
what becomes of the memory afterwards. The buffer cannot be recycled before
the consumer's own reference is dropped, which is the same contract `rtsp.c`'s
"ZERO-COPY FLUSH BARRIER" has relied on since the RTP sinks went zero-copy.

---

## What was tested

**1. `make test-hub-pool` (new, `scripts/test_hub_pool.c`).** The property under
test is *which allocations are not made*, which is invisible from the outside,
so the harness counts `frame.c`'s own `malloc`/`free` calls via linker
`--wrap`. 11 cases, **60085 checks, all passing**:

- the freelist path is byte-for-byte unchanged (pointer identity + zero
  allocations on a recycled small borrow);
- an oversized buffer is retained on last unref and reused with **zero**
  allocation, steadily (20 further large frames: 0 mallocs, 0 frees);
- size-matched dispatch, **both directions** — a small borrow is never handed
  the retained big buffer, and a big borrow never raids the small freelist;
- at most one oversized buffer is retained (the second and third return free
  exactly 2 blocks each);
- `pkt_pool_trim` frees exactly the slot, is idempotent, is a no-op on an empty
  pool, and leaves the freelist alone;
- the retained buffer grows once and is then reused for smaller frames without
  allocating;
- `max_free` still bounds the freelist at 4;
- the refcount still gates reuse (a buffer with subscribers outstanding is
  never handed out);
- `pkt_new()` packets are still never pooled;
- a 20 000-frame randomised fan-out (mixed P/IDR/JPEG sizes, 0-4 subscriber
  queues draining at their own pace, random trims) closes with **0 blocks
  live**;
- a concurrent producer / 2 consumers / trimmer run (60 000 frames, trims
  landing while packets are in flight) likewise.

Every case drains its pool and the harness asserts the process-wide
malloc/free balance is zero afterwards, so a leak, a double free or a silently
dropped packet fails the case rather than needing a sanitiser to surface.

**2. The test discriminates.** Built against the pre-change `frame.c` (HEAD's
version plus a two-line shim so it compiles against the new struct), it fails
**14 checks across the 5 cases that assert the new behaviour** — "last unref of
a big buffer freed 2 blocks - it must be retained", "trim freed 0 blocks, want
2", "20 further big frames cost 40 mallocs, want 0" — while the 6 cases that
assert *unchanged* behaviour (freelist bound, refcount gate, `pkt_new`,
randomised fan-out, concurrency) pass on both. Total allocations over the same
workload: **46 187 before, 36 533 after.**

**3. Sanitisers.** `make test-hub-pool SAN=asan` (ASan+UBSan) and
`SAN=tsan` (ThreadSanitizer): all 60085 checks pass, **no reports of any
kind**, 0 blocks live. The TSan run is what covers the trim racing the
producer and the consumers' cross-thread returns.

**4. ASan+UBSan sim under multi-client load, with realistic frame sizes.** The
2026-08-07 review's own validation shape, repeated: a sanitised `make sim`
build fed a **1080p 20 Mbps H.264 clip (167 KB mean AU, IDRs well over the
96 KB split)** and an **820 KB 1080p JPEG** — deliberately the exact size
`hal_ingenic.c` documents for cam-L — with 3 RTSP clients (2 TCP, 1 UDP),
3 fMP4 clients, 3 MJPEG clients, a 120-request snapshot hammer and 20
1-second fMP4 reconnects, for ~35 s, then a clean SIGTERM. **No ASan or UBSan
report of any kind**, no leak at exit, orderly teardown ("all client threads
gone after 0 ms"). This is the case where the new `big` slot is genuinely hot:
every JPEG frame and every IDR goes through it, while the fMP4 consumers hold
iovecs into those same buffers across their sends.

**5. No regression, and a null result worth recording.** Baseline vs. changed
`make sim` builds under identical load (3 MJPEG on the 820 KB source, 2 fMP4,
1 RTSP, 15 s measurement window, 3 runs each):

| | CPU ticks | VmRSS |
|---|---|---|
| baseline | 61 / 74 / 58 | 25 696 / 25 696 / 25 640 kB |
| this change | 64 / 70 / 68 | 25 756 / 25 632 / 25 812 kB |

Indistinguishable, in both directions — which is the *expected* result given
the glibc measurement above, and is reported as confirmation that nothing
regresses rather than as evidence of benefit. Note in particular that the RSS
cost of the retained buffer does not show up here either, for the same reason
(glibc keeps the freed chunk in the heap regardless); on the target it will be
real. See residual risk 1.

**6. Regression.** `make sim` warning-free; `make test-config`, `make test-fmp4`
(11/11) and `make test-auth` (PASS=4 FAIL=0) unchanged.

**7. Binary size** (T31 cross-build, `./build.sh timps T31`, built in a clean
worktree at HEAD so a concurrent unrelated change could not skew it):

| | .text | .data | .bss | stripped file |
|---|---|---|---|---|
| baseline | 996 619 | 8 712 | 173 012 | 1 054 148 |
| this change | 996 975 | 8 720 | 173 044 | 1 054 156 |

**+356 B `.text`, +8 B on flash.** The +32 B `.bss` is the new pointer in each
of the 8 per-source pools.

### Not tested on hardware

No camera was flashed. Garage was in use by a concurrent unrelated soak and
Schuppen is a live production camera; the only things done to either were
read-only `/proc` sampling, one config read, one `/snapshot.jpg` fetch (normal
on-demand behaviour, self-limiting via the idle stop) and the standalone
microbenchmark above, which never touches `timpsd`. So:

- **unverified:** that the daemon's own RSS and CPU move as the microbenchmark
  predicts under a real MJPEG viewer, and that nothing regresses over a soak.
  The correctness argument does not depend on this — it rests on the unit test
  and the sanitisers — but the *benefit* figure is inferred from a
  microbenchmark of the same allocator on the same SoC rather than measured in
  situ.
- The natural confirmation, when a camera is free, is `strace -c -p $(pidof
  timpsd)` over ~20 s with an MJPEG client attached in daylight: `mmap`/`munmap`
  counts should drop to near zero for the JPEG source, and minor faults with
  them.

---

## Residual risk and follow-ups

1. **One buffer of extra residency per active source.** The retained slot
   ratchets to the largest frame that source has produced (≤ `MS_AU_BUF_MAX`,
   1 MB) and is held for as long as the source keeps publishing. An MJPEG
   viewer attached through a bright afternoon into the night leaves an 800 KB
   buffer retained even though frames have since shrunk to 224 KB, until the
   viewer leaves and the idle stop trims it. This is the real cost of the
   change and it is deliberate: it is the same bargain the pool already makes
   for small buffers, and today's fMP4 change freed 410 KB *per client* against
   it. If RSS ever becomes the binding constraint, the follow-up is a decay —
   trim the slot when the last N frames all came in far under `cap` — not a
   smaller cap, which would forfeit exactly the 800 KB case that pays for this.
2. **The trim can race a late return.** A packet still queued at idle-stop
   returns after the trim and refills the slot. Reaching the idle stop means
   `MS_IDLE_STOP_US` with no subscriber, so in practice everything has long
   since returned; and the worst case is one buffer, self-limiting. Not worth a
   second flag.
3. **A latent use-after-unref is now slightly quieter above 96 KB.** Recycled
   memory stays mapped where `free()`d memory (above the mmap threshold) would
   fault. This is not a new class of risk — every sub-96 KB frame, i.e. the
   overwhelming majority, has behaved this way since P-01 — but it now extends
   to the large frames. The ASan run over the randomised fan-out and the
   concurrent case is the counter-measure.
4. **`HUB_POOL_KEEP_CAP` is now doing a subtly different job** — a dispatch
   split rather than a discard threshold — under the same name. The block
   comment in `hub.c` says so explicitly; anyone tuning it via `-D` should read
   it, because lowering it now widens what the `big` slot serves rather than
   narrowing what is kept.
