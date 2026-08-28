# timps performance review — 2026-08-28

8 parallel Fable reviews across the full streamer (~28k lines), each scoped to
a coherent subsystem, focused on efficiency/performance for embedded MIPS
(Ingenic T-series, single/dual-core, memory- and flash-constrained, 24/7
uptime). Every finding below was verified by the reviewing agent reading the
actual code paths, not inferred from comments. Correctness bugs spotted in
passing are listed separately — this was a performance pass, not a
security/correctness audit.

**This document has been updated after the fix rounds** to record what
actually happened to every finding: implemented, declined, or assessed-only,
each with the real reasoning. Status markers: ✅ **Implemented** · ❌
**Declined** (tried/considered, not done, with why) · ⏸ **Assessed only**
(investigated, no code change, with why) · ⬜ **Not actioned** (found, never
picked up).

**Overall verdict, then and now:** the architecture was genuinely well-built
going in — zero-copy hub fan-out, condvar-blocking waits everywhere, single-
pass JSON building, pooled packet buffers, change-detection before flash
writes. Every item below was a refinement on a solid base, not a structural
problem, and every implemented fix was independently build-verified (host
sim + cross-compiles across the fleet's SoC families where relevant) before
landing.

## Status at a glance

| Tier | Items | Implemented | Declined | Assessed only |
|---|---|---|---|---|
| Tier 1 | 5 | 2 | 0 | 3 (one of which spawned a separate implementation, see below) |
| Tier 2 | 8 | 7 | 1 | 0 |
| Tier 3 | 13 | 13 | 0 | 0 |
| Side findings | 2 | 1 (TLS tickets) | 0 | 1 (keep-alive) |
| Correctness bugs | 7 | 0 | 0 | 0 (not actioned - out of scope for the perf-fix rounds) |

23 of 28 performance findings implemented and build-verified; 1 declined with
a concrete architectural reason; 4 deliberately left as assessed-only
(2 genuinely not worth the risk/complexity as specified, 1 resolved by
implementing a different, safer thing instead, 1 pending a data point only
the user can supply).

---

## Tier 1 — do these first (fleet-wide, continuous, or worsening over time)

### 1. `record.c`/`timelapse.c`: full directory-tree walk on every prune, gets worse every day

✅ **Implemented.** `find_oldest()` → `collect_oldest()`, gathers the 32
oldest files into a bounded array in one walk instead of one walk per
deleted file; `prune_free()` unlinks from that list, rechecking `statvfs`
between unlinks. `timelapse.c`'s `prune()` rate-limited to hourly via a
monotonic guard. Both build-verified, own differential test harness (200
seeds × 30 sizes vs. a brute-force `qsort`, plus a real day/hour tree and
edge cases). Deliberately **not** keyed on the day/hour directory buckets
`record.name` produces by default, since that pattern is runtime-mutable
via `/control` and a layout-agnostic mtime scan stays correct for any
naming scheme. Flagged, not fixed: `record.c` never `rmdir`s emptied
directories (bounded by uptime-hours, not a return of the quadratic
behavior, but a slow accumulation). Commits `5f17b53`, `7cd31b7`.

### 2. Redundant NAL start-code scanning — the same bytes get byte-scanned 3-5 times

✅ **Implemented — but not the suggested SDK-field fix.** `au_is_key()`
rewritten to decide from the **first VCL NAL** (an AU carries one coded
picture, so all its slices agree on IDR-ness) instead of scanning to the
AU's end; `vparam_update()` gets the matching early-exit once SPS/PPS/VPS
are captured. The originally-suggested fix — reading keyframe status from
`IMPEncoderPack.nalType`/`.dataType` instead of scanning at all — was
investigated and **rejected**: that field describes a libimp bitstream
*section*, not necessarily one NAL (a single pack can hold SPS+PPS+IDR
together), so trusting it would trade a verifiable byte test for an
unverified per-SoC/per-libimp-version assumption, and the SW-rotate path
(`IMP_Encoder_YuvEncode`) has no pack array at all to read it from. Once
the byte-scan itself only touches a few dozen bytes, the SDK-field route
had nothing left to win. Verified with real MIPS cross-compiles on **all 9
supported SoC families**, a 200k-iteration differential fuzz test, and an
end-to-end sim run with real ffmpeg streams. `+160 B .text` on T31.
Commit `2045e44`.

The larger version of this fix (a shared NAL-boundary table cached in
`ms_pkt` so RTSP/mp4 subscribers stop re-scanning too) was considered and
**declined** as its own decision: it would add per-frame state every
producer must fill and every consumer must trust correctly, with an
overflow fallback that would rarely execute and stay under-tested — real
regression risk on a currently-working hot path for a saving that's a
linear sweep the CPU already touches while copying. Left as a documented
follow-up, not attempted.

### 3. `msttf.c`: OSD font rasterization redone from scratch every second, forever

⏸ **Assessed only — recommended against implementing the cache as
specified, two cheap alternatives identified, neither implemented yet.**

A dedicated cost/benefit pass (with special attention to memory, per
request) found:
- Actual cost is small: 1.2-2.4% of one core in the default config, ≤1.9%
  measured as an upper bound from real T31 QA data — a 1 Hz background
  thread already gated off entirely when nobody is watching.
- The proposed cache's **worst case is 16-21 MB** — reachable purely
  through in-clamp config values (16 OSD items, full ASCII, `font_size`
  up to the old 256px ceiling) — 3-4× the daemon's entire RSS. Making it
  safe would need a byte cap + per-entry size ceiling + LRU + four
  invalidation triggers (font/hinting/supersample/size), i.e. real new
  mutable shared state in a file whose history is a catalogue of past
  OOB/UAF bugs, for ~1% of a core.
- **Two zero-risk alternatives get most of the same win and were NOT
  implemented, still open:**
  - a rasterizer inner-loop fix (an integer modulo → branch in the
    scanline fill, plus size-adaptive bezier stepping) — **33% CPU
    measured**, zero new state, bit-exact output for part of it.
  - fixing `timps.conf.example`'s own OSD template, which bundles
    `{hostname}` into the live-clock item and forces it to re-rasterize
    every second for nothing — **35% saving on that item**, one config
    edit. (This specific bundling was independently fixed anyway, see
    Tier-1 #3's own resolution below — the *general* pattern/lesson
    remains unactioned as a rasterizer-level fix.)
- **Real finding along the way, not part of the original question:** a
  255-char OSD string at the old `font_size=256` ceiling could transiently
  allocate a **~14.1 MB** rasterizer canvas in one `msttf_render()` call
  before being discarded for exceeding the frame — bigger and more
  concrete than anything in the cache debate. **This got fixed as a
  side-effect of item #3's config-sync work below** (clamp lowered to
  128, quartering the worst case), not as its own ticket.

