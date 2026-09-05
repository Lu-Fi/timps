# MJPEG client queue depth 8 -> 2 — 2026-09-05

A one-constant change in `src/mp4/httpd.c`, plus the unit test that now holds
the fanqueue overflow contract in place. Third item from the same review pass as
`FMP4_ZEROCOPY_2026-09-05.md`.

**Scope:** HTTP MJPEG streaming clients (`/stream.mjpeg`) only. The snapshot
path, RTSP, fMP4, SRT and the recorder keep their capacities.

---

## What changed

`stream_mjpeg()` subscribed its client with `fanqueue_init(&q, 8)` — a bare
literal, unchanged since the initial import (`37af211`), with no comment and
nothing in the commit history or `dev_notes/` weighing it. It is now
`MS_MJPEG_QCAP`, defined next to `MS_MP4_QCAP` with the rationale, and set to
**2**. `-D` overridable like the other QCAPs.

The reasoning, in one line: **MJPEG has latest-frame semantics.** Every part is
a self-contained JPEG, so a client that has fallen behind gains nothing from a
deep backlog — the frames it would eventually drain are stale by the time it
gets them, and depth buys only latency and pinned memory. fMP4/RTSP need depth
for the opposite reason (dropping one P-frame corrupts the rest of the GOP, so
their queues absorb bursts instead), which is why `MS_MP4_QCAP` stays at 64.

The queue only fills once the socket send buffer is full — i.e. the client
genuinely cannot keep up; TCP already smooths ordinary jitter — so 2 slots (one
being written, one fresh behind it) is the useful depth.

## What it is worth, on this fleet's numbers

Schuppen's live daemon serves **~380 KB** JPEGs (1080p, five consecutive
`/snapshot.jpg` fetches: 381557, 380385, 379908, 379112, 378659 B). At that size
the `FQ_MAX_BYTES` (2 MB) budget is what bound cap 8, not its slots:

| | frames pinned by a stalled client | bytes |
|---|---|---|
| cap 8 | 5 (byte-budget-capped) | **1900 KB** |
| cap 2 | 2 | **760 KB** |

~1.1 MB per stalled MJPEG viewer, on a camera with 37 MB of RAM in total. At
200 KB JPEGs the slot cap binds instead and it is 1600 KB -> 400 KB.

Latency: MJPEG runs at 5 fps in this fleet (`PTZ_LATENCY_INVESTIGATION_2026-08-29.md`
§5), so a lagging viewer drifted up to ~1.6 s behind before drop-oldest engaged.
Now ~0.4 s.

---

## What was tested

