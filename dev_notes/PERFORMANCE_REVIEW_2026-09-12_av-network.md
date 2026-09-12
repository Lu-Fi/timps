# timps performance review — audio / video / network — 2026-09-12

**Scope:** the audio, video and networking subsystems only — `codec/`, `hal/`
(video + audio producer paths, OSD), `rtsp/`, `mp4/`, `hub.c`/`fanqueue.c`/
`frame.c`, `net.c`, `srt.c`, `tls.c`, `record.c`/`timelapse.c` (I/O shape only),
`events.c`. **Not** a correctness/security review (one ran in parallel on
tonight's `control.c`/`httpd.c` changes), and not a review of `config.c`,
`daynight.c`, `control.c` or `imp_motion.c` beyond where they touch the A/V
pipeline.

**Basis:** static reading of the current tree at `61733fb`, plus **live
per-thread CPU sampling on two production cameras** (cam-schuppen T31,
Galayou T23) — read-only, no config change, no restart, no flash write.
The live data is the novel part of this pass and it changes where the
remaining effort should go.

**Prior art checked before writing anything** — `PERFORMANCE_AUDIT_2026-08-07`,
`PERFORMANCE_REVIEW_2026-08-28`, `PERF_FOLLOWUPS_2026-09-05`,
`FMP4_ZEROCOPY_2026-09-05`, `FRAME_POOL_BIG_2026-09-05`,
`MJPEG_QUEUE_DEPTH_2026-09-05`, `PREVIEW_REALTIME_IDEAS_2026-08-29`. Items
already implemented, measured-and-declined, or explicitly rejected there are
**not** re-raised; the ones a reader might expect to see are listed in
*Already settled elsewhere* at the bottom with a pointer.

---

## Overall verdict, per subsystem

**Audio — reviewed, essentially nothing to do.** Capture → encode → publish →
packetize → wire is allocation-free and copy-free from the pooled packet all
the way to `sendmsg`. The per-sample loops run at 8–16 kHz, i.e. thousandths
of a core. The one structural asymmetry worth naming (the AI capture channel
is never stopped when nobody is listening, unlike the video framesource) was
**measured on hardware at 0.25 % of a core** and is therefore declined, not
recommended. What is left is ~40 KB of oversized buffers, free to fix.

**Video — the timps-side code is done; the remaining lever is libimp
configuration.** Every classic finding has already been taken: pooled
packets, one assembly copy, single-pass NAL indexing, gather-write fMP4,
cache-tiled SW rotate, 1 Hz OSD rasterization with change detection. The
live measurement (below) says the daemon's own threads are now a **minority**
of its CPU on a T31 — the majority sits in libimp's per-frame group threads,
which timps configures but does not write. That is the finding.

**Network — reviewed, clean.** Socket options are right (including the
deliberate absence of `SO_SNDBUF`), every per-frame path uses a gather write
on the plain transport, the TLS serialization is measured and deliberate, SRT
batching is already at libsrt's hard ceiling. The only real item in this area
is in the recorder's failure path, not on the wire.

---

## Live baseline (new — 2026-09-12, read-only)

Per-thread `utime+stime` deltas from `/proc/<pid>/task/*/stat`, 20 s windows,
on cameras carrying their normal production load.

### cam-schuppen (T31L / sc2336, 37 MB RAM), 2 RTSP-over-TCP clients

| thread | ticks / 20 s | % of one core |
|---|---|---|
| `group_update` ×2 (hot pair) | 74 + 72 | **7.3 %** |
| `group_update` ×4 (rest) | 26 + 22 + 10 + 5 | 3.2 % |
| **libimp group threads, total** | **209** | **10.5 %** |
| timps' own threads (13 of them, summed) | 136 | 6.8 % |
| `ai-_ai_record_t` (libimp audio capture) | 5 | 0.25 % |
| `ENC(n)-update_f` + `FS(n)-tick` | 12 | 0.6 % |
| **total** | **362** | **18.1 %** |

`VmRSS` 5036 kB, 34 threads (13 timps + 13 libimp + client threads).
Cross-checked against a whole-process sample taken minutes earlier: 179
ticks/10 s = 17.9 %. `record.enabled = 0` on this camera.

### Galayou (T23N / sc2336), 1 client, substream only

| thread | ticks / 20 s | % of one core |
|---|---|---|
| `OSD-1` | 41 | **2.05 %** |
| `OSD-0` (stream 0 idle) | 0 | 0 % |
| timps threads (summed) | 69 | 3.5 % |
| `Encoder-1` + `Framesource-1` + `ENC(1)-update_f` + `FS(1)-tick` | 33 | 1.7 % |
| `ai-_ai_record_t` | 5 | 0.25 % |
| **total** | **148** | **7.4 %** |

`VmRSS` 4596 kB, 26 threads.

**Two things fall out of this and drive the findings below:**

1. On the T31, **~58 % of the daemon's CPU is in libimp `group_update`
   threads**. On the T23, the equivalent thread is named `OSD-N` and it is
   the single largest consumer — and it is **0 for the stream that has no
   subscribers**, which proves the cost is per-delivered-frame and scales
   with active streams, not a fixed init cost.
2. `ai-_ai_record_t` costs **0.25 % of a core on both SoCs**. That single
   number settles the one open audio question (see AV-D1).

---

## Findings

| ID | Sev | Topic | File:Line | Impact | Effort |
|----|-----|-------|-----------|--------|--------|
| AV-01 | 🟠 MEDIUM | The remaining CPU lever is the libimp group/OSD pipeline, not timps' C | measurement above; `imp_osd.c:417-521`, `hal_ingenic.c` bind chain | ~7–10 % of a core on a T31 sits in threads driven by OSD/group configuration | investigation first, then config |
| AV-02 | 🟠 MEDIUM | `seg_open()` retries at packet rate: ~400 syscalls/s + 31 unrate-limited `LOGE`/s during any SD failure, plus create/unlink churn on the card at every cold start | `record.c:665-670`, `:299-341`, `:374-378` | bounded to failure windows, but those are unbounded in time; hits flash and the central log collector | small |
| AV-03 | 🟡 LOW | Audio pool borrows are 3–10× the reachable frame size; `faac_max` is computed and then unused | `hal_ingenic.c:3672`, `:3933`, `:3973`, `:3999-4001` | ~30–45 KB resident heap, in the size class the 2026-08-07 audit flags for uClibc fragmentation | trivial |
| AV-04 | 🟡 LOW | The recorder is the last consumer still on the contiguous fMP4 path | `record.c:401-402`, `:406`, `:420` | 128 KB pinned at fleet settings, **512 KB at 4–6 Mbps**; ~0.1–0.3 % of a core | medium |
| AV-05 | 🟡 LOW | `SPK_DEC_FR = 4096` → 24 KB always-resident BSS + a 512 ms lock quantum | `speaker.c:182`, `:202-203`, `:346` | 24 KB BSS paid even on cameras that never play a clip; STOP latency | trivial |
| AV-06 | 🟡 LOW | SRT caller-mode reconnect backoff is a 10 Hz `usleep` poll — the last un-converted P-02 site | `srt.c:769` | ~0.01 % of a core; removes a 10 Hz timer from an otherwise idle daemon | trivial |
| AV-07 | ℹ️ INFO | `backchannel.c` `g_pcm[8192]` is ~2× the reachable maximum | `backchannel.c:53` | 8 KB BSS; coupled to `SPK_RS_MAX` (`speaker.c:32`) by comment only | trivial |
| AV-08 | ℹ️ INFO | `stream_mjpeg()` is the one consumer the `fanqueue_pop_ex()` conversion missed | `httpd.c:896-897` | 2 extra queue-lock cycles per frame at 5 fps — consistency, not CPU | trivial |
| AV-09 | ℹ️ INFO | SSE emits one `send()` per event during a burst; two `csend`s at connect | `httpd.c:1153-1161`, `:1312`+`:1315` | WiFi airtime from small segments, not CPU | trivial |
| AV-10 | ℹ️ INFO | Dead `memmove(..., 0)` in the per-TS-packet loop | `srt.c:376` | zero runtime cost (GCC folds it); it reads like a shift that isn't happening | trivial |
| AV-11 | ℹ️ INFO | `RING_MAX_BYTES` = 4 MB is reachable on a 37 MB board if `pre_roll_s` is raised | `record.c:60`, `config.c:387` | 11 % of board RAM pinned at `pre_roll_s` ≫ default; a known ceiling, not a bug | – |
| AV-12 | ℹ️ INFO | `SRTO_RCVBUF` unset on strictly send-only SRT sockets | `srt.c` (`srt_common_opts`) | unknown: 32–128 KB × up to 8 clients depending on libsrt version | 5-minute check |

---

### AV-01 — the daemon's own code is now the minority of its CPU

This is the reframing the measurement forces, and it is why there is no
Tier-1 C-level finding in this review.

On cam-schuppen under its normal two-RTSP-client load, timps' thirteen own
threads account for **6.8 % of a core** — the encoder drain, AU assembly,
`au_is_key`, RTP packetization, the fan-out, two client loops, OSD
rasterization, day/night, motion, everything. The six libimp `group_update`
threads account for **10.5 %**. The Galayou shows the same shape with a
clearer label: the libimp thread is literally called `OSD-1`, it costs
2.05 % of a core, and its sibling `OSD-0` costs exactly **zero** because
stream 0 has no subscribers.

What this does and does not establish:

- **Established:** the dominant per-frame cost on these boards is inside
  libimp's group pipeline, it scales with the number of *actively delivering*
  streams, and it is configured entirely by timps (`IMP_OSD_CreateGroup` /
  `RegisterRgn` / the FS→OSD→ENC bind chain in `imp_osd.c:417-521` and
  `ing_start`). On the T23 that thread is unambiguously the OSD compositor.
- **Not established:** on the T31 the threads are named `group_update` and
  attribution between OSD compositing and plain encoder-group frame dispatch
  is open. Against the OSD hypothesis: the hot pair is almost exactly equal
  (74 vs 72 ticks) for a 1080p main stream and a 640×360 substream, which
  looks like fixed per-frame dispatch rather than pixel work. For it: OSD
  `font_size` is an absolute pixel height per stream, so both streams'
  overlay areas really are similar.

**Why this matters practically.** `imp_osd_setup()` creates an OSD group
whenever OSD text *or* any privacy region is wanted, and the whole video path
is then bound FS → OSD group → encoder group. A camera that needs neither
would drop a group (and its thread) out of every frame's path. The 2026-08-28
review measured the *timps-side* OSD cost (msttf rasterization, 1.2–2.4 % of
a core, 1 Hz, already gated off when nobody watches) and correctly judged the
cache not worth it — but nobody has measured the **libimp-side per-frame
compositing** cost, which this data says is 2–5× larger and paid on every
frame.

**Suggested next step, not a change:** settle it on the Garage test camera
(currently down; it is the disposable unit per the fleet notes, and the only
one where toggling OSD is acceptable). Sample `group_update`/`OSD-N` per-thread
ticks over 20 s in four states — OSD on with the default 4 items, OSD with 1
item, OSD off entirely (`osd.enabled=0`, no privacy region → no group), and
with `osd.enabled=0` on the substream only. That is ~10 minutes of work and it
tells you whether "drop OSD on the substream" is worth several percent of a
core fleet-wide, or nothing at all. **Do not change fleet config on the
strength of this note alone** — the attribution is genuinely open.

**Update 2026-09-12, measured on Garage (T31, `wuuk_y0510_t31x_sc4336p_ssv6158`,
192.168.10.21):** one RTSP/TCP client on the main stream throughout (`ffmpeg`
pulling `ch0` to `/dev/null`), 20 s `group_update` tick samples via
`/proc/<pid>/task/*/stat`, `osd0`/`osd1` item state changed live via
`POST /control` (nested-JSON form, e.g. `{"osd0":{"1":{"enabled":0}}}` - flat
dotted keys like `{"osd0.1.enabled":0}` are rejected as `unknown_fields`, see
`scripts/timps-qa.sh:2246` for the working pattern), `timpsd` restarted after
each change since the OSD/group bind happens at stream setup, not live.

| State | Main stream (osd0) | Substream (osd1) | `group_update` threads | CPU |
|---|---|---|---|---|
| A - default | 4 items | 4 items | 6 | **8.8 %** |
| B - one item | 1 item | 1 item | 6 | **4.55 %** |
| C - OSD off | off (`osd.enabled=0`) | off | **4** (2 fewer) | **2.15 %** |
| D - substream off only | 4 items (unchanged) | 0 items | 6 | 5.90 % |

A→B→C is a clean, monotonic, roughly-linear relationship between active OSD
item count and `group_update` CPU (8.8 → 4.55 → 2.15 %), and C additionally
drops two of the six `group_update` threads entirely - **this settles the
attribution the prior note left open: on the T31 the `group_update` cost is
real per-item OSD compositing, not fixed per-frame dispatch.** The T23's
unambiguous `OSD-1`/`OSD-0` naming and the T31's `group_update` naming are the
same underlying cost.

State D is the one result that doesn't fit cleanly: main-stream config was
identical to State A (4 items, the only stream with a subscriber throughout),
so D should have measured close to A if substream OSD state has no effect
without a substream subscriber - instead it came in at 5.90 %, between B and
A. Two explanations, not distinguished by this data: either substream OSD
configuration has some effect on the shared/main pipeline even without a
substream subscriber (would be a genuinely new finding), or this is
run-to-run measurement noise from a single 20 s sample after a fresh restart
(no warm-up period was used, and every state here is one sample, not an
average of several). **Given the noise in D, treat the A→B→C trend as the
reliable result and D as inconclusive** rather than over-reading it.

**Practical takeaway:** dropping from 4 OSD items to 1 saves ~4.3 points of a
core (8.8→4.55 %); dropping OSD entirely on a stream saves ~6.7 points
(8.8→2.15 %) and two threads. At fleet scale (T31 cameras, one subscriber
each) this is a genuine multi-percent-of-a-core win *if* fewer OSD items is
otherwise acceptable - but this is a **product/UX tradeoff (less on-screen
info), not a free code optimization**, and the substream-specific question
(D) needs a cleaner repeat (multiple samples, or averaging over a longer
window) before drawing a fleet-config conclusion from it specifically.
Garage was restored to its normal 4-items-both-streams default and
`timpsd` restarted before this measurement was written up.

### AV-02 — `seg_open()` retry storm on any recording failure

`rec_thread` calls `seg_open()` once per incoming packet whenever
`writing && !w_fp` (`record.c:665-670`). Exactly one failure mode is backed
off — `min_free_mb` unreachable, via `refused_until_us` at `record.c:210-211`.
Every other failure re-runs the full attempt per packet:

`prune_free()` → `statvfs`; two `config_str_lock` round-trips;
`ms_media_path()` → `time()` + `localtime_r()` + `gethostname()` (a real
syscall each on this no-vDSO kernel); `ms_mkdirs()` → one unconditional
`mkdir()` **per path component** (`util.c:294-302`, 5–8 syscalls for a dated
strftime template); `open(O_CREAT|O_EXCL)`; then an **unrate-limited**
`LOGE(MOD,"open %s: %s",...)` at `record.c:335` — `log.c` has no rate limiting
anywhere.

That is ~350–430 syscalls/s and ~31 syslog lines/s (~3 KB/s to the central
Loki collector, per camera), for the entire duration of an SD-removed /
read-only-remount / ENOSPC condition. The file's own comment already measured
the rate: *"seg_open() is called once per incoming packet while nothing is
open - measured ~31/s on a stuck camera"* (`record.c:202-204`).

There is a second, worse variant that is **not** a failure condition at all:
when `open()` succeeds but `fmp4_init_segment()` fails because the vparam
isn't warm yet (`record.c:374-378`), each attempt is a real
create + `fdopen` + `malloc(4096)` + `fclose` + **`unlink` on the card** —
two SD metadata transactions per packet, for roughly one GOP at every cold
start and after every encoder restart. `record_clip()` already gets this
right: it tests `hub_get_vparam(chn,&vp) && vparam_ready(&vp)` at
`record.c:825` *before* touching the filesystem. `seg_open()` does not.

**Fix direction.** (a) A `static int64_t retry_after_us` in `rec_thread`, set
on any `seg_open()!=0`, ~250–500 ms. This costs nothing behaviourally:
`ring_clear()` only runs after a *successful* open (`record.c:669`), so the
pre-roll survives the delay exactly as the comment at `:667-668` intends.
(b) Hoist the vparam-readiness test above the filesystem work, mirroring
`record_clip`. **Risk: low.** The only change is that recording starts up to
one backoff later after a transient failure — which is precisely the window
currently spending 31 futile attempts per second.

Note `record.enabled = 0` on the fleet cameras checked, so this is latent
today. It is still the best *code* item in this review because the blast
radius (flash wear + a log flood to the shared collector) is disproportionate
to the fix.

### AV-03 — audio pool borrows sized 3–10× larger than any reachable frame

`hub_pkt_get()` allocates `cap` bytes and the pool never shrinks a buffer
(`frame.c:111-121` grows only; `hub.c` keeps `HUB_POOL_MAX_FREE = 4` idle
buffers per source). So the audio source pins ~4× the requested cap in
resident heap regardless of what the encoder actually emits.

The AAC case is the clearest: **`faac_max` is read from the encoder at
`hal_ingenic.c:3672` and then used only in a log line at `:3679`** — the
actual borrow at `:3933` hardcodes `8192`. libfaac's `max_output_bytes` for
mono AAC-LC is on the order of 768 bytes (1536 stereo), so the pool holds
~32 KB where ~3–6 KB would do. Opus borrows 4096 against an RFC 7587 maximum
of 1275 (`:3973`). G.711 borrows a flat 2048 (`:3999-4001`) against an actual
320 samples at 8 kHz / 640 at 16 kHz.

**Fix direction.** AAC: `hub_pkt_get(HUB_AUDIO_SRC, faac_max < 1024 ? 1024 :
faac_max)` — which also makes an already-computed value do its job. Opus:
1500. G.711: the clamped `samples`, keeping `g711_max` as the sanity bound.
All three encoders already take `pk->cap` as an explicit output-capacity
argument and fail cleanly on overflow, so there is no truncation hazard. The
only thing to verify is that `faac_encoder_get_info` populated
`max_output_bytes` — it is already guarded by `FAAC_OK` at `:3670`, and the
`8192` initializer at `:3653` covers the failure case. **Risk: very low.**

Checked and confirmed harmless: `fanqueue` budgets on `p->len`, never
`p->cap` (`fanqueue.c:56,60,100`), so these oversized caps do not distort
queue admission. This is purely resident heap.

### AV-04 — the recorder is the last consumer still copying whole AUs

`record.c:401-402` keeps `static ms_buf frag` and resets it with a 256 KB
soft cap; `:406` calls `fmp4_video_fragment()` (which copies the entire access
unit into it) and `:420` `fwrite`s the result. `ms_buf_reserve` grows by
powers of two and `ms_buf_reset` only shrinks after 64 consecutive small
resets — and `util.c:69` states the failure case outright: *"at 4-6 Mbit with
a 2 s GOP every IDR fragment overshoots the recorder's 256 KB cap"*, so the
shrink run never completes and the buffer stays at high-water mark for the
life of the process.

This is the same shape the HTTP path was converted away from
(`httpd.c:499-513`, the Z1 comment), where `FMP4_ZEROCOPY_2026-09-05.md`
measured 410 kB saved per client. That note dismissed the recorder in one
line (*"the recorder writes fragments to a file, where contiguous is the
right shape"*, `FMP4_ZEROCOPY_2026-09-05.md:104`) with no measurement.
`writev()` is the gather-write for a file, and
`fmp4_video_fragment_iov()` already exists and already handles the >32-NAL
overflow internally (`fmp4.h:63-79`).

**Sized honestly:** at current fleet settings (1200 kbps @ 15 fps) an IDR is
~80–120 KB, so the pinned buffer is ~128 KB. At 4–6 Mbps 1080p it is 512 KB,
about 10 % of a ~5 MB daemon on a 37 MB board. The memcpy saving is
150 KB/s at fleet bitrate, ~500 KB/s at 4 Mbps — 0.1–0.3 % of a core.
**The RAM is the argument, not the CPU.** Syscalls are roughly a wash: at
≥1200 kbps the average fragment already exceeds the stdio buffer, so `fwrite`
is already doing flush-partial + direct-write.

**Risk: moderate**, which is why this is not ranked higher. Mixing `writev`
with the `FILE*` layer needs an `fflush` discipline or dropping stdio for the
segment writer entirely, and small audio fragments (~600 B) would each become
their own syscall unless a coalescing buffer is kept. A "when you are next in
this file" item.

### AV-05 — `SPK_DEC_FR = 4096` is 512 ms of audio

`speaker.c:182` sets the play-queue decode block to 4096 mono frames — 512 ms
at 8 kHz — which sizes `g_ilv[8192]` + `g_dec[4096]` (`:202-203`, 24 KB of
**BSS**, resident for the life of the process even on a camera that never
plays a clip) and an 8 KB stack buffer at `:346`. `play_write()`
(`:368-389`) then holds `g_lock` while `hal_ao_write()` splits that block
into ~13 blocking `IMP_AO_SendFrame` calls (`hal_ingenic.c:5288-5303`), so
the lock quantum is up to 512 ms and `fifo_drain(0)` for STOP responsiveness
only runs between blocks (`:476`).

Dropping `SPK_DEC_FR` to 1024 (128 ms) takes BSS to 6 KB, the stack buffer to
2 KB, and the lock quantum to ~128 ms, at the cost of 8 decode iterations per
second instead of 2. **Risk: low** — and to be explicit, this is *not* a
re-litigation of the declined "speaker holds `g_lock` across the blocking AO
write" finding (`PERFORMANCE_REVIEW_2026-08-28.md:209`): the locking
architecture and the close-vs-in-flight-write invariant are untouched, only
the block size changes. Worth confirming no Opus path assumes a minimum
`op_read` request size (`:335` caps at `SPK_DEC_FR`, still valid at 1024).

While in there: `downmix()` (`speaker.c:317-322`, callers `:352`/`:363`)
`memcpy`s every mono block from `g_ilv` to `g_dec` for nothing — the PCM16
path could `fread` straight into `g_dec` and the companded path could decode
straight into it. 8 KB per block at ~2 blocks/s during playback only, i.e.
unmeasurable; listed only because it is the same function.

### AV-06 — the last un-converted P-02 poll loop

```c
srt.c:769:  for (int i = 0; g_run && i < backoff * 10; i++) usleep(100000);
```

Ten `nanosleep` wakeups per second, forever, while an SRT receiver is
unreachable. This is exactly the pattern P-02 replaced with `ms_stopgate` in
`record.c:587` and `timelapse.c:193/212`; `srt.c` was never converted. The
comment's stated reason — wakeability for the join — is precisely what
`ms_stopgate_wait` provides, and better (it wakes instantly rather than
within 100 ms). ~0.01 % of a core; the value is closing the inconsistency,
not the cycles. **Risk: low**, but it touches shutdown sequencing in a file
with careful close-once invariants — needs the `make sim USE_SRT=1` caller
test. `USE_SRT` is off by default fleet-wide.

### AV-07 … AV-12 — the small stuff

- **AV-07** `backchannel.c:53` `g_pcm[8192]` = 16 KB BSS whenever
  `USE_BACKCHANNEL` is built. The reachable maximum is bounded by the RTP
  payload: G.711 is 1 sample/byte so ≤ ~1400 samples at a normal MTU
  (`:258`, `:267`), and the AAC path stops at `cap - 2048` by its own guard
  (`:146`). 4096 is comfortable headroom. **Caveat:** `SPK_RS_MAX`
  (`speaker.c:32`) is explicitly derived from "an 8192-sample backchannel
  block stretched 8 kHz → 48 kHz" — the two must move together, and the
  coupling is comment-only.
- **AV-08** `stream_mjpeg()` still does `fanqueue_closed(&q)` then
  `fanqueue_pop(&q, 500)` (`httpd.c:896-897`) where `stream_mp4()` and
  `rtsp.c`'s `stream_loop()` were converted to a single `fanqueue_pop_ex()`
  in the 2026-08-28 Tier-3 round. At 5 fps the two extra lock cycles per
  frame are nothing; it is a "the conversion missed one" note so the next
  reader doesn't have to re-derive that it was deliberate (it wasn't).
- **AV-09** `sse_emit()` (`httpd.c:1153-1161`) does one `csend` per event and
  the drain loop at `:1342-1346` can pop several per wakeup, so a motion
  burst becomes several small TCP segments (TCP_NODELAY is on). `:1312` and
  `:1315` are two `csend`s at connect where one `csendv` would do. Negligible
  CPU; the cost is WiFi airtime per open WebUI tab. Only worth doing if
  already editing that function.
- **AV-10** `srt.c:376` `memmove(p + o + stuff, p + o, 0)` — folded away by
  GCC, so free, but it sits in the per-TS-packet inner loop and reads like a
  buffer shift that is not happening. Delete.
- **AV-11** `RING_MAX_BYTES` = 4 MB (`record.c:60`). At the default
  `pre_roll_s = 3` (`config.c:387`) the ring pins ~450 KB at fleet bitrate —
  fine. But `pre_roll_s` is accepted up to 60, and at 6 Mbps/25 fps the
  `RING_CAP = 256` packet limit allows ~10 s ≈ 7.5 MB, so the 4 MB byte cap
  binds: 11 % of a 37 MB board, pinned. The warning at `record.c:555-573`
  tells the operator the pre-roll came out shorter than asked, but nothing
  ties the cap to actual board RAM. Flagged as a known ceiling, same spirit
  as `PERF_FOLLOWUPS_2026-09-05.md`'s `FQ_MAX_BYTES` trigger condition.
- **AV-12** `srt.c` never sets `SRTO_RCVBUF`, and these sockets are strictly
  send-only (no `srt_recvmsg` anywhere in the file). libsrt is not vendored
  here, so the default receive-buffer allocation per socket is unknown —
  anywhere from ~32 KB to ~128 KB depending on version, × up to
  `SRT_MAX_CLIENTS = 8`. If SRT is ever turned on, that is a one-line
  `srt_setsockflag` in `srt_common_opts()` plus a before/after `VmRSS` read.
  Not worth guessing at now.

---

## Measured or analysed, and **declined**

These are the ones that looked real and are not. Recorded so the next
reviewer does not re-open them.

### AV-D1 — "the AI capture channel is never stopped when nobody is listening"

**Verdict: real, measured at 0.25 % of a core, declined.**

The asymmetry is genuine and the code says so in its own comment
(`hal_ingenic.c:3806`, comment `:3809-3825`): when `g_aactive` drops to 0 the
audio *thread* parks on `act_wait()`, but nothing touches the AI —
`IMP_AI_Enable`/`EnableChn` stay up, libimp keeps running I²S DMA into the
channel FIFO, and whatever DSP was enabled at boot (`IMP_AI_EnableHpf` /
`EnableAgc` / `EnableNs`, `:3643-3645`) keeps running on every 40 ms frame
with zero consumers. It is also *why* the resume path needs the stale-backlog
flush at `:3826-3842`. The video side solved the equivalent problem with
`fs_use`/`fs_unuse` at a measured ~19 % idle CPU (`:424-429`); audio has no
analogue.

So I measured it rather than reasoning about it: the `ai-_ai_record_t` thread
costs **5 ticks / 20 s = 0.25 % of one core, identically on both a T31 and a
T23**. The fleet default is `audio.ns = 0`, `audio.agc = 0`,
`audio.high_pass = 1` (`config.c:318`), so only the high-pass filter is in
that number.

Against 0.25 %, the change is not worth it. A correct idle-disable has to go
through `g_ai_lock` in the same sequence as `:3739-3772`, must not strand
`g_aec_on` (`hal_ao_open` at `:5263-5275` only enables AEC while `g_ai_up`,
and `hal_ao_close` at `:5313-5324` disables it under that lock), and would
cycle a channel whose bring-up the code already treats as failure-prone —
`:3605-3632` return-checks all three calls precisely because some T-series +
sensor combinations fail one of them, and a *re*-enable failure mid-life is
worse than a boot failure. Plus first-frame latency after a subscriber
arrives grows by the bring-up time.

**Re-open only if** a camera is ever configured with `audio.ns > 0` and/or
`audio.agc` — then re-take the same per-thread sample; if `ai-_ai_record_t`
is still under ~1 % of a core, the answer stays no.

### AV-D2 — "`sw_osd_blit_y` does two integer divisions per pixel"

**Verdict: disproved by codegen, and the path is dead fleet-wide.**

`hal_ingenic.c:2322` and `:2325` both divide by `255u` inside the per-pixel
blit loop, which on MIPS would be ~35 non-pipelined cycles each. It is not:
compiled with the real target toolchain
(`toolchain/xburst1-uclibc/.../mipsel-thingino-linux-uclibc-gcc` 15.3.0,
`-Os`), the loop emits **zero `div`/`divu` instructions** — GCC turns both
constant divisions into a multiply-high plus shift, as it does for any
compile-time constant divisor.

Separately, the SW-rotate path this lives on is inactive: it is gated on
`USE_SW_ROTATE` *and* a configured 90/270 rotation, and neither T23 in the
fleet (Galayou 192.168.10.28, cam-kinder-links 192.168.10.124) sets
`videoN.rotation` — checked live.

Two small nits remain on that path and are worth folding into any future edit
there, but not a commit of their own: the `fx` bounds test sits inside the
inner loop where the row's x-range could be intersected once (`:2318-2320`),
and the alpha scale is computed *before* the `if (!a) continue` so fully
transparent pixels — the majority of a glyph bitmap's bounding box — pay a
multiply and a shift for nothing (`:2322-2323`; `if (!p[3]) continue;` first
is exactly equivalent).

### AV-D3 — "`sw_osd_compose` snapshots a ~550-byte config struct per item per frame"

**Verdict: real, ~0.05 % of a core, same dead path as AV-D2.**

`hal_ingenic.c:2416-2419` takes `config_str_lock()` and copies a whole
`ms_osd_item` (~424 B) plus `osd.vars_file` (128 B) **per frame per item**,
even though on the non-refresh path only `enabled`/`x`/`y`/`transparency`
are used — `imp_osd.c`'s `refresh_text()` does the same snapshot once per
second. Four items at 15 fps is 60 lock pairs and ~33 KB/s of copying. Fold
into AV-D2's edit if that path ever comes back; not worth touching now.

### AV-D4 — raising the packetization size on TCP-interleaved RTSP

**Verdict: considered, rejected on interop grounds.**

Over TCP-interleaved transport an RTP packet does not need to fit a UDP
datagram — the `$` framing header carries a 16-bit length, so up to 65535
bytes is legal. At `rtsp.mtu = 1200` (`config.c:250`) a 200 KB IDR is
fragmented into ~170 FU-A packets, ~11 `net_sendmsg_all` calls and 340 iovec
entries, purely to satisfy a limit that only binds on UDP. Unfragmenting the
TCP sink would take that to one packet, one syscall, two iovecs.

Not recommended: client depacketizers commonly cap the interleaved packet
size they will accept (live555-derived stacks in particular), and the failure
mode is a truncated-payload warning or a silently broken stream on some NVR,
found in the field rather than in test. This daemon's whole posture is
defensive and the win is ~150 avoided packets/s per TCP client on a path
already batched to ~11 syscalls per IDR. If anyone ever wants it, it needs a
real interop matrix (ffmpeg, VLC, live555, go2rtc/Frigate, ONVIF NVR) first,
not a constant change.

### AV-D5 / AV-D6 — two audio items deliberately not pursued

- `g711.c:19`/`:35`'s `for (int m = 0x4000; ...)` exponent search is the
  classic un-optimized form and could be a `__builtin_clz` or a 256-entry
  segment table. At 8000 samples/s it is ~0.01 % of a core. The table costs
  flash; the CLZ costs a diff. Don't.
- `fmp4_audio_fragment()` (`fmp4.c:620`) emits one `moof` per AAC frame:
  ~108 bytes of box overhead per ~250-byte payload, ~1.7 KB/s per client.
  Batching samples into a multi-sample `trun` would recover it at the cost of
  audio latency. No.

---

## Checked and found clean

**Core pipeline.** `fanqueue.c` — one lock per pop via `fanqueue_pop_ex`,
signal gated on "queue was empty", byte budget on `->len`, drop-forward
through a headless GOP done inside the same critical section; nothing left.
`frame.c`/`hub.c` — pooled packets with size-matched dispatch, the `big` slot,
zero-subscriber skip, one caller-supplied `now_us` threaded through publish;
`hub_publish_take()` is allocation-free in steady state. `hub_prepare_locked`
+ `hub_finish_push` are two uncontended mutex acquisitions per frame — ~100 ns,
not worth collapsing.

**Video producers.** `video_thread` (`hal_ingenic.c:1985-2239`) sizes the
pooled borrow from the exact pack lengths, assembles once, and publishes with
no second copy; the AU-overflow and pool-failure drops are cheap and
rate-limited. `au_is_key()` already early-exits on the first VCL NAL and
`vparam_update()` on a complete parameter set, so neither walks a 200 KB IDR
under the source lock. `enc_assemble_packs()` is one `memcpy` per pack.
`jpeg_thread` releases the IMP stream before the snapshot `fwrite`. The
2-second idle-stop debounce keeps assembling AUs at 0 subscribers, which is
~150 KB of memcpy per disconnect event — not a finding.

**NAL / mux.** `nal.c`'s `find_start` is `memchr`-driven, and `nal_iter_next`
scans each NAL body exactly once (the second `find_start` resumes *at* the
start code it already found). `fmp4.c`'s `index_nals()` is shared by both
fragment builders so trun length and mdat payload cannot drift; the iovec
builder writes ~120 bytes and points the rest at the packet.

**RTSP / RTP.** `stream_loop()` reads the clock once per iteration, gates the
control poll to 50 ms and the RTCP liveness drain to 1 Hz, and the zero-copy
flush barrier is structural rather than incidental. `rtp.c` allocates nothing
and copies nothing on any codec path; header blocks are ≤16 B stack buffers
the sink copies. `rtp_maybe_sr`'s deliberate re-read of the monotonic clock
on the SR path only (~1/s/track) is correct and documented. The `rtp_batch`
is ~1.1 KB per video sink for ~170 avoided syscalls per IDR.

**HTTP.** No per-frame allocation anywhere in `httpd.c`. Every plain-transport
per-frame send is a gather write (fMP4 `:745`, MJPEG `:932`, snapshot `:828`);
the only serialization is the TLS branch, which is measured and deliberate.
`stats_collect` does no `statvfs`. `peek_first_byte` costs one `poll` + one
`MSG_PEEK` per connection, not per frame.

**Network.** TCP_NODELAY on every accepted client on both listeners
(`httpd.c:2079`, `rtsp.c:1679` — the RTSP one correctly before the TLS
handshake), SO_RCVTIMEO/SO_SNDTIMEO 30/15 s, SO_REUSEADDR on listeners,
`accept4(SOCK_CLOEXEC)` with a correct fallback. **`SO_SNDBUF` deliberately
unset is right** — setting it disables Linux TCP send-buffer autotuning, and
for a handful of LAN viewers with wildly different drain rates a hand-picked
value would either throttle a fast client or pin more kernel memory than the
slow ones need. Same for the UDP RTP pair: a blocking `sendto` into a full
sndbuf self-paces an IDR burst instead of dropping it.

**SRT.** `TS_BATCH_PKTS = 7` → 1316 B **is** `SRTO_PAYLOADSIZE` in live mode,
so the batching is already at libsrt's hard ceiling; the per-AU `ts_flush()`
adds ~45 partial datagrams/s worst case, which is the right trade for
bounding added latency to one AU. One clock read per received packet, taken
after the pop. Per-client TS re-mux is inherent (independent CC counters).

**TLS.** The handshake loop is genuinely poll-driven, not a spin
(`tls.c:262-301`). `ms_tls_write`'s bare `continue` on WANT_WRITE is safe
despite its own comment's worry: mbedTLS's `net_would_block()` returns 0 when
the fd is not `O_NONBLOCK`, so an SO_SNDTIMEO expiry surfaces as
`MBEDTLS_ERR_NET_SEND_FAILED` and the write returns −1. Session tickets are
set up once per context. Nothing per-frame at all.

**Events / timelapse.** `events.c` is fully condvar-driven; `broadcast` with
zero waiters does not touch the futex, so the producer path is a mutex pair
plus a 112 B copy per *changed* motion grid. `timelapse.c` is clean —
just-in-time grab, hourly prune gate, one large `fwrite` per shot, stopgate
throughout.

**Audio (beyond the findings).** `codec/resample.c` is Q32 fixed-point, one
32×32→64 multiply per output sample, no divides, no allocation.
`codec/aac.c`'s rate-index scan runs once per session. `rtsp/talk_ws.c` and
`backchannel.c` are as cheap as their contracts allow. The audio thread's hot
loop is 3 ioctls per frame; the AAC re-block accumulator does one ~1.3 KB
`memcpy` plus one ≤2 KB `memmove` per frame (~50 KB/s — not worth a ring
rewrite); the `usleep(15000 - a_dt)` pacing never fires because
`PollingFrame` blocks ~40 ms.

**Build.** `USE_TRACE` is 0 in every real build (`Makefile:74`,
`timps.mk:213`), so every `ms_trace_*` hook compiles to nothing — the send
paths carry no instrumentation cost in production.

**Auth.** Three MD5s over short strings per digest check (`auth.c:103-105`,
`:157-163`). Not a hot path under the SSE-push WebUI.

---

## Already settled elsewhere — checked, not re-raised

- Shared NAL-boundary table so RTSP/fMP4/record stop re-scanning each AU —
  **declined** with reasons, `PERFORMANCE_REVIEW_2026-08-28.md` Tier 1 #2.
- `msttf` glyph cache — **declined** (16–21 MB worst case); the rasterizer
  inner-loop fix it recommended instead **has since landed** (`ee53b5f`).
- `speaker.c` holding `g_lock` across the blocking AO write — **declined**,
  Tier 2 #10. AV-05 deliberately does not touch it.
- WebSocket talk-path syscalls per frame — **measured at 0.04 % of a core and
  declined**, `PERF_FOLLOWUPS_2026-09-05.md` #5 (`ws.c` is vendored).
- `IMP_Encoder_GetChnAveBitrate` per frame — **disassembled**, cheap, and
  throttling it would produce a wrong number, #6.
- Three clock reads per audio frame — **implemented, measured, reverted**, #7.
- `FQ_MAX_BYTES` / client-cap worst case — **measured**, no action at fleet
  settings; the trigger condition is recorded in #4.
- MJPEG queue depth 8 → 2 — **implemented** (`45aec8a`).
- fMP4 gather-write / 410 kB per client — **implemented** (`db64973`); AV-04
  is the one consumer it explicitly left behind.
- Frame-pool `big` slot / R-01 — **implemented** (`3cade7d`).
- HTTP keep-alive, TLS session cache, config-persist debounce — all
  **assessed and declined** in the 2026-08-28 round.
- WebUI preview latency — client-side policy, covered by
  `PREVIEW_REALTIME_IDEAS_2026-08-29.md`; no daemon work.
- Classic-API (T10–T30) JPEG encoder probably encoding every video frame —
  already recorded as a probable-but-unverifiable finding in
  `hal_ingenic.c:3354-3368`.

---

## Recommended order

1. **AV-01's measurement on Garage** (~10 min once that camera is back up).
   It is the only thing in this document that could be worth several percent
   of a core fleet-wide, and it is currently an open attribution question,
   not a change.
2. **AV-02** — `seg_open()` backoff + the vparam-readiness hoist. Small,
   low-risk, and the only item whose failure mode reaches flash and the
   shared log collector.
3. **AV-03** — audio pool caps. Trivial, free, and it retires a dead
   variable.
4. **AV-05 + AV-07** — the two BSS reductions, together, since `SPK_RS_MAX`
   couples them.
5. **AV-06, AV-08, AV-09, AV-10** — batch into whatever next touches those
   files. None of them justifies its own commit.
6. **AV-04** — when the recorder is next opened for another reason, or if a
   camera is ever configured above ~3 Mbps with recording on.

Nothing here is urgent. The honest summary is that the A/V and network code
is finished as a target for micro-optimization: the daemon's own threads
account for under 7 % of a core on a T31 under real load, and the next
meaningful win — if there is one — is in how timps *configures* the vendor
pipeline, not in what its own C does per frame.
