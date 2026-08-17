# Testing / QA

## `make test-auth` (fast, no hardware)

A self-contained authentication regression test: builds `timpsd-sim` with
`USE_CONTROL=1`, starts it on unprivileged ports against
`scripts/test_auth.conf`, runs `scripts/test_auth.sh`, then stops it and
propagates the exit code. RTSP auth is fully exercised over loopback; the
HTTP negative tests are skipped over loopback by design (the HTTP server
trusts `127.0.0.0/8`) — point `scripts/test_auth.sh` at a real device's
LAN IP to exercise those. See [Building](Building.md#self-test-target).

## `scripts/timps-qa.sh` — full QA harness against a real camera

`timps-qa.sh` is a ~1200-line bash harness that drives a **real timps
camera** over the network (RTSP + HTTP + ONVIF), optionally with SSH for
on-device checks, and produces a PASS/WARN/FAIL summary. It is the tool
to run before/after any change that could affect stream stability, timing,
or the `/control` API's live-apply behavior.

### Running it

```sh
./scripts/timps-qa.sh --cam 192.168.1.100
./scripts/timps-qa.sh --cam 192.168.1.100 --ssh root@192.168.1.100 --profile standard
./scripts/timps-qa.sh --cam 192.168.1.100 --profile quick   # ~3 min smoke test
./scripts/timps-qa.sh --help
```

Every flag is also settable as an environment variable (`CAM=...`,
`LOAD_DUR=...`, etc.).

**Host-side prerequisites**: `ffmpeg` + `ffprobe` + `curl` are required
(checked in the Preflight section; a missing `ffprobe` aborts the stream
tests). `python3` is optional but needed for JSON parsing in the
live-settings round-trip test (falls back to `grep` otherwise) and for
the backchannel test scripts. `openssl` is used for ONVIF WS-Security
digest auth (without it, ONVIF calls go out unauthenticated). `--ssh`
expects **key-based auth already configured** (it runs with
`BatchMode=yes`, so it fails rather than prompts for a password) — no
key-setup step is scripted; that's a prerequisite the operator handles
separately.

### What it checks, section by section

1. **Preflight** — required tools present, optional SSH connectivity,
   camera reachability (ping is warn-only, not fail — it may be
   firewalled), and that the RTSP/HTTP TCP ports are actually open.
2. **Discovery (ffprobe)** — probes both the main (`ch0`) and sub (`ch1`)
   RTSP streams for codec/resolution/fps/audio, recording nominal fps for
   later comparison.
   - **2b Auth enforcement** — both negative (unauthenticated/wrong-password
     requests must be rejected on `/control`, `/events`, `/snapshot.jpg`,
     `/stream.mp4`, `/stream.mjpeg`, and RTSP `DESCRIBE`) and positive
     (correct credentials must be accepted) checks, so "everything is
     blocked" can't false-pass the test.
   - **2c Backchannel handshake** (default-on unless excluded via
     `--only`) — a real ONVIF backchannel handshake via `scripts/bc-send.py`
     (DESCRIBE+Require → SETUP trackID=2 → PLAY, then streams a short
     PCMU tone); a disabled/missing backchannel is reported as info, not
     a failure.
   - **2d Backchannel acoustic loopback** — **opt-in via
     `--test-backchannel`**, never run by a profile by default: plays a
     tone into the speaker over the backchannel while simultaneously
     recording the camera's own outgoing mic audio over RTSP, then
     measures the tone-frequency band energy against a quiet control band.
     A ≥12dB delta passes; 5–12dB warns; below 5dB fails (explicitly
     flagged as possibly environmental rather than a code defect — mic/
     speaker placement, acoustic isolation, etc.).
3. **Stream integrity + A/V sync** — records each stream for
   `--integ-dur` seconds and measures packet counts, real-time playback
   rate (catches stalls or wrong-clock fast-forward), max inter-packet
   gap (freeze detection), timestamp monotonicity, A/V drift measured
   after a warmup period (to exclude the startup transient), audio/video
   pace ratio (catches sample-rate mismatches), and counts ffmpeg
   decode/timestamp warnings. Compares measured fps against the nominal
   fps from Discovery.
   The main stream is additionally analysed once over **UDP** transport
   explicitly (skipped when the run is already `--transport udp`): the UDP
   send path has its own batching and its own orphaned-session reaping, and
   no `SO_SNDTIMEO` backstop is even possible there, so it needs its own
   pass rather than inheriting whatever `--transport` the run used.
4. **HTTP fMP4** (`/stream.mp4?chn=0`) — the same integrity analysis as
   section 3, over HTTP instead of RTSP.
5. **MJPEG** (`/stream.mjpeg?chn=0`) — records and computes actual fps,
   fails on zero delivered frames.
6. **Snapshot** (`/snapshot.jpg`, both channels) — fetches multiple
   snapshots, validates HTTP 200 + JPEG magic bytes + minimum size,
   tracks latency.
7. **Audio** — codec/sample-rate/channel-count probe (with
   `--expect-channels N` as a hard assertion), plus a silence-detection
   scan flagging any gap longer than 1.5s.
8. **`/control` API** — basic read + safe write round-trip (reads
   `image.brightness`, POSTs the same value back, checks 200).
   - **8b Live settings apply + read-back verify** — the most thorough
     section: for every live-applicable `/control` key (across `image`,
     `audio`, `osd0.0`, `privacy.0.0`, `motion`, `daynight`, `record`,
     `timelapse`), POSTs a new value, `GET`s it back, asserts the change
     took, restores the original, and confirms the revert — end-to-end
     proof of JSON-parse → `config_apply_kv` → HAL live-apply → reported
     state. Also does a **persist-only sanity check** on `video.0.bitrate`
     (must round-trip through config even though only a restart applies
     it). **Rotation** round-trip testing is **opt-in via
     `--test-rotation`**: gated on whatever `caps.rotation` this SoC's
     build actually advertises, testing each supported value and, for
     90/270, verifying `eff_width`/`eff_height` swap correctly.
   - **8f Pixel-verified hflip + forced chn0 relatch** — **opt-in via
     `--test-flip`**, needs a static scene: mirrors the `hflip=0` snapshot
     and PSNR-compares it against the `hflip=1` snapshot, so the flip is
     proven to change actual pixels rather than merely round-trip through
     `/control`. Then idles the pipeline past `MS_IDLE_STOP_US`, attaches a
     fresh client to force a chn0 re-latch, and re-checks that the flip
     survived. Ambiguous PSNR deltas warn rather than fail.
   - **8g Encoder settings verified after a restart** — **opt-in via
     `--test-encoder`**, needs `--ssh`: first **measures** what the
     **substream** actually delivers under its current config, then cuts the
     bitrate target to ~0.4x of that measured rate (plus `rc_mode`), restarts
     the daemon for real, and measures again. Aiming *downward* matters: a
     bitrate target is a ceiling, so raising it proves nothing on a stream
     that is already quality- or content-limited (min_qp floor, static
     scene) — a lower ceiling always binds, which is what makes "the encoder
     followed it" and "the encoder ignored it" distinguishable. This is
     the only check that can catch a value being accepted, persisted and
     faithfully echoed while the running encoder ignores or coerces it. Also
     surfaces the previously-unused `IMP_Encoder_Query` telemetry
     (`left_pics`/`work_done`/`ave_bitrate`). Restores and restarts again.
   - **8h Day/night transition** — **opt-in via `--test-daynight`**: sets
     `mode=time` with a window that contains the camera's current time,
     waits for the state to flip, asserts the board hook actually ran
     (logread, needs `--ssh`) and that `image.running_mode` followed, then
     inverts the window and asserts it comes back — counting switches in
     each direction to catch the overnight flap-loop regression. Physically
     clicks the IR-cut filter twice.
9. **`/events` (SSE)** — connects for 8 seconds and checks that some data
   streamed (an idle camera with no config changes may legitimately send
   nothing, so this warns rather than fails on silence).
10. **ONVIF** — skipped entirely if the ONVIF port isn't open. Checks the
    snapshot CGI proxies, `GetSystemDateAndTime` liveness, and
    `GetProfiles` (cross-checking advertised resolution/codec against the
    real stream, and flagging template-default frame-rate/bitrate limits
    that were never updated to match the real encoder).
11. **On-demand recording clip** — POSTs `{"record":{"clip":...,"seconds":4}}`
    and, with `--ssh`, verifies the resulting file exists and has a
    plausible size.
12. **Reliability — reconnect churn** — rapid connect/grab-frames/
    disconnect cycles (both TCP and UDP RTSP transport), tracking success
    rate and time-to-first-frames.
    - **12b Session reaping** — **opt-in via `--test-leak`**: starts a
      client, `kill -9`s it without a TEARDOWN, and requires the daemon's own
      subscriber count (from the `/events` stats frame) to return to
      baseline — within 150s for RTSP-UDP (2× the advertised session
      timeout) and 90s for HTTP fMP4 and SRT. Regression test for the
      immortal-session class of bug.
13. **Load — concurrent client ramp** — spawns increasing numbers of
    concurrent RTSP pulls of the main stream, tracking per-client and
    aggregate fps and (with `--ssh`) `timpsd`'s RSS and system load
    average before/after, reporting the highest client count with zero
    failures. Steps above the compiled-in `RTSP_MAX_CLIENTS` cap (8) are
    reported as *at cap* — correctly enforced admission control — rather
    than as degradation. fd/thread counts are sampled at each step and a
    monotonic rise across the ramp is failed as a leak.
    - **13b Hostile (stalled) client** — **opt-in via `--test-hostile`**,
      needs python3: runs healthy clients alongside one client
      (`scripts/rtsp-stall.py`) that completes the RTSP handshake over
      interleaved TCP and then stops reading — refreshing that session every
      12 s, since the server reaps a peer after 15 s of zero write progress and
      only the first half of the phase would otherwise be hostile. Asserts the healthy clients
      keep the frame rate **they measured in the baseline phase moments
      earlier** — isolation is a differential question, so comparing against
      nominal fps instead would just re-report section 3's "this SoC does not
      sustain its configured fps" finding as a bogus isolation failure — that
      their keyframe rate does not spike (the global IDR-request rate limiter
      must stop one slow client from forcing keyframes on everybody) and that
      memory stays bounded.
14. **Restart resilience** — **opt-in via `--restart`**, requires
    `--ssh`: restarts the timps service and polls until the stream
    recovers, failing if it doesn't within 60s.
    - **14c Reboot persistence** — **opt-in via `--test-reboot`**, needs
      `--ssh`: makes one persisted config change, reboots the camera for
      real, and then verifies the change survived, that `/etc/timps.conf`'s
      and `/usr/bin/timpsd`'s md5s and the reported version string are what
      they should be, and that nothing else in the whole `/control` document
      silently reset at boot.
15. **Soak** — runs when a soak duration is configured (auto-enabled by
    `--profile soak`, or via `--soak-dur`): repeatedly records short
    slices for the full duration, tallying decode/timestamp warnings and
    (with `--ssh`) sampling RSS, **fd count, thread count and %CPU** at each
    slice. RSS growth is warned on; a monotonic rise in fds or threads is
    failed (that is the per-session leak signature, and it costs kilobytes
    rather than megabytes, so an RSS threshold never sees it); a rising CPU
    trend at constant load is flagged too.
    - **15b Long-session A/V drift** — **opt-in via `--drift-dur`** (or
      `--profile drift`, 2h): holds ONE unbroken RTSP connection open for
      the whole duration and measures A/V skew at `--drift-seg` checkpoints
      (default 5 min) using `-reset_timestamps 0` so segment timestamps stay
      session-relative and comparable. Judged on the **trend** (first-quarter
      vs last-quarter average, plus how monotone the movement is), not on
      any single sample — a drift that creeps to 0.35s over two hours never
      trips a snapshot threshold but is exactly the failure signature of a
      stale-timestamp bug. The absolute skew level is reported separately.
16. **On-device checks (SSH)** — confirms the expected `timps` version and
    that `timpsd` is alive, scans `logread` for error-ish lines **and
    separately for watchdog escalations** (`encoder dead`, `PollingStream
    idle`, `forced-recovery`, `no audio frames received`, `giving up on this
    channel` — none of which match the generic error grep, and all of which
    mean the daemon is up and answering while producing nothing), scans
    `dmesg` (the kernel/driver log, a different surface from `logread`)
    for driver-level trouble — anchored to the sensor bring-up line
    (`<sensor> stream on`) so only messages logged *after* streaming started
    are scanned, because the boot preamble on these boards unconditionally
    contains benign lines carrying `error`/`panic`/`watchdog` (the kernel
    version banner embeds a build-time linker message, `panic=2` is in the
    kernel command line, and the USB-OTG/SDIO probes always complain) —
    samples idle CPU/fd/thread counts with no clients attached, checks
    `/etc/timps.conf` for signs of a config-write bug (glued lines,
    duplicate keys), stress-tests 20 rapid concurrent `/control` POSTs
    toggling `audio.agc` (this specifically re-exercises a historical
    live-DSP-toggle use-after-free class that is now avoided by making
    `agc`/`ns`/`high_pass` restart-only — see [Audio](Audio.md)), and
    checks `dmesg` for segfault signatures.

### CLI flags

| Flag | Effect | Default |
| --- | --- | --- |
| `--cam IP` | Camera address (required) | — |
| `--profile quick\|standard\|load\|soak` | Preset tuning of durations/counts | `standard` |
| `--rtsp-user`/`--rtsp-pass` | RTSP credentials | `thingino`/`thingino` |
| `--http-user`/`--http-pass` | HTTP credentials | `thingino`/`thingino` |
| `--expect-channels N` | Hard-assert audio channel count | unset (report-only) |
| `--main PATH` / `--sub PATH` | RTSP stream paths | `ch0` / `ch1` |
| `--transport tcp\|udp` | RTSP transport for the integrity tests | `tcp` |
| `--ssh TARGET` | e.g. `root@IP`; enables all on-device checks | unset |
| `--integ-dur S` | Seconds recorded per integrity test | 30 (10 on `quick`) |
| `--load-dur S` / `--load-clients "N N ..."` | Load-test tuning | 30s / `"1 2 4 8"` |
| `--reconnects N` | Reconnect cycles per transport | 20 |
| `--snaps N` | Snapshot request count | 30 |
| `--soak-dur S` | Soak duration in seconds | 0 (off unless `soak` profile) |
| `--restart` | **Opt-in**: exercise a full service restart | off |
| `--drift-dur S` / `--drift-seg S` | Long-session A/V drift window and checkpoint interval (section 15b) | 0 (off) / 300 |
| `--only LIST` | Comma-separated section names/numbers to run | all |
| `--out DIR` | Output directory | `timps-qa-<timestamp>` |
| `--test-backchannel` | **Opt-in**: acoustic loopback test (section 2d) | off, never in a profile |
| `--bc-test-freq HZ` / `--bc-test-secs S` | Backchannel test tone tuning | 1500 / 4 |
| `--test-rotation` | **Opt-in**: rotation round-trip test (part of 8b) | off, never in a profile |
| `--test-flip` | **Opt-in**: pixel-verified hflip + chn0 relatch (8f) | off, never in a profile |
| `--test-encoder` | **Opt-in**: measured bitrate/rc_mode after a restart (8g), needs `--ssh` | off, never in a profile |
| `--test-daynight` | **Opt-in**: forced day/night transition + flap check (8h) | off, never in a profile |
| `--test-leak` | **Opt-in**: reaping of SIGKILLed clients (12b) | off, never in a profile |
| `--test-hostile` | **Opt-in**: stalled-client isolation + IDR-storm check (13b) | off, never in a profile |
| `--test-reboot` | **Opt-in**: real reboot, config/binary persistence (14c), needs `--ssh` | off, never in a profile |

**Profiles**: `quick` (~3 min smoke test), `standard` (~15 min, the
default), `load` (~20 min, wider client-count sweep), `soak` (standard +
a 2-hour soak run unless `--soak-dur` overrides), `drift` (standard + a
2-hour single-session A/V drift run unless `--drift-dur` overrides).
`soak` and `drift` are deliberately separate: both take hours and run
serially, so bundling them would silently double a soak run's wall time.

### Output

Each run creates `timps-qa-<YYYYMMDD-HHMMSS>/` (or the directory given by
`--out`) at the repository root, containing `summary.txt` (the full
colorized `[PASS]`/`[WARN]`/`[FAIL]` log) plus raw artifacts: ffprobe
JSON dumps, recorded segments + ffmpeg logs for every integrity check,
packet-timeline CSVs, `/control` JSON snapshots from the live-settings
round trip, the `/events` capture log, ONVIF/snapshot JPEGs, per-load-step
subdirectories, and reconnect/soak logs. The final summary line reports
`PASS=X WARN=Y FAIL=Z SKIP=W` and an overall `RESULT:` verdict; the
process exit code is `2` on any FAIL, `1` on any WARN with no FAIL, `0`
on a clean pass — suitable for wiring into CI-style gating.

Related scripts: `scripts/bc-send.py` (backchannel tone sender, used by
sections 2c/2d), `scripts/rtsp-stall.py` (the deliberately hostile
never-reads RTSP client used by section 13b, also useful by hand), `scripts/bc-talk.py` (interactive backchannel testing,
not invoked by `timps-qa.sh` itself), and `scripts/test_auth.sh` +
`scripts/test_auth.conf` (the deeper fail-closed auth audit `make
test-auth` runs, and referenced by section 2b as a companion for
transport-level negative testing beyond what `timps-qa.sh` covers
itself).
