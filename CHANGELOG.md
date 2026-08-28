# Changelog

All notable changes to timps are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/), and the project aims to follow
semantic versioning.

## [Unreleased]

### Added

- **`timps-qa.sh --profile longrun` (section 15c): long-run reliability
  testing for RTSP, fMP4 and MJPEG concurrently**, not just RTSP. Sections
  15 (soak) and 15b (drift) only ever opened the RTSP main stream, so a
  degradation confined to the HTTP fMP4 or MJPEG serving path was
  unreachable no matter how long those ran. The new section holds one
  session per protocol open at once for a shared window (default 2h,
  `--longrun-dur`/`--longrun-seg`/`--longrun-protos`) and judges each on a
  protocol-appropriate trend: A/V drift + delivery pacing for RTSP/fMP4
  (sharing 15b's verdict ladder), frame-rate/gap/JPEG-validity for MJPEG
  (which has no audio track, so drift is reported N/A rather than silently
  skipped). Also fixes a `printf`-locale bug (`LC_NUMERIC` mis-formatting
  numbers under non-`C` locales), extends the shared decode-warning pattern
  to catch truncated/corrupt JPEG frames, and closes a vacuous-PASS gap
  where a session that never received data could still report success.

### Changed

- **`/events` `stats` push now skips ticks where nothing actually changed**
  (`src/mp4/httpd.c`). Previously it re-sent every enabled stream's
  subs/fps/kbps/dims/codec/drop-counters on every `events.stats_ms` tick
  unconditionally - unlike the `motion`/`daynight` events beside it, which
  already only push on a real change. Split the old single `stats_json()`
  into `stats_collect()`/`stats_render_json()`/`stats_changed()` so the SSE
  loop can compare against the last-sent snapshot before paying for the
  `snprintf`+emit: subs/dims/codec/drop counters are exact-match (a drop
  bump means a client's link is actively struggling, worth reporting
  immediately rather than up to `events.stats_ms` late), fps/kbps get a
  noise threshold (0.1fps / 5%) so measurement jitter alone doesn't trigger
  a push. `uptime_s` is deliberately excluded from the comparison - it
  changes every tick by definition. A connection that opens still gets an
  immediate baseline push; an idle, unchanging stream now sends one instead
  of one every 2s.

- **The per-frame keyframe test no longer scans the whole access unit**
  (`src/hal/hal_ingenic.c`, `src/codec/vparam.c`). `au_is_key()` ran on the
  video producer thread for every encoded frame of every stream and walked
  the AU to its end whenever it found no IDR NAL - i.e. for the ~96% of
  frames that are P-frames, it read the entire frame just to answer "no".
  It now walks start codes and stops at the FIRST VCL NAL: an access unit
  carries exactly one coded picture, so all of its slices agree on
  IDR-/IRAP-ness, and the parameter-set/SEI/AUD NALs that may precede them
  are a few dozen bytes. Same answer, a couple of dozen bytes read instead
  of up to 1 MB. `vparam_update()` gets the matching treatment on the
  keyframe path: it stops once the access unit has supplied the complete
  set (VPS+)SPS+PPS instead of walking the several-hundred-KB IDR slice
  behind them - and it does that while `hub_prepare_locked()` holds the
  source lock that subscribe/unsubscribe and the `/events` stats tick also
  contend for. An AU that carries no parameter sets, or refreshes only part
  of the set, is still scanned to the end exactly as before.

  Deliberately NOT taken from `IMPEncoderPack`'s own NAL-type field, which
  every vendored SDK header does expose (`nalType.h264NalType`/`h265NalType`
  on T31/C100/T40/T41, `dataType.h264Type`/`h265Type` on T20/T21/T23/T30,
  with T20's union missing the H.265 arm entirely): the field describes a
  libimp bitstream *section*, which is not guaranteed to be one NAL, so
  believing it would trade a per-SoC-verifiable byte test for an unverified
  per-libimp-version assumption - and the SW-rotate path
  (`IMP_Encoder_YuvEncode`) has no pack array at all. With the early exit
  there is nothing measurable left to win. `+160 B` .text on T31/`-Os`.

- **Recording's free-space prune now walks the records tree ONCE per
  rotation instead of once per deleted file** (`src/record.c`). A card that
  has reached `record.min_free_mb` - the steady state of continuous
  recording, not an edge case - made every single `seg_open()` enter the
  prune loop, and each iteration did a full recursive
  `opendir`/`readdir`/`lstat` sweep of the whole tree just to identify the
  one oldest file, unlinked it, then swept the entire tree again for the
  next victim. With thousands of segments on the card that is seconds of
  synchronous I/O inside `seg_open()`, while encoded frames pile into the
  record fanqueue - so the cost was not confined to recording: an overflow
  there drops a keyframe and re-requests an IDR on the shared encoder, which
  every RTSP and SRT viewer pays for. `find_oldest()` is replaced by
  `collect_oldest()`, which gathers the 32 oldest files in one walk into a
  bounded insertion-sorted array; `prune_free()` unlinks from that list and
  re-checks `statvfs` between each unlink, stopping the moment there is
  enough space, and only re-walks in the rare case the batch is exhausted
  and the card is still short. A batch covers a normal rotation many times
  over, so the steady state is now one walk per rotation. Deliberately not
  keyed on the day/hour directory buckets that `record.name` produces by
  default: that pattern is runtime-mutable via `/control`, so a
  layout-agnostic oldest-by-mtime scan stays correct for a flat or
  custom-strftime naming scheme too.

- **Timelapse retention pruning is rate-limited to hourly** (`src/timelapse.c`).
  `prune()` ran after *every* successful shot, recursively `lstat`-ing every
  kept JPEG under the timelapse tree - at a 10 s interval and a multi-day
  `keep_days` that is a full-tree walk six times a minute, forever, to
  re-evaluate a cutoff whose granularity is days. It kept the SD card busy
  around the clock for no benefit, which also defeats the point of the
  just-in-time frame grab that otherwise leaves the pipeline idle between
  shots. A monotonic (`ms_now_us`) guard now skips the walk unless an hour
  has passed - still ~24x finer than the cutoff actually moves. The guard
  starts unset so the first shot after a start/restart prunes immediately,
  which is the one moment the backlog can be arbitrarily old.

- **`timps.conf.example`'s OSD section brought back in sync with the actual
  compiled default** (`timps.conf.example`, `src/config.c`). The shipped
  example bundled `{hostname}` into the same item as the live clock (forcing
  an unchanging value to re-rasterize every second alongside it) and had no
  `{uptime}`/logo entries at all - `config_defaults()` has set the intended
  time / hostname / uptime / logo 4-item layout (`src/config.c:307-336`,
  added 2026-07-11) as the real default all along; the example file had just
  drifted from it. Re-derived the example's `osd0.*`/`osd1.0.*` block from
  that function so both agree byte-for-byte (10px margins, same
  `/usr/share/images/thingino_100x30.bgra` - installed fleet-wide by the
  `thingino-logo` package, confirmed present in every camera profile's build
  output, not a timps-specific asset). Also lowered the `font_size` clamp
  256->128 (`src/config.c`): a 255-char item (the `txt[]` cap) at 256px could
  still transiently allocate a ~14 MB rasterizer canvas inside one
  `msttf_render()` call before `refresh_text()` discarded it for exceeding
  the frame - no legitimate OSD text needs to be that tall, and 128 keeps the
  worst case a quarter of that.

- **TCP-interleaved RTSP now batches a whole access unit into one
  `sendmsg()`** (`src/rtsp/rtsp.c`). The UDP path has batched into `sendmmsg`
  since P3; TCP was left at one `net_sendmsg_all()` per RTP packet, so a
  200 KB IDR fragmented into ~170 FU-A packets was ~170 syscalls serialized
  on that client's thread - the transport ffmpeg/Frigate/go2rtc default to.
  A byte stream needs no per-packet framing beyond the 4-byte `'$'` prefix,
  so the same `rtp_batch` now stages `{prefix + RTP header}` (one iovec, one
  segment, as before) plus a payload iovec pointing into the access unit, and
  the play loop's existing flush barrier pushes up to `RTP_BATCH_N` packets
  per `sendmsg()`. Measured on the host sim with a 1080p/6 Mbit source over
  RTSP/TCP: 3498 -> 346 `sendmsg` calls for a 5 s session. The zero-copy
  payload-lifetime contract is unchanged (`rtp_out_fn`, rtp.h) - the barrier
  that already covered the UDP batch covers this one by construction, and
  `sink_discard()` drops staged pointers on the error path. An interleaved
  RTCP packet flushes the batch before it is written rather than being
  staged, so it can never jump ahead of RTP bytes already queued on the
  stream. RTSPS deliberately keeps its one-record-per-packet path: mbedTLS
  has no scatter/gather write, so batching there would only trade the memcpy
  for extra TLS records (same reasoning as the existing note in
  `sink_send()`), and such sinks simply get no batch allocated.

- **The RTCP liveness drain is throttled to 1 Hz instead of running per
  media frame** (`src/rtsp/rtsp.c`). Every UDP-transport session did a
  non-blocking `recvfrom()` on both RTCP sockets on every iteration of the
  stream loop - ~50/s on a 25 fps video+audio session, essentially all of
  them `EAGAIN` - to feed `last_act_us`, whose reaping threshold is 120 s
  and whose real input (the peer's receiver reports) arrives every few
  seconds at most. Gated on the loop's existing `now` snapshot, matching the
  P-04 control-poll throttle a few lines above; the socket buffers hold
  whatever accumulates between drains. 6 s UDP session on the sim: 511 -> 137
  `recvfrom`, of which 486 -> 112 were `EAGAIN`.

- **The fMP4 mux no longer copies every video access unit twice** (`src/mp4/
  fmp4.c`). `trun` has to record the sample length before any `mdat` byte is
  written, which is why `fmp4_video_fragment()` built the whole AVCC sample
  in a per-thread scratch `ms_buf` and then copied it again into the caller's
  fragment buffer - once per client per frame, plus once for the recorder.
  The Annex-B walk now *indexes* the AU's NALs (offset+length, skipping
  parameter sets) in the single pass it already had to make, which yields
  both the total length for `trun` and the pointers the `mdat` body is then
  written from - straight into `out`. `fragment()` is split into
  `fragment_head()` (moof + the mdat box header, byte-for-byte unchanged) and
  the body write; the audio path keeps the old contiguous-sample signature as
  a thin wrapper. Deliberately NOT done as a length-only pre-pass:
  `nal_iter`/`find_start` is a byte-at-a-time start-code scan, so a second
  walk of a 200 KB IDR would have cost more than the word-wise `memcpy` it
  saved - indexing is what makes one pass enough. Also removes the
  `pthread_key` scratch machinery entirely, i.e. one frame-sized persistent
  buffer per fMP4 client thread and per recorder. An AU with more than
  `FMP4_NAL_IDX` (32) non-parameter-set NALs falls back to re-scanning via
  the old `annexb_to_sample()`. Verified byte-identical against the previous
  implementation on three H.264 sources, with the index cap forced to 2 to
  exercise the fallback as well.

- **MJPEG sends one frame in one syscall instead of three**
  (`src/mp4/httpd.c`). The part header, the JPEG body and the boundary
  delimiter were three separate `csend()`s - and with `TCP_NODELAY` set,
  three TCP segments - for every frame. A new `csendv()` wraps
  `net_sendmsg_all()` for the plain path (the HTTPS path keeps the sequential
  writes: mbedTLS has no gather write, and it is not the per-frame-cost
  case). `http_send_ex()` and `snapshot_jpg()`, the same header+body shape at
  much lower frequency, use it too. Wire output is byte-identical, boundary
  framing included.

- **`jpeg_thread` releases the IMP encoder stream before the snapshot file
  write, not after** (`src/hal/hal_ingenic.c`). Order was `GetStream` ->
  assemble into the pooled packet -> `fopen`/`fwrite`/`fclose`/`rename` the
  snapshot JPEG -> publish -> `ReleaseStream`, so a card doing wear-levelling
  (hundreds of ms, occasionally seconds) held one of the encoder's stream
  buffers checked out for the whole stall, with the next frame having nowhere
  to land. `enc_assemble_packs()` has already copied every pack into
  `pk->data` by then and nothing after that point reads `st`, so the release
  simply moves up.

- **The SRT TS mux stops copying payload a byte at a time, and builds each
  packet straight into the send batch** (`src/srt.c`). `send_pes()` filled
  the PES header and payload into its 188-byte packet with three scalar
  `while` loops, then `ts_send()` `memcpy`'d that stack buffer into
  `m->batch` - so every byte of every access unit went through a per-byte
  loop and then a second copy. The loops become `memcpy`/`memcpy`/`memset`,
  and `ts_slot()`/`ts_commit()` let `send_pes()` write directly into the next
  free batch slot (the PSI writers keep `ts_send()`: they build their packet
  before they know it will be sent). Output verified bit-identical to the
  previous mux across 16 payload sizes spanning the 188- and 1316-byte
  boundaries, video (with PCR + random-access adaptation field) and audio.

- **`GET /control` no longer walks the sounds directory on every request**
  (`src/control.c`, `USE_PLAY` builds). Building `caps.play.sounds` meant an
  `opendir`/`readdir` walk plus one `stat()` per candidate - up to
  `SOUNDS_LIST_MAX` (96) of them - on every single GET *and* every `/events`
  snapshot, for a directory that ships with the firmware image and
  essentially never changes at runtime. The rendered list is cached and
  rebuilt only when `SOUNDS_DIR`'s own mtime moves (one `stat()` on the
  directory, not on its contents). Guarded by its own mutex because the
  callers are concurrent httpd worker threads, and held across the append so
  the buffer cannot be reallocated under a reader. Directory mtime has 1 s
  granularity, so a file dropped in during the same second as a build can be
  missed until the next change - immaterial for a picker list, and the `play`
  POST re-validates the chosen name against the real directory anyway.

  Size: `+201 B` .text / `-12 B` .data / `-16 B` .bss on the default T31
  `-Os` build for all of the above (`+280 B` .text and `+48 B` .bss more in a
  `USE_PLAY` build, `+50 B` .text for `srt.c`). Heap moves the other way: TCP
  RTSP clients gain a ~1.1 KB batch each, while every fMP4 client thread and
  the recorder lose a frame-sized (up to a few hundred KB) scratch buffer.

- **TLS listeners now offer session resumption via RFC 5077 tickets**
  (`src/tls.c`, `USE_TLS` builds). Every HTTPS/RTSPS connection previously
  paid for a full handshake - an RSA/ECDHE signature on a 1 GHz MIPS core -
  even when the same browser reconnected seconds later, which the WebUI does
  constantly. `ms_tls_ctx_new()` now sets up an `mbedtls_ssl_ticket_context`
  (AES-256-GCM) fed by the DRBG the context already seeds, and hands its
  write/parse callbacks to the config; httpd's HTTPS listener and rtsp's
  RTSPS listener each build their own `ms_tls_ctx`, so they keep their
  separate ticket keys rather than sharing one.

  Tickets only - deliberately not `mbedtls_ssl_conf_session_cache()`, whose
  `f_get_cache` hook mbedTLS 3.6 references only from `ssl_tls12_server.c`:
  this build's `PRESET_DEFAULT` reaches TLS 1.3, where every current browser
  lands and where the cache does nothing. Server-side wiring is
  `mbedtls_ssl_conf_session_tickets_cb()`; `mbedtls_ssl_conf_session_tickets()`
  is the client-only, TLS-1.2-only knob and is not used here.

  The ticket key lifetime is a real 12 h (`MS_TLS_TICKET_LIFETIME`), never 0:
  mbedTLS rotates the ticket key only when the lifetime is non-zero, so 0
  would pin one key for the whole uptime of a camera that runs for months,
  and a single key compromise would then retroactively decrypt every session
  ever resumed under it. 12 h keeps that window short while still covering a
  WebUI session, and stays well under the 7 days TLS 1.3 allows. Note that a
  *resumed* session is not forward-secret for the life of the ticket key
  (full handshakes keep their ECDHE forward secrecy either way) - now stated
  in `timps.conf.example`'s TLS section, since it is a user-visible property
  and not just an implementation detail.

  All of it sits behind `MBEDTLS_SSL_SESSION_TICKETS && MBEDTLS_SSL_TICKET_C`,
  so an mbedTLS trimmed for flash still compiles and simply runs without
  resumption, and a runtime `mbedtls_ssl_ticket_setup()` failure warns and
  keeps the listener rather than killing it. `mbedtls_ssl_ticket_free()` runs
  after the config (which points at the ticket context) and before the DRBG
  (which the ticket context points at). `+208 B` .text in `tls.o` on the T31
  `-Os` cross build, byte-identical to before with tickets compiled out.

- **One clock read per published frame instead of three** (`src/hub.c`,
  `src/hub.h`, `src/hal/hal_ingenic.c`, `src/hal/hal_sim.c`). There is no vDSO
  on this target, so every `ms_now_us()` is a real `clock_gettime` syscall -
  and the publish path made three of them for what is, to microseconds, one
  instant: the producer took one for `pts_sanitize()`, `hub_publish*()` took
  another for the packet's `enq_us` stamp, and `hub_prepare_locked()` took a
  third *while holding the source lock* that subscribe/unsubscribe and the
  `/events` stats tick contend for. `hub_publish()`/`hub_publish_take()` now
  take the producer's reading as a `now_us` argument and thread it through,
  so each published access unit costs exactly the one syscall its producer
  already had to make. Applies to the audio publish path too, which had the
  same three-deep pattern in each of its AAC/Opus/G.711 branches. The same
  fix in `src/srt.c`'s `stream_run()`: one reading per loop iteration now
  serves the link-stats tick, the encoder-stall bound, the arrival stamp and
  the PAT/PMT cadence, instead of three per received packet - and it is read
  *after* the pop, so it is the arrival instant rather than one that predates
  a wait of up to 200 ms.

- **The per-client fanqueue is locked once per frame, not four or five
  times** (`src/fanqueue.c`, `src/fanqueue.h`, `src/rtsp/rtsp.c`,
  `src/mp4/httpd.c`). Both streaming loops asked their queue a series of
  one-line questions immediately around every `fanqueue_pop()` - is it
  closed, did an overflow lose a keyframe, did it lose any packet, how deep
  is the backlog - and each was its own lock/unlock cycle on the very mutex
  the producer thread contends for on every push. New `fanqueue_pop_ex()`
  answers all of them inside the pop's own critical section and hands back an
  `fq_status`. The overflow flags are read-and-cleared exactly as their
  `take_*` functions did, but ONLY on a pop that returns a packet: an
  overflow always leaves its packet queued behind it, so the flags travel
  with that packet and a timed-out pop cannot swallow a signal it has no
  packet to deliver alongside. `dropped_audio` is deliberately left out - its
  read-and-clear timing is what lets `httpd.c` tell a real mute apart from a
  congestion eviction (2026-08-22, cam-garage), and folding it in would have
  cleared it on every pop and re-broken exactly that. Both loops also stopped
  needing a second clock read: the iteration's `now` is now taken after the
  pop, which is where the trace's `t_pop` wanted it anyway.

- **`fanqueue_push()` only signals the condvar when the queue was empty**
  (`src/fanqueue.c`). Single consumer, so a waiter can only be parked when
  the queue is empty; once a client has any backlog at all - which is
  precisely the state a struggling client sits in - every push was paying for
  a `pthread_cond_signal()` that could not wake anybody. `fanqueue_close()`
  still broadcasts unconditionally.

- **`pkt_pool_get()` no longer copies the previous frame when it grows a
  pooled buffer** (`src/frame.c`). The borrow starts at `len == 0`, so the
  old contents are the last frame's bytes - which the caller is about to
  overwrite and nobody will read - yet `realloc()` faithfully copied them on
  every grow (the ~1/GOP oversized IDR). `free()` + `malloc()` skips the copy
  and lets the allocator pick a better-fitting chunk instead of having to
  extend or move this one.

- **Audio frames are encoded straight into a pooled hub packet**
  (`src/hal/hal_ingenic.c`). Video and JPEG have used
  `hub_pkt_get()` + encode-in-place + `hub_publish_take()` since P-01; audio
  was still on the old `pkt_new()` path, encoding into a `__thread` scratch
  buffer and then paying a `malloc` plus a full-frame copy out of it, per
  frame, forever. All three encoder branches (faac, Opus, G.711) now take the
  pooled buffer as their output target directly; a borrow that yields no
  frame (faac priming, an encoder error, OOM) is handed back with
  `pkt_unref()`. Small in bytes - 320 B to 1.5 KB at 25-50 frames/s - but it
  also retires the 8 KB AAC and 4 KB Opus scratch buffers, dropping the audio
  worker's thread-local footprint by ~12 KB.

- **`ms_buf_reset()` stopped realloc-ing the recorder's fragment buffer down
  and back up once per GOP** (`src/util.c`, `src/util.h`). The shrink-back
  exists to release a ONE-OFF outlier, but it fired the instant a single
  payload fit under the cap again - so a stream whose payloads *routinely*
  exceed it paid a shrink-realloc plus a grow-realloc every time one arrived.
  At 4-6 Mbit with a 2 s GOP that is every IDR fragment against the 256 KB
  cap `record.c` and the fMP4 client loop in `mp4/httpd.c` both pass: a
  realloc pair per GOP, per recorder and per fMP4 client, for the life of the
  process. The capacity is now handed back only after `MS_BUF_SHRINK_RUN`
  (64) consecutive resets have stayed under the cap - i.e. the big payload
  really was the exception and not the shape of this stream. A genuine
  outlier is still released, a couple of seconds later; the memory bound is
  unchanged.

- **T23 software rotation: the inner loop no longer recomputes a destination
  index per byte** (`src/hal/nv12_rot.c`). This is the highest-frequency loop
  in the daemon when it runs - ~21 MB/s of plane bytes at 720p15 - and each
  byte cost a direction ternary, a multiply and an add to locate its
  destination. `x` enters that index only through the `x*dw` /`(sw-1-x)*dw`
  term, so within a source row the destination advances by a CONSTANT `±dw`
  elements: the whole computation collapses to one pointer add, and the
  1-byte (Y) versus 2-byte (CbCr pair) test is hoisted out of the innermost
  loop with it. Verified byte-for-byte against the previous implementation
  over both directions at 12 frame geometries, including odd and
  smaller-than-a-tile ones.

- **OSD templates without a `%` skip the whole time-formatting stage**
  (`src/hal/osd_vars.c`). `osd_expand()` ran the strftime whitelist pass plus
  `localtime_r()`+`strftime()` unconditionally - once per OSD item, per
  refresh tick - including for pure `{var}` templates and static labels that
  contain no strftime directive at all. It now copies through and returns as
  soon as stage 1 comes out without a `%`. A `{var}` whose *value* contains a
  `%` still goes through the whitelist exactly as before.

- **The day/night ISP scrape parses prefix-anchored, stops when it has
  everything, and remembers which `/proc` path works** (`src/daynight.c`).
  This loop runs every `daynight.interval_ms` forever - ~43,000 times a day
  at the 2 s default - and did three avoidable things on every pass.

  It matched each of its nine fields with `strstr()` (scan the whole line for
  the prefix, anywhere) and then re-matched the same prefix with `sscanf()`
  to extract the value. The dump's value lines all start at column 0 and the
  `sscanf` formats were anchored there anyway - a scanf literal only ever
  matches at the current position - so an indented line already yielded
  nothing despite `strstr()` matching it. One `strncmp()` plus `strtol()`
  therefore accepts exactly the same lines for one comparison instead of two
  scans, with `strtol`'s "no number there -> leave the target alone" rule
  preserving the `-1` absent-markers. The `MAX ...` prefixes keep their place
  ahead of the plain ones they contain. Verified against the old chain on 12
  dumps: the two real fleet layouts (T31/T23 `isp-m0`, T20 `isp_info`), the
  replay harness's, and the empty/garbage/indented/duplicate-substring edge
  cases - identical field-for-field. The loop also stops once all nine fields
  have been supplied rather than reading out the rest of the register dump.

  And on a T20 the *first* `fopen()` was guaranteed to fail: `isp-m0` does
  not exist in that SDK and never will, so the configured-path-then-fallback
  probe order cost one ENOENT syscall pair on every single tick of those
  cameras' lives. The working path is now remembered and tried first, with
  the configured one re-probed whenever the cached path stops opening and
  once every `DN_ISP_REPROBE_TICKS` (~5 min) regardless - so "the configured
  path came back", or was corrected via `/control`, is still picked up.

- **Schedule mode computes sunrise/sunset once a day, not once every 2 s**
  (`src/daynight.c`). `mode=schedule` with a lat/lon calendar ran the full
  sunrise equation on every tick: `gmtime_r` plus ~10 double-precision libm
  calls (`sin`/`cos`/`asin`/`acos`/`fmod`) on a soft-float SoC, ~43,000 times
  a day, to re-derive two instants that are constant for the whole UTC day by
  construction. `dn_sun_times()` now memoizes on `(UTC day, lat, lon)`, so a
  coordinate change via `/control` still takes effect on the next tick, while
  the sunrise/sunset OFFSETS are applied by the callers afterwards and need
  no invalidation at all. Two slots, because `dn_secs_to_dawn()` asks for
  today and tomorrow within one call and one slot would thrash between them.
  Verified identical (return value and both instants) against the uncached
  form over 400 consecutive days at six locations including polar day and
  polar night, with the today/tomorrow interleave. The `HH:MM` window mode's
  per-tick `sscanf` was left alone: its cache key would be the config string
  itself, and comparing that costs about what parsing it does.

- **Backchannel resampling dropped its per-sample floating-point divide**
  (`src/codec/resample.c`). `ms_resample()` computed `pos = i / ratio` for
  every output sample - a soft-float divide per sample on the two-way-talk
  path. It now walks a fixed-point cursor with one add per sample. Q32, not
  Q16: the step is truncated so its error accumulates across the call, and at
  Q16 a 3x upsample of a full 16 K-sample buffer walks off by ~0.1 sample,
  which is visible against the reference; Q32 puts that at 1e-5 samples.
  Measured against the old double implementation across 8/16/44.1/48 kHz
  conversions in both directions: identical sample counts, maximum deviation
  2 LSB out of ±32768 (~-84 dBFS), which is the truncate-toward-zero versus
  floor rounding difference and not error.

  Sizes for all of the above, T31 `-Os` cross build, `.text` per object:
  `daynight.c +656 B` (the inlined constant-prefix `strncmp`s are bigger than
  the `strstr` calls they replace, and the sun memo adds a lookup),
  `fanqueue.c +116 B`, `osd_vars.c +52 B`, `util.c +48 B`, `nv12_rot.c +32 B`,
  `resample.c +20 B`, `hub.c +4 B`, `frame.c ±0`, against `hal_ingenic.c
  -48 B`, `httpd.c -40 B` and `rtsp.c -36 B` - `+804 B` net. `srt.c` is not
  in that count (its build needs libsrt headers); its change is a few
  removed calls.

### Fixed

- **A hung `daynight.switch_cmd`/`irprobe_cmd` no longer freezes day/night
  switching AND daemon shutdown** (`src/daynight.c`). Both board hooks are
  `fork()`+`execlp()`'d from the detection thread and were then reaped with a
  plain blocking `waitpid()`, so a script that never returns - stuck I2C, a
  script on a mount that stopped answering, a shell waiting on a pipe nobody
  writes - parked that thread inside the kernel forever. Day/night stopped
  switching, and because `daynight_stop()` `pthread_join()`s the same thread,
  so did shutdown: the daemon had to be killed. The new `dn_reap()` polls
  `waitpid(WNOHANG)` on the thread's own stop gate (the `ms_stopgate` idiom
  the sample loop already sleeps on, so a shutdown is noticed within one
  20 ms slice rather than at the deadline), `SIGKILL`s a hook still running
  after 10 s (`switch_cmd`) / 5 s (`irprobe_cmd`), and bounds the reap after
  that kill too - a child wedged in uninterruptible sleep survives `SIGKILL`,
  and waiting on it would be the same hang one step later, so it is
  abandoned with an ERROR line instead. When shutdown is already requested
  the deadline shortens to 1 s: long enough that a healthy script finishes
  the switch it started (verified - a fast hook still returns its real exit
  status during shutdown), short enough that a wedged one cannot hold the
  join. The cost on the normal path is up to one 20 ms poll slice per hook
  invocation, i.e. twice a day for `switch_cmd`.

- **`find_field()` no longer matches a key name inside a string VALUE**
  (`src/control.c`). Its sibling `find_obj()` has always skipped over string
  literals while brace-matching; `find_field()` swept the object's byte range
  with a raw `memcmp`, so the bytes of a member name occurring inside an
  earlier string value bound the lookup to that substring. A `/control` POST
  of `{"osd":{"0":{"text":"hi"x":999","x":10}}}` set `osd0.x` to **999**
  instead of 10 - the value came out of the middle of the text field. Only an
  authenticated client can reach it and it only misapplies that client's own
  request, but it is a real field-N-takes-field-M's-value bug. The scan now
  steps literal to literal and skips the contents of every one it does not
  match, the same discipline `find_obj()` uses. Well-formed JSON is
  unaffected either way (`\"` escapes never produced a raw match); the
  reachable case is the lenient parser's tolerance of unescaped quotes.

- **The `{ip}`/`{mac}`/`{net}` OSD placeholders no longer latch a failed
  interface probe for the rest of the session** (`src/hal/osd_vars.c`).
  `resolve()` cached the `getifaddrs()` result on the FIRST call and set the
  cache flag even when the probe failed - and on these cameras the first OSD
  tick routinely runs before DHCP has answered (no RTC, network comes up
  asynchronously). A camera that booted that way burned `{ip}`=`0.0.0.0`,
  plus a guessed `eth0` for `{mac}`/`{net}`, into its video until the next
  restart. Only a genuinely resolved interface is cached now; a failure keeps
  the fallback strings but stays unresolved and re-probes on the same ~1 s
  TTL the rest of the file's readers use (not per call - that is the cost the
  caching exists to avoid).

- **`osd_vars.c`'s shared caches are no longer written from two threads
  without synchronization** (`src/hal/osd_vars.c`). `g_fps`/`g_bitrate` and
  the `cached[]`/`last_us` pairs in `get_mac`/`get_uptime`/`get_net_tx`/
  `get_cpu`/`get_mem` are reached by imp_osd.c's OSD updater thread and, on
  T23 with software rotation, by one `sw_rot_thread` per rotated channel - a
  mixed config runs both at once. The `double`s are torn reads on 32-bit
  MIPS and the string caches could be read mid-`snprintf`. Worst case was one
  garbled OSD value for one tick rather than a crash, but it is UB. One
  file-local mutex now covers all of it; the protected regions are a string
  copy or a once-per-second `/proc` read and never call back out of the file,
  and `resolve()` copies the interface name out from under the lock before
  calling the `get_*()` helpers, so the lock can never be taken recursively.

- **`serve_player()` answers 500 instead of shipping a truncated player page
  as 200 OK** (`src/mp4/httpd.c`). Every other `snprintf` overflow guard in
  this file refuses; this one clamped `n` and sent the cut-off HTML as a
  success, which is a page severed mid-`<script>` and a dead `<video>` with
  no reason given. It now mirrors the `/control` JSON guard beside it. The
  rendered page is ~3.0 KB against the 4 KB buffer, so this is a tripwire for
  a future edit rather than a live failure.

- **`config_write_keys()` says so when it drops keys past its 64-key
  tracking array** (`src/config.c`). The clamp is unreachable from the only
  caller (`/control` batches at most `CTRL_MAX_CHG` = 48), but it was the one
  shape of persist failure that cannot be noticed: the values are live in
  `g_cfg` and read back correctly, and the loss only surfaces at the next
  restart. It now logs which key the tail starts at. The array is left at a
  fixed 64 - sizing it by `n` would put a caller-controlled allocation on a
  path that today has no caller able to reach it.

- **A fragmented G.711 packet records its own timestamp for the RTCP SR**
  (`src/rtsp/rtp.c`). The fragment loop stamped each RTP header with
  `ts + off` but handed `emit()` the base `ts`, so `last_rtp_ts` - which the
  Sender Report extrapolates from - lagged by up to one fragment offset.
  Unreachable today (a 320-byte G.711 frame never exceeds the MTU), fixed
  because it is a one-line correction to a value the SR depends on.

  Sizes, T31 `-Os` cross build, `.text` per object: `daynight.c +928 B`
  (`dn_reap()` plus its log strings), `osd_vars.c +416 B`, `config.c +156 B`,
  `control.c +80 B`, `httpd.c ±0`, `rtp.c ±0` - `+1580 B`, `+1.7 KB` in the
  linked binary.

## [1.9.3] - 2026-08-23

### Changed

