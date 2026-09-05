# Perf review follow-ups #4-#7 — measured, none implemented — 2026-09-05

The four lower-confidence items from the same review pass that produced
`FMP4_ZEROCOPY_2026-09-05.md` (`db64973`), `MJPEG_QUEUE_DEPTH_2026-09-05.md`
(`45aec8a`) and `FRAME_POOL_BIG_2026-09-05.md` (`3cade7d`). Those three were
implemented. **These four are not**, and this note is the record of why —
three of them because a measurement that had never been taken says the payoff
is between "nothing" and "negative", and one (#6) because the disassembly says
the proposed change would have produced a **wrong number**.

Nothing here needs re-investigating unless the underlying numbers move. The
measurements are the deliverable.

---

## Measurement rig

Everything below was measured on the **Garage T31** (`cam-garage`,
192.168.10.21, kernel 3.10.14 `isvp_swan_1.0`, uClibc-ng), the disposable test
unit, with a static MIPS scratch binary — the same approach the frame-pool work
used. Load average was 1.8 throughout, so the syscall figures are if anything
pessimistic. Three runs, spread ≤3%:

| | µs/call |
|---|---|
| `clock_gettime(CLOCK_MONOTONIC)` | **0.52 – 0.55** |
| `poll()`, 1 fd, already readable | 1.81 – 1.85 |
| `read()`, 2 B / 4 B / 160 B | 1.24 / 1.25 / 1.40 – 1.59 |

The first row is the important one and it is **not** what the findings assumed.
`clock_gettime` on this kernel costs ~0.53 µs, not the ~2 µs a "MIPS 3.10 has
no vDSO, so it is a full syscall" argument implies. Every "N syscalls/s"
estimate in findings #5 and #7 is therefore roughly **4x too pessimistic**.

Also confirmed here: Garage's `libimp.so` is **byte-identical**
(`md5 97ca48de…`) to the vendored
`dl/ingenic-lib/git/T31/lib/1.1.6/uclibc/5.4.0/libimp.so`, which is what makes
the static analysis in #6 binding on the fleet and not just on one camera.

---

## #4 — `FQ_MAX_BYTES` worst case after `462cd49` (HTTP_MAX_CLIENTS 8 → 16)

**Verdict: no change needed. The HTTP worst case went *down* today, not up.**

The concern was that doubling `HTTP_MAX_CLIENTS` doubled the theoretical
stalled-client pin. It did — but `45aec8a` (MJPEG queue depth 8 → 2) landed the
same day and cut the per-client figure by more than the client count grew, for
the case that actually bound.

### What carries a fanqueue

| consumer | slots | cap constant | count |
|---|---|---|---|
| HTTP fMP4 (`/stream.mp4`, `/rt`) | 64 | `MS_MP4_QCAP` | ≤ `HTTP_MAX_CLIENTS` |
| HTTP MJPEG (`/stream.mjpeg`) | **2** | `MS_MJPEG_QCAP` | ≤ `HTTP_MAX_CLIENTS` |
| RTSP session | 64 | `MS_RTSP_QCAP` | ≤ `RTSP_MAX_CLIENTS` (8) |
| SRT client | 128 | `SRT_QCAP` | ≤ `SRT_MAX_CLIENTS` (8), `USE_SRT=0` by default |
| recorder / clip | 128 | `REC_QCAP` | 1–2 |

**SSE/`/events` carries no fanqueue at all** — which matters, because
`preview.html` holds 3+ HTTP slots per tab and two of them are SSE. The
snapshot path (`hub_grab_jpeg`, 4 slots) is transient, one grab long.

Pinned bytes per queue = `min(slots × frame_size, FQ_MAX_BYTES)`. Which term
binds is the whole question.

### At this fleet's real settings (1200 kbps @ 15 fps, 380 KB 1080p JPEGs)

| | slots × size | pinned |
|---|---|---|
| fMP4 / RTSP | 64 × 10 KB | **640 KB** (slot-bound) |
| MJPEG, cap 2 | 2 × 380 KB | **760 KB** (slot-bound) |
| MJPEG, cap 8 (before `45aec8a`) | byte-bound | **~1900 KB** |

The byte budget only binds for a coded stream above **~3.84 Mbit/s at 15 fps**
(6.4 Mbit/s at 25 fps) — i.e. `2 MB / 64 slots = 32 KB` average frame — and for
MJPEG only above **~1 MB JPEGs**. Neither happens on this fleet.

So the all-clients-stalled worst case, HTTP component only:

| | before today | after today |
|---|---|---|
| 8 clients × MJPEG cap 8 | **15.2 MB** | — |
| 16 clients × MJPEG cap 2 | — | **12.2 MB** |
| 8 → 16 clients × fMP4 | 5.1 MB | 10.2 MB |
| **HTTP worst case (max of the rows)** | **15.2 MB** | **12.2 MB** |

Whole-daemon worst case at fleet settings: 12.2 (HTTP) + 5.1 (RTSP) + ~2.6
(recorder) ≈ **20 MB**, and only if all 24 clients stall simultaneously *at
staggered offsets* — packets are refcounted and shared, so clients that stall
together pin the union of the newest frames, not one set each.

### The theoretical ceiling, and the board it would matter on

If the byte budget ever binds (high bitrate, or >1 MB JPEGs):

| | `FQ_MAX_BYTES` = 2 MB | = 1 MB (T10/T20/T21) |
|---|---|---|
| HTTP 16 | 32 MB | 16 MB |
| RTSP 8 | 16 MB | 8 MB |
| recorder 2 | 4 MB | 2 MB |
| **total (SRT off)** | **52 MB** | **26 MB** |

Against real board RAM:

- **37–38 MB** `MemTotal`, `rmem=22M` — `cinnado_d1_t31l` / `wuuk_y0510_t31x`,
  **5 of the 12 fleet cameras** (cam-db, cam-schuppen, cam-wintergarten,
  cam-sz, cam-kinder-rechts; see `TODO.md` under the 2026-08-22 encoder-init
  finding).
- **74 MB** `MemTotal`, `rmem=50M` — cam-garage, measured today.
- `timpsd` RSS on Garage right now: **4.8 MB** (VSZ 101 MB).

### The actionable part

The P-08 build flag is keyed on **SoC generation** (`T10`/`T20`/`T21` get
`-DFQ_MAX_BYTES=1048576`), but the constraint is **board RAM**, and the
tightest boards in this fleet are 37 MB **T31**s — which get the 2 MB default.
`PLATFORM=T31` cannot distinguish them from the 74 MB Garage, so this can never
be a Makefile switch; it belongs in the per-camera Kconfig / `local.fragment`
in the firmware repo.

**No action today** — at 1200 kbps the slot cap binds and the byte budget is
never reached. But if a 37 MB board is ever configured above ~3.8 Mbit/s (15
fps) or with JPEGs over 1 MB, it should get
`-DFQ_MAX_BYTES=1048576 -DHTTP_MAX_CLIENTS=8` at the same time. That is the
trigger condition to remember, not a number to change now.

Minor, for completeness: each HTTP connection thread also reserves
`MS_STACK_CONN` = 256 KB of stack, so the client-cap doubling added 2 MB of
*virtual* reservation. Stacks fault in lazily; RSS only reflects what is
touched.

---

## #5 — WebSocket talk path: ~10 syscalls and two copies per audio frame

**Verdict: investigated, measured, declined.**

The finding is factually right about the shape. `ws_read_message()`
(`src/ws.c`) reads a 20 ms mu-law frame as three separate `read_exact()` calls
— 2-byte header, 4-byte mask, then the payload — each preceded by its own
`ws_now_ms()` + `poll()`, plus one `ws_now_ms()` for the deadline and one for
`last_rx_ms`. That is 4 `clock_gettime` + 3 `poll` + 3 `read` = **10 syscalls**,
and the payload is then copied into `c->frag` and again into the caller's
buffer. Confirmed by reading the parser, not assumed.

### What it actually costs

Measured on Garage by replaying exactly that call sequence against a prefilled
socketpair, versus the proposed shape (one `poll` + one `read` into a 4 KB
staging buffer + one clock read):

| | per frame | at 50 frames/s |
|---|---|---|
| current sequence | 14.1 µs | **0.71 ms/s** |
| staging buffer | 6.3 µs | **0.32 ms/s** |
| **recoverable** | 7.8 µs | **0.39 ms/s** |

**0.39 ms/s is 0.04% of one core** — and only while somebody is holding
push-to-talk. The finding estimated 2–3 ms/s; the real total is 0.71 ms/s,
because `clock_gettime` is 0.53 µs here and not ~2 µs.

The two payload copies are 160 bytes each (960 B at 48 kHz). They do not
register.

### Why that is not worth doing

Three reasons, in increasing order of weight:

1. **`USE_BC_WS` is off by default** and is a per-camera Kconfig opt-in
   (`BR2_PACKAGE_TIMPS_BC_WS`), so on most of the fleet this code is not even
   linked.
2. **`src/ws.c` is vendored** from thingino-motors
   (`feature/websocket-daemon`) under an explicit contract, stated in its own
   header: *"kept as close to the original as possible so the two copies stay
   diffable"*, with exactly two documented deltas (the transport seam and
   `WS_MAX_PAYLOAD`). Restructuring the frame reader would be a third, far
   larger divergence in the file that is supposed to stay a near-copy.
