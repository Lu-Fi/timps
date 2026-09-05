# fMP4 HTTP streaming — removing the last full-AU copy — 2026-09-05

Follow-up to `PERFORMANCE_REVIEW_2026-08-28.md` Tier 2 #8. That round removed
the *second* of two per-frame copies in the fMP4 mux; this one removes the
first, which the code had until now described as unavoidable. It is not, and
the reason it looked that way is a timing accident in the last review.

**Scope:** plain-HTTP fMP4 clients (`/stream.mp4`). HTTPS clients, audio
fragments and the SD recorder are deliberately untouched — see *What is not
changed* below.

---

## Was this already considered and rejected?

Half of it. The distinction matters, so it is written out rather than
summarised.

`httpd.c`'s per-connection-buffer comment used to call the remaining copy
"already unavoidable", which reads like a recorded decision. It is not one —
it is a parenthetical inside the M1 comment about *malloc churn*, describing
the status quo rather than a conclusion reached about it.

What was genuinely considered and correctly rejected is recorded in commit
`4741bd6`: muxing the AU into a scratch buffer twice, and the idea of fixing
it with a read-only **length pre-pass**. That was rejected on solid grounds —
`nal_iter`/`find_start` is a byte-at-a-time start-code scan, so re-walking a
200 KB IDR costs more than the memcpy it would save. The fix that landed
instead (index the NALs during the one walk that has to happen anyway) is
what makes *this* change possible, because the NAL index is exactly the set of
pointers a gather-write needs.

The reason the iovec route was not taken then is chronology, not analysis:
`csendv()` — the gather-write helper — was added in the **same review batch**,
as item #12, for MJPEG. When `fmp4.c` was rewritten there was no gather-write
helper in `httpd.c` to target. Nothing in `dev_notes/`, the commit history or
the code comments weighs the iovec approach and turns it down.

So: not previously rejected, and the earlier reasoning does not carry over.

---

## What changed

**`src/mp4/fmp4.c` / `fmp4.h`**

- The NAL-indexing walk is factored out of `fmp4_video_fragment()` into a
  shared `index_nals()`. Both fragment builders call it, so they cannot drift
  apart on which NALs are skipped or how long the sample is — a trun length
  that disagrees with the mdat payload is unrecoverable for the client, and
  that invariant is now structural rather than duplicated.
- New `fmp4_video_fragment_iov()`: writes **only** moof + the 8-byte mdat
  header into a caller-supplied `ms_buf` (~120 bytes) and fills a
  `fmp4_frag_iov` whose iovecs point straight at the caller's access unit —
  `{head, len0, nal0, len1, nal1, …}`, at most 65 entries.
- `FMP4_NAL_IDX` moved to the header (the iovec array is sized from it), plus
  `FMP4_IOV_MAX = 1 + 2*FMP4_NAL_IDX` = 65, far below `IOV_MAX` (1024), so one
  `sendmsg()` always covers a whole fragment.

The mdat box size is the one thing that needs care. On the contiguous path
`box_close()` patches it to `buf->len - mdat_pos` *after* the body is
appended. Here the body never enters the buffer, so the size is written
directly as `8 + slen` — which is by construction the value `box_close()`
would have produced. The `!head->data || head->err` guard before that write is
the same one `box_close()` applies, and for the same reason: a failed grow
anywhere inside `fragment_head()` leaves the mdat offset no longer reliably
inside the buffer.

**`src/mp4/httpd.c`** — `stream_mp4()` calls the iovec variant for video on
plain connections and sends with the existing `csendv()`. The per-connection
`ms_buf`'s reset soft cap drops from 256 KB to 4 KB on that path.

---

## Why it is safe: the lifetime contract

This is the whole argument, so it is stated plainly.

The iovecs alias `p->data` — the packet the loop popped off the fanqueue. That
is sound because the consumer holds its reference across the send:
`csendv()` at the top of the frame body, `pkt_unref(p)` at the bottom, with
nothing in between that can recycle the buffer.