- **Day/night boot now MEASURES before it decides, and asserts the answer on
  the board once** (`src/daynight.c`, `src/daynight.h`, `src/config.c`,
  `docs/wiki/Day-Night.md`, `docs/wiki/Configuration-Reference.md`). The old
  boot path committed the persisted `image.running_mode` to the ISP
  immediately and only *optionally* verified it afterwards, and it
  deliberately never ran `switch_cmd` at all. Two things were wrong with
  that. The persisted value has no bounded age - a camera that sat powered
  off for a year boots carrying a year-old opinion about the light - and
  because nothing forced a physical assertion, a boot that *confirmed* the
  persisted value left the board untouched. That is the 2026-08-22 IR/ircut
  desync: five fleet cameras rebooted after dark, adopted `night` in
  software, and spent the night with the IR LEDs off, because
  `/run/thingino/daynight_mode` is tmpfs, resets to `day` on every reboot,
  and is only ever written by an actual `daynight day|night` call that never
  came.

  Boot is now: wait for the AE to converge (unchanged bounds -
  `DN_BOOT_SETTLE_S`, `DN_STABLE_N`, hard-capped at `DN_STABLE_MAX_MS`), run
  **one ordinary probe** into the day pipeline - the same `dn_switch`, the
  same `DN_PROBE_SETTLE_S` verdict, the same ISP readback gate a runtime
  probe uses, no second measurement mechanism - read it against `day_gain`,
  and assert the answer. Cost, counted honestly: **one `switch_cmd`
  invocation when the answer is day** (the probe's own drive *is* the
  assertion) **and two when it is night**; on a board that comes up in its
  reset day position the first moves nothing and the second is the movement
  the camera actually needs. `daynight.boot_probe=0` now opts out of the
  *measurement* only - the persisted value is adopted, then still asserted on
  the board once - and a railed AE (0 units of reserve) overrides the opt-out
  exactly as before, except that with `boot_probe=1` the measurement *is* the
  real transition that re-tunes the meter, so the T23 railed-boot case is
  subsumed rather than special-cased.

  Three things the boot probe deliberately does NOT do, each of them a scar:
  it never takes the **silent** route (the ratio divides a lit reading by an
  unlit one, and at boot nothing has established that the illuminator was
  ever on - in a dark room that lands on "pegged, therefore night", the right
  mode with no assertion, which is precisely how the desync went unnoticed);
  it never arms the repeating **running_mode re-assert** (the re-assert
  re-drives whatever `image.running_mode` says, which at boot is still the
  persisted guess, and it would land `DN_REASSERT_MS` in - right on the boot
  verdict - which is the 2026-08 "switch to day overwritten twice, eight
  seconds apart" living-room incident verbatim); and it keeps its pre-probe
  level as the night-reference candidate rather than deferring the anchor to
  `DN_REF_DELAY_S`, because an anchor sampled 30 s after a boot can land
  inside a lighting transition - which is incident f8a7b21, and scenario 02
  reproduced it immediately when that was tried. For the same reason the boot
  probe charges `probe_min_gap_s` like any other probe: exempting it let the
  very next transient re-anchor the reference it had just set correctly.
  Post-boot transitions, their re-assert timers and their readback
  enforcement are untouched.

  If no usable exposure reading appears within `DN_STABLE_MAX_MS` of start-up
  there is nothing to measure: boot falls back to the persisted value, still
  asserts it on the board once, and says so with a `WRN`.

  Corpus (`scripts/dn-scenarios/`, `scripts/dn-replay.py`): every changed
  scenario carries a dated note saying what moved and why. Click budgets
  re-derived for the boot pair (19-23, 25-28); the two boot-probe log names
  updated (04, 07); and four scenarios genuinely retimed rather than
  re-budgeted, because a boot that always performs a real transition consumed
  the setup they depended on - 19 runs to t=780 so the darkness ratio verdict
  it asserts comes from the post-light-off heartbeat probe instead of from
  boot; 25 pins `probe_min_gap_s` at its 60 s floor so its light-on event is
  still reachable (it is not a scenario about rationing); 26 moves its phase-1
  `filter_cost` measurement from the boot silent probe to a runtime path-C
  probe, without which phase 2's projection branch was never reached and the
  scenario was quietly testing nothing; and 27's `isp_sticky` gained a `from`
  key plus an honestly dark pre-light day curve, because a stuck-from-boot ISP
  has its release edge eaten by the boot transition and the readback drama
  would have played out at t=24 instead of at the runtime probe the incident
  actually happened on. Full corpus: 28/28. Worth knowing before trusting a
  single run: `03-noisy-night` came back 6 switches against a budget of 5 on
  two consecutive runs - one of them a pre-change baseline - and 4 on the
  third, so it is flaky on harness timing rather than on this change.

### Fixed

- **A real restart can no longer strand the daemon dead on a start-stage
  encoder alloc failure** (`src/main.c`) - root cause of the 2026-08-22 T31
  fleet incident (5 of 12 cameras down after `--test-encoder`'s
  restore-restart, manual reboot needed). After `S95timps restart`, the
  previous instance's rmem carve-out (22 MB on the T31 boards) is not always
  released by the time the new instance runs: `wait_stop()` only proves the
  PID is gone, while the kernel-side ISP/encoder release lags (measured 4 s+
  after a *clean* teardown, longer/possibly never when the 3 s shutdown
  guillotine cut `g_hal->stop()` short). The bring-up handled this
  asymmetrically: `g_hal->init()` failures retried forever (backoff loop +
  the `IMP_System_Init` 5x1s retry), but a single `g_hal->start()` failure
  exited the process permanently - and on low-RAM T31 boards it is precisely
  start's big contiguous `Codec_Encode_Create` alloc that fails while
  init's small allocs squeak through. The QA evidence matches exactly: the
  five dead cameras were each observed minutes later still alive with 2
  threads/0 listeners (parked in the init retry loop), then gone for good
  (the one unguarded start-failure exit); cam-garage recovered because its
  pool drained before start ran. `start()` failure now unwinds via
  `g_hal->stop()` (safe: `ing_start`'s fail path already leaves no channels,
  and `ing_stop`/`imp_osd_stop` tolerate empty state) and re-enters the same
  retry loop. Additionally the shutdown path re-arms the hard-exit alarm
  right before `g_hal->stop()`, so the vendor IMP teardown always gets the
  full `MS_SHUTDOWN_ALARM_S` budget instead of whatever the recorder/server
  stops left over - shrinking the window in which a guillotined teardown
  leaves the pool dirty for the next instance in the first place.
  Hardware-verified same evening on cam-kinder-rechts: the daemon stayed
  alive and kept retrying instead of dying, though on that board the
  rmem carve-out needed a real reboot to actually clear rather than
  clearing on its own within ~9 minutes of retries - which is why the
  next entry adds a bound.
- **A `start()` failure loop that never clears no longer retries forever**
  (`src/main.c`) - discovered immediately by the hardware verification of
  the fix above: retrying every 60 s is only an improvement over dying if
  the retries eventually succeed, and on a board whose rmem genuinely will
  not clear without a reboot they do not. `start()` failures are now capped
  at `MS_STARTUP_MAX_START_FAILS` (10); past that the daemon logs clearly
  and exits, matching the existing `MS_VIDEO_WATCHDOG_MAX_RECOVERIES`
  convention in `hal_ingenic.c` (give up loudly, let a human/scheduler
  restart it, rather than spin forever looking alive while serving nothing).
  `init()` failures are unaffected and still retry forever - a
  misconfiguration is a different failure class, meant to wait for a human
  to fix the config rather than a bounded resource race.
- **Exhausting the start() retry budget now escalates to one real reboot
  before giving up** (`src/main.c`) - retries alone did not fix
  cam-kinder-rechts' incident, a real `reboot` did, every time this class of
  problem occurred tonight. A persistent marker file
  (`/etc/timps-startup-reboot.flag`, survives the reboot unlike anything in
  `/run`) makes the escalation exactly one-shot per incident: if the SAME
  problem is still failing after the reboot, the daemon gives up for good
  instead of rebooting again, so this cannot become a boot loop. The marker
  clears the moment `start()` next succeeds, so a later, unrelated incident
  gets its own fresh attempt. Hardware-verified on cam-kinder-rechts by
  deliberately reproducing the stuck state again: counted 1/10 through
  9/10 over ~8 minutes, escalated to a real reboot at attempt 10, back to
  serving within ~15s with no manual intervention, marker file cleared.

### Added

- **GET /control now reports the rate-control attrs the encoder ACTUALLY
  holds** (`src/hal/hal.h`, `src/hal/hal_ingenic.c`, `src/control.c`): each
  `encoder.<n>` status object gains an `rc` sub-object read live via
  `IMP_Encoder_GetChnAttrRcMode` - per channel, read-only, present on every
  supported SoC. This is the missing diagnostic from the T23 rate-control
  investigation (`dev_notes/T23_RATECONTROL_INVESTIGATION_2026-08-21.md`):
  timps has always written these attrs at bring-up and never verified they
  arrive unaltered, and two header-derived hypotheses about the controller
  already failed against measurement. Keys reuse the `videoN.*` names where
  they mean the same thing, so written and held values can be diffed
  directly; the object is deliberately separate from the configured `video`
  block. On the ENC_NEW_API SoCs this also makes `uMaxBitRate`, `iIPDelta`,
  `iPBDelta`, `eRcOptions`, `uMaxPictureSize` and `uMaxPSNR` readable for the
  first time (raw SDK values, units unverified - that open question is one of
  the things the readback exists to answer). Channels without a queryable
  encoder (disabled stream, T23 sw-rotate path, host sim) omit the object,
  matching the stats behaviour.

- **`videoN.fluc_lvl` exposes the H265 flucLvl knob** (0..4, default 0 = the
  previous literal, so an untouched config produces the same stream;
  `src/config.h`, `src/config.c`, `src/hal/hal_ingenic.c`, `src/control.c`,
  `timps.conf.example`). It was the last classic-path rc literal with a
  documented domain ("bitrate fluctuation relative to the average") and is
  the H265 counterpart to `i_bias_lvl`. H265 only: the H264 rc structs have
  no such field, and the T23 sw-rotate path stays H264-only. `staticTime`
  (2), `frmQPStep` (3), `gopQPStep` (15) and `gopRelation` (0) deliberately
  stay hardcoded - the SDK headers document no range for any of them, and a
  key without defensible bounds is an invitation to misconfigure. Per
  channel like every other `videoN.*` key. Both status-JSON branches in
  `control.c` were extended (the drift trap that bit in 8f3c84c).

- **The rate-control keys apply LIVE to the running encoder** where the
  SoC's SDK allows it - per channel, no daemon restart, effective at the
  next IDR/GOP (`src/enc_caps.h` new, `src/hal/hal_ingenic.c`, `src/hub.h`,
  `src/hub.c`, `src/control.h`, `src/control.c`, `src/mp4/httpd.c`,
  `timps.conf.example`). What is live per platform (from the vendored
  headers, 2026-08-21):
  - classic T10..T30 incl. T23: `rc_mode`, `bitrate`, `qp`, `min_qp`,
    `max_qp`, `quality_lvl`, `change_pos`, `i_bias_lvl` - one
    `IMP_Encoder_SetChnAttrRcMode` call re-derives the whole rc union from
    the config via the same fill bring-up uses (now factored into
    `classic_rc_fill()`), so the encoder never sees a half-updated struct.
    H264 streams only; the SDK marks the call H264-only.
  - T31/C100: `bitrate` (SetChnBitRate), `min_qp`/`max_qp`
    (SetChnQpBounds), `qp` under fixqp (read-modify-write via
    Set/GetChnAttrRcMode), `i_bias_lvl` (SetChnQpIPDelta).
  - T40: as above minus `i_bias_lvl`; T41: only `bitrate` and QP bounds
    (no rc-mode setter in that SDK at all).
  - `SetChnBitRate` takes bit/s while `videoN.bitrate` is kbps - converted,
    and the target:max ratio the channel currently holds is preserved (raw
    readback quotient, so it is unit-safe) instead of flattening the SDK's
    `uMaxBitRate` default.
  Everything is graded honestly, per request: `hub_control()` now returns
  whether a key reached the running pipeline, GET /control advertises the
  platform's live set as `caps.video_live` (empty on the sim), and every
  POST reply carries `deferred`/`deferred_keys` listing the changed
  video/sensor fields that did NOT apply live (channel not running, classic
  H265, rejected IMP call) and therefore wait for a restart. The
  `control.h` persist-only contract is updated accordingly; all other
  sections keep their documented semantics. Verified against the
  simulator (grading, caps, clamping, per-channel isolation); the actual
  IMP runtime calls need hardware verification against the new
  `encoder.<n>.rc` readback.

- **`videoN.i_bias_lvl` is wired on T31/C100** via
  `IMP_Encoder_SetChnQpIPDelta`, applied after `RegisterChn` the same way
  the QP bounds are (the 0a8bb9f pattern), non-fatal on rejection
  (`src/hal/hal_ingenic.c`). Only a non-default value is written, so the
  SDK's own `iIPDelta` stays untouched otherwise; the effective value is
  readable as `encoder.<n>.rc.ip_delta`. The classic `iBiasLvl` and the new
  `iIPDelta` are close relatives, not proven identical in sign/scale - the
  value is passed through 1:1 and should be checked against the rc readback
  on hardware before the mapping is trusted.

- **`videoN.bitrate`'s per-SoC meaning is documented instead of unified**
  (`timps.conf.example`). Classic path: the value lands in
  `maxBitRate`/`outBitRate`, a hard ceiling under vbr/smart. New API: it is
  `uTargetBitRate`, a target, while the real cap `uMaxBitRate` stays at the
  SDK default. Decision recorded in the file: NOT unified via
  `IMP_Encoder_SetChnBitRate` at bring-up, because that would change the
  fielded behaviour of every new-API camera on header-only knowledge - the
  new rc readback is the tool to measure first. Related decision on the
  other unreachable new-API attrs (`uMaxPSNR`, `uMaxBitRate`, `eRcOptions`,
  `uMaxPictureSize`): they become READABLE via `encoder.<n>.rc` but stay
  unwritable - `uMaxPSNR` is the knob `capped_quality` is named after, yet
  everything known about these fields is header-derived and unverified on
  hardware, so exposing setters would repeat the exact
  header-reads-convincingly failure the investigation documented twice.

- **The silent `smart` -> `capped_quality` substitution on the new API warns
  once** (`src/hal/hal_ingenic.c`). The classic path has always warned when
  it substitutes `capped_vbr`/`capped_quality` with `vbr`; the mirror-image
  substitution in the other direction ran without a log line, so a T31
  configured with `rc_mode = smart` gave no hint it was running
  capped_quality.

- **The new-API rc-knob warning now tells the truth per key**
  (`src/hal/hal_ingenic.c`). The 8f3c84c wording claimed
  `quality_lvl/change_pos/i_bias_lvl` "have no effect on this SoC", implying
  impossibility for all three. In fact only `quality_lvl`/`change_pos` (and
  now `fluc_lvl`) have no equivalent field in the new-API rc structs;
  `i_bias_lvl` has a runtime call that the T31/C100 SDKs ship (now wired,
  above) and the T40/T41 SDKs genuinely lack. The warning is split
  accordingly: "no equivalent field in this SoC's encoder API" vs "this
  SoC's SDK has no IMP_Encoder_SetChnQpIPDelta".

- **Three classic-SoC rate-control knobs are now config keys**
  (`videoN.quality_lvl` 0..7, `videoN.change_pos` 50..100,
  `videoN.i_bias_lvl` -3..3; `src/config.h`, `src/config.c`,
  `src/hal/hal_ingenic.c`, `src/control.c`). They were literals in the VBR/CBR
  attribute fills, duplicated across the H264, H265 and T23 sw-rotate paths.
  `qualityLvl` in particular was not cosmetic: the vendor SDK derives
  `minBitRate = bitrate * quality[lvl]` from it, so the hardcoded 2 imposed an
  invisible floor at 60% of the configured bitrate. Measured on a T23 with a
  static scene: 2091 kbit/s under cbr and 1715 under vbr, against 278 kbit/s
  for the same scene at a fixed qp - the encoder could always encode it
  cheaply, the floor would not let it. Defaults are the previous literals
  (2/80/0), so an untouched config produces the same stream as before.
  The ENC_NEW_API SoCs (T31/C100/T40/T41) have no equivalent fields and now
  warn once when these keys deviate from the defaults, instead of accepting
  them and doing nothing - the failure mode that hid the `min_qp`/`max_qp` gap
  until 0a8bb9f.

- **`POST /control` names the fields it ignored** (`src/control.c`,
  `src/control.h`, `src/mp4/httpd.c`, `scripts/timps-qa.sh`,
  `docs/wiki/HTTP-Control-API.md`): the reply gains an `ignored` array listing
  the field names the request carried that this build did not apply - a typo, a
  key from another section, a key gated out of this binary, or one with no
  `/control` write path. A body carrying ONLY unknown keys has always been
  visible (`422 unknown_fields`); a body mixing one good key with one typo
  answered `200 accepted:1` and dropped the typo without a word, which is the
  shape a real client actually produces. Nothing about what is applied changes
  and no count moves - `{"quality_lvl":7,"quality_level":5}` still applies the
  first key, it just no longer looks like a clean success. Built by walking the
  request body and testing each member against the SAME table+`F_CTRL` rule the
  apply path uses, so the two cannot drift; names are `ms_json_esc`-escaped
  (they are client data) and `ignored_truncated` flags a short list, the same
  contract as `applied`/`deferred_keys`. Scope is unknown fields inside known
  sections: an unknown top-level section, an out-of-range stream/item index and
  an object-valued member are not fields and are not listed.

- **`{bitrateN}` OSD placeholder** (`src/hal/osd_vars.c`, `src/hal/osd_vars.h`,
  `timps.conf.example`, `README.md`,
  `docs/wiki/Configuration-Reference.md`): the per-channel counterpart to
  `{fpsN}`, e.g. `osd1.1.text = {fps1} fps {bitrate1} kbit/s`. `{bitrate}`
  without a number reads `osd.monitor_stream` and therefore prints the SAME
  figure on every stream's overlay; `{bitrateN}` prints stream N's own
  measured encoder output. `hub_get_bitrate()` has taken a per-channel hub
  source since it was written - only the placeholder was missing, forgotten
  when `{fpsN}` was added. It inherits that getter's 2 s staleness guard, so
  an on-demand stream with no viewer shows 0 rather than a frozen rate.

### Changed

- **day/night: the config surface is consolidated, ten keys smaller**
  (`src/daynight.c`, `src/daynight.h`, `src/config.h`, `src/config.c`,
  `src/control.c`, `docs/wiki/Configuration-Reference.md`,
  `docs/wiki/Day-Night.md`, `docs/wiki/HTTP-Control-API.md`,
  `dev_notes/TODO.md`, `timps.conf.example`). Two different reasons, two
  different treatments:

  **`daynight.learn`/`daynight.state_path` are removed outright.** The
  learning subsystem recorded each confirmed day's lowest exposure reading
  and, with `learn=1`, let the median of the last 8 raise `day_gain` when the
  configured value turned out to be unreachable for a scene - the failure
  that had three cameras stuck in night on 2026-08-16. In practice its own
  safety clamp (never raise the threshold past `night_gain/2`) could not
  raise it far enough for the cameras that actually needed it: the live
  fleet measurements show `cam-sz`/`cam-wohn-ofen` need `day_gain` up
  around 2528-3238 while the clamp caps a raised value at `night_gain/2` =
  2048 under their current `night_gain`
  (`private/fleet/camera-fleet.md`, "Konsequenz für die Schwellwerte"). A
  mechanism that cannot fix its own motivating incident is not worth the
  config surface, the state file, or the daily "learned:" log line it cost
  to keep around. `daynight.diagnose_thresholds` covers the same failure
  mode today - it names the value to raise `day_gain` above instead of
  trying to raise it automatically, which is the honest version of what
  `learn` was attempting.

  **Eight more fields become fixed internal constants**: `probe_jump_pct`,
  `probe_settle_s`, `ref_delay_s`, `ir_ratio_night`, `ir_ratio_day`,
  `ir_min_headroom`, `boot_settle_s`, `transition_s` (now `DN_PROBE_JUMP_PCT`
  etc in `src/daynight.h`, the one place all eight live so `control.c`'s
  status JSON and `config.c`'s grace-period warning can't drift apart). Every
  one of these was already documented in code as camera-invariant: the
  `ir_ratio_*` pair is a dimensionless ratio (`r = D(illuminator off) /
  D(illuminator on)`) that "needs no per-camera calibration" by construction,
  re-derived from a twelve-camera dusk-to-dawn campaign where anything in
  1.8..2.2 gave identical verdicts across the whole night; the settle-time
  floors (`ref_delay_s`/`boot_settle_s`/`transition_s`) and the probe
  economy's own bar/settle time (`probe_jump_pct`/`probe_settle_s`) are AE
  and IR-LED physics, not per-installation tuning. A config key nobody ever
  needed to change per camera is not a config key.

  Both groups keep a config-file **grace period**: a `timps.conf` that still
  sets one of the ten old keys is parsed and the line is ignored rather than
  landing in the generic "unknown key" warning, so an old config does not
  read like it has a typo. `learn`/`state_path` warn unconditionally (the
  mechanism they controlled is gone); the eight hardcoded fields warn only
  when the configured value differs from the constant it became, since the
  constant's value equals the field's old default - a config that never
  touched one of these is silently unaffected, matching its default exactly.
  All eight remain visible read-only in `GET /control`'s `daynight` status
  object for diagnostics; they are simply no longer POST-able or
  config-file-tunable.

### Fixed

- **`hub_get_fps()` reports 0 for an idle stream instead of a frozen number**
  (`src/hub.c`, `src/hub.h`). `hub_get_bitrate()` has expired its measurement
  after 2 s since `eda8302` - "so an idle on-demand stream shows 0 rather than
  a frozen rate" - and its fps twin never got the same guard, so the two
  disagreed on the same OSD overlay: `{bitrate}` at 0 next to a `{fps}` still
  showing whatever the last closed 1 s window happened to hold. Since video
  encoding is on-demand (`MS_IDLE_STOP_US`), the window right before an
  idle-stop is exactly the atypical one to freeze. Affects `{fps}`/`{fpsN}` in
  the OSD and the `fps` field of the HTTP `/stats` payload, both of which now
  match their bitrate counterparts. **Not confirmed** as the explanation for
  the one field report that prompted this (`dev_notes/TODO.md`, an OSD reading
  13 instead of ~25 on cam-kinder-rechts); the check that would have decided it
  was never run, and the fps ceiling on those cameras turned out to have a
  driver-level cause. Applied because a frozen reading on an idle channel is
  wrong on its own terms.

- **`video<N>.qp` no longer claims a live apply it cannot make** on the
  new-API SoCs (`src/enc_caps.h`, `src/hal/hal_ingenic.c`, `src/control.h`,
  `scripts/timps-qa.sh`, `docs/wiki/Rate-Control-Parameters.md`,
  `docs/wiki/HTTP-Control-API.md`). Measured on cam-garage (T31X, substream in
  `fixqp`): a live POST is answered `deferred:0` and `encoder.<n>.rc.qp`
  echoes the new value, while the encoded bitstream does not move at all - the
  same QP pair applied at boot spans 6.4x. `IMP_Encoder_SetChnAttrRcMode`
  stores `attrFixQp.iInitialQP` where the next `Get` reads it back and never
  re-programs the running channel, so the readback is complicit and only a
  bitstream measurement can see it. `qp` is therefore out of `ENC_LIVE_KEYS`
  for T31/C100 and for T40, which shares that code path untested; it is graded
  restart-bound, which is what it always was. `caps.video_live` on T31/C100 is
  now `bitrate,min_qp,max_qp,i_bias_lvl` and T40 matches T41 exactly. Nothing
  about what gets applied changes - only what the reply promises.
  `IMP_Encoder_SetChnQp()` (T31 1.1.5+ / C100 headers only) is the candidate
  for a real live `qp` there and stays unwired until it is measured.

## [1.9.2] - 2026-08-21

### Added

- **A standing disagreement between the decided mode and the ISP is now
  reported - and deliberately never enforced** (`src/daynight.c`,
  `src/control.c`, `src/mp4/httpd.c`). The readback gate below covers one
  direction with a bounded window: "I commanded X and the ISP did not
  follow within 18 s." The field morning of 2026-08-21 produced the other
  direction by hand: while unsticking cam-wohn-ofen the operator drove the
  board script to day while timps stood on night, and afterwards nothing
  said that the two disagreed - the automaton would simply have flipped the
  ISP back at its next opportunity. That case must NOT be enforced: outside
  the gate window a mismatch may be exactly the manual override the
  operator just made, and forcing it back would clobber it (the same
  reasoning that keeps the re-assert divergence check WARN-only). What was
  missing is the report. The automaton now compares its decided mode
  against the ISP readback every tick it is idle (outside the verify
  window, after boot, both sides known); a mismatch standing
  `DN_DESYNC_MS` (20 s - well past any switch transient) earns ONE WARN
  naming both sides and the resolution path that worked in the field, a
  requested probe (`POST /control {"daynight":{"probe":1}}`, after which
  timps re-measured, decided day at r=0.98 and the two agreed). One WARN
  per episode, not one per tick; an INFO marks the episode's end when the
  two agree again, and only then can a new episode report. The debounced
  state rides along on `/control` and `/events` as `daynight.isp_desync`
  (-1 unknown, 0 in sync, 1 standing), so a dashboard can alert on it
  without log scraping.
  This also makes the second latch defect class from `hal_ingenic.c`
  visible: a value that DID latch and then silently REVERTED long after
  the gate's window closed (observed on cam-L 2026-08-09, flip reverting
  across a chn0 idle/active cycle). Such a revert now stands out as a
  mode mismatch within 20 s and is warned once - still not enforced,
  `fs_use()`'s chn0-edge re-apply remains the self-heal; what was missing
  was any line saying it happened. Corpus scenario
  `28-standing-desync-reported` (new harness key `isp_override`: the served
  ISP mode forced from outside, the operator's hand): one WARN with the
  probe hint, one INFO on agreement, zero board switches - the click budget
  of 0 is the proof that this reports rather than enforces.

### Fixed

- **A mode switch only counts once the ISP confirms it - and a stuck ISP is
  now unstuck by the one thing that acts on it, a real transition**
  (`src/daynight.c`, `scripts/dn-replay.py`). Field incident, cam-wohn
  (cinnado_d1_t31l, T31L), 2026-08-21: at 10:29:07 the automaton decided day
  ("IR ratio 0.97"), `switch_cmd` ran with rc=0, the board hook chain POSTed
  `running_mode=0`, both post-switch re-asserts fired - and
  `/proc/jz/isp/isp-m0` kept reporting "ISP Runing Mode : Night" until past
  11:00, half an hour of black-and-white video in broad daylight, with
  `/control` reporting day the whole time and not one warning anywhere.
  Root cause is structural: the machine kept three mode registers - its own
  decision (`cur`), the config (`image.running_mode`), and the hardware
  (the isp-m0 readback) - and only ever compared the first two (the
  re-assert divergence check, WARN-only). The one register that is ground
  truth was parsed by `dn_read()` every tick and thrown away; `dn_switch()`
  checked the script's exit code, which only proves the script ran, and the
  re-asserts pushed `SetISPRunningMode` with a value the driver already
  believed - which the latch defect class documented in `hal_ingenic.c`
  (T23/T31, rc=0 with chn0 idle, queued and never applied; or latched and
  silently reverted) turns into a no-op. Field repair confirmed the rule:
  `daynight night` then `daynight day` - a genuine edge - fixed it in 18 s;
  re-asserting the same value had done nothing for half an hour.
  The fix is a readback gate. Every commanded mode (each `dn_switch`, and
  the boot assert) arms a verify deadline of `DN_VERIFY_MS` (both re-asserts
  plus 2 s = 18 s, so the cheap existing chain always gets its full chance
  first). The gate confirms early and silently (DEBUG) the moment the
  readback matches. If the deadline expires unmatched, it forces exactly ONE
  transition through the counter mode and back (5 s hold), re-arming the
  verify; while the forced cycle is in flight all decisions and a pending
  probe verdict are held, because the optics are deliberately wrong. If even
  the forced cycle does not take, it gives up loudly (WARN naming the
  mismatch) until the next commanded mode change - one cycle is the cap
  because a second identical cycle has no new mechanism to offer, and a
  permanently stuck ISP must not turn the gate into a click generator.
  Cost accounting, since every filter movement is wear (see 2026-08-17):
  on a healthy camera the gate costs nothing - the readback matches within
  a tick or two and the confirmation is a DEBUG line. It spends 2 extra
  movements only on a camera that has already eaten a lost switch, where
  the alternative is unbounded hours of wrong mode (the heartbeat never
  catches this: the machine believes it is in the right mode, so no trigger
  is even armed - the day branch sat content at exposure 900 all morning).
  Worst case, an ISP that never follows: 2 extra movements per legitimate
  switch, bounded by the same dwell/gap config that bounds switches
  themselves, plus one WARN per occurrence.
  Relation to the railed-boot cycle (2026-08-21, scenario 24): same rule -
  "re-asserting a believed value is a no-op to the ISP, only a transition
  acts" - but disjoint detectors, so they cannot merge: the boot defect
  shows the CORRECT mode with railed tuning (caught by AE reserve == 0,
  readback matches throughout), this one shows the WRONG mode (caught by
  readback, reserve says nothing). The repair they share is the forced
  transition.
  Harness: new scenario key `isp_sticky` - switches away from the stuck mode
  are recorded but not rendered until one switch TO the stuck mode arrives
  (the driver-view edge), and the mode assertions then judge the rendered
  timeline instead of the switch log, which is exactly the pair that
  diverges. Corpus scenario `27-isp-does-not-follow`, built from the
  cam-wohn morning; pre-fix it reproduces the incident (1 switch, night for
  the rest of the run, 110 s past the wrong-mode bound), post-fix it passes
  with the exact field click count: 1 dropped + 2 for the cycle = 3.

## [1.9.1] - 2026-08-21

### Fixed

- **A silent-probe escalation actually reaches the audible probe now**
  (`src/daynight.c`). All three verdict branches that escalate ("no usable
  reading", "reserve unknown", "inconclusive between the thresholds") set
  `want_probe` - and the commit block then handed that straight back to the
  silent path, same tick, with `d_lit` freshly cleared: the next verdict read
  `r=-1`, fell into "no usable reading", and the loop silently probed forever
  without ever consulting the day pipeline it kept asking for. Never seen in
  the field or the corpus because the shipped thresholds
  (`ir_ratio_day == ir_ratio_night == 2.0`) make the inconclusive branch
  unreachable and the other two need a broken read - it surfaced the moment
  scenario 25 exercised the new unknown-reserve escalation (35 silent probes
  in 600 s, zero audible). Escalating branches now set the same
  one-tick `no_silent` latch the railed boot uses, so the audible probe fires
  that tick, subject to its own gap and dwell gates as ever.
- **The filter-cost projection verdict lowers the reference like every other
  night verdict** (`src/daynight.c`). The projection branch ("the room
  supplies the light, but day mode would read above `night_gain` - staying
  night", added with the `21-ir-ratio-flap` fix) is a proven night verdict at
  a level below the probe bar, and it left the reference standing. On a scene
  resting below the bar the jump trigger therefore re-armed and fired every
  `probe_confirm_s`, indefinitely - measured on cam-schuppen on the morning of
  2026-08-21: reference 5753, scene at 1040, `verdict=night` every 26 seconds,
  each one dimming the image for the 8 s settle. The verdicts themselves were
  CORRECT (day mode had been measured at 5.6x that same morning and re-learned
  steeper on a later flap; 1040 x cost was still above `night_gain`, so
  switching would only have bounced) - what was missing was the same lowering
  rule scenario 23 established for the `r >= ir_ratio_night` branch: a silent
  probe answering night below the bar is proof the reference describes a
  scene that no longer exists. It now lowers to the lit level there too, so
  each re-fire needs a further genuine halving. Also explains the "rising"
  reference (2337 -> 5753) that looked like a ratchet violation: it was not
  the ratchet but re-anchoring - every reverted day excursion discards the
  reference and (3a) re-anchors at the then-current night reading, AE
  transients included. Corpus scenario `26-projection-verdict-no-way-down`,
  built from the camera's own morning numbers.
- **A camera without gain ceilings says so, and stops calling everything
  night** (`src/daynight.c`). When the ISP dump lacks the `MAX SENSOR analog
  gain` / `MAX ISP digital gain` lines, the AE reserve is unknown (`hr=-1`).
  Two consequences were silent. First, the entire clip protection above is
  absent on such a camera - unknown must count as usable, or it would never
  anchor a reference at all - and nothing said so; an operator would have had
  to notice `hr=-1` in a DEBUG probe line and know what it implies. Now
  warned once per session (`dn_ceiling_check()`, sibling of the path-C
  blindness notice; three consecutive misses arm it so a torn /proc read
  cannot). Second, and worse: in the silent-probe verdict an unknown reserve
  fell into the `room < ir_min_headroom` branch (-1 is less than everything),
  i.e. was treated as *proof* of a meter pegged at the dark end. On a
  ceiling-less camera every low ratio therefore answered "staying night",
  the brightening trigger was swallowed on every fire, and the ratio path
  could never reach day - a structural stuck-night with a log line claiming
  evidence it did not have. Unknown reserve now escalates to the audible
  probe instead: r ~= 1 from a lit room and from a railed meter look
  identical, only the reserve separates them, and without it the day
  pipeline is the one honest judge left. That is exactly the pre-redesign
  behaviour for these cameras, at the pre-redesign cost. Fleet data shows
  the case is rarer than assumed - both T20s (jxf22/jxf23) do publish
  ceilings, with *different* ISP digital maxima (45 vs 32) on the same SoC,
  which also rules out ever hard-coding a fallback ceiling. Corpus scenario
  `25-no-ceilings-unknown-reserve`; the harness learned a `no_ceilings` key
  for it, because its synthetic camera always declared its limits and the
  whole point here is a camera that does not - the old code passes every
  other scenario and sits in stuck-night on this one.
- **A reading the automaton itself calls worthless can no longer enter its
  long-term memory** (`src/daynight.c`). Measured on a galayou_y4_t23n,
  22 seconds after boot: the T23 ISP comes up with 128 units of digital gain
  nobody asked for, the exposure index reads its rail (131072) with **0 units
  of AE reserve**, and the silent boot probe correctly diagnosed it - "the
  meter is pegged at the dark end" - then the automaton anchored its night
  reference on exactly that value 22 seconds later. From there the probe bar
  stood at 65536 and a twentyfold-too-dark night was declared normal. This is
  one instance of a class: the trustworthiness signal (`headroom`, the AE
  reserve) existed and was used for the *verdict*, but none of the places
  that persist a measured value beyond the current tick checked it. The rule
  now applied uniformly (`dn_clipped()`): a reading from a railed AE - the
  reserve is known and below `ir_min_headroom` - is a **clip, not a level**.
  It may still justify a verdict (a meter pegged at the dark end cannot
  happen in daylight, so it *is* night evidence), but it may not be
  remembered. Concretely gated: the post-entry reference anchor (waits,
  logging "night reference deferred", until the meter is off the rail; the
  configured absolute thresholds and the heartbeat carry the automaton
  meanwhile - the steady-state probe cadence of a permanently railed camera
  is unchanged, because the heartbeat deferral never depended on the
  reference being set); the revert ratchet (a probe that found night leaves
  the reference unset rather than anchoring a clipped pre-probe level); the
  `filter_cost` learning in the day branch (a clipped day reading understates
  the cost and re-opens the flap that factor closes); `probe_best` (else the
  threshold diagnostic advises raising `day_gain` above a clip); and the
  trend memory (a railed boot seeded `ema_slow` at the rail, and the repair
  then read as a dawn - one wasted probe per boot). The silent-probe verdict
  additionally records the AE reserve of the *lit* reading (`lit_hr` in the
  structured probe line): the same night's data shows `r=1.00` from a railed
  meter and `r=16.10` from the same scene once repaired - a ratio of two
  clips carries no information, so it now answers "staying night" instead of
  feeding any of the branches that compare it against thresholds. The
  reference-lowering path and the ratio-day verdict are downstream of that
  check and therefore only ever see a trusted lit level. NOT covered:
  platforms whose isp dump has no gain-ceiling fields report headroom as
  unknown, and unknown must count as usable or those cameras would never
  anchor at all - they keep the old exposure.
- **A railed boot is repaired by one real mode transition, not by
  re-assertion** (`src/daynight.c`). The 2026-08-20 fix pushed
  `image.running_mode` into the ISP at boot; measured tonight, that cannot
  work: the ISP already holds the persisted value, so writing the same value
  back is a no-op, and the camera still read the rail 25 minutes later. What
  demonstrably re-tunes the AE is a genuine transition through the other
  mode: `day` had the index moving within 12 s, `night` again read 5720
  within another 12 s - factor 23, the magnitude the original incident
  documented. So when boot finds the persisted mode NIGHT and the AE
  hard-railed (0 units of reserve - the state in which every reading is a
  clip and even the silent probe is structurally blind), the automaton now
  fires the audible boot probe directly, skipping the silent path that
  cannot answer there. If the scene is actually day, the probe confirms it
  and the single movement was the right one anyway; if it is night, the
  revert re-tunes the ISP and the reference then anchors from the first
  honest reading. This deliberately revisits the 2026-08-20 decision that a
  filter movement per boot is "a mechanical cost the evidence does not ask
  for": for a *healthy* boot that is still true and nothing changes
  (`boot_probe` semantics untouched, including `boot_probe=0`); for a railed
  boot the alternative is a camera that is blind all night, poisons its
  reference, and burns probes against a bar derived from a clip - one click
  per railed boot is the cheaper side of that trade, and it fires only in
  the state where the meter proves it cannot measure. A persisted-DAY boot
  needs no special case: its honest pipeline forces the night switch, which
  is already a real transition. Corpus scenario
  `24-pegged-boot-poisoned-reference` replays the full night: railed boot,
  cycle, deferred-then-honest anchor, and a light at t=600 that must still
  reach day - which the poisoned bar of 65536 would have deferred
  indefinitely.
- **The C++ runtime is actually linked statically now** (`Makefile`). The
  build carried a comment stating that SRT builds link the final binary with
  g++ "so libstdc++ is resolved and static-linked (-static-libstdc++, passed
  via IMPLIBS) reliably". `-static-libstdc++` appeared nowhere but in that
  comment: `IMPLIBS` was `-l:libimp.a -l:libalog.a -l:libsysutils.a`, and the
  firmware build overrides it in any case. Every SRT build therefore shipped
  `libstdc++.so.6` - 2130 KB in the target, the single largest file in a 5 MB
  rootfs, larger than `libimp.so`, the ISP driver or busybox, and more than
  three times `timpsd` itself. Its only other consumer was
  `libaudioProcess.so`, which nothing links and which was not mapped in any
  process on a running camera. `USE_SRT=1` now links with
  `-static-libstdc++ -static-libgcc`: `timpsd` grows 683744 -> 1278396 bytes
  and drops `libstdc++.so.6` from its `NEEDED` list, which with the shared
  library gone is a net 581632 bytes off the packed image. Do **not** extend
  this to `libgcc_s.so.1` - uClibc dlopens it for `pthread_cancel` unwinding
  with no `DT_NEEDED` entry, so a link-time dependency scan cannot see the
  requirement, and removing it aborts the daemon right after audio init with
  "libgcc_s.so.1 must be installed for pthread_cancel to work". Measurements
  and the full method are in `docs/flash-footprint-srt-2026-08-20.md`.
- **libsrt is built with section granularity** (firmware
  `package/libsrt/libsrt.mk`). timps compiled its own sources with
  `-ffunction-sections -fdata-sections` and linked with `--gc-sections`, but
  `libsrt.a` was built without them, so the linker could only discard whole
  object files while timps uses a small slice of libsrt's C API. Worth 28672
  bytes on the packed image at no functional cost.
- **A low IR ratio no longer switches to day on its own** (`src/daynight.c`).
  The silent probe answers "is the illuminator earning its keep". Switching to
  day also closes the IR-cut filter, which is a separate cost, and on a dim
  interior it is the binding one. Measured on a bedroom-class scene with one
  dimmed lamp: the probe reported `r = 1.27`, the automaton switched to day,
  the day pipeline read 11480 against a `night_gain` of 4096, the absolute
  rule sent it straight back, and the cycle repeated - eight audible round
  trips in one evening. The automaton now measures what the filter costs the
  first time a ratio verdict is undone that way (3.27x in that scene) and
  requires the projected day reading to clear `night_gain` before acting on a
  ratio again. One exploratory switch remains unavoidable; the repeats are
  gone. At real dawn the night reading falls and the switch happens by itself,
  with no special case. The factor is measured per scene rather than
  configured, and is re-measured after a restart rather than persisted.
  Corpus scenarios `21-ir-ratio-flap-cam-sz` and `22-ir-ratio-flap-t20`, both
  built from measured numbers, hold this to two switches where the unguarded
  automaton spent twelve.
  The factor may be **below** 1, and the first version of this guard rejected
  that as nonsense. Measured on a T20 whose illuminator contributes nothing
  (`r = 1.00`), the day pipeline read 6166 against a night level of 8171 - and
  still above `night_gain`, so the loop ran anyway while the guard never armed.
  The camera flapped 11 times in a day. What is learned is not what the filter
  costs but what day mode actually reads here, and that is meaningful in both
  directions. Twenty scenarios did not find this; one camera did, an hour
  after the first version shipped to it.
- **An illuminator command that does not work retires the silent probe**
  (`src/daynight.c`). Every failed attempt fell back to the audible IR-cut
  probe, so a board that cannot switch its LEDs separately paid a motor
  movement for each try - worse than never having tried. Two consecutive
  failures now retire the silent probe for the session, and the trend trigger
  goes with it, leaving the jump trigger and the heartbeat. That is the
  fallback the design describes and the code did not enforce; it only became
  visible once the probe shipped enabled.
- **The replay harness clears `daynight.irprobe_cmd` for scenarios that do
  not model an illuminator** (`scripts/dn-replay.py`). It set the key when a
  scenario had a `night_gain_noir` curve but relied on the compiled default
  being empty otherwise. Once that default became a real command, legacy
  scenarios armed the trend path with nothing to feed it: two of them lost
  their click budget to probes the scenario could not answer.

### Added

- **Per-module debug logging** (`src/log.c`, `general.debug_modules`). There
  was one global level, so reaching a single subsystem's debug lines meant
  raising everything - on a device whose syslog ring is 64 KB and recycles in
  minutes under load, which loses the very lines it was raised for. Names, not
  a bitmask: a mask in a config file quietly means something else the day a
  module is added, and nothing warns. `general.debug_modules = HAL_ING` is 10
  extra lines; the same reach used to cost the whole ring.
- **Every silent probe logs its measurement in columns** (`src/daynight.c`):
  `probe: r=1.05 lit=3350 dark=3520 hr=199 verdict=day mode=night ref=12098
  why=brightening`. `lit` and `dark` are the two readings the probe compares -
  with the illuminator and without - which is exactly what the 2026-08-17
  campaign collected by hand, except this runs wherever timps runs instead of
  needing a script installed on each camera. That campaign died silently when
  its script stopped being called, and nobody noticed until a morning needed
  explaining and had no curve. One line at the point where all five verdict
  branches meet, rather than a tail on each: they word themselves differently,
  and five places to keep in step is how a parser ends up knowing four.
- **The day/night switch line ends in a machine-readable tail**
  (`src/daynight.c`): `[mode=night exp=6182 ref=-1 bar=768]`. Everything in it
  is already in the prose, but the dashboards count these lines with a grep,
  and a reworded sentence would empty them without a word - the same silence
  the exposure series had when it stopped collecting.

### Fixed

- **`osd.*` globals no longer depend on where they sit in the JSON**
  (`src/control.c`). The walk stopped at the first `{`, so
  `{"osd":{"0":{..},"enabled":false}}` had its `enabled` thrown away and
  answered 200 - not accepted, not rejected. Descending into the items would
  be wrong (`osd_item` has its own `enabled`), so the globals are now read in
  the segments between the nested objects. Object order carries no meaning in
  JSON; our own WebUI happens to emit the globals first, and nobody else has
  to.
- **A POST says when it changed more than it could persist**
  (`src/control.c`, `src/mp4/httpd.c`): the response carries `not_persisted`.
  Past the 48-key persist list the values were applied and dropped from the
  file with only a LOGW, so the caller learned about it after the next reboot,
  by way of missing settings.
- **A renamed key replaces its old spelling in the config file**
  (`src/config.c`). `config_write_keys()` matched the exact string, so a file
  holding a pre-rename alias kept that line and got the canonical key appended
  instead. The loaded result was right - the later line wins - but the stale
  line stayed forever, and editing it by hand did nothing, which is a good way
  to lose an afternoon.
- **`osd.hinting` says so when the build cannot do it** (`src/config.c`). It
  parsed, clamped, persisted and echoed on every build, but the rasterizer's
  hinting pass is compiled out unless `USE_OSD_HINTING` - the only clue was
  that the text looked exactly as before. Warns once per session, like
  `max_gop`.
- **`general.*` is readable, and appears in `/control?fields=1`**
  (`src/config.c`, `src/control.c`). Making `debug_modules` POST-able exposed
  an invariant that had held across every table until then: `F_CTRL` and
  not-readable are mutually exclusive. Broken, the change detection can never
  fire, so every POST of the same value rewrites `/etc/timps.conf` with an
  fsync - the flash wear that detection exists to prevent - and the field-drift
  watchdog that QA section 8d diffs against was blind to the section.
- **The persisted mode is asserted into the ISP at boot** (`src/daynight.c`).
  Adopting a persisted mode set the automaton's own state and armed a probe,
  but never told the hardware anything: the re-assert that follows a switch
  was the only path that ever pushed `image.running_mode`. A T23 came up that
  way after every single boot with 128 units of ISP digital gain and an
  exposure index of 131072 where the scene was worth 6100 - twenty times too
  dark, and indistinguishable in the numbers from a genuinely dark night. It
  was only visible in the picture. Re-asserting the running mode is what
  repaired it; `switch_cmd` is deliberately **not** run, because the board was
  already right (illuminator on, ISP reporting Night) and a filter movement
  per boot is a mechanical cost the evidence does not ask for. Eight corpus
  scenarios failed their click budget when it did.
  The first version of this fix pushed `running_mode` the same way a switch
  does: once immediately, then armed the same repeating re-assert
  (`DN_REASSERT_COUNT`/`DN_REASSERT_MS`) a switch arms, to cover a lost
  fire-and-forget POST. At boot that repeat is wrong - the boot probe can
  decide within seconds, and the pending re-asserts then overwrote that fresh
  decision with the stale persisted value a second and third time, eight
  seconds apart. Measured on a camera in a living room: the switch to day was
  overwritten twice and the room stayed in night mode through daylight. Boot
  now pushes `running_mode` once, directly, and does not arm the repeat; a
  switch still arms its own.
- **The night reference can come down** (`src/daynight.c`). It only ever
  ratcheted upward, on proof from a day probe that came back dark, so a
  reference anchored while the ISP was misbehaving could never be corrected. A
  silent probe answering "night" at a level *below* the probe bar is the same
  kind of proof pointing the other way: the reference predicted a brightening
  worth spending a look on, and the look said night. The camera above then
  fired the jump trigger every fourteen seconds - seventeen probes in a row,
  each eight seconds with the illuminator off, which reads from outside as an
  IR lamp that keeps switching itself off. Corpus scenario
  `23-stale-reference-no-way-down`.
- **The exposure and probe logs roll over instead of ending the series**
  (`scripts/dn-isp-log.sh`, `scripts/dn-irprobe.sh`). Both capped their CSV
  at a line count meant to bound what the file costs an SD card, but exiting
  once the cap was hit stopped the measurement for good rather than bounding
  it - seven of twelve cameras had quietly stopped logging exposure after
  about 66 hours, and `dn-irprobe.sh` stopped after about eight nights. The
  cap sat above the syslog copy in both scripts, so hitting it silenced the
  central collection as well, which has no line limit of its own. Both now
  trim to the newest half and carry on. `dn-isp-probe.sh`, a frozen duplicate
  of the exposure logger that an install run would have overwritten the fix
  with, is gone; the installer reads the one canonical script.

### Added

- **A failed HAL init retries instead of ending the process** (`src/main.c`).
  Nothing supervises this daemon: `S95timps` starts it with
  `start-stop-daemon -b`, busybox init does not respawn, and no watchdog
  covers it - so `HAL init failed` meant the camera stayed dark until someone
  logged in. Observed once: after a restart the vendor ISP was still held four
  seconds after a *clean* teardown, past the five one-second attempts in the
  HAL. The init path unwinds fully on failure, so a retry costs only the wait;
  it backs off 5s -> 60s and stays there. A genuine misconfiguration now
  retries forever, which is log noise rather than harm, and recovers by itself
  once the config is fixed.
- **A shutdown says which way it ended** (`src/main.c`). `hard_exit()` writes
  one constant string with `write()` - the only thing safe in that context -
  and the clean path logs `teardown complete - exiting`. Until now both ended
  after "shutting down" with whatever subsystem happened to log last, so the
  guillotine and a normal exit were indistinguishable. Any change to the 3s
  alarm or the 5s HAL wait would have been guesswork without this.
- **`/control` can ask for a probe on demand** (`src/control.c`,
  `src/daynight.c`): `{"daynight":{"probe":1}}` arms one for the next tick.
  The probe previously only fired on the jump trigger, the trend or the
  heartbeat, so confirming that a camera could see daylight meant waiting up
  to `heartbeat_s` - which is what it took to find three cameras that had sat
  in night mode all afternoon. It is counted as a command, like
  `record.clip`, not as a setting. The request only ever arms the silent
  probe, never the audible one, and is rejected outright on a camera with no
  `daynight.irprobe_cmd` configured or whose silent probe has retired itself
  for the session - a reported rejection rather than a silent no-op, since
  otherwise "nothing happened" would be indistinguishable from "it ran and
  found nothing".

### Changed

- **`logcat-ship.sh` ships the event's own timestamp** (`scripts/`). It used
  `logcat` plain, which prints no time, so every line arrived stamped with the
  minute it was forwarded - up to 60s late, and in one measured case over two
  hours, because the whole buffer ships at once after a restart. `logcat -t`
  prefixes each line with its epoch time; that timestamp is now also the
  incremental marker, which replaces the line-count-plus-text scheme and
  survives a ring wrap, a truncation and a reboot on its own. The script got
  smaller doing it.

- **The silent probe ships enabled** (`src/config.c`): `daynight.irprobe_cmd`
  now defaults to `timps-irprobe`, so a camera gets the cheap probe without
  per-camera configuration. Boards that cannot drive the illuminator
  separately retire it themselves, see above.

### Documentation

- **The silent probe is documented** (`docs/wiki/Day-Night.md`,
  `docs/wiki/Configuration-Reference.md`, `timps.conf.example`).
  `irprobe_cmd`, `ir_ratio_night`, `ir_ratio_day` and `ir_min_headroom` were
  absent from both the reference table and the example config - tolerable
  while the feature shipped switched off, and not once it became the default.

## [1.9.0] - 2026-08-19

### Changed

- **RTSP: RTP payloads are sent straight out of the packet** (`src/rtsp/rtp.c`,
  `src/rtsp/rtsp.c`). The TCP-interleaved path built each packet by copying the
  payload into a scratch buffer; it now hands `sendmsg` three iovecs pointing at
  the header, the interleave prefix and the payload where it already lies. TLS
  keeps the copying path deliberately - its API has no scatter/gather - so the
  branch is explicit rather than accidental.
- **The video and JPEG threads share one packet-assembly loop**
  (`src/hal/hal_ingenic.c`, new `enc_assemble_packs()`). Both copied the vendor
  stream's packs identically apart from whether Annex-B start codes are wanted.
  The duplication mattered because only one copy carried the evidence for a
  libimp-version-specific assumption about the stream buffer not wrapping; the
  other made the same assumption silently. Costs 16 bytes of `.text`.
- **`build.sh` refuses to link against another SoC's vendor libraries**.
  `3rdparty/install/lib` is shared state: `deps` copies one SoC's libimp there,
  a later `deps` overwrites it, and `timps` linked against whatever was present.
  `deps` now stamps what it installed and `timps` refuses a mismatch. Between
  closely related SoCs the link succeeds and the mismatch only shows up as wrong
  behaviour on the camera.
- **One home for the helpers `record.c` and `timelapse.c` each had a copy of**
  (`src/util.c`, `src/util.h`). Security-relevant: the `..` path-traversal check
  existed twice, so a fix to one copy left the other behind.
- **day/night: redesigned around four independent paths and one rate limit**
  (`src/daynight.c` 2030 -> 1290 lines, `src/daynight_probe.h` (732 lines)
  deleted). Design: `dev_notes/DAYNIGHT_REDESIGN_2026-08-17.md`; evidence:
  `docs/wiki/Day-Night-Design-Notes.md` (kept, now marked historical).

  **The diagnosis.** The old machine had **one** path from night to day (the
  probe) and **nine rules rationing it** - backoff, failure ratchet, anchor
  override, passive-evidence skip, `probe_max_skip_s`, trend suspension,
  brightening hold, oscillation breaker, verify deadlines. Each was correct
  and each was bought with a real incident, but because they all gated the
  *same* path their failures multiplied instead of cancelling: the 2026-08-14
  camera rendered IR video in broad daylight for four hours while a service
  restart fixed it in ten seconds. The redesign's construction rule is that
  **a rationing rule may only be applied while the independent trigger it
  defers to is working** - and that is now a runtime predicate
  (`dn_c_sighted()`), not a design intention.

  **The metric changed, and this is the load-bearing part.** The decision now
  runs on an *exposure index*, `total_gain * integration_time /
  max_integration_time` (higher = darker), not on bare gain. `total_gain` has
  a hard floor at 256 (1.0x), so once the AE rails there any further
  brightening of the scene is **invisible** - which is why a camera resting at
  256-268 under its own IR (`a5dae07` cam-J, `14a1d61` cam-H)
  could never have its brightening detected no matter how the probe was
  scheduled, and why a good part of the rationing apparatus existed at all.
  The AE keeps shortening the exposure once the gain bottoms out, so the
  product carries the signal across the whole range. In a dark scene the
  integration time is railed at max and the index **equals** `total_gain`, so
  every threshold keeps its historic calibration and the whole scenario corpus
  keeps its meaning; when the integration-time fields are unreadable the index
  degrades to bare gain, i.e. to the old behaviour. Both fields come from the
  `isp_path` scrape, so that scrape is back on every decision tick - paid for
  by `interval_ms` 500 -> 2000.

  **The four paths.** (A) day->night is a plain measurement on the honest day
  pipeline, `night_gain` held for `day_confirm_s`. (C) night->day is *only*
  ever probe-mediated, asked for when the index falls below `probe_jump_pct`%
  of the night reference for `probe_confirm_s` - this is the path that carries
  cameras with no location data, which is most of a fleet, and it is what makes
  "light on -> colour in ~25 s" work without a special case. (B) a flat
  `heartbeat_s`/`heartbeat_max_s` probe, sensor-independent, the only bound on
  a wrong night - deliberately **not** a multiplying backoff, since a doubling
  interval is exactly how the previous design turned a bounded guarantee into
  an unbounded one. (D) at boot the persisted mode is measured rather than
  believed: persisted day costs nothing (we are already in the honest
  pipeline), persisted night costs one probe (`boot_probe`). That makes the
  design notes' *restart-equivalence* invariant literal at t=0 and replaces
  dead-zone adoption, both verify deadlines and the still-brightening
  extension outright.

  **One night reference instead of five.** `ref` is the level at which night
  was last *proven* - by entering it from day, or by a probe that found
  darkness. It never drifts. It replaces `night_baseline` (drifting),
  `smooth_tg`, `probe_fail_smooth`, `min_smooth_since_probe` and
  `brighten_ref`, and the failed-probe assignment *is* the entire ratchet. A
  reference anchored too high now costs one self-correcting probe instead of
  looping forever every 25 minutes (`a5dae07`); one anchored too low costs
  path C until the next heartbeat re-anchors it. The oscillation breaker is
  gone with no replacement: an IR-reflection feedback loop needs a threshold
  crossing to flip night->day, and no threshold can do that any more.

  **Calendar demoted to a scheduler.** `time`/`sun` no longer decide anything
  in `auto` mode - they can only pull the heartbeat in to the next sunrise.
  This is what keeps basements, windowless rooms and artificially lit spaces
  correct, and why a camera without location data loses scheduling sharpness
  rather than correctness. `daynight.mode` is now `auto`|`schedule`; the old
  `sensor`/`time`/`sun` tokens still parse.

  **Measured cost.** On the target architecture (`mipsel-linux-gcc -Os`, the
  `make target` flags): **22,636 -> 18,748 bytes of text, i.e. 3,888 saved
  (17%)**. Notably *less*
  than the ~12 KB estimated during design - the bulk of `.text` turns out to
  be log/format call sites and the sun math, which scale with the number of
  messages rather than with the complexity of the algorithm, and cutting
  further would mean cutting the log lines every one of the twelve incident
  diagnoses rested on. BSS is unchanged
  at 360 bytes and the thread locals drop from ~30 scalars plus three ring
  buffers to 15 scalars and none, so **there is no meaningful RAM saving here
  and the change should not be justified with one**. The win is the state
  space and the bounded click budget.

  **Validated against the corpus, which is what made the redesign admissible
  at all** - the design notes' "no rewrite" verdict was explicitly conditional
  on there being no executable oracle, and there is one now. All 15 scenarios
  pass. The behavioural assertions (`expected_mode`, `max_wrong_mode_s`,
  `max_switches`, `restart_equivalence_s`, `monotonicity`) survived the
  rewrite untouched; what had to be restated is documented per scenario, and
  falls into two kinds. Log assertions naming machinery that no longer exists
  were retargeted at the line carrying the same behaviour. And **two `mode_at`
  assertions were encoding the latency of the bug rather than a requirement**:
  `04-deadzone-boot` asserted NIGHT at t=200 because the old machine adopted a
  stale persisted night and could only correct itself when a verify deadline
  came due; `07-stale-day-boot` asserted DAY at t=200 for the mirror-image
  reason. The boot probe measures instead of adopting, so both are corrected
  within ~3 s, and both now assert that.

  The corpus also found three defects in the redesign before it shipped, which
  is the argument for having built it: (1) the heartbeat's "has anything
  happened" test was noise-naive in the first draft - `03-noisy-night`'s ±25%
  AGC jitter defeats a running minimum and `11-dim-lightson`'s permanent 12.5%
  step defeats a max/min range test, so the sustained-minimum tumbling window
  from the old design had to be carried over deliberately; (2) the "path C is
  blind on this camera" notice fired from only one of the two places a night
  reference is set, caught by `05-inverted-regime`; (3) the unreachable-
  threshold diagnostic, originally emitted from the daily learning line, would
  have left a misconfigured camera silent for 24 h - it now waits for three
  consecutive failed probes instead, which arrives in minutes and still cannot
  be produced by one unlucky measurement.

  Two scenarios are new and exist as a pair: `09-dawn-ramp` (unchanged input,
  no integration-time channel, so the metric degrades to bare gain and its
  night curve floors at 260) versus `15-dawn-ramp-exposure-index` (the same
  dawn with the integration-time channel). Same scene, same thresholds,
  same ground truth: **2926 s of wrong mode against 9 s**. That difference is
  the entire argument for changing the metric, written as an assertion instead
  of a claim.
- **day/night: default `total_gain_day_threshold` 300 -> 768, and
  `total_gain_night_threshold` 3000 -> 4096** (`src/config.c`, renamed to
  `daynight.day_gain` / `daynight.night_gain` by the redesign above; the old
  names remain as aliases). Both are now
  expressed as multiples of the [24.8] gain floor (256 = 1.0x), which is the
  scale the decision actually lives on: **day is confirmed when the day
  pipeline can hold the scene at <= 3x gain, night is entered when it needs
  > 16x.** 300 was 1.17x - it asserted "day only when the day pipeline needs
  essentially no gain", i.e. outdoor daylight, which indoors is simply wrong: a
  normally-lit room in daytime needs 2-3x, so `tg < total_gain_day_threshold`
  could never come true, every probe reverted, and the camera sat in night mode
  in daylight. Five of twelve fleet cameras needed a manual override on
  2026-08-16 (800..5000); the two diagnosed in detail measured 531 (2.07x) and
  2505 (9.79x) as the best their day pipelines managed. 3x is deliberately
  modest and does **not** pretend to fix the extreme end - it clears the
  marginal cases a default can reasonably cover (cam-E 2.07x, cam-D
  ~2.7x) and leaves genuinely dim rooms like cam-F (9.79x) as
  explicit per-camera overrides. Picking the fleet's lowest observed override
  (800) would be fitting to five samples; 3x is a statable premise about indoor
  light that happens to cover them. The night default moves with it to keep the
  hysteresis band sane - 768..4096 is 5.3x, against 300..3000's 10x: narrower,
  but still over two stops of dead-zone, and 16x is a defensible "colour is
  hopeless" point. **Every corpus scenario now pins the two thresholds
  explicitly at the historical 300/3000** (under their new names since the
  redesign, except scenario 01 which keeps the old spelling to cover the
  aliases), so
  each keeps reproducing its own incident rather than silently tracking a
  moving default - which is also what made this change safe to measure.

### Added
- **day/night: the TREND trigger (path T) - a dawn is a ramp, and the step
  trigger structurally cannot see one** (`src/daynight.c`, `DN_TREND_*`;
  decision and evidence: `dev_notes/DAYNIGHT_DECISION_2026-08-17.md`, section
  "What shipped - 2026-08-18"). Night now keeps two further EMAs of the
  exposure index - a fast one (tau 3 min) that follows the scene and a slow
  one (tau 60 min) that remembers it - and asks for a probe when `fast/slow`
  falls below **75 %** and holds there for `probe_confirm_s`. This is the
  trigger the 2026-08-17 decision note specified and the 2026-08-17
  implementation did not carry: the silent probe and its ratio verdict
  shipped, the trigger that was supposed to fire it did not, so for a day the
  fleet's mornings were found by the heartbeat or not at all.

  **Why the existing trigger cannot do it.** Path C fires when the reading
  drops below `probe_jump_pct` (50 %) of the level night was last *proven* at.
  Natural twilight does not do that for a long time: measured on
  `cam-C`, the dawn of 2026-08-18 fell 11839 -> 5312 over two hours, a
  factor of 2.23, and did not reach the 50 % bar until 07:31 - 67 minutes
  after sunrise. `ref` answers steps, `fast/slow` answers ramps, and neither
  subsumes the other, which is why `ref` and its proof-only ratchet **stay**
  (they are what makes the `0f5fc80` flap loop and the `b4a54f0` repeat-dip
  impossible, and a drifting 60-minute EMA is a memory, not a proof).

  **Measured, not guessed** (sweep over the fleet night of 2026-08-17/18: 12
  cameras, 181 camera-hours *actually in night mode*, `scripts/dn-trend-eval.py`):

  | tau fast/slow | bar | dawns found | false fires per camera-hour |
  |---|---|---|---|
  | 3 / 60 min | **75 %** | **10 of 12** | **0.22** |
  | 3 / 60 min | 70 % | 9 of 12 | 0.15 |
  | 3 / 15 min | 75 % | 8 of 12 | 0.19 (and 30-70 min later) |
  | 3 / 10 min | 75 % | 6 of 12 | 0.13 |

  Shorter memories fire later and find fewer, because they track the twilight
  instead of noticing it. The two cameras nobody finds are the permanently
  dark garage and one whose AE never leaves its rail - both heartbeat-carried
  by construction. A **third, medium (~10 min) constant** was proposed after
  that night and is **not** implemented: the event that motivated it (a
  bedroom light inside the ongoing dusk, a factor of 1.54) bottoms at 0.87
  against a 10-minute memory, still short of the bar, and the ~88 % bar it
  would take sits inside the +-25 % AGC noise band, doubles the false-fire
  rate to 0.44/camera-hour, and still finds fewer dawns than 3/60 alone.

  **Armed only where the probe is silent** (`daynight.irprobe_cmd` set). That
  is the affordability argument, not a portability detail: 0.22 false fires
  per camera-hour is a few seconds of dimmer image each when the illuminator
  can be switched on its own, and about 2.6 audible **motor movements** per
  12-hour night when it cannot - worse than the 2 clicks a day this design
  exists to reach. Boards without separately switchable LEDs keep jump plus
  heartbeat, i.e. exactly their previous behaviour.

  Costs three scalars (14 -> 17, still no ring buffers), two appended trace
  columns (`trend_fast`, `trend_slow`) and **432 bytes of `.text` on MIPS**
  (`mipsel-linux-gcc -Os`, 21,620 -> 22,052; `.bss` unchanged at 272), most of
  it the two new log call sites. New corpus scenario
  **20-dawn-trend-schuppen**, built on that measured dawn: the pre-change
  build spends **2570 virtual seconds in the wrong mode**, the post-change
  build **0**, for one audible click either way.
- **day/night: `daynight.learn` (default 0) - opt-in threshold learning with a
  daily log line either way.** Every confirmed day records its lowest exposure
  reading; the median of the last 8 says how bright this scene actually gets.
  With `learn=1` that median raises the effective `day_gain` when the
  configured value turns out to be unreachable for the room - the failure that
  had three cameras stuck in night on 2026-08-16, each needing an SSH session
  to diagnose - and persists to `daynight.state_path` (change-only, at most
  one write an hour, atomic rename; a state file that does not parse is
  discarded silently). Two rules keep it from becoming the next incident
  generator: it may only ever **raise** the threshold (a too-generous
  `day_gain` produces a false day, which the honest day->night measurement
  corrects within `day_confirm_s`, while a too-strict one makes day
  unconfirmable, which is the failure with no bound), and it is clamped below
  `night_gain/2` so the two thresholds cannot cross and oscillate.
  Deliberately **off by default**, but the values are collected and logged
  **once a day regardless**, so the numbers can be read off a running camera
  before deciding to switch it on. Deliberately *not* persisted: the night
  reference (re-anchored in 30 s anyway, and a stale one would disable the
  spontaneous trigger) and the mode (measured at boot, not believed).
- **day/night config keys** (see
  `dev_notes/DAYNIGHT_REDESIGN_2026-08-17.md` section 7.3 for the mapping):
  new `day_confirm_s`, `probe_min_gap_s`, `probe_jump_pct`, `probe_confirm_s`,
  `probe_settle_s`, `ref_delay_s`, `heartbeat_s`, `heartbeat_max_s`,
  `boot_probe`, `learn`, `state_path`. Retired, and now parsed with a
  one-line warning naming the replacement rather than a bare "unknown key":
  `day_gain_pct`, `baseline_delay_s`, `boot_settle_max_s`, `boot_stable_pct`,
  `night_reconfirm_s`, `probe_max_skip_s`, `threshold_low`, `threshold_high`,
  `hysteresis`. The brightness-percentage fallback is no longer a decision
  path at all - it was a second, cruder view of the same two ISP fields the
  exposure index now uses properly, and keeping it meant writing every
  decision rule twice; `dn_brightness()` survives only to feed the WebUI
  readout.
- **day/night: `exposure` in the `/control` and `/events` status object** -
  the value the decision actually runs on. Plot this rather than `total_gain`
  when diagnosing: they are identical in a dark scene and only `exposure` has
  range in a bright one. `night_baseline`/`day_trigger` keep their key names
  (existing photosensing pages bind to them) but now carry the proven night
  reference and the probe bar.
- **day/night: `daynight.diagnose_thresholds` (default 0)** - the unreachable-
  threshold warning added earlier this session is now **opt-in**. The condition
  it reports is a real misconfiguration and the line is worth having (it turned
  three 2026-08-16 incidents from an SSH session each into a one-line fix), but
  it is a WARN that repeats, forever, until a human edits the config: on a
  correctly-configured fleet that is pure log growth on flash-backed syslog,
  and on a misconfigured camera it makes the point many times over. Same
  reasoning as
  `daynight.trace_path` - a diagnostic you switch on when you are diagnosing.
  Unlike `trace_path` it is safe on the `/control` surface (it names no path
  and writes no file), so it can be toggled live without a restart. Corpus
  scenario 13 carries the key and asserts the message; new scenario 14 is its
  default-off twin and asserts the silence, so if the gate is ever removed 13
  keeps passing and only 14 notices. **Restated by the 2026-08-17 redesign:**
  a single failed probe cannot distinguish "the room is dark" from "the
  threshold is too strict", so the warning is now emitted from a day's worth
  of evidence (in the daily learning line) rather than per probe, and says
  which of the two it might be instead of assuming.
- **day/night: a warning when `total_gain_day_threshold` is set below anything
  the scene's day pipeline can produce** (`src/daynight.c`,
  `src/daynight_probe.h`; three live cameras on 2026-08-16 - cam-D in
  the morning, then cam-E 192.168.1.100 and cam-F
  192.168.1.100 the same afternoon). Observability only, no behaviour change.
  `total_gain_day_threshold` is a config bar rather than a derived one, so the
  generator-C guard never looked at it - but it fails identically. Set below
  what a room's day pipeline actually reads, `tg < total_gain_day_threshold`
  can never come true: every probe lands ambiguous, the verify expires, the
  camera reverts, and it spends one audible IR-cut pair per reconfirm interval
  sitting in night mode in daylight. Measured: cam-E's day pipeline ran
  700 -> 591 -> 531 against a threshold of 300 (best reading 1.77x the bar);
  cam-F's ran 3299 -> 2629 -> 2250 against 450 (5.0x). The machine's
  behaviour is correct in every case - the day pipeline is the trustworthy
  judge, it judged "not day", night is the recoverable side - but the revert
  logged only "unverified day, gain N" and never named the threshold that made
  it unverifiable, so all three diagnoses cost an SSH session and a hand-read
  of day-pipeline gains out of syslog. The daemon now tracks the best (lowest)
  day-pipeline reading of each day excursion and, at an unverified revert,
  warns with the number to change the config to. Gated on
  `DN_DAY_THR_UNREACHABLE` (1.5x) so a dawn ramp stays quiet - a ramp's best
  reading sits just above the threshold and crosses it a probe or two later,
  while a mis-set threshold is missed by a wide factor - and latched once per
  excursion so it cannot spam. Corpus 09/08/06 are the negative controls (zero
  emissions); new corpus scenario 13 is the positive one.

### Fixed

- **Shutdown left the stream threads running and tore their state down anyway**
  (`src/rtsp/rtsp.c`, `src/mp4/httpd.c`, `src/hub.c`, `src/util.c`). The stop
  paths only WAITED for their detached per-client threads - 500 ms for HTTP, 1 s
  for RTSP - and then freed what those threads were still using. Measured with a
  client attached: teardown took 20.5 s and the threads outlived it. A live
  client registry now wakes them, and `/snapshot.jpg`'s grab is reachable by it
  too; the same teardown is 26-30 ms. On a TLS build the freed context was one
  the threads were still reading from.
- **SRT: a blocked `srt_sendmsg2` outlived the shutdown drain** (`src/srt.c`).
  Client sockets are now closed before the drain rather than after it, so a
  sender parked in the SRT library cannot survive teardown.
- **fMP4: an init segment could ship with an empty codec configuration box**
  (`src/mp4/fmp4.c`). When SPS/PPS were not yet known the `avcC` came out empty
  and the segment was sent regardless, which a browser accepts and then renders
  as a permanently black stream with no error anywhere. The init segment now
  fails instead.
- **The QA harness attested tests it had never run** (`scripts/timps-qa.sh`).
  Its attestation check was one-directional, so a field the script never posted
  still counted as covered. It is now bidirectional, and what it writes to
  `timps.conf` is quoted.
- **SW rotate: the unbound rotate/encode thread processed the SENSOR frame rate,
  not `videoN.fps`, so everything downstream of it ran at double the configured
  rate** (`src/hal/hal_ingenic.c`, `sw_rot_thread`; measured on cam-H,
  T23/sc2336, libimp 1.3.0, profile
  `cinnado_d1_t23n_sc2336_atbm6012bx`, 2026-08-18). `fs_create()` set
  `outFrmRateNum` to the configured fps and `IMP_Encoder_YuvInit` was told the
  same (so SPS and container correctly advertised 15), but the framesource rate
  divider turns out to be honoured only for a **bound** consumer - not for the
  user-mode `IMP_FrameSource_GetFrame` consumer this unbound path uses. Same
  class of gap as the missing HW OSD / privacy / piggyback-JPEG here. With
  `video1 = 640x480@15 rotation=90` (eff 480x640) the stream delivered **29.37
  fps** while the bound channel 0 delivered its configured 25 (24.67 measured)
  at the same moment, and the daemon's own instrumentation now names the split
  outright: `framesource delivered 29.85 fps, encoded 14.97 fps (target 15)`.

  Four things rode on that. The NV12 transpose and the software encode ran at 2x
  (`sw_rot_thread` at **39.4%** of the lone core); CBR/VBR was parameterised for
  15 fps and fed 30, so the rotated stream ran **708 kbps against a configured
  384**; the safe envelope in `sw_rot_start` gates on the *configured* fps and
  therefore did not bound what the thread actually did; and delivery came in
  bursts under load.

  Fix: a cadence gate immediately after `GetFrame` and **before**
  `nv12_rotate90`, so a surplus frame costs one GetFrame/ReleaseFrame pair and
  nothing else - transpose, software encode, OSD composite, JPEG and publish all
  move to the configured rate. There is no SDK call to reach for instead: T23
  1.3.0 exports no `IMP_FrameSource_SetFrameRate`, and `SetChnAttr`'s
  `outFrmRateNum` is precisely the field already proven not to reach this
  consumer. The gate keys off the **capture** timestamp (so kept frames are
  evenly spaced in the media time the published pts is derived from, with
  `ms_now_us()` substituting when libimp leaves `timeStamp` at 0, as
  `pts_sanitize()` already does for the same field) and advances a **fixed
  grid** by exactly one period per kept frame, so the long-run rate is capped at
  exactly `videoN.fps` even when the source ratio is not an integer. A
  quarter-period tolerance absorbs capture jitter - without it a frame arriving
  a few hundred us early is dropped and its successor lands a full source
  interval late, halving the rate in bursts. Two re-anchors keep a bad clock
  from wedging the gate shut (a ts more than 1 s behind the deadline is a
  reset/wrap; a deadline still in the past after the advance means the source
  stalled). It is a ceiling and never a source of latency: if a future libimp
  honours the divider for this consumer, nothing arrives early and the gate
  simply stops dropping.

  **Verified on the hardware above**, A/B with two binaries built from an
  identical tree differing only in this gate, two RTSP clients (rotated ch1 +
  bound ch0), 65 s per run:

  | | before | after |
  |---|---|---|
  | ch1 delivered fps (configured 15) | 29.37 | **14.96** |
  | ch1 video bitrate (configured 384 kbps) | 708 kbps | **429 kbps** |
  | `sw_rot_thread` CPU | 39.39% | **20.21%** |
  | daemon total CPU | 61.84% | **41.52%** |
  | ch1 non-monotonic DTS (ffmpeg) | 8 | **0** |
  | ch0 control (configured 25 fps) | 24.67 fps / 2160 kbps | 24.70 fps / 2191 kbps |

  With **five** concurrent clients on the rotated stream: 29.09-29.10 fps each
  and 20 non-monotonic DTS across the five before, against 14.97 fps each and
  **zero** after (thread 39.78% -> 20.35%, daemon 66.50% -> 44.28%). The rotate
  thread's share halved exactly as predicted. Stream metadata is unchanged and
  now truthful (`480x640`, `avg_frame_rate=15/1`); a snapshot confirms the
  picture is still correctly rotated with the OSD text upright and anchored to
  the rotated frame's top edge (the compose-order guard at the YuvEncode call
  still holds); and a client reconnect after churn resumes at 14.98 fps with
  zero DTS warnings. NOT reproduced: the finding's ~14.6 non-monotonic DTS/s
  under 5 clients - this baseline showed only 0.03-0.10/s per client, so that
  particular burst magnitude was situational, though its direction (20 -> 0) is
  confirmed.
