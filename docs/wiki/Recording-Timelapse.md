# Recording & Timelapse

Both features write to local storage (typically the SD card) and are
built only with their respective flags (`USE_RECORD`/`USE_TIMELAPSE`,
both on by default — see [Building](Building.md)); without the flag, stub
functions keep call sites in `main.c`/`control.c` unconditional and
`GET /control` reports `"available":0`.

## Local recording (`src/record.c`)

Continuous or motion-triggered fragmented-MP4 recording, reusing the
**same fMP4 muxer** (`src/mp4/fmp4.c`) as the live `/stream.mp4` HTTP
preview — this is "raptor's RMR" for timps. Configured under `record.*`
(see [Configuration Reference](Configuration-Reference.md#record--local-sd-recording)).

### File layout

`<record.dir>/<hostname>/records/<strftime(record.name)>.mp4`, default
dir `/mnt/mmcblk0p1`, default name template `%Y%m%d/%H/%Y%m%dT%H%M%S`.
`gethostname()` falls back to `"camera"` on failure. Paths are validated
against `..` path components and an absolute `record.name` before every
open, since `record.dir`/`record.name` are themselves runtime-mutable via
`/control` and an authenticated caller could otherwise escape the
records tree.

### Modes

- **`record.mode = continuous` (0)** — always recording while
  `record.enabled`.
- **`record.mode = motion` (1, the default)** — records only while
  "recent" motion is true: [motion detection](Motion-Detection.md) is
  available, enabled, and the time since its last event is within
  `record.post_roll_s`. This single check elegantly covers both "motion
  is happening right now" and the post-roll tail in one condition.

A manual override (`{"record":{"active":1|0}}` via `/control`, the
WebUI's record button) takes precedence over both config and mode: `1`
forces recording on, `0` forces it off, and omitting/negative returns to
config-driven ("auto") behavior.

### Pre-roll (motion mode)

A circular ring buffer (256 packet slots, plus a hard 4 MB byte backstop
independent of slot count) continuously retains recent packets — refcounted,
not copied — while motion mode is not actively writing. When a recording
transition to "writing" happens, the ring is scanned **oldest first** for
the first video keyframe still buffered, and everything from there
forward is flushed into the new segment, so a motion-triggered clip
always starts cleanly at a keyframe rather than mid-GOP — and as far back
as `record.pre_roll_s` allows. (Scanning newest-first instead would start
at the *most recent* keyframe and silently cap every pre-roll at roughly
one GOP, whatever `pre_roll_s` said; that was the behaviour until
`dd7946a`.) If the pre-roll window configured
(`record.pre_roll_s`) exceeds what the ring can actually hold
at the stream's current bitrate/fps, a one-time warning is logged (the
ring silently truncates otherwise).

### Segment rotation

If `record.segment_s > 0`, a segment is closed and a new one opened as
soon as the current packet is a **video keyframe** and the elapsed time
since the segment started reaches `segment_s` — rotation only ever
happens at a keyframe boundary, never mid-GOP. `segment_s = 0` disables
rotation (one continuous file for as long as recording stays active).

### Free-space pruning

Before opening any new segment, if `record.min_free_mb > 0` and free
space on `record.dir` is below that threshold, the globally **oldest**
regular files anywhere under `<dir>/<hostname>/records` are deleted until
the threshold is met or no candidate remains — one directory walk
collects the 32 oldest at a time (rather than re-walking the whole tree
per deleted file, which on a full card costs dropped frames), and
traversal uses `lstat`, never following a symlink out of the tree.

**Pruning is not unconditional.** The same walk that collects candidates
also totals the size of the records tree, and before deleting anything
the first walk asks whether the target is reachable at all: if
`free_mb + everything under the records tree` is still below
`min_free_mb`, no amount of deleting gets there — the usual cause is a
`min_free_mb` larger than the card. In that case **nothing is deleted**,
and instead:

- `seg_open()` fails, so no segment is opened at all: `recording` stays
  `0` even with `record.enabled=1` or a manual
  `{"record":{"active":1}}` override — the daemon refuses to record
  rather than empty the archive chasing a target it can never hit;
- one warning is logged (`record.min_free_mb=<N> unreachable (only <F>MB
  free, <M>MB even if every existing recording were deleted)`), once per
  bad-value edge rather than once per attempted rotation;
- the refusal is surfaced through the same `write_errors` / `last_error`
  fields `record_get_status()` uses for a write failure, so `GET
  /control`'s `record` object explains it without an SSH session:
  `"write_errors"` increments and `"last_error"` reads
  `min_free_mb=<N> unreachable (max <M>MB)`;
- and the answer is re-evaluated at most once every 5 seconds. While
  nothing is open, `seg_open()` runs once per incoming packet (~31/s
  measured), so a retry inside that window reuses the known refusal
  and costs a single `statvfs`, not another tree walk. Space appearing by
  some other route (a manual delete, a config fix) is therefore noticed
  within 5 seconds, not immediately.

This is a deliberate behaviour change: earlier versions deleted the
oldest file over and over until the archive was empty and then recorded
anyway. If a camera reports `"recording":0` together with a
`min_free_mb ... unreachable` `last_error`, the fix is to lower
`record.min_free_mb` (or point `record.dir` at larger media), not to
delete recordings by hand — the daemon already knows deleting them would
not be enough. A failed `statvfs` is not a refusal: free space simply
cannot be measured, and recording proceeds as if no threshold were set.

### Durability

Every ~5 seconds, an open segment gets a non-blocking `fflush()` +
`sync_file_range()` (bounding data loss on a power cut to roughly that
window without risking a slow SD card stalling the writer thread and
dropping frames); the real blocking `fsync()` happens once, at segment
close. A short write (SD card yanked, disk full) closes the segment
immediately rather than continuing to falsely report "recording."

### Boot-snapshot geometry (important detail)

Both the rotating recorder and on-demand clip capture (below) build the
fMP4 container using **`g_cfg_boot`** — the codec/dimensions/fps the
encoder was actually started with — never the live, possibly-edited
`g_cfg`. This mirrors the same rule
[Configuration Reference](Configuration-Reference.md) documents for
RTSP SDP/HTTP fMP4: those fields are restart-only, so describing them
from a live edit would desync the container's declared format from the
bytes the encoder is actually producing.

### Audio

`record.audio=1` (default) mixes AAC audio into the recording when the
hub reports an AAC track; **G.711 cannot be muxed into fMP4** (same
limitation as the HTTP preview), so a G.711 configuration is warned about
once and recordings come out video-only. Toggling `record.audio` via
`/control` is reconciled live even mid-recording (an earlier version
could leave a broken empty AAC track, or leak a stale subscription, when
toggled while a segment was open).

### On-demand clip capture

`{"record":{"clip":"<path>","seconds":N}}` (via `/control`, used by
send2/Telegram-style motion notifications) captures a short fMP4 clip
completely independent of the rotating SD recorder — it works even with
`record.enabled=0`, since subscribing to the hub wakes the shared encoder
on demand. Notable safety details:

- `path` must live under `/tmp/` and contain no `..`.
- `seconds` is clamped to 1–30 (default 6).
- A single concurrency guard (`trylock`, not a queue) rejects a second
  concurrent clip request outright rather than stacking parallel
  RAM-backed captures during a burst of motion events.
- The output file is opened `O_CREAT|O_EXCL|O_NOFOLLOW` — never follows a
  pre-planted symlink, never clobbers an existing file.
- Recording starts only once both a video keyframe *and* a ready
  parameter set (SPS/PPS) have arrived, and gives up after
  `seconds + 5` if neither ever does.

### Status

`GET /control`'s `"record"` object reports `available`/`enabled`/
`recording`/`channel`/`mode`/`bytes` (current segment size)/`free_mb`/
`file` (current path, empty when idle), alongside the persisted config
values so a WebUI can populate its record settings page from the same
read.

It also carries the answers to "it says enabled, so why is nothing being
written?": `motion_gate_available`/`motion_gate_enabled` (motion mode
with the detector structurally inert, versus merely quiet),
`manual_off` (the manual override latched off), and the write-path health
triple `write_errors` (count since start) / `last_error_age_s` (seconds
since the last one, `-1` = never) / `last_error` (its text). A dying or
full SD card, and the unreachable-`min_free_mb` refusal above, both
announce themselves there rather than only in a log ring that recycles in
hours.

## Timelapse (`src/timelapse.c`)

Periodic native JPEG capture: every `timelapse.interval_s`, grabs the
current frame from a stream's JPEG source and writes it to
`<timelapse.dir>/<hostname>/timelapses/<strftime(timelapse.name)>.jpg`.
Structurally similar to the recorder (own thread, gated by `enabled`,
idles unsubscribed while off) but with no pre-roll/manual-override/
on-demand-clip equivalents. Configured under `timelapse.*` (see
[Configuration Reference](Configuration-Reference.md#timelapse--periodic-jpeg-capture)).

### JPEG source selection

Mirrors the same selection logic `/snapshot.jpg?chn=N` uses: prefer
`timelapse.channel`'s piggyback JPEG encoder if that stream is
boot-enabled with `jpeg_enabled=1`; else the dedicated `jpeg.*` channel;
else the first stream with any piggyback JPEG encoder; else no source
(the thread idles).

### Just-in-time capture (avoiding a wasted always-on pipeline)

Because timps encodes nothing while idle, subscribing to a JPEG hub
source is exactly what wakes the shared encoder/framesource — so a naive
"stay subscribed forever, take one frame every interval" design would
otherwise keep the pipeline running 24/7 and discard nearly every encoded
frame between shots. Instead, each shot:

1. Subscribes to the JPEG source and waits up to 1.5s for a fresh JPEG
   packet.
2. If nothing arrives on a **piggyback** source (a genuinely cold-start
   case), it additionally subscribes — with a tiny, immediately-discarded
   queue — to the *parent video stream* itself, exactly mirroring how an
   RTSP/HTTP client brings up a shared, currently-idle framesource/encoder
   group, then waits another 1.5s.
3. Both subscriptions are released once the frame is captured (the helper
   video subscription first), letting the pipeline's normal ~2s idle-stop
   debounce shut it back down between shots.

Intervals shorter than roughly 5s therefore never cause the pipeline to
fully stop between consecutive shots (they stay inside the idle-stop
debounce window), avoiding needless start/stop churn on a fast timelapse
schedule. On a failed grab, the retry backs off to 5s (or `interval_s` if
smaller) rather than waiting out the full configured interval.

### Writing and retention

Shots are written to a `.tmp` file first, then atomically `rename()`d
into place — readers never see a partial JPEG. If `timelapse.keep_days >
0`, every *successful* shot triggers a recursive prune of `*.jpg` files
older than `keep_days*86400` seconds under the timelapses tree
(directory-depth-bounded), removing any directory left empty afterward.

### Status

`GET /control`'s `"timelapse"` object reports `available`/`enabled`/
`interval_s`/`count` (shots since daemon start)/`last_t` (unix time of
the last shot, `0` = never)/`free_mb`/`file` (last written path).