**1. New unit test — `make test-fanqueue` (`scripts/test_fanqueue.c`).**
The change is a capacity, and every bound that matters is enforced in one
capacity-dependent place (`fanqueue_push()`'s drop-oldest loop), so the test
targets that loop rather than the MJPEG loop around it. 69 checks, all passing:

- drop-oldest keeps the **newest** packets *in order* at caps 1, 2 and 4 —
  proven by sequence number, not just by count;
- `FQ_MAX_BYTES` bounds pinned payload independently of the slot count (200 KB
  and 800 KB frames, both capacities), and a single packet larger than the whole
  budget is still admissible and still evicted by the next one;
- `dropped_key`/`dropped_any`/`dropped_audio` and the **headless-GOP forward
  drop** still fire at cap 2 — the smaller queue does not quietly disable the
  eviction logic the coded-stream consumers depend on;
- `fanqueue_pop_ex()` does not invent a drop flag on a dry pop;
- close semantics (post-close push dropped and unref'd, queued packet still
  pops, pop on a closed empty queue returns at once);
- a **live producer thread against a deliberately slow consumer** (8x slower),
  asserting frames arrive newest-forward and the backlog never exceeds capacity.
  Measured: average backlog **6.25 frames at cap 8 -> 0.95 at cap 2**, max 7 -> 1.
  That number *is* the viewer's staleness.

The test is `-D FQ_MAX_BYTES`-aware, so it moves with the T10/T20/T21 platforms
that halve the budget.

**2. The test was checked against deliberate bugs, not just run.** Four mutants
of `fanqueue.c`, each caught (30, 3, 2 and 4 failing checks respectively):
slot cap never trips; byte budget removed; headless-GOP forward drop disabled;
drop-newest instead of drop-oldest.

**3. Clean under sanitizers.** ASan + UBSan + LSan: no leaks, no reports — i.e.
every evicted packet really is unref'd, which is what keeps a stalled client
dropping rather than leaking. TSan clean on the producer/consumer case.

**4. End-to-end through the real MJPEG path, host sim.** `make sim` fed a
189725 B JPEG at 5 fps; four clients with `SO_RCVBUF` forced to 2048 connect to
`/stream.mjpeg` and then never read. Baseline arm is the same tree built with
`-DMS_MJPEG_QCAP=8`. Peak `VmRSS` above the idle-with-one-healthy-client
baseline, two runs each:

| | run 1 | run 2 |
|---|---|---|
| cap 8 | +1804 kB | +1804 kB |
| cap 2 | +864 kB | +864 kB |

~940 kB less pinned across four stalled viewers. (Packets are refcounted and
shared between subscribers, so simultaneously-stalled clients pin the *union* of
the newest `cap` frames rather than `cap` frames each — the saving does not
scale with client count when they stall together, and does when they stall at
different moments.)

Healthy clients are unaffected: 10 s captures, multipart parsed and every part
checked for a complete JPEG (SOI..EOI) against its `Content-Length`:

| | healthy client | slow client (50 ms per read) |
|---|---|---|
| cap 8 | 50 frames, 0 bad, 5.00 fps | 49 frames, 0 bad, 4.90 fps |
| cap 2 | 50 frames, 0 bad, 5.00 fps | 49 frames, 0 bad, 4.90 fps |

**5. Builds.** Host sim clean, no new warnings. T31 cross-build via
`./build.sh timps T31` clean, and the size delta against the same tree built
with `-DMS_MJPEG_QCAP=8` is **zero** — `.text` 996975, `.data` 8720, stripped
file 1054156 B in both. The queue is heap-allocated from a runtime `cap`, so the
constant costs nothing in flash either way.

---

## No hardware run, and why

Deliberate. The working tree carries another change in progress
(`src/frame.c`/`src/frame.h`), so a camera binary built from it today would put
*that* unfinished work on a production camera rather than isolating this
one-constant change — the opposite of a conservative test. Garage
(192.168.10.21) is also still on the raptor+WebRTC build (see
`FMP4_ZEROCOPY_2026-09-05.md`), so it is not a free target either.

What *was* taken from real hardware is the thing the argument actually rests
on — Schuppen's live JPEG sizes (~380 KB), which is what puts cap 8 against the
byte budget rather than its slot count. The behaviour itself is exercised at
unit level against the real `fanqueue.c` and end-to-end through the real
`stream_mjpeg()` in the sim.

## Residual risk

- **Low.** The only behavioural difference is that a client which cannot keep up
  drops a stale frame sooner. It cannot affect a client that keeps up (proven
  above at 5 fps, and the queue is empty for such a client by construction), and
  it cannot affect any other consumer — no other call site shares the constant.
- **Higher `jpeg.fps` shortens the buffer in wall-clock terms** (2 frames is
  80 ms at 25 fps, 400 ms at 5). That is the intended trade — at 25 fps a viewer
  that stalls for 80 ms *should* be shown the newer frame — but if a fleet camera
  is ever run at a high `jpeg.fps` over a lossy link and the stream looks
  choppier than it used to, this is the knob (`-DMS_MJPEG_QCAP`).
- The pre-existing `FQ_MAX_BYTES` interaction is now documented rather than
  changed: below ~350 KB JPEGs the slot cap binds, above it the byte budget does.