- **RTSP: a partially received control request had no deadline, so a
  byte-trickling client could hold a client slot for hours** (`src/rtsp/rtsp.c`;
  found while assessing an external review, 2026-08-18). The control loop's
  only bound was `SO_RCVTIMEO` (30 s, set once on accept) - but that timer
  limits a *single* `recv()` and re-arms on every byte that arrives. A client
  sending one byte every 29 s therefore never trips it and never completes a
  request: the 4 KB buffer fills after ~34 h, and eight such connections occupy
  all `RTSP_MAX_CLIENTS` slots for that long, locking out every legitimate
  client without sending a single malformed packet. `mp4/httpd.c` had closed
  exactly this hole with its 5 s `hdr_deadline`; RTSP had not, and none of the
  four prior reviews flagged it. Fix: `RTSP_REQ_TIMEOUT_US` (10 s) bounds how
  long an **incomplete** request may stay pending, measured from the arrival of
  its first byte and re-armed whenever a complete request is consumed. Idle
  time *between* complete requests is deliberately left unbounded - gaps are
  normal on a control connection, and a peer that goes fully silent is already
  covered by `SO_RCVTIMEO`. Verified on the host sim: a 1-byte-per-2 s client
  is dropped after 12 s with a logged reason, while a connection idling 15 s
  between two complete `OPTIONS` requests is unaffected.