3. **It is the security boundary.** `ws_read_message()` is what enforces the
   mask requirement (RFC 6455 §5.1 — the anti-cache-poisoning rule), the RSV
   check, the 64-bit length sign check, the control-frame constraints and the
   `WS_MAX_PAYLOAD` reassembly bound that stops a fragment sequence becoming a
   memory bomb. A staging-buffer rewrite changes the byte-consumption
   discipline of every one of those, and a desynchronised WebSocket parser
   fails silently rather than loudly.

Spending that risk to recover 0.04% of a core during push-to-talk is not a
trade worth making. Reconsider only if talk latency or CPU is ever *measured*
as a problem on a single-core board — which is what the finding itself said,
and the measurement now says it is not.

---

## #6 — T31 `IMP_Encoder_GetChnAveBitrate` on every encoded frame

**Verdict: measured by disassembly, declined — and the proposed throttle would
have been a bug, not just a no-op.**

The finding asked for timing before changing anything, and flagged libimp as
opaque. It is opaque to a debugger, but not to `objdump`. The function is 744
bytes at `0x8754c` in the exact `libimp.so` running on the fleet.

### What it does

The steady-state path — the one that runs on **every** frame — is:

1. validate `encChn < 9` and `stream != NULL`;
2. loop over `stream->packCount` packs (stride 32 = `sizeof(IMPEncoderPack)`),
   summing `pack[i].length` — **1–2 iterations** for H.264;