### 3b. (not a numbered finding, discovered while investigating #3) `timps.conf.example`'s OSD layout had drifted from the real compiled default

✅ **Implemented.** The example config bundled `{hostname}` into the same
item as the live clock (forcing an unchanging value to re-rasterize every
second alongside it) and had no `{uptime}`/logo entries — but
`config_defaults()` in `config.c` had already set the intended time /
hostname / uptime / logo 4-item layout as the real compiled default since
2026-07-11; the example file had just drifted from it. Re-derived the
example's `osd0.*`/`osd1.0.*` from that function so both agree exactly.
Also lowered the `font_size` clamp 256→128 as part of the same change
(see the 14.1 MB finding above). Commit `b159195`.

### 4. Config persist: full file rewrite + 2 fsyncs per `/control` POST

⏸ **Assessed only — recommended against, does not manifest in practice.**

A dedicated review classified every `/control`-writing call site across
the entire WebUI (`timps-api.js` plus every page that calls `.set()`/
`.setDebounced()`) against its triggering DOM event. Finding: **every
range/slider input in this WebUI is bound `input`→label-only,
`change`→POST** — a deliberate, consistent, but previously undocumented
convention that already prevents the burst case this finding worried
about. A drag produces exactly one POST, at release. The only unverified
gap: three `<input type=color>` color pickers, which in some browsers
(Chromium) may fire `change` continuously mid-drag — this was flagged as
needing a 30-second DevTools check the assessment agent didn't have
browser access to perform. **Not implemented; the one open item is a
possible narrow client-side fix (deepen `setDebounced`'s merge to 3
levels, or a local debounce timer on those 3 call sites) contingent on
that unverified browser check**, not the server-side persist-debounce
the original finding proposed (which was assessed as disproportionate
regardless — see the full reasoning in conversation history if reviving
this).