- **digest auth: the client's `realm` was used as a hash input without being
  checked against the realm the server advertises** (`src/auth.c`, both
  `auth_rtsp_digest()` and `auth_http_digest()`; raised by an external review,
  2026-08-18). `realm` was parsed from the `Authorization` header and fed
  straight into HA1 = MD5(`user:realm:pass`), while the server's own
  `AUTH_REALM` ("timps") was only ever *sent* in the 401, never compared
  against. This was **not** exploitable: HA1 is derived from the cleartext
  password on every request, so a forged realm changes both sides of the
  comparison alike and the client still needs the password - and the
  per-session nonce check blocks replay independently. It was nonetheless the
  server's value to state rather than the client's to choose. Fix: reject any
  response whose `realm` is not `AUTH_REALM`. Legitimate clients echo what the
  401 offered, so this is compatible by construction; verified against the host
  sim (correct realm authenticates, `realm="evil"` with an otherwise valid
  digest is rejected with 401).
- **day/night: the unsatisfiable-bar guard checked a different number from the
  gate it guards, so it stayed silent on the one camera it was written for**
  (`src/daynight_probe.h`, `src/daynight.c`; live incident cam-D
  192.168.1.100, 2026-08-16 ~10:03-11:28; design-notes generator C). The
  brightening hold compares `smooth_tg` against
  `baseline * (100+day_gain_pct)/200 * DN_BRIGHTEN_MARGIN`.
  `dn_bar_check()` was handed that expression **without** the margin. At
  `baseline = 326` the nominal bar is 260.8 - over the 256 sensor floor, so the
  guard said nothing - while the operative gate is 252.98, under it, so the
  sustained-brightening path was structurally dead. The camera sat in night
  mode in daylight for an hour with **zero diagnostic output**, and was
  recovered by hand by raising `total_gain_day_threshold` to 700 for that
  camera. The silent dead band is exactly `baseline` in [320.0, 329.9), and the
  guard fired correctly on either side of it (at 315 after a restart the
  nominal bar is 252 and it warned) - which is why this read as a camera quirk
  rather than an off-by-one multiplication. Fix: `dn_hold_gate()`, one
  definition of the value the hold actually compares against, called by the
  schedule *and* by both guard call sites - the arithmetic error was only
  possible because the guard re-derived the bar itself and so was free to drift
  from it. The log line now names the operative quantity ("brightening probe
  gate"). Refusing to plant an unsatisfiable baseline outright was considered
  and rejected with a measured reason - `night_baseline <= 0` makes the skip
  gate's `can_judge` false, so every periodic reconfirm would fire a physical
  probe, which on exactly the near-floor cameras this would target
  (cam-J and the closet, resting 256-268) re-creates the audible-clicking
  complaint the skip gate exists to fix; see the design notes.

- **day/night: `min_smooth_since_probe` was a running minimum, which defeated
  the reconfirm backoff entirely on a static scene** (`src/daynight_probe.h`,
  `src/daynight.c`; regression in the same-day fix below). A running minimum is
  an **order statistic** and, uniquely in this file, has no noise rejection at
  all: its expected depth grows without bound in the number of samples, so on a
  perfectly static dark scene it descends until it crosses the 0.97 anchor bar
  and suspends the backoff. The sample count is set by the reconfirm interval,
  which is precisely what the backoff lengthens, so the error is
  self-reinforcing in the wrong direction - longer backoff, more samples,
  deeper spurious minimum, suspension fires, backoff defeated. Measured on a
  static 3500 gain with realistic stochastic AGC noise and a real 3600s
  `night_reconfirm_s`: an audible IR-cut probe **every hour, all night**, 7
  board switches against a budget of 4, on a camera watching a room where
  nothing changed - and emitting log lines structurally identical to the ones
  corpus scenario 10 asserts as proof the fix works. Fix: a reading only enters
  the minimum once the gain has held there for `DN_BRIGHTEN_CONFIRM_MS`,
  implemented as a tumbling window whose *maximum* is what becomes eligible, so
  a value counts only if the gain stayed at or below it for the whole window.
  O(1) state, no ring buffer, same debounce shape and same constant the hold
  already uses. Durability is unchanged, so property 3 says exactly what it
  said before. A 5s AGC trough can no longer latch anything; corpus 10's dip
  spans ten windows. Note the sensitivity floor this implies and which the
  previous behaviour only appeared to beat: a dip must now be deeper than the
  scene's own noise band to count, so the real cam-H 23:21 dip (1599 vs
  a 1653 anchor, 3.3%) would not latch under realistic AGC noise. That is the
  correct trade - the alternative is no working backoff on any camera - and the
  periodic reconfirm still bounds recovery at one `night_reconfirm_s`.

### Changed
- **replay harness: the noise model is stochastic** (`scripts/dn-replay.py`).
  `noise_factor()` was a bounded sum of two sinusoids. A deterministic periodic
  signal has a fixed, finite set of extremes, so any running minimum over it
  converges after one period and then stops moving: **any defect whose
  behaviour depends on a statistic of a window of samples was structurally
  inexpressible**, at any `noise_pct`, for any duration. That is how eleven
  green scenarios reported zero click regressions while the reconfirm backoff
  was inoperative. Replaced with seeded gaussian noise (sigma = `noise_pct`/2)
  plus an AR(1) term for the short autocorrelation real AGC hunting has -
  genuinely stochastic so window statistics drift as they do in the field,
  seeded so a corpus failure can still be bisected. New optional `noise_seed`
  scenario key. Scenarios 11 and 12 carry a realistic 10-12%; scenario 10 stays
  at 3% and says why (the frozen-reference band it discriminates is at most
  `DN_HOLD_REF_LEAD` = 6% wide by construction, so it cannot both demonstrate
  that mechanism and carry fleet-level noise - scenario 12 is the noise
  scenario).
- **replay corpus: new scenario 12, the null hypothesis**
  (`scripts/dn-scenarios/12-static-night-null.json`). A static dark scene,
  stochastic noise, real 3600s reconfirm, four hours, asserting a **click
  floor** plus a `forbid_log` on the suspension line. Every other scenario
  asserts that *changed* evidence is eventually acted on; not one asserted the
  converse, and that gap shipped a bug. This is the only scenario in the corpus
  that fails when the machine becomes too *eager*, which is the direction every
  fix in this subsystem's record pushes.