3. add that to a per-channel byte accumulator in `.bss`;
4. increment a per-channel frame counter; if it has not yet reached the
   `frames` argument, **return 0**.

That is roughly **25–30 instructions, no syscall, no lock, no allocation, no
floating point** beyond the prologue's `sdc1`/`ldc1` of `$f20`. Call it well
under a microsecond. At 25 fps across two streams it is on the order of 1500
instructions per second — comfortably below the noise floor of anything.

The expensive path (two indirect calls for a timestamp and a 64-bit divide,
then a `div.d`) runs **once every `frames` frames**, i.e. once per GOP, which
is already the averaging window.

So the answer to "is it more than a few µs" is **no**, decisively, and the
throttle would save nothing measurable.

### The part that matters more

The disassembly also shows *why* the value is computed the way it is, and it
rules the change out on correctness grounds:

```
876e4:  lw   a2, 0(s3)      # per-channel byte accumulator
876f0:  sll  a2, a2, 3      # bytes -> bits
...     __floatdidf(elapsed_ms)
87728:  div.d $f0, $f20, $f0 # bits / elapsed_ms  == kbit/s
```

The accumulator is filled **only by the calls that are actually made**, while
the divisor is wall-clock elapsed time between the first call and the
`frames`-th. Calling it on one frame in N therefore accumulates 1/N of the
bits over the *same* elapsed time, and the reported average bitrate comes out
**N times too low**.