### 5. No TLS session cache — every HTTPS poll pays a full handshake

⏸ **Assessed, cache explicitly rejected** → ✅ **session tickets
implemented instead, as a deliberate follow-up request.**

The original finding conflated "cache and/or tickets." A dedicated
security-focused assessment found TLS is compiled into 100% of the fleet
but **enabled on 0% of cameras today** (all `http.https`/`rtsp.tls`
overlays shipped commented-out; the one historical exception was RTSPS on
the disposable Garage test camera for ~90 minutes, since reverted) — so
this was a theoretical finding, not a live one. It also found the
**session cache is dead code under TLS 1.3** (mbedTLS 3.6.6's cache path
is only read by the TLS-1.2 server branch; this build negotiates up to
1.3, which is what a browser — the actual WebUI use case — would reach),
so "cache and/or tickets" was never really an either/or; only tickets
matter here. Verdict at the time: **don't ship now** (optimizing a 0%-used
path in a security-sensitive file is a bad risk/reward trade), but with a
full, specific "if you ever do" spec: tickets only, real `lifetime` in the
12-24h range (never 0 — a 0 lifetime never rotates the key, so a single
compromise would retroactively decrypt every resumed session for the
device's entire uptime), AES-256-GCM, proper free path.

**The user then explicitly asked for exactly that spec to be implemented
preemptively**, which happened: `mbedtls_ssl_ticket_setup()` with a 12h
(43200s) lifetime, AES-256-GCM, wired via
`mbedtls_ssl_conf_session_tickets_cb()` — **not**
`mbedtls_ssl_conf_session_tickets()`, which the implementing agent found
is actually client-only/TLS-1.2-only per the mbedTLS 3.6.6 source (neither
server branch reads it) and would have been a silent no-op; caught by
reading the real library source before assuming the API name from the
brief. Separate ticket contexts for HTTPS and RTSPS (fell out for free —
they already had separate `ms_tls_ctx`s). Forward-secrecy trade-off
documented in a code comment and in `timps.conf.example`. Cross-compiled
and **linked** against a real T31 sysroot with mbedTLS 3.6.6; the
flash-trimmed (guard-off) build path verified byte-identical in object
size to the pre-change baseline. `+52 lines` in `tls.c` (+208 B `.text`).
Not committed.

---

## Tier 2 — real, narrower scope

All 8 triaged by one agent in one pass; 7 implemented, 1 declined.

| # | Where | Finding | Status |
|---|---|---|---|
| 6 | rtsp.c | TCP-interleaved RTSP sends one `sendmsg()` per RTP packet | ✅ **Implemented.** `rtp_batch` extended to serve TCP too (RTSPS excluded on purpose — no scatter/gather under TLS). Measured: 3498→346 `sendmsg` calls per 5s 1080p/6Mbit session. |
| 7 | rtsp.c | RTCP liveness drain runs every frame instead of throttled | ✅ **Implemented.** Gated to 1 Hz, matching an existing throttle pattern in the same file. Measured: `recvfrom` 511→137 per 6s UDP session. |
| 8 | fmp4.c | fMP4 mux copies every AU twice per client per frame | ✅ **Implemented — not via the suggested length pre-pass.** A read-only pre-scan for the length would have cost *more* than the memcpy it saved (the NAL scanner is byte-at-a-time). Instead the existing single Annex-B walk now indexes the kept NALs as it goes: one scan, one copy, not a second scan. Byte-identical output verified against the old mux on 3 sources plus a forced-fallback path. |
| 9 | srt.c | SRT payload copied byte-at-a-time, twice | ✅ **Implemented, both copies removed.** `memcpy`/`memset` swap-in plus building each TS packet directly into the send batch. Bit-identical across 16 payload sizes, both codecs, plus a live ffmpeg-over-SRT run. |
| 10 | speaker.c | Backchannel audio blocks on hardware while holding the global speaker lock | ❌ **Declined, real architectural reason.** The shared resample scratch buffer and the AO device's own close-path both depend on `g_lock` staying held across the blocking write — narrowing the lock opens a close-vs-in-flight-write race against real hardware. A correct fix needs a second lock with a new ordering invariant across three call sites, or a ring+feeder-thread redesign that reworks the "AO backpressure is the playback clock" design on purpose — assessed as genuinely beyond a batch-fix scope, for a backchannel-only (not always-on) code path. |
| 11 | hal_ingenic.c | `jpeg_thread` holds the IMP stream buffer across a blocking SD-card snapshot write | ✅ **Implemented.** `IMP_Encoder_ReleaseStream` moved to immediately after the packet is copied out, before the file write. Cross-compiled clean on all 8 real SoC variants. |
| 12 | httpd.c | MJPEG/short responses send 3 separate small writes instead of 1 | ✅ **Implemented, all 3 sites** (MJPEG, `http_send_ex`, `snapshot_jpg`) via a new `csendv()` gather-write helper. 116→33 syscalls per 6s MJPEG session; wire output byte-identical. |
| 13 | control.c | `caps.play.sounds` walks the sounds directory on every `GET /control` | ✅ **Implemented.** Cached, invalidated on the directory's own mtime. Compiled and warning-checked explicitly with `-DUSE_PLAY` since it's not part of the sim build. |

---

## Tier 3 — cheap, worth batching into other work in the same files

All 13 implemented by one agent in one pass — the most thorough
verification pass of the whole review (equivalence harnesses for every
item where "same output" was the bar, cross-compiles across all 9 SoC
families, a 400-day × 6-location differential test for the sun-times
cache).

- **3 timestamp syscalls per published frame** (hal_ingenic.c, hub.c) —
  ✅ **Implemented.** One `now_us` threaded through `hub_publish()`/
  `hub_publish_take()` instead of re-deriving it 3× (once under the
  source lock). All 9 producer call sites updated, video and audio.
- **Same pattern in SRT** — ✅ **Implemented.** One `now` per received
  packet instead of up to 3.
- **`pkt_pool_get()`'s realloc on pool-cap overflow** — ✅ **Implemented.**
  Confirmed nothing relies on in-place growth; swapped for `free`+`malloc`.
- **Audio publish still malloc+memcpy, not pool-backed** — ✅
  **Implemented.** Clean redirect into the existing pool machinery;
  also retires ~12 KB of `__thread` scratch buffers. One noted new-but-
  equivalent failure mode: an OOM during publish now drops a 1024-sample
  block via a slightly different path than before, same net effect (no
  publish).
- **`ms_buf_reset`'s 256 KB soft-shrink thrashes every GOP** — ✅
  **Implemented — not either originally-proposed option.** Raising the
  cap enough to matter would retain ~1 MB × 8 clients; a naive hysteresis
  was defeated by the buffer's own power-of-two growth. Landed a real
  64-reset hysteresis counter instead. Also fixed an identical,
  previously-unlisted 256 KB call in `httpd.c`.
- **3-4 separate fanqueue lock/unlock cycles per popped frame** — ✅
  **Implemented, both rtsp.c and httpd.c.** New `fanqueue_pop_ex()`
  returns closed/dropped/depth state from inside the pop's own critical
  section. One deliberate exclusion: `dropped_audio` stayed separate,
  because folding it in would silently re-break a real prior fix (the
  2026-08-22 mute-vs-eviction distinction on cam-garage).
- **`fanqueue_push` signals the condvar even with no possible waiter** —
  ✅ **Implemented.** Gated on "queue was empty before this push",
  relying on the documented single-consumer contract.
- **Backchannel resample does a float divide per sample** — ✅
  **Implemented, at higher precision than proposed.** Q16 (as suggested)
  turned out inaccurate enough to drift audibly over a buffer; landed Q32
  instead, verified against a double-precision reference (max deviation
  2 LSB, i.e. rounding noise, not error) across all 4 sample rates both
  directions — with a differential test harness this repo didn't
  previously have.
- **T23 SW-rotate inner loop, index math per byte** — ✅ **Implemented.**
  Collapsed to pointer-stepping; byte-for-byte verified against the old
  output across both rotation directions × 12 geometries.
- **OSD template expansion runs strftime even with no `%` in it** — ✅
  **Implemented**, confirmed identical on the truncation/empty-string edge
  cases too.
- **daynight.c's 2s ISP-proc parser, 8-way strstr+sscanf, no early exit**
  — ✅ **Implemented.** `strstr`→prefix-anchored `strncmp`+`strtol`,
  9-bit early-exit mask. Verified field-for-field identical against the
  old parser on 12 dumps including edge cases (indented lines, CRLF,
  garbage, missing values) — turned up a latent quirk along the way
  (an indented line matched the old `strstr` but the `sscanf` right after
  it never actually matched anything, so behavior is provably unchanged,
  not just probably).
- **T20 wrong-default ISP path, guaranteed failed fopen every tick** — ✅
  **Implemented, with an added safety net not in the original ask.**
  Caches the path that worked, but still re-probes the configured default
  once every ~5 minutes rather than never — a pure cache would silently
  stop noticing a corrected config value.
- **Schedule/sun-mode recomputes solar position every 2s** — ✅
  **Implemented** (2-slot memo on UTC day, since the caller needs
  today-and-tomorrow in one call). The smaller related item (caching the
  HH:MM time-window parse too) was **deliberately skipped** — its cache
  key would have to be the raw config string, and comparing that costs
  about what re-parsing it does.

Net size cost across all 13: **+804 B** `.text` on a T31 `-Os` cross
build (daynight.c dominates, from inlined `strncmp` calls being larger
than the `strstr` calls they replaced — a deliberate trade already
covered by the earlier reasoning), offset by real reductions elsewhere
(`hal_ingenic.c −48 B`, `httpd.c −40 B`, `rtsp.c −36 B`).

---

## Side findings (surfaced during the fix rounds, not in the original 26-item list)

### HTTP keep-alive (surfaced as a side-note while assessing TLS)

⏸ **Assessed only — recommended against.**

The idea: `httpd.c` sends `Connection: close` on every response
(confirmed genuinely unconditional, 8 hardcoded sites, and explicitly
documented as a load-bearing design choice, not an oversight — the Digest
nonce table depends on it). Keep-alive would help regardless of whether
TLS is ever turned on. Assessed and declined for three independent
reasons: (1) the connection-setup overhead it would remove is
0.1-0.3ms against per-request work that's 1-5ms (`/control`) or up to a
**measured 2694ms** (`/snapshot.jpg` waking an idle JPEG encoder) — noise
next to the real cost; (2) the traffic it would optimize barely exists —
a fleet-wide WebUI polling-frequency audit found the WebUI already uses
SSE push for 12 of 17 modules, the only recurring `/control` poll is a
dead fallback branch that only arms after 4 consecutive SSE failures, and
the only snapshot poll is 0.25 req/s in one editor page (there IS an
always-on ~12 req/min baseline from a shell-side heartbeat CGI on every
open page, found during that audit — small enough not to change the
verdict); (3) the real implementation cost is a restructure of a request
parser that currently assumes exactly one request per connection, in
exactly the area (`control_apply_json`'s permissive quoted-name scanner)
that already has documented prior bugs — plus streaming endpoints
structurally can't use keep-alive at all (close-delimited bodies, no
chunked encoding support), and the 8-client connection cap would go from
"a tab holds 2 of 8 slots" to "3 of 8," meaning 2-3 open browser tabs
could exhaust it. No code changes made.

### TLS session tickets

✅ **Implemented** — see Tier 1 §5 above; this is the same item, listed
here only because it wasn't part of the original 26-item enumeration.

---

## Correctness bugs found in passing (not the focus of this pass — none actioned)

None of these were requested to be fixed during the performance-fix
rounds; they remain open exactly as originally found.

- ⬜ **control.c:185-197** (`find_field`) — unlike its sibling `find_obj`,
  doesn't skip over string literal contents while scanning for a key
  name. A POSTed string value whose raw bytes happen to contain
  `"x":123` can bind to the wrong field. Authenticated-client-only, but
  silently misapplies a setting.
- ⬜ **daynight.c:522,574** — `waitpid()` on `switch_cmd`/`irprobe_cmd` has
  no timeout, called from the detection thread itself. A hung board
  script freezes day/night detection *and* daemon shutdown forever.
- ⬜ **config.c:1898-1899** — `config_write_keys()` silently truncates and
  drops keys past 64 in one persist, no log line.
- ⬜ **httpd.c:1376** — `serve_player`'s snprintf-overflow guard truncates
  and still sends 200 OK with a broken page, instead of refusing like
  every other overrun guard in the file does.
- ⬜ **osd_vars.c** — `g_fps`/`g_bitrate` and several small caches are
  shared, unsynchronized statics now written from two threads
  (`osd_thread` and the T23 `sw_rot_thread`) — worst case is one garbled
  OSD value on a torn read/write.
- ⬜ **osd_vars.c:167-168** — `{ip}`/`{mac}`/interface resolution caches
  permanently on first call, *including on failure* — a camera that
  boots before DHCP completes shows `0.0.0.0` until the next restart.
- ⬜ **rtp.c:334-335** — fragmented G.711 RTP packets stamp the wrong base
  timestamp for SR extrapolation. Unreachable today (frames never exceed
  MTU), noted for completeness.

---

## What was checked and found clean (so you know it wasn't skipped)

Lock scoping in the hub/fanqueue fan-out; wait strategies everywhere (all
condvar-based, zero busy-polling found across 28k lines); `net.c`'s socket
helpers (`accept4`, gathered writes — already about as tight as possible);
`log.c`'s level-filter short-circuit (arguments aren't evaluated when
filtered, in its current call sites); POST /control's linear key-matching
(technically O(n·m), practically irrelevant at the real table/body sizes);
the 24-slot SSE config table and motion ring (both correctly sized); the
`while(g_run) sleep(1)` main loop (looks wasteful, is actually the correct
minimal design given the signal-handling guillotine); `imp_motion.c`'s
grid processing (hardware-driven, CPU side is trivial); HAL bring-up
retry/backoff.