This is not a new pattern. `src/rtsp/rtsp.c` has depended on exactly it since
the RTP sinks went zero-copy — its "ZERO-COPY FLUSH BARRIER" comment states
the same invariant for the same reason, and `rtp.h` writes it into the
`rtp_out_fn` contract. The fMP4 loop has the same shape; the contract is now
documented on `fmp4_video_fragment_iov()` in `fmp4.h` and referenced at the
call site so the ordering cannot be quietly broken later.

---

## What is not changed, and why

- **HTTPS clients.** `csendv()` has no scatter/gather write over TLS; it
  serialises per iovec. A 5-NAL AU would become 11 separate `ms_tls_write()`
  calls — 11 TLS records at ~29 bytes of framing each, and 11 syscalls —
  strictly worse than one copy and one record. TLS connections take the
  contiguous path unchanged (including the 256 KB soft cap), which is the same
  call they made before. `sink_send()` in `rtsp.c` splits on TLS for exactly
  this reason.
- **Audio fragments.** An AAC frame is a few hundred bytes. There is no copy
  worth avoiding and no buffer growth worth preventing.
- **`record.c`.** The recorder writes fragments to a file, where contiguous is
  the right shape. Untouched.
- **AUs with more than `FMP4_NAL_IDX` (32) NALs.** Handled inside
  `fmp4_video_fragment_iov()`: it calls the contiguous builder and returns the
  result as a single iovec, so `stream_mp4()` keeps one send path. The
  timeline advances exactly once (the overflow is detected during the index
  walk, before any mux state is touched). `head` grows to AU size for that
  frame and the reset soft cap hands it back afterwards.

---

## Measured

All figures from the host sim (`make sim`) on x86, comparing a baseline build
at `462cd49` against this change, same binary flags, same source clip.

