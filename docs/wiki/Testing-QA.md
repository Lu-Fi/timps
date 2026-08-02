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
./scripts/timps-qa.sh --cam 192.168.241.190
./scripts/timps-qa.sh --cam 192.168.241.190 --ssh root@192.168.241.190 --profile standard
./scripts/timps-qa.sh --cam 192.168.241.190 --profile quick   # ~3 min smoke test
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
13. **Load — concurrent client ramp** — spawns increasing numbers of
    concurrent RTSP pulls of the main stream, tracking per-client and
    aggregate fps and (with `--ssh`) `timpsd`'s RSS and system load
    average before/after, reporting the highest client count with zero
    failures.
14. **Restart resilience** — **opt-in via `--restart`**, requires
    `--ssh`: restarts the timps service and polls until the stream
    recovers, failing if it doesn't within 60s.
15. **Soak** — runs when a soak duration is configured (auto-enabled by
    `--profile soak`, or via `--soak-dur`): repeatedly records short
    slices for the full duration, tallying decode/timestamp warnings and
    (with `--ssh`) sampling RSS at each slice to flag possible memory
    growth.
16. **On-device checks (SSH)** — confirms the expected `timps` version and
    that `timpsd` is alive, scans `logread` for error-ish lines, checks
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
| `--only LIST` | Comma-separated section names/numbers to run | all |
| `--out DIR` | Output directory | `timps-qa-<timestamp>` |
| `--test-backchannel` | **Opt-in**: acoustic loopback test (section 2d) | off, never in a profile |
| `--bc-test-freq HZ` / `--bc-test-secs S` | Backchannel test tone tuning | 1500 / 4 |
| `--test-rotation` | **Opt-in**: rotation round-trip test (part of 8b) | off, never in a profile |

**Profiles**: `quick` (~3 min smoke test), `standard` (~15 min, the
default), `load` (~20 min, wider client-count sweep), `soak` (standard +
a 2-hour soak run unless `--soak-dur` overrides).

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
sections 2c/2d), `scripts/bc-talk.py` (interactive backchannel testing,
not invoked by `timps-qa.sh` itself), and `scripts/test_auth.sh` +
`scripts/test_auth.conf` (the deeper fail-closed auth audit `make
test-auth` runs, and referenced by section 2b as a companion for
transport-level negative testing beyond what `timps-qa.sh` covers
itself).