---

## Measured result

Before/after CPU, same concurrent RTSP(TCP)+fMP4+MJPEG load, cam-garage
(T31): **22.55% → ~14.5%**, roughly **35% CPU reduction**. Before figure
is a stable 2h-longrun average (95 samples, `v1.9.3-21`); after figure is
two independent 10s `/proc/<pid>/stat`-delta samples (14.59%, 14.39%) on
`v1.9.3-47`, agreeing within 2%. RSS: 5764 KB avg → 5128 KB under load,
~11% less. Two full 2h longruns (Garage + cam-vorne-garage) were kicked
off after this round of fixes specifically to get a durable, equal-length
before/after data point for the future — see
`dev_notes/qa-runs/longrun-cam-garage-2026-08-28/` and
`dev_notes/qa-runs/longrun-cam-vorne-garage-2026-08-28/` once those land.

## What's left, if picked back up later

1. **Tier 1 #3, the two zero-risk OSD wins** — rasterizer modulo→branch
   fix (33% measured) and the general config-template lesson beyond the
   one instance already fixed.
2. **Tier 1 #4's one open item** — verify the `<input type=color>` browser
   behavior; only then decide if a 3-site client-side fix is warranted.
3. **The 7 correctness bugs**, none performance-related, none actioned.
4. **Tier 2 #10, backchannel lock** — needs a real design decision
   (second lock + ordering invariant, or a ring+feeder-thread rework),
   not a batch fix.
5. **The larger NAL-boundary-table version of Tier 1 #2** — deliberately
   left as a documented follow-up, not attempted.