**Memory — 8 concurrent fMP4 clients, 1080p, ~400 KB fragments** (chosen so a
fragment lands between the 256 KB soft cap and the sim's 512 KB AU ceiling,
i.e. the case where `ms_buf_reserve`'s power-of-two growth pins 512 KB):

| | VmRSS idle | VmRSS, 8 clients | per client |
|---|---|---|---|
| baseline | 251920 kB | 256288 kB | **546 kB** |
| this change | 251912 kB | 253000 kB | **136 kB** |

**~410 kB saved per streaming client**, 3.2 MB across 8. The 136 kB that
remains is the client's fanqueue and packet references, not the fragment
buffer. At `HTTP_MAX_CLIENTS` = 16 (raised in `462cd49`) that is ~6.6 MB of
headroom recovered on a daemon whose loaded RSS is around 5 MB — on Schuppen,
a camera with **37 MB of RAM in total**.

The pre-existing behaviour this fixes: `ms_buf_reserve` grows by powers of two,
so a 300 KB IDR fragment allocates 512 KB (a 1440p one, 1 MB), and
`ms_buf_reset`'s shrink needs `MS_BUF_SHRINK_RUN` (64) consecutive sub-cap
resets — with a 50-frame GOP every IDR restarted that run, so the buffer never
shrank for the life of the connection.

A lower-bitrate run (~130 KB fragments, same 8 clients) saved ~136 kB/client:
the effect scales with fragment size, as expected.

**CPU — same load, 15 s window, `/proc/<pid>/stat` utime+stime delta, 3 runs
each:**

| | run 1 | run 2 | run 3 |
|---|---|---|---|
| baseline | 1650 ms | 1640 ms | 1660 ms |
| this change | 1460 ms | 1450 ms | 1400 ms |

**~12% less daemon CPU** under 8-client 1080p fMP4 load, tightly reproducible.
On MIPS the saving should be at least this large in relative terms — memory
bandwidth is the scarcer resource there than on x86.

**Binary size (T31 cross-build, `./build.sh timps T31`):**

| | .text | .data | stripped file |
|---|---|---|---|
| baseline | 995743 | 8708 | 1054144 |
| this change | 996619 | 8712 | 1054880 |

**+876 B `.text`, +736 B on flash.** Paid once; the memory it buys back is per
client.

---

## What was tested, and how

**1. Byte-identity unit test — `make test-fmp4` (new, `scripts/test_fmp4.c`).**
This is an ISO BMFF wire format, so the check that matters is byte-for-byte
equality against the mux already in the field, not "a player accepts it". The
harness runs a *sequence* of AUs through two independently initialised muxers
(both builders are stateful — mfhd sequence, per-track tfdt accumulators, the
shared A/V zero point) and compares the concatenated output **and** the
resulting mux state, so a divergence that only appears on the second fragment
cannot pass. It also walks the top-level boxes independently, checking the
size chain closes exactly and that every mdat is `8 +` its trun sample size.

11 cases, all passing: a 13-frame H.264 GOP with jittered PTS; single IDR with
unknown PTS; a parameter-set-only AU mid-stream (must emit nothing and not
advance the timeline); 3-byte start codes; a 300 KB IDR; 1-byte NAL bodies;
exactly `FMP4_NAL_IDX` NALs; NAL-index overflow **followed by normal frames**
(proving the fallback leaves the timeline where the reference leaves it); a
24 s PTS gap tripping the M6 re-anchor; `fps` unset (nominal-duration branch);
and an H.265 GOP, whose skip set (VPS/SPS/PPS/AUD = 32/33/34/35) differs.

**2. The test was checked against deliberate bugs, not just run.** Three
mutants, each caught:

| mutant | result |
|---|---|
| mdat size `8+slen` → `9+slen` | 0/11 pass, points at the exact byte |
| loop bound `i < nn` → `i < nn-1` (drop last NAL's iovecs) | 0/11 pass |
| `pts_us` → `pts_us+1` in the overflow fallback only | 10/11 pass — only the overflow case fails |

The third matters most: it confirms the overflow case is genuinely exercised
rather than incidentally passing.

**3. End-to-end over a real socket, host sim.** Baseline and new builds fed the
same H.264 clip, `/stream.mp4` fetched over HTTP with curl:

- New output decodes clean under `ffmpeg -err_detect explode -xerror`
  (249 frames, no errors).
- Extracting every mdat payload from both captures: **byte-identical**,
  1023972 bytes, same MD5. Decoded pixel MD5 also identical.
- The moof boxes differ between the two — and *also between two runs of the
  baseline against itself*, which was checked explicitly. That difference is
  real-time capture jitter in tfdt/duration, inherent to the sim, not caused by
  this change.

**4. The overflow path, live.** The sim's AU assembler splits on every VCL NAL,
so it cannot feed a genuine multi-slice AU (a 45-slice clip was generated and
confirmed not to reach the mux as one AU — the baseline behaves identically, so
this is a harness limitation, not a regression). Instead `FMP4_NAL_IDX` was
forced to 1 and the sim re-run, which puts the IDR AUs (SEI + IDR = 2 kept
NALs) on the fallback while P frames stay on the gather-write path — the same
technique commit `4741bd6` used. Output decodes clean under `-xerror` and is
pixel-identical to both the normal run and the baseline.

**5. Builds.** Host sim clean, no new warnings. T31 cross-build via
`./build.sh timps T31` clean. `httpd.c` compile-checked with `-DUSE_TLS` (the
fleet's cameras build with `BR2_PACKAGE_TIMPS_TLS=y`, so the TLS branch is
compiled in even where `http.https` is off).

**6. Real hardware — Schuppen (192.168.10.25, T31L / SC2336 / ATBM6031).**

Built through Buildroot with the camera's own options (`rebuild-timps` against
the existing `output/ciao/…-192.168.10.25` dir, so the binary carries exactly
this camera's flags — including `BR2_PACKAGE_TIMPS_TLS=y`, while `http.https`
is off, which is precisely the configuration the change targets).

*Method — no flash write, no reboot.* The binary was copied to `/tmp`
(tmpfs), `S95timps` stopped, the new binary run from tmpfs, and the flashed
service restarted from a shell `trap` so it came back whatever the test did.
Total swap window ~50 s. Verified afterwards: the flashed `/usr/bin/timpsd`
v1.9.8 is running again, `/stream.mp4` and `/snapshot.jpg` both 200, RTSP
(554) and HTTP (8880) listening, motion and day/night active (an
`image.running_mode` switch was logged during the check), `/tmp` artifacts
removed, nothing in the log.

*Correctness.* A 1.1 MB capture of **real Ingenic T31 encoder output** taken
through the gather-write path: 1920x1080 H.264, 89 video frames, decodes clean
under `ffmpeg -err_detect explode -xerror`, and the box chain closes exactly at
EOF. 181 moof/mdat pairs for 89 video frames — the other 92 are audio, so this
also confirms the gather-write video path and the untouched contiguous audio
path interleave correctly on one connection, which no host test covered. The
new binary logged no `[WRN]` or `[ERR]` of its own.

*Memory,* 4 concurrent fMP4 clients on this camera's stream (1080p but only
1200 kbps @ 15 fps, so fragments are ~10 KB — nowhere near the 512 KB case):