- **day/night: turning a room light on took up to a full `night_reconfirm_s`
  to be noticed, instead of the ~30s the sustained-brightening path exists to
  deliver** (`src/daynight_probe.h`, `src/daynight.c`; cam-H
  2026-08-16, the third gate; design-notes generator D). With the two fixes
  below the mode did recover - but through the periodic reconfirm, i.e. up to
  an hour on a real camera's 3600s default. The fast path never fired, and the
  reason is generator D in its fastest-acting form yet:
  the hold's bar is `(100+day_gain_pct)/200` of the **live** `night_baseline`,
  and `night_baseline` drifts toward `smooth_tg`. So the instant a light comes
  on, the bar starts converging on the very reading it exists to detect. The
  bar is only 22.4% away and `DN_BASELINE_ALPHA` closes 22.4% in about **25s**,
  against a `DN_BRIGHTEN_CONFIRM_MS` of **30s** - the debounce loses that race
  by construction, for every event whose size is comparable to the bar itself.
  Traced tick by tick on the replay: a 23% light-on left the margin test 2.2%
  short at the instant the step completed and *falling* from there, so it never
  opened on any tick; 25s later the bar had dropped straight *through*
  `smooth_tg`, which takes the `>= bar` branch - and that branch re-arms and
  clears the hold. A permanently brighter room therefore read as "nothing has
  changed" from then on. Not merely slow: **erased**.
  Fix: `brighten_ref`, a frozen snapshot of the baseline taken while the scene
  is still at rest, which the hold's bar is derived from instead. Three details
  are load-bearing and each is pinned by a named property-test case:
  * It freezes at the 3% `DN_BRIGHTEN_MARGIN` band, not at the bar. Freezing
    at the bar is far too late - the bar is 22.4% away, so by the time the
    scene reaches it the baseline has been chasing for tens of seconds;
    measured, that late freeze captured 2353 where the true pre-event rest
    level was 2397, and 1.8% of contamination was itself enough to keep the
    margin shut.
  * The reference is the **max** of the frozen value and the live baseline.
    Without the max a stale-low reference hands a brighter scene a *lower* bar
    than a dimmer one gets - brighter evidence buying a later probe, the exact
    shape of `0f5fc80`/`14a1d61`. The property test finds it immediately: 228
    monotonicity violations.
  * The freeze is released once it leads the live baseline by more than
    `DN_HOLD_REF_LEAD` (6%). Frozen indefinitely it reintroduces `fad4f40`'s
    pre-dawn probe volley - the bar stands still, a dawn ramp walks into it,
    the probe fails, the baseline replants lower, the reference re-freezes
    there, repeat. Corpus scenario 09 caught that on the first run: six
    sustained-brightening pairs down one ramp, 14 board switches against a
    budget of 9. The release is the step-vs-ramp discriminator and the two
    numbers separate cleanly - a step contaminates the reference by 2.3%, a
    ramp the baseline can track separates them by 10-15%. Expressed as a
    divergence rather than a timer, which it is otherwise equivalent to (the
    drift rate is fixed, so "how far has it led" and "how long has it been
    frozen" are the same measurement), so `dn_next_probe()` still reads no
    clock but `now_ms`.
  Measured on corpus scenario 10: day at **t=2685, 35s** after the light-on
  completes, against t=3089 (489s) without it; the gate-reverted build fails
  `mode@2750` and runs 353s of wrong mode. Nothing about the debounce is
  relaxed - the arming edge, the 30s confirm, the failure ratchet, the
  `transition_s` dwell and the oscillation breaker are untouched, so a passing
  headlight is rejected exactly as before, and a named property case asserts
  that a scene returning to its resting level discards an in-progress hold
  rather than banking it. What the fix does *not* claim is recorded too: a
  fast, large light-on from a settled dark room (27% over 10s) was already
  caught before this change - the freeze **widens** the catchable band rather
  than creating it, and the band it adds is exactly where real room lights sit.
  New corpus scenario 11 pins the other side, a light-on too small for the
  fast path (12.5%), which must still recover via the reconfirm inside one
  `night_reconfirm_s` and must *not* fire the hold (`forbid_log`).

- **day/night: the camera did not react to a room light being switched on**
  (`src/daynight_probe.h`, `src/daynight.c`; live test, cam-H
  2026-08-16; design-notes generators E and the newly named F). Someone turned
  a room light off and back on in front of a deployed camera while the raw
  sensor register was polled over SSH. The sensor saw both edges perfectly -
  analog gain 85 -> 100 and back to 86, each reached in ~5 s and then held
  flat, which is `total_gain` 1614 -> 2233 -> 1649 (gain registers are log2,
  32 units per stop, so a 15-unit move is a **38%** `total_gain` move, not a
  15% one). The decision logic did not react to either edge in any way: no
  baseline-drift line, no brightening hold, no backoff suspension, no probe -
  and, traced through the schedule afterwards, would not have done for another
  eleven hours, when `probe_max_skip_s` came due. Two independent defects, both
  in `dn_next_probe()`:
  1. **The trend suspension forgot evidence it had already been shown.**
     `dn_trend_falling()` compared the *instantaneous* smoothed gain against
     the frozen `probe_fail_smooth` anchor, so it un-fired as readily as it
     fired. On the incident night it suspended an x4 backoff at 23:21:08 (gain
     1599 against a 1653 anchor) and pulled the reconfirm in from 03:12 to
     00:12 - and then the room dimmed on its own, the predicate went false,
     and the four hours came straight back, long before the deadline it had
     pulled in. The scene had been measured brighter than confirmed night; the
     schedule simply did not keep it. Fix: the evidence struct carries
     `min_smooth_since_probe`, the lowest smoothed gain since that anchor was
     frozen, and both `dn_trend_falling()` and the skip gate's ratchet-anchor
     override read it instead of the current tick. "The scene has been
     measurably brighter than confirmed night" is now a fact about the
     interval rather than about this instant, which is what the sentence always
     meant. Still a measurement rather than scheduler state, so
     `dn_next_probe()` stays pure and generator D's frozen-anchor rule still
     holds.
  2. **The failure ratchet borrowed a threshold calibrated for something
     else.** Its bar was `day_gain_pct`% (60%) of the anchor - but 60% is the
     day/night *pipeline* discriminator, 0.74 stops, sized for IR-cut
     transitions that move `total_gain` by orders of magnitude. The ratchet
     asks a different question ("is this new evidence, distinguishable from
     the evidence that already failed?"), which is a noise question. Cost,
     measured: the anchor latched at 1653 - simply where that room rests at
     night, which is where a failed reconfirm always latches it - so the bar
     demanded 992, i.e. analog gain 62 from a room that lives between 85 and
     100 all night. The bar sat below the entire scene's nightly range, so the
     sustained-brightening path was dead until dawn for any indoor-light-sized
     event. `dn_bar_reachable()` could not catch it: 992 clears the 256 sensor
     floor comfortably. Fix: a dedicated `DN_RATCHET_MARGIN`, one quarter stop
     (0.84), ~5x the 3% `DN_BRIGHTEN_MARGIN` noise bar and far outside the
     jitter in the fleet traces, but well inside what a light switch does. The
     incidents the ratchet exists for are all *same-level* re-fires (the
     pre-dawn tangent re-cross at 4898 against a 4906 bar; the cam-J
     dawn dip returning to the ~820 it was latched at) and remain blocked.
  Verified end to end by new replay-corpus scenario 10
  (`scripts/dn-scenarios/10-roomlight-20260816.json`), built against a copy of
  the current tree with the fix hunks reverted: pre-fix the machine never
  leaves night inside the run (4 board switches, longest wrong-mode run
  3100 s, `probe_max_skip_s` left at its 43200 default so the backstop cannot
  mask it); post-fix it recovers 489 s after the light comes on, inside one
  `night_reconfirm_s`. All nine pre-existing scenarios keep their **exact**
  pre-fix board-switch counts, including 03-noisy-night at 25% noise, so
  neither change costs a click. `tests/dn-probe-props.c` gains
  `assert_dip_monotone()` - the sequence property the old snapshot-only
  properties could not state ("evidence once measured is never worth less
  later") - plus the two `cam-H` cases by name; the sweep is unchanged
  at ~2.0 M assertions, 0 violations.

- **day/night: the reconfirm backoff kept stretching the probe interval while
  the scene was measurably getting brighter** (`src/daynight_probe.h`,
  `src/daynight.c`; design-notes generator E). A failed probe doubles the
  periodic-reconfirm interval, x1 -> x2 -> x4, on the premise that the
  darkness it just measured is confirmed and there is no point clunking the
  IR-cut hourly. That premise is a LEVEL test with no notion of direction, so
  it survived evidence contradicting it: on cam-L 2026-08-14 the backoff
  hit its x4 cap at 05:58 *while the gain was falling through three orders of
  magnitude*, and that cap is what turned a bad revert into a four-hour
  wrong-mode window; on a dim outbuilding 2026-08-13 the same cap put the recovering
  probe 4 h out while the baseline chased a day-level gain down. Fix: while
  the smoothed gain sits below `DN_BRIGHTEN_MARGIN` (97%) of
  `probe_fail_smooth` - the gain frozen at the instant a physical probe last
  *measured* genuine night, the same frozen anchor 14a1d61 introduced - the
  multiplier is SUSPENDED and the deadline falls back to one plain
  `night_reconfirm_s` after it was armed. Deliberately a suspension, not a
  retry ladder (design notes section 7: do not add escalation timers): it can
  only ever SHORTEN a deadline, never below `night_reconfirm_s`, so it cannot
  manufacture an extra probe beyond the un-backed-off schedule - it restores
  exactly the bound `night_reconfirm_s` was introduced (b3eec71) to provide.
  It costs **zero** additional IR-cut clicks in the case the backoff exists
  for: an unchanging dark scene sits at or above its own `probe_fail_smooth`,
  the trend test is false, and the multiplier applies unchanged. (Amended
  2026-08-16, when the test moved to a running minimum: the anchor is a single
  sample of a noisy signal while a minimum over a whole night is an
  extreme-value statistic, so a static-but-noisy scene *can* eventually hold
  the suspension on. The bound that survives is the one above - never more
  often than the un-backed-off schedule - and the measured cost on the corpus
  is still zero clicks.) Measured on
  the replay corpus (scenario 08, one 900 s reconfirm interval): recovery at
  t=1276 s instead of t=3976 s, longest wrong-mode run 174 s instead of
  2426 s, same number of board switches.

- **day/night: an unverified day was reverted to night mid-dawn, because the
  verification judged a gain LEVEL at one instant and was blind to the
  direction that level was travelling** (`src/daynight.c`; live incident
  cam-L, T23/SC2336, 2026-08-14). An unverified day - adopted at boot, or
  landed on by a reconfirm probe whose day-pipeline reading fell inside the
  dead-zone - is re-read once at its deadline and reverted to night if the
  reading is still ambiguous. That rule cannot tell the scene it exists to
  protect against (a camera booted after dark on a stale persisted day,
  rendering black) from a scene in free fall through the dead-zone toward
  daylight, and dawn is exactly the second one. Live: four probes down the
  dawn ramp read day-pipeline gain 9024 -> 4813 -> 2425 -> 708, the last two
  landing in the dead-zone and reverting five minutes later at 1436 and 452 -
  each a 36-41% FALL since the reading that armed the deadline, i.e.
  unmistakably dawn. Each revert is additionally accounted as a failed probe,
  so it doubled the reconfirm backoff and latched the brightening ratchet
  lower; the second one latched it at 315, putting its bar (189) below the
  sensor's own night-pipeline gain floor (~256 = 1.0x) where no reading can
  ever satisfy it. With the brightening path dead and the backoff at its x4
  cap, the next periodic reconfirm was 4 h out and the camera rendered IR-mode
  video in broad daylight from 06:20 until a manual service restart at 08:07 -
  a restart that re-decided from scratch and read *day* immediately off the
  very same gain (257), i.e. the machine was holding a belief a cold start
  contradicts instantly. Fix: at the deadline, revert only once the
  improvement has STOPPED. If the metric has moved at least
  `DN_DAY_VERIFY_FALL` (10%) better than the reading that armed the deadline,
  re-arm for another `min(night_reconfirm_s, 300 s)` and re-anchor on the new
  reading instead. It can only ever DELAY a revert - it never causes a switch
  and costs zero IR-cut clicks (strictly fewer than before: the incident's
  second probe pair never happens at all); a darkening scene cannot buy an
  extension, and a genuinely dark one rails above
  `total_gain_night_threshold`, which is tested first and reverts within the
  ordinary hysteresis regardless of any deadline. The anchor ratchets down on
  every extension, so noise cannot sustain it, and the rule is
  self-terminating rather than merely bounded: each extension moves the metric
  >=10% closer to the day threshold, at most ~22 of them from the top of the
  dead-zone at the default thresholds. Mirrored into the brightness-fallback
  path. Verified in `timpsd-sim` against both shapes: a dawn ramp through the
  dead-zone (2383 -> 1387 -> 807 -> 450 -> 279) now extends three times and
  confirms day with **zero** board switches, while a static dead-zone reading
  still reverts to night on the first deadline exactly as before.
- **day/night: periodic reconfirm's skip gate kept re-arming instead of
  probing while a failure ratchet was outstanding, because "solidly night"
  was judged against a baseline the same failed probe was busy dragging
  down** (`src/daynight.c`; live incident, a dim outbuilding T31/SC2336, 2026-08-13).
  A sustained-brightening probe failed at gain 284, latching
  `probe_fail_smooth=284`; over the next 2.5 h the camera was visibly
  daytime (gain 257-266) but the periodic reconfirm's skip gate kept
  silently re-arming instead of firing a real probe, because
  `night_baseline` drifts toward the current (wrongly-classified) gain
  every tick, unconditionally - two independent "verify before trusting"
  mechanisms (the ratchet and the passive-evidence skip) ended up
  validating each other instead of either being the other's escape hatch.
  Only a manual service restart (replanting the baseline from scratch)
  recovered it; the 12 h `probe_max_skip_s` backstop was the sole
  remaining net, well inside its bound but a poor substitute for the
  hourly-scale reconfirm that should have caught this. Fix: while a
  ratchet is outstanding, also require gain to not have moved meaningfully
  brighter than `probe_fail_smooth` - a frozen snapshot from the moment a
  probe last physically checked and found genuine night, immune to the
  baseline's ongoing chase. Reuses `DN_BRIGHTEN_MARGIN`, no new tunable; a
  flat/dark scene still sits at or above its own `probe_fail_smooth` and
  keeps skipping exactly as before - only genuine further brightening past
  the last-checked point loses the skip.
- **day/night: switching felt sluggish - the sustained-brightening confirm
  halved, 60 s -> 30 s** (`src/daynight.c`, `DN_BRIGHTEN_CONFIRM_MS`).
  Since b4a54f0 removed the direct adaptive night->day switch, this hold
  *is* the entire night->day latency for the everyday "a light came on"
  case: the smoothed gain reaches the probe bar within ~2-11 s of the
  step, so the 60 s confirm was ~85% of the wait. The hold does two jobs
  and only one is load-bearing at this duration: transient rejection (a
  30-60 s brightening now buys one probe pair where it bought none,
  bounded by the same ratchet to at most one pair per night entry, same
  as at 60 s) and letting `smooth_tg` converge to the dip floor before the
  ratchet latches on it - the EMA residual is 0.9^N, so 30 s = 60 ticks is
  99.8% converged (on the cam-J dawn numbers the ratchet latches at
  838 instead of ~820, a 2% shift in a bar that sits at 60% of it).
  Deliberately unchanged: `night_reconfirm_s`, which governs only the
  worst-case self-heal backstop, not everyday switching.
- **day/night: dawn-time flip pairs surviving the adaptive baseline fix
  below, because the direct night->day switch was still reachable AFTER
  the baseline plants** (`src/daynight.c`; observed live on cam-J
  the morning after deploying the fix above: resting night gain a stable
  10856 for hours, then dawn dipped it to ~820 - under the adaptive day
  trigger 6514 - which fired the direct switch after just 5 s hysteresis,
  preempting the sustained-brightening probe 4 s into its 60 s confirm.
  The day pipeline then read gain 8192 (still dark) and reverted; because
  both flips were genuine (non-probe) the revert took the branch that
  RESETS the failure ratchet instead of latching it, so the identical pair
  repeated on every dawn fluctuation - 5 audible IR-cut pairs in one
  morning, each >60 s apart so the oscillation breaker never tripped). The
  night/IR pipeline is structurally unreliable here regardless of baseline
  freshness: with the IR-cut open the sensor sees visible+NIR, so a level
  reading "day" through it can read solidly "night" through the closed-
  IR-cut day pipeline. Fix: in the adaptive regime (`0 < day_gain_pct <
  100`) night->day is now exclusively probe-mediated, before *and* after
  the baseline plants - routing dawn through the 60 s brightening hold
  lets `smooth_tg` converge to the true dip floor, so a failed probe
  latches `probe_fail_smooth` at that floor and the ratchet then requires
  a strictly deeper dip to re-fire, so the same dawn fluctuation can't
  repeat (at most one audible probe pair per dawn, zero if the dip is
  under 60 s). `day_gain_pct<=0` keeps the legacy fixed-threshold direct
  switch; `day_gain_pct>=100` (where the brightening probe is gated off)
  keeps the old post-baseline direct switch so that edge doesn't lose its
  night->day path. Cost: adaptive night->day latency goes from 5 s to the
  probe's 60 s confirm - the same price the pre-baseline window and the
  marginal brightening band already pay.
- **day/night: perpetual flip loop + oscillation-breaker freeze cycle on
  cameras whose IR-lit night gain is at/below the static day threshold**
  (`src/daynight.c`; observed live on cam-J, T20/jxf22, in a
  genuinely dark room with very strong IR return - resting night gain
  ~256-268 vs the default `total_gain_day_threshold` 300, breaker firing
  every ~25 min indefinitely, latterly 13 s flip cycles at every 600 s
  freeze expiry). Three compounding causes, three fixes:
  1. the adaptive night **baseline was planted mid-AE-descent** (fixed 30 s
     delay; the log showed baselines of 10856/5148 while the gain was still
     collapsing toward ~800), putting the derived day trigger above the
     resting level - the plant is now additionally gated on the existing
     `dn_ae_stable()` ring, bounded by `baseline_delay_s +
     boot_settle_max_s` (and `boot_stable_pct=0` keeps the old behaviour);
  2. `dn_day_trigger()` **floored the adaptive trigger at the static day
     threshold even when that floor sat ABOVE the measured baseline**,
     which is a perpetual false "day" verdict - the floor now applies only
     while it is below the baseline; in the inverted regime the trigger
     stays purely adaptive and day detection is owned by the probe
     machinery (the 2026-08-02 too-low-baseline concern the floor was
     built for is covered by exactly those probes);
  3. before the baseline exists, a night-pipeline gain under the static
     threshold is ambiguous between "lights on" and "strong IR return in
     darkness" - the **full switch in that window is replaced by a probe**
     (real light sticks in day within seconds; darkness reverts cheaply,
     outside the oscillation-breaker's flip count, and the
     `probe_fail_smooth` ratchet blocks an identical re-fire on the next
     night entry), so the loop terminates after at most one probe pair.
  NOT a state-machine timer bug: the suspicious "confirming 60s one second
  after the baseline" log line marks the *start* of the 60 s brightening
  hold (target duration), not its completion, and every brighten/probe/
  smooth state is correctly reset on each switch - the flips came from the
  trigger paths above. The breaker itself worked as designed; its
  "IR-reflection" wording is its generic hypothesis text. Physical note:
  gain ~1x under IR in a dark room means the sensor receives a LOT of IR -
  after this camera's recent disassembly/reassembly, internal reflection
  (lens smudge, baffle/IR-cut seating) is the likely physical amplifier
  and worth a hands-on check, but the state machine must not loop on it
  regardless. `day_gain_pct=0` disables all three changes (legacy plain
  thresholds).
- **Follow-up to the RTCP SR clock fix below: the SR anchor wobbled by the
  fanqueue latency, re-timing ffmpeg's RTCP-driven timeline once per SR**
  (`src/rtsp/rtsp.c` anchor call sites, `src/hub.c`): the first version of
  the fix paired each sent packet's media timestamp with the sender loop's
  ITERATION-TOP clock read. On a healthy session those coincide, but during
  a TCP-backpressure drain the packets being sent were captured hundreds of
  ms earlier, so each SR sent mid-drain advertised a differently-shifted
  NTP<->RTP mapping. ffmpeg re-times the stream on every SR: the result was
  "Non-monotonic DTS" warning waves on the plain TCP audio+video path -
  one overshot packet, then real timestamps climbing back to the bogus
  peak - with wave amplitude matching the capture's max delivery gap
  (rtcpfix-camC: 4 warnings; rtcpfix-camA: 17, waves of ~370 ms
  against a 0.43 s max gap; this path had been warning-free on every prior
  run because the OLD bug's error was near-constant and receivers absorb a
  constant offset silently). The anchor now uses the packet's hub publish
  stamp (`p->enq_us`, stamped unconditionally in hub_publish/_take now -
  one clock read per published frame per SOURCE, ~25-40/s, not per
  subscriber), which is the instant the media timestamp was actually
  current, immune to how long the packet later sat in the queue. A/B on
  timpsd-sim with induced reader stalls (SIGSTOP bursts): pre-fix binary
  reproduces the Non-monotonic warnings, fixed binary is clean, the benign
  one-per-session "Timestamps are unset" line is unchanged in both.
- **RTCP SR paired its NTP timestamp with an RTP timestamp computed on the
  wrong clock** (`src/rtsp/rtp.c` `rtcp_wr_sr()`; regression shipped in
  v1.8.5, 365162d): the stale-pairing fix was right to re-sample NTP fresh,
  but its RTP side (`now_us - t->pts0`) assumed hub pts values live on
  `ms_now_us()`'s clock. They do not - `hal_ingenic` publishes
  `pts_sanitize()` output (which leads/lags the monotonic clock while the
  sanitizer slews, perpetually on sensors whose real fps differs from the
  configured one, e.g. cam-L's 25.42 vs 25), and `hal_sim` publishes
  g_epoch-relative values. The SR then contradicted the media timestamps by
  exactly that offset; ffmpeg's RTCP NTP-sync path (active whenever
  audio+video are both SETUP) rebased the stream timeline once the SRs
  arrived and invalidated already-queued video AUs to NOPTS. With audio
  muxed alongside that is survivable, but a video-only (`-an`) matroska
  `-c copy` client dies fatally ("Can't write packet with unknown
  timestamp") when a NOPTS AU lands after the first cluster opened - QA
  13b's "isolation not holding" FAIL: both healthy clients aborted within
  ~1 s, 4/4 deterministic across two cam-L runs, while the stalled client
  was innocent (T31/T20 boards with well-behaved pts passed the same
  phase). The SR now extrapolates from the last sent packet's media
  timestamp paired with the sender loop's monotonic stamp
  (`rtp_sr_anchor()`, both subtractions within a single clock each), so it
  is fresh AND consistent with the media timeline, across send stalls too.
  Reproduced and verified against `timpsd-sim` (pre-fix: ffmpeg reported a
  video start_time equal to host uptime, 41756 s; post-fix: sane 0-based
  timelines, and 2 healthy `-an` clients ran a full 30 s beside a
  deaf/stalled interleaved client with zero errors). The single benign
  "Timestamps are unset for stream 0" first-AU warning that predates
  v1.8.5 remains - it is inherent ffmpeg receiver behavior for any
  two-stream RTSP server and harmless. +29 bytes `.text` (sim build).
- **`videoN.gop` ran at DOUBLE the configured value on every new-API SoC**
  (`src/hal/hal_ingenic.c`, `enc_create()` `ENC_NEW_API` path - T31, C100,
  T40, T41): the keyframe interval was accepted, clamped, persisted and
  faithfully echoed by `/control`, while the encoder quietly used `2 * gop`.
  `IMP_Encoder_SetDefaultParam()`'s `uMaxSameSenceCnt` argument is not a
  "same scene" hint - every vendored new-API `imp_encoder.h` documents it as
  `GOPLength = uGopLength * uMaxSameSenceCnt, Default is 2`, so passing the
  documented default 2 alongside `uGopLength = v->gop` doubled it. Measured on
  a T31 (`cinnado_d1_t31l_sc2336`, `video1.gop=50` @ 25 fps): IDRs landed
  exactly 4.000 s apart (100 frames) instead of 2.000 s, on both the main and
  the sub stream (the path is shared by every stream). Now passes 1 and
  re-asserts `gopAttr.uGopLength` / `gopAttr.uMaxSameSenceCnt` explicitly, so
  the effective GOP no longer depends on how a given libimp build folds the
  argument into the struct. Classic-API SoCs (T10..T30) were never affected -
  they set `rcAttr.maxGop = v->gop` directly. **Note:** cameras will now emit
  twice as many keyframes as they did before, i.e. the delivered bitrate rises
  toward the configured target; that is the configured behaviour, but raise
  `videoN.gop` if you were unknowingly relying on the doubled interval.
  Found by the new `--test-encoder` QA section (v1.8.5), +8 bytes `.text`.

- **fMP4 clients could receive corrupted mid-GOP video after a queue
  eviction** (`src/mp4/httpd.c`, `stream_mp4()`): the adaptive-drop resync
  only triggered on an evicted *keyframe* or on the slot-count high-water
  mark, but a fanqueue can evict without tripping either - the `FQ_MAX_BYTES`
  byte budget binds below `MS_MP4_DROP_HIWAT` slots during a bitrate spike
  (2 MB / 48 slots = ~43 KB/frame, an ordinary 1080p motion burst), and an
  eviction hole shorter than one GOP need not contain a keyframe. The client
  then resumed on mid-GOP P-frames referencing AUs it never received - the
  silent-corruption case adaptive drop was built to prevent, and the same
  defect rtsp.c already heals via `fanqueue_take_dropped()`. Observed in the
  cam-L (T23, weak atbm6062 WiFi) QA capture 2026-08-11: three ~1.4-1.6 s
  eviction holes each resuming on a non-keyframe. Any eviction now arms the
  adaptive freeze-until-keyframe (and, with `http.adaptive_drop=0`, a
  rate-limited IDR request, mirroring rtsp.c). Sim binary size unchanged
  (193616 -> 193616 bytes).

- **day/night: silent decision/ISP desync when the board hook chain fails**
  (`src/daynight.c`, re-assert block): timps by design never writes
  `image.running_mode` itself - the board `switch_cmd` script backgrounds the
  `color` script, which curls a POST back to `/control` (fire-and-forget,
  output discarded). A lost/failed POST left `daynight.mode` and the ISP
  silently disagreeing until the next transition, with nothing in the logs.
  The post-switch re-assert (8 s after a switch, long past the ~1 s hook
  latency measured on real T20+T31 boards) now WARNs when `running_mode`
  still contradicts the switched mode, naming the hook chain. Deliberately
  WARN-only: re-running `switch_cmd` there would clobber a legitimate manual
  override, which the re-assert intentionally honours. Combined `.text`
  delta for this + the fMP4 eviction fix above: +471 bytes (sim build).

### Testing
- **Scenario `16-shed-light-measured` asserted a capability it deliberately
  withholds, and so failed on every build.** It demanded a switch to day on a
  measured lit-shed event, but supplies no `night_gain_noir` curve - so the
  harness leaves `daynight.irprobe_cmd` unset and the machine's only route out
  of night is the audible probe judged against the absolute `day_gain`. The
  measured day pipeline reads ~3700 against a 768 threshold, so that judgement
  can only ever come back "night". That is a scenario defect, not a daemon
  defect, and it was failing *before* the trend work as well. It is now the
  **negative control** of the pair 16/19 on the same measured event: without a
  third optical state, staying night is the only verdict the evidence
  supports, and the machine must reach it quietly (2 switches, the boot
  verification, and `probe confirmed day` forbidden). Scenario 19 hands the
  same event a `night_gain_noir` curve and gets the opposite, correct answer
  out of the same code - which is the whole value of the ratio, and is only
  legible because 16 keeps the third curve away.
- **`scripts/dn-replay.py`: a scenario that threw before its first assertion
  reported `RESULT: PASS`.** `emit()` runs from a `finally`, and the
  `aborted` flag only covered the virtual-clock handshake (which raises
  `SystemExit`); anything else - a sim binary that could not be spawned, for
  instance - printed a green scenario with no assertions under it. A run with
  zero checks is now reported as `ABORTED` and fails. Related: the trace
  parsers tested `len(p) == 10` for a row, so appending a diagnostic column to
  `daynight.trace_path` would have turned every trace into "empty" and every
  monotonicity check into a vacuous pass; both now test `>=`.
- **`scripts/dn-trend-eval.py` measured half a verdict.** It reported the
  false-fire floor of the two-EMA detector but never whether it *detects*
  anything - and a threshold of zero has a perfect false-fire rate. It also
  counted fires while the camera was in day mode, which the daemon never does,
  and defaulted to `--slow 15`, a value the design had already rejected. It
  now models path T as shipped (same `dt/tau` coefficient as
  `dn_ema_alpha()`, same 3/60 constants, same 75% bar), counts fires only over
  night-mode samples, and reports dawn detections beside the false-fire rate.
- **`dn_next_probe()` is now property-tested** (`tests/dn-probe-props.c`,
  `make dn-props`, run as corpus entry `00` by
  `scripts/dn-replay.py --all`). Collapsing the probe schedule into one pure
  function over one typed evidence struct makes the counterfactual reachable
  - "what would this build have done had the scene been slightly brighter at
  that instant" - which is what every stuck-mode incident in this subsystem
  turned on and which a replay can never ask, because a running machine only
  visits the evidence its own decisions produce. Two properties over ~217 k
  evidence points / ~1.0 M assertions per run, exhaustive rather than random
  so it is reproducible: **monotonicity** (brighter evidence must never buy a
  *later* correction - violated by `0f5fc80`, `14a1d61` and the 2026-08-14
  incident alike) and the **reconfirm bound** (once the frozen anchor says
  the scene is brighter than the night a probe actually measured, the
  correction may not land later than one un-backed-off `night_reconfirm_s`
  after the deadline was armed - monotonicity alone is satisfied by a
  schedule that is uniformly four hours late). Runs in well under a second,
  needs no HAL, no config and no threads.
- **The replay corpus is now shown to discriminate, not assumed to.** Each of
  the two scenarios whose fix is a *decision* rule rather than a new log line
  was re-run against a build with exactly that rule disabled, and must fail
  it on BEHAVIOURAL assertions: `08-baseline-chase-14a1d61` without the
  ratchet-anchor override never leaves night (wrong-mode 4450 s vs the 900 s
  bound) and without the trend suspension recovers only at the x4 deadline
  (2426 s); `09-dawn-ramp-20260814` without 19dcd74's still-brightening
  extension holds night for 3518 s past the point the day pipeline reads
  daylight. Scenario 08's bounds are tightened from backoff-cap-aware
  (3800 s) to the design notes' actual invariant, T = one `night_reconfirm_s`
  (900 s), now that the backoff is no longer trend-blind.
- **`scripts/dn-replay.py` refuses to run a sim binary that ignores the
  virtual clock** (`SimRun.check_clock`, plus the scale banner the sim now
  prints at startup). A tree older than `e06bf41` has no `MS_CLOCK_SCALE`
  hook, silently drops `SIM_CFLAGS` and runs in REAL time while the harness
  keeps feeding the scenario 60x faster than the machine experiences it: no
  deadline is ever reached, the incident cascade cannot occur, and the run
  comes back green on every behavioural assertion. That false negative reads
  exactly like "this scenario does not discriminate" and has already been
  mistaken for it once. The handshake is now a hard error naming the correct
  way to build a negative control - revert the fix's hunk in a copy of the
  CURRENT tree, which keeps the clock/trace contract while isolating the
  behaviour under test.
- `scripts/timps-qa.sh` **13b (`--test-hostile`)**: the verdict now
  distinguishes healthy-client DEATH from STARVATION - a fatal ffmpeg muxer
  abort zeroes the frame counts exactly like starvation, but means the
  server delivered data with a broken timestamp and the client gave up. The
  2026-08-11 cam-L FAIL read "fell to 0.0 fps because ONE client stopped
  reading" while both clients had aborted in ~1 s on the RTCP SR regression
  above; a WARN naming the abort (and pointing at timestamp anomalies
  first) now precedes the fps verdicts whenever the client logs show one.
- `scripts/timps-qa.sh` `analyze_stream()`: the ffmpeg-warning verdict for
  **UDP** captures now separates `RTP: missed N packets` lines (inherent
  transport loss - UDP RTSP has no retransmission by design) from real
  decode/timestamp warnings, which keep the strict zero-tolerance ladder on
  every transport. Loss is judged as a RATE against the estimated datagram
  count (capture bytes / the 1200-byte default `rtsp.mtu`; undercounting
  small audio packets errs strict): <=0.5% -> WARN naming it ordinary
  residual WiFi loss, above -> still FAIL (genuinely degraded path).
  Grounded in the cam-A 2026-08-11 run: 8 single-packet, evenly scattered
  losses over ~6100 datagrams (0.13%) with every other metric clean and the
  TCP capture from the same run at zero warnings - TCP retransmit masks the
  identical underlying loss, UDP correctly surfaces it; a sender burst/
  pacing defect would instead lose multi-packet runs clustered at IDR
  bursts. Previously any 4th lost packet in 30s hard-FAILed the run.
- `scripts/timps-qa.sh` **8h (`--test-daynight`)**: the first two hardware
  runs (T31, a dim outbuilding + T20, cam-K, 2026-08-11) each produced one FAIL -
  seemingly "night direction broken, day fine" on both - and BOTH were test
  artifacts, differently caused:
  - *cam-K*: the camera sat in genuine night when the test began
    (`control.json` at run start: `daynight.mode=1`), so the forced-night
    phase confirmed an already-night camera vacuously - no transition, no
    hook run - and the hook-count check FAILed a working chain. 8h now
    reads the LIVE starting state and, when already night, forces a fresh
    DAY state first so both directions are real edges; the hook-count
    baselines are taken after that precondition switch.
  - *a dim outbuilding*: the `image.running_mode` read-back was a single-shot read
    taken as little as ~0.2 s after `daynight.mode` flipped, racing the
    asynchronous board hook chain (~0.3-1.5 s when idle, more under QA
    load, up to curl's 5 s timeout legitimately) - a coin-flip false FAIL.
    Both directions now poll `running_mode` for up to 10 s before ruling,
    report the observed latency on PASS, and the FAIL text explains that
    timps never writes `running_mode` itself and points at the on-camera
    chain (`switch_cmd` script, `thingino.json` `daynight.controls.color`,
    `/usr/sbin/color`). The day-direction verdict is also no longer
    vacuous: if night never reached `running_mode=1`, the day phase's
    0-read is reported as "not a real edge, no verdict" instead of PASS.
  Also fixed: the hook-count delta now requires the *baseline* logread
  samples too (an empty baseline plus stale history could fabricate an
  invocation), and the restore no longer posts empty `time_night_start`/
  `time_day_start` strings for cameras that had none (the server ignores
  empty, which silently left the TEST windows persisted - visible in both
  runs' post-reboot thread banners).
- `scripts/timps-qa.sh` `av_skew()` + the A/V-skew verdict in
  `analyze_stream()`: a transport dropout hole overlapping the 2 s warmup
  boundary could fake a seconds-large "A/V skew". The start reference was
  each track's *own* first post-warmup packet, so the two references could
  land on opposite sides of a delivery hole - packets that were never
  contemporaneous. Observed on cam-L 2026-08-11: a lone keyframe at 2.060 s
  inside a startup dropout vs audio resuming at 3.558 s read as
  `start=1.498s end=0.014s drift=-1.484s` -> a false `[FAIL] out of sync /
  growing` on a stream whose tracks were locked (0.014 s) wherever both
  flowed; the real finding (fanqueue eviction dropouts on a weak link) was
  already correctly WARNed by the max-gap checks. The start reference is now
  the first post-warmup *audio* packet paired with the *video packet nearest
  to it in time* (end/drift stay last-minus-last, so genuine rate divergence
  is still fully visible), and the verdict distinguishes the two shapes:
  PTS are wall-locked, so dropouts shrink neither track's span while real
  divergence opens a span difference of about the drift's size - a large
  drift with agreeing spans plus a >1 s max-gap now downgrades to a WARN
  naming the dropout instead of a bogus out-of-sync FAIL. Healthy-stream
  values are unchanged (verified against the same run's three RTSP captures).
- `scripts/timps-qa.sh` **8g (`--test-encoder`)**: the bitrate check now
  measures what the substream actually delivers *before* changing anything and
  then aims the new target *below* that. A bitrate target is a ceiling, so
  raising it cannot move a stream that is already quality- or content-limited
  (`videoN.min_qp` floor on a static scene) - the old "is the measured value
  closer to the new number or the old one?" heuristic hard-FAILED a perfectly
  healthy T31 encoder for that reason. A lowered ceiling always binds, so the
  verdict is now sharp in both directions; the un-measurable upward fallback
  can only WARN. The cbr-vs-vbr variance comparison gained the same guard.
- `scripts/timps-qa.sh` **13b (`--test-hostile`)**: the isolation verdict now
  compares the healthy clients against the baseline phase measured moments
  earlier instead of against nominal fps. Isolation is a differential
  question; comparing to nominal re-reported section 3's "this SoC does not
  sustain its configured fps" finding as a bogus isolation FAIL (observed:
  baseline 18.2 fps -> 18.1 fps with a stalled client attached, i.e. isolation
  was holding).

## [1.8.5] - 2026-08-10

### Fixed
- **RTCP Sender Report pairs a stale RTP timestamp with a fresh NTP
  timestamp** (`src/rtsp/rtp.c`): players that reconstruct wall-clock PTS from
  RTCP SR data (ffmpeg, mpv) could see the video/audio timeline jump backward
  or forward - a universal burst on every fresh connection, plus recurring
  jumps roughly every 15-25s during playback, worse on cameras with large or
  frequent IDR frames (noisy night scenes especially). Root cause: a 2026-08-07
  perf change (single `ms_now_us()` read per `stream_loop()` iteration) made
  the RTP-timestamp side of the SR use a `now_us` value that could be seconds
  stale by the time it was written - the AU send earlier in that same
  iteration can block for a while under TCP backpressure on a large IDR -
  while the SR's NTP field was still sampled fresh. `rtp_maybe_sr()` now
  re-samples the monotonic clock immediately before writing the SR, once one
  is actually due, so both halves of the pairing are sampled back-to-back
  again. The server's own RTP timestamps were never wrong; only the SR's
  internal NTP<->RTP mapping was. Root-caused via a live report of mpv
  repeatedly self-pausing, confirmed with raw TCP-interleaved packet capture,
  independently reviewed before shipping.

### Testing
- `scripts/timps-qa.sh`: fixed a QA-script bug that let the above regression
  through every prior QA/soak run undetected - the "no ffmpeg decode/timestamp
  warnings" check grepped for `non-monotonous`, but ffmpeg's actual muxer
  wording is `Non-monotonic` - the pattern never matched, so this whole class
  of warning was silently invisible in every `analyze_stream` call (stream
  integrity, fMP4, SRT, and the long-running soak section all reuse it).

## [1.8.4] - 2026-08-09

### Fixed
- **hflip/vflip/running_mode reverting mid-run, no reboot involved**
  (`hal_ingenic.c`): a camera could silently flip back to unflipped (or the
  wrong day/night mode) hours after boot, with `/control` still reporting the
  correct config the whole time - only a live re-POST of the same value (or a
  full daemon restart) fixed the image, since nothing had actually changed the
  config. Observed live on a T23/sc2336 board after a normal idle-viewer
  reconnect cycle (chn0 going idle when no one is watching, then re-enabling
  for a later viewer) with no crash or reboot in between. `fs_use()` now
  self-heals: every genuine framesource-chn0 enable edge re-applies
  hflip/vflip/running_mode, and a running_mode change now also re-asserts
  hflip/vflip as a belt-and-braces measure in case a day/night switch alone
  can trigger the same reset mid-stream. The existing boot-time and
  `/control`-POST latch kicks (v1.8.2/v1.8.3) remain for the separate
  "apply while chn0 is genuinely idle" case; this closes the "value already
  applied but silently lost" gap they didn't cover.

## [1.8.3] - 2026-08-09

### Fixed
- **Audio idle-resume A/V skew** (`hal_ingenic.c`): unlike the video encoder,
  the audio input is never stopped while idle - it keeps capturing into its
  FIFO the whole time. Draining that stale backlog on resume could ratchet
  the v1.8.2 audio pts sanitizer ahead of real time with no way back, showing
  up as several seconds of A/V skew on a fresh `/stream.mp4` connection
  (RTSP was unaffected). Fixed by flushing the stale AI backlog on
  idle-to-active resume (mirroring the video encoder's own idle teardown)
  and hardening the pts sanitizer's fallback so it can never run ahead of
  the wall clock.
- **hflip/vflip not taking effect at boot** (`hal_ingenic.c`, T23): a camera
  configured with `image.hflip=1`/`image.vflip=1` could boot with the image
  unflipped despite the correct config, only "fixing itself" after a live
  `/control` toggle. Root cause: like `image.running_mode` (fixed earlier),
  the ISP only latches a pending hflip/vflip change while framesource
  channel 0 is actively delivering frames - and on the on-demand pipeline,
  chn0 is idle at boot before any client connects, so the boot-time apply
  never took effect. Generalized the existing running_mode latch-kick
  mechanism to also cover hflip/vflip, both at boot and on every live
  `/control` change.
- **Non-self-correcting A/V drift after a capture stall** (`hal_ingenic.c`):
  the v1.8.2 pts sanitizer's accept path could lock permanently to a
  hardware-clock offset that had drifted from the wall clock by any amount
  under its threshold - and since video and audio use different thresholds,
  a real stall could leave one track locked to the drift while the other
  resynced, producing a persistent A/V skew (observed as 0.9s+ after a
  real-hardware capture stall). Added a slow, bounded self-correction
  (NTP-style slew) so any accepted drift converges back toward zero over
  time instead of freezing in place, without affecting legitimate
  multi-second gap preservation or introducing visible per-frame jitter.

## [1.8.2] - 2026-08-09

### Fixed
- **Video RTP/fMP4/SRT timestamps** (`hal_ingenic.c`): the video publish path
  stamped every frame with `ms_now_us()` (wall-clock time at the moment the
  publish thread happened to run) instead of the encoder's actual per-frame
  capture timestamp. This carried publish-thread scheduling jitter and, after
  any encoder-poll hiccup, burst several frames with near-identical stamps as
  the backlog drained - non-monotonic timestamps that made mpv report "No
  video PTS! Making something up" / "Invalid video timestamp" over RTSP
  (reported against `cinnado_d1_t31l_sc2336_atbm6031`). New `pts_sanitize()`
  reprojects the encoder's real capture clock (`IMPEncoderPack.timestamp` /
  `IMPFrameInfo.timeStamp`) onto the existing `ms_now_us()` timebase via a
  per-stream offset, so RTCP SR / fMP4 tfdt / SRT PES see no timebase shift,
  only jitter removal - with a monotonicity + wall-clock-skew sanity check
  and safe fallback when the hardware value looks bad.
- **Audio timestamps, same bug class**: audio was also stamped with
  `ms_now_us()` at publish time. RTP's audio timeline is sample-count-driven
  and unaffected, but the gap-repair heuristics that consume the publish
  pts (`audio_gap_resync()`, fMP4's M2 audio re-anchor) reacted to the
  jittery stamp - a publish-thread stall that lost no samples (the AI FIFO
  buffered and burst them) looked like a gap and inserted phantom samples,
  drifting the audio timeline over long uptimes. Now uses the same
  `pts_sanitize()` helper with its own per-channel state and a tighter,
  FIFO-sized skew tolerance.
- **Audio idle-resume A/V skew**: fixing the above surfaced a real
  regression, caught by hardware QA: unlike the video encoder (which stops
  capturing while idle), the audio input is never stopped while idle - it
  keeps capturing into its FIFO the whole time. On resume, draining that
  stale backlog as a fast burst could ratchet the sanitized audio pts ahead
  of real time with no way back, showing up as several seconds of A/V skew
  on a fresh `/stream.mp4` connection (RTSP was unaffected - each RTP track
  re-anchors to NTP independently). Fixed by flushing the stale AI backlog
  on idle-to-active resume (mirroring the video encoder's own idle
  teardown) and hardening `pts_sanitize()`'s fallback so the sanitized pts
  can never run ahead of the wall clock.
- **`act_wait()`**: the only `CLOCK_REALTIME` condition-variable wait with no
  predicate loop in the codebase (everywhere else uses `CLOCK_MONOTONIC` for
  exactly this reason). Closed a lost-wakeup race on the stream-start path
  and removed a window where an NTP step (common shortly after boot, exactly
  when the first viewer often connects) could stretch the 1s idle-wait
  timeout, adding up to a second of avoidable latency when opening a live
  view from idle.
- **WebUI live-preview reconnect** (`mp4/httpd.c`): the fMP4/MSE preview
  player had no reconnect logic at all. The server's 15s `SO_SNDTIMEO`
  correctly kills an idle connection when a backgrounded browser tab
  throttles the muted-autoplay `<video>` element and stops draining it, but
  the player stayed dead until a manual page reload. Added
  `connect()`/`teardown()`/`retry()` plus a `visibilitychange` handler,
  mirroring the auto-reconnect the `/events` SSE endpoint already had.

## [1.8.1] - 2026-08-09

### Added
- **`GET /control?fields=1`** (`control.c`/`control.h`, wired in
  `mp4/httpd.c`): the authoritative inventory of every `F_CTRL`-flagged
  config field, grouped by section, walking the exact same `cfg_fields_*()`
  accessors `apply_ctrl_fields()` already uses (never a second hand-written
  list). Exists so `scripts/timps-qa.sh` can diff its own hand-maintained
  "fields I test" list against the daemon's real one and flag drift loudly
  instead of silently - the same bug class (a field landing in `config.c`
  but never reachable from a hand-written array) recurring one layer up, in
  the QA script's own coverage. New section 8d does that diff on every run.
- **`GET /control`'s `version` key**: exposes the existing `MS_VERSION`
  compile-time string (git describe tag+commit+dirty flag, already used by
  `timpsd -v`/the startup log line) over the API. Prompted by a fleet
  incident where `fw_ota.sh`'s flash script logged "Firmware flashed
  successfully" on multiple cameras whose running binary had demonstrably
  NOT changed post-reboot - the flash script's own success signal only
  proved a reboot happened, not that the new binary came back up. QA
  section 1b now surfaces this prominently, early, alongside the local
  checkout's own `git describe` for a quick manual eyeball-diff.
- **`GET /control`'s `srt` status block**: `{"available","enabled","port",
  "channel"}` for `USE_SRT` builds (`{"available":0}` otherwise) - there was
  previously no way to discover SRT capability/state from the outside at
  all. Backs the new QA section 4b below.
- **`scripts/timps-qa.sh`**, informed by a coverage-gap review:
  - Section 1b: build-identity check (see `version` above).
  - Section 4b: SRT stream integrity, gated on both the host's `ffmpeg`
    having `srt://` support and the camera's own `srt` capability/enabled
    state; reuses the existing `analyze_stream` core rather than a parallel
    analysis path. Previously zero SRT coverage despite `srt_fields`/
    `srt.c` existing.
  - Section 8b: added live/persist-only coverage for fields that were
    F_CTRL-POST-able but never tested - `osd.*` globals (`monitor_stream`/
    `font_path`/`vars_file` live; `enabled`/`supersample`/`hinting`
    persist-only), `motion.hold_ms`/`skip_frames`, and 14 previously-untested
    `audio.*` persist-only keys (`enabled`, `samplerate`, `channels`,
    `bitrate`, `high_pass`, `agc`, `ns`, `agc_target_dbfs`,
    `agc_compression_db`, `force_stereo`, `spk_enabled`, `backchannel`,
    `backchannel_codec`, `backchannel_rate`).
  - Section 8d: field-inventory drift check against the new
    `GET /control?fields=1` endpoint, with a documented allowlist of
    deliberately-untested fields (`sensor.*`, `motion.cols`/`rows`,
    `record`/`timelapse` enable/mode/channel, risky `video.*` geometry/
    codec/identity fields, `osd_item.enabled`/`type`, and the legacy
    brightness-only `daynight` fallback fields) - any *other* gap now warns
    by name instead of staying silent.
  - Section 8e: 5 negative-case POSTs against `control.c`'s hand-rolled
    JSON parser (truncated JSON, unterminated string, an overflow-prone
    number, an unknown top-level section, a wrong-type value) - asserts the
    daemon stays alive and answering, not a specific error-response shape.
  - Section 9 (`/events`): now POSTs a harmless `image.brightness` toggle
    mid-window and asserts the corresponding `config` SSE event actually
    arrives, instead of passively waiting and soft-warning "may be idle"
    forever regardless of whether the push path works.
  - Section 14b + an always-on preflight check (opt-in `--test-crash`,
    DESTRUCTIVE): sends a real `SIGSEGV` to the running `timpsd` over SSH to
    exercise the production fatal-signal handler end-to-end (no special
    build flag needed - `sigaction()` catches externally-sent signals the
    same as a genuine fault), asserts `/run/timps.crash`'s exact format,
    then restarts the daemon for real and confirms `/control` answers
    again. Separately, preflight now always checks for a *stale*
    `/run/timps.crash` left over from before this run (evaluated before the
    opt-in test above can create a fresh one) and renames it after
    reporting so it won't re-trigger.

### Fixed
- **`scripts/timps-qa.sh` section 8d (Fable review of the field-inventory
  drift check above)**: a pre-708ea08 daemon doesn't recognize `?fields=1`
  at all and just serves the normal `GET /control` status document instead
  (200 OK, non-empty, so the existing "can't even fetch it" guard never
  fired). That document has a totally different shape from
  `control_fields_json()`'s output - a top-level `"caps"` key the
  field-inventory doc never has, and per-section values that are *objects*
  (`"image":{"brightness":..}`, `"video":{"0":{...}}`) rather than flat
  arrays of field names. Section 8d's diff didn't detect the mismatch and
  happily iterated the normal document's object keys as if they were
  `F_CTRL` field names, producing bogus drift warnings - confirmed: 32 false
  warnings against a real camera on an older build, misreporting read-only
  status keys like `motion.available`/`video.0` as "POST-able". Added a
  shape guard (`"caps"` absent and `"image"` is an array) before the diff
  runs; on mismatch it now `skip`s cleanly with an explanatory message,
  same "older build lacks this capability" pattern already used for SRT
  (section 4b) and ONVIF above.
- **`scripts/timps-qa.sh` section 9 (`/events`)**: the `image.brightness`
  poke used to provoke a `config` SSE event had no interruption protection,
  unlike every other round-trip test in this script. A run interrupted
  between the poke and its restore (external kill, SSH drop, a colliding
  `--test-crash` run) stranded the extreme value on the real camera -
  confirmed: `image.brightness` left at 255 (blown-out image) on cam-A
  (192.168.1.100) until fixed by hand. Section 9 now uses the same
  `LV_PENDING`/`trap EXIT INT TERM` pattern section 8b's live-settings test
  already uses (added after the 2026-08-02 cam-K WB-rgain/bgain=32767
  incident): the pending restore body is tracked and flushed from an
  EXIT/INT/TERM trap, not just the normal fall-through path.

### Fixed
- **`videoN.rc_mode=fixqp` crashed the streamer** (reported on Discord
  against v1.7.8): `IMP_Encoder_SetDefaultParam()`'s `iInitialQP` argument
  was hardcoded to `-1` ("auto") regardless of `rc_mode`. Fine for the
  rate-controlled modes, fatal for FIXQP, which has no rate control to fall
  back on. `videoN.qp` (already exposed via `/control`, previously
  documented as "RESERVED/no-effect - no HAL consumer") now feeds
  `iInitialQP` when `rc_mode=fixqp`, default 35 if unset. Extended to the
  older manual-attribute SoCs (T20/T21/T23/T30), which had the same gap but
  silently ran CBR instead of crashing, and to the T23 software-rotate
  encoder path, which was missed in the first pass.
- **Classic-path H265 CBR/FixQP filled the wrong union member on T30**:
  `IMPEncoderAttrH265CBR`'s layout differs from H264's (`staticTime` sits
  between `minQp` and `outBitRate`, `flucLvl` replaces two H264-only bools)
  - an H265 channel was getting the bitrate written into `staticTime` and
    garbage into `outBitRate`. Now branches on `v->codec`.
- **`min_qp`/`max_qp` had no effect on T31/C100/T40/T41**: wired
  `IMP_Encoder_SetChnQpBounds()` into the ENC_NEW_API path (only the
  classic and T23 sw-rotate paths applied these before).
- **`rc_mode=vbr/smart/capped_vbr/capped_quality` silently ran as CBR on
  classic SoCs**: only `fixqp` was wired up going into this cycle. `vbr`/
  `smart` now use their real classic-SDK modes; `capped_vbr`/
  `capped_quality` (no classic equivalent) fall back to `vbr` with a
  one-time warning instead of a silent CBR substitution.
- **`codec=h265` on T10/T20 silently encoded H264 while advertising it as
  H265** (no H265 encoder exists on those SoCs) - RTSP/SDP and the
  keyframe detector both keyed off the config value, not the real stream,
  producing a mislabeled, broken stream with no warning. Now coerced to
  H264 at config-apply time, same pattern as the existing `rotation=180`
  unsupported-SoC coercion.
- **`jpeg.quality`/`videoN.jpeg_quality` had no effect on classic-API
  SoCs**: `IMP_Encoder_SetJpegeQl()` takes a full quantization table, not a
  scalar - synthesized one from the standard JPEG Annex-K base tables via
  the IJG/libjpeg quality formula. Covers T20/T21/T23/T30; T10 is
  deliberately excluded (matching prudynt-t's own T10 special-case: a
  custom table measurably degrades JPEG quality there, not just unverified).
- **`IMP_AI_EnableAec`/`DisableAec` raced the audio capture thread (UAF
  class)**: same free-while-recording-thread-runs hazard already documented
  for AGC/NS/HPF (why those are boot-only), but AEC was toggled live on
  every backchannel hangup/play completion with no lock. Now serialized
  under `g_ai_lock` against `audio_thread`'s frame fetch.
- **AAC→G.711 AI re-init raced live `/control` volume/gain writes**: the
  disable/re-enable sequence ran without `g_ai_lock`, so a write that just
  passed a liveness check could land on a channel mid-rebuild. Now locked.
- **HTTP shutdown drain only ran under `USE_TLS`**: non-TLS builds had no
  wait for in-flight `/control` handler threads before `g_hal->stop()`'s
  full IMP teardown - a narrow, exit-time-only crash window (backstopped by
  the existing 3s hard-exit alarm, but not otherwise closed). The ~500ms
  bounded drain now runs unconditionally.
- **`motion.hold_ms`/`skip_frames` live `/control` POSTs were silently
  dropped**: both are F_CTRL but only actually took effect via a full
  `motion_sync` rebuild, which nothing triggered on their own. Now routed
  through the same deferred-resync path the geometry keys already use.
- **`IMP_OSD_CreateRgn`'s return value was never checked**: a pool-
  exhaustion failure would proceed to call `RegisterRgn`/`SetGrpRgnAttr`/
  `ShowRgn`/`SetRgnAttr` against an invalid (-1) handle before any later
  guard caught it. Now checked immediately, logged, and that region's setup
  is skipped on failure.
- **`scripts/timps-qa.sh` section 8e's overflow-number robustness test
  stranded `image.brightness=255` on a real camera**: `{"image":
  {"brightness":99999999999999999999999999}}` is well-formed JSON that
  `control.c` actually applies (clamped to 255) - unlike the test's other 4
  cases, which are all rejected outright. `mb_check()` is a liveness check
  by design and never restores anything, so the camera was left at 255
  with a full PASS and nothing in the log to explain it. This was the
  actual, final source of a brightness-255 incident that recurred across
  this whole release cycle even after two earlier (real, but incomplete)
  fixes to `ov_clamp_test()` and section 9. Now captures and restores the
  value itself, with its own dedicated pending-var + EXIT/INT/TERM trap so
  it works standalone via `--only 8e` too.
- **`scripts/timps-qa.sh`'s `OUTDIR` default collided across cameras run
  in parallel**: only second-resolution-timestamped, no camera/PID tag - a
  fleet-wide QA invocation launching multiple cameras' runs within the
  same second shared one output directory and clobbered each other's
  recordings, surfacing as bogus "non-monotonic timestamp" failures with
  no real regression behind them. Tagged with the camera IP and PID now;
  `--out` still overrides.
- **`ov_clamp_test()`'s restore was fire-and-forget**: the restore POST's
  result was discarded and never re-verified, unlike `lv_section`'s
  restore. Now re-fetches after posting, retries once, and warns (with the
  actual HTTP code) if it still didn't land.

## [1.8.0] - 2026-08-08

### Added
- **Fatal-signal handler for SIGSEGV/SIGBUS/SIGFPE/SIGABRT (`main.c`)**, closing
  the gap flagged by a crash-surface research pass: timpsd links closed-source
  Ingenic vendor libraries (`libimp.so`, `libaudioProcess.so`) with documented
  crash modes already noted elsewhere in this codebase's own hardening
  comments (a `libaudioProcess.so` UAF on channel-teardown races, a
  `libimp`/encoder div-by-zero SIGFPE from a bad QP, ...). Previously a hit
  took the whole process down silently — only SIGINT/SIGTERM/SIGPIPE had
  handlers, and there is no core-dump story on this embedded target, so a
  vendor-library crash left literally zero diagnostic trail. The new handler
  installs via `sigaltstack()` (so it still runs on a stack-overflow fault)
  and, on any of the four signals, writes to both stderr and a new
  `/run/timps.crash` flat file (same `/run` convention as the singleton lock
  and `/control` token file — survives the dying process for a respawn
  supervisor to pick up, since the init script normally backgrounds timpsd
  and its stderr is otherwise discarded): the signal number, `siginfo_t`'s
  faulting address, the faulting PC (MIPS o32 `uc_mcontext.pc` — confirmed
  against this project's actual T31/T23 cross-toolchain `sys/ucontext.h`,
  not assumed), and a `/proc/self/maps` dump plus a classification of
  whether the fault address falls inside `libimp.so`, `libaudioProcess.so`,
  or timps's own code — the single most useful post-mortem fact for triage.
  Entirely hand-rolled on `open`/`read`/`write`/`close`/`signal`/`raise`/
  `sigprocmask` (no malloc, no `LOGx` — `log.c`'s `log_printf()` uses
  `vsnprintf`/a `pthread_mutex`/`syslog()`, none async-signal-safe). Never
  attempts to continue past the fault: restores the signal's default
  disposition, unblocks it, and re-raises so the kernel does its normal
  thing; `_exit()` is a last-resort fallback that should never execute.
  Verified with a standalone test harness (real SIGSEGV/SIGFPE/SIGABRT
  triggers) confirming the handler fires, logs correctly, classifies the
  fault address, and the process actually terminates (128+signum exit,
  core dumped) rather than looping or hanging; cross-compiled `main.c`
  clean against both the real T31 and T23 toolchains to confirm the MIPS
  `ucontext_t` field access. Adds ~2.8 KB `.text` and ~80 KB `.bss` (the
  altstack + a static `/proc/self/maps` scratch buffer — RAM only, not
  flash-resident) to the target binary.
- **`_Static_assert(MOTION_MAX_CELLS <= MOTION_STATUS_MAX, ...)` in
  `imp_motion.c`**: `MOTION_MAX_CELLS` is taken straight from whichever IMP
  SDK header a build compiles against (52 on most, 4 on the old T10/T20
  3.9.0 SDK), while every status array in `imp_motion.h` is fixed at
  `MOTION_STATUS_MAX` (64) slots. A future SDK bump that raised
  `IMP_IVS_MOVE_MAX_ROI_CNT` past 64 would have silently turned the
  existing runtime clamps into an out-of-bounds write instead of failing to
  build; this makes that a compile error instead.

### Changed
- **`GET /control`'s `caps.image`/`caps.audio` no longer hand-list field
  names in a second place either** (the caps-list half of the follow-on
  architecture finding flagged alongside the `F_CTRL` consolidation above):
  `control.c`'s `IMG_CAPS[]`/`AUD_CAPS[]` arrays re-typed every `image.*`/
  `audio.*` key name a third time, under their own copy of `isp_caps.h`'s/
  `audio_caps.h`'s `#ifdef` platform gates — the same "typed three times,
  one gets forgotten" shape as the field-name duplication `F_CTRL` just
  fixed, just in the capability-advertisement layer instead of the
  POST-apply layer. Added a second flag, `F_CAP`, to `cfg_field` (alongside
  `F_NOGET`/`F_ATOMIC`/`F_CTRL`): `config.c`'s `image_fields[]`/
  `audio_fields[]` tables now carry it directly, gated by the exact same
  `ISP_HAS_*`/`AUDIO_HAS_*`/`USE_PLAY`/`USE_BACKCHANNEL` macros the old
  arrays used, and `control_get_json()` builds `caps.image`/`caps.audio` by
  walking `cfg_fields_image()`/`cfg_fields_audio()` for entries carrying
  **both** `F_CTRL` and `F_CAP`, instead of the removed `IMG_CAPS[]`/
  `AUD_CAPS[]` arrays.
  - `F_CAP` is a **separate axis from `F_CTRL`, not a refinement of it**:
    every `image.*`/`audio.*` field is `F_CTRL` (POST-able) on every build,
    but `F_CAP` is compile-time platform/feature gated. For `image.*` it is
    a pure hardware-capability gate (identical to `isp_apply_image()`'s own
    guards in `hal_ingenic.c`). For `audio.*` it is narrower still: several
    fully POST-able, fully persisted fields (`codec`, `samplerate`,
    `bitrate`, `channels`, `enabled`, `force_stereo`, `spk_enabled`,
    `backchannel`/`_codec`/`_rate`, `high_pass`, `agc`, `ns`,
    `agc_target_dbfs`, `agc_compression_db`) deliberately never get `F_CAP`
    on **any** platform — not because the hardware lacks them, but because
    they are restart-required or persist-only (libimp runs several of them
    on its own vendor thread and frees state unlocked, so a live toggle
    would race it), matching the old `AUD_CAPS[]` array's explicit
    exclusions exactly.
  - Verified by building a standalone harness against `config.c`'s
    `cfg_fields_image()`/`cfg_fields_audio()` accessors under every
    `PLATFORM_T10`/`T21`/`T23`/`T31`/`T40`/`T41` macro (with and without
    `USE_PLAY`) and diffing the emitted name **set** against the old
    hand-written arrays' `#ifdef` conditions by hand for each platform:
    identical for every platform tested. The only difference found is
    cosmetic — `caps.audio`'s JSON array now emits `mute` before
    `spk_volume`/`spk_gain`/`aec` (table order) instead of after (the old
    array's order); the array is a membership list, not positionally
    consumed, so this reorders nothing observable. `make sim` builds clean
    with the new `F_CAP`-gated tables.
  - **Left alone, deliberately**: `src/hal/hal_ingenic.c`'s `isp_apply_image`/
    `ai_apply_key` strcmp dispatch chains still hand-list field names a
    third(now fourth) way, to route each one to its actual
    `IMP_ISP_Tuning_Set*`/`IMP_AI_Set*` call. Unifying that would need a
    real per-field dispatch mechanism (function pointers or tags covering
    genuinely different SDK calls and value transforms per field), which is
    a materially bigger, riskier change than relocating an existing,
    already-correct set of `#ifdef` conditions — not something to invent
    and land unreviewed in the same pass as this consolidation. The
    duplication risk there is unchanged: a future new image/audio field
    still needs a human to remember to wire it into `hal_ingenic.c` by
    hand.

- **`src/control.c`'s `/control` POST handling no longer hand-lists field
  names in a second place** (architecture-audit catch): 11 hand-written
  string arrays (`IMG`, `AUD_LIVE`, `AUD_REST`, `OSD`, `OSD_GLOBAL_KEYS`,
  `VID_REST`, `SENSOR`, `DN_KEYS`, `MOTION_KEYS`, `MOTION_RESTART_KEYS`,
  `REC_KEYS`, `TL_KEYS`, `PRIV_KEYS`) re-listed ~100 field names already
  present in `src/config.c`'s `cfg_field` tables — the confirmed root cause
  of the "nine orphaned fields" bug fixed just above (a field added to
  config.c's table but forgotten in control.c's matching array silently
  never applied over HTTP). Added one flag, `F_CTRL`, to `cfg_field`
  (alongside the existing `F_NOGET`/`F_ATOMIC`); config.c's tables are now
  the single source of truth for "is this field POST-able", exposed to
  control.c via eleven `cfg_fields_*()` accessors (image/audio/sensor/osd/
  osd_item/motion/record/timelapse/daynight/video/privacy). control.c's new
  generic `apply_ctrl_fields()` walks a section's table and applies any
  entry present in the POSTed JSON with `F_CTRL` set, reusing
  `timps_apply_setting()` exactly as before — the live-apply funnel itself
  is untouched.
  - `F_CTRL` is a deliberate, mandatory-per-field **security allowlist**,
    not a walk-everything default: fields the arrays never exposed keep no
    `F_CTRL` and stay silently unreachable via POST, matching prior
    behavior exactly — `motion.on_motion`/`cooldown_ms` (fork()+execlp()
    hook / re-exec floor), `daynight.switch_cmd`/`isp_path` (exec'd
    command / scraped proc path), every `rtsp.*`/`http.*` credential and
    token, the `videoN.imp_chn`/`jpeg`/`jpeg_quality`/`jpeg_fps`/`jpeg_chn`
    internal channel-wiring fields, and the OSD item's `logo`/`logo_w`/
    `logo_h`/`font_path` (never in the old per-item `OSD[]` array either).
  - Genuinely special cases stayed hand-written, unchanged: `speaker.play`/
    `stop` (transient actions), `record.active`/`record.clip` (actions),
    the legacy `force_mode` alias, `daynight.mode`'s reject-on-garbage
    validation (deliberately stricter than `config_apply_kv`'s own
    coerce-to-sensor-and-persist), and the nested/indexed structural loops
    for `osd`/`osd<S>`/`video`/`privacy` (each loop now calls the generic
    per-field walker once it has located the right sub-object/sub-table,
    rather than the top-level dispatch itself being generic across nesting
    shapes). The `osd.enabled`/`motion.enabled`/`daynight.enabled` and
    `daynight.time_night_start`/`time_day_start` hand-written special cases
    were removed as genuinely redundant (their truthy-string pre-conversion
    duplicated what `pbool()`/`copystr()` already do) rather than kept.
  - Verified against `timpsd-sim`: representative fields from each of the
    11 old arrays still apply/persist/round-trip identically; all 13
    fields fixed by the immediately-preceding "nine orphaned entries + osd
    item type" commit still work; a nested/indexed case (`osd0.0.type`)
    still applies; and every security-excluded field above still returns
    `{"ok":true}` with **no** `[CTRL] set` log line and **no** config-file
    write, confirmed both by daemon log inspection and a `GET /control`
    diff. Cross-compiled clean for T31 against real Ingenic headers/libs
    (zero new warnings); the resulting binary is 1312 bytes smaller
    (`.text` -940 B, `.data` -372 B, `.bss` unchanged) than before this
    change on an otherwise-identical build.

### Fixed
- **Boot-order gap left over from the 7th rotation-effective-dims site fix
  (`cb4c7de`)**: independent review of that fix found it correct but
  incomplete. `imp_osd.c`'s `osd_rotated()` asks the hub for a stream's
  ACTUAL post-refusal dims to tell a genuinely-applied 90/270 rotation apart
  from one refused by the T23 SW-rotate / T31 FS-rotate safe-envelope check,
  falling back to the raw (pre-refusal) computation only if the hub isn't
  populated yet for that stream — documented as "shouldn't happen in
  practice" because the OSD refresh thread only starts after every stream's
  `hub_set_video_params()` call. True for the refresh thread, but
  `hal_ingenic.c`'s `ing_start()` also calls `imp_osd_setup()` itself
  **before** that same stream's `hub_set_video_params()` call, and
  `imp_osd_setup()` performs the first OSD text/logo/cover render
  synchronously. So on every boot the hub-not-populated-yet fallback fired
  for that first render, and on a *refused* rotation the boot-time render
  used the pre-refusal (wrong) `rotated`/`hlim` — the exact oversized-bound
  scenario `cb4c7de` targeted — for one render pass. Text self-heals via the
  OSD updater (~1 s later); privacy covers are pre-clamped to width/height
  regardless (harmless); a boot-configured **logo** with `logo_h` in
  `(height, width]`, though, would pass the defeated H5 size check in
  `setup_logo()` and never get re-checked until a later `/control` write.
  Fixed by reordering `ing_start()` (`src/hal/hal_ingenic.c`) so
  `hub_set_video_params(i,...)` for a stream is published as soon as its
  effective (post-refusal) dims are known — right after
  `ms_vstream_eff_dims()` — instead of after `imp_osd_setup()` and its
  binds; `imp_osd_setup()`'s first render for that stream now always sees
  the correct, final `osd_rotated()` answer instead of the stale fallback.
  Confirmed safe: `hub_set_video_params()` is a pure mutex-protected struct
  store with no dependency on OSD/bind state, the published dims are
  already final at the new call site (computed post rotation-refusal
  retarget), and nothing reads the hub for that stream until after
  `ing_start()` returns. No behavior change for the accept path (dims are
  identical either way) or for text/covers (already self-healing/harmless);
  only the boot-logo edge case is closed. `imp_osd.c`'s `osd_rotated()`
  comment updated to match. Cross-compiled clean (zero new warnings) for
  T31 (`USE_ROTATE=1`) and T23 (`USE_ROTATE=1 USE_SW_ROTATE=1`) against
  their real Ingenic headers/libs.
- **Nine `cfg_field` table entries were silently unreachable via `POST
  /control`** (audit catch, not intentional design like their neighbors):
  each existed as a real, validated, clampable entry in `src/config.c` and
  was correctly settable by hand-editing `/etc/timps.conf`, but the
  hand-written per-section JSON handlers in `src/control.c` were never
  extended to cover them — a POST returned `{"ok":true}` with zero effect
  (no `[CTRL] set` log line, no config-file write, no live apply). None of
  the nine carry an `F_NOGET`/exclusion marker, unlike the fields that
  *are* deliberately excluded in the same tables (`motion.on_motion`,
  `motion.cooldown_ms`, `daynight.switch_cmd`, `daynight.isp_path` — all
  stay file-only, unchanged by this fix).
  - `osd.monitor_stream`/`font_path`/`vars_file`/`supersample`/`hinting`:
    added to the `osd` section's master-switch handler (same
    restart-required class as the already-POST-able `osd.enabled` —
    `imp_osd_setup()` only builds the OSD groups once at startup) and to
    `GET /control`'s `osd` object.
  - `osd<S>.<N>.type` (text vs. logo): added to the per-item POST key list
    (it was already GET-readable and every sibling field — `text`/`x`/`y`/
    `font_size`/`color`/etc. — was already POST-able). Persists and is
    pushed through the existing live-reapply call, but `imp_osd_apply()`'s
    render dispatch is fixed at region-creation time, so switching an
    existing item's type needs a restart to actually change what's drawn
    — same restart-required class as `enabled`.
  - `daynight.transition_s`/`interval_ms`/`threshold_low`/`threshold_high`/
    `hysteresis`: added to the `daynight` section's key list and to its
    `GET`/`/events` JSON — same live class as every other numeric
    `daynight.*` key (the detection thread polls `g_cfg` directly, no HAL
    call involved).
  - `motion.hold_ms`/`skip_frames`: added to the `motion` section's key
    list and to its `GET`/`/events` JSON. Unlike their already-POST-able
    neighbors (`enabled`/`cols`/`rows`/`sensitivity`/`monitor_stream`,
    which trigger a live IVS grid rebuild), these two only feed
    `IMP_IVS_MoveParam`/`g_hold_ms` at grid *create* time
    (`imp_motion.c`), so a POST persists and takes effect at the next
    such rebuild or a daemon restart, not immediately — documented as
    such rather than force-added to the live-rebuild path.
  - Verified end-to-end against `timpsd-sim`: for one field per section
    (`osd.hinting`, `daynight.transition_s`, `motion.skip_frames`), POSTed
    the value, confirmed the `[CTRL] set <key> = <value>` log line, the
    `CONFIG persisted ... setting(s)` write to the conf file, and the
    clamped/persisted value round-tripping back through `GET /control`;
    also re-verified `daynight.switch_cmd`/`motion.cooldown_ms` remain
    silently unreachable (no log line, no config-file write) and that an
    out-of-range `daynight.hysteresis` POST still clamps to its
    documented bound. `docs/wiki/Configuration-Reference.md` and
    `docs/wiki/HTTP-Control-API.md` updated to match (File-only →
    Restart-only/Live per field, per the corrected POST-able-key lists).
- **`osd.hinting` autohinter could erase thin stems instead of sharpening
  them** (adversarial review catch on the autohinting feature added above,
  fixed on the second attempt after the first repair was itself caught in
  re-review): the feature as first landed snapped each stem-side edge
  independently to `floorf(mid + 0.5)` (nearest pixel column/row). For a
  stem thinner than 1px in device pixels - which is exactly what the shipped
  `/usr/share/fonts/default.ttf` (correctly identified below as UbuntuMono
  **Regular**, not "Roboto Bold" as originally claimed above) produces for
  its ~0.98px-wide vertical strokes at the 12px OSD default - the two edges'
  independently-rounded columns could land on the very same integer (one
  edge at -0.49px displacement, the other at +0.49px, both floor to the same
  column), collapsing the stem to zero width, and a zero-width path renders
  as nothing: 'I' lost its entire vertical stem at 12px, "IlIlIl" at 8px
  lost stems wholesale - the exact failure the feature was meant to fix,
  made worse instead. Root cause: only tested against a Bold-weight
  substitute font, whose stems are wide enough that this collision is
  structurally unreachable, hiding the bug. A first repair attempt (sort
  the candidate edges by pre-snap position, push apart ADJACENT pairs whose
  snaps collided) was geometrically wrong and never shipped past review:
  sorted adjacency doesn't establish that two edges bound the same stroke,
  so it forced apart edges that were legitimately collinear (the aligned
  caps of H/U/N/M's twin stems, split segments of one stem side - new
  regressions at sizes that had been fine), mis-paired stems whose sides
  interleave with bar/bowl edges in sort order (57/89 sub-pixel stem pairs
  in the shipped font still collapsed, e.g. 'b' at 8px lost its left stem),
  and tore '+''s crossbar onto different rows at 11-13px.
  The final fix (`src/hal/msttf.c`: `autohint_glyph()`/`resolve_snaps()`,
  replacing the per-contour pass) pairs edges by actual stroke geometry
  instead. Candidates are collected across the WHOLE glyph (a bowl wall is
  bounded by one outer-contour and one counter-contour edge) together with
  their traversal direction and perpendicular extent, and two edges form a
  stroke pair only if ALL of: opposite traversal direction (a consistently
  wound outline walks the two sides of one stroke in opposite directions),
  overlapping perpendicular extents (they face each other along the stroke),
  and ink between them (even-odd midpoint test on the pre-snap outline,
  same fill rule as the rasterizer - rejects sub-pixel counters/gaps, whose
  bounding edges also satisfy the first two conditions). Only such pairs
  whose independent snaps collided are touched: the lower edge keeps its
  snap, the upper is pushed out by the pair's own measured pre-snap width
  rounded (floored at 1px). Edges at near-equal positions are recognized as
  already merged (never separated), and if a push moves one segment of a
  split edge line its near-equal siblings follow (keeps '+'-style split
  crossbars on one row). Everything the collision pass doesn't touch keeps
  its plain independent snap bit-for-bit. For width>=1px, `floor(x+0.5)`
  and `floor(x+w+0.5)` are always different (a half-open interval of
  length>=1 contains an integer boundary), so thick stems never enter the
  collision branch at all. No font- or size-specific constants: widths and
  separations are measured per pair at render time; the only epsilons are a
  0.05px float-identity tolerance and a 0.25px degenerate-overlap gate in
  device pixel space.
  Re-verified against the actual shipped font (byte-identical via md5 to
  `package/thingino-fonts/files/UbuntuMono-Regular2.ttf`, TTF `name` table
  family "UbuntuMono" subfamily "Regular") plus 5 other fonts spanning
  Regular-to-Black weights (Roboto Regular/Bold/Black, DejaVu Sans
  Regular/Bold) via an ink-verified white-box sweep (even-odd inside test
  against the pre-hint outline, so only pairs with real ink between them
  count) over every ASCII glyph at every size 8-40px plus 48/64/96/128/192/
  256px: 4568-5274 ink-verified stroke pairs per font, 0 collapse to zero
  width (89/89 pairs that collided under independent snapping in the
  shipped font are preserved; 70-71/70-71 and 4/4, 2/2, 0/0 in the others),
  0 collinear splits torn apart, 0 thin strokes over-widened. Regression
  gate at thick sizes: rendering all 94 ASCII glyphs at 32/48/64/96/128px
  through the real `msttf_render()` path, the new pairing logic is
  byte-for-byte identical to plain independent snapping (the pre-repair
  semantics) in all 470 renders - and byte-identical to the ORIGINAL
  feature commit for H/U/N/K at every thick size and every glyph at
  32/48px except where that commit's own in-place snapping mutated later
  edges' inputs mid-pass (a defect the collect-then-apply restructure
  removed). Spot-verified renders: 'I'@12px full stem, "CAM-K-CAM-H"
  @12px and "IlIlIl"@8px keep every stroke, 'b'/'K'/'P'/'d'/'g'/'p'/'q'/
  'u'@8px and 'B'/'D'/'K'/'R'/'h'@10px all keep their stems (alpha-coverage
  on/off ratios 0.87-1.41, no collapse signature), '+'@11-13px crossbar
  symmetric on every row. Fuzzing: 300k ASan+UBSan iterations of a direct
  degenerate-input driver against the new pairing code (0/1/odd candidate
  counts, all-identical positions, .5-boundary straddles, +/-1e9
  coordinates, capacity-overflow contours) plus the 1200-mutant malformed-
  font corpus (bit flips/truncation/zero-fill/0xFF-fill of 3 seed fonts)
  with hinting forced on: 0 crashes/UB.
- **Double-instance ISP collision hardening**, root-caused after a real
  incident: a manually-launched foreground `/tmp/timpsd` test build was still
  running when `/etc/init.d/S95timps restart` fired in another shell.
  busybox's `start-stop-daemon -S -x /usr/bin/timpsd` matches "already
  running" by executable PATH, not by process identity, so it never saw the
  `/tmp/timpsd` process and started a second `/usr/bin/timpsd`. That second
  process's `IMP_ISP_Open`/sensor-init reset the shared ISP kernel driver
  state out from under the first, destroying its FrameSource channels; the
  first process then spun in its encoder watchdog's recovery cycle forever
  (every attempt "succeeded" per `StartRecvPic` but the hardware was
  genuinely gone) producing zero video for 2+ minutes with no way to
  self-recover and no mechanism to hand off to init supervision. Two
  independent fixes:
  - `src/main.c`: an exclusive `flock()` on `/run/timps.lock`, taken before
    any ISP/HAL initialization. A second instance that loses the race logs a
    clear fatal error and exits immediately, never touching the shared
    hardware in the first place - this closes the actual race, independent
    of how the double-start happens. The fd is opened `O_CLOEXEC` (Fable
    review catch): `imp_motion.c`'s `on_motion` hook double-forks a detached
    grandchild that `execlp()`s a long-lived clip-capture/upload script -
    without `O_CLOEXEC` that grandchild would inherit the lock fd and keep it
    held after timpsd itself exits, permanently blocking the next instance
    from ever acquiring it.
  - `src/hal/hal_ingenic.c` (`video_thread`): the encoder watchdog's forced
    recovery cycle (`fs_unuse()`/`fs_use()`/`StartRecvPic`) now tracks
    consecutive cycles that never actually yielded a frame (via
    `IMP_Encoder_GetStream`, not just a "successful" `StartRecvPic`). After
    `MS_VIDEO_WATCHDOG_MAX_RECOVERIES` (default 5, ~25 s of total dead time)
    it logs FATAL and raises `SIGTERM` on itself so the existing orderly
    shutdown path exits the process, instead of retrying forever - this
    firmware has no process supervisor/respawn beyond a console getty, so the
    camera stays down until a manual/scheduled restart, but that is still
    strictly better than the prior silent, unbounded zero-video hang.
    `jpeg_thread` had the identical infinite-retry gap; it now gives up on
    just its own channel after the same number of failed cycles
    (`MS_JPEG_WATCHDOG_MAX_RECOVERIES`), mirroring how `audio_thread` already
    disables itself alone rather than taking the whole process down for a
    non-primary stream.
- **Piggyback JPEG (`/snapshot.jpg`, `/stream.mjpeg`) broke when a 90/270
  rotation was refused by the SW-rotate safe envelope.** On a T23 build with
  `USE_ROTATE`/`USE_SW_ROTATE`, requesting a rotation that exceeds the safe
  envelope (e.g. a full-res `1920x1080@25` main stream) is correctly refused
  by `sw_rot_start()` — the video/RTSP path falls back to running UNROTATED.
  But `jpeg_attach()` re-read the *raw* `cfg->video[vi]` (rotation still 90)
  instead of the caller's post-refusal effective config, so it sized the JPEG
  encoder channel from the rotated dims (e.g. `1080x1920`). That width is not
  16-aligned, so `IMP_Encoder_CreateChn` failed ("JPEG CreateChn N failed")
  and `/snapshot.jpg?chn=N` returned the literal `no frame` on every affected
  channel, even though video streamed fine. `jpeg_attach()` now takes the
  effective `const ms_vstream_cfg *` the caller actually brought the video
  channel up with, so the JPEG channel always shares the video channel's true
  post-refusal dimensions. This also covers the analogous T31
  (`ROT_HAS_FS_ROTATE`) FS-rotate fallback path, which had the same defect.
  Pre-existing bug (introduced with the JPEG-piggyback + rotation plumbing in
  July; unrelated to the recent C11-hardening / frame-pool changes).
- **IVS motion detection also used raw rotated dims after a refused 90/270
  rotation** — the sibling defect flagged during the review of the JPEG fix
  above, same root cause: `imp_motion.c`'s `imp_motion_start()` re-derived the
  monitored stream's geometry from the raw `cfg->video[mon]` instead of the
  effective post-refusal config. When the monitored stream's 90/270 request
  was refused by a safe-envelope check and the stream came up UNROTATED, the
  config still said 90, so on T31 (`ROT_HAS_FS_ROTATE`) IVS was created with
  swapped `frameInfo` dims (e.g. `1080x1920` against a framesource really
  delivering `1920x1080`), and on T23 (`ROT_HAS_SW_90`) the ROI grid was
  inverse-rotation-mapped for a rotation that was not happening — grid cells
  (and the privacy-mask exclusion, which is compared in displayed-frame space)
  landing transposed/mirrored relative to the frame the user actually sees.
  Fix follows the `jpeg_attach()` precedent: `ing_start()` now records the
  rotation it ACTUALLY applied per stream (`g_eff_rot[]`; the refusal decision
  only exists in `hal_ingenic.c`, and `videoN.rotation` is restart-only so the
  boot-time record stays valid), and `motion_sync()` hands
  `imp_motion_start()` an effective copy of the monitored stream's config
  built from `g_cfg_boot.video[mon]` + that recorded rotation. Sourcing the
  geometry from `g_cfg_boot` (per the `config.h` WHY block) also stops a live
  `/control` write to restart-only `videoN.*` geometry from leaking into a
  later motion-grid rebuild against the still-running old pipeline. Verified:
  `make sim` clean; `gcc -fsyntax-only -Wall -Wextra` of `hal_ingenic.c` +
  `imp_motion.c` against the vendored SDK headers for T31 (1.1.6, with and
  without `USE_ROTATE`) and T23 (1.3.0, with and without
  `MS_ENABLE_SW_ROTATE`). Needs an on-device check on a T31 with
  `motion.enabled=1` and a refused rotation (e.g. `1920x1080@25` + rotation
  90) confirming the IVS grid tracks the unrotated frame.
- **The identical raw-vs-effective-rotation-dims bug (see the `jpeg_attach()`
  fix and the IVS fix directly above, commits `8cc8987` and `efe94b9`) was
  still live in every OTHER consumer of a stream's post-rotation geometry**:
  `record.c`'s fMP4 track (`tkhd`) dims (both the continuous recorder's
  `seg_open()` and the on-demand clip muxer in `record_clip()`),
  `mp4/httpd.c`'s live-MP4 mux dims in `stream_mp4()` AND its `/events` SSE
  `stats` payload's `video[].width/height`, `rtsp.c`'s SDP `a=framesize`
  attribute in `gen_sdp()`, and `/control` GET's `eff_width`/`eff_height`
  fields all called `ms_vstream_eff_dims()` on the RAW configured rotation
  instead of the ACTUAL post-refusal geometry - so on any T23 SW-rotate /
  T31 FS-rotate safe-envelope refusal (the common case at typical
  main-stream resolutions/framerates - confirmed on cam-H and
  cam-L, both flashed with rotation enabled tonight), a recorded MP4, a
  live-MP4 stream, the `/events` stats feed, RTSP SDP negotiation, and the
  `/control` status could all advertise swapped W/H for a stream that is
  actually running unrotated. Root cause: `hal_ingenic.c` already tracks the
  real per-stream rotation (`g_eff_rot[]`) and already pushes the
  correspondingly correct effective width/height into the hub via
  `hub_set_video_params()` (both the SW-rotate accept path and the bound
  FS-rotate/no-rotate path push from the post-refusal `v`), but nothing
  could read that back out. Added `hub_get_video_params(int src, int
  *vcodec, int *w, int *h, int *fps)` (`src/hub.c`/`hub.h`) as that missing
  getter; all five originally-flagged call sites plus the SSE stats block
  now call it first and only fall back to the raw `ms_vstream_eff_dims()`
  computation when the hub hasn't been populated yet (stream never started
  - the getter returns 0). `/control`'s GET needed one extra bit of care:
  its `eff_width`/`eff_height` describe the live (persist-only, possibly
  POSTed-but-not-yet-restarted) config, not necessarily the boot config, so
  it only trusts the hub when the live rotation/width/height still match
  `g_cfg_boot` (i.e. nothing pending); otherwise it keeps the raw preview
  swap, preserving `scripts/timps-qa.sh --test-rotation`'s existing
  pending-config-preview contract. `ms_vstream_eff_dims()` itself is
  untouched and still used for its original purpose inside
  `hal_ingenic.c`'s pre-refusal setup (`sw_rot_start()`'s envelope checks,
  `enc_create()`, `fs_create()`) where the raw computation is exactly what's
  needed before the accept/refuse decision has been made. Verified: `make
  sim` clean (`hal_sim.c` never refuses rotation, so its raw-computed hub
  push already always equals the effective dims - no change needed there
  for consistency); cross-compiled `hal_ingenic.c`/`record.c`/`mp4/httpd.c`/
  `rtsp.c`/`control.c`/`hub.c` clean (`-Wall -Wextra`, zero warnings) against
  the real vendored SDK headers for T31 1.1.6 (`USE_ROTATE=1`) and T23 1.3.0
  (`USE_ROTATE=1 USE_SW_ROTATE=1`), using tonight's already-built
  `wuuk_y0510_t31x...`/`camL_y4_t23n...` cross toolchains (link step not
  attempted - the vendor `libimp.a`/`libalog.a`/`libsysutils.a` stubs aren't
  vendored in this checkout, only the headers). Not verified on real
  hardware: cam-H (192.168.1.100) and cam-L (192.168.1.100) were
  both reachable tonight, but a live redeploy onto production cameras mid-
  incident was judged too risky to rush safely - this relies on header-level
  type-checking plus manual tracing of both the SW-rotate and FS-rotate
  refusal paths instead.
- **OSD read live `videoN.rotation` instead of the boot snapshot** (audit A2):
  `imp_osd.c` read `g_cfg.video[si].rotation` in the text/logo/privacy placement
  paths. `videoN.rotation` is restart-only, so a `/control` write to it would
  immediately re-place the OSD for a rotation the running encoder is not
  producing (overlay wrong until restart). All rotation reads now use
  `g_cfg_boot`, which also removes them from the C11 data-race class below.
- **`imp_osd.c` was the 7th site of the raw-vs-effective-rotation-dims bug**
  (see `jpeg_attach()`/IVS/hub-getter fixes above, commits `8cc8987`,
  `efe94b9`, `9f71b41` - found by independent review of the latter): the
  rotated-IPU-OSD-path gate in `refresh_text()`, `setup_logo()`,
  `setup_cover()` and `osd_rot_place()` (text/logo/privacy placement, plus the
  H5 oversize check and `osd_even_pad()`) all decided `rotated` from the RAW
  `g_cfg_boot.video[si].rotation`, not whether rotation was actually applied.
  On a refused 90/270 (T23 SW-rotate / T31 FS-rotate safe-envelope refusal -
  stream runs unrotated, OSD still active on the bound path), `rotated` was
  wrongly computed `true`: the H5 size check used `hlim = s->width` (the
  narrower, rotated-frame limit) against a bitmap sized for the real, wider
  unrotated frame, defeating the very check whose own comment warns IMP_OSD
  "writes past the frame buffer" on several SoCs - plus needless even-padding
  and top-band placement clamping. Fixed by a new `osd_rotated(s)` helper,
  used at all four sites, that asks `hub_get_video_params()` for the dims the
  HAL is ACTUALLY running and compares them against the raw configured
  (unrotated) dims: swapped relative to raw = rotation genuinely applied,
  unswapped despite a 90/270 request = refused. Falls back to the raw
  `ms_vstream_eff_dims()` computation only if the hub hasn't been populated
  yet for that stream (matches the other sites' fallback; in practice
  unreachable here since `ing_start()` calls `hub_set_video_params()` for
  every stream before starting the OSD updater thread). Verified: `imp_osd.c`
  is not part of `make sim` (confirmed via the Makefile's `SIM_SRC` list, same
  as `hal_ingenic.c`); cross-compiled clean (`-Wall -Wextra`, only pre-existing
  unrelated warnings) against the real vendored SDK headers for T31 1.1.6
  (`USE_ROTATE=1`) and T23 1.3.0 (`USE_ROTATE=1 USE_SW_ROTATE=1`) using
  tonight's already-built cross toolchains; traced the refused-rotation +
  active-OSD scenario by hand, confirming `osd_rotated()` now returns `false`
  and every site behaves as the non-rotated (identity) case.
- **`sensor.*` numerics were POST-able and persisted but unclamped** (config
  audit F-01, high severity): `sensor.i2c_addr/fps/width/height` had no clamp,
  so a garbage `/control` POST (e.g. `sensor.width=70000`) survived a reboot and
  could feed the ISP init a value that crash-loops the camera. Now clamped like
  `videoN.*` (`fps` 0–120, `width`/`height` 0–8192, `i2c_addr` 0–0x7F; `0` still
  means "auto").
- **Completed the C11 data-race sweep started in v1.7.8** (audit A1 / config
  audit F-02/F-03): several background threads and the `GET /control` path still
  read live-mutable `g_cfg.*` ints/enums/floats lock-free while the `/control`
  connection thread mutates them under `config_str_lock()`. Fixed with the
  daynight per-iteration whole-section snapshot pattern (or a point-of-use
  lock+copy where a loop snapshot doesn't fit):
  - `record.c`: `rec_thread` now snapshots the whole `record` section once per
    pass and threads it through `want_run`/`want_write`/`motion_recent`/
    `seg_open`/`prune_free`; `record_clip` and `record_get_status` snapshot the
    live ints under the lock. (Strings were already correctly locked.)
  - `timelapse.c`: same per-pass snapshot for `enabled/channel/interval_s/
    keep_days`; `timelapse_get_status` reads its ints under the lock.
  - `rtsp/speaker.c` (`ao_ensure`) and `hal_ingenic.c` (`hal_ao_open` AEC):
    cold reads of `spk_enabled/spk_volume/spk_gain` and `audio.aec` now under
    the lock.
  - `rtsp/rtsp.c`: SDP/PLAY reads of `audio.bitrate_kbps/samplerate/channels`
    now under the lock (video fields already used `g_cfg_boot`).
  - Status/GET accessors: `daynight_get_status`/`daynight_sun_status` (incl. the
    tearable `sun_latitude/longitude` floats), the sim `motion_get_status` stub,
    and the numeric parts of `control_get_json`/`control_daynight_json` plus
    `motion.monitor_stream` now snapshot under the lock.
  - **Concurrent POSTs are now serialized**: `control_apply_json` runs its whole
    apply-and-notify body under a single mutex so two simultaneous POSTs can no
    longer interleave a partially-applied config (`hub_control()` still runs
    after each field's `config_str_unlock()` as before).
- **Doc drift** (config audit F-10): `record.post_roll_s` range is 1–300, not
  0–300 (wiki + QA spec fixed); `general.osd_pool_size` is in KB, not bytes
  (wiki fixed); `timps.conf.example` `osd_pool_size` now shows the real default
  (1024).

### Changed
- **Clamp the remaining unclamped live/persist config fields** (config audit
  F-09): `image.running_mode` 0–1, `image.anti_flicker` 0–2, `image.core_wb_mode`
  0–1, `audio.samplerate` 8000–96000.
- **`videoN.qp` and `videoN.max_gop` marked reserved / no-effect** (config audit
  F-04): both are parsed, clamped, persisted and echoed for compatibility but no
  HAL consumes them — the encoder's keyframe interval comes from `videoN.gop`
  (`rcAttr.maxGop = v->gop`) and there is no separate init/fixed-QP wiring (the
  pre-T31 attribute path is CBR-only). Wiring them up cleanly would need a
  rate-control redesign that cannot be hardware-verified here, so they are
  documented as reserved like `motion.roi_*`, with a one-shot warning when a
  non-zero value is set. Use `videoN.min_qp`/`max_qp` + `videoN.rc_mode` for
  quality control.
- **Removed the `cmfc`/`cmf2` CMAF brands from the fMP4 `ftyp`** (audit A3):
  CMAF (ISO/IEC 23000-19) requires one track per file, but this muxer always
  writes combined video+audio into a single `moov`, so advertising CMAF
  conformance was wrong for the A/V case (the strict validators the brands were
  meant to satisfy would flag it). Browsers ignore compatible-brands, so this
  was cosmetic; reverting the v1.7.8 addition.
- **Halve `FQ_MAX_BYTES` on the small-RAM SoCs** (perf audit P-08): T10/T20/T21
  builds now compile with `-DFQ_MAX_BYTES=1048576` (2 MB → 1 MB per queue), so
  the all-stalled worst case (8 HTTP + 8 RTSP + 8 SRT) is ~24 MB instead of
  ~48 MB. Added a `PLATFORM_CFLAGS` hook to the Makefile for the affected
  platforms.
- **RTSP play-loop syscall trimming** (perf audit P-03/P-04): `stream_loop` now
  reads `ms_now_us()` once per iteration and reuses it (no vDSO on this MIPS
  target — each call was a real syscall, 3–5×/iteration), and gates the
  nonblocking control-socket poll to ~50 ms instead of once per media frame
  (backchannel sessions still poll every iteration so interleaved speaker audio
  is not delayed). Teardown latency stays well under the RTSP keepalive window.

### Performance
- **Single-copy frame publish via `hub_publish_take()` + a per-source packet
  pool** (perf audit P-01, previously deferred): every published frame used to
  be copied twice - once when `video_thread`/`jpeg_thread` assemble the IMP
  packs into a scratch buffer (necessary: Annex-B start-code fix-up across
  scattered packs), then a SECOND time when `hub_publish()` -> `pkt_new()`
  malloc'd and copied the whole access unit into a refcounted `ms_pkt` (plus a
  `free` after the last `unref`). At 25 fps x 2 streams that was ~50 malloc/free
  pairs/s and up to ~1.5 MB/s of redundant memcpy, and frame-sized heap churn
  (4 KB-400 KB) is the classic fragmentation source on the 32 MB SoCs.
  - New `frame.c` primitives: an `ms_pkt` now carries `cap`/`pool` fields; a
    tiny per-source recycling `pkt_pool` (`pkt_pool_get`/`pkt_pool_init`, and a
    pool-aware `pkt_unref` that returns the buffer to the pool instead of
    `free`ing it on the last reference). `pkt_new()` (copy constructor) is
    unchanged, so every existing sink (fanqueue, the recorder pre-roll ring,
    RTSP/SRT/HTTP) is byte-for-byte unaffected.
  - New `hub_pkt_get(src, cap)` + `hub_publish_take(src, pkt, ...)`: the
    producer assembles the AU DIRECTLY into a pooled buffer and hands ownership
    to the hub - no second copy. `hub_publish()` and `hub_publish_take()` share
    one under-lock helper, so the two invariants are provably preserved: (a) the
    0-subscriber skip does NOT cause a per-frame malloc/free - a 0-sub publish
    just returns the buffer to the pool (borrow+return of the same buffer while
    idle); (b) `vparam_update()` still reads the AU under `s->lock` at the same
    point in the sequence, before any push/hand-off.
  - Pools are process-lifetime statics (a slow subscriber can hold a packet long
    after the producer stopped), sized `HUB_POOL_MAX_FREE=4` idle buffers with a
    `HUB_POOL_KEEP_CAP=96 KB` ceiling so a one-off large IDR is freed rather than
    pinned idle (idle pool memory bounded to ~384 KB/source). Beyond the pool
    (recorder pre-roll pinning many frames, or a slow client) it falls back to
    malloc/free - never worse than before.
  - Converted producers: the hardware `video_thread` and dedicated `jpeg_thread`
    (`hal_ingenic.c`) and both `make sim` producers (`hal_sim.c` video + JPEG,
    so the new path is actually exercised host-side). Left on the copy path
    deliberately: the three audio producers (tiny `__thread`-static frames whose
    per-frame copy is negligible and whose no-heap-churn design is intentional)
    and the opt-in T23 software-rotate video/JPEG path (`ROT_HAS_SW_90`,
    untestable here and flagged as extra risk by the audit). `hub_publish()`
    stays a first-class API for those.
  - Verified: clean `make sim`; an ASan+UBSan host build driven with concurrent
    multi-client RTSP (TCP+UDP) fan-out, snapshot hammering and motion recording
    (pre-roll ring pinning pooled packets) reported zero leaks / use-after-free /
    UB across subscribe/publish/unsubscribe churn and shutdown; and
    `hal_ingenic.c` type-checks clean against both the T31 headers (converted
    paths) and the T23 headers (untouched sw-rotate path). Still wants the
    on-hardware soak the audit called for before fleet rollout.
- **Idle-wakeup consolidation via a stop-condvar** (perf audit P-02, previously
  deferred): the periodic worker threads stayed responsive to shutdown by
  slice-sleeping (usleep in 100-300 ms chunks, re-checking a stop flag each
  chunk), waking ~25x/s in aggregate even fully idle. New reusable `ms_stopgate`
  primitive (`util.c`, same CLOCK_MONOTONIC condvar pattern as `fanqueue`/
  `events`): a worker now blocks on one `pthread_cond_timedwait` per real
  interval and wakes immediately on stop.
  - Converted: `daynight.c` (200 ms slices -> one wait per detection interval),
    `record.c` and `timelapse.c` (disabled-idle poll 300 ms -> 1 s, only bounds
    how fast a `/control` enable is noticed from the fully-idle state; motion/
    continuous recording is unaffected - it runs in the `fanqueue_pop` path),
    and the `imp_osd.c` updater (ten 100 ms slices -> one 1 s wait; a plain
    stop-gate, NOT the `hub_set_activity_cb` wake the audit floated, since that
    couples the OSD updater into the untestable HAL on-demand path for only a
    sub-second first-connect timestamp-freshness nicety). Idle wakeups drop from
    ~25/s to <5/s.
  - `main.c`'s `while (g_run) sleep(1)` is intentionally left as-is: `sleep()`
    already returns immediately on the shutdown signal (EINTR), and a condvar
    cannot be signalled from an async signal handler.
  - Shutdown safety (the highest priority for this item): `ms_stopgate_stop()`
    sets the flag and broadcasts under the SAME mutex the waiter tests its
    predicate under, and the waiter checks the predicate both before and inside
    the wait loop - so a stop requested before OR during the wait can never be
    missed. Traced by hand for all four threads and verified live: idle,
    recording and mid-stream SIGTERM all shut down in 0.14-0.34 s (well under
    the 3 s watchdog `alarm()`), ASan-clean.

### Added
- **Opt-in geometric autohinting for the OSD TrueType rasterizer** (`osd.hinting`,
  default `0` = off), addressing a research finding that substream OSD text
  (12px default, `src/config.c`) looks visibly less crisp than the main
  stream: `msttf.c`'s from-scratch `glyf` rasterizer scales outlines by a
  plain float factor with 2x2 supersampled coverage AA but never interpreted
  the font's embedded TrueType hint bytecode, so unhinted glyphs land at
  inconsistent sub-pixel positions and show uneven stroke widths between
  characters at small sizes - a real, well-known font-rendering problem, not
  imagined, and confirmed present in the shipped `/usr/share/fonts/default.ttf`
  (UbuntuMono Regular - corrected below; originally misidentified as "Roboto
  Bold" in this entry). Considered two fixes: (a) a true TrueType instruction
  interpreter (stack-based VM executing the font's own hint bytecode -
  genuinely correct, but real interpreter-writing work with a real
  correctness/security surface for bytecode running on-device with no
  sandboxing) vs (b) a lightweight geometric autohinter (snap long,
  near-vertical/near-horizontal flattened outline edges - typical letter
  stems/serifs - to the pixel grid before rasterizing, skipping bytecode
  execution entirely). Chose (b): this is a from-scratch, deliberately minimal
  rasterizer in a security/stability-sensitive embedded daemon, not a
  general-purpose font library, and a modest, safely-bounded improvement beats
  a more "authentic" but real bytecode-execution surface. `msttf_set_hinting()`
  (mirrors the existing `msttf_set_ss()` pattern) gates a new autohint pass
  (now `autohint_glyph()`; see the stem-collapse fix above for the final
  algorithm) that runs after `parse_glyph()` flattens a glyph's
  contours to device-pixel-space polylines: any edge longer than ~2px and
  within ~11 degrees of vertical/horizontal has both endpoints forced to a
  common rounded pixel column/row. A length gate excludes the short chords
  `quad()` emits per flattened bezier (8/curve), so round glyphs ('O', 'o')
  are provably untouched (verified bit-identical alpha-sum before/after in
  testing below) while stems ('l', 'I', '1', 'i') snap cleanly. Purely
  geometric, no bytecode execution, no new attack surface. Wired up as
  `osd.hinting` (bool, default 0) next to the existing `osd.supersample` in
  `config.c`'s `osd_fields`/`ms_osd_cfg` (not `general.*`: it's the same
  category of global TTF-rasterizer tunable as supersample, and unlike
  `general.*` the `osd.*` section is readable via `GET /control`). Same
  File-only/restart-only handling as `supersample`: applied once via
  `msttf_set_hinting()` in `imp_osd_setup()`, which only runs at startup, so
  it needs a restart to take effect - no live-apply plumbing added. Default
  off: zero behavior/output change for any existing install. Verified: `make
  sim` clean; a standalone host-side test (`msttf.c` has no external
  dependencies, so it links directly with a small driver) rendered strings at
  12px with hinting off/on against a real glyf-outline TTF (DejaVu Sans Bold -
  the shipped font's actual identity, UbuntuMono Regular, wasn't confirmed at
  the time and a Bold-weight substitute was used instead; see the follow-up
  fix below for why testing only a Bold weight hid a real bug) confirming: no
  crash, no all-zero/degenerate output, canvas size stable within 2px, alpha
  coverage within a 0.5-2x sane band; round glyphs ("OoOo0") bit-identical
  alpha sum on vs off; a repeated-stem string ("IlIlIlIl") showed edge
  "fuzziness" (partial-alpha-coverage variance at each stem's boundary
  columns, the direct signature of sub-pixel-phase inconsistency) drop from
  0.038 to exactly 0.0 with hinting on, i.e. every stem instance became fully
  sharp-edged regardless of its accumulated sub-pixel phase; multi-word test
  strings at 12px and 32px stayed fully legible with no garbling in both
  modes. Caveats for review: this is a heuristic approximation, not real
  hinting - it does not reproduce the font's authored hint intent and cannot
  guarantee exact stem-width preservation the way a bytecode interpreter
  would; the angle/length thresholds were tuned against one real font at OSD
  text sizes (8-32px) and are untested against other TTF files if a user
  swaps `osd.font_path`; `imp_osd.c`/`config.c`/`config.h` changes (the
  `msttf_set_hinting()` call site and the new `hinting` field) could not be
  cross-compiled in this pass (no vendored IMP SDK headers available in the
  sandbox) and still need an on-device or cross-toolchain build check.
- **QA coverage for previously-untested live-settable fields** (config audit
  F-08, `scripts/timps-qa.sh` section 8b): the daynight TIME/SUN path
  (`daynight.mode` + `time_night_start`/`time_day_start`/`sun_latitude`/
  `sun_longitude`/`sun_sunrise_offset_min`/`sun_sunset_offset_min`), the speaker
  keys `audio.spk_volume`/`spk_gain`/`aec` (gated on `caps.audio`, skipped
  cleanly when no AO pipeline is compiled in), and the path-traversal-sensitive
  live strings `record.dir`/`timelapse.dir`.

### Documentation
- **`timps.conf.example` brought back in sync with the code** (config audit
  F-05/F-06/F-07): added the 12 missing `daynight.*` keys (`mode`, the
  `time_*`/`sun_*` keys, `boot_settle_s`/`boot_settle_max_s`/`boot_stable_pct`,
  `night_reconfirm_s`, `probe_max_skip_s`) with example values and comments;
  corrected `http.adaptive_drop` (default is `1`, hardware-verified, not the
  stale "EXPERIMENTAL, default 0" text); fixed the audio/speaker block (there IS
  an AO pipeline on USE_PLAY/USE_BACKCHANNEL builds — `spk_enabled` gates the
  physical speaker and `spk_volume`/`spk_gain` are live), added the missing
  `audio.aec` key, and added `opus` to the `audio.codec` list.
- Fixed the stale doctrine comment in `config.c` that still claimed live int
  reads "need no lock" (it contradicted the correct `config.h` doctrine and was
  the likely root cause of the recurring F-02/F-03 class), and the stale
  `control.c` comment that described `on_motion` as `system()` when it is
  `fork()+execlp()`.

### Deferred (audited, not done this pass)
- **Perf P-01 and P-02 are now done** (see the Performance section above) - the
  dedicated session with a "cam-A" (Wuuk/T31-class) camera earmarked for the
  soak test made it reasonable to attempt them. The T23 software-rotate publish
  path and the three audio producers were deliberately left on the copy path
  (rationale in the Performance section); those remain available follow-ups if a
  T23 with rotation is ever soak-tested.
- **SDK feature-gap items #5 (`GetChnEvalInfo`) and #6 (exposure ceiling)** were
  skipped: both are new IMP features that cannot be verified without live
  hardware.

### Changed
- **De-duplicated JPEG source selection + cold-wake grab** between
  `mp4/httpd.c` (`/snapshot.jpg`, `/stream.mjpeg`) and `timelapse.c`.
  `timelapse.c` hand-mirrored `httpd.c`'s `jpeg_src_from_path()` and
  `snapshot_jpg()`'s two-phase piggyback-wake grab ever since timelapse
  gained just-in-time subscription (`32ac430`, explicitly "ported
  snapshot_jpg's pattern") - and every later fix to that logic had to be
  remembered twice: `26afbee` (gethostname hardening) and `51d9325` (read
  restart-only `videoN.*` from the boot snapshot) both touched both files.
  Both are now `hub_pick_jpeg_src(cfg, chn, strict)` and
  `hub_grab_jpeg(src, wait_ms, busy)` in `hub.c`/`hub.h` - hub-source-index
  logic, so it lives with the rest of the hub source abstraction rather than
  a new standalone file. `httpd.c` now does only HTTP response framing
  around the grabbed packet; `timelapse.c` does only the file write.
  Preserved a real behavioral difference between the two callers as an
  explicit `strict` parameter: an explicit `/snapshot.jpg?chn=N` never falls
  back to a different channel (404s instead), while timelapse's configured
  channel always falls through to the dedicated `jpeg.*` channel / any
  piggyback stream, same as before. Also preserved `snapshot_jpg()`'s
  distinct 503 "busy" (subscribe failed - source full) vs. "no frame"
  (grab timed out) responses via an optional `busy` out-param, since
  `timelapse.c`'s copy never made that distinction and a naive merge would
  have silently collapsed it for HTTP callers. No functional change to
  either call site; verified with `make sim` and against `timpsd-sim`
  (both a live timelapse capture cycle and `/snapshot.jpg`,
  `/snapshot.jpg?chn=0`, an invalid `?chn=1`, and `/stream.mjpeg?chn=0`).

## [1.7.8] - 2026-08-06

### Fixed
- **C11 data races on runtime-mutable config**: several live-settable fields
  were read by a different thread than the `/control` writer without any
  synchronization against it — a data race regardless of whether the plain
  int read ever visibly "tears".
  - `audio.mute`: the per-frame audio worker read it lock-free on every
    captured frame. It's now `_Atomic int`, and a new `F_ATOMIC` field flag in
    `config.c` routes `field_set()`/`field_get()` through `atomic_store`/
    `atomic_load` instead of a plain assignment.
  - **daynight thread**: was reading individual `g_cfg.daynight.*` fields
    (and `image.running_mode`) lock-free once per ~500 ms poll. It now
    snapshots the whole `ms_daynight_cfg` struct plus `running_mode` under
    `config_str_lock()` once per iteration — the same whole-struct pattern
    `imp_osd.c` already used — and threads that local snapshot through
    `dn_day_trigger()`/`dn_status_update()`/`dn_switch()` instead of each
    re-reading global state.
  - **OSD `.enabled`**: the updater loop checked it lock-free before calling
    `refresh_text()`; the check now happens inside `refresh_text()`, after its
    own under-lock item snapshot.
- **`aac_asc()`** now logs a warning when handed a samplerate outside the
  standard AAC table instead of silently falling back to the 16 kHz index.

### Added
- **fMP4 CMAF brands** (`cmfc`, `cmf2`) added to the `ftyp` compatible-brands
  list for stricter CMAF validators (Bento4, some HLS/DASH tooling).
- **QA script** (`scripts/timps-qa.sh`):
  - New section 8c: an SSH-based round-trip test of the `osd.vars_file`
    custom-placeholder mechanism.
  - Section 8b: added the missing `probe_max_skip_s` live-setting coverage,
    and a new persist-clamp regression test (`ov_clamp_test`) that POSTs
    out-of-range values and asserts the read-back is the clamped boundary,
    not the raw input.

### Documentation
- Documented the `osd.vars_file` custom-OSD-placeholder mechanism
  (Configuration Reference), including its non-atomic-write concurrency
  caveat and the atomic-replace mitigation.
- Documented that empty `rtsp.user`/`http.user` credentials (the shipped
  default) leave the media endpoints open to anyone on the network while
  `/control`/`/events` stay loopback-gated (Configuration Reference, HTTP
  /control API Reference, `timps.conf.example`).
- Documented a deferred RTP/RTCP timestamp overflow after ~2.8-3 years of
  continuous uptime ("L13", Streaming Protocols § Known limitations).

## [1.7.7] - 2026-08-05

### Fixed
- **Day/night reconfirm probes were themselves the visible "periodische
  Tag/Nacht-Umschaltungen."** Every probe switch clunks the IR-cut relay,
  kills the IR LEDs and shows ~7–9s of dark colour video before reverting —
  so on a genuinely dark, unchanging night the hourly `night_reconfirm_s`
  probe flapped 8–12× per camera per night while learning nothing, and on a
  slow pre-dawn ramp the v1.7.4 sustained-brightening probe added another
  2–6 (each failed probe re-sampled a *lower* baseline, so the continuously
  declining gain kept re-crossing the freshly-lowered bar every 10–40 min to
  sunrise; fleet logs all 11 cameras 2026-08-03/04). Three pure
  probe-scheduling measures in `daynight.c`, no config/schema change:
  - **exponential backoff** — a probe that fails (reverts within 30s,
    `DN_PROBE_FAIL_WINDOW_MS`) doubles the periodic interval ×1→×2→×4
    (`DN_PROBE_BACKOFF_MAX`), bounded by `max(night_reconfirm_s,
    DN_PROBE_BACKOFF_CAP_S=4h)`; a camera keeps its first-hour self-healing
    probe but then stops clunking hourly. Any genuine transition (or a probe
    that sticks in day) resets the multiplier.
  - **brightening arming margin** (`DN_BRIGHTEN_MARGIN`, 0.97) — the hold
    starts only clearly below the probe bar, never on a tangent graze (fleet
    logs showed holds starting 0.2% under the bar).
  - **failure ratchet** — after a failed probe, a new brightening hold must
    additionally undercut `day_gain_pct`% of the level that just failed (a
    whole further trigger-worth of new brightening), so a slow ramp can no
    longer re-fire on a drifted bar; a real light-on step (20–35% gain drop)
    still passes immediately. Latched on failure, cleared on any genuine
    transition.

  Verified in `timpsd-sim` (fake-ISP harness): backoff intervals stretch
  15→30→60→60s (×2,×4,cap) under constant darkness; a clean 66% step still
  brightening-probes and sticks in day on the first try; a forced probe
  failure latches `backoff x2, ratchet < N`; a below-margin/above-ratchet
  gain produces no new hold (the volley cycle); and a gain that undercuts the
  ratchet is allowed a fresh probe again.
- **The periodic reconfirm probe still physically clunked the IR-cut on a
  schedule even when nothing had changed.** Backoff cut the *frequency* of the
  probe but not its *invasiveness*: on a camera sitting in genuine, unchanging
  darkness (cam-K, closet, 2026-08-04: "das klacken der IR blende nervt …
  nachts andauernd") every backed-off probe still drove the board's IR-cut
  relay — an audible mechanical click — only to read railed night gain and
  revert. The periodic probe is now **gated on passive evidence**: before it
  fires, the smoothed night gain is compared against the same probe bar the
  sustained-brightening hold uses; if the gain is still solidly deep in night
  (≥ `DN_BRIGHTEN_MARGIN` of the bar), the physical switch is **skipped** — no
  `dn_switch`, no IR-cut click — and the probe silently re-arms on the same
  backoff schedule. This is not weaker self-healing: a *false* night latch
  (actually daytime behind an engaged IR pipeline) reads low gain, which is
  exactly the evidence that fires the probe; only a genuinely-dark scene, where
  a probe could only fail, is skipped. The first probe after each night entry
  still always fires (so the stuck-forever class stays covered within the first
  interval), and a `DN_PROBE_MAX_SKIP_S` outer bound (12h) forces a probe
  regardless of gain once that long has passed since the last *actual* physical
  probe — the trust-nothing safety net for a permanently-flat reading that
  evidence alone can never clear. Net effect under permanent darkness: at most
  ~2 physical clicks/day (vs up to 6/day at the 4h backoff cap before). Verified
  in `timpsd-sim`: constant deep-night darkness fires one first probe then skips
  every subsequent scheduled probe (zero further `switching to day`); and with
  a lowered outer bound the skips interleave with a forced probe every bound
  period, proving the safety net is never silently disabled.
- **IR-reflection feedback loop could flip day/night every few seconds
  indefinitely.** A camera mounted very close (~30 cm) to a reflective object
  hits a *physical* loop the probe-economy logic above cannot see, because it
  happens on the PRIMARY threshold crossings, not on a probe: night → IR LED
  on → the LED reflects intensely off the close object → AGC gain reads very
  low ("bright") → genuine night→day crossing → IR LED off + colour pipeline →
  but it is actually still dark → gain rails back up → genuine day→night
  crossing → IR LED on again → repeat, clunking the IR-cut every few seconds.
  Added a general **oscillation breaker** in `daynight.c` (a backstop for ANY
  fast day/night oscillation, not IR-specific detection): it counts *genuine*
  (non-probe) mode flips in a rolling window (`DN_OSC_WINDOW_MS`, 60s) and, if
  `DN_OSC_FLIPS` (3) of them land inside it, logs one warning
  (`possible IR-reflection feedback loop detected (N flips in Ms) - camera may
  be mounted too close to a reflective object; freezing in <mode> for Ts`) and
  FREEZES the last-decided mode for `DN_OSC_FREEZE_MS` (10 min), suppressing
  both switches and probes so the loop cannot continue; after the cooldown it
  resumes and re-detects if the condition persists. Probe fire/revert flips are
  deliberately NOT counted — a reconfirm/brightening probe cycle is a normal,
  intentional 2-flip event under the probe-economy design above and can never
  trip the breaker. These are compile-time `#ifndef`-overridable constants (like
  the other `DN_*` tunables), non-configurable at runtime by design. Verified in
  `timpsd-sim` (fake-ISP harness): a gain swing across both thresholds every ~7s
  trips the breaker on the third genuine flip, holds the mode for the whole
  cooldown while the gain keeps swinging (zero switches), then lifts and
  re-detects; and — proving no regression — the reconfirm-probe fire/fail/backoff
  scenario (reconfirm=15s, constant darkness, a probe cycle every 15–60s) never
  emits the oscillation warning, because its flips are all probe-driven.
- **`/control` persisted (and echoed) the raw pre-clamp POST value instead of
  the clamped one.** In `timps_apply_setting()` (`control.c`, the single funnel
  every `/control` key passes through) the clamped/canonical value read back
  from `g_cfg` (`config_get_kv`) was computed but used *only* for change
  detection: the live HAL call, the `/events` "config" SSE echo and the value
  written to `/etc/timps.conf` all used the raw string. Posting an out-of-range
  numeric (e.g. `daynight.probe_max_skip_s` below its 3600 floor, or
  `image.brightness` above 255) left the daemon's in-memory value correctly
  clamped — `GET /control` reads `g_cfg`, so it was always right — but wrote the
  raw out-of-range text to the config file and pushed it over SSE. The file then
  disagreed with reality indefinitely (a reboot re-clamps in memory on load but
  never rewrites the file), and other open WebUI tabs showed the bogus value.
  Not a live-safety bug (the running daemon is governed by the clamped in-memory
  struct; `ing_control` re-reads `g_cfg` and only used the raw string for a log
  line), purely a persistence/display inconsistency. Fixed by feeding the
  canonical read-back value (exactly what `GET /control` reports) to all three
  consumers. The read-back is now taken unconditionally after the write (not
  gated on the *before*-write readability flag) so the legacy `osdN.*`
  all-streams keys — unreadable while per-stream item sets have diverged, but
  re-converged by the write — also persist their clamped value.
  Edge case ruled out: keys whose clamp happens but whose value is *not*
  read-back-able (`F_NOGET` fields `jpeg_quality`/`jpeg_fps`/`logo_w`/`logo_h`,
  and clamped ints inside `noget` sections `jpeg`/`srt`/`rtsp`/`http`/`events`/
  `general`/`sim`) would keep the old raw-value behaviour — but none of those
  keys are settable through `control_apply_json`, so the residual gap is
  unreachable in practice. Verified in `timpsd-sim`: for several ranged keys the
  `GET /control` read-back, the on-disk config bytes and the `/events` config
  push now all agree on the clamped value; QA section 8b (every live-settings
  round-trip: image/audio/osd/privacy/motion/daynight/record/timelapse, the
  persist-only `video0.bitrate` and `audio.codec` checks) stays 15 PASS / 0
  WARN / 0 FAIL, and the unchanged-value skip, the `image.running_mode`
  re-assert-without-persist and the `motion.sensitivity` quantization skip are
  behaviourally unchanged.

### Changed
- **`daynight.probe_max_skip_s` (the passive-evidence-skip outer bound above)
  is now a live-configurable setting** instead of a compile-time-only
  `DN_PROBE_MAX_SKIP_S` constant, requested after confirming the skip fix on
  real hardware overnight. Default unchanged (43200s/12h); range 3600–604800s,
  deliberately floored at 1h in `config.c`'s validation table and clamped
  rather than accepted below it — this stays a safety net, not a switch to
  turn the self-healing check off outright.

## [1.7.6] - 2026-08-03

### Fixed
Comprehensive audit for the same bug class as the v1.7.5 day/night fix — a
thread reaching a state with (1) healthy-looking continued execution, (2)
zero log output, (3) no reachable recovery path — across the rest of the
codebase, followed by fixes for every confirmed instance:

- **Motion detection could silently stall forever with `motion.enabled`
  still reporting true.** `imp_motion.c`'s IVS poll loop treated every
  `PollingResult`/`GetResult` failure as a bare retry with no counter, no
  log, and no recovery: a driver/SDK wedge left the thread ticking at the
  poll rate while `/control` kept claiming motion was live and the last
  "active" grid snapshot froze in place — silently killing motion-triggered
  recording downstream. Added a stall watchdog (10s of consecutive misses)
  that cycles the IVS channel and surfaces a new `motion.stalled` status
  field instead of failing silently.
- **RTSP sessions could become immortal.** The idle-reap exemption keyed off
  `session.tcp`, a flag latched at SETUP time and never cleared — but
  several real transport combinations (UDP video + TCP-only backchannel, a
  transport-switch re-SETUP, a SETUP-but-never-published TCP audio track, an
  encoder wedge before the first frame) set that flag while never actually
  writing a byte over TCP, so neither the idle reaper nor `SO_SNDTIMEO` could
  ever catch an ungracefully-dead client on that session. The exemption now
  tracks real per-sink TCP write success instead of the latched transport
  choice.
- **HTTP fMP4/MJPEG and SRT streaming loops could spin forever on an
  encoder stall.** Disconnect detection was entirely data-driven (piggybacked
  on send/receive of media), so a stalled encoder combined with a TLS client
  (whose non-blocking recv can never observe an orderly close) or a client
  that vanished without a clean TCP close left the loop spinning at its poll
  cadence indefinitely, pinning a client slot. Added a 60s no-packet idle
  bound to all three loops. The `stream.mp4`/`stream.mjpeg` pre-keyframe
  discard state also only ever requested one IDR and never re-checked for a
  disconnected client; it now retries the IDR request and probes for
  disconnect every ~1s while waiting.
- **Audio watchdog could never trip on a persistently failing `GetFrame`.**
  The miss-streak counter was reset unconditionally right before calling
  `IMP_AI_GetFrame`, so a GetFrame that kept failing after a successful
  `PollingFrame` reset its own watchdog on every tick — audio died silently
  and permanently (with a busy-loop risk, since that path had no `usleep`
  either) instead of tripping the existing 500-miss teardown.
- **JPEG (snapshot/MJPEG) encoder thread had no stall watchdog at all**,
  unlike the video encoder thread, despite being able to pin the
  framesource 24/7 once wedged. Added the same PollingStream-miss-counter +
  framesource recycle cycle video_thread already had.
- **Framesource `EnableChn` failures were never retried** once the
  refcount left them at ≥1 (e.g. motion detection holding a pin) — every
  subsequent user believed the channel was enabled. `fs_use()` now retries
  the real hardware enable independent of the refcount transition, closing
  this for the common single-holder case (co-holder cases are a documented
  partial gap, tracked as a follow-up rather than rushed in without
  hardware validation).
- **Backchannel/speaker ownership had no inactivity release.** A session
  that talked once and then went quiet (but kept its RTSP connection open)
  held the backchannel decode election and the physical speaker
  indefinitely — every other client's talk audio was silently dropped and
  the play-clip queue never played. Both now re-elect/release after 10s of
  silence from the current owner.
- **SRT client threads had no liveness check at all** when the source
  stopped publishing (`if (!p) continue;`), unlike the equivalent HTTP
  path. Added the same 60s idle-stall bound.
- Lower-severity visibility/config-trap fixes found in the same pass:
  `record.post_roll_s=0` made motion-triggered recording silently record
  nothing, ever (config minimum raised to 1); `/control`'s `record` status
  now exposes whether the motion gate backing motion-mode recording is
  actually available/enabled, and whether a manual recording override is
  latched; the OSD updater thread's create-failure path now logs (matching
  every sibling thread); the day/night "ISP unreadable" idle path is now a
  visible one-shot warning instead of debug-only.

Every fix independently reviewed by a second model pass against the actual
diff (not just the diagnosis) before landing; one review finding (a
`hal_ingenic.c` comment overclaiming the framesource-recycle fix under a
co-holder) was corrected to accurately describe the remaining gap. Verified
via `make sim` and a real T31 cross-build; deployed and QA-tested on one
camera before fleet rollout.

## [1.7.5] - 2026-08-03

### Fixed
- **Day/night thread could get permanently stuck at boot with zero self-
  healing.** From `DN_UNKNOWN`, the decision silently stays put while gain
  sits inside the day/night dead-zone (300..3000 default) — by original
  design. But a camera can boot with the ISP already in a *persisted* mode
  and a dead-zone reading (e.g. a restart in daylight with a stale night
  config), and both self-healing probes (periodic reconfirm, sustained
  brightening) are gated on `cur==DN_NIGHT` — which `DN_UNKNOWN` never
  satisfies. Result: a camera could render night video (or day, in the
  inverse case) indefinitely after a reboot, with a perfectly healthy
  thread producing zero log lines, only discovered live on a T31 that
  restarted at 09:23 in broad daylight and stayed dark for hours.
  Once the boot-settle window ends still undecided, the thread now adopts
  the persisted `image.running_mode` as its internal state (the ISP is
  already running it, so nothing switches) so the normal in-mode triggers
  and probes arm. Since an adopted night is a guess rather than a
  measurement, its first day-pipeline verify probe fires within 5 minutes
  (or sooner if `night_reconfirm_s` is set lower) — once, even when
  periodic reconfirm is disabled. Pre-existing gap, not a v1.7.4
  regression; v1.7.4 only happened to be the build running when a restart
  finally landed in the dead-zone. Verified in `timpsd-sim` replaying the
  exact incident (adopts, verifies, and reaches day in ~25s versus
  indefinitely stuck before) plus a one-shot-probe-with-reconfirm-disabled
  case and two clean (non-dead-zone) boots showing zero adoption noise.

## [1.7.4] - 2026-08-03

### Fixed
- **Day/night baseline drift ratcheting into an overnight flap loop.** The
  v1.7.3 hardening's upward-only EMA baseline drift tracked raw gain ticks;
  noisy night AGC ratcheted the baseline to its noise ceiling, causing
  night↔day to flap every few minutes to every hour, all night, on real
  cameras — worse than the bug it was meant to fix. Replaced with a
  night-only smoothed gain driving a slow, symmetric baseline drift, an
  edge-armed brightening probe that disarms after a failed attempt, and an
  8s post-probe AE-stability gate so a lit room's exposure-convergence
  transient can't kill a legitimate probe. Verified against the exact
  logged flap pattern in `timpsd-sim`: zero flaps over 3 minutes where
  v1.7.3 flapped every 1-2 minutes.

## [1.7.3] - 2026-08-02

### Fixed
- **Adaptive night→day threshold too strict for a real light source.** Two
  live incidents: a basement whose only light dropped gain to 65% of a
  cleanly-sampled night baseline (never crossing `day_gain_pct`'s 60% bar),
  and a room whose baseline was sampled mid-lighting-transition
  (unrepresentatively low). The trigger is now floored at
  `total_gain_day_threshold`, the baseline drifts toward observed gain
  instead of staying fixed, and a "sustained brightening" probe forces an
  early day-pipeline recheck instead of waiting up to `night_reconfirm_s`.
  (Superseded by the fix in 1.7.4 above once this introduced its own
  regression.)
- `night_baseline`/`day_trigger` (the adaptive values currently in effect)
  are now exposed read-only in `GET /control` and the `/events` SSE push.

## [1.7.2] - 2026-08-02

### Fixed
- `boot_settle_s`/`boot_settle_max_s`/`boot_stable_pct`/`night_reconfirm_s`
  (new in 1.7.1) were live-settable but never actually appeared in the
  `GET /control` status JSON — a separate hand-written serializer had its
  own hardcoded field list, unrelated to the settings path.

## [1.7.1] - 2026-08-02

### Fixed
- **False night-mode latch surviving a reflash into broad daylight.** A
  fixed 5s post-boot settle window was too short for a cold/freshly-
  reflashed sensor's AE to converge, so a transient gain spike could
  commit straight to night regardless of real daylight and never recover.
  `boot_settle_s`/`boot_settle_max_s`/`boot_stable_pct` now wait for
  several consecutive gain readings to actually stabilize before trusting
  the first decision, and a new `night_reconfirm_s` periodically forces a
  real day-pipeline probe so an already-latched false night self-heals
  instead of requiring a manual `/control` override.

## [1.7.0] - 2026-08-02

### Added
- **HTTP Digest authentication** (RFC 7616 `qop=auth` + legacy RFC 2069)
  alongside the existing Basic auth, for both the HTTP preview endpoints
  and RTSP.
- **Read-only encoder telemetry** via `IMP_Encoder_Query`: per-channel
  queue/buffer stats (`registered`, `left_pics`, `left_stream_bytes`,
  `left_stream_frames`, `cur_packs`, `work_done`) and, on T31,
  `ave_bitrate` from `IMP_Encoder_GetChnAveBitrate`, exposed as a new
  `"encoder"` object in `GET /control`.
- **Per-client adaptive fMP4 frame-dropping** on weak links: a slow
  `/stream.mp4` client freezes on its last frame and resumes cleanly at
  the next keyframe instead of stalling every subscriber, with drop stats
  (fps/kbps/resolution/drops) visible in status. Defaulted on once
  hardware-verified.
- **Live IVS motion sensitivity** via `IMP_IVS_SetParam` (no grid rebuild
  needed for a sensitivity-only change), and opt-in AEC
  (`IMP_AI_EnableAec`) for the backchannel.
- `USE_RECORD`/`USE_TIMELAPSE` compile-time flags to shrink SD-less builds;
  `{fpsN}`/`{bitrate}` OSD placeholders for a specific stream's measured
  throughput.

### Changed
- **`rotation=180` removed on classic-API SoCs** (T10/T20/T21/T23/T30/T31/
  C100), since it's mechanically identical to `image.hflip`+`image.vflip`
  there — then **restored specifically for T40/T41**, which have a genuine
  per-channel I2D-based 180° distinct from their (global) hflip/vflip
  registers. Final `caps.rotation`: T31 `[0,90,270]`, T40/T41
  `[0,90,180,270]`, no-rotation SoCs `[0]`.
- `sendmmsg`-batched UDP video RTP per access unit, table-driven
  `/control` key lookup (~17KB smaller `.text`), explicit per-thread-type
  stack sizes, and a just-in-time timelapse hub subscription (was held
  24/7) — all throughput/footprint work with no behavior change.

### Fixed
- **T31 FS-rotate / T23 SW-rotate crash safety.** A rotation request
  outside the vendor-safe envelope (64-aligned & ≤1280x704 & ≤15fps for
  T31; 16-aligned & ≤704x576 & ≤15fps for T23) used to silently fall back
  to an oversized/misaligned software path that then failed encoder
  bring-up and took the **entire multi-stream pipeline** down — reproduced
  live via a rotation the `/control` API had itself accepted and
  persisted. Both platforms now refuse an out-of-envelope rotation and
  bring that one stream up unrotated instead; a `SetChnRotate`/
  `YuvInit` failure is likewise isolated to the affected stream rather
  than aborting the whole daemon.
- **A batch of RTSP/RTP/RTCP/SDP conformance fixes** found across several
  review rounds: `SET_PARAMETER` answered as a keepalive (200, RFC 2326
  §10.9) instead of 405; idle TCP backchannel-only sessions reaped after
  the standard timeout (previously immortal, since they never trip the
  media-write timeout other TCP sessions rely on); `Content-Length`
  request bodies actually consumed so the byte stream stays framed;
  `rtsps://` scheme stripped so TLS clients resolve the right stream;
  unsupported `Require:` feature-tags answered 551; Digest `uri=`
  verified against the real request-target; orphaned UDP sessions reaped
  at 2x the advertised timeout; `CSeq` echoed on every error response;
  `HEAD` answers `GET`'s headers with no body (RFC 7231 §4.3.2); plus
  fixes for SDP truncation/`Content-Length` mismatches, `FD_CLOEXEC` on
  accepted sockets, and several `/control` JSON-encoding hardenings
  (control-char/UTF-8 handling, `\uXXXX` decoding, failing closed instead
  of shipping truncated JSON).
- **Motion detection**: IVS grid now uses pre-rotation frame dimensions
  with a transposed grid on the T23 SW-rotate path (was building the grid
  in the wrong orientation), sensitivity changes that map to the same IVS
  level are deduped, and `cooldown_ms` is floored and persisted correctly.
- **Day/night**: a queued ISP `running_mode` change is now actively
  latched (`fs_kick_running_mode`) instead of only taking effect on the
  next unrelated encoder event; a pre-switch hysteresis window (raptor-
  style) replaced blind reassertion; a transient reading during the AE
  settle window is now ignored instead of seeding a false decision.
- **Recording/timelapse**: `record.audio` toggles the hub subscription
  live; a dropped packet (not just a missed keyframe) now requests a
  rate-limited IDR; `gethostname()` results are NUL-terminated before use
  in path templates.
- Video/JPEG AU buffers now size from the actual frame instead of a fixed
  estimate (fixes both the 1.6.1 sub-stream stall class and an analogous
  JPEG/snapshot/MJPEG stall once a scene crosses a detail threshold — see
  below), audio speaker/backchannel gating and resampling edge cases, and
  a `/control` re-POST of an unchanged `image.running_mode` now still
  re-drives the ISP (some SoCs need the write even when the value didn't
  change).

## [1.6.4] - 2026-07-29

### Added
- **`{bitrate}` OSD text placeholder.** Reports the live measured
  throughput (kbit/s) of the monitored stream, mirroring the existing
  `{fps}` placeholder's mechanism and style. Shows `0` when the encoder
  has no active consumer rather than a frozen last-seen value.

### Fixed
- **JPEG snapshot/MJPEG/WebUI preview going permanently dark once a scene
  crosses a detail threshold.** `jpeg_thread()`'s buffer starts at a
  ~0.5 byte/pixel estimate, bounded to `[MS_JPEG_BUF_MIN, MS_JPEG_BUF_MAX]`
  — same class of bug as the `[1.6.1]` AU buffer fix, but unlike a video
  frame, nothing here shrinks a JPEG scene back down once it crosses the
  estimate (e.g. daylight bringing out more detail than a dawn/dusk
  scene), so every frame overflowed and got dropped forever from that
  point on: snapshots returned "no frame", MJPEG and the WebUI preview
  went dark, while day/night switching and RTSP video kept working fine
  (different codec/buffer entirely) — easy to mistake for a day/night bug
  from the WebUI. Now sums the pack lengths before assembly and grows the
  buffer (bounded by `MS_JPEG_BUF_MAX`) to fit the real frame, same as the
  AU buffer. Verified on real hardware (cam-L Y4, T23n): `/snapshot.jpg`
  went from HTTP 503 "no frame" with continuous buffer-overflow log spam
  to a valid ~450KB daytime JPEG, no overflow since.

## [1.6.3] - 2026-07-28

### Fixed
- **1-3s browser preview lag behind the physical camera (noticeable during
  PTZ).** Two independent contributors on the encoder→browser path:
  - The embedded MSE player JS (`src/mp4/httpd.c`) only corrected its
    live-edge position once it drifted more than 6s behind, jumping back
    to just 1s behind even then. With `autoplay`, the browser starts
    playback wherever it first had enough buffered data (typically 1-3s)
    and then plays at a flat 1x forever — that initial gap never shrunk on
    its own. Replaced the dead-zone jump with active drain:
    `playbackRate` now scales 1.0→1.3x with how far behind live the
    player is, settling at a steady-state ~0.5s behind live (kept as
    jitter margin), with a hard seek reserved for a large post-stall
    drift (>4s).
  - The HTTP/fMP4 listener never set `TCP_NODELAY` (the RTSP listener
    already did) — Nagle's algorithm held small fragments until the prior
    write was ACKed, adding up to ~200ms of pure transport latency per
    fragment, compounding across every video/audio fragment sent.
  GOP size/B-frames/rate-control and the encoder polling loop were
  reviewed and ruled out: no B-frames are used (no look-ahead latency),
  the poll timeout only bounds idle-wait and never delays an
  already-produced frame, and the fanqueue has no steady-state queuing
  delay. GOP interval affects only startup/post-drop recovery, not
  in-progress PTZ framing, so it was left unchanged (shrinking it further
  would trade bandwidth/quality for no benefit here). Verified on real
  hardware (Cinnado D1 T31L x2).

## [1.6.2] - 2026-07-28

### Fixed
- **Video encoder permanent stall when a framesource enable silently
  fails.** `fs_use()` never checked `IMP_FrameSource_EnableChn()`'s return
  value, and the enable only fires on the 0→1 user-count edge. If it fails
  once — or "succeeds" without actually arming the channel, a failure
  class this file already documents twice (the AI watchdog, and the T31
  `nrVBs` case) — `video_thread()`'s `StartRecvPic` still reports success
  and `PollingStream` spins at `rc=-1` forever: the encoder never produces
  another frame. With a client still subscribed, the idle-stop debounce
  never fires, so the framesource never gets a fresh enable attempt — a
  permanent stall recoverable only by restarting the daemon. Observed live
  on a T31L main channel (`nrVBs=1`, i.e. no buffer slack) after streaming
  correctly for hours; the sub-stream (independent framesource, ≥2
  buffers) kept working the whole time. Now mirrors the existing AI
  watchdog for video: after `MS_VIDEO_WATCHDOG_ITERS` (~5s) consecutive
  `PollingStream` misses, force a real Stop/Disable/Enable/Start cycle
  instead of spinning; `EnableChn` failures are now logged instead of
  silently swallowed. Verified on real hardware (Cinnado D1 T31L): full QA
  pass (77 PASS / 0 FAIL) after the fix, including 20/20 clean TCP and
  20/20 clean UDP reconnect cycles through the previously-fragile
  enable/disable edge.

## [1.6.1] - 2026-07-28

### Fixed
- **Sub-stream permanent stall on a large (e.g. complex-scene) IDR frame.**
  `video_thread()`'s AU assembly buffer starts at a ~0.5 byte/pixel
  estimate, clamped to `[MS_AU_BUF_MIN 128KB, MS_AU_BUF_MAX 1MB]`. For a
  small sub-stream (e.g. 640x360) that estimate sits at the 128KB floor,
  which a complex-scene IDR can exceed. The overflow handler used to drop
  the oversized frame and force a fresh IDR — but an IDR is the *largest*
  frame type, so the forced replacement overflowed too, forced another
  IDR, overflowed again: a permanent self-reinforcing stall that delivered
  zero decodable video on that stream from the first oversized frame
  onward (found as "only one stream works" — main stayed fine on its
  larger 1MB cap). Now sums the pack lengths before assembly and grows the
  buffer (bounded by `MS_AU_BUF_MAX`) to fit the real frame instead of
  truncating it; the `IMP_Encoder_RequestIDR()` call on the (now
  last-resort, >1MB-AU-or-failed-realloc-only) overflow path is dropped,
  since forcing an IDR there was the actual cause of the stall — a dropped
  frame already recovers via the existing
  `fanqueue_take_dropped_key`/`hub_request_idr` path on a real client.
  Verified on real hardware (cam-L Y4, T23n): the sub stream went from
  zero video across 20+ minutes and every reconnect to streaming
  correctly (640x360 h264@25+aac, IDR ~99KB) alongside the main stream,
  with zero overflow events since boot.

## [1.6.0] - 2026-07-27

### Added
- **Native speaker output (`IMP_AO`) — timps now owns the camera speaker
  directly, no more `/bin/iac`.** New `src/rtsp/speaker.c` is the sole
  `IMP_AO` owner and arbitrates two producers: the ONVIF **backchannel**
  (live RTP → PCM, always preempts) and a **system-sound play queue**
  driven by a FIFO at `/run/timps/audio_out` taking `PLAY url=<path>
  [vol= gain= rate= format= loop= delay=]` / `STOP` lines — the same
  protocol prudynt/raptor's `/usr/sbin/play` wrapper already speaks, so
  the WiFi captive-portal prompts, the post-upgrade chime and the ESPHome
  `media_player`/TTS integration all get a working speaker on a timps
  image for free. The play queue decodes Ogg-Opus (`opusfile`, gated on
  new `USE_PLAY_OPUS` like `USE_BC_AAC` gates the AAC backchannel), WAV,
  raw PCM16 and G.711 µ/A-law. New `USE_PLAY`/`USE_PLAY_OPUS` build flags
  (off by default; `USE_BACKCHANNEL` no longer needs `/bin/iac` present at
  all — `bc_available()` is always true once built in). New
  `hal_ao_open/write/close/set_vol/set_gain` in the HAL mirror the
  existing `IMP_AI` bring-up (rate-fallback loop, lazy open/close); a new
  `src/codec/resample.c` (extracted from `backchannel.c`) is shared by
  both producers.
- **Live speaker volume/gain + WebUI-driven system-sound play.**
  `audio.spk_volume`/`spk_gain` were parsed and persisted but never
  actually reached the hardware before; now every `IMP_AO` open applies
  them, and `POST /control {"audio":{"spk_volume":..,"spk_gain":..}}`
  writes through live (`caps.audio` gains the two keys, gated on an AO
  pipeline being compiled in). `GET /control` gains
  `caps.play={available,sounds:[...]}`, enumerated from
  `/usr/share/sounds` (`.wav` always listed since the µ-law/PCM decoder
  needs no library; `.opus` only when `USE_PLAY_OPUS` is actually built,
  so the list never offers a file this exact build can't decode).
  `POST {"speaker":{"play":"<file>"}}` / `{"stop":1}` enqueues on the FIFO
  after validating the name against that directory (rejects `/`, `..`,
  non-regular files) — this is what drives the thingino WebUI's
  test-sound dropdown and live speaker volume slider.
- **Day/night: time-window and sunrise/sunset override modes.** The
  native detector could previously only decide from the ISP sensor
  (`total_gain`/brightness). `daynight.mode` (`sensor`/`time`/`sun`, a
  string token) adds two sensor-independent modes: **`time`** forces by
  the local wall clock — a fixed `[time_night_start .. time_day_start]`
  `"HH:MM"` window, wrapping past midnight (e.g. night 20:00, day 06:30).
  **`sun`** forces by today's real sunrise/sunset for
  `sun_latitude`/`sun_longitude` via the standard low-precision sunrise
  equation (pure math, UTC epoch throughout), each edge shiftable by
  `sun_sunrise_offset_min`/`sun_sunset_offset_min` (negative allowed);
  polar day/night degenerate cases fall back to permanent day/night
  instead of NaN. `sensor` stays the default and its gain/brightness
  branch is untouched; `time`/`sun` reuse the existing switch + minimum-
  dwell machinery, and `daynight.enabled=0` (manual) still suppresses
  forcing in all three modes. `GET /control` exposes the new config
  fields plus today's computed sunrise/sunset (`sun_computed_sunrise`/
  `sunset`, local `"HH:MM"`) so a UI can sanity-check the configured
  lat/long before trusting it.
- **T31(L) `nrVBs` buffer-count override** (`video.buffers`, raptor-style):
  the T31 non-scaled-channel safety clamp (see Fixed below) now only
  applies to the *default* buffer count — an explicit `buffers=` in
  `timps.conf` is trusted as-is (with a warning, since a bad value fails
  silently down in the kernel/dmesg), letting a board/sensor combination
  that doesn't hit the constraint opt out without patching code.

### Fixed
- **Play-file tail no longer cut short on normal end-of-clip.** Two
  layered bugs, both in `hal_ao_close()`'s drain path: (1) it
  unconditionally discarded the AO ring buffer (`IMP_AO_ClearChnBuf`) on
  every close, including a clip finishing normally, not just on
  stop/preempt — a `drain` flag now distinguishes the two, discarding
  immediately only on stop/preempt/backchannel-takeover. (2) the drain
  path's fixed sleep (one `MS_AI_FRM_NUM`-period ring's worth, ~0.24 s)
  assumed that was the whole story, but the IMP AO keeps its own
  playback cache on top of that ring — the real residual is ~0.7 s, so
  the fixed sleep still closed the channel ~0.5 s early (e.g.
  "Configuration portal is down" stopped after "portal"). Now uses
  `IMP_AO_FlushChnBuf`, the SDK's "wait for the last segment to finish
  playing" primitive, which blocks until the whole cache has actually
  reached the DAC regardless of depth. Verified acoustically via RTSP
  mic loopback across clips from 0.6 s to 2.7 s, before/after audible
  span matched against each source clip's real content window.
- **T31(L) `nrVBs=1` clamp scoped to `PLATFORM_T31` only.** The non-
  scaled-channel buffer-count safety clamp in `fs_create()` (shared
  across every SoC family) fired for any chip's channel requesting >1
  buffer at native sensor resolution, but the kernel constraint requiring
  it was only ever observed on T31(L) — T10/T20/T21/T23/T30/T40/T41/C100
  now keep their untouched 2-buffer default.

## [1.5.0] - 2026-07-26

### Changed
- **Default `http.port` moved 8080 → 8880.** Port 8080 clashed with the
  ONVIF daemon (`onvif_srvd`), which also listens there; whichever bound
  first won, so ONVIF could fail to start when timps grabbed 8080. timps now
  defaults to `8880`, leaving 8080 to ONVIF. The port is still configurable
  via `http.port`; the WebUI reads the live port from `/x/timps-token.cgi`,
  so browser pages follow automatically.
- **Sub-stream OSD default `font_size` 24 → 12 px.** Better fit on typical
  sub-stream resolutions; still an absolute px value, not auto-scaled — see
  `osd1.*` in `timps.conf.example`.

### Added
- **Optional image rotation** (`USE_ROTATE`/`USE_SW_ROTATE` build flags,
  `videoN.rotation` config key: `0|90|180|270`). 180° works on every SoC;
  hardware 90/270 on T31/T40/T41; software 90/270 (CPU transpose + SW
  JPEG/OSD) on T23. Restart-required; downstream (encoder, RTSP SDP,
  fMP4/MP4, OSD, snapshots) all use the post-rotation dimensions via one
  helper. Off by default, ~0.2 KB when disabled. Known limitation: on T31,
  90/270 can't carry a hardware OSD/privacy overlay (libimp IPU-OSD stride
  bug) — see `docs/rotation.md`.
- **Optional ONVIF audio backchannel** (`USE_BACKCHANNEL`/`USE_BC_AAC` build
  flags, `audio.backchannel`/`backchannel_codec`/`backchannel_rate` config
  keys, `caps.backchannel.available`). Implements ONVIF Profile T two-way
  audio: an RTSP client streams RTP audio (PCMU/PCMA pure-C, or AAC via
  libhelix-aac) to the camera; timps decodes + resamples it to PCM16 and
  pipes it to `/bin/iac -s` (thingino's `ingenic-audiodaemon`) — timps itself
  never opens `IMP_AO`, so it works identically on every SoC as long as the
  audiodaemon is installed. See `docs/backchannel.md`.
- **Optional HTTPS + SRT (compile-time gated).** New `USE_TLS` (mbedTLS) and
  `USE_SRT` (libsrt) build flags, auto-enabled by the buildroot package
  selection (`BR2_PACKAGE_MBEDTLS` / `BR2_PACKAGE_LIBSRT`) - if the lib isn't in
  the image nothing changes. `USE_TLS`: a small mbedTLS wrapper (`src/tls.c`)
  behind which the HTTP server can run **HTTPS** (`http.https` + `http.tls_cert`
  / `http.tls_key`); the httpd I/O now goes through a TLS-aware send/recv layer
  that is byte-for-byte the old plain path when `USE_TLS` is off. `USE_SRT`:
  MPEG-TS over SRT output in listener mode (`src/srt.c`, `srt.enabled`/`port`/
  `channel`/`latency_ms`/`streamid`/`passphrase`) served from the hub like the
  recorder. Config keys for RTSPS (`rtsp.tls`/`rtsp.tls_port`) are parsed and
  reserved. NOTE: the TLS and SRT code paths cannot be built in the x86 sim
  (no mbedTLS/libsrt) - the default build stays verified; the TLS/SRT paths and
  the hand-rolled TS muxer need on-device verification.
- **Local recording to SD** (`record` section + `/control` action): records one
  video stream (+AAC audio) to `<dir>/<hostname>/records/<strftime>.mp4` as
  fragmented MP4, reusing the `/stream.mp4` muxer (`src/record.c`). Modes:
  `continuous` or `motion` (pre-roll ring from the keyframe before the trigger +
  `post_roll_s` after the last motion). Segments rotate every `record.segment_s`
  at a keyframe; oldest files are pruned to keep `record.min_free_mb` free.
  `GET /control` reports a `record` status object (recording/channel/mode/bytes/
  free_mb/file) and `caps.record`; `{"record":{"active":1|0}}` is a manual
  start/stop override (the WebUI record button). thingino path defaults
  (`/mnt/mmcblk0p1`, `<host>/records/` tree). Verified end-to-end in the x86 sim
  (valid MP4 segments via ffprobe).
- **Privacy cover masks** (`privacy` section, `/control` + config): solid filled
  rectangles per video stream (`privacy<S>.<N>.{enabled,x,y,w,h,color}`, up to
  `MS_MAX_PRIVACY` per stream) that black out sensitive areas, implemented as IMP
  OSD cover regions in the per-stream OSD group. Applied LIVE (create/show/hide/
  move without a restart, as long as OSD or a privacy region was on at startup)
  and persisted. `GET /control` dumps the `privacy` tree and advertises
  `caps.privacy = {available, max_regions}`. Replaces the prudynt-era WebUI
  privacy page's dependency on the `json-prudynt.cgi` bridge. NOTE: the IMP cover
  region call in `imp_osd.c` uses the common SDK form and needs on-device
  verification against the exact `<imp/imp_osd.h>` coverData layout.
- **Token now also unlocks HTTP media viewing** (`USE_CONTROL` builds): the
  `/control` token (per-boot `http.token_file` + optional persistent
  `http.token`, same constant-time check) is accepted on `/stream.mp4`,
  `/stream.mjpeg` and `/snapshot.jpg` (incl. `?chn=N`) as `?token=` — the
  only form an `<img>`/`<video src>` can use — or `X-Timps-Token`. This lets
  the thingino WebUI previews load the streams DIRECTLY from the HTTP port
  (no on-device proxy CGIs) even with `http.user` set. Media access is now
  localhost ∨ token ∨ Basic ∨ open-when-no-user — the existing rules are
  unchanged, the token is a pure addition; it still never unlocks RTSP, and
  non-media paths (`/` player, bogus paths) are NOT unlocked by a token.
  The media endpoints also answer the CORS `OPTIONS` preflight now, and
  `/stream.mjpeg` + `/snapshot.jpg` responses carry
  `Access-Control-Allow-Origin: *` like `/stream.mp4` always did, so
  cross-origin `fetch()`es of all three work. Caveat as with `/events`: a
  query token can end up in access logs — accepted on a LAN.
- **`GET /events` SSE push stream** (`USE_CONTROL` builds): a long-lived
  `text/event-stream` that PUSHES JSON state instead of being polled —
  `event: motion` (the `/control` motion object, emitted when the active
  grid/enabled/geometry/sensitivity changed), `event: daynight` (the
  `/control` daynight object, on a mode flip or ≥1 % brightness / ≥5 % gain
  move) and a periodic `event: stats`
  (`{"uptime_s","clients","video":[{"chn","subs","fps"},…]}`, every
  `events.stats_ms`). `?stream=motion,daynight,stats` filters the types
  (default all). Same auth as `/control` (localhost / token / Basic, CORS +
  OPTIONS preflight); the token is also accepted as `?token=` because
  EventSource cannot send headers. On connect: `retry: 3000`, a
  `: connected` comment and the full current state once; afterwards
  per-connection dedup (last-sent snapshot per event type) plus a `: ping`
  keepalive (~12 s) that doubles as dead-client detection. New tiny notify
  hub `src/events.c/.h` (generation counter + `CLOCK_MONOTONIC` condvar):
  `events_notify()` is called from the IVS result thread (grid changed,
  start/stop), the day/night sampler (real changes only) and `/control`
  writes to `motion.*`/`daynight.*`/`image.running_mode`; it is a no-op stub
  without `USE_CONTROL`, so every build permutation still links. Config:
  `events.enabled` (default 1), `events.stats_ms` (default 2000, 0 = off),
  `events.max_clients` (default 8; beyond → `503`, so an /events flood
  cannot exhaust the HTTP connection threads). The status-object JSON is
  built by shared helpers (`control_motion_json`/`control_daynight_json`),
  so `/control` and `/events` emit identical shapes by construction. The
  thingino WebUI preview overlay now subscribes to `?stream=motion` (with a
  4 Hz `/control` polling fallback) instead of polling.
- **Grid motion detection (IMP_IVS)**: the single detection ROI became a
  configurable `motion.cols` × `motion.rows` GRID of IMP_IVS move-ROIs laid
  evenly over the `motion.monitor_stream` frame (integer pixel split, the last
  row/column absorbs rounding; cell index row-major = `row*cols+col`).
  `cols*rows` is clamped to the SDK's compile-time `IMP_IVS_MOVE_MAX_ROI_CNT`,
  taken from the `imp_ivs_move.h` being built against via the new
  `motion_caps.h` (`MOTION_AVAILABLE`/`MOTION_MAX_CELLS`): 52 on most SDKs,
  **4** on the old T10/T20 3.9.0 SDK (grid defaults 5×5, 2×2 on 4-cell SDKs).
  The UI sensitivity 0..255 maps to IMP's 0..4 normal-camera range (one global
  value for all cells for now). SDKs without the move API compile a no-op stub
  and report the feature unavailable. The IVS group is now explicitly bound to
  the monitor stream's FrameSource (FS→IVS, unbound on stop) and the move
  interface is released via `IMP_IVS_DestroyMoveInterface` (both were missing).
- **Live motion control + status**: `motion.enabled`/`sensitivity`/`cols`/
  `rows`/`monitor_stream` are settable via `/control` and applied LIVE — the
  HAL cleanly stops and recreates the IVS channel (move params are create-time
  attributes). `cooldown_ms`/`on_motion` stay config-file only (`on_motion`
  runs through `system()`). `GET /control` gained `caps.motion`
  (`available`, `max_cells`) and a read-only `motion` status object:
  `{"available","enabled","cols","rows","max_cells","sensitivity",
  "monitor_stream","active":[0/1,... row-major, length cols*rows],
  "last_ms"}` (`last_ms` = ms since the last motion event, -1 = never). The
  thingino WebUI polls it directly on `:8880` with the `/control` token to
  draw a live grid overlay on the preview.

- **Token auth for `/control`**: the endpoint now allows any one of localhost
  (unchanged), a valid token, or HTTP Basic (unchanged). Tokens travel as an
  `X-Timps-Token:` header (preferred) or `?token=` query parameter, are
  compared in constant time and only unlock `/control` — never the streams.
  A random 128-bit per-boot token is generated from `/dev/urandom` and
  published to `http.token_file` (default `/run/timps.token`, mode 0640,
  `""` disables) so a local privileged helper (the thingino WebUI) can hand it
  to its authenticated browser session; an optional persistent `http.token`
  secret is also accepted for remote automation and is never written to the
  token file.
- **CORS on `/control`**: `OPTIONS` preflight (204, answered before auth — a
  preflight carries no credentials) and reflection of the request's `Origin`
  (+ `Vary: Origin`, `Access-Control-Allow-Headers: X-Timps-Token,
  Content-Type`, methods, max-age) on `/control` responses, so a browser page
  on another port (WebUI on `:80`) can call `:8880/control` directly with the
  token. `Access-Control-Allow-Credentials` is deliberately never sent.

### Fixed
- Target builds now pass `-I$(IMP_INC)/imp` too: the T10/T20 3.12.0 IVS
  headers include `<imp_ivs.h>` without the `imp/` prefix and did not resolve
  with `-I$(IMP_INC)` alone.
- **Command injection hardening**: `daynight.switch_cmd` (day/night switch
  script) and `motion.on_motion` (motion-trigger script) now run via
  `fork()`+`execlp()` instead of `system()` — no shell, so a value containing
  shell metacharacters just fails to exec instead of running as injected
  commands. Both keys were already config-file-only (never settable via
  `/control`), but this closes the gap for anyone with config-file write
  access. See `dev_notes/SECURITY_AUDIT_2026-07-23.md`.
- **Value clamping**: `audio.gain` now clamps to the IMP-documented mic PGA
  range (0..31, was 0..100); `audio.volume`/`alc_gain`/`spk_volume`/
  `spk_gain` and OSD `logo_w`/`logo_h`/`outline` are clamped against their
  real IMP/rendering limits so out-of-range `/control` values can't wrap or
  blow up an allocation.
- `hal_get()`'s return value is now NULL-checked before its first use at
  startup (previously dereferenced once, in the startup log line, before the
  existing check further down).

## [1.2.0] - 2026-07-11

### Added
- **Full ISP image control via `/control`**: the `image` section now covers the
  complete tuning set — brightness, contrast, saturation, sharpness, hue, h/v
  flip, running_mode, anti-flicker, AE compensation, max analog/digital gain,
  sinter & temper (noise), DPC, defog, DRC (WDR), highlight-depress (tone),
  backlight compensation and white balance (mode + R/B gain) — applied live via
  the matching `IMP_ISP_Tuning_*` call. A compile-time per-SoC capability matrix
  (`isp_caps.h`, T10–T41 + C100) is reported as `caps.image` so a UI can grey
  out what a given SoC cannot do; unsupported keys still persist.
- **Full audio control via `/control`**: live mic volume, gain, ALC gain,
  high-pass filter, AGC (+ target level / compression), noise-suppression, and a
  **live mic mute** (`audio.mute` — captured frames are dropped before the
  encoder/hub, no restart). Capability matrix in `audio_caps.h` → `caps.audio`.
  Codec / sample-rate / bitrate / channels persist and apply on restart.
  Speaker & forced-stereo have no IMP-AO path and are reported unsupported.
- **Full encoder & sensor control** (persist + restart): `video.N` accepts the
  whole per-stream key set (codec, width, height, fps, bitrate, rc_mode, gop,
  max_gop, profile, qp, min/max_qp, rotation, buffers, enabled, rtsp_path) and a
  new `sensor` section (model, i2c_addr, fps, width, height). These never touch
  the running pipeline; `GET /control` flags them in `caps.restart` and dumps
  the current values so a UI can populate.
- **Per-stream OSD**: every video stream has its own independent overlay set
  (`osd.items[stream][item]`). Canonical keys `osd<S>.<N>.<field>` (e.g.
  `osd0.0.text`, `osd1.2.x`); legacy `osd<N>.<field>` keys still load and mirror
  onto every stream. `/control` accepts `"osd0"/"osd1"` objects (live via
  `imp_osd_apply(stream,item)`) and still the shared legacy `"osd"` object.
- **OSD text outline/stroke**: new per-item `outline` (width px, 0 = off,
  default) and `outline_color` (`0xAARRGGBB`, default black). The TTF and
  embedded-bitmap rasterizers dilate the glyph coverage and blend the stroke
  under the fill; the region grows by the outline width. `caps.osd` lists the
  new leaves.
- **Day/night measurement exposed** (`daynight_get_status()`): the detection
  thread derives the **total gain** from the isp-m0 gain fields (IMP log2 units)
  converted to the `GetTotalGain` [24.8] linear scale (256 = 1×, matching what
  prudynt/raptor report), keeps sampling in manual mode, and `GET /control`
  reports `daynight: {enabled, mode, brightness%, total_gain}` (−1 = unknown; a
  stub answers unknowns without `USE_DAYNIGHT`).
- **System log output**: timps now also logs to syslog (tag `timpsd`) so
  messages appear in `logread` (the init script backgrounds timpsd, so its
  stderr is otherwise discarded). On by default; disable with
  `general.syslog = false`.

### Changed / Fixed
- **Idle CPU** (~19 % → ~0 with no clients): on-demand now stops the
  `IMP_FrameSource` channel (not just the encoder) once a stream has no
  subscribers — an enabled FrameSource kept capturing/piping frames through the
  FS→OSD→encoder groups in the libimp worker threads. Producer threads now block
  on a condition variable instead of a poll loop, and the OSD updater only
  renders while a stream has viewers. Reactivation is immediate; the monitored
  FrameSource is pinned while motion detection is enabled.
- **`GET /control` capabilities** now report `caps.{image,audio,osd,restart}` so
  UIs can present exactly what this build/SoC supports.

## [1.1.0] - 2026-07-11

### Added
- **Live control API** (`POST`/`GET /control`, compile flag `USE_CONTROL`, on by
  default). A nested JSON blob changes settings live *and* persists the changed
  keys back to the config file (atomic tmp+rename, comments/order preserved).
  Supported: `image` (brightness, contrast, saturation, sharpness, hue, hflip,
  vflip, running_mode), `audio` (volume, gain), `osd.N` overlays, `video.N`
  bitrate (persisted only — applies on restart). The legacy flat form and
  `{"force_mode":"day"|"night"}` still work. Requests from localhost bypass
  auth; remote access requires configured HTTP/RTSP credentials.
- **Native automatic day/night** (compile flag `USE_DAYNIGHT`, on by default). A
  background thread reads ISP brightness from `/proc/jz/isp/isp-m0` and applies
  threshold + hysteresis + transition-delay logic (ported from thingino's
  `daynightd`), switching via the board's `daynight day|night` script (IR-cut /
  IR-LEDs / colour). Runtime toggle through `/control`
  (`{"daynight":{"enabled":true|false}}`). New `daynight.*` config keys
  (`enabled`, `threshold_low`, `threshold_high`, `hysteresis`, `interval_ms`,
  `transition_s`, `switch_cmd`, `isp_path`).
- **Live OSD apply** (`imp_osd_apply`): OSD overlay changes made through
  `/control` are re-rendered on the running streams.
- `config_get_kv()` — read a config value back as a normalized string
  (used for change detection).

### Fixed / Hardened
- **`/control` change detection**: a value that does not actually change is no
  longer re-applied to the ISP nor rewritten to the config file. This stops
  clients that poll and re-post the same value every few seconds from hammering
  the ISP and, worse, rewriting the config on flash over and over.
- **`/control` input validation**: invalid values (`null`, `undefined`, empty)
  are rejected instead of being stored and parsed to `0`.
- **Config-injection defense**: persisted values are stripped of control
  characters and double quotes before being written to the flat config file.

### Build
- `USE_DAYNIGHT` added to the Makefile (target and host-sim recipes); both
  `USE_CONTROL` and `USE_DAYNIGHT` default on and can be disabled independently
  (`USE_CONTROL=0` / `USE_DAYNIGHT=0`), compiling the feature out entirely.

## [1.0.0]

### Added
- Initial import: Tiny IMP Streamer — pure-C RTSP + fragmented-MP4 + JPEG/MJPEG
  streamer for Ingenic SoC cameras, built straight on the vendor `libimp`
  (no live555 / libconfig / libwebsockets / libschrift). On-demand encoding,
  TrueType OSD, motion detection, RTSP-Digest / HTTP-Basic auth. Ingenic
  headers via the `ingenic-headers` submodule.

[1.2.0]: https://github.com/Lu-Fi/timps/compare/v1.1.0...v1.2.0
[1.1.0]: https://github.com/Lu-Fi/timps/compare/v1.0.0...v1.1.0
[1.0.0]: https://github.com/Lu-Fi/timps/releases/tag/v1.0.0