Gating this to "once per second" or "once per GOP" would not have been a free
optimisation with a staleness trade-off. It would have silently broken
`/control`'s `ave_bitrate`. The existing call-every-frame placement — with its
comment about needing the just-fetched `st` and running before
`ReleaseStream` — is not incidental; it is required.

**Nothing to change.** Worth leaving the reasoning here so the next reviewer
who spots a vendor call in a hot loop does not re-open it.

---

## #7 — Three clock reads per audio frame in `audio_thread`

**Verdict: implemented, measured, then reverted. Recommending against it.**

The opportunity is real and I read the code to confirm it: `audio_thread`
(`src/hal/hal_ingenic.c`) takes `ms_now_us()` at the top of each iteration
(`a_t0`), again at publish time in whichever of the three codec branches runs
(`a_now`), and a third time for the pacing check at the bottom. The third one
is genuinely redundant — only a hub fanout and `IMP_AI_ReleaseFrame` separate
it from the second.

I implemented it: hoist an `int64_t a_pub = 0;` next to `a_t0`, have all three
codec branches assign it instead of declaring their own `a_now` (a net
*removal* of one local per branch), and make the pacing check
`(a_pub ? a_pub : ms_now_us()) - a_t0`. It compiles clean under `-Wall
-Wextra` on the T31 cross build, and — checked separately with stub headers,
since the default build has neither — in the `USE_FAAC` and `USE_STREAM_OPUS`
branches too.

Then I measured both sides:

| | |
|---|---|
| saved | 25 AI frames/s × 0.53 µs = **13 µs/s** = **0.0013% of one core** |
| cost | **+144 B `.text`** (996975 → 997119; stripped file unchanged at 1054156 B, by alignment luck) |

That is the whole trade: one part in 75,000 of a core, against 144 bytes of
flash on boards where flash headroom is a live constraint, plus a conditional
in a hot loop and eleven lines of comment explaining why reusing a slightly
earlier timestamp is safe. Code that needs a paragraph to justify a
microsecond is worse code.

There is one variant with no size cost — hoist a single unconditional
`ms_now_us()` to just after the mute check and use it for both the PTS and the
pacing — but it moves the clock reference *before* the encode, which changes
the wall-clock input to `pts_sanitize()` on the carefully-built A1
capture-clock path. Real behavioural change to the media clock, for
0.0013% of a core. Not worth it either.

So: reverted, tree unchanged. The finding's own guidance was *"belongs in
whatever next touches this function rather than its own commit"* — and since
#6 turned out to need no change, nothing else is touching `audio_thread`
today. If someone does touch it later, the patch is described precisely enough
above to re-apply in five minutes.

Direction of the drift, for whoever does: reusing `a_pub` under-reads elapsed
time by the publish duration, so the pacing sleep comes out those few
microseconds *too long* against its 15 ms target — the harmless direction for
a guard whose only job is to stop the loop spinning.

---

## What was not done

- **No hardware deploy.** Nothing was changed, so there was nothing to deploy.
  Garage was used read-only: a static scratch benchmark in `/tmp` (removed
  afterwards) and a `cat` of `/usr/lib/libimp.so`. `timpsd` was not restarted,
  no flash was written, and no production camera was touched. Schuppen
  (192.168.10.25) was down for the whole session and was not needed.
- **No new unit test.** The two candidates for one (#5's frame parser, #7's
  pacing) were both declined, and a test for behaviour that did not change
  would only assert the status quo.
- **The T31 cross build was exercised anyway** (`./build.sh timps T31`, clean,
  no new warnings) to produce the before/after size numbers in #7, and the
  before/after binaries are byte-size identical.