| | idle | 4 clients | per client |
|---|---|---|---|
| deployed v1.9.8 | 4564 kB | 5640 kB | **269 kB** |
| this change | 3940 kB | 4648 kB | **177 kB** |

~92 kB per client, which is the right order for fragments this small. The idle
figures are **not** comparable between the two rows (the deployed daemon had
1 d 4 h of uptime against a fresh start), so only the per-client delta is
meaningful here.

*CPU* was a wash: 296 vs 305 ticks over 15 s. Expected — at 1.2 Mbit the copy
is ~150 KB/s per client, which is nothing on a T31, and this is an outdoor
camera whose scene content (and therefore bitrate) differs between two runs
minutes apart. The sim's 12% figure was 8 clients at roughly 8 Mbit; the CPU
half of this change only pays off at high bitrate, and nothing here contradicts
that.

**Build-dir gotcha worth remembering:** `make rebuild-timps` derives its
output dir from `git rev-parse --abbrev-ref HEAD`, which returns the literal
string `HEAD` when the firmware checkout is in detached HEAD — so it starts a
**new from-scratch build** under `output/HEAD/…` instead of reusing
`output/ciao/…`. Pass `THINGINO_OUTPUT_DIR=<existing dir>` to target the real
one. (A partial `output/HEAD/cinnado_d1_t31l_sc2336_atbm6031-…-192.168.10.25`
tree, ~3.4 GB, was created and abandoned this way before the mistake was
caught; it is safe to delete.)

---

## Residual risk and follow-up

- **No long-duration multi-client soak on real hardware yet.** The Schuppen
  run above is a ~50 s functional check on a production camera, not a soak, and
  its bitrate is too low to exercise the large-fragment case the change is
  really aimed at. Garage (192.168.10.21) came free during this work but is
  currently flashed with the raptor+WebRTC build, so using it would mean a full
  reflash back to timps first — out of scope here. `timps-qa.sh --profile soak`
  on Garage with several concurrent fMP4 clients at a realistic bitrate is the
  natural next step, and the thing that would turn the sim's CPU/RSS numbers
  into fleet numbers.
- **The 512 KB/1 MB buffer case has not been observed on real hardware**, only
  reasoned from `ms_buf_reserve`'s growth rule and reproduced in the sim. It
  needs a camera running a high enough bitrate that IDR fragments clear 256 KB
  — the 1440p cameras, not Schuppen's 1.2 Mbit stream.
- **The TLS path keeps the old cost.** Cameras that actually turn on
  `http.https` see no benefit — neither the copy nor the 512 KB buffer. If
  HTTPS fMP4 becomes common, the fix is batching the iovecs into one TLS record
  rather than serialising them, which is a change to `csendv()`, not here.
- **`FMP4_NAL_IDX` is a compile-time constant with no runtime signal.** An AU
  past 32 NALs silently takes the slower path. That was already true before
  this change; worth a `LOGD` if multi-slice encoders ever show up in the
  fleet.
- The 45-slice sim limitation above is worth remembering — the sim cannot
  exercise multi-slice AUs at all, for any test.
