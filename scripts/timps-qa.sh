#!/usr/bin/env bash
# =============================================================================
# timps-qa.sh - comprehensive automated QA / stress test for a timps camera.
#
# Exercises the whole streamer from a Linux host over the network and reports
# on SYNCHRONISATION, STABILITY, RELIABILITY and LOAD:
#
#   1. Preflight ...... ping, ports, tool detection; stale /run/timps.crash check
#   1b. Build identity  GET /control "version" (git describe) vs local checkout
#   2. Discovery ...... ffprobe every stream (codec/res/fps/audio)
#   2b. Auth .......... no-auth + wrong-pass must be blocked (RTSP + HTTP surfaces)
#   2c. Backchannel ... optional ONVIF backchannel: handshake + short PCMU tone
#   3. Integrity+Sync . record each stream, measure fps, real-time rate,
#                       A/V drift, timestamp monotonicity, decode errors
#   4. HTTP fMP4 ...... /stream.mp4 (MSE feed) plays, fps/bitrate, errors
#   4b. SRT ........... optional: srt:// integrity via the same analyze_stream core
#   5. MJPEG .......... /stream.mjpeg frame rate + integrity
#   6. Snapshot ....... /snapshot.jpg?chn=N validity + latency + success rate
#   7. Audio .......... codec/rate, silence-gap scan
#   8. /control API ... status JSON, caps, safe write+persist round-trip
#   8b. Live settings . POST every live setting, read it back, verify applied,
#                       plus clamp regression (out-of-range persists clamped)
#                       and the videoN rate-control block: caps.video_live vs
#                       enc_caps.h, POST deferred/deferred_keys grading, and
#                       encoder.<n>.rc (what the encoder HOLDS) vs what was
#                       written - incl. i_bias_lvl->iIPDelta and per-channel
#                       isolation, all without a restart
#   8c. OSD vars_file . optional SSH: custom {placeholder} round-trip via the
#                       on-device osd.vars_file key=value mechanism
#   8d. Field drift ... diff GET /control?fields=1 (F_CTRL inventory) against
#                       8b's tested set + a documented allowlist
#   8e. Malformed body  5 negative-case POSTs against the hand-rolled JSON
#                       parser (liveness, not a strict error contract), the
#                       three fixed httpd request-handling defects, and the
#                       "ignored" reply array on a mixed known+typo body
#   8f. Flip/relatch .. optional: PIXEL-verified hflip + forced chn0 relatch
#   8g. Encoder ....... bitrate/rc_mode verified by MEASURING the substream,
#                       live where caps.video_live allows it, otherwise after
#                       a real restart (needs SSH only for the restart route)
#   8h. Day/night ..... optional: forced time-window transition both ways, hook
#                       invocation, running_mode follow-through, flap count
#   9. /events ........ SSE stream emits events, provoked by a live-settings POST
#  10. ONVIF .......... both snapshot proxies + GetProfiles (resolution/codec vs
#                       real stream, fps/bitrate surfaced with template note)
#  11. Recording ..... on-demand clip via /control record.clip
#  12. Reliability ... reconnect churn (TCP+UDP), time-to-first-frame
#  12b. Session reap .. optional: SIGKILLed UDP/HTTP/SRT clients must be reaped
#  13. Load .......... concurrent-client ramp, per-client fps/drops, max stable
#  13b. Hostile client  optional: one stalled client must not degrade the others
#                       (fps, keyframe rate / global IDR limiter, memory)
#  14. Restart ....... optional streamer restart + recovery time
#  14b. Fatal signal .. optional, DESTRUCTIVE: kill -SEGV + handler/restart verify
#  14c. Reboot ....... optional: real reboot; config/binary/version persistence
#  15. Soak .......... long capture with periodic health sampling
#  15b. Session drift  optional: ONE unbroken RTSP session for hours, A/V skew
#                      sampled at checkpoints, judged on TREND not snapshot
#  16. On-device ..... optional SSH: timpsd RSS/CPU/fds/threads, logread errors,
#                      watchdog-escalation grep, dmesg (kernel/driver) scan,
#                      /etc/timps.conf integrity (glued/duplicate keys)
#
# Everything is host-side (needs ffmpeg + ffprobe + curl). SSH is optional and
# only unlocks the on-device checks. All raw logs land in an output directory;
# a final table summarises PASS / WARN / FAIL and the process exit code
# reflects the worst result.
#
# Usage:   ./timps-qa.sh --cam 192.168.1.100 [--profile standard] [options]
#          CAM=192.168.1.100 ./timps-qa.sh
# Help:    ./timps-qa.sh --help
# =============================================================================
set -u

# ----------------------------------------------------------------------------- config
CAM="${CAM:-}"
RTSP_PORT="${RTSP_PORT:-554}"
HTTP_PORT="${HTTP_PORT:-8880}"
ONVIF_PORT="${ONVIF_PORT:-80}"

RTSP_USER="${RTSP_USER:-thingino}"
RTSP_PASS="${RTSP_PASS:-thingino}"
HTTP_USER="${HTTP_USER:-thingino}"
HTTP_PASS="${HTTP_PASS:-thingino}"
# expected audio channel count for a hard stereo/mono assertion (empty = report only)
EXPECT_CHANNELS="${EXPECT_CHANNELS:-}"
# ONVIF WS-Security creds — S96onvif_discovery sources these from timps rtsp.user/pass
ONVIF_USER="${ONVIF_USER:-$RTSP_USER}"
ONVIF_PASS="${ONVIF_PASS:-$RTSP_PASS}"

# RTSP endpoints (main + sub). Adjust if your rtsp_path config differs.
PATH_MAIN="${PATH_MAIN:-ch0}"
PATH_SUB="${PATH_SUB:-ch1}"
RTSP_TRANSPORT="${RTSP_TRANSPORT:-tcp}"

# Optional on-device access, e.g. SSH_TARGET="root@192.168.1.100"
SSH_TARGET="${SSH_TARGET:-}"
SSH_OPTS="${SSH_OPTS:--o ConnectTimeout=6 -o StrictHostKeyChecking=no -o BatchMode=yes}"
# osd.vars_file path on the camera (only needs overriding if a camera's
# config points it somewhere other than the compiled-in default).
OSD_VARS_FILE="${OSD_VARS_FILE:-/tmp/timps_osd.vars}"

PROFILE="${PROFILE:-standard}"
OUTDIR="${OUTDIR:-}"

# Tunables (overridable per profile below / by env)
INTEG_DUR="${INTEG_DUR:-30}"       # seconds recorded per stream for integrity+sync
SNAP_COUNT="${SNAP_COUNT:-30}"     # snapshot requests
RECONNECT_CYCLES="${RECONNECT_CYCLES:-20}"
LOAD_CLIENTS="${LOAD_CLIENTS:-1 2 4 8}"   # concurrent-client ramp
LOAD_DUR="${LOAD_DUR:-30}"         # seconds per load step
# Hard concurrent-RTSP-client cap compiled into the daemon (RTSP_MAX_CLIENTS,
# src/rtsp/rtsp.c:32). Not reported by /control, so it is a constant here;
# override only if a build changes the #define. Used to tell "correctly
# enforced admission control" apart from "degrading under load".
RTSP_CAP="${RTSP_CAP:-8}"
SOAK_DUR="${SOAK_DUR:-0}"          # seconds of soak (0 = skip unless profile sets it)
SOAK_SAMPLE="${SOAK_SAMPLE:-60}"   # health sample interval during soak
# Section 15b - long-session A/V drift. Deliberately NOT covered by the soak
# above: soak reconnects every SOAK_SAMPLE seconds, and every fresh RTSP
# connection resets the per-session pts anchor, so drift that only accumulates
# WITHIN one long-lived session (the class of bug fixed in rtp.c 2026-08-10,
# stale now_us -> inconsistent NTP<->RTP pairing in the RTCP SR) is structurally
# invisible there. 0 = skip unless a profile sets it.
DRIFT_DUR="${DRIFT_DUR:-0}"        # seconds of ONE unbroken RTSP session (0 = skip)
DRIFT_SEG="${DRIFT_SEG:-300}"      # checkpoint interval within that session
# |skew| at which the session is aborted early - the bug is already proven at
# this point and there is nothing to learn from another hour of it.
DRIFT_ABORT="${DRIFT_ABORT:-1.0}"
DO_RESTART="${DO_RESTART:-0}"      # 1 = exercise streamer restart
ONLY=""                            # comma-sep section names/numbers to run (empty = all)
# Optional backchannel acoustic-loopback test (default OFF, never in a profile):
# plays a tone into the speaker via backchannel and checks the mic picks it up.
TEST_BACKCHANNEL="${TEST_BACKCHANNEL:-0}"
BC_TEST_FREQ="${BC_TEST_FREQ:-1500}"   # test-tone frequency (Hz), narrow + mid-band
BC_TEST_SECS="${BC_TEST_SECS:-4}"      # tone duration (s)

# Optional rotation persist-only round-trip test (default OFF, never in a
# profile): SoC-gated via caps.rotation (only present when USE_ROTATE was
# compiled in, only advertises the degree values this SoC's rotation path
# actually supports) - skips cleanly on a build without rotation support.
TEST_ROTATION="${TEST_ROTATION:-0}"

# Optional fatal-signal-handler test (default OFF, never in a profile,
# DESTRUCTIVE - see section 14b): sends a real SIGSEGV to the running timpsd
# over SSH to exercise main.c's production fatal_signal_handler (no special
# build/debug flag needed - sigaction() catches externally-sent signals the
# same as a genuine internal fault), then restarts the daemon for real.
TEST_CRASH="${TEST_CRASH:-0}"

# Further opt-in tests (all default OFF, none ever part of a profile) added
# from the 2026-08-11 coverage audit against this project's real bug history:
TEST_LEAK="${TEST_LEAK:-0}"      # 12b: SIGKILLed clients must be reaped (6473848/265befb)
TEST_FLIP="${TEST_FLIP:-0}"      # 8f:  pixel-verified hflip + forced chn0 relatch (8fb6fd3/9034d61)
TEST_ENCODER="${TEST_ENCODER:-0}" # 8g: encoder settings verified by measuring the substream (live or after a restart)
TEST_DAYNIGHT="${TEST_DAYNIGHT:-0}" # 8h: deterministic day/night transition + flap check (0f5fc80)
TEST_HOSTILE="${TEST_HOSTILE:-0}" # 13b: stalled client must not degrade healthy ones
TEST_REBOOT="${TEST_REBOOT:-0}"  # 14c: real reboot, config/binary/version persistence

usage() {
	sed -n '2,58p' "$0" | sed 's/^# \{0,1\}//'
	cat <<EOF

Options (also settable as env vars):
  --cam IP            camera address (required)
  --profile P         quick | standard | load | soak | drift  (default: standard)
  --rtsp-user U       --rtsp-pass P    --http-user U   --http-pass P
  --expect-channels N assert audio channel count (e.g. 2 = stereo); FAIL on mismatch
  --main PATH         RTSP main path (default ch0)   --sub PATH (default ch1)
  --transport tcp|udp default tcp
  --ssh TARGET        e.g. root@IP  -> enables on-device checks
  --integ-dur S       --load-dur S  --load-clients "1 2 4 8"
  --reconnects N      --snaps N     --soak-dur S       --restart
  --drift-dur S       hold ONE RTSP connection open for S seconds and track
                      A/V drift across it (section 15b). Default 0 = off,
                      like --soak-dur; --profile drift sets 7200 (2h).
                      Unlike the soak, this never reconnects, so drift that
                      only accumulates within a single long-lived session
                      stays visible instead of being reset every slice.
  --drift-seg S       drift checkpoint interval in seconds (default 300).
                      Needs >=4 checkpoints for a trend verdict.
  --only LIST         run only these sections (names or numbers, comma-sep),
                      e.g. --only onvif  or  --only 3,10 ; preflight always runs
  --out DIR           output directory
  (env) HTTP_TOKEN    a valid /control token for section 2b's token-acceptance
                      test; without it (and without --ssh to read TOKEN_FILE,
                      default /run/timps.token) only token REJECTION is tested
  --test-backchannel  run the acoustic-loopback backchannel test (section 2d):
                      play a tone into the speaker, confirm the mic hears it.
                      Default OFF, never part of a profile (environmental).
  --bc-test-freq HZ   test-tone frequency (default 1500)
  --bc-test-secs S    test-tone duration (default 4)
  --test-rotation     run the persist-only video0.rotation round-trip check
                      (section 8b): SoC-gated via caps.rotation, skips
                      cleanly if this build has no rotation support.
                      Default OFF, never part of a profile.
  --osd-vars-file P   override the on-device osd.vars_file path used by
                      section 8c's custom-placeholder round-trip
                      (default /tmp/timps_osd.vars, only needed if a camera
                      configures a non-default path). Needs --ssh.
  --test-leak         (section 12b) start a client, SIGKILL it without a
                      TEARDOWN, and require the daemon to reap the orphaned
                      session (RTSP-UDP <=150s, HTTP fMP4 and SRT <=90s).
                      Takes several minutes of waiting. Default OFF.
  --test-flip         (section 8f) PIXEL-verify image.hflip via snapshots
                      instead of trusting the /control read-back, then force
                      a chn0 relatch and verify the flip survived it.
                      NEEDS A STATIC SCENE; ambiguous results WARN.
  --test-encoder      (section 8g) measure what the SUBSTREAM actually
                      delivers, then cut its bitrate target well below that
                      (a lower ceiling always binds) and MEASURE again to
                      prove the encoder honours it. The new target goes in
                      LIVE where caps.video_live lists bitrate, otherwise via
                      a real restart, which is the only case that needs
                      --ssh. Also compares cbr/vbr variance. Restores after.
  --test-daynight     (section 8h) force a day/night transition in both
                      directions via time windows, assert the board hook ran,
                      that image.running_mode followed, and that it did not
                      flap. Physically clicks the IR-cut filter twice.
  --test-hostile      (section 13b) run healthy clients alongside one client
                      that connects and then stops reading; assert the healthy
                      clients keep the fps THEY MEASURED IN THE BASELINE PHASE
                      (isolation is a differential question, not an absolute
                      one), that keyframe rate does not spike (global IDR rate
                      limiter) and that memory stays bounded. Needs python3.
  --test-reboot       (section 14c) needs --ssh. REBOOTS THE CAMERA: makes a
                      persisted config change, reboots, then verifies the
                      change survived, that /etc/timps.conf and
                      /usr/bin/timpsd md5s and the version string are what
                      they should be, and that nothing else in /control
                      silently reset at boot. Camera is offline 1-2 min.
                      Default OFF, never part of a profile.
  --test-crash        DESTRUCTIVE (section 14b): sends a real SIGSEGV to the
                      running timpsd over SSH to verify the fatal-signal
                      handler (writes/checks /run/timps.crash, format
                      SIGSEGV+fault address), then restarts the daemon for
                      real and confirms it comes back healthy. The daemon
                      WILL go down and restart during this test. Needs --ssh.
                      Default OFF, never part of a profile.

Profiles:
  quick     ~3 min  : short integrity + snapshot + tiny load, no soak
  standard  ~15 min : full integrity/sync + reliability + load ramp
  load      ~20 min : heavier + longer load ramp
  soak      hours   : standard + long soak (default 2h; set --soak-dur)
  drift     hours   : standard + one unbroken RTSP session with A/V-drift
                      checkpoints (default 2h; set --drift-dur/--drift-seg)
EOF
	exit 1
}

# ----------------------------------------------------------------------------- args
while [ $# -gt 0 ]; do
	case "$1" in
		--cam) CAM="$2"; shift 2;;
		--profile) PROFILE="$2"; shift 2;;
		--rtsp-user) RTSP_USER="$2"; shift 2;;
		--rtsp-pass) RTSP_PASS="$2"; shift 2;;
		--http-user) HTTP_USER="$2"; shift 2;;
		--http-pass) HTTP_PASS="$2"; shift 2;;
		--expect-channels) EXPECT_CHANNELS="$2"; shift 2;;
		--main) PATH_MAIN="$2"; shift 2;;
		--sub) PATH_SUB="$2"; shift 2;;
		--transport) RTSP_TRANSPORT="$2"; shift 2;;
		--ssh) SSH_TARGET="$2"; shift 2;;
		--integ-dur) INTEG_DUR="$2"; shift 2;;
		--load-dur) LOAD_DUR="$2"; shift 2;;
		--load-clients) LOAD_CLIENTS="$2"; shift 2;;
		--reconnects) RECONNECT_CYCLES="$2"; shift 2;;
		--snaps) SNAP_COUNT="$2"; shift 2;;
		--soak-dur) SOAK_DUR="$2"; shift 2;;
		--drift-dur) DRIFT_DUR="$2"; shift 2;;
		--drift-seg) DRIFT_SEG="$2"; shift 2;;
		--restart) DO_RESTART=1; shift;;
		--only) ONLY="$2"; shift 2;;
		--out) OUTDIR="$2"; shift 2;;
		--test-backchannel) TEST_BACKCHANNEL=1; shift;;
		--bc-test-freq) BC_TEST_FREQ="$2"; shift 2;;
		--bc-test-secs) BC_TEST_SECS="$2"; shift 2;;
		--test-rotation) TEST_ROTATION=1; shift;;
		--osd-vars-file) OSD_VARS_FILE="$2"; shift 2;;
		--test-crash) TEST_CRASH=1; shift;;
		--test-leak) TEST_LEAK=1; shift;;
		--test-flip) TEST_FLIP=1; shift;;
		--test-encoder) TEST_ENCODER=1; shift;;
		--test-daynight) TEST_DAYNIGHT=1; shift;;
		--test-hostile) TEST_HOSTILE=1; shift;;
		--test-reboot) TEST_REBOOT=1; shift;;
		-h|--help) usage;;
		*) echo "unknown option: $1" >&2; usage;;
	esac
done
[ -n "$CAM" ] || { echo "ERROR: --cam <ip> is required"; usage; }

# Second-resolution timestamp alone collided when multiple cameras' runs were
# launched in the same shell within the same second (parallel fleet QA) -
# their recordings/probe files landed in the SAME directory and clobbered
# each other, surfacing as bogus "non-monotonic timestamp" FAILs with no
# hint anything was shared. Tag with the camera and PID so concurrent runs
# (even against the same camera) never collide; --out still overrides.
if [ -z "$OUTDIR" ]; then
	_cam_tag=$(printf '%s' "$CAM" | tr -c 'A-Za-z0-9._-' '_')
	OUTDIR="timps-qa-${_cam_tag}-$(date +%Y%m%d-%H%M%S)-$$"
fi

case "$PROFILE" in
	quick)    INTEG_DUR=${INTEG_DUR_SET:-10}; SNAP_COUNT=10; RECONNECT_CYCLES=6;  LOAD_CLIENTS="1 2"; LOAD_DUR=15; SOAK_DUR=0;;
	standard) : ;;
	load)     LOAD_CLIENTS="1 2 4 8 12 16"; LOAD_DUR=45;;
	soak)     [ "$SOAK_DUR" -gt 0 ] || SOAK_DUR=7200;;
	# separate profile rather than folding into `soak`: both are hours long and
	# run serially, so bundling them would silently double a soak run's wall time
	drift)    [ "$DRIFT_DUR" -gt 0 ] || DRIFT_DUR=7200;;
	*) echo "unknown profile: $PROFILE" >&2; usage;;
esac

mkdir -p "$OUTDIR" || { echo "cannot create $OUTDIR"; exit 1; }
SUMMARY="$OUTDIR/summary.txt"
: > "$SUMMARY"

# ----------------------------------------------------------------------------- helpers
c_red=$'\033[31m'; c_grn=$'\033[32m'; c_yel=$'\033[33m'; c_cyn=$'\033[36m'; c_rst=$'\033[0m'
PASS=0; WARN=0; FAIL=0; SKIP=0

log()  { printf '%s\n' "$*" | tee -a "$SUMMARY"; }
hdr()  { printf '\n%s=== %s ===%s\n' "$c_cyn" "$*" "$c_rst" | tee -a "$SUMMARY"; }
ok()   { PASS=$((PASS+1)); printf '  %s[PASS]%s %s\n' "$c_grn" "$c_rst" "$*" | tee -a "$SUMMARY"; }
warn() { WARN=$((WARN+1)); printf '  %s[WARN]%s %s\n' "$c_yel" "$c_rst" "$*" | tee -a "$SUMMARY"; }
bad()  { FAIL=$((FAIL+1)); printf '  %s[FAIL]%s %s\n' "$c_red" "$c_rst" "$*" | tee -a "$SUMMARY"; }
skip() { SKIP=$((SKIP+1)); printf '  [skip] %s\n' "$*" | tee -a "$SUMMARY"; }
info() { printf '  %s\n' "$*" | tee -a "$SUMMARY"; }

have() { command -v "$1" >/dev/null 2>&1; }

# section gate: want <num> <name...>  -> true if --only unset or matches num/any name
want() {
	[ -z "$ONLY" ] && return 0
	local tok
	for tok in $(echo "$ONLY" | tr ',' ' '); do
		for a in "$@"; do
			[ "$tok" = "$a" ] && return 0
		done
	done
	return 1
}

# float compare: fcmp A OP B  (OP: lt le gt ge)  -> exit 0 if true
fcmp() { awk -v a="$1" -v b="$3" -v op="$2" 'BEGIN{
	if(op=="lt")exit!(a<b); if(op=="le")exit!(a<=b);
	if(op=="gt")exit!(a>b); if(op=="ge")exit!(a>=b); exit 1}'; }

# ---------------------------------------------------------------- shared patterns
# ONE definition of "ffmpeg complained about the media" for every capture this
# script takes. analyze_stream's copy and the soak loop's copy had already
# drifted apart (the soak was missing decode_slice|missed), and it was exactly
# this class of drift that let the "non-monotonous" vs "non-monotonic" typo
# survive undetected in analyze_stream for the whole life of the project -
# ffmpeg's actual muxer wording is "Non-monotonic DTS; previous: X, current: Y".
# Keep it here, use it everywhere, never inline a second copy.
FFWARN_RE='non-monoton(ous|ic)|discontinuit|corrupt|error while|decode_slice|concealing|invalid data|missed'
# ffwarn_count <logfile> -> count of matching lines, always a bare integer.
# NOTE the `n=$(grep -c ...)` shape: grep -c prints "0" AND exits 1 on no match,
# so chaining `|| echo 0` (as the soak loop once did) yields a two-line "0\n0"
# that turns the caller's $((...)) into a fatal arithmetic syntax error.
ffwarn_count() { local n; n=$(grep -icE "$FFWARN_RE" "$1" 2>/dev/null); printf '%s' "${n:-0}"; }

# enc_measure <rtsp-path> <dur> <tag> -> "<kbps> <cv> <i_avg> <i_cnt> <p_avg>"
#   kbps  = delivered VIDEO bitrate (payload bytes over the media timespan;
#           audio excluded so it is comparable with the configured video
#           bitrate)
#   cv    = coefficient of variation of the per-second byte totals. Per-FRAME
#           variance is dominated by I-vs-P size in every rc mode and says
#           nothing; per-SECOND variance is what actually separates CBR from
#           VBR.
#   i_avg = mean KEYFRAME size in bytes, i_cnt how many were seen, p_avg the
#           mean non-keyframe size. Split off the packet FLAGS column ("K_" on
#           a keyframe) rather than decoding: -show_entries packet=... never
#           opens the decoder, so this costs nothing on top of the bitrate pass
#           and works for H264 and H265 alike. Added for RC5's i_bias_lvl
#           sweep, which needs I-frame size specifically - overall kbps cannot
#           see an I-vs-P bit REDISTRIBUTION at a constant CBR target.
# Shared by 8b's RC3b/RC3c/RC4/RC5 (live-apply verification, no restart) and 8g
# (the fuller persist+restart route) - all need "what did the encoder actually
# deliver", none can get it from an SDK struct readback (see RC4's own comment
# on why that readback is unreliable without an active client pulling frames).
enc_measure() {
	local pth="$1" dur="$2" tag="$3"
	local f="$OUTDIR/enc_${tag}.mkv" c="$OUTDIR/enc_${tag}.csv"
	timeout -k 5 "$((dur+15))" ffmpeg -hide_banner -nostdin -y -loglevel warning \
		-rtsp_transport tcp -i "$(rtsp_url "$pth")" -t "$dur" -an -c copy "$f" \
		</dev/null 2>"$OUTDIR/enc_${tag}.log" || true
	[ -s "$f" ] || return 1
	ffprobe -v error -select_streams v:0 -show_entries packet=pts_time,size,flags \
		-of csv=p=0 "$f" 2>/dev/null > "$c"
	[ -s "$c" ] || return 1
	awk -F, '{t=$1+0; s=$2+0; if(n++==0)t0=t; tl=t; sum+=s; b[int(t)]+=s;
		if($3 ~ /K/){ ic++; isum+=s } else { pc++; psum+=s }}
	END{
		iavg=(ic>0)?isum/ic:0; pavg=(pc>0)?psum/pc:0;
		span=tl-t0; if(span<=0){ printf "0 0 %.0f %d %.0f\n", iavg, ic, pavg; exit }
		kbps=sum*8/span/1000;
		# drop the first and last (partial) second buckets
		m=0; c2=0; for(k in b){ ks[c2++]=k }
		lo=1e18; hi=-1e18; for(i=0;i<c2;i++){ if(ks[i]+0<lo)lo=ks[i]+0; if(ks[i]+0>hi)hi=ks[i]+0 }
		nn=0; for(k in b){ if(k+0>lo && k+0<hi){ v[nn++]=b[k]; m+=b[k] } }
		if(nn<3){ printf "%.0f 0 %.0f %d %.0f\n", kbps, iavg, ic, pavg; exit }
		m/=nn; sd=0; for(i=0;i<nn;i++) sd+=(v[i]-m)*(v[i]-m);
		sd=sqrt(sd/nn);
		printf "%.0f %.3f %.0f %d %.0f\n", kbps, (m>0)?sd/m:0, iavg, ic, pavg;
	}' "$c"
	rm -f "$f"
}

# ---------------------------------------------------------- on-device telemetry
# dev_proc_sample [window_s] -> "<rss_kB> <fds> <threads> <cpu_pct>", empty
# without SSH or if timpsd isn't running.
#
# Why fds/threads and not just RSS: every real leak in this codebase's history
# (0980d05, d07b173, and the whole unreaped-session class) costs an fd pair, a
# thread and a hub subscription per stuck session - kilobytes, which never
# moves an RSS threshold, but which shows up immediately as a monotonically
# rising fd/thread count. Why CPU: a hot spin (the historical httpd 5Hz
# busy-discard, or a `continue` with no usleep) degrades nothing observable
# from the stream side and never grows RSS - CPU is the only place it shows.
#
# CPU is utime+stime deltas over a real window divided by wall time; USER_HZ is
# 100 on every Linux/MIPS build this runs against. Field 14/15 of /proc/PID/stat
# are safe to index positionally here because the comm field ("(timpsd)")
# contains no spaces.
#
# dev_snap        -> "<rss_kB> <fds> <threads> <cpu_ticks> <uptime_s>" (1 ssh,
#                    no sleep - cheap enough to bracket any existing test)
# dev_cpu_between A B -> %CPU between two dev_snap results
# dev_proc_sample [w] -> "<rss> <fds> <threads> <cpu_pct>" over its own w-second
#                    window (for when there is no existing interval to bracket)
dev_snap() {
	[ -n "$SSH_TARGET" ] || return 1
	sshx "p=\$(pidof timpsd | awk '{print \$1}'); [ -n \"\$p\" ] || exit 1;
	      r=\$(awk '/VmRSS/{print \$2}' /proc/\$p/status);
	      t=\$(awk '/Threads/{print \$2}' /proc/\$p/status);
	      f=\$(ls /proc/\$p/fd 2>/dev/null | wc -l);
	      c=\$(awk '{print \$14+\$15}' /proc/\$p/stat);
	      u=\$(awk '{print \$1}' /proc/uptime);
	      echo \"\$r \$f \$t \$c \$u\"" 2>/dev/null
}
dev_cpu_between() {
	[ -n "${1:-}" ] && [ -n "${2:-}" ] || { printf '0.0'; return 1; }
	awk -v a="$1" -v b="$2" 'BEGIN{
		split(a,A," "); split(b,B," "); d=B[5]-A[5];
		printf "%.1f", (d>0)?((B[4]-A[4])/100.0/d*100.0):0 }'
}
dev_proc_sample() {
	local w="${1:-3}" a b r f t
	a=$(dev_snap) || return 1
	sleep "$w"
	b=$(dev_snap) || return 1
	read -r r f t _ _ <<<"$b"
	printf '%s %s %s %s' "$r" "$f" "$t" "$(dev_cpu_between "$a" "$b")"
}

# dmesg_capture <outfile> [lines] -> saves the kernel ring buffer tail, prints
# the count of interesting lines. The kernel log is a genuinely DIFFERENT
# failure surface from logread: logread is userspace syslog (timpsd's own
# LOG*), dmesg is where the Ingenic ISP/VIC kernel driver and libimp's internal
# diagnostics print (e.g. "wait stop q->num_buffers=..", "streamoff stop",
# "osd_draw_cover_pic rejects .. keep within picture range" - see the comment at
# src/hal/imp_osd.c:219). A driver-level stall or DMA error appears there and
# nowhere else. Deliberately low-noise: embedded boot logs are full of benign
# probe/init chatter, so the grep excludes the known-harmless traffic rather
# than reporting a scary number on every healthy camera.
#
# TWO defences against false positives, both added 2026-08-11 after the first
# hardware run of this check produced a FAIL on a dim outbuilding and the same-class
# WARN on cam-A and cam-L - i.e. it fired on every camera, every boot, with
# nothing actually wrong:
#
# 1. SCAN ONLY WHAT CAME AFTER THE SENSOR STARTED STREAMING. On these boards the
#    ring buffer is small enough that its whole contents are usually still the
#    boot log (all three captures topped out around t=20s), and a boot log
#    unconditionally contains lines carrying scary words with no fault behind
#    them - the kernel version banner embeds a BUILD-time linker message
#    ("(collect2: error: ld returned 1 exit status)"), "CPU0 RESET ERROR PC:.."
#    is printed by this SoC on every cold start, "panic=2" is part of the
#    kernel command line, "jz-wdt: watchdog initialized" is the watchdog
#    working, and the USB-OTG/SDIO/cache probes print "cgu clk gate get error",
#    "pls check processor_id[..],sc_jz not support!" and "jzmmc..: Error
#    status->..: cmd=52" as normal probe chatter. Anchoring on the sensor
#    bring-up ("<sensor> stream on", "chip found @", ...) drops the entire
#    preamble, which is also what this check is FOR: driver/DMA/ISP trouble
#    while the camera is actually running. If no anchor is found (a busy device
#    whose ring has wrapped past boot) everything is scanned, which is correct -
#    in that case the buffer holds runtime messages, not boot.
# 2. Tighter tokens. Bare "Oops" matched "lo(ops)_per_jiffy" in a cpufreq
#    table; bare "panic"/"watchdog" matched the two benign lines above. The
#    fatal forms are word-anchored, and the specific verified-benign strings
#    are denylisted as well so they cannot come back through the no-anchor path.
DMESG_BAD_RE='Kernel panic|\bOops\b|BUG:|soft lockup|oom-killer|[Oo]ut of memory|SYN flooding|segfault|do_page_fault|error|fail|timeout'
DMESG_BENIGN_RE='collect2: error|RESET ERROR|Kernel command line|cgu clk gate get error|pls check processor_id|sc_jz not support|watchdog initialized|jz-wdt|\[atbm_log\]|NOHZ:|loops_per_jiffy|jzmmc.*Error status|streamoff|wait stop|num_buffers|done_count|link_stream|sensor_probe|probe ok|Error Recovery|failover|no error|error_code=0'
# last boot-stage marker; everything after it is runtime. Sensor-model agnostic
# (sc2336/sc4336p/... all print "<model> stream on").
DMESG_BOOT_ANCHOR='stream on|chip found @|sensor driver version|sensor_detect|codec_set_device|codec_codec_ctl'
# Prints "<findings> <lines_scanned> <anchored 0|1> <runtime_file>" and returns
# nonzero when there is nothing to report at all (no SSH / unreadable dmesg).
# Everything comes back on stdout rather than through globals on purpose:
# callers use $(dmesg_capture ...), which runs in a subshell, so any variable
# the function set would be silently lost.
dmesg_capture() {
	local f="$1" n="${2:-200}"
	[ -n "$SSH_TARGET" ] || return 1
	sshx "dmesg 2>/dev/null | tail -$n" > "$f" 2>/dev/null || return 1
	[ -s "$f" ] || return 1
	local anchor rt anchored scanned c
	anchor=$(grep -nE "$DMESG_BOOT_ANCHOR" "$f" 2>/dev/null | tail -1 | cut -d: -f1)
	rt="${f%.txt}_runtime.txt"
	if [ -n "$anchor" ]; then
		tail -n "+$((anchor+1))" "$f" > "$rt"; anchored=1
	else
		cp "$f" "$rt"; anchored=0
	fi
	scanned=$(grep -c . "$rt" 2>/dev/null); scanned=${scanned:-0}
	c=$(grep -iE "$DMESG_BAD_RE" "$rt" 2>/dev/null | grep -civE "$DMESG_BENIGN_RE")
	printf '%s %s %s %s' "${c:-0}" "$scanned" "$anchored" "$rt"
}

# hub_clients -> the daemon's own live video-subscriber count (hub_video_subs(),
# surfaced as "clients" in the /events stats frame, httpd.c:852). Prints an
# integer, or nothing if /events is unreachable. Each /events connection emits
# the full state immediately on connect, so a short-lived reader is simpler and
# less racy than tailing one long-lived stream.
hub_clients() {
	local f="$OUTDIR/hub_stats.txt"
	timeout 6 curl -s -N -u "$HTTP_USER:$HTTP_PASS" "$(http_base)/events?stream=stats" > "$f" 2>/dev/null
	grep -o '"clients":[0-9]*' "$f" 2>/dev/null | tail -1 | cut -d: -f2
}

# psnr_db <ref.jpg> <cmp.jpg> -> average PSNR in dB, "99" for identical images
# (ffmpeg prints "inf"), empty if the two cannot be compared (size mismatch,
# unreadable file). Used by the flip tests to answer "did the picture actually
# change in the way we asked it to", which no /control read-back can prove -
# the daemon reports the value it accepted, not what the ISP did with it.
psnr_db() {
	local v
	v=$(ffmpeg -hide_banner -nostdin -loglevel info -i "$1" -i "$2" \
	    -lavfi psnr -f null - </dev/null 2>&1 | grep -oE 'average:[0-9.a-z]+' | tail -1 | cut -d: -f2)
	case "$v" in inf|INF) printf '99';; "") : ;; *) printf '%s' "$v";; esac
}

# leak_trend  (reads one number per line on stdin)
#   -> "<n> <first> <last> <delta> <nondecreasing 0|1> <up_steps>"
# "nondecreasing" is the leak signature: a resource that is taken and never
# given back only ever goes up. Judge on that shape, never on an absolute
# count - the healthy absolute number depends on client count, build and SoC.
leak_trend() {
	awk 'NF{n++; v[n]=$1+0}
	END{
		if(n<1){print "0 0 0 0 0 0"; exit}
		nd=1; up=0;
		for(i=2;i<=n;i++){ if(v[i]<v[i-1])nd=0; if(v[i]>v[i-1])up++ }
		printf "%d %d %d %d %d %d\n", n, v[1], v[n], v[n]-v[1], nd, up;
	}'
}

RU="$RTSP_USER"; RP="$RTSP_PASS"
rtsp_url() { printf 'rtsp://%s:%s@%s:%s/%s' "$RU" "$RP" "$CAM" "$RTSP_PORT" "$1"; }
http_base() { printf 'http://%s:%s' "$CAM" "$HTTP_PORT"; }
curlq() { curl -s --max-time "${1:-10}" -u "$HTTP_USER:$HTTP_PASS" "${@:2}"; }

# ffmpeg's HTTP inputs need the Basic header spelled out (-headers). Defined
# HERE, with the other always-available helpers, not inside section 4: it only
# depends on HTTP_USER/PASS, and sections 4 AND 5 both use it - defining it in
# 4 meant `--only 5` died under set -u on the unbound variable before ever
# reaching a verdict (reproduced 2026-08-18; the run ended with no SUMMARY and
# without the 8b interruption traps armed). tr -d '\n' because base64 wraps at
# 76 chars - short creds never hit that, but a long pass would silently break
# the header.
AUTH_HDR="Authorization: Basic $(printf '%s:%s' "$HTTP_USER" "$HTTP_PASS" | base64 | tr -d '\n')"

# is_loopback <host> - same 127.0.0.0/8 test test_auth.sh documents: httpd.c
# trusts every loopback peer ON PURPOSE (the on-device WebUI bridge CGIs must
# always get through), so HTTP auth NEGATIVES against a loopback target (the
# host sim) test the deliberate bypass, not the auth code - and used to
# produce a wall of false "served WITHOUT auth" FAILs in 2b. RTSP has no such
# bypass and is always tested.
is_loopback() {
	case "$1" in 127.*|::1|localhost|localhost.localdomain) return 0;; esac
	return 1
}

sshx() { [ -n "$SSH_TARGET" ] || return 2; ssh $SSH_OPTS "$SSH_TARGET" "$@"; }

# json get (python if present, else grep). usage: jget <file> <dotted.key>
jget() {
	if have python3; then
		python3 - "$1" "$2" <<'PY' 2>/dev/null
import json,sys
try:
    d=json.load(open(sys.argv[1]))
    for k in sys.argv[2].split('.'):
        if isinstance(d,list):        d=d[int(k)]
        elif isinstance(d,dict) and k in d: d=d[k]      # object with numeric string keys ("osd0":{"0":{..}})
        else:                          d=d[int(k)]
    print(d if not isinstance(d,(dict,list)) else json.dumps(d))
except Exception: pass
PY
	else
		# No python3 (busybox-only on-device shell): a genuinely path-aware
		# extractor. The old fallback grepped for only the LAST dotted component
		# and took the first hit anywhere, so on /control's JSON it returned the
		# WRONG field by name collision (video.0.bitrate -> the earlier audio
		# section's bitrate; video.0.width -> sensor.width) and truncated array
		# values at the first comma (caps.rotation [0,90,270] -> "[0"), silently
		# reporting a wrong-but-plausible value. This walks the dotted path
		# key-by-key, descending into the matched value at each step (respecting
		# nested {}/[] and quoted strings), so nested shapes this API emits -
		# {"video":{"0":{"bitrate":..}}}, arrays, numeric-string object keys -
		# resolve correctly. Not a general JSON parser, but correct for the
		# structures this API produces, and it matches the python3 path's result
		# (prints nothing when the path doesn't resolve, like a missing key).
		awk -v path="$2" '
		function skipws(s,i,  c){while(i<=length(s)){c=substr(s,i,1);if(c==" "||c=="\t"||c=="\n"||c=="\r")i++;else break}return i}
		function valend(s,i,  c,cc,depth,instr,j){
			c=substr(s,i,1)
			if(c=="\""){j=i+1;while(j<=length(s)){cc=substr(s,j,1);if(cc=="\\"){j+=2;continue}if(cc=="\""){return j}j++}return length(s)}
			if(c=="{"||c=="["){depth=0;instr=0;j=i;while(j<=length(s)){cc=substr(s,j,1);if(instr){if(cc=="\\"){j+=2;continue}if(cc=="\""){instr=0}j++;continue}if(cc=="\""){instr=1;j++;continue}if(cc=="{"||cc=="["){depth++}else if(cc=="}"||cc=="]"){depth--;if(depth==0)return j}j++}return length(s)}
			j=i;while(j<=length(s)){cc=substr(s,j,1);if(cc==","||cc=="}"||cc=="]"||cc==" "||cc=="\t"||cc=="\n"||cc=="\r")return j-1;j++}return length(s)
		}
		{doc=doc $0 "\n"}
		END{
			n=split(path,parts,".");cur=doc;ok=1
			for(pi=1;pi<=n;pi++){
				k=parts[pi];i=skipws(cur,1);c=substr(cur,i,1)
				if(c=="["){idx=0;j=i+1;found=0
					while(j<=length(cur)){j=skipws(cur,j);cc=substr(cur,j,1);if(cc=="]")break;ve=valend(cur,j);if(idx==k+0){cur=substr(cur,j,ve-j+1);found=1;break}idx++;j=skipws(cur,ve+1);if(substr(cur,j,1)==",")j++}
					if(!found){ok=0;break}
				}else if(c=="{"){j=i+1;found=0
					while(j<=length(cur)){j=skipws(cur,j);cc=substr(cur,j,1);if(cc=="}")break;if(cc!="\""){ok=0;break}ke=valend(cur,j);key=substr(cur,j+1,ke-j-1);j=skipws(cur,ke+1);if(substr(cur,j,1)!=":"){ok=0;break}j=skipws(cur,j+1);ve=valend(cur,j);if(key==k){cur=substr(cur,j,ve-j+1);found=1;break}j=skipws(cur,ve+1);if(substr(cur,j,1)==",")j++}
					if(!found){ok=0;break}
				}else{ok=0;break}
			}
			if(!ok)exit 1
			i=skipws(cur,1);e=length(cur);while(e>=i){c=substr(cur,e,1);if(c==" "||c=="\t"||c=="\n"||c=="\r")e--;else break}
			cur=substr(cur,i,e-i+1)
			if(substr(cur,1,1)=="\""){cur=substr(cur,2,length(cur)-2);gsub(/\\"/,"\"",cur);gsub(/\\\\/,"\\",cur);gsub(/\\\//,"/",cur)}
			print cur
		}' "$1"
	fi
}

# jarr <file> <top-level-key> -> space-separated flat string array (e.g. the
# GET /control?fields=1 document, section 8d). Unlike jget's generic
# path-walker, this only ever needs to handle ONE shape - a top-level key
# mapping straight to a flat array of quoted strings, no nesting - so a
# simpler extractor suffices for both the python3 and busybox-awk-less paths.
jarr() {
	if have python3; then
		python3 - "$1" "$2" <<'PY' 2>/dev/null
import json,sys
try:
    d=json.load(open(sys.argv[1]))
    print(' '.join(d.get(sys.argv[2],[])))
except Exception: pass
PY
	else
		local m
		m=$(grep -oE "\"$2\":\[[^]]*\]" "$1" | head -1)
		m=${m#*[}; m=${m%]}
		printf '%s' "$m" | grep -oE '"[^"]*"' | tr -d '"' | tr '\n' ' '
	fi
}

# ONVIF SOAP call with WS-Security UsernameToken (PasswordDigest =
# base64(sha1(nonce + created + password))). $1 = service (e.g. media_service),
# $2 = SOAP body. Prints the response. Needs openssl for the digest; without it
# the call goes unauthenticated (fine for GetSystemDateAndTime, likely 401 else).
onvif_call() {
	local svc="$1" body="$2" created nb nonce_b64 digest sec=""
	created=$(date -u +%Y-%m-%dT%H:%M:%SZ)
	if have openssl; then
		nb=$(mktemp); head -c16 /dev/urandom > "$nb"
		nonce_b64=$(base64 < "$nb" 2>/dev/null | tr -d '\n')
		digest=$(cat "$nb" <(printf '%s%s' "$created" "$ONVIF_PASS") | openssl dgst -sha1 -binary 2>/dev/null | base64 | tr -d '\n')
		rm -f "$nb"
		sec='<wsse:Security s:mustUnderstand="1" xmlns:wsse="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd" xmlns:wsu="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd"><wsse:UsernameToken><wsse:Username>'"$ONVIF_USER"'</wsse:Username><wsse:Password Type="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0#PasswordDigest">'"$digest"'</wsse:Password><wsse:Nonce EncodingType="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-soap-message-security-1.0#Base64Binary">'"$nonce_b64"'</wsse:Nonce><wsu:Created>'"$created"'</wsu:Created></wsse:UsernameToken></wsse:Security>'
	fi
	curl -s --max-time 10 -H 'Content-Type: application/soap+xml; charset=utf-8' \
		-d '<?xml version="1.0" encoding="UTF-8"?><s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope"><s:Header>'"$sec"'</s:Header><s:Body>'"$body"'</s:Body></s:Envelope>' \
		"http://$CAM:$ONVIF_PORT/onvif/$svc" 2>/dev/null
}

# --------------------------------------------------- A/V skew from a packet timeline
# av_skew <pkts.csv> [warmup_s]  ->  "<start> <end> <drift>" (seconds, audio
# minus video; drift = end-start). Prints nothing if the capture has no
# audio+video pair. The CSV is ffprobe's `packet=codec_type,pts_time -of
# csv=p=0` output (field 1 = codec_type, field 2 = pts_time).
#
# Single implementation shared by section 3's analyze_stream() and section
# 15b's long-session drift tracker, deliberately: the tricky part here is the
# steady-state windowing, and a second hand-rolled variant of it would drift
# out of agreement with this one at the first tweak.
#
# WARMUP_S excludes the connection/first-keyframe startup transient: a lone
# leading video keyframe (sent immediately so the player has something to
# decode) followed by a real gap before audio and steady video both start is
# NORMAL, not growing drift - fMP4 anchors both tracks to a shared t=0 at that
# first keyframe (unlike RTSP, which gives each track its own independent PTS
# zero), so a client that attaches mid-warmup sees a one-time skew that LOCKS
# once real media starts. The start reference is the first AUDIO packet
# at/after warmup paired with the VIDEO packet nearest to it in time, falling
# back to the raw first packets when the capture is entirely inside the warmup
# window. It is deliberately NOT each track's own first post-warmup packet:
# PTS are wall-locked, so when a transport dropout hole overlaps the warmup
# boundary the two per-track references can land on opposite sides of the hole
# and fake a seconds-large "skew" between packets that were never
# contemporaneous (cam-L 2026-08-11: a lone keyframe at 2.060s inside a
# startup dropout vs audio resuming at 3.558s read as start=1.498s, end=0.014s
# -> a false "out of sync" FAIL on a stream whose tracks were locked wherever
# both actually flowed). Pairing the start reference by proximity measures the
# real inter-track offset; genuine RATE divergence is still fully visible in
# end/drift, which stay last-audio-minus-last-video.
#
# Note for section 15b: because its segments carry session-relative timestamps
# (-reset_timestamps 0), every segment after the first begins well past
# warmup, so the warmup branch is a no-op there and each checkpoint's "end"
# value is the ABSOLUTE session A/V offset at that point - which is exactly
# what makes those values comparable across checkpoints.
av_skew() {
	awk -F, -v warmup="${2:-${QA_WARMUP_S:-2}}" '
	{
		ct=$1; p=$2+0;
		if(ct=="video"||ct=="audio"){
			if(!(ct in first)) first[ct]=p;
			if(!(ct in first_ss) && p>=warmup) first_ss[ct]=p;
			last[ct]=p;
			if(ct=="video") vp[nv++]=p;
		}
	}
	END{
		if(!(("audio" in first)&&("video" in first))) exit;
		se=last["audio"]-last["video"];
		if("audio" in first_ss){
			# start = first post-warmup audio vs the video packet NEAREST
			# to it in time (see the header comment: per-track independent
			# start references sit on opposite sides of a dropout hole and
			# fake a skew between non-contemporaneous packets)
			a0=first_ss["audio"];
			v0=vp[0]; best=(v0>a0)?v0-a0:a0-v0;
			for(i=1;i<nv;i++){d=vp[i]-a0; if(d<0)d=-d; if(d<best){best=d;v0=vp[i]}}
			ss=a0-v0;
		} else {
			ss=first["audio"]-first["video"];
		}
		printf "%.3f %.3f %.3f\n", ss, se, se-ss;
	}' "$1"
}

# --------------------------------------------------- stream integrity + A/V sync core
# analyze_stream <url> <label> <dur> <input-opts...>
analyze_stream() {
	local url="$1" label="$2" dur="$3"; shift 3
	local inopts=("$@")
	local seg="$OUTDIR/rec_${label}.mkv" err="$OUTDIR/rec_${label}.log"
	local t0 t1 wall
	info "$label: recording ${dur}s ..."
	t0=$(date +%s.%N)
	# Parallel ICMP probe for the whole capture window: rt<0.5 (below) cannot
	# by itself tell a server stall from a degraded WLAN path, and 2026-08-18
	# that ambiguity sent a real debugging session after the daemon when the
	# network was at fault. Ping loss measured DURING the capture is the cheap
	# independent witness: heavy ICMP loss = the path itself was dropping, so
	# the transport verdict must not blame the code. Best-effort - a missing
	# ping binary or an ICMP-filtering path just leaves ping_loss empty and
	# the verdict falls back to its old (strict) form.
	local pingf="$OUTDIR/ping_${label}.txt" pingpid="" ping_loss=""
	if have ping; then
		ping -i 0.5 -w "$((dur+2))" "$CAM" > "$pingf" 2>/dev/null &
		pingpid=$!
	fi
	# -nostdin + timeout -k: ffmpeg over RTSP-TCP may ignore a lone SIGTERM, so
	# force a SIGKILL if -t doesn't self-stop. No -copyts (it breaks -t and isn't
	# needed: fps/rate/gaps/monotonicity/drift are all measured from the recorded
	# packet timeline regardless of the absolute offset).
	timeout -k 5 "$((dur+6))" ffmpeg -hide_banner -nostdin -y -loglevel warning "${inopts[@]}" \
		-i "$url" -t "$dur" -c copy "$seg" </dev/null 2>"$err" || true
	t1=$(date +%s.%N)
	if [ -n "$pingpid" ]; then
		wait "$pingpid" 2>/dev/null || true
		ping_loss=$(grep -oE '[0-9.]+% packet loss' "$pingf" 2>/dev/null | grep -oE '^[0-9.]+' | head -1)
	fi
	wall=$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.3f", b-a}')

	if [ ! -s "$seg" ]; then
		bad "$label: no data captured (see $err)"; return 1
	fi
	local ffe
	# shared FFWARN_RE (see the helper section) - do NOT inline a pattern here
	ffe=$(ffwarn_count "$err")

	local probe="$OUTDIR/pkts_${label}.csv"
	# NOTE: ffprobe emits csv columns in a FIXED order (codec_type,pts_time here),
	# not the -show_entries order, so keep this to exactly the two fields the awk
	# below reads as $1 (codec_type) and $2 (pts_time).
	ffprobe -v error -show_entries packet=codec_type,pts_time \
		-of csv=p=0 "$seg" 2>/dev/null > "$probe"

	# awk: per codec_type -> count, first/last pts, max gap, non-monotonic count
	#
	# WARMUP_S excludes the connection/first-keyframe startup transient from
	# maxgap (and, in av_skew() below, from the skew start reference) - see the
	# long note on av_skew() for why that transient is a one-time locked offset
	# and not growing drift.
	local rep
	rep=$(awk -F, -v wall="$wall" -v warmup="${QA_WARMUP_S:-2}" '
	{
		ct=$1; p=$2+0;
		if(ct=="video"||ct=="audio"){
			n[ct]++;
			if(!(ct in first)){first[ct]=p; last[ct]=p; prev[ct]=p}
			if(prev[ct]>=warmup){g=p-prev[ct]; if(g>maxgap[ct])maxgap[ct]=g}
			if(p<prev[ct]-0.0005)nonmono[ct]++;
			prev[ct]=p; last[ct]=p;
		}
	}
	END{
		for(ct in n){
			span=last[ct]-first[ct];
			rate=(n[ct]>1&&span>0)?(n[ct]-1)/span:0;
			rt=(wall>0)?span/wall:0;
			printf "%s %d %.3f %.3f %.3f %.3f %d\n", ct, n[ct], span, rate, rt, maxgap[ct], nonmono[ct];
		}
	}' "$probe")
	# A/V skew comes from the shared av_skew() helper (also used by section 15b)
	# so the two can never disagree about HOW skew is measured. Appended in the
	# same "<field1> <field2> ..." shape the dispatch loop below already parses.
	local skewline
	skewline=$(av_skew "$probe")
	[ -n "$skewline" ] && rep="$rep
SKEW $skewline"

	info "$label: wall=${wall}s ffmpeg-warnings=$ffe"
	local vrate="" v_rt ratio
	v_rt=$(awk '$1=="video"{print $5}' <<<"$rep")
	# spans + max gaps of both tracks, needed by the SKEW verdict below to
	# tell a dropout-polluted endpoint from genuine A/V divergence
	local v_span a_span v_maxgap a_maxgap
	read -r v_span v_maxgap <<<"$(awk '$1=="video"{print $3, $6}' <<<"$rep")"
	read -r a_span a_maxgap <<<"$(awk '$1=="audio"{print $3, $6}' <<<"$rep")"
	while read -r ct n span rate rt maxgap nonmono; do
		case "$ct" in
		video)
			vrate="$rate"
			info "  video: pkts=$n span=${span}s fps=${rate} rt=${rt}x maxgap=${maxgap}s nonmono=$nonmono"
			# rt = media_span / wall. rt<1 is almost always RTSP connect + keyframe
			# SETUP overhead (benign, largest on short captures); rt>1 means media
			# arrives FASTER than real time = a wrong-clock / fast-forward bug.
			if fcmp "$rt" gt 1.20; then bad "$label video real-time rate ${rt}x >1.2 (fast-forward / wrong clock)"
			elif fcmp "$rt" lt 0.50; then
				# rt alone cannot separate "the server stalled" from "the
				# network path was dropping" - and per this script's own
				# WARN/FAIL line (environment=WARN, code=FAIL) a bad link
				# must not be reported as a defect. The parallel ping probe
				# above is the tie-breaker: significant ICMP loss during the
				# SAME window means the path itself was degraded (2% is an
				# order of magnitude above the ~0.1-0.2% residual WiFi loss
				# measured on a weak-but-working AP, see the UDP note below).
				if [ -n "$ping_loss" ] && fcmp "$ping_loss" ge 2; then
					warn "$label video real-time rate ${rt}x <0.5, but ${ping_loss}% ICMP loss was measured DURING the capture - the network path was degraded; this is an environment finding, not proven server stall (re-run on a clean link for a code verdict)"
				else
					bad "$label video real-time rate ${rt}x <0.5 (severe stall; ICMP loss during the capture: ${ping_loss:-unmeasured}% - the path looked clean, so suspect the server, not the network)"
				fi
			elif fcmp "$rt" ge 0.80; then ok "$label video real-time rate ${rt}x (healthy; <1 = capture setup overhead)"
			else warn "$label video real-time rate ${rt}x (marginal - setup overhead or mild stall)"; fi
			fcmp "$maxgap" le 1.0 && ok "$label video max frame gap ${maxgap}s" \
				|| warn "$label video max frame gap ${maxgap}s (possible freeze)"
			[ "${nonmono:-0}" -eq 0 ] && ok "$label video timestamps monotonic" \
				|| bad "$label video non-monotonic timestamps: $nonmono"
			;;
		audio)
			info "  audio: pkts=$n span=${span}s pkts/s=${rate} rt=${rt}x maxgap=${maxgap}s nonmono=$nonmono"
			# The audio sample-rate bug (e.g. 2x) makes AUDIO advance at a DIFFERENT
			# pace than video; connection/setup overhead cancels in the audio/video
			# ratio, so compare paces instead of audio rt vs 1.0 (which is setup-noisy).
			ratio=$(awk -v a="$rt" -v v="${v_rt:-0}" 'BEGIN{ if(v>0) printf "%.3f", a/v; else print "0" }')
			if [ "$ratio" = "0" ]; then
				fcmp "$rt" le 1.20 && fcmp "$rt" ge 0.50 && ok "$label audio real-time rate ${rt}x" \
					|| bad "$label audio real-time rate ${rt}x (rate mismatch?)"
			elif fcmp "$ratio" ge 0.85 && fcmp "$ratio" le 1.15; then
				ok "$label audio pace matches video (a/v ratio ${ratio}x) - no sample-rate mismatch"
			else
				bad "$label audio/video pace ratio ${ratio}x (audio at a different rate = sample-rate mismatch, e.g. the 2x bug)"
			fi
			fcmp "$maxgap" le 0.5 && ok "$label audio max gap ${maxgap}s" \
				|| warn "$label audio max gap ${maxgap}s (dropouts)"
			[ "${nonmono:-0}" -eq 0 ] && ok "$label audio timestamps monotonic" \
				|| bad "$label audio non-monotonic timestamps: $nonmono"
			;;
		SKEW)
			# ct=SKEW n=start span=end rate=drift
			local drift="$rate"
			info "  A/V skew start=${n}s end=${span}s drift=${drift}s"
			local ad; ad=$(awk -v d="$drift" 'BEGIN{printf "%.3f", (d<0?-d:d)}')
			# Track-span disagreement: PTS are wall-locked, so a transport
			# dropout (a hole in DELIVERY) shrinks neither track's first-to-
			# last span, while genuine rate divergence (the growing-desync
			# bug this check exists for) opens a span difference of about the
			# drift's own size. A large drift with AGREEING spans therefore
			# means a skew endpoint sat next to a dropout hole - a delivery
			# problem (reported separately by the max-gap checks), not tracks
			# drifting apart - and is downgraded to a WARN naming that.
			local sd; sd=$(awk -v a="${a_span:-0}" -v v="${v_span:-0}" \
				'BEGIN{d=a-v; printf "%.3f", (d<0?-d:d)}')
			if fcmp "$ad" le 0.15; then
				ok "$label A/V drift ${drift}s (in sync)"
			elif fcmp "$sd" le 0.30 && { fcmp "${a_maxgap:-0}" gt 1.0 || fcmp "${v_maxgap:-0}" gt 1.0; }; then
				warn "$label A/V drift ${drift}s is a dropout artifact, not divergence (track spans agree within ${sd}s; max gap a=${a_maxgap:-?}s v=${v_maxgap:-?}s put a skew endpoint beside a delivery hole - see the max-gap warnings)"
			elif fcmp "$ad" le 0.40; then
				warn "$label A/V drift ${drift}s (marginal)"
			else
				bad "$label A/V drift ${drift}s (out of sync: growing divergence, or one track stalled near the capture end)"
			fi
			;;
		esac
	done <<< "$rep"

	# UDP captures only: separate RTP packet-loss lines from real decode/
	# timestamp warnings before ruling. UDP RTSP has no retransmission by
	# design, so some residual WiFi loss is inherent transport behaviour the
	# daemon cannot act on - the TCP-interleaved capture minutes earlier in
	# the same run masks the identical underlying loss via TCP retransmit
	# (cam-A 2026-08-11: UDP 8 losses / TCP 0 warnings, same link). The
	# grounding for the 0.5% line: post-L2-retry residual loss on a weak-but-
	# working WiFi AP measures ~0.1-0.2% (that run: 8 of ~5800 datagrams =
	# 0.14%, every event a SINGLE packet, evenly scattered = RF loss, while a
	# sender-side burst/pacing defect would lose multi-packet runs clustered
	# at IDR bursts), and video-over-RTP is generally considered healthy
	# below ~0.5% loss. Loss above that = a genuinely degraded link, still a
	# FAIL. Non-loss warnings (corrupt/monotonicity/concealment) keep the
	# strict zero-tolerance ladder on every transport - loss tolerance must
	# never absorb real decode trouble.
	local rtp_lines=0 rtp_missed=0
	case "$label" in *udp*)
		rtp_lines=$(grep -c 'RTP: missed' "$err" 2>/dev/null || true)
		rtp_lines=${rtp_lines:-0}
		if [ "$rtp_lines" -gt 0 ]; then
			rtp_missed=$(awk '/RTP: missed/{for(i=1;i<NF;i++)if($i=="missed"){s+=$(i+1)+0}}END{print s+0}' "$err")
			ffe=$((ffe - rtp_lines)); [ "$ffe" -lt 0 ] && ffe=0
			# estimated datagram count: capture bytes / the 1200-byte default
			# rtsp.mtu (undercounts small audio packets -> overstates the loss
			# fraction, i.e. errs on the strict side)
			local est_pkts loss_pct
			est_pkts=$(( $(stat -c %s "$seg" 2>/dev/null || echo 0) / 1200 ))
			loss_pct=$(awk -v m="$rtp_missed" -v n="$est_pkts" \
				'BEGIN{printf "%.2f", ((m+n)>0 ? 100.0*m/(m+n) : 0)}')
			# Concealment/corruption lines that are the DIRECT DECODER
			# CONSEQUENCE of that same loss (a missing RTP packet means a
			# damaged slice, which ffmpeg then reports as concealing/corrupt/
			# error-while-decoding). The old code subtracted only the "RTP:
			# missed" lines and left their consequences in ffe, so one
			# tolerated WiFi loss event was simultaneously WARNed (as loss)
			# and FAILed (as "decode trouble") - the same datagram counted
			# twice, on both sides of the environment/code line. Attribute
			# the consequence lines to the loss verdict AS LONG AS the loss
			# itself is within the tolerated 0.5%; concealment with NO
			# corresponding RTP loss stays in ffe - that really is the
			# decoder in trouble on intact input. Monotonicity/discontinuity
			# lines are never subtracted on any transport.
			local conceal=0
			conceal=$(grep -icE 'concealing|corrupt|error while|decode_slice' "$err" 2>/dev/null)
			conceal=${conceal:-0}
			if fcmp "$loss_pct" le 0.50; then
				local cnote=""
				if [ "$conceal" -gt 0 ]; then
					ffe=$((ffe - conceal)); [ "$ffe" -lt 0 ] && ffe=0
					cnote="; $conceal concealment/corruption line(s) are that loss's decoder fallout and are counted here, not as decode failures"
				fi
				warn "$label: $rtp_missed RTP packet(s) lost over UDP (~${loss_pct}% of ~$est_pkts - ordinary residual WiFi loss; UDP has no retransmission, the TCP capture masks the same loss${cnote})"
			else
				bad "$label: $rtp_missed RTP packet(s) lost over UDP (~${loss_pct}% of ~$est_pkts - above the 0.5% healthy-link line, the network path is genuinely degraded)"
			fi
		fi
		;;
	esac
	[ "${ffe:-0}" -eq 0 ] && ok "$label: no ffmpeg decode/timestamp warnings" \
		|| { [ "${ffe:-0}" -le 3 ] && warn "$label: $ffe ffmpeg warnings (see $err)" \
		     || bad "$label: $ffe ffmpeg decode/timestamp warnings (see $err)"; }
	ANALYZE_VFPS="${vrate:-0}"   # returned via global so callers keep the pass/fail counters
}
ANALYZE_VFPS=0

# nominal fps per stream, populated in section 2; declared here so sections 3/13
# still resolve ${NOM_FPS[...]} when section 2 is skipped via --only
declare -A NOM_FPS

# ============================================================================= run
log "timps-qa  cam=$CAM  profile=$PROFILE  out=$OUTDIR  $(date)"
log "streams: rtsp://$CAM:$RTSP_PORT/{$PATH_MAIN,$PATH_SUB}  http://$CAM:$HTTP_PORT"

# --- 1. preflight -----------------------------------------------------------
hdr "1. Preflight"
for t in ffmpeg ffprobe curl; do
	have "$t" && ok "tool present: $t" || bad "missing required tool: $t"
done
have python3 && info "python3 present (JSON parsing)" || info "python3 absent (grep fallback for JSON)"
if [ -n "$SSH_TARGET" ]; then
	if sshx true 2>/dev/null; then ok "SSH to $SSH_TARGET works (on-device checks enabled)"
	else warn "SSH to $SSH_TARGET failed (on-device checks skipped)"; SSH_TARGET=""; fi
else info "no --ssh target (on-device checks skipped)"; fi

# Stale crash-log check (Finding #2, always-on, no flag needed). Thematically
# this belongs with the rest of the on-device checks (section 16), but it
# MUST run before section 14b's own opt-in --test-crash can create a FRESH
# /run/timps.crash (which runs much later in this script) - evaluated here
# instead, at the earliest point SSH is confirmed usable, so a pre-existing
# file is unambiguous evidence the daemon crashed at some earlier, UNNOTICED
# point since last boot, never an artifact of this run's own destructive test.
if [ -n "$SSH_TARGET" ]; then
	stale=$(sshx "[ -s /run/timps.crash ] && echo yes" 2>/dev/null)
	if [ "$stale" = "yes" ]; then
		mt=$(sshx "stat -c %y /run/timps.crash 2>/dev/null || stat /run/timps.crash 2>/dev/null | grep -i modify")
		warn "STALE /run/timps.crash found on $CAM (mtime: ${mt:-unknown}) - direct evidence timpsd crashed at some point since last boot and nobody looked. Contents:"
		sshx "cat /run/timps.crash" 2>/dev/null | sed 's/^/    /' | tee -a "$SUMMARY"
		# rename (not delete) so the evidence survives on-device for a human to
		# inspect later, but won't re-trigger this same warning on the NEXT
		# QA run against this camera
		sshx "mv -f /run/timps.crash /run/timps.crash.qa-seen-\$(date +%s 2>/dev/null || echo 0) 2>/dev/null || rm -f /run/timps.crash" >/dev/null 2>&1
		info "  renamed on-device to /run/timps.crash.qa-seen-* so it won't re-trigger this warning next run"
	else
		ok "no stale /run/timps.crash on $CAM (no unnoticed crash evidence since last boot)"
	fi
fi

ping -c1 -W2 "$CAM" >/dev/null 2>&1 && ok "camera $CAM reachable (ping)" || warn "ping $CAM failed (may be firewalled)"
for p in "$RTSP_PORT" "$HTTP_PORT"; do
	# NOTE: fd 3 is opened inside the (subshell) probe only, so it never leaks
	# into this shell - no cleanup needed. Do NOT write `exec 3>&- 2>/dev/null`
	# here: with no command, that `exec` applies its redirection to THIS shell
	# permanently, silently sending all later stderr (and any `set -x` trace) to
	# /dev/null for the rest of the run.
	if (exec 3<>"/dev/tcp/$CAM/$p") 2>/dev/null; then ok "tcp port $p open"
	else bad "tcp port $p closed/unreachable"; fi
done

have ffprobe || { bad "ffprobe missing - aborting stream tests"; }

if want 1b version identity; then
# --- 1b. Build identity -------------------------------------------------
# 2026-08 fleet incident: fw_ota.sh's flash script logged "Firmware flashed
# successfully" on multiple cameras whose /usr/bin/timpsd binary had
# demonstrably NOT changed post-reboot (confirmed by hand via MD5 + mtime) -
# the flash script's own success signal only proves a reboot was triggered,
# not that the new binary is what came back up. GET /control now reports a
# "version" key (git describe's tag+commit+dirty string, the same constant
# `timpsd -v` prints); surface it prominently, early, before the bulk of
# testing, so a human glancing at the run immediately sees what build is
# actually answering - this is a visibility fix, not a pass/fail gate (there
# is no "expected version" to compare against from the host side in general).
hdr "1b. Build identity"
vj="$OUTDIR/version.json"
if curlq 8 "$(http_base)/control" -o "$vj" && [ -s "$vj" ]; then
	dev_ver=$(jget "$vj" version)
	if [ -n "$dev_ver" ]; then
		info "camera $CAM reports: timps $dev_ver"
	else
		warn "/control reachable but has no \"version\" field (older timpsd build without this check)"
	fi
else
	warn "/control unreachable for the version check (see $vj)"
fi
# Optional, purely informational convenience: this script lives in the same
# repo as the firmware source, so print the LOCAL checkout's own git describe
# alongside the device's for a quick manual eyeball-diff - exactly the
# scenario above (does the camera's reported build match what was just
# pushed?). Never a warn/bad: an older camera legitimately running behind
# this checkout is normal, not a defect this script should flag.
if have git; then
	local_ver=$(git -C "$(dirname "$0")" describe --tags --always --dirty 2>/dev/null)
	[ -n "$local_ver" ] && info "  (this checkout's git describe: $local_ver - compare by eye; a mismatch is often expected, not a failure)"
fi

fi
if want 2 discovery; then
# --- 2. discovery -----------------------------------------------------------
hdr "2. Discovery (ffprobe)"
# NOM_FPS ("nominal fps") used to be ffprobe's live r_frame_rate. Measured on
# a real camera 2026-08-19 and every link verified: configured 25 (video1.fps
# config default, config.c:269), sensor commanded 30 (auto-detected from
# /proc/jz/sensor/max_fps), SPS/VUI advertises exactly 25 (num_units_in_tick=1
# time_scale=50), delivered 24.65 - and ffprobe's r_frame_rate over that same
# RTSP session read 29.67, matching NONE of the above. Over live RTSP, ffmpeg
# derives r_frame_rate from the SMALLEST observed inter-frame timestamp delta
# and rationalises it, so what it actually reports is the SENSOR TICK RATE:
# the Ingenic framesource divider is a frame-COUNT gate (keep 25 of every 30
# sensor frames) and the sensor undertakes slightly, so 29.6 * 25/30 = 24.7 -
# confirmed exactly by the interval histogram (589 single-tick gaps, 147
# double-tick gaps, nothing else). That means the old reference was wrong on
# every correctly-configured camera whose sensor rate exceeds its configured
# fps (most of the fleet), producing false "off nominal" warnings here, a
# false ONVIF-advertises-template-defaults accusation in section 10, and false
# "degrading" verdicts in section 13/13b's load tests.
#
# NOM_FPS is now /control's declared video.<N>.fps: authoritative, exact, an
# integer, available with a single cheap GET, and precisely what ONVIF ought
# to be advertising - which is what section 10 actually needs to compare
# against. The alternative considered was reading it back out of the SPS/VUI
# of the file section 3 records: that's real evidence too (it agreed with
# /control in the measurement above), but it only exists AFTER section 3
# records a stream, which breaks NOM_FPS resolving from section 2 alone per
# the --only contract at the `declare -A NOM_FPS` above, needs its own VUI
# parsing ffprobe doesn't expose via -show_streams, and can only ever repeat
# what /control already states more directly - so /control is queried here
# instead. ffprobe's r_frame_rate is still shown below for the record (it is
# genuinely informative - e.g. it will reveal a sensor running slower than
# expected) but it no longer feeds any pass/fail comparison.
#
# This does NOT weaken the check this section exists for: section 3 measures
# the DELIVERED fps off the wire and still compares it to this same declared
# reference, so a camera whose encoder over/under-delivers against its own
# configured+SPS-advertised rate (real example, 2026-08-17: configured 15,
# SPS 15, delivered 30) still fails loudly - only the reference camera 15/25
# example above is now measured correctly too.
n2_cj="$OUTDIR/control_nomfps.json"
curlq 8 "$(http_base)/control" -o "$n2_cj" 2>/dev/null || true
for pair in "main:$PATH_MAIN" "sub:$PATH_SUB"; do
	lbl="${pair%%:*}"; pth="${pair##*:}"
	url="$(rtsp_url "$pth")"
	j="$OUTDIR/probe_${lbl}.json"
	if timeout 20 ffprobe -v error -rtsp_transport "$RTSP_TRANSPORT" -show_streams -of json "$url" > "$j" 2>"$OUTDIR/probe_${lbl}.err"; then
		vcodec=$(jget "$j" codec_name)
		info "$lbl ($pth):"
		# summarise each stream line
		ffprobe -v error -rtsp_transport "$RTSP_TRANSPORT" -show_entries \
			stream=codec_type,codec_name,width,height,r_frame_rate,sample_rate,channels \
			-of csv=p=0 "$url" 2>/dev/null | while IFS= read -r line; do info "    $line"; done
		fr=$(ffprobe -v error -rtsp_transport "$RTSP_TRANSPORT" -select_streams v:0 \
			-show_entries stream=r_frame_rate -of csv=p=0 "$url" 2>/dev/null)
		fnum=$(awk -F/ 'NF==2&&$2>0{printf "%.2f",$1/$2; next}{print $1}' <<<"$fr")
		# /control's video.<N> index for this label. PATH_MAIN/PATH_SUB default
		# to ch0/ch1 but are user-overridable (--main/--sub), and rtsp_path is
		# itself a per-channel F_CTRL field (video<N>.rtsp_path, config.c:985),
		# so "main" is NOT reliably chn 0 - derive it from the path's trailing
		# digit instead of hardcoding main=0/sub=1. Only fall back to that
		# convention when the path carries no digit to derive from, and say so
		# rather than silently comparing against chn 0 for whichever channel it
		# actually is.
		fb=0; [ "$lbl" = sub ] && fb=1
		chn=$(grep -oE '[0-9]+' <<<"$pth" | tail -1)
		if [ -z "$chn" ]; then
			info "$lbl: RTSP path \"$pth\" has no channel digit - cannot derive its /control channel for the fps reference, falling back to the conventional chn $fb (main=0/sub=1)"
			chn="$fb"
		fi
		declfps=$(jget "$n2_cj" "video.$chn.fps" 2>/dev/null)
		if [ -n "$declfps" ] && fcmp "$declfps" gt 0; then
			NOM_FPS[$lbl]="$declfps"
			ok "$lbl advertises video + $( [ -n "$(ffprobe -v error -rtsp_transport "$RTSP_TRANSPORT" -select_streams a:0 -show_entries stream=codec_name -of csv=p=0 "$url" 2>/dev/null)" ] && echo audio || echo 'NO audio') (declared ${declfps} fps via /control video.$chn.fps; ffprobe's live r_frame_rate read ${fnum} - that's the RTSP sensor tick rate, not the configured rate, see comment above)"
		else
			NOM_FPS[$lbl]="$fnum"
			warn "$lbl: /control video.$chn.fps unavailable (see $n2_cj) - falling back to ffprobe's r_frame_rate (${fnum}) as nominal; note that over live RTSP this is the SENSOR TICK RATE, not the configured fps (see comment above), so downstream fps checks may misfire until /control is reachable"
		fi
	else
		bad "$lbl: ffprobe could not open $url (see probe_${lbl}.err)"
	fi
done

fi
if want 2b auth; then
# --- 2b. Auth enforcement (no/wrong credentials must be blocked) -------------
hdr "2b. Auth enforcement (unauthenticated must be blocked)"
# HTTP surfaces: a request with NO Authorization header must get 401/403.
# Only a 2xx is a LEAK: a 404/5xx means the request was refused/failed for a
# non-auth reason and no data was served, so it proves nothing either way -
# the old catch-all "bad" here FAILed the sim's /snapshot.jpg 503 ("no frame")
# as "served WITHOUT auth", i.e. claimed a leak the measurement cannot show.
# Same 2xx-only rule test_auth.sh has always applied.
check_noauth() { # <url> <label>
	local code
	code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 8 "$1")
	case "$code" in
		401|403) ok "$2 blocks no-auth (HTTP $code)";;
		000)     warn "$2 unreachable (HTTP 000) - cannot judge";;
		2*)      bad "$2 served WITHOUT auth (HTTP $code) - NOT protected";;
		*)       skip "$2 no-auth -> HTTP $code (not a 2xx leak, but no explicit 401/403 either - nothing proven)";;
	esac
}
if is_loopback "$CAM"; then
	# httpd trusts 127.0.0.0/8 by design (see the is_loopback helper) - every
	# HTTP negative below would "fail" against the intended bypass, not the
	# auth code. The positives further down still run (they prove reachability
	# and that correct credentials are not rejected).
	skip "HTTP auth negatives skipped: $CAM is loopback and httpd trusts 127.0.0.0/8 on purpose - run against the camera's LAN address (or the sim via the host's LAN IP) to exercise HTTP auth"
else
	check_noauth "$(http_base)/control"            "/control"
	check_noauth "$(http_base)/events"             "/events"
	check_noauth "$(http_base)/snapshot.jpg?chn=0" "/snapshot.jpg"
	check_noauth "$(http_base)/stream.mp4?chn=0"   "/stream.mp4"
	check_noauth "$(http_base)/stream.mjpeg?chn=0" "/stream.mjpeg"

	# wrong password must also be rejected (proves it is not "any credential passes")
	wcode=$(curl -s -o /dev/null -w '%{http_code}' --max-time 8 -u "$HTTP_USER:wrong_$$" "$(http_base)/control")
	case "$wcode" in
		401|403) ok "/control rejects WRONG password (HTTP $wcode)";;
		000)     warn "/control unreachable for wrong-pass test (HTTP 000)";;
		2*)      bad "/control accepted WRONG password (HTTP $wcode)";;
		*)       skip "/control wrong-pass -> HTTP $wcode (no 2xx leak, no explicit reject)";;
	esac

	# --- Digest (httpd.c offers "WWW-Authenticate: Digest ... qop=auth" FIRST
	# in every 401; auth.c carries a complete digest implementation with nonce
	# tracking and realm binding). curl -u sends Basic, so nothing in this
	# script - or in test_auth.sh - had ever exercised that code: a broken
	# digest path would only have surfaced when a real NVR tried it. ---------
	dcode=$(curl -s -o /dev/null -w '%{http_code}' --max-time 8 --digest -u "$HTTP_USER:wrong_$$" "$(http_base)/control")
	case "$dcode" in
		401|403) ok "/control rejects WRONG password via Digest (HTTP $dcode)";;
		000)     warn "/control unreachable for the digest wrong-pass test";;
		2*)      bad "/control accepted a WRONG password via Digest (HTTP $dcode) - digest verification broken";;
		*)       skip "/control digest wrong-pass -> HTTP $dcode";;
	esac
	dcode=$(curl -s -o /dev/null -w '%{http_code}' --max-time 8 --digest -u "$HTTP_USER:$HTTP_PASS" "$(http_base)/control")
	case "$dcode" in
		2*)      ok "/control accepts correct credentials via Digest (HTTP $dcode) - the advertised digest path actually works";;
		401|403) bad "/control REJECTS correct credentials via Digest (HTTP $dcode) - the 401 advertises a digest scheme that then doesn't verify";;
		000)     warn "/control unreachable for the digest positive test";;
		*)       skip "/control digest positive -> HTTP $dcode";;
	esac

	# --- Digest realm binding (regression test for the auth.c hardening: a
	# client echoing a realm the server never issued must be rejected BEFORE
	# any hash comparison, because the realm is an HA1 input - a forged realm
	# would otherwise let a cross-service digest replay through). curl always
	# echoes the server's realm, so this needs a hand-rolled Authorization
	# header: compute a CORRECT RFC-2069 response over a WRONG realm (exactly
	# what a confused/malicious client would send) and require a 401. The
	# correct-realm twin request proves the hand-rolled digest itself is
	# valid, so the wrong-realm 401 is attributable to the BINDING and not to
	# a mistake in this test's own arithmetic. Fresh nonce per attempt: the
	# server tracks nonces, and reusing one across attempts would make the
	# second verdict about nonce state instead of the realm. -----------------
	if have md5sum; then
		dg_md5() { printf '%s' "$1" | md5sum | cut -d' ' -f1; }
		dg_try() {  # <realm> -> http code for a self-computed digest GET /control
			local realm="$1" nonce ha1 ha2 resp
			nonce=$(curl -s -D - -o /dev/null --max-time 8 "$(http_base)/control" 2>/dev/null \
				| grep -oiE 'nonce="[0-9a-f]+"' | head -1 | cut -d'"' -f2)
			[ -n "$nonce" ] || { echo "nononce"; return; }
			ha1=$(dg_md5 "$HTTP_USER:$realm:$HTTP_PASS")
			ha2=$(dg_md5 "GET:/control")
			resp=$(dg_md5 "$ha1:$nonce:$ha2")
			curl -s -o /dev/null -w '%{http_code}' --max-time 8 \
				-H "Authorization: Digest username=\"$HTTP_USER\", realm=\"$realm\", nonce=\"$nonce\", uri=\"/control\", response=\"$resp\"" \
				"$(http_base)/control"
		}
		dg_good=$(dg_try "timps")     # AUTH_REALM, src/auth.h
		if [ "$dg_good" = "nononce" ]; then
			skip "digest realm-binding test: no Digest challenge in the 401 (older build?) - cannot craft a request"
		elif [ "${dg_good#2}" = "$dg_good" ]; then   # not 2xx
			warn "digest realm-binding test not sharp: even the CORRECT-realm hand-rolled digest got HTTP $dg_good (legacy RFC-2069 form no longer accepted?) - the wrong-realm verdict below would be meaningless, skipping it"
		else
			dg_bad=$(dg_try "qa_wrong_realm")
			case "$dg_bad" in
				401|403) ok "digest realm binding holds: a correctly-computed response over a FORGED realm is rejected (HTTP $dg_bad; correct realm passed with $dg_good)";;
				2*)      bad "digest realm binding BROKEN: a response computed over a realm this server never issued was accepted (HTTP $dg_bad) - cross-service digest replay is possible";;
				*)       skip "digest realm-binding: wrong-realm request -> HTTP $dg_bad";;
			esac
		fi
	else
		skip "digest realm-binding test needs md5sum - not found"
	fi

	# --- token surface (?token= / X-Timps-Token, httpd.c http_check_token):
	# never exercised by anything before - neither rejection of a bad token
	# nor acceptance of the real one. A wrong token must fall through to the
	# normal 401; the real one (the per-boot secret in http.token_file,
	# default /run/timps.token, readable only on-device -> needs --ssh, or
	# hand it in via HTTP_TOKEN=) must unlock /control in BOTH transport
	# forms, because header and query are parsed by different code paths. ----
	tcode=$(curl -s -o /dev/null -w '%{http_code}' --max-time 8 "$(http_base)/control?token=qa_wrong_$$")
	case "$tcode" in
		401|403) ok "/control rejects a WRONG ?token= (HTTP $tcode)";;
		000)     warn "/control unreachable for the wrong-token test";;
		2*)      bad "/control accepted a WRONG ?token= (HTTP $tcode) - token comparison broken";;
		*)       skip "/control wrong-token -> HTTP $tcode";;
	esac
	QA_TOKEN="${HTTP_TOKEN:-}"
	if [ -z "$QA_TOKEN" ] && [ -n "$SSH_TARGET" ]; then
		QA_TOKEN=$(sshx "cat ${TOKEN_FILE:-/run/timps.token} 2>/dev/null" | head -1 | tr -d ' \r\n')
	fi
	if [ -n "$QA_TOKEN" ]; then
		for tf in "query:$(http_base)/control?token=$QA_TOKEN" "header:$(http_base)/control"; do
			tlbl="${tf%%:*}"; turl="${tf#*:}"
			if [ "$tlbl" = header ]; then
				tcode=$(curl -s -o /dev/null -w '%{http_code}' --max-time 8 -H "X-Timps-Token: $QA_TOKEN" "$turl")
			else
				tcode=$(curl -s -o /dev/null -w '%{http_code}' --max-time 8 "$turl")
			fi
			case "$tcode" in
				2*)      ok "/control accepts the real token via $tlbl (HTTP $tcode)";;
				401|403) bad "/control REJECTS the real token via $tlbl (HTTP $tcode) - token auth broken (or the token file is stale)";;
				*)       warn "/control real-token via $tlbl -> HTTP $tcode";;
			esac
		done
	else
		info "  token acceptance not tested: no --ssh to read ${TOKEN_FILE:-/run/timps.token} and no HTTP_TOKEN= given (rejection of a wrong token was still verified above)"
	fi
fi

# --- OPTIONS / CORS preflight (httpd.c answers 204 BEFORE auth by design - a
# preflight carries no credentials; a regression here silently breaks every
# cross-origin WebUI page while all the authenticated tests keep passing).
# Auth-free, so this runs against loopback too. --------------------------------
pf="$OUTDIR/preflight.txt"
pcode=$(curl -s -D "$pf" -o /dev/null -w '%{http_code}' --max-time 8 -X OPTIONS \
	-H "Origin: http://qa.invalid" "$(http_base)/control")
if [ "$pcode" = "204" ] && grep -qi '^Access-Control-Allow-Origin: http://qa.invalid' "$pf"; then
	ok "OPTIONS /control preflight: 204 with the Origin reflected (CORS preflight path alive)"
elif [ "$pcode" = "000" ]; then
	warn "OPTIONS /control preflight unreachable (HTTP 000)"
else
	warn "OPTIONS /control preflight: HTTP $pcode, Origin reflected: $(grep -ci '^Access-Control-Allow-Origin:' "$pf" 2>/dev/null || echo 0) (want 204 + echo; see $pf)"
fi

# RTSP: DESCRIBE without credentials must not open the stream
rerr="$OUTDIR/auth_rtsp.err"
if timeout 12 ffprobe -v error -rtsp_transport "$RTSP_TRANSPORT" -show_streams \
		"rtsp://$CAM:$RTSP_PORT/$PATH_MAIN" >/dev/null 2>"$rerr"; then
	bad "RTSP $PATH_MAIN opened WITHOUT credentials - NOT protected"
elif grep -qiE '401|unauthor' "$rerr"; then
	ok "RTSP $PATH_MAIN blocks no-auth (401)"
else
	warn "RTSP $PATH_MAIN did not open without creds, but no explicit 401 (see $rerr)"
fi

# RTSP: a WRONG password must be rejected too (not "any credential passes")
rwerr="$OUTDIR/auth_rtsp_wrong.err"
if timeout 12 ffprobe -v error -rtsp_transport "$RTSP_TRANSPORT" -show_streams \
		"rtsp://$RU:wrong_$$@$CAM:$RTSP_PORT/$PATH_MAIN" >/dev/null 2>"$rwerr"; then
	bad "RTSP $PATH_MAIN opened with WRONG password - NOT protected"
elif grep -qiE '401|unauthor' "$rwerr"; then
	ok "RTSP $PATH_MAIN rejects WRONG password (401)"
else
	warn "RTSP $PATH_MAIN did not open with wrong pass, but no explicit 401 (see $rwerr)"
fi

# --- positive counter-tests: prove the checks above are SHARP, i.e. the
# CORRECT credentials are actually accepted (else "everything is blocked"
# would pass the negatives for the wrong reason). -----------------------------
# HTTP: correct creds must NOT be rejected (2xx expected; a 5xx still proves
# auth was accepted before any body, which is all we assert here).
check_auth_ok() { # <url> <label>
	local code
	code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 8 -u "$HTTP_USER:$HTTP_PASS" "$1")
	case "$code" in
		401|403) bad "$2 REJECTS correct credentials (HTTP $code) - auth misconfigured / test not sharp";;
		000)     warn "$2 unreachable for positive auth test (HTTP 000)";;
		*)       ok "$2 accepts correct credentials (HTTP $code)";;
	esac
}
check_auth_ok "$(http_base)/control"            "/control"
check_auth_ok "$(http_base)/snapshot.jpg?chn=0" "/snapshot.jpg"

# RTSP: correct creds must open the stream (401 here = broken/misconfigured)
rgerr="$OUTDIR/auth_rtsp_good.err"
if timeout 12 ffprobe -v error -rtsp_transport "$RTSP_TRANSPORT" -show_streams \
		"$(rtsp_url "$PATH_MAIN")" >/dev/null 2>"$rgerr"; then
	ok "RTSP $PATH_MAIN opens with correct credentials (test is sharp)"
elif grep -qiE '401|unauthor' "$rgerr"; then
	bad "RTSP $PATH_MAIN REJECTS correct credentials (401) - auth misconfigured / test not sharp"
else
	warn "RTSP $PATH_MAIN with correct creds did not open (non-auth error, see $rgerr)"
fi

# Deep, transport-level negatives (RTSP SETUP, HTTP POST /control, per-endpoint
# wrong-pass) live in scripts/test_auth.sh - run it for a focused fail-closed
# audit:  scripts/test_auth.sh --host $CAM --rtsp-port $RTSP_PORT --http-port $HTTP_PORT

fi
if want 2c backchannel bc; then
# --- 2c. ONVIF audio backchannel (real handshake + short tone) --------------
hdr "2c. Audio backchannel (optional)"
# Uses bc-send.py (same dir): DESCRIBE+Require -> SETUP trackID=2 -> PLAY, then
# streams a short PCMU tone to the camera speaker. Optional feature, so a
# missing/disabled backchannel is info, not FAIL. With --ssh we also confirm
# timps actually received it (acquired the speaker / native IMP_AO).
bcpy="$(dirname "$0")/bc-send.py"
bclog="$OUTDIR/backchannel.log"
if ! have python3; then
	info "backchannel test skipped (needs python3)"
elif [ ! -f "$bcpy" ]; then
	info "backchannel test skipped (scripts/bc-send.py not found)"
elif python3 "$bcpy" --host "$CAM" --port "$RTSP_PORT" --path "$PATH_MAIN" \
        --user "$RTSP_USER" --pw "$RTSP_PASS" --secs 2 > "$bclog" 2>&1; then
	bcname=$(grep -oE 'rtpmap:[0-9]+ [A-Za-z0-9-]+' "$bclog" | tail -1 | awk '{print $2}')
	ok "backchannel handshake ok (SDP trackID=2, SETUP+PLAY) - sent 2s ${bcname:-PCMU} tone"
	if [ -n "$SSH_TARGET" ]; then
		if sshx "logread 2>/dev/null | grep -q 'speaker owner acquired'"; then
			ok "camera received backchannel audio (speaker owner acquired, native IMP_AO)"
		else
			warn "tone sent but no 'speaker owner acquired' in logread - not confirmed received"
		fi
	else
		info "  pass --ssh root@$CAM to confirm the camera acquired the speaker (IMP_AO)"
	fi
elif grep -q "no backchannel" "$bclog"; then
	info "no backchannel advertised (audio.backchannel off / not built) - optional"
else
	warn "backchannel handshake failed (see $bclog)"
fi

fi
if [ "$TEST_BACKCHANNEL" = "1" ]; then
# --- 2d. Backchannel acoustic loopback (opt-in: --test-backchannel) ----------
hdr "2d. Backchannel acoustic loopback (optional)"
# End-to-end check WITHOUT a human listening: drive a clean sine into the camera
# speaker over the backchannel; because the speaker and mic share one enclosure,
# the tone should couple acoustically into timps' own outgoing audio. We record
# that outgoing audio and measure band energy at the tone frequency vs a control
# band - a clear excess means the loop closed (speaker played + mic heard it).
#
# This depends on PHYSICAL acoustics (mic/speaker proximity, playback volume,
# ambient noise), so it is far less deterministic than the digital checks and is
# therefore OFF by default and never part of quick/standard/load/soak. A FAIL
# here can be environmental (too quiet, muted amp) rather than a code defect.
bcpy="$(dirname "$0")/bc-send.py"
rec="$OUTDIR/bc_loopback.wav"
sendlog="$OUTDIR/bc_loopback_send.log"
reclog="$OUTDIR/bc_loopback_rec.log"
aurl="$(rtsp_url "$PATH_MAIN")"
F="$BC_TEST_FREQ"
CTRL=$(awk -v f="$F" 'BEGIN{print (f>=2500)? f-1200 : f+1500}')   # a quiet control band
DUR="$BC_TEST_SECS"

# ffmpeg mean level (dBFS, negative) of a narrow band around <freq> in <file>
band_db() {
	ffmpeg -nostdin -hide_banner -loglevel info -i "$1" \
		-af "bandpass=f=$2:width_type=h:w=140,volumedetect" -f null - 2>&1 \
		| grep -oE 'mean_volume: *-?[0-9.]+ dB' | grep -oE '\-?[0-9.]+' | head -1
}

if ! have python3; then skip "acoustic loopback: needs python3"
elif [ ! -f "$bcpy" ]; then skip "acoustic loopback: scripts/bc-send.py not found"
elif ! have ffmpeg; then skip "acoustic loopback: needs ffmpeg"
elif [ -z "$(ffprobe -v error -rtsp_transport "$RTSP_TRANSPORT" -select_streams a:0 \
		-show_entries stream=codec_name -of csv=p=0 "$aurl" 2>/dev/null)" ]; then
	warn "acoustic loopback: no outgoing audio stream on $PATH_MAIN (enable mic codec + restart)"
else
	info "tone=${F}Hz control=${CTRL}Hz dur=${DUR}s - recording outgoing audio while playing"
	# record the mic (outgoing) audio, forced mono/16k, for the tone window + margin
	timeout -k 5 "$((DUR+6))" ffmpeg -hide_banner -nostdin -y -loglevel warning \
		-rtsp_transport "$RTSP_TRANSPORT" -i "$aurl" -t "$((DUR+2))" \
		-map a:0 -ac 1 -ar 16000 "$rec" </dev/null >"$reclog" 2>&1 &
	recpid=$!
	sleep 1   # let the recording establish before the tone starts
	python3 "$bcpy" --host "$CAM" --port "$RTSP_PORT" --path "$PATH_MAIN" \
		--user "$RTSP_USER" --pw "$RTSP_PASS" --freq "$F" --secs "$DUR" \
		> "$sendlog" 2>&1 || true
	wait "$recpid" 2>/dev/null || true

	if [ ! -s "$rec" ]; then
		warn "acoustic loopback: no audio captured (see $reclog) - inconclusive"
	elif grep -q "no backchannel" "$sendlog"; then
		info "acoustic loopback: backchannel not advertised - skipped"
	else
		et=$(band_db "$rec" "$F"); ec=$(band_db "$rec" "$CTRL")
		if [ -z "$et" ] || [ -z "$ec" ]; then
			warn "acoustic loopback: could not measure band energy (see $reclog)"
		else
			delta=$(awk -v a="$et" -v b="$ec" 'BEGIN{printf "%.1f", a-b}')
			info "tone-band ${et}dB, control-band ${ec}dB, delta ${delta}dB"
			if fcmp "$delta" ge 12; then
				ok "acoustic loopback: tone clearly detected in mic (delta ${delta}dB >= 12) - speaker+mic loop verified"
			elif fcmp "$delta" ge 5; then
				warn "acoustic loopback: tone weakly present (delta ${delta}dB) - raise volume / quieter room to confirm"
			else
				# WARN, not FAIL: this section's own header says "a FAIL here
				# can be environmental ... rather than a code defect", and the
				# script's verdict rule is environment=WARN, code=FAIL. A
				# muted amp or a quiet room must not set exit code 2 on an
				# otherwise clean run - the measurement cannot separate
				# "speaker code broken" from "room ate the tone".
				warn "acoustic loopback: tone NOT detected (delta ${delta}dB < 5) - speaker silent, muted, or too quiet. Env-dependent by design (see this section's header), so this is a WARN: verify by ear / raise volume; only a repeat failure in a controlled setup points at the code"
			fi
		fi
	fi
fi

fi
if want 3 integrity; then
# --- 3. integrity + A/V sync ------------------------------------------------
hdr "3. Stream integrity + A/V sync (record ${INTEG_DUR}s each, transport=$RTSP_TRANSPORT)"
for pair in "main:$PATH_MAIN" "sub:$PATH_SUB"; do
	lbl="${pair%%:*}"; pth="${pair##*:}"
	analyze_stream "$(rtsp_url "$pth")" "rtsp_$lbl" "$INTEG_DUR" -rtsp_transport "$RTSP_TRANSPORT"
	vr="$ANALYZE_VFPS"
	nf="${NOM_FPS[$lbl]:-0}"
	if fcmp "$nf" gt 0 && fcmp "$vr" gt 0; then
		lo=$(awk -v n="$nf" 'BEGIN{printf "%.2f",n*0.9}'); hi=$(awk -v n="$nf" 'BEGIN{printf "%.2f",n*1.1}')
		fcmp "$vr" ge "$lo" && fcmp "$vr" le "$hi" && ok "rtsp_$lbl fps ${vr} within 10% of nominal ${nf}" \
			|| warn "rtsp_$lbl fps ${vr} off nominal ${nf} (>10%)"
	fi
done
# Explicit UDP pass on the main stream. Everything else in this script runs at
# $RTSP_TRANSPORT (tcp by default), yet the UDP path is genuinely different
# code: its own send batching (rtsp.c:95-101), its own orphaned-session reaping
# (2x the advertised 60s session timeout, rtsp.c:45/1361), and - unlike TCP - no
# SO_SNDTIMEO backstop is even possible, because sendto() on an unconnected UDP
# socket never errors no matter what the peer does. Skipped when the run is
# already UDP, so this never doubles the work for `--transport udp`.
if [ "$RTSP_TRANSPORT" != "udp" ]; then
	analyze_stream "$(rtsp_url "$PATH_MAIN")" "rtsp_main_udp" "$INTEG_DUR" -rtsp_transport udp
else
	skip "explicit UDP integrity pass (this run is already --transport udp)"
fi

fi
if want 4 fmp4; then
# --- 4. HTTP fMP4 -----------------------------------------------------------
hdr "4. HTTP fMP4 (/stream.mp4)"
murl="$(http_base)/stream.mp4?chn=0"
# AUTH_HDR (the Basic header for ffmpeg's -headers) lives with the global
# helpers now - section 5 uses it too, and defining it here broke --only 5
analyze_stream "$murl" "fmp4_main" "$INTEG_DUR" -headers "$AUTH_HDR"$'\r\n'

fi
if want 4b srt; then
# --- 4b. SRT (optional MPEG-TS output) --------------------------------------
# src/config.c has had srt_fields and a live src/srt.c listener for a while,
# but this script never opened an SRT socket - zero coverage. Two independent
# gates before attempting anything: does THIS HOST's ffmpeg even support
# srt:// (needs libsrt at ffmpeg build time, not universal), and does THIS
# CAMERA have SRT compiled in + enabled (via the new "srt" status block in
# GET /control, added alongside this test - there was previously no way to
# discover srt.enabled/port from the outside at all). Either gate failing
# skips cleanly rather than failing - this is a genuinely optional feature.
hdr "4b. SRT (optional MPEG-TS output)"
srt_proto_ok=0
if have ffmpeg && ffmpeg -hide_banner -protocols 2>/dev/null | grep -qiE '^[[:space:]]*srt[[:space:]]*$'; then
	srt_proto_ok=1
fi
if [ "$srt_proto_ok" != "1" ]; then
	skip "SRT: this host's ffmpeg has no srt:// protocol support (built without libsrt) - cannot test even if the camera has it"
else
	sj="$OUTDIR/srt_caps.json"
	if ! curlq 8 "$(http_base)/control" -o "$sj" || [ ! -s "$sj" ]; then
		warn "SRT: cannot GET /control to check srt capability - skipping"
	else
		srt_avail=$(jget "$sj" srt.available)
		if [ "${srt_avail:-0}" != "1" ]; then
			skip "SRT: not compiled into this camera's build (srt.available=0 / absent) - USE_SRT not set"
		else
			srt_en=$(jget "$sj" srt.enabled)
			srt_port=$(jget "$sj" srt.port)
			if [ "${srt_en:-0}" != "1" ]; then
				info "SRT: compiled in but srt.enabled=0 on this camera - skipping (enable srt.enabled + restart to test)"
			elif [ -z "$srt_port" ]; then
				warn "SRT: enabled but no port reported by /control - skipping"
			else
				# TCP-port-open (section 1's preflight loop) is not a meaningful
				# signal for SRT - it is a UDP protocol. Just hand the srt:// URL
				# straight to the SAME analyze_stream core sections 3/4 already
				# use (ffprobe/ffmpeg handle srt:// like any other input URL) -
				# no parallel bespoke analysis path. If this camera's
				# srt.streamid/passphrase are configured (deliberately NOT
				# exposed via /control - they are credentials, same treatment as
				# rtsp/http passwords), an unauthenticated caller connection
				# attempt here will legitimately fail to receive data; that
				# surfaces as analyze_stream's normal "no data captured" bad
				# below like any other unreachable stream, not a special case.
				info "SRT: available, enabled, port $srt_port - connecting as an SRT caller (srt://$CAM:$srt_port)"
				analyze_stream "srt://$CAM:$srt_port" "srt_main" "$INTEG_DUR"
			fi
		fi
	fi
fi

fi
if want 5 mjpeg; then
# --- 5. MJPEG ---------------------------------------------------------------
hdr "5. MJPEG (/stream.mjpeg)"
mjurl="$(http_base)/stream.mjpeg?chn=0"
mjlog="$OUTDIR/mjpeg.log"
# nojpeg configs exist ON PURPOSE (scripts/camera-portrait-nojpeg.conf): a
# build with the JPEG pipeline disabled answers 404 "no jpeg"
# (httpd.c jpeg_src_from_path -> src<0). That is the camera working exactly
# as configured - "configuration unsuitable for this test", not "defect" -
# so it must skip, not FAIL. 503 ("busy"/"no frame") means the pipeline IS
# there but has no frame yet, which the real capture below judges properly.
mjpre=$(curl -s -o /dev/null -w '%{http_code}' --max-time 6 -u "$HTTP_USER:$HTTP_PASS" "$(http_base)/snapshot.jpg?chn=0")
if [ "$mjpre" = "404" ]; then
	skip "MJPEG: no JPEG source on this config (snapshot probe -> 404 'no jpeg') - a deliberate nojpeg build/config, working as configured"
else
	timeout -k 5 "$((INTEG_DUR+5))" ffmpeg -hide_banner -nostdin -stats -y -loglevel warning -headers "$AUTH_HDR"$'\r\n' \
		-i "$mjurl" -t "$INTEG_DUR" -f null - </dev/null 2>"$mjlog" || true
	frames=$(grep -oE 'frame= *[0-9]+' "$mjlog" | tail -1 | grep -oE '[0-9]+')
	if [ -n "${frames:-}" ] && [ "$frames" -gt 0 ]; then
		fps=$(awk -v f="$frames" -v d="$INTEG_DUR" 'BEGIN{printf "%.1f", f/d}')
		ok "MJPEG delivered $frames frames (~${fps} fps)"
	else bad "MJPEG produced no frames (see $mjlog)"; fi
fi

fi
if want 6 snapshot; then
# --- 6. Snapshot ------------------------------------------------------------
hdr "6. Snapshot (/snapshot.jpg) x$SNAP_COUNT"
for chn in 0 1; do
	# nojpeg detection, same reasoning as section 5: 404 "no jpeg" = this
	# channel has no JPEG source BY CONFIGURATION (camera-portrait-nojpeg.conf
	# class) - skip instead of burning $SNAP_COUNT requests into a FAIL that
	# blames a working build for its own config.
	pre=$(curl -s -o /dev/null -w '%{http_code}' --max-time 6 -u "$HTTP_USER:$HTTP_PASS" "$(http_base)/snapshot.jpg?chn=$chn")
	if [ "$pre" = "404" ]; then
		skip "chn$chn snapshots: no JPEG source on this config (404 'no jpeg') - deliberate nojpeg build/config"
		continue
	fi
	okc=0; badc=0; tsum=0; minb=99999999
	for i in $(seq 1 "$SNAP_COUNT"); do
		f="$OUTDIR/snap_${chn}.jpg"
		st=$(date +%s.%N)
		code=$(curl -s -o "$f" -w '%{http_code}' --max-time 8 -u "$HTTP_USER:$HTTP_PASS" "$(http_base)/snapshot.jpg?chn=$chn")
		en=$(date +%s.%N)
		dt=$(awk -v a="$st" -v b="$en" 'BEGIN{printf "%.3f",b-a}')
		tsum=$(awk -v s="$tsum" -v d="$dt" 'BEGIN{printf "%.3f",s+d}')
		sz=$(wc -c < "$f" 2>/dev/null || echo 0)
		# JPEG magic FFD8
		magic=$(head -c2 "$f" 2>/dev/null | od -An -tx1 | tr -d ' \n')
		if [ "$code" = "200" ] && [ "$magic" = "ffd8" ] && [ "$sz" -gt 1000 ]; then
			okc=$((okc+1)); [ "$sz" -lt "$minb" ] && minb=$sz
			# keep the FIRST good snapshot for the freshness check below;
			# every later one keeps overwriting snap_${chn}.jpg (so the last
			# survives as the second reference)
			[ -s "$OUTDIR/snap_${chn}_first.jpg" ] || cp "$f" "$OUTDIR/snap_${chn}_first.jpg" 2>/dev/null
		else badc=$((badc+1)); fi
	done
	avg=$(awk -v s="$tsum" -v n="$SNAP_COUNT" 'BEGIN{printf "%.3f",s/n}')
	if [ "$badc" -eq 0 ]; then ok "chn$chn snapshots ${okc}/${SNAP_COUNT} valid JPEG, avg ${avg}s, min ${minb}B"
	elif [ "$okc" -gt 0 ]; then warn "chn$chn snapshots ${okc} ok / ${badc} bad (avg ${avg}s)"
	else bad "chn$chn snapshots all $SNAP_COUNT failed"; fi
	# --- freshness: are these ACTUALLY new frames? -------------------------
	# HTTP 200 + valid JPEG + good latency all pass on an encoder that serves
	# the SAME buffered frame forever (the "silent limbo" state section 16
	# only catches via SSH logread). The server hands out stored JPEG bytes,
	# so a frozen pipeline yields BIT-IDENTICAL files (psnr_db prints 99 for
	# identical) - while on a live camera two frames tens of seconds apart are
	# never byte-identical (sensor noise alone, plus the OSD clock). Host-side
	# and free, since the first and last snapshot of the loop already exist.
	if [ "$okc" -ge 2 ] && [ -s "$OUTDIR/snap_${chn}_first.jpg" ] && [ -s "$OUTDIR/snap_${chn}.jpg" ] && have ffmpeg; then
		fr_psnr=$(psnr_db "$OUTDIR/snap_${chn}_first.jpg" "$OUTDIR/snap_${chn}.jpg")
		if [ -n "$fr_psnr" ] && fcmp "$fr_psnr" ge 99; then
			warn "chn$chn snapshots FROZEN: first and last snapshot of the run are byte-identical - the pipeline is serving one stale buffered frame, not live captures (silent-limbo signature; check logread for encoder-dead/PollingStream-idle)"
		else
			info "  chn$chn freshness: first vs last snapshot differ (PSNR ${fr_psnr:-?}dB) - frames are live"
		fi
	fi
done

fi
if want 7 audio; then
# --- 7. Audio continuity ----------------------------------------------------
hdr "7. Audio (codec + silence scan)"
aurl="$(rtsp_url "$PATH_MAIN")"
acodec=$(ffprobe -v error -rtsp_transport "$RTSP_TRANSPORT" -select_streams a:0 -show_entries stream=codec_name,sample_rate,channels -of csv=p=0 "$aurl" 2>/dev/null)
if [ -n "$acodec" ]; then
	info "audio stream: $acodec"
	# acodec csv = codec_name,sample_rate,channels
	ach=$(printf '%s' "$acodec" | awk -F, '{print $3}')
	if [ -n "$EXPECT_CHANNELS" ]; then
		if [ "${ach:-0}" = "$EXPECT_CHANNELS" ]; then
			ok "audio channels=$ach matches expected ($EXPECT_CHANNELS)"
		else
			bad "audio channels=${ach:-?} but expected $EXPECT_CHANNELS (channel config not active?)"
		fi
		# simulated stereo is dual-mono AAC only
		if [ "$EXPECT_CHANNELS" = "2" ]; then
			case "$acodec" in
				aac*) ok "stereo carried by AAC (dual-mono sim path)";;
				*)    warn "channels=2 but codec is '${acodec%%,*}' - simulated stereo is AAC-only";;
			esac
		fi
	else
		info "  (channels=${ach:-?}; pass --expect-channels N for a hard assertion)"
	fi
	sl="$OUTDIR/silence.log"
	timeout -k 5 "$((INTEG_DUR+5))" ffmpeg -hide_banner -nostdin -loglevel info -rtsp_transport "$RTSP_TRANSPORT" \
		-i "$aurl" -t "$INTEG_DUR" -map a:0 -af silencedetect=n=-45dB:d=1.5 -f null - </dev/null 2>"$sl" || true
	sil=$(grep -c silence_start "$sl" 2>/dev/null); sil=${sil:-0}
	[ "$sil" -eq 0 ] && ok "no long (>1.5s) silence gaps in ${INTEG_DUR}s audio" \
		|| warn "$sil silence gap(s) >1.5s detected (see $sl - may be real quiet, or dropouts)"
else warn "no audio stream on $PATH_MAIN (set the codec in the WebUI + restart timps)"; fi

fi
if want 8 control; then
# --- 8. /control API --------------------------------------------------------
hdr "8. /control API (status, caps, write round-trip)"
cj="$OUTDIR/control.json"
if curlq 10 "$(http_base)/control" -o "$cj" && [ -s "$cj" ]; then
	ok "/control returned status JSON ($(wc -c <"$cj") bytes)"
	for key in video audio caps; do
		grep -q "\"$key\"" "$cj" && info "  contains \"$key\" block" || warn "  \"$key\" block missing"
	done
	# POST transport probe ONLY - deliberately NOT called a "round-trip":
	# POST /control answers 200 UNCONDITIONALLY (httpd.c applies the body via
	# control_apply_json(), which has no error return, then always sends
	# 200 {"ok":true} - verified against the sim with garbage bodies), and
	# this writes the CURRENT value back anyway, so neither the status code
	# nor a re-read could prove anything about the apply path. The old
	# message ("write round-trip accepted") claimed exactly that proof and
	# could never fail except on transport loss. The real write round-trip
	# (distinct value, re-GET, verify, restore) is section 8b's job. Writing
	# the unchanged value is what makes this safe to leave stranded.
	# jget, not the old `grep '"brightness"...' | head -1`: the caps block
	# precedes the image block in the status JSON and lists "brightness" as a
	# bare capability NAME, so head -1 grabbed that valueless hit and the
	# probe silently skipped itself on every build with caps (seen on the sim
	# 2026-08-18; path-aware jget cannot take the wrong branch).
	bri=$(jget "$cj" image.brightness)
	if [ -n "${bri:-}" ]; then
		code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 8 -u "$HTTP_USER:$HTTP_PASS" \
			-X POST "$(http_base)/control" -d "{\"image\":{\"brightness\":$bri}}")
		[ "$code" = "200" ] && ok "/control accepts POST (HTTP 200; transport only - the daemon answers 200 unconditionally, so value application is proven in 8b, not here)" \
			|| warn "/control write returned HTTP $code"
	else info "  brightness not found; skipped the POST transport probe"; fi

	# --- self-reported health fields that nothing ever read ------------------
	# These exist specifically to make a class of silent failure visible from
	# the outside (control.c:799 / control.c:1249-1259), and until now no test
	# looked at either of them:
	#
	#   motion.stalled=1        detection is enabled but IVS has delivered no
	#                           result for a while and a recovery cycle ran -
	#                           motion looks on, but nothing flows through it.
	#   record.motion_gate_available=0 while record.mode=1
	#                           motion-gated recording is configured but the
	#                           gate isn't there - the recorder is structurally
	#                           inert, not merely "quiet because nothing moved".
	#                           Indistinguishable from healthy without this key.
	m_en=$(jget "$cj" motion.enabled); m_st=$(jget "$cj" motion.stalled)
	if [ "${m_en:-0}" = "1" ]; then
		if [ "${m_st:-0}" = "0" ]; then ok "motion: enabled and not stalled (motion.stalled=0)"
		else bad "motion.stalled=1 - detection is enabled but IVS stopped delivering results (a recovery cycle ran; see logread)"; fi
	else info "  motion detection disabled (motion.stalled not applicable)"; fi

	r_mode=$(jget "$cj" record.mode); r_en=$(jget "$cj" record.enabled)
	r_ga=$(jget "$cj" record.motion_gate_available); r_ge=$(jget "$cj" record.motion_gate_enabled)
	if [ "${r_mode:-0}" = "1" ]; then
		if [ "${r_ga:-0}" = "1" ]; then
			ok "record: motion-gated mode with the gate available (motion_gate_available=1, gate_enabled=${r_ge:-?})"
			[ "${r_ge:-0}" = "1" ] || warn "record.mode=1 (motion-gated) but motion_gate_enabled=0 - motion isn't running, so this recorder can never trigger"
		else
			bad "record.mode=1 (motion-gated) but record.motion_gate_available=0 - recording is structurally inert (looks configured, can never fire)"
		fi
	else info "  record.mode=${r_mode:-?} (not motion-gated; enabled=${r_en:-?}) - gate assertions not applicable"; fi
else bad "/control not reachable (auth? http.user/pass=$HTTP_USER) - see $cj"; fi

fi
if want 8b live livesettings; then
# --- 8b. Live settings apply + read-back verify -----------------------------
# For every LIVE-applicable /control setting: POST a changed (still-valid)
# value, GET /control back and assert the daemon reports the new value, then
# restore the original and confirm it reverts. This proves the whole path -
# JSON parse -> config_apply_kv -> hub_control (HAL live-apply) -> the value
# the daemon reports. Persist-only keys (video/sensor/osd.enabled) apply on
# restart, so they are checked separately as a config round-trip, not "live".
#
# What "applied" means here: the daemon's own reported state changes. For
# motion this is read from the live IVS status (real live-apply proof on HW);
# for the ISP image knobs the daemon reports g_cfg (accepted+persisted) - the
# physical sensor effect (e.g. a real hflip) is not observable host-side and
# is out of scope. Everything is posted in ONE request per section and
# restored in one, so a real camera sees just 2 config writes per section.
hdr "8b. Live settings apply + read-back verify"
if ! have python3; then
	skip "live-settings verify needs python3 (nested JSON read-back) - not found"
else
	LV_BASE="$OUTDIR/lv_base.json"
	if ! curlq 12 "$(http_base)/control" -o "$LV_BASE" || [ ! -s "$LV_BASE" ]; then
		bad "cannot GET /control baseline - skipping live-settings tests"
	else

	# --- runtime coverage ledger for section 8d ------------------------------
	# 8d used to attest coverage from its hand-written TESTED_* lists alone -
	# a one-way check (inventory contained in TESTED+ALLOW) that could never
	# notice when a field IN a TESTED list was silently skipped at runtime.
	# That is not hypothetical: video.rotation was attested "tested" in every
	# standard run although it is only POSTed under --test-rotation, and
	# spk_volume/spk_gain/aec were attested on builds whose caps gate skipped
	# them (both misattestations happened live 2026-08-18, twice). So 8b now
	# writes down, AT RUNTIME, what it really did:
	#   lv_posted.txt  "<section> <key>"          - actually POSTed + read back
	#   lv_gated.txt   "<section> <key> <reason>" - probe exists but a runtime
	#                                               gate (opt-in flag, caps)
	#                                               turned it off THIS run
	# and 8d judges against these files instead of trusting the hand lists.
	LV_POSTED="$OUTDIR/lv_posted.txt"; : > "$LV_POSTED"
	LV_GATED="$OUTDIR/lv_gated.txt";   : > "$LV_GATED"
	lv_mark()       { local s="$1"; shift; local k; for k in "$@"; do printf '%s %s\n' "$s" "$k" >> "$LV_POSTED"; done; }
	lv_mark_gated() { local s="$1" r="$2"; shift 2; local k; for k in "$@"; do printf '%s %s %s\n' "$s" "$k" "$r" >> "$LV_GATED"; done; }

	# POST a JSON body, echo the HTTP status
	lv_post() { curl -s -o /dev/null -w '%{http_code}' --max-time 12 \
		-u "$HTTP_USER:$HTTP_PASS" -X POST "$(http_base)/control" -d "$1"; }
	# Same POST, but KEEP the reply body ($2). lv_post throws it away, which was
	# fine while a POST reply could only say accepted/changed/rejected. Since
	# 22832f1 the body also carries "deferred"/"deferred_keys" (httpd.c) - the
	# only place the daemon states, per REQUEST, whether a changed video/sensor
	# key reached the running pipeline or is waiting for a restart. The rc
	# checks below are built on exactly that statement, so they need the body.
	lv_post_r() { curl -s -o "$2" -w '%{http_code}' --max-time 12 \
		-u "$HTTP_USER:$HTTP_PASS" -X POST "$(http_base)/control" -d "$1"; }
	lv_get()  { curlq 12 "$(http_base)/control" -o "$1"; }
	# Interruption safety: /control POSTs PERSIST to the camera's flash config.
	# A run killed between POST(new) and POST(restore) used to strand the test
	# values on a live camera (seen 2026-08-02: cam-K left with manual WB
	# rgain/bgain=32767 -> full magenta image, surviving reboots). Track the
	# pending restore body and flush it from EXIT/INT/TERM traps. kill -9 or a
	# network drop still defeats this, hence also the safe-if-stranded test
	# values below.
	LV_PENDING=""
	lv_restore_pending() {
		[ -n "${LV_PENDING:-}" ] || return 0
		warn "interrupted mid live-settings test - restoring camera settings"
		lv_post "$LV_PENDING" >/dev/null 2>&1 || true
		LV_PENDING=""
	}
	trap 'lv_restore_pending' EXIT
	trap 'lv_restore_pending; trap - INT;  kill -INT  $$' INT
	trap 'lv_restore_pending; trap - TERM; kill -TERM $$' TERM
	# pick a valid value != cur within [lo,hi]
	flip_int()  { awk -v lo="$1" -v hi="$2" -v c="$3" 'BEGIN{
		m=int((lo+hi)/2); if(m!=c){print m} else if(m<hi){print m+1} else{print m-1}}'; }
	flip_bool() { [ "${1:-0}" = "1" ] && echo 0 || echo 1; }
	# T_FLT fields (e.g. daynight.day_gain/night_gain) need their own flip:
	# flip_int's int() would collapse a fractional range onto a single value
	# and the read-back compare would then pass without anything having
	# changed. (The example used to be daynight.ir_ratio_*, which the
	# 2026-08-22 consolidation turned into a fixed constant.)
	flip_flt()  { awk -v lo="$1" -v hi="$2" -v c="$3" 'BEGIN{
		m=(lo+hi)/2; m=int(m*10+0.5)/10;
		if(m!=c+0){printf "%g", m} else {printf "%g", (m+0.1<=hi)?m+0.1:m-0.1}}'; }

	# lv_section <label> <wrap-open> <wrap-close> <read-prefix> <spec...>
	#   spec = "key type [lo hi]"   type: int|flt|bool|hex|str
	# Reads each key's current value from $LV_BASE, computes a distinct valid
	# new value, POSTs them all at once, verifies the read-back, then restores.
	lv_section() {
		local label="$1" wo="$2" wc="$3" rp="$4"; shift 4
		local body="" rbody="" spec key type lo hi cur new q
		local -a P N O
		for spec in "$@"; do
			# shellcheck disable=SC2086
			set -- $spec; key="$1"; type="$2"; lo="${3:-0}"; hi="${4:-1}"
			cur=$(jget "$LV_BASE" "$rp.$key")
			if [ -z "$cur" ] && [ "$type" != str ]; then
				info "  $label.$key not present - skipped"; continue; fi
			q=0
			case "$type" in
				int)  new=$(flip_int "$lo" "$hi" "$cur");;
				flt)  new=$(flip_flt "$lo" "$hi" "$cur");;
				bool) new=$(flip_bool "$cur");;
				hex)  new="0x40C08000"; [ "$cur" = "$new" ] && new="0x80C04000"; q=1;;
				str)  new="qa_probe";  [ "$cur" = "$new" ] && new="qa_probe2"; q=1;;
				*) continue;;
			esac
			if [ "$q" = 1 ]; then
				body="$body${body:+,}\"$key\":\"$new\""
				rbody="$rbody${rbody:+,}\"$key\":\"$cur\""
			else
				body="$body${body:+,}\"$key\":$new"
				rbody="$rbody${rbody:+,}\"$key\":$cur"
			fi
			P+=("$rp.$key"); N+=("$new"); O+=("$cur")
		done
		local n=${#P[@]}
		[ "$n" -gt 0 ] || { skip "$label: no testable keys"; return; }
		local code gf rf i got pass=0
		LV_PENDING="${wo}{$rbody}${wc}"     # armed until restore lands
		code=$(lv_post "${wo}{$body}${wc}")
		if [ "$code" != "200" ]; then
			bad "$label: POST(new) HTTP $code"
			# a non-200 can still be a partial apply - restore, then disarm
			lv_post "${wo}{$rbody}${wc}" >/dev/null; LV_PENDING=""
			return
		fi
		gf="$OUTDIR/lv_${label}_new.json"; lv_get "$gf"
		# 8d-ledger section name: strip the item indices ("osd0.0" is the
		# per-item OSD leaf set the inventory calls "osd_item"; "privacy.0.0"
		# is "privacy") so the ledger speaks the inventory's language
		local lsec
		case "$rp" in
			osd0.*|osd1.*) lsec=osd_item;;
			privacy.*)     lsec=privacy;;
			video.*)       lsec=video;;     # per-channel leaf set; the inventory calls it "video"
			*)             lsec="$rp";;
		esac
		for ((i=0;i<n;i++)); do
			got=$(jget "$gf" "${P[i]}")
			# every key that reached this loop WAS POSTed (HTTP 200) and read
			# back - record it for 8d whether or not the compare matches (a
			# mismatch already FAILed loudly right here; the ledger answers
			# "was it exercised", not "did it pass")
			lv_mark "$lsec" "${P[i]##*.}"
			if [ "$got" = "${N[i]}" ]; then pass=$((pass+1))
			elif [ "${LV_MODE:-live}" = persist ]; then
				bad "$label: ${P[i]##*.} did not persist (got '$got', want '${N[i]}')"
			else bad "$label: ${P[i]##*.} not applied (got '$got', want '${N[i]}')"; fi
		done
		if [ "$pass" = "$n" ]; then
			if [ "${LV_MODE:-live}" = persist ]; then
				# Finding #1: mechanically identical POST->GET->restore check as
				# the live path above - a persist-only (restart-required) key's
				# config value still updates immediately (GET/config_get_kv reads
				# g_cfg, not the running pipeline), only the physical effect
				# waits for a restart. Same contract as the video0.bitrate/
				# audio.codec spot-checks further down, generalized here so
				# Finding #1's newly-covered fields don't need their own copies
				# of this block.
				ok "persist-only $label: all $n key(s) round-trip through config, applies on restart (${P[*]##*.})"
			else
				ok "$label: all $n live key(s) applied & read back (${P[*]##*.})"
			fi
		fi
		# restore originals and confirm they revert
		code=$(lv_post "${wo}{$rbody}${wc}")
		LV_PENDING=""                       # restore POSTed - disarm the trap
		rf="$OUTDIR/lv_${label}_restore.json"; lv_get "$rf"
		local rok=0
		for ((i=0;i<n;i++)); do [ "$(jget "$rf" "${P[i]}")" = "${O[i]}" ] && rok=$((rok+1)); done
		if [ "$rok" = "$n" ]; then
			info "  $label: restored $n/$n to original (HTTP $code)"
		elif [ "${LV_RESTORE_SOFT:-0}" = 1 ]; then
			# Some keys are read from LIVE subsystem state (not g_cfg) and become
			# unobservable once that subsystem is turned back off on restore - the
			# config IS restored (POST $code), the read-back source just goes stale.
			info "  $label: config restored (HTTP $code); $((n-rok)) key(s) read from live state, unobservable once disabled"
		else
			warn "$label: restored only $rok/$n keys to original"
		fi
	}

	# --- image: every ISP knob (accepted + persisted on any SoC; live where the
	# HAL supports it). uchar knobs 0..255, highlight/backlight 0..10,
	# hflip/vflip/running_mode bool, anti_flicker 0..2, core_wb 0..1.
	# WB gains: the config accepts 0..65535 but the probe range is deliberately
	# 0..2048 so flip_int's midpoint lands on 1024 = unity gain on Ingenic ISPs.
	# core_wb_mode DOES flip to 1 (manual) during the test, so if a run dies
	# with the trap defeated (kill -9, network drop) the stranded state is
	# manual WB at ~neutral gains - a usable image - instead of the magenta
	# rgain/bgain=32767 that hit cam-K on 2026-08-02. ---
	lv_section image '{"image":' '}' image \
		"brightness int 0 255" "contrast int 0 255" "saturation int 0 255" \
		"sharpness int 0 255" "hue int 0 255" "ae_compensation int 0 255" \
		"max_again int 0 255" "max_dgain int 0 255" "sinter_strength int 0 255" \
		"temper_strength int 0 255" "dpc_strength int 0 255" "defog_strength int 0 255" \
		"drc_strength int 0 255" "highlight_depress int 0 10" \
		"backlight_compensation int 0 10" "wb_rgain int 0 2048" "wb_bgain int 0 2048" \
		"hflip bool" "vflip bool" "running_mode bool" "anti_flicker int 0 2" \
		"core_wb_mode int 0 1"

	# --- audio: only the keys the HAL applies LIVE (caps.audio). volume is
	# 0..100; gain is clamped server-side to the IMP mic PGA range 0..31
	# (F-03); alc_gain to the PGA 0..7; mute is the live publish gate. The
	# restart-only audio keys (codec/samplerate/agc/ns/...) are covered by
	# the persist check ---
	lv_section audio '{"audio":' '}' audio \
		"volume int 0 100" "gain int 0 31" "alc_gain int 0 7" "mute bool"

	# --- speaker (AO) live keys (F-08): spk_volume/spk_gain/aec are live only on
	# USE_PLAY / USE_BACKCHANNEL builds, where caps.audio lists them (they own the
	# IMP_AO device). Gate on caps.audio so a build without an audio-output
	# pipeline skips cleanly, matching how the rest of the script gates. spk_*
	# are 0..100, aec is a bool; all round-trip through config.
	# Caps come from $LV_BASE, NOT from section 8's $cj: $cj only exists when
	# section 8 ran, so `--only 8b` hit an unbound variable under set -u, the
	# expansion came up empty, and the fallback below then asserted "no AO
	# pipeline in this build" about a build it had never looked at (reproduced
	# against the sim 2026-08-18) - on a real AO build that silently skipped
	# spk_*/aec while 8d still attested them. $LV_BASE is fetched
	# unconditionally at the top of 8b, so it always exists here. ---
	case "$(jget "$LV_BASE" caps.audio)" in
		*spk_volume*)
			lv_section audio_spk '{"audio":' '}' audio \
				"spk_volume int 0 100" "spk_gain int 0 100" "aec bool"
			;;
		*)
			info "audio spk_*/aec: caps.audio has no spk_volume (no AO pipeline in this build) - skipping"
			# known-skipped, with the reason - 8d reports these as gated
			# rather than attesting them tested (the A1 misattestation)
			lv_mark_gated audio "caps.audio-has-no-spk_volume(no-AO-pipeline)" spk_volume spk_gain aec
			;;
	esac

	# --- audio (Finding #1, field-inventory audit): the RESTART-REQUIRED /
	# persist-only audio.* keys - hub.c's audio branch only applies these at
	# the next AI/AO pipeline init (see the audio_fields[] doc comment in
	# config.c: "restart-required or persist-only"), so they round-trip
	# through config exactly like video0.bitrate/audio.codec further down,
	# just not live. codec itself is already covered by the dedicated
	# opt-in-aware opus check below - not repeated here. Previously entirely
	# absent from 8b's coverage despite all being F_CTRL (confirmed missing
	# by diffing against GET /control?fields=1, section 8d below). ---
	LV_MODE=persist
	lv_section audio_persist '{"audio":' '}' audio \
		"enabled bool" "samplerate int 8000 96000" "channels int 1 2" \
		"bitrate int 8 320" "high_pass bool" "agc bool" "ns int 0 3" \
		"agc_target_dbfs int 0 31" "agc_compression_db int 0 90" \
		"force_stereo bool" "spk_enabled bool" "backchannel bool" \
		"backchannel_codec int 0 2" "backchannel_rate int 8000 48000"
	LV_MODE=live

	# --- osd stream 0 item 0: the live text-overlay leaf keys (caps.osd).
	# font_size clamps 8..256, transparency 0..255, colors are 0xAARRGGBB hex ---
	lv_section osd0.0 '{"osd0":{"0":' '}}' osd0.0 \
		"text str" "x int 0 200" "y int 0 200" "font_size int 8 256" \
		"transparency int 0 255" "outline int 0 4" "color hex" "outline_color hex"

	# --- osd.* globals (Finding #1): monitor_stream/font_path/vars_file are
	# LIVE - control.c's GET /control comment says so explicitly ("osd.font_path/
	# vars_file are runtime-mutable via POST"), and per the caps-builder doc
	# comment above control_get_json(), "'osd.enabled' ... lives in the osd.*
	# section whose OTHER keys are live, but itself only takes effect on
	# restart" - i.e. enabled is the one documented exception, not the rule.
	# font_path/vars_file briefly point at a nonexistent "qa_probe" path during
	# the test, same accepted risk as record.dir/timelapse.dir below. ---
	lv_section osd '{"osd":' '}' osd \
		"monitor_stream int 0 1" "font_path str" "vars_file str"

	# --- osd.* globals, restart-required half: enabled is the documented
	# exception above; supersample/hinting are explicitly commented in
	# config.h as "File-only, takes effect on restart" (imp_osd_setup() only
	# builds groups / configures the TTF rasterizer once at startup) despite
	# being F_CTRL (POST-able + persisted). All three were entirely missing
	# from 8b's coverage before this fix. ---
	LV_MODE=persist
	lv_section osd_persist '{"osd":' '}' osd \
		"enabled bool" "supersample int 1 4" "hinting bool"
	LV_MODE=live

	# --- privacy cover mask stream 0 region 0: applied live when an OSD group
	# exists (else persisted). enabled bool, geometry px, color hex ---
	lv_section privacy.0.0 '{"privacy":{"0":{"0":' '}}}' privacy.0.0 \
		"enabled bool" "x int 0 200" "y int 0 200" "w int 1 200" "h int 1 200" "color hex"

	# --- motion: all applied live (the HAL recreates the IVS grid). sensitivity
	# 0..255, monitor_stream 0..1, enabled bool. hold_ms/skip_frames (Finding
	# #1: previously missing) are read straight from g_cfg by
	# control_motion_json() (not the live IVS "st" struct), so they round-trip
	# immediately like any other live key even though their EFFECT only lands
	# at the grid's next create/resync. cols/rows are omitted: they are
	# clamped to the SDK cell budget, which would look like a mismatch here.
	# LV_RESTORE_SOFT: on hardware motion.sensitivity is reported from the live
	# IVS status, not g_cfg - after restoring motion to disabled the grid is torn
	# down and that read-back goes stale (config is still correctly restored), so
	# a partial restore here is expected and reported as info, not a warning ---
	LV_RESTORE_SOFT=1
	lv_section motion '{"motion":' '}' motion \
		"sensitivity int 0 255" "monitor_stream int 0 1" "enabled bool" \
		"hold_ms int 0 5000" "skip_frames int 1 30"
	LV_RESTORE_SOFT=0

	# --- daynight: the detection thread reads these live from config. Test the
	# thresholds + tunables that are still per-camera settable; "enabled"
	# reflects the thread's own state (poll lag) so it is left out.
	# day_gain/night_gain are T_FLT in config.c and are probed as such.
	#
	# NOT here any more: probe_jump_pct, probe_settle_s, ref_delay_s,
	# ir_ratio_night, ir_ratio_day, ir_min_headroom and boot_settle_s became
	# fixed DN_* constants on 2026-08-22 (daynight.h), and daynight.learn went
	# away with the learning subsystem. A round-trip probe on any of them is
	# not a weaker test, it is a FAILING one: POSTed alone the key is a 422,
	# POSTed alongside valid keys (which is what lv_section does) the request
	# is a 200 and the key is silently dropped - so the read-back returns the
	# constant and lv_section reports "not applied". They are asserted
	# read-only immediately below instead, which is all there is left to
	# assert about them. ---
	lv_section daynight '{"daynight":' '}' daynight \
		"day_gain flt 100 900" \
		"night_gain flt 2000 8000" \
		"day_confirm_s int 1 120" "boot_probe int 0 1" \
		"probe_min_gap_s int 60 3600" "probe_confirm_s int 1 120" \
		"heartbeat_s int 300 86400" "heartbeat_max_s int 300 604800" \
		"sun_sunrise_offset_min int -1440 1440" \
		"sun_sunset_offset_min int -1440 1440"

	# --- daynight fixed constants (2026-08-22 consolidation): read-only, and
	# GET /control must report the compiled DN_* value for each. control.c
	# prints these straight from the daynight.h constants, so this catches the
	# one way they can still go wrong - a value edited in the header without
	# the status/config-warning side following it, or the reverse. Floats come
	# through "%g", hence "2" and not "2.0". ---
	dn_const() {   # <key> <expected>
		dnc_n=$((dnc_n+1))
		local got; got=$(jget "$LV_BASE" "daynight.$1")
		if [ "$got" = "$2" ]; then dnc_ok=$((dnc_ok+1))
		else bad "daynight.$1: status reports '$got', want the fixed constant '$2' (DN_* in daynight.h vs control.c drift, or the key vanished from the status object)"; fi
	}
	dnc_ok=0; dnc_n=0
	dn_const probe_jump_pct  50
	dn_const probe_settle_s  8
	dn_const ref_delay_s     30
	dn_const ir_ratio_night  2
	dn_const ir_ratio_day    2
	dn_const ir_min_headroom 8
	dn_const boot_settle_s   5
	dn_const transition_s    5
	[ "$dnc_ok" = "$dnc_n" ] && ok "daynight fixed constants: all $dnc_n report their compiled DN_* value (50/8/30/2/2/8/5/5) - not per-camera settable since 2026-08-22"

	# --- daynight TIME/SUN path (F-08): the mode enum + the string/float keys the
	# time/sun modes use. None fit the generic lv_section round-trip - "mode" is
	# POSTed as a string but echoes back as "dn_mode"; time_*_start are <=5-char
	# HH:MM strings (the generic "qa_probe" probe would truncate them); sun_lat/
	# long are floats. Probe them explicitly, verify each read-back key, restore.
	# Keys are always present (control_daynight_json emits them regardless of the
	# USE_DAYNIGHT build), but guard on dn_mode anyway for safety. ---
	dn_mode_cur=$(jget "$LV_BASE" daynight.dn_mode)
	if [ -n "$dn_mode_cur" ]; then
		tns_cur=$(jget "$LV_BASE" daynight.time_night_start)
		tds_cur=$(jget "$LV_BASE" daynight.time_day_start)
		lat_cur=$(jget "$LV_BASE" daynight.sun_latitude)
		lon_cur=$(jget "$LV_BASE" daynight.sun_longitude)
		dn_restore="{\"daynight\":{\"mode\":\"$dn_mode_cur\",\"time_night_start\":\"$tns_cur\",\"time_day_start\":\"$tds_cur\",\"sun_latitude\":${lat_cur:-0},\"sun_longitude\":${lon_cur:-0}}}"
		LV_PENDING="$dn_restore"        # armed until restore lands
		code=$(lv_post '{"daynight":{"mode":"sun","time_night_start":"19:30","time_day_start":"06:30","sun_latitude":52.5,"sun_longitude":13.5}}')
		gf="$OUTDIR/lv_daynight_timesun.json"; lv_get "$gf"
		dnp=0
		# The redesign collapsed sensor/time/sun into auto/schedule and accepts
		# the legacy names on input, normalising them. POSTing "sun" must therefore
		# read back as "schedule" - asserting that pins the compat mapping itself,
		# which is the part that could silently rot.
		[ "$(jget "$gf" daynight.dn_mode)" = "schedule" ]         && dnp=$((dnp+1)) || bad "daynight.mode not applied (dn_mode='$(jget "$gf" daynight.dn_mode)', want 'schedule' - legacy 'sun' must normalise to it)"
		[ "$(jget "$gf" daynight.time_night_start)" = "19:30" ]  && dnp=$((dnp+1)) || bad "daynight.time_night_start not applied (got '$(jget "$gf" daynight.time_night_start)')"
		[ "$(jget "$gf" daynight.time_day_start)" = "06:30" ]    && dnp=$((dnp+1)) || bad "daynight.time_day_start not applied (got '$(jget "$gf" daynight.time_day_start)')"
		[ "$(jget "$gf" daynight.sun_latitude)" = "52.5" ]       && dnp=$((dnp+1)) || bad "daynight.sun_latitude not applied (got '$(jget "$gf" daynight.sun_latitude)')"
		[ "$(jget "$gf" daynight.sun_longitude)" = "13.5" ]      && dnp=$((dnp+1)) || bad "daynight.sun_longitude not applied (got '$(jget "$gf" daynight.sun_longitude)')"
		[ "$dnp" = 5 ] && ok "daynight TIME/SUN: mode+time_night_start+time_day_start+sun_latitude+sun_longitude applied & read back (HTTP $code)"
		# ledger: the four F_CTRL keys this block just POSTed + read back
		# ("mode" is deliberately not F_CTRL - hand-validated in control.c -
		# so it never appears in the inventory 8d diffs against)
		lv_mark daynight time_night_start time_day_start sun_latitude sun_longitude
		lv_post "$dn_restore" >/dev/null; LV_PENDING=""
		rf="$OUTDIR/lv_daynight_timesun_restore.json"; lv_get "$rf"
		[ "$(jget "$rf" daynight.dn_mode)" = "$dn_mode_cur" ] \
			&& info "  daynight TIME/SUN: restored mode to $dn_mode_cur" \
			|| warn "daynight TIME/SUN: mode did not restore to $dn_mode_cur"
	fi

	# --- 8h. Deterministic day/night TRANSITION (opt-in: --test-daynight) ----
	# The block above proves the time/sun KEYS round-trip. It never makes the
	# camera actually switch, so the whole transition path - decision -> hook
	# script -> ISP running_mode re-assert -> stability afterwards - has never
	# been executed by this harness. That path is where the real bugs were:
	# b3eec71/f8a7b21/bd21ce6/fad4f40/10a192a, and above all 0f5fc80, an
	# overnight FLAP LOOP where the camera oscillated between day and night for
	# hours. A flap is invisible to any single-sample check; the only way to
	# see it is to force one deliberate switch in each direction and then count
	# how many switches actually happened.
	#
	# Invasive but not destructive: the board hook physically drives the IR-cut
	# filter and IR illuminator, so this test makes the camera click twice.
	if [ "${TEST_DAYNIGHT:-0}" = "1" ]; then
		dn_en=$(jget "$LV_BASE" daynight.enabled)
		if [ "${dn_en:-0}" != "1" ]; then
			skip "day/night transition test: daynight.enabled=0 on this camera (the detection thread is not running, nothing would ever switch)"
		else
			dn_mode_cur2=$(jget "$LV_BASE" daynight.dn_mode)
			tns_cur2=$(jget "$LV_BASE" daynight.time_night_start)
			tds_cur2=$(jget "$LV_BASE" daynight.time_day_start)
			# ALWAYS restore the window keys, including back to empty. The
			# "posting "" is rejected server-side" note this block used to
			# carry is not true of the current daemon: time_night_start /
			# time_day_start ride the generic apply_ctrl_fields walker
			# (control.c), which accepts an empty string and clears the field
			# (verified 2026-08-19: POST {"time_night_start":"",
			# "time_day_start":""} -> accepted 2, changed 2, read-back "").
			# Skipping the restore is what actually stranded the TEST windows
			# on the camera (observed 2026-08-19 on the QA camera: mode
			# restored to auto but the thread banner then read
			# calendar="time window" 14:07..12:05, which in auto mode silently
			# pulls the heartbeat in to a fabricated dawn).
			# transition_s used to be forced to 2 here (and restored) so the
			# mode-switch dwell would not eat into the wait bounds below. It
			# became the fixed DN_TRANSITION_S=5 on 2026-08-22: the POST is
			# now silently dropped (mixed body -> HTTP 200, value unchanged)
			# and the restore compared 5 against 5, so the block read green
			# while asserting nothing. Dropped on both sides. The real dwell
			# is 5 s and the sampling interval 2 s, i.e. at most ~7 s from a
			# window POST to the decision - an order of magnitude inside the
			# 60 s dn_wait bounds used below, so no bound needs widening.
			dn_win_restore="\"time_night_start\":\"$tns_cur2\",\"time_day_start\":\"$tds_cur2\""
			dn_full_restore="{\"daynight\":{\"mode\":\"$dn_mode_cur2\",${dn_win_restore}}}"
			LV_PENDING="$dn_full_restore"

			# Windows are evaluated against the CAMERA's clock, which is not
			# necessarily this host's - prefer the camera's own time whenever
			# SSH is available, and say so when falling back.
			if [ -n "$SSH_TARGET" ]; then dn_now=$(sshx "date +%H:%M" 2>/dev/null); fi
			if [ -z "${dn_now:-}" ]; then
				dn_now=$(date +%H:%M)
				info "  day/night: using THIS HOST's clock ($dn_now) for the test windows - with --ssh the camera's own clock would be used"
			else
				info "  day/night: camera clock is $dn_now"
			fi
			# helper: HH:MM +/- minutes, wrapping at midnight
			dn_shift() { awk -v t="$1" -v d="$2" 'BEGIN{
				split(t,a,":"); m=a[1]*60+a[2]+d; m=(m%1440+1440)%1440;
				printf "%02d:%02d", int(m/60), m%60 }'; }
			# poll GET /control until daynight.mode (0=day, 1=night; the live
			# state, distinct from dn_mode which is the auto/time/sun selector)
			# reaches the wanted value. Bounded, never an unbounded wait.
			dn_wait() {   # $1=wanted 0|1  $2=bound_s -> echoes seconds taken, or empty
				local want="$1" bound="$2" i gf got
				gf="$OUTDIR/lv_dn_poll.json"
				for i in $(seq 1 "$((bound/2))"); do
					lv_get "$gf"; got=$(jget "$gf" daynight.mode)
					[ "${got:-}" = "$want" ] && { echo $((i*2)); return 0; }
					sleep 2
				done
				return 1
			}
			dn_hook_count() {   # $1=day|night -> how many times the hook has fired so far
				[ -n "$SSH_TARGET" ] || return 1
				sshx "logread 2>/dev/null | grep -c 'switching to $1'" 2>/dev/null
			}
			# poll GET /control until image.running_mode reaches the wanted
			# value. The board hook chain (timps forks switch_cmd -> the script
			# BACKGROUNDS 'color ... &' -> color curls POST /control) is
			# asynchronous and fire-and-forget: ~0.3-1.5s on an idle board
			# (measured on real T20+T31, 2026-08-11), more under QA load, up
			# to curl's own 5s timeout legitimately. A single-shot read right
			# after daynight.mode flips is therefore a coin-flip race, not a
			# verdict - it produced a false "ISP night mode did not follow"
			# FAIL on a camera whose chain works (a dim outbuilding 2026-08-11).
			dn_wait_rm() {   # $1=wanted 0|1  $2=bound_s -> echoes seconds taken
				local want="$1" bound="$2" i gf got
				gf="$OUTDIR/lv_dn_rm_poll.json"
				for i in $(seq 1 "$bound"); do
					lv_get "$gf"; got=$(jget "$gf" image.running_mode)
					[ "${got:-}" = "$want" ] && { echo "$i"; return 0; }
					sleep 1
				done
				return 1
			}
			# --- precondition: a DAY starting state --------------------------
			# cam-K 2026-08-11: the camera sat in genuine night (dark room) when
			# this test began, so "force NIGHT" below confirmed an already-
			# night camera (vacuously - no transition, no hook run, no
			# running_mode edge) and the hook-count check then FAILed a
			# perfectly working chain. Both direction verdicts below are only
			# meaningful as REAL edges: if the camera is in night now, drive
			# it to day first (one extra IR-cut click - this test is
			# documented invasive). Read the LIVE state, not LV_BASE: earlier
			# 8x subsections may have moved it since.
			dn_pre_j="$OUTDIR/lv_dn_pre.json"; lv_get "$dn_pre_j"
			if [ "$(jget "$dn_pre_j" daynight.mode)" = "1" ]; then
				dn_ns0=$(dn_shift "$dn_now" 120); dn_ds0=$(dn_shift "$dn_now" -2)
				lv_post "{\"daynight\":{\"mode\":\"time\",\"time_night_start\":\"$dn_ns0\",\"time_day_start\":\"$dn_ds0\"}}" >/dev/null
				if dn_wait 0 60 >/dev/null; then
					dn_wait_rm 0 10 >/dev/null || true
					info "  day/night: camera started in NIGHT - forced DAY first so both transitions below are fresh edges"
				else
					warn "day/night: camera started in night and did not reach day within 60s - the transition verdicts below may be vacuous"
				fi
			fi
			# hook-count baselines AFTER the precondition switch, so a
			# precondition day-click is never miscounted as the test's own
			dn_night0=$(dn_hook_count night); dn_day0=$(dn_hook_count day)
			# snapshot for the orientation cross-check further down
			if [ "${TEST_FLIP:-0}" = "1" ]; then
				mkdir -p "$OUTDIR/flip"
				curl -s --max-time 12 -u "$HTTP_USER:$HTTP_PASS" "$(http_base)/snapshot.jpg?chn=0" -o "$OUTDIR/flip/dn_before.jpg" 2>/dev/null
			fi

			# --- force NIGHT: a window that contains "now" ------------------
			dn_ns=$(dn_shift "$dn_now" -2); dn_ds=$(dn_shift "$dn_now" 120)
			code=$(lv_post "{\"daynight\":{\"mode\":\"time\",\"time_night_start\":\"$dn_ns\",\"time_day_start\":\"$dn_ds\"}}")
			info "  day/night: night window $dn_ns -> $dn_ds (now=$dn_now is inside it), dwell DN_TRANSITION_S=5s, HTTP $code"
			if t_night=$(dn_wait 1 60); then
				ok "day/night: camera switched to NIGHT within ${t_night}s of the forced time window (daynight.mode=1)"
				dn_rm_edge=0
				if t_rm=$(dn_wait_rm 1 10); then
					dn_rm_edge=1
					ok "day/night: image.running_mode followed the switch to night (=1 within ${t_rm}s - the async board hook chain landed)"
				else
					bad "day/night: switched to night but image.running_mode still 0 a full 10s after the decision (hook latency is ~1s) - the board hook chain never set it. NOTE timps by DESIGN never writes running_mode itself (daynight.h): switch_cmd -> 'color' script -> POST /control does. Check on the camera: the switch_cmd script, thingino.json daynight.controls.color, and /usr/sbin/color's curl target"
				fi
				rm_j="$OUTDIR/lv_dn_night.json"; lv_get "$rm_j"
			else
				bad "day/night: camera did NOT switch to night within 60s of a time window containing the current time (daynight.mode stayed $(jget "$OUTDIR/lv_dn_poll.json" daynight.mode)) - the time-mode decision path is not working"
			fi

			# --- force DAY: invert the window --------------------------------
			dn_ns2=$(dn_shift "$dn_now" 120); dn_ds2=$(dn_shift "$dn_now" -2)
			code=$(lv_post "{\"daynight\":{\"mode\":\"time\",\"time_night_start\":\"$dn_ns2\",\"time_day_start\":\"$dn_ds2\"}}")
			info "  day/night: day window (night $dn_ns2 -> day $dn_ds2), HTTP $code"
			if t_day=$(dn_wait 0 60); then
				ok "day/night: camera switched back to DAY within ${t_day}s (daynight.mode=0)"
				if t_rm2=$(dn_wait_rm 0 10); then
					if [ "${dn_rm_edge:-0}" = "1" ]; then
						ok "day/night: image.running_mode followed the switch back to day (=0 within ${t_rm2}s)"
					else
						# running_mode never reached 1 in the night phase, so
						# 0 here is not a real 1->0 edge - don't count a PASS
						# on the back of the night-direction failure
						info "  day/night: image.running_mode=0 after the day switch, but it never reached 1 above - not a real edge, no verdict"
					fi
				else
					bad "day/night: switched to day but image.running_mode still 1 a full 10s after the decision - the board hook chain never set it (see the night-direction note)"
				fi
				rm_j2="$OUTDIR/lv_dn_day.json"; lv_get "$rm_j2"
			else
				bad "day/night: camera did NOT switch back to day within 60s of an inverted time window"
			fi

			# --- hook fired? and exactly ONCE per direction? ------------------
			dn_night1=$(dn_hook_count night); dn_day1=$(dn_hook_count day)
			# require the BASELINES too: a failed baseline ssh (empty ->
			# treated as 0) would turn any stale historical 'switching to'
			# line in logread into a phantom invocation delta
			if [ -z "${dn_night1:-}" ] || [ -z "${dn_night0:-}" ] || [ -z "${dn_day0:-}" ]; then
				info "  day/night: hook-invocation check needs --ssh with working logread on both samples - skipped"
			else
				n_sw=$(( ${dn_night1:-0} - ${dn_night0:-0} )); d_sw=$(( ${dn_day1:-0} - ${dn_day0:-0} ))
				# A NEGATIVE delta cannot mean "the hook did not run" - it means
				# the baseline lines scrolled out of the syslog ring between the
				# two samples (busybox syslogd -C64 = 64 KB, ~6 min of QA-load
				# logging on the fleet's cameras), so the counts are not
				# comparable at all. Reporting that as "no hook invocation" is
				# how a working chain gets failed; say what actually happened.
				if [ "$n_sw" -lt 0 ] || [ "$d_sw" -lt 0 ]; then
					warn "day/night: the syslog ring buffer wrapped between the hook-count samples (night ${dn_night0}->${dn_night1}, day ${dn_day0}->${dn_day1}) - hook-invocation and flap counts are not usable for this run; raise syslogd's -C size to re-enable them"
				else
				[ "$n_sw" -ge 1 ] && ok "day/night: the board hook actually ran for the night switch ($n_sw invocation(s) logged)" \
					|| bad "day/night: no 'switching to night' hook invocation in logread - the decision changed state but the board script (IR-cut/illuminator) was never run"
				[ "$d_sw" -ge 1 ] && ok "day/night: the board hook actually ran for the day switch ($d_sw invocation(s) logged)" \
					|| bad "day/night: no 'switching to day' hook invocation in logread"
				# The flap regression (0f5fc80): more than one switch per
				# direction for two deliberate, unambiguous window changes means
				# the state machine is oscillating. EXACTLY one - a 0 delta is
				# the hook-invocation failure above, and calling that "no
				# flapping" in the same breath is a contradiction the reader
				# has to untangle (it printed both on the 2026-08-11 runs).
				if [ "$n_sw" -eq 1 ] && [ "$d_sw" -eq 1 ]; then
					ok "day/night: exactly one switch per direction - no flapping"
				elif [ "$n_sw" -le 1 ] && [ "$d_sw" -le 1 ]; then
					info "  day/night: flap count not assessable (${n_sw} night / ${d_sw} day switches - see the hook-invocation failure above)"
				else
					bad "day/night: FLAPPING - ${n_sw} night and ${d_sw} day switches for two deliberate window changes (expected 1 each). This is the 0f5fc80 overnight flap-loop signature"
				fi
				fi
			fi

			# --- did the transition disturb the image orientation? -----------
			# 8fb6fd3 fixed flip loss on a chn0 relatch and noted, untested,
			# that a day/night switch might reset flip by a different route.
			# Compare orientation only (both snapshots are equally affected by
			# the IR-cut/lighting change, so the RELATIVE comparison survives
			# it) and keep it a WARN - a night-mode image is a hostile subject
			# for pixel comparison.
			if [ "${TEST_FLIP:-0}" = "1" ] && [ -s "$OUTDIR/flip/dn_before.jpg" ]; then
				curl -s --max-time 12 -u "$HTTP_USER:$HTTP_PASS" "$(http_base)/snapshot.jpg?chn=0" -o "$OUTDIR/flip/dn_after.jpg" 2>/dev/null
				if [ -s "$OUTDIR/flip/dn_after.jpg" ]; then
					ffmpeg -hide_banner -nostdin -loglevel error -y -i "$OUTDIR/flip/dn_before.jpg" -vf hflip "$OUTDIR/flip/dn_before_m.jpg" </dev/null 2>/dev/null
					pd=$(psnr_db "$OUTDIR/flip/dn_after.jpg" "$OUTDIR/flip/dn_before.jpg")
					pm=$(psnr_db "$OUTDIR/flip/dn_after.jpg" "$OUTDIR/flip/dn_before_m.jpg")
					if [ -n "$pd" ] && [ -n "$pm" ] && fcmp "$pm" ge "$(awk -v x="$pd" 'BEGIN{printf "%.2f", x+3}')"; then
						warn "day/night: the image appears MIRRORED after the transition (matches the mirrored pre-transition frame by $(awk -v a="$pm" -v b="$pd" 'BEGIN{printf "%.2f",a-b}')dB) - a day/night switch may be resetting hflip/vflip independently of a chn0 relatch (8fb6fd3's untested hypothesis)"
					else
						ok "day/night: image orientation unchanged across the transition (direct ${pd:-?}dB vs mirrored ${pm:-?}dB)"
					fi
				fi
			fi

			# --- restore -------------------------------------------------------
			lv_post "$dn_full_restore" >/dev/null
			dnr="$OUTDIR/lv_dn_restore.json"; lv_get "$dnr"
			# verify the WINDOWS too, not just mode: leaving the test
			# windows behind is silent (they change nothing visible in
			# auto mode except the heartbeat's dawn target), so nothing else
			# would ever report it.
			dnr_tns=$(jget "$dnr" daynight.time_night_start); dnr_tds=$(jget "$dnr" daynight.time_day_start)
			if [ "$(jget "$dnr" daynight.dn_mode)" = "$dn_mode_cur2" ] &&
			   [ "${dnr_tns:-}" = "${tns_cur2:-}" ] && [ "${dnr_tds:-}" = "${tds_cur2:-}" ]; then
				LV_PENDING=""; info "  day/night: restored mode=$dn_mode_cur2 and windows ('${tns_cur2}'..'${tds_cur2}')"
			else
				warn "day/night: restore did not fully land (mode=$(jget "$dnr" daynight.dn_mode), windows '${dnr_tns}'..'${dnr_tds}' want '${tns_cur2}'..'${tds_cur2}') - check the camera's daynight settings by hand"
			fi
		fi
	fi

	# --- record: the running recorder reads these live. enabled/mode/channel are
	# left out (they would start/stop capture or depend on stream count); the
	# rolls/segment/min-free/audio/name/dir round-trip live. seg 0..86400, pre
	# 0..60, post 1..300 (F-10: floor is 1, not 0), min_free 0..1048576. dir is
	# the path-traversal-sensitive live string (F-08) - covered alongside name ---
	lv_section record '{"record":' '}' record \
		"segment_s int 10 600" "pre_roll_s int 0 60" "post_roll_s int 1 300" \
		"min_free_mb int 50 2000" "audio bool" "name str" "dir str"

	# --- timelapse: the running timelapse thread reads these live. interval_s
	# >=1, keep_days >=0; enabled/channel left out for the same reason as record.
	# dir is the path-traversal-sensitive live string (F-08), covered with name ---
	lv_section timelapse '{"timelapse":' '}' timelapse \
		"interval_s int 1 3600" "keep_days int 0 365" "name str" "dir str"

	# --- clamp regression: timps_apply_setting() must persist/echo the
	# VALIDATED (clamped) value, not the raw pre-clamp POST body - the
	# 2026-08-05 fix in this session's v1.7.7 release notes. Every lv_section
	# test above only ever POSTs values already inside [lo,hi] (flip_int's
	# midpoint), so none of them would catch a regression here. POST a
	# generously out-of-range value for one already-covered int field per
	# section and confirm the read-back is the clamped boundary, not the raw
	# number we sent. ---
	ov_clamp_test() {
		local label="$1" wo="$2" wc="$3" rp="$4" key="$5" raw="$6" want="$7"
		local cur code gf got
		cur=$(jget "$LV_BASE" "$rp.$key")
		if [ -z "$cur" ]; then skip "$label.$key clamp test: field not present"; return; fi
		LV_PENDING="${wo}{\"$key\":$cur}${wc}"
		code=$(lv_post "${wo}{\"$key\":$raw}${wc}")
		gf="$OUTDIR/clamp_${label}_${key}.json"; lv_get "$gf"
		got=$(jget "$gf" "$rp.$key")
		if [ "$got" = "$want" ]; then
			ok "$label.$key clamp: raw=$raw persisted/echoed as clamped $want, not the raw value (HTTP $code)"
		else
			bad "$label.$key clamp: raw=$raw -> got '$got', want clamped '$want' (persist-clamp regression?)"
		fi
		# Restore and VERIFY it actually landed - a silently-dropped/failed
		# restore POST here (network blip, concurrent-run contention) used to
		# strand the camera at the clamped boundary (e.g. brightness=255)
		# with zero indication in this script's output. One retry before
		# warning: transient POST failures are the expected failure mode
		# under load, not a persist regression worth a "bad".
		local rcode rgf rgot
		rcode=$(lv_post "${wo}{\"$key\":$cur}${wc}")
		rgf="$OUTDIR/clamp_${label}_${key}_restore.json"; lv_get "$rgf"
		rgot=$(jget "$rgf" "$rp.$key")
		if [ "$rgot" != "$cur" ]; then
			rcode=$(lv_post "${wo}{\"$key\":$cur}${wc}")
			lv_get "$rgf"; rgot=$(jget "$rgf" "$rp.$key")
		fi
		if [ "$rgot" = "$cur" ]; then
			LV_PENDING=""
		else
			warn "$label.$key clamp: restore to '$cur' did not land (got '$rgot', HTTP $rcode) - camera may still be at the clamped boundary"
		fi
	}
	ov_clamp_test image      '{"image":'    '}' image      brightness       -99      0
	ov_clamp_test image      '{"image":'    '}' image      brightness       9999     255
	ov_clamp_test daynight   '{"daynight":' '}' daynight   probe_min_gap_s  1        60
	ov_clamp_test daynight   '{"daynight":' '}' daynight   heartbeat_max_s  99999999 604800

	# --- video/sensor persist-first sanity: whatever the platform can or cannot
	# push to a live encoder, EVERY videoN.* key must still round-trip through
	# the config layer and be reported back. One representative key; the WHEN
	# (live now vs. next restart) is a separate question, graded per key by the
	# rc block below against caps.video_live and the POST reply's deferred list.
	# This used to be worded "must NOT be advertised as live", which was true of
	# every videoN.* key until 22832f1 and is now wrong for the rc subset on
	# most SoCs - bitrate is live on all four new-API platforms and on classic.
	pv_cur=$(jget "$LV_BASE" video.0.bitrate)
	if [ -n "$pv_cur" ]; then
		pv_new=$(flip_int 512 8000 "$pv_cur")
		LV_PENDING="{\"video\":{\"0\":{\"bitrate\":$pv_cur}}}"   # armed until restore
		code=$(lv_post "{\"video\":{\"0\":{\"bitrate\":$pv_new}}}")
		lv_get "$OUTDIR/lv_persist.json"
		got=$(jget "$OUTDIR/lv_persist.json" video.0.bitrate)
		[ "$got" = "$pv_new" ] && ok "video0.bitrate round-trips through config ($pv_new; live-vs-restart graded below)" \
			|| bad "video0.bitrate did not persist (got '$got', want '$pv_new')"
		lv_mark video bitrate
		lv_post "{\"video\":{\"0\":{\"bitrate\":$pv_cur}}}" >/dev/null   # restore
		LV_PENDING=""
	fi

	# --- videoN rate control: LIVE apply, per-request grading, and what the
	# encoder ACTUALLY holds (c4e434f/3edb85e/443584e/22832f1, 2026-08-21) ----
	#
	# Until those four commits every videoN.* key was persist-only by design,
	# so 8b covered exactly one of them (the bitrate round-trip above) and 8d
	# allowlisted the rest wholesale. Three new host-observable mechanisms
	# changed that, which is why these checks live here in the default flow
	# instead of behind --test-encoder's restart-and-measure:
	#   caps.video_live      the videoN.* keys THIS build can push to a RUNNING
	#                        encoder. enc_caps.h feeds both this list and the
	#                        HAL's own gate, so a list that matches none of the
	#                        four documented per-SoC sets means the two drifted.
	#   deferred/deferred_keys in every POST reply - the per-REQUEST truth about
	#                        which changed video/sensor keys did NOT reach the
	#                        pipeline (channel down, wrong rc mode, rejected IMP
	#                        call). A platform capability is not a promise; this
	#                        is the promise being kept or not, per request.
	#   encoder.<n>.rc       IMP_Encoder_GetChnAttrRcMode readback: what the
	#                        encoder HOLDS, as opposed to what g_cfg was told.
	#                        Everything else in 8b can only ever see the latter,
	#                        which is precisely why "accepted, persisted,
	#                        faithfully echoed, and silently ignored" is this
	#                        project's most-repeated bug shape (340fb1f,
	#                        ff28ee2, f003655, 0a8bb9f, 6ec766e, dd2221f,
	#                        51bf052, 30ecc74). This is the first check in the
	#                        script that can separate the two WITHOUT a restart
	#                        and a stream measurement.
	#
	# NONE of this had ever executed on real hardware when these checks were
	# written - all four commit messages say so in as many words, and 1bdd1b3
	# lists the specific open questions (bitrate unit, iIPDelta sign/scale).
	# So: a failure here is a candidate DAEMON bug first and a script bug
	# second, and the verdict wording below says which one it would be.
	#
	# Everything is probed on video1 (the SUBSTREAM) for the same reason 8g is
	# substream-only, except the per-channel isolation check, which cannot
	# prove anything without touching both. That one is limited to a +-1 QP
	# bound nudge on the mainstream, applied live and reverted in the next
	# request - the smallest perturbation that is still observable.
	rc_live=$(jget "$LV_BASE" caps.video_live)
	# every rc/tuning key this block has a probe for, for the 8d ledger: each
	# is either POSTed below or gate-marked with the reason it was not.
	RC_ALL_KEYS="rc_mode bitrate qp min_qp max_qp quality_lvl change_pos i_bias_lvl fluc_lvl gop max_gop profile"
	if [ -z "$rc_live" ]; then
		# No caps.video_live at all: pre-22832f1 daemon. Same "older build
		# lacks this capability" skip 8d uses for ?fields=1 and 4b for SRT -
		# and emphatically NOT a silent pass, because a stale build is the one
		# way this whole block can look green without testing anything.
		skip "videoN rate control: GET /control has no caps.video_live - this daemon predates 22832f1 (live rc apply) and cannot exercise any of it. Everything below (live apply, deferred grading, encoder.<n>.rc readback) is untestable on this build; reflash before trusting a green run here"
		lv_mark_gated video "no-caps.video_live(daemon-predates-22832f1)" $RC_ALL_KEYS
	else
	rc_live_keys=$(printf '%s' "$rc_live" | tr -d '[]"' | tr ',' ' ' | tr -s ' ' | sed 's/^ *//;s/ *$//')
	rc_live_has() { case " $rc_live_keys " in *" $1 "*) return 0;; esac; return 1; }
	rc_defer_has() { case "$(jget "$1" deferred_keys)" in *"\"$2\""*) return 0;; esac; return 1; }

	# --- RC1: caps.video_live must be one of enc_caps.h's per-SoC sets -------
	# "not more, not fewer" is checkable without knowing the SoC: enc_caps.h
	# defines four ENC_LIVE_KEYS variants collapsing to three distinct sets
	# (T40 and T41 are identical since qp stopped being advertised live, see
	# the qp note in enc_caps.h), so anything else means the
	# caps builder in control.c and the header have drifted apart - the exact
	# failure the isp_caps.h pattern exists to prevent. With --ssh the SoC is
	# also pinned to the RIGHT one of the four.
	rc_set=$(printf '%s\n' $rc_live_keys | sort -u | tr '\n' ' ')
	case "$rc_set" in
		"bitrate i_bias_lvl max_qp min_qp ")    rc_plat="T31/C100";;
		"bitrate max_qp min_qp ")               rc_plat="T40/T41";;
		"bitrate change_pos i_bias_lvl max_qp min_qp qp quality_lvl rc_mode ") rc_plat="classic";;
		" ") rc_plat="none";;
		*)   rc_plat="?";;
	esac
	if [ "$rc_plat" = none ]; then
		info "  caps.video_live is empty - no live rc path in this build (host sim, or a HAL without a control callback). Nothing to grade live."
		lv_mark_gated video "caps.video_live-empty(no-live-rc-path-in-this-build)" $RC_ALL_KEYS
	elif [ "$rc_plat" = "?" ]; then
		bad "caps.video_live is [$rc_live_keys], which matches NONE of enc_caps.h's four documented ENC_LIVE_KEYS sets (T31/C100, T40, T41, classic) - either control.c's caps builder and enc_caps.h have drifted apart, or a platform was added without updating this check. Everything below grades against a list that cannot be trusted"
		lv_mark_gated video "caps.video_live-unrecognised" $RC_ALL_KEYS
	else
		ok "caps.video_live = [$rc_live_keys] - exactly enc_caps.h's $rc_plat set (no drift between the header and the caps builder)"
		# thingino's /usr/sbin/soc prints the full part number (t31x, t31l,
		# t23n, c100...); enc_caps.h switches on the FAMILY, so drop the
		# variant suffix. Without --ssh the set-shape check above stands on
		# its own - it just cannot say the shape belongs to THIS camera.
		rc_soc=""; [ -n "$SSH_TARGET" ] && rc_soc=$(sshx "soc 2>/dev/null" 2>/dev/null | tr -dc 'a-z0-9')
		if [ -n "$rc_soc" ]; then
			case "$rc_soc" in
				t31*|c100*)                    rc_exp="T31/C100";;
				t40*|t41*)                     rc_exp="T40/T41";;
				t10*|t20*|t21*|t23*|t30*)      rc_exp="classic";;
				*)                             rc_exp="";;
			esac
			if [ -z "$rc_exp" ]; then
				info "  camera reports soc=$rc_soc, which this check has no enc_caps.h mapping for - set shape verified, SoC match not"
			elif [ "$rc_exp" = "$rc_plat" ]; then
				ok "caps.video_live matches the SoC this camera actually is (soc=$rc_soc -> $rc_exp)"
			else
				bad "caps.video_live advertises the $rc_plat key set but the camera reports soc=$rc_soc, which enc_caps.h maps to $rc_exp - the daemon was built for a different platform than it is running on, or enc_caps.h's #if chain is wrong"
			fi
		fi
	fi

	# The rest only makes sense with a live path AND a queryable encoder. The
	# readback is omitted by design for a channel with no encoder to query
	# (disabled stream, T23 sw-rotate Yuv path, sim) - matching hal_enc_stats -
	# so its absence is a legitimate skip, not a failure, and the deferred
	# grading is then the only thing left to assert.
	rc_rb=$(jget "$LV_BASE" encoder.1.rc)
	if [ "$rc_plat" = none ] || [ "$rc_plat" = "?" ]; then
		:
	elif [ -z "$rc_rb" ]; then
		skip "videoN rate control: caps.video_live is non-empty but GET /control carries no encoder.1.rc - substream channel has no queryable encoder (disabled, sw-rotate Yuv path, or bring-up failed). Cannot compare written against held values"
		lv_mark_gated video "encoder.1.rc-absent(no-queryable-encoder-on-substream)" $RC_ALL_KEYS
	else
		# --- RC2: written vs held, before touching anything ------------------
		# The readback's entire reason to exist. Compare the CONFIGURED
		# videoN.* block against what the encoder reports holding, for every
		# field both carry. Fields the current mode/API does not carry are
		# omitted from the JSON, so an empty read here means "not applicable",
		# never "zero".
		# rc_mode needs a translation table, not an equality: both HALs
		# substitute modes their SDK lacks, and both say so in the log. Not
		# encoding that here would turn a documented, deliberate substitution
		# into a false FAIL.
		rc_mode_expect() {
			case "$rc_plat" in
				classic) case "$1" in capped_vbr|capped_quality) echo vbr;; *) echo "$1";; esac;;
				*)       case "$1" in smart) echo capped_quality;; *) echo "$1";; esac;;
			esac
		}
		rc_hold_ok=1
		for rc_ch in 0 1; do
			[ "$(jget "$LV_BASE" "video.$rc_ch.enabled")" = "1" ] || continue
			rc_h_mode=$(jget "$LV_BASE" "encoder.$rc_ch.rc.rc_mode")
			if [ -z "$rc_h_mode" ]; then
				info "  encoder.$rc_ch.rc absent although video$rc_ch is enabled - channel has no queryable encoder (sw-rotate/bring-up); nothing to compare for this channel"
				continue
			fi
			rc_c_mode=$(jget "$LV_BASE" "video.$rc_ch.rc_mode")
			rc_w_mode=$(rc_mode_expect "$rc_c_mode")
			if [ "$rc_h_mode" != "$rc_w_mode" ]; then
				bad "encoder.$rc_ch.rc: configured rc_mode=$rc_c_mode should be held as '$rc_w_mode' on a $rc_plat SoC, but the encoder reports '$rc_h_mode' - the mode timps wrote at bring-up is not the mode the encoder is running"
				rc_hold_ok=0
			fi
			for rc_f in min_qp max_qp; do
				rc_h=$(jget "$LV_BASE" "encoder.$rc_ch.rc.$rc_f")
				[ -n "$rc_h" ] || continue          # not carried by this mode
				rc_c=$(jget "$LV_BASE" "video.$rc_ch.$rc_f")
				[ "$rc_h" = "$rc_c" ] && continue
				bad "encoder.$rc_ch.rc.$rc_f=$rc_h but video$rc_ch.$rc_f=$rc_c was configured - the QP bound timps wrote at bring-up is not the one the encoder holds"
				rc_hold_ok=0
			done
			# bitrate is NOT compared for equality: whether the SDK stores the
			# value in kbps (what SetDefaultParam is fed) or bit/s is the open
			# question c4e434f was written to answer, and either answer is a
			# legitimate reading here. Report the ratio; the live probe below
			# is what turns it into a verdict, by checking the BOOT path and
			# the LIVE path agree on it.
			rc_h_br=$(jget "$LV_BASE" "encoder.$rc_ch.rc.bitrate")
			rc_c_br=$(jget "$LV_BASE" "video.$rc_ch.bitrate")
			if [ -n "$rc_h_br" ] && [ -n "$rc_c_br" ] && fcmp "$rc_c_br" gt 0; then
				info "  encoder.$rc_ch.rc: mode=$rc_h_mode holds bitrate=$rc_h_br against a configured ${rc_c_br} kbps (ratio $(awk -v a="$rc_h_br" -v b="$rc_c_br" 'BEGIN{printf "%.4g", a/b}') - 1 = the SDK stores kbps, 1000 = bit/s)"
			fi
		done
		[ "$rc_hold_ok" = 1 ] && ok "encoder.<n>.rc readback agrees with the configured videoN.* block on every field both carry (rc_mode incl. the documented $rc_plat substitutions, QP bounds)"

		# --- the live probes -------------------------------------------------
		# A restart between the POST and the read-back would make every
		# assertion below meaningless (a restarted daemon applies EVERYTHING,
		# live path or not), so pin the daemon identity across the whole
		# sequence the same way the rest of the script detects a restart:
		# pidof over SSH. Without --ssh the checks still run - a live apply
		# that silently needed a restart would show up as the daemon being
		# unreachable for a while, not as a pass - but say that they are
		# unpinned rather than implying more than was proven.
		rc_pid0=""; [ -n "$SSH_TARGET" ] && rc_pid0=$(sshx "pidof timpsd" 2>/dev/null | awk '{print $1}')
		rc_pid_check() {   # $1=context; FAILs (and only then) if the daemon restarted
			local now
			[ -n "$rc_pid0" ] || return 0
			now=$(sshx "pidof timpsd" 2>/dev/null | awk '{print $1}')
			[ "$now" = "$rc_pid0" ] && return 0
			bad "$1: timpsd pid changed during the live-apply probe ($rc_pid0 -> ${now:-gone}) - the daemon RESTARTED, so nothing above proves a live path"
		}
		[ -n "$rc_pid0" ] && info "  live-rc probes pinned to timpsd pid $rc_pid0 (any restart mid-probe invalidates the results and is caught)" \
			|| info "  live-rc probes NOT pinned to a pid (no --ssh) - a live apply that secretly required a restart cannot be distinguished from one that did not"

		# rc_probe <key> <new-value> <readback-field> -> POST video1.<key>,
		# assert the daemon graded it live (not deferred), that the readback
		# followed, and that no restart happened. Restores in the same shape
		# as lv_section, trap-armed.
		rc_probe() {
			local key="$1" new="$2" fld="$3" cur rf gf got code
			cur=$(jget "$LV_BASE" "video.1.$key")
			if [ -z "$cur" ]; then
				info "  video1.$key not present in GET /control - skipped"
				lv_mark_gated video "not-present-in-GET-/control" "$key"; return; fi
			rf="$OUTDIR/rc_post_$key.json"; gf="$OUTDIR/rc_read_$key.json"
			LV_PENDING="{\"video\":{\"1\":{\"$key\":$cur}}}"
			code=$(lv_post_r "{\"video\":{\"1\":{\"$key\":$new}}}" "$rf")
			lv_mark video "$key"
			if [ "$code" != "200" ]; then
				bad "video1.$key: POST(live) HTTP $code"
				lv_post "$LV_PENDING" >/dev/null; LV_PENDING=""; return; fi
			lv_get "$gf"
			if rc_defer_has "$rf" "video1.$key"; then
				bad "video1.$key is listed in caps.video_live but the POST reply DEFERRED it (deferred=$(jget "$rf" deferred), keys=$(jget "$rf" deferred_keys)) - the platform advertises a live path the runtime then refused. Check the daemon log for the matching 'applies on restart' line; this is the daemon disagreeing with its own caps, not a script assumption"
			else
				got=$(jget "$gf" "encoder.1.rc.$fld")
				if [ -z "$got" ]; then
					warn "video1.$key: graded live (not deferred) but encoder.1.rc.$fld is absent from the readback - the current rc mode ($(jget "$gf" encoder.1.rc.rc_mode)) does not carry that field, so the apply cannot be confirmed either way"
				elif [ "$got" = "$new" ]; then
					ok "video1.$key=$new applied LIVE and the encoder confirms it (encoder.1.rc.$fld=$got) - no restart"
				else
					bad "video1.$key=$new was graded applied-live, but the encoder holds encoder.1.rc.$fld=$got, not $new - the IMP call reported success and the value did not arrive unaltered. This is the c4e434f readback doing its job; suspect the HAL's key->SDK-field mapping before suspecting this check"
				fi
			fi
			rc_pid_check "video1.$key"
			lv_post "$LV_PENDING" >/dev/null; LV_PENDING=""
			lv_get "$OUTDIR/rc_restore_$key.json"
			[ "$(jget "$OUTDIR/rc_restore_$key.json" "video.1.$key")" = "$cur" ] \
				|| warn "video1.$key: did not restore to $cur"
		}

		# --- RC3: QP bounds, the unit-free live keys -------------------------
		# min_qp/max_qp go through one dedicated SDK call on the new API and
		# ride the full union re-fill on classic, and unlike bitrate they have
		# no unit ambiguity at all - so they are the cleanest proof that the
		# live path works end to end. Nudged by 1 from the current value:
		# enough to be observable, small enough that a stranded value is a
		# non-event. The SDK's own [0..maxQp] constraint is respected by
		# construction (only the floor moves, and only downward).
		for rc_k in min_qp max_qp; do
			if ! rc_live_has "$rc_k"; then
				info "  video1.$rc_k: not in caps.video_live on this $rc_plat build - restart-bound here"
				lv_mark_gated video "not-in-caps.video_live-on-$rc_plat" "$rc_k"
				continue
			fi
			rc_cur=$(jget "$LV_BASE" "video.1.$rc_k")
			[ -n "$rc_cur" ] || continue
			rc_probe "$rc_k" "$((rc_cur-1))" "$rc_k"
		done

		# --- the measured probes: one shared baseline ------------------------
		# RC3b/RC3c/RC4 all need "what does the substream deliver RIGHT NOW,
		# untouched", and all three run back-to-back on an unchanged config, so
		# measuring it three times would cost three captures to learn the same
		# number. Memoised: the first caller pays, the rest read the cache. It
		# also anchors the three verdicts to ONE reference measurement, so a
		# scene change mid-section shifts all of them together instead of making
		# two of them silently disagree.
		#   rc_base_kbps  delivered kbps, "" when unmeasurable
		#   rc_base_iavg  mean keyframe bytes (RC5's sweep reference)
		# A camera whose substream cannot be pulled at all leaves these empty and
		# every measured probe below skips - never FAILs, because an unreachable
		# stream proves nothing about the encoder.
		rc_base_done=0; rc_base_kbps=""; rc_base_iavg=""; rc_base_icnt=""
		rc_meas_dur="${RC_MEAS_DUR:-${RC4_DUR:-15}}"
		rc_baseline() {
			local r
			[ "$rc_base_done" = 1 ] && return 0
			rc_base_done=1
			if r=$(enc_measure "$PATH_SUB" "$rc_meas_dur" rc_base); then
				read -r rc_base_kbps _ rc_base_iavg rc_base_icnt _ <<<"$r"
				info "  measured baseline: substream delivers ${rc_base_kbps} kbps (configured $(jget "$LV_BASE" video.1.bitrate) kbps), mean keyframe ${rc_base_iavg} B over ${rc_base_icnt} I-frames in ${rc_meas_dur}s"
			fi
			return 0
		}
		# How long to wait after a live rc POST before the change is fully
		# expressed in the bitstream: rate control re-plans at the next IDR, so
		# ~3 GOPs, clamped into 4..20s. Same figure RC4 derived; hoisted here so
		# every measured probe uses it instead of growing a second constant.
		rc_settle_s=$(awk -v g="$(jget "$LV_BASE" video.1.gop)" -v f="$(jget "$LV_BASE" video.1.fps)" \
			'BEGIN{ s=(f>0&&g>0)? 3*g/f : 6; if(s<4)s=4; if(s>20)s=20; printf "%.0f", s }')
		rc_can_measure() { have ffmpeg && have ffprobe; }

		# --- RC3b: min_qp must actually CONSTRAIN the bitstream --------------
		# RC3 above proves min_qp reaches the SDK struct and is echoed back.
		# That is precisely the evidence this project has learned not to trust
		# on its own: "accepted, persisted, faithfully echoed, and silently
		# ignored" is what 340fb1f/ff28ee2/0a8bb9f/6ec766e all turned out to be.
		# A QP FLOOR is measurable without extracting QP at all: raising it
		# forbids the encoder the fine quantisation it needs to spend bits, so
		# the delivered bitrate must FALL.
		#
		# Direction, measured on a T31/sc4336p 2026-08-22 (640x360 substream,
		# cbr, 384 kbps configured, static scene):
		#     min_qp 10           -> 392 kbps  (rises to the 384 ceiling)
		#     min_qp 20 (default) -> 208..225 kbps
		#     min_qp 40           -> 23..24 kbps
		# i.e. RAISING the floor LOWERS the bitrate. Worth stating explicitly
		# because the intuition runs the other way: low QP means FINE
		# quantisation means MORE bits, so a floor on QP is a ceiling on rate.
		#
		# The probe raises the floor to max_qp-5 rather than by a fixed step -
		# coarse enough to starve any scene on any sensor, so the verdict does
		# not depend on how busy the picture happens to be. It needs real
		# headroom between the current floor and that target, otherwise a null
		# result would be meaningless rather than damning: hence the >=8 gate.
		rc_mq_lo=$(jget "$LV_BASE" video.1.min_qp)
		rc_mq_hi=$(jget "$LV_BASE" video.1.max_qp)
		if ! rc_live_has min_qp || ! rc_live_has max_qp; then
			: # RC3 already gate-marked these; nothing measurable to add
		elif ! rc_can_measure; then
			skip "video1.min_qp/max_qp bitstream verification needs ffmpeg+ffprobe - not found (the RC3 readback still ran, but it only proves the value reached the struct, not that the bound binds)"
		elif [ -z "$rc_mq_lo" ] || [ -z "$rc_mq_hi" ] || [ "$((rc_mq_hi-rc_mq_lo))" -lt 8 ]; then
			skip "video1.min_qp: floor $rc_mq_lo and ceiling $rc_mq_hi are less than 8 QP apart - no room for a probe coarse enough to prove the floor binds"
		else
			rc_baseline
			# 24 kbps, not RC4's 64: this probe needs no bitrate headroom, only
			# enough delivered bits that a 0.6x drop is bigger than the capture
			# noise. A dark static scene at night legitimately sits near here.
			if [ -z "$rc_base_kbps" ] || ! fcmp "$rc_base_kbps" ge 24; then
				skip "video1.min_qp: no usable substream baseline (got '${rc_base_kbps:-none}' kbps) - a floor that binds cannot be told apart from a stream that was already delivering nothing"
				lv_mark_gated video "no-usable-baseline-for-bitstream-check" min_qp
			else
				rc_mq_new=$((rc_mq_hi-5))
				rf="$OUTDIR/rc_post_minqp_meas.json"
				LV_PENDING="{\"video\":{\"1\":{\"min_qp\":$rc_mq_lo}}}"
				code=$(lv_post_r "{\"video\":{\"1\":{\"min_qp\":$rc_mq_new}}}" "$rf")
				lv_mark video min_qp
				if [ "$code" != "200" ]; then
					bad "video1.min_qp: POST(live) HTTP $code"
				elif rc_defer_has "$rf" video1.min_qp; then
					bad "video1.min_qp is in caps.video_live but the POST reply DEFERRED it (keys=$(jget "$rf" deferred_keys))"
				else
					sleep "$rc_settle_s"
					if rc_r=$(enc_measure "$PATH_SUB" "$rc_meas_dur" rc3b); then
						read -r rc_mq_kbps _ _ _ _ <<<"$rc_r"
						rc_mq_want=$(awk -v b="$rc_base_kbps" 'BEGIN{printf "%.0f", b*0.6}')
						rc_mq_ratio=$(awk -v a="$rc_mq_kbps" -v b="$rc_base_kbps" 'BEGIN{printf "%.2f", (b>0)?a/b:0}')
						if fcmp "$rc_mq_kbps" le "$rc_mq_want"; then
							ok "video1.min_qp really CONSTRAINS the bitstream: raising the QP floor $rc_mq_lo -> $rc_mq_new cut the delivered substream ${rc_base_kbps} -> ${rc_mq_kbps} kbps (${rc_mq_ratio}x) with the bitrate target untouched - the bound is not merely echoed back, it binds"
						elif fcmp "$rc_mq_ratio" ge 0.9; then
							bad "video1.min_qp: the QP floor went $rc_mq_lo -> $rc_mq_new live (deferred:0) and encoder.1.rc echoed it, yet the substream still delivers ${rc_mq_kbps} kbps against a ${rc_base_kbps} kbps baseline (${rc_mq_ratio}x) - a floor that coarse cannot leave the bitrate untouched, so the bound is being accepted, persisted, echoed and IGNORED (the 340fb1f/ff28ee2 class). Suspect IMP_Encoder_SetChnQpBounds not reaching the running channel"
						else
							warn "video1.min_qp: floor $rc_mq_lo -> $rc_mq_new moved the substream ${rc_base_kbps} -> ${rc_mq_kbps} kbps (${rc_mq_ratio}x) - right direction, weaker than the 0.6x this probe expects. The scene may already be quantisation-limited; re-run on a busier picture before reading anything into it"
						fi
					else
						warn "video1.min_qp: could not measure the substream after raising the floor - cannot confirm the bound binds"
					fi
				fi
				rc_pid_check video1.min_qp
				lv_post "$LV_PENDING" >/dev/null; LV_PENDING=""
			fi
		fi

		# --- RC3c: max_qp must actually CONSTRAIN the bitstream --------------
		# The QP CEILING is the mirror image and needs a different setup: it only
		# binds when the encoder WANTS to quantise coarsely, i.e. when the bitrate
		# target is starved relative to what the scene costs. On a comfortable
		# target the encoder never approaches max_qp and moving it proves nothing,
		# which is why this probe deliberately starves the target first and then
		# compares two ceilings against that SAME target.
		#
		# Differential, measured on the same T31 2026-08-22 (bitrate starved to
		# 33 kbps, ~0.15x of the 208 kbps the scene was delivering):
		#     max_qp 28 -> 82..83 kbps  (not allowed to degrade, overshoots ~2.5x)
		#     max_qp 45 -> 67 kbps
		#     max_qp 51 -> 13..14 kbps  (free to degrade, nearly reaches target)
		# The low-vs-high pair separates by ~6x, against only 1.2x for low-vs-
		# default, which is why the probe uses both ends rather than comparing one
		# end against the configured value. Both halves sit at the same starved
		# bitrate, so the delta is attributable to max_qp alone.
		if ! rc_live_has min_qp || ! rc_live_has max_qp || ! rc_live_has bitrate; then
			: # gate-marked by RC3/RC4
		elif ! rc_can_measure || [ -z "$rc_mq_lo" ] || [ -z "$rc_mq_hi" ]; then
			: # already reported by RC3b
		elif [ "$((51-rc_mq_hi))" -lt 4 ] || [ "$((rc_mq_hi-rc_mq_lo))" -lt 8 ]; then
			skip "video1.max_qp: ceiling $rc_mq_hi leaves no room for a low/high pair inside [$rc_mq_lo..51] - cannot build a differential that isolates the ceiling"
		else
			rc_baseline
			# starve to ~0.15x of what the scene actually costs, floored at the
			# videoN.bitrate config clamp of 16 - low enough that the encoder is
			# pinned against its ceiling in both halves of the pair. The gate is
			# not a flat baseline floor but whether that clamp still leaves a real
			# starve: on a scene delivering under ~32 kbps the floored target is no
			# longer below what the scene costs, and neither ceiling would bind.
			rc_starve=$(awk -v m="${rc_base_kbps:-0}" 'BEGIN{v=int(m*0.15); if(v<16)v=16; print v}')
			if [ -z "$rc_base_kbps" ] || ! fcmp "$rc_starve" le "$(awk -v b="${rc_base_kbps:-0}" 'BEGIN{printf "%.0f", b*0.5}')"; then
				skip "video1.max_qp: baseline '${rc_base_kbps:-none}' kbps leaves no starved target below the 16 kbps config clamp - cannot make the ceiling bind, so a null result would prove nothing"
				lv_mark_gated video "no-usable-baseline-for-bitstream-check" max_qp
			else
				rc_b_orig=$(jget "$LV_BASE" video.1.bitrate)
				rc_qc_lo=$((rc_mq_lo+8)); [ "$rc_qc_lo" -gt "$rc_mq_hi" ] && rc_qc_lo=$rc_mq_hi
				rc_qc_hi=$((rc_mq_hi+6)); [ "$rc_qc_hi" -gt 51 ] && rc_qc_hi=51
				LV_PENDING="{\"video\":{\"1\":{\"bitrate\":$rc_b_orig,\"max_qp\":$rc_mq_hi}}}"
				lv_mark video max_qp bitrate
				rc_qc_a=""; rc_qc_b=""
				code=$(lv_post_r "{\"video\":{\"1\":{\"bitrate\":$rc_starve,\"max_qp\":$rc_qc_lo}}}" "$OUTDIR/rc_post_maxqp_lo.json")
				if [ "$code" != "200" ]; then
					bad "video1.max_qp: POST(live, starve+low ceiling) HTTP $code"
				else
					sleep "$rc_settle_s"
					rc_r=$(enc_measure "$PATH_SUB" "$rc_meas_dur" rc3c_lo) && read -r rc_qc_a _ _ _ _ <<<"$rc_r"
					code=$(lv_post_r "{\"video\":{\"1\":{\"max_qp\":$rc_qc_hi}}}" "$OUTDIR/rc_post_maxqp_hi.json")
					if [ "$code" != "200" ]; then
						bad "video1.max_qp: POST(live, high ceiling) HTTP $code"
					else
						sleep "$rc_settle_s"
						rc_r=$(enc_measure "$PATH_SUB" "$rc_meas_dur" rc3c_hi) && read -r rc_qc_b _ _ _ _ <<<"$rc_r"
					fi
				fi
				if [ -z "$rc_qc_a" ] || [ -z "$rc_qc_b" ] || ! fcmp "${rc_qc_b:-0}" gt 0; then
					warn "video1.max_qp: could not measure both halves of the ceiling differential (low=${rc_qc_a:-none}, high=${rc_qc_b:-none} kbps) - the ceiling stays unverified against the bitstream"
				else
					rc_qc_r=$(awk -v a="$rc_qc_a" -v b="$rc_qc_b" 'BEGIN{printf "%.2f", a/b}')
					if fcmp "$rc_qc_r" ge 1.5; then
						ok "video1.max_qp really CONSTRAINS the bitstream: at the same starved ${rc_starve} kbps target, ceiling $rc_qc_lo delivered ${rc_qc_a} kbps and ceiling $rc_qc_hi delivered ${rc_qc_b} kbps (${rc_qc_r}x apart) - a low ceiling forbids the encoder from degrading enough to reach the target, exactly as a QP bound must"
					else
						bad "video1.max_qp: at a starved ${rc_starve} kbps target the ceiling made no difference to the bitstream - $rc_qc_lo delivered ${rc_qc_a} kbps and $rc_qc_hi delivered ${rc_qc_b} kbps (${rc_qc_r}x). Both POSTs were graded live and encoder.1.rc echoes the bound, so this is the ceiling being accepted and IGNORED (the 340fb1f/ff28ee2 class) - unless the stream is content-limited well under ${rc_starve} kbps in both halves, in which case rate control had nothing to express either way"
					fi
				fi
				rc_pid_check video1.max_qp
				lv_post "$LV_PENDING" >/dev/null; LV_PENDING=""
				gf="$OUTDIR/rc_restore_maxqp.json"; lv_get "$gf"
				{ [ "$(jget "$gf" video.1.bitrate)" = "$rc_b_orig" ] && [ "$(jget "$gf" video.1.max_qp)" = "$rc_mq_hi" ]; } \
					|| warn "video1.max_qp: did not restore to bitrate=$rc_b_orig / max_qp=$rc_mq_hi"
			fi
		fi

		# --- RC4: bitrate, verified against the actual bitstream ------------
		# 1bdd1b3 left open whether SetDefaultParam (boot) and SetChnBitRate
		# (live) agree on the bitrate unit. The first version of this check
		# tried to settle it from encoder.1.rc.bitrate alone (held/configured
		# before vs. after a live POST) - no stream, no wait. That version is
		# WRONG: encoder.<n>.rc.bitrate does not advance until the channel
		# actually encodes a frame, which on an idle channel (no RTSP/HTTP
		# client pulling it) never happens - measured 2026-08-22 on a T31:
		# eight seconds of polling after a live POST, register frozen at the
		# OLD value throughout, while the real substream (pulled with ffmpeg
		# for the same eight seconds) had already moved. The register is a
		# fine cross-check for the fields that DO update synchronously
		# (min_qp/max_qp/i_bias_lvl, see RC3/RC5), just not for this one - so
		# this check now measures the bitstream directly, the same way 8g
		# proves the persist+restart route, and grades on THAT.
		if ! rc_live_has bitrate; then
			info "  video1.bitrate: not in caps.video_live on this $rc_plat build - restart-bound here"
			lv_mark_gated video "not-in-caps.video_live-on-$rc_plat" bitrate
		elif ! have ffmpeg || ! have ffprobe; then
			skip "video1.bitrate live-apply verification needs ffmpeg+ffprobe (real bitstream measurement) - not found"
			lv_mark_gated video "no-ffmpeg-ffprobe" bitrate
		else
			rc_b_cfg=$(jget "$LV_BASE" video.1.bitrate)
			rc_dur="$rc_meas_dur"
			# Baseline shared with RC3b/RC3c - see rc_baseline(). Both of those
			# restore the config before returning, so the cached number still
			# describes the state this probe starts from, and reusing it keeps all
			# three verdicts anchored to one reference instead of paying for a third
			# identical capture of the same untouched stream.
			rc_baseline
			rc_pre_kbps="$rc_base_kbps"
			[ -n "$rc_pre_kbps" ] && info "  video1.bitrate: substream delivers ${rc_pre_kbps} kbps at a configured ${rc_b_cfg} kbps"
			if [ -z "$rc_pre_kbps" ] || ! fcmp "$rc_pre_kbps" ge 64; then
				# no usable baseline (stream unreachable, or already so low
				# that 0.4x would land under the videoN.bitrate clamp of 16) -
				# same escape 8g takes, for the same reason: an unprovable
				# direction must not produce a hard FAIL.
				skip "video1.bitrate: could not measure a usable substream baseline (got '${rc_pre_kbps:-none}' kbps) - cannot pick a target the encoder is provably forced to follow"
				lv_mark_gated video "no-usable-baseline" bitrate
			else
				rc_b_new=$(awk -v m="$rc_pre_kbps" 'BEGIN{v=int(m*0.4); if(v<16)v=16; print v}')
				rf="$OUTDIR/rc_post_bitrate.json"
				LV_PENDING="{\"video\":{\"1\":{\"bitrate\":$rc_b_cfg}}}"
				code=$(lv_post_r "{\"video\":{\"1\":{\"bitrate\":$rc_b_new}}}" "$rf")
				lv_mark video bitrate
				if [ "$code" != "200" ]; then
					bad "video1.bitrate: POST(live) HTTP $code"
				elif rc_defer_has "$rf" video1.bitrate; then
					bad "video1.bitrate is in caps.video_live but the POST reply DEFERRED it (keys=$(jget "$rf" deferred_keys)) - SetChnBitRate was refused or the channel is not running; the platform advertises a live path the runtime did not take"
				else
					rc_settle="$rc_settle_s"   # ~3 GOPs, computed once above
					info "  video1.bitrate=$rc_b_new went in LIVE (deferred:0) - settling ${rc_settle}s (~3 GOPs) for the next-IDR latency before measuring"
					sleep "$rc_settle"
					if res1=$(enc_measure "$PATH_SUB" "$rc_dur" rc4_post); then
						read -r rc_post_kbps rc_post_cv _ _ _ <<<"$res1"
						rc_hi=$(awk -v n="$rc_b_new" 'BEGIN{printf "%.0f", n*1.5}')
						rc_keep=$(awk -v b="$rc_pre_kbps" 'BEGIN{printf "%.0f", b*0.8}')
						if fcmp "$rc_post_kbps" le "$rc_hi"; then
							ok "video1.bitrate=$rc_b_new applied LIVE and the substream actually follows it - measured ${rc_pre_kbps} -> ${rc_post_kbps} kbps (target was cut to ${rc_b_new}, delivered within 1.5x of it)"
						elif fcmp "$rc_post_kbps" ge "$rc_keep"; then
							bad "video1.bitrate: substream still delivers ${rc_post_kbps} kbps (was ${rc_pre_kbps}) after a live cut to ${rc_b_new} - the value was accepted and reported as applied live (deferred:0) but the encoder is ignoring it (the 340fb1f/ff28ee2 class - 22832f1's SetChnBitRate returning success without effect, invisible to any struct readback)"
						else
							warn "video1.bitrate: delivered ${rc_post_kbps} kbps moved toward the requested ${rc_b_new} (from ${rc_pre_kbps}) but did not get within 1.5x of it - rate control is loose on this SoC/scene, or ${rc_dur}s is too short a settle for this GOP length"
						fi
					else
						warn "video1.bitrate: could not measure the substream after the live POST (stream drop?) - cannot confirm the live-apply path had a real effect"
					fi
				fi
				rc_pid_check video1.bitrate
				lv_post "$LV_PENDING" >/dev/null; LV_PENDING=""
			fi
		fi

		# --- RC5: i_bias_lvl, the single most explicitly-unverified mapping --
		# 443584e passes videoN.i_bias_lvl (classic "I proportional frame
		# support", -3..3) straight into IMP_Encoder_SetChnQpIPDelta ("QP
		# difference between I frame and the first P"), 1:1, and says in the
		# commit message that the two are close relatives NOT proven identical
		# in sign or scale, with "the rc readback is the tool to verify the
		# mapping on hardware before trusting it". This is that verification.
		# Three distinguishable outcomes, worded apart on purpose:
		#   readback == posted        the 1:1 pass-through holds (this only
		#                             settles SCALE and SIGN of the transfer;
		#                             whether the two knobs MEAN the same thing
		#                             is RC5b's keyframe sweep, below)
		#   readback == -posted, or a
		#     consistent multiple     mapping is real but not 1:1 - the pass-
		#                             through is wrong and needs a conversion
		#   readback unmoved          the call did not reach the encoder at all
		# The probe value is +2 (in-domain, non-default, and not +-1 so a sign
		# flip cannot be mistaken for an off-by-one).
		if ! rc_live_has i_bias_lvl; then
			info "  video1.i_bias_lvl: not in caps.video_live on this $rc_plat build (T40/T41 SDKs have no SetChnQpIPDelta) - restart-bound or unsupported here"
			lv_mark_gated video "not-in-caps.video_live-on-$rc_plat" i_bias_lvl
		else
			rc_ib_cur=$(jget "$LV_BASE" video.1.i_bias_lvl)
			# which field carries it depends on the API generation: classic
			# reads iBiasLvl straight back as i_bias_lvl, the new API has no
			# such field and 443584e routes the value into iIPDelta instead.
			rc_ib_fld=ip_delta
			[ -n "$(jget "$LV_BASE" encoder.1.rc.i_bias_lvl)" ] && rc_ib_fld=i_bias_lvl
			rc_ib_before=$(jget "$LV_BASE" "encoder.1.rc.$rc_ib_fld")
			rc_ib_new=2; [ "${rc_ib_cur:-0}" = 2 ] && rc_ib_new=-2
			if [ -z "$rc_ib_cur" ]; then
				info "  video1.i_bias_lvl not present in GET /control - skipped"
				lv_mark_gated video "not-present-in-GET-/control" i_bias_lvl
			else
				rf="$OUTDIR/rc_post_i_bias_lvl.json"; gf="$OUTDIR/rc_read_i_bias_lvl.json"
				LV_PENDING="{\"video\":{\"1\":{\"i_bias_lvl\":$rc_ib_cur}}}"
				code=$(lv_post_r "{\"video\":{\"1\":{\"i_bias_lvl\":$rc_ib_new}}}" "$rf")
				lv_mark video i_bias_lvl
				lv_get "$gf"
				rc_ib_after=$(jget "$gf" "encoder.1.rc.$rc_ib_fld")
				if [ "$code" != "200" ]; then
					bad "video1.i_bias_lvl: POST(live) HTTP $code"
				elif rc_defer_has "$rf" video1.i_bias_lvl; then
					bad "video1.i_bias_lvl is in caps.video_live but the POST reply DEFERRED it (keys=$(jget "$rf" deferred_keys)) - IMP_Encoder_SetChnQpIPDelta was rejected by the SDK on this SoC. 443584e wired the call on the strength of a header grep alone; a refusal here means the call exists but does not work as assumed"
				elif [ -z "$rc_ib_after" ]; then
					warn "video1.i_bias_lvl: graded applied-live but encoder.1.rc.$rc_ib_fld is absent (mode $(jget "$gf" encoder.1.rc.rc_mode) does not carry it) - the 1:1 iBiasLvl->iIPDelta mapping stays UNVERIFIED on this camera"
				elif [ "$rc_ib_after" = "$rc_ib_new" ]; then
					ok "video1.i_bias_lvl=$rc_ib_new reaches the encoder 1:1 (encoder.1.rc.$rc_ib_fld $rc_ib_before -> $rc_ib_after) - the 443584e pass-through holds in scale and sign. What it MEANS is a separate question, answered by the RC5b keyframe sweep under --test-encoder (measured to be a no-op on T31, see its comment)"
				elif [ "$rc_ib_after" = "$((0-rc_ib_new))" ]; then
					bad "video1.i_bias_lvl=$rc_ib_new is held by the encoder as $rc_ib_fld=$rc_ib_after - the SIGN IS INVERTED. 443584e passes classic iBiasLvl into the new API's iIPDelta 1:1 and flags exactly this as unproven; the pass-through needs a negation on this SoC"
				elif [ "$rc_ib_after" = "$rc_ib_before" ]; then
					bad "video1.i_bias_lvl=$rc_ib_new was graded applied-live (SetChnQpIPDelta returned 0) but encoder.1.rc.$rc_ib_fld did not move off $rc_ib_before - the call succeeds and changes nothing. This is the 443584e mapping being wrong, or the SDK silently ignoring the setter"
				else
					bad "video1.i_bias_lvl=$rc_ib_new is held by the encoder as $rc_ib_fld=$rc_ib_after (was $rc_ib_before) - the value reaches the encoder but NOT 1:1, so 443584e's straight pass-through of iBiasLvl into iIPDelta needs a scale/offset conversion"
				fi
				rc_pid_check video1.i_bias_lvl
				lv_post "$LV_PENDING" >/dev/null; LV_PENDING=""
			fi
		fi

		# --- RC5b: what i_bias_lvl MEANS, measured on the keyframes ----------
		# RC5 settles the transfer (scale and sign of iBiasLvl -> iIPDelta); it
		# cannot settle the semantics, and dev_notes/TODO.md has carried "sweep
		# -3/0/+3 and measure I-frame windows" as open work since 443584e. This is
		# that sweep. i_bias_lvl is an I-frame QP bias, so its effect is an I-vs-P
		# bit REDISTRIBUTION at a constant target - invisible in overall kbps,
		# visible only in mean keyframe size, which enc_measure() now returns.
		#
		# Opt-in behind --test-encoder, and that is a deliberate cost call rather
		# than caution: the sweep is two more captures (~45s), and the ONLY SoCs
		# that can run it at all are T31/C100 (T40/T41 SDKs have no
		# SetChnQpIPDelta, so caps.video_live omits the key and this block never
		# fires) - where it has already been measured to be a null. Paying 45s on
		# every default run for a line we can predict is not worth it; keeping the
		# probe so a future SoC or SDK can be checked against it is.
		#
		# RESULT ON T31/sc4336p, 2026-08-22 - NO measurable semantic effect:
		#   live path, 3 interleaved -3/+3 pairs, 15s each, stable scene at the
		#   configured 384 kbps: mean keyframe 26254/26695/26646 B at -3 against
		#   26687/26701/26700 B at +3. Spread 1.7%, no direction, total delivered
		#   rate 224/225/226 kbps either way.
		#   boot path (value applied by enc_create after RegisterChn, daemon
		#   restarted between halves), 2 interleaved pairs: 13855/20549 B at -3
		#   against 14942/23034 B at +3 - the +3 half larger both times, but the
		#   scene was drifting hard across those runs (keyframes fell 26k -> 14k
		#   over the session) and 8-12% is inside that drift, so it is not a
		#   result. Boot and live use the identical SDK call on the identical
		#   channel, and the register echoes the value faithfully in both.
		# So: SetChnQpIPDelta is accepted, echoed by encoder.<n>.rc.ip_delta, and
		# does nothing to the bitstream under cbr on this SoC. That is an SDK-side
		# null, not a timps bug - the value timps sends is the value the encoder
		# reports holding - which is why the null grades INFO here. An INVERTED
		# sign would be actionable (443584e would need a negation) and grades WARN.
		# 15% is the effect threshold: an order of magnitude over the 1.7%
		# same-config spread measured above, comfortably under a real QP-delta
		# effect, which would move keyframe size by tens of percent.
		if [ "$TEST_ENCODER" != 1 ]; then
			:
		elif ! rc_live_has i_bias_lvl || [ -z "${rc_ib_cur:-}" ]; then
			:   # gate-marked by RC5
		elif ! rc_can_measure; then
			skip "video1.i_bias_lvl I-frame sweep needs ffmpeg+ffprobe - not found"
		else
			# Bracketed -3 / +3 / -3, not a bare A/B pair. Keyframe size on a real
			# camera drifts with the light: the same config measured across one
			# session on cam-garage produced 26.7k B in the afternoon and 13.9k B at
			# dusk, and an unbracketed pair duly reported a 41.9% "effect" with the
			# opposite sign to three earlier controlled runs that had agreed on 0.1%.
			# Repeating the -3 half at the end costs one capture and turns that
			# unknown into a measured drift estimate, so the verdict can require the
			# effect to be bigger than the drift instead of assuming it away.
			rc_ib_a1=""; rc_ib_b=""; rc_ib_a2=""
			LV_PENDING="{\"video\":{\"1\":{\"i_bias_lvl\":$rc_ib_cur}}}"
			for rc_ib_leg in a1:-3 b:3 a2:-3; do
				rc_ib_v="${rc_ib_leg#*:}"
				code=$(lv_post "{\"video\":{\"1\":{\"i_bias_lvl\":$rc_ib_v}}}")
				[ "$code" = "200" ] || { warn "video1.i_bias_lvl=$rc_ib_v: POST HTTP $code - sweep aborted"; break; }
				sleep "$rc_settle_s"
				rc_r=$(enc_measure "$PATH_SUB" "$rc_meas_dur" "rc5b_${rc_ib_leg%%:*}") || continue
				read -r _ _ rc_ib_iavg rc_ib_icnt _ <<<"$rc_r"
				[ "${rc_ib_icnt:-0}" -ge 3 ] || continue      # too few keyframes to average
				case "${rc_ib_leg%%:*}" in
					a1) rc_ib_a1="$rc_ib_iavg";; b) rc_ib_b="$rc_ib_iavg";; a2) rc_ib_a2="$rc_ib_iavg";;
				esac
			done
			rc_pid_check video1.i_bias_lvl-sweep
			lv_post "$LV_PENDING" >/dev/null; LV_PENDING=""
			if [ -z "$rc_ib_a1" ] || [ -z "$rc_ib_b" ] || [ -z "$rc_ib_a2" ] || ! fcmp "${rc_ib_b:-0}" gt 0; then
				warn "video1.i_bias_lvl: could not measure all three legs of the bracketed keyframe sweep (-3=${rc_ib_a1:-none} B, +3=${rc_ib_b:-none} B, -3 again=${rc_ib_a2:-none} B) - the semantics stay unverified on this camera"
			else
				# mean the two -3 legs so the comparison sits at the middle of the
				# drift, and measure the drift from how far they fell apart
				rc_ib_lo=$(awk -v x="$rc_ib_a1" -v y="$rc_ib_a2" 'BEGIN{printf "%.0f", (x+y)/2}')
				rc_ib_hi="$rc_ib_b"
				rc_ib_drift=$(awk -v x="$rc_ib_a1" -v y="$rc_ib_a2" 'BEGIN{m=(x>y)?x:y; n=(x<y)?x:y; printf "%.1f", (n>0)?100*(m/n-1):999}')
				rc_ib_sep=$(awk -v a="$rc_ib_lo" -v b="$rc_ib_hi" 'BEGIN{m=(a>b)?a:b; n=(a<b)?a:b; printf "%.1f", (n>0)?100*(m/n-1):0}')
				rc_ib_ev="mean keyframe ${rc_ib_lo} B at -3 (legs ${rc_ib_a1}/${rc_ib_a2} B, ${rc_ib_drift}% scene drift between them) vs ${rc_ib_hi} B at +3, ${rc_ib_sep}% apart"
				if fcmp "$rc_ib_sep" ge 15 && ! fcmp "$rc_ib_sep" ge "$(awk -v d="$rc_ib_drift" 'BEGIN{printf "%.1f", 2*d}')"; then
					info "  video1.i_bias_lvl sweep: $rc_ib_ev - over the 15% effect threshold but NOT over twice the drift the two -3 legs measured, so the scene moved as much as the knob supposedly did. Inconclusive; re-run on a stable scene (steady light, no day/night transition) before reading a sign off this"
				elif fcmp "$rc_ib_sep" lt 15; then
					info "  video1.i_bias_lvl sweep: $rc_ib_ev - under the 15% effect threshold. The value reaches the encoder (RC5 proved that) but does not move the bitstream: SetChnQpIPDelta is a no-op in this rc mode on this SoC. Matches the 2026-08-22 T31 measurement; nothing for timps to fix, the value it sends is the value the encoder reports"
				elif fcmp "$rc_ib_lo" gt "$rc_ib_hi"; then
					ok "video1.i_bias_lvl has a real, correctly-signed effect: $rc_ib_ev, and that is more than twice the drift - negative spends MORE bits on keyframes, which is the convention 443584e assumed"
				else
					warn "video1.i_bias_lvl has a real effect with the OPPOSITE sign to the documented one: $rc_ib_ev, and that is more than twice the drift - i.e. POSITIVE spends more bits on keyframes. 443584e passes classic iBiasLvl into iIPDelta 1:1 on the assumption they share a sign convention; this says they do not, and the pass-through needs a negation on this SoC"
				fi
			fi
		fi

		# --- RC6: qp, where "live-capable" and "has an effect" diverge -------
		# 2026-08-22: the new-API SoCs no longer list qp live at all (RC6b
		# measured that path as inert - see the qp note in enc_caps.h), so both
		# this check and RC6b self-gate there now. What follows describes the
		# new-API contract they were written for and is kept for the day a
		# verified SetChnQp() puts qp back in caps.video_live.
		# The new API listed qp as live, but rc_live_apply only writes it under
		# FIXQP (iInitialQP is the fixqp union member; under any other mode the
		# RMW bails and the key is graded deferred). 1bdd1b3 accepted that as
		# honest-but-imprecise grading. Assert BOTH halves of it so the
		# behaviour is pinned rather than assumed: deferred under a non-fixqp
		# mode, live under fixqp. Nothing here switches rc_mode to fixqp to
		# force the second half - that would put the substream into constant-QP
		# for the duration, and this section is not opt-in.
		if ! rc_live_has qp; then
			info "  video1.qp: not in caps.video_live on this $rc_plat build"
			lv_mark_gated video "not-in-caps.video_live-on-$rc_plat" qp
		else
			rc_q_cur=$(jget "$LV_BASE" video.1.qp)
			rc_q_mode=$(jget "$LV_BASE" encoder.1.rc.rc_mode)
			if [ -z "$rc_q_cur" ]; then
				lv_mark_gated video "not-present-in-GET-/control" qp
			else
				rf="$OUTDIR/rc_post_qp.json"; gf="$OUTDIR/rc_read_qp.json"
				rc_q_new=$((rc_q_cur-1))
				LV_PENDING="{\"video\":{\"1\":{\"qp\":$rc_q_cur}}}"
				code=$(lv_post_r "{\"video\":{\"1\":{\"qp\":$rc_q_new}}}" "$rf")
				lv_mark video qp
				lv_get "$gf"
				rc_q_hold=$(jget "$gf" encoder.1.rc.qp)
				if [ "$code" != "200" ]; then
					bad "video1.qp: POST HTTP $code"
				elif [ "$rc_q_mode" = fixqp ]; then
					if rc_defer_has "$rf" video1.qp; then
						bad "video1.qp was DEFERRED although the channel runs in fixqp, the one mode rc_live_apply writes it in - the Get/SetChnAttrRcMode read-modify-write failed"
					elif [ "$rc_q_hold" = "$rc_q_new" ]; then
						ok "video1.qp=$rc_q_new applied LIVE under fixqp and the encoder holds it (encoder.1.rc.qp=$rc_q_hold)"
					else
						bad "video1.qp=$rc_q_new graded applied-live under fixqp but the encoder holds qp=$rc_q_hold"
					fi
				else
					if rc_defer_has "$rf" video1.qp; then
						ok "video1.qp is correctly DEFERRED under rc_mode=$rc_q_mode - qp only reaches the encoder in fixqp, and the grading says so instead of claiming a live apply that would do nothing"
					else
						bad "video1.qp was graded applied-live under rc_mode=$rc_q_mode, but rc_live_apply only writes iInitialQP under FIXQP and bails otherwise - the reply is claiming an apply that did not happen (the grading contract 1bdd1b3 signed off on)"
					fi
				fi
				lv_post "$LV_PENDING" >/dev/null; LV_PENDING=""
			fi
		fi

		# --- RC6b: under fixqp, the QP must actually drive the bitrate -------
		# RC6 grades the CONTRACT (deferred outside fixqp, live inside it) and that
		# stays the point of it - it is the one check that pins 443584e/22832f1's
		# honest-but-imprecise "live-capable" wording. But inside fixqp the same
		# probe can be made to prove something stronger for free: rate control is
		# switched off there, so QP alone decides the bitrate, and a real bitstream
		# measurement at two QP values is a second, independent proof that the live
		# path reaches the ENCODER and not just the struct - the one thing
		# encoder.1.rc.qp cannot tell apart.
		# Reference for the magnitude: the 2026-08-21 T23 investigation put the
		# same scene at 278 kbit/s at a fixed QP of 42 against 990-2091 kbit/s
		# under rate control, so a 17-point QP spread is worth multiples, not
		# percent. 1.5x is a deliberately loose bar for that.
		# Only when the channel is ALREADY in fixqp: rc_mode is restart-bound on
		# T31/C100/T40/T41, and forcing the mode would mean a restart inside a
		# section whose every verdict depends on the daemon NOT restarting
		# (rc_pid_check exists for exactly that). An honest skip beats breaking
		# the premise the rest of the block rests on.
		if [ -z "${rc_q_cur:-}" ] || ! rc_live_has qp; then
			:   # gate-marked by RC6
		elif [ "${rc_q_mode:-}" != fixqp ]; then
			info "  video1.qp: substream runs rc_mode=$rc_q_mode, so the fixed QP is inert and there is no bitrate effect to measure. RC6 above verified the grading contract, which is what IS testable outside fixqp; configure the substream for fixqp to get the measured half"
		elif ! rc_can_measure; then
			skip "video1.qp bitrate-vs-QP measurement needs ffmpeg+ffprobe - not found"
		else
			rc_q_lo=25; rc_q_hi=42        # both inside config.c's 1..51 domain
			rc_q_a=""; rc_q_b=""
			LV_PENDING="{\"video\":{\"1\":{\"qp\":$rc_q_cur}}}"
			lv_mark video qp
			for rc_q_v in "$rc_q_lo" "$rc_q_hi"; do
				code=$(lv_post "{\"video\":{\"1\":{\"qp\":$rc_q_v}}}")
				[ "$code" = "200" ] || { bad "video1.qp=$rc_q_v: POST(live) HTTP $code"; break; }
				sleep "$rc_settle_s"
				rc_r=$(enc_measure "$PATH_SUB" "$rc_meas_dur" "rc6b_$rc_q_v") || continue
				read -r rc_q_kbps _ _ _ _ <<<"$rc_r"
				if [ "$rc_q_v" = "$rc_q_lo" ]; then rc_q_a="$rc_q_kbps"; else rc_q_b="$rc_q_kbps"; fi
			done
			rc_pid_check video1.qp-sweep
			lv_post "$LV_PENDING" >/dev/null; LV_PENDING=""
			if [ -z "$rc_q_a" ] || [ -z "$rc_q_b" ] || ! fcmp "${rc_q_b:-0}" gt 0; then
				warn "video1.qp: could not measure both halves of the fixqp sweep (qp $rc_q_lo=${rc_q_a:-none}, qp $rc_q_hi=${rc_q_b:-none} kbps) - the live qp path stays unverified against the bitstream"
			else
				rc_q_r=$(awk -v a="$rc_q_a" -v b="$rc_q_b" 'BEGIN{printf "%.2f", a/b}')
				if fcmp "$rc_q_r" ge 1.5; then
					ok "video1.qp really drives the bitstream under fixqp: qp $rc_q_lo delivered ${rc_q_a} kbps and qp $rc_q_hi delivered ${rc_q_b} kbps (${rc_q_r}x apart) - lower QP, more bits, applied live with no restart. Independent of encoder.1.rc.qp, so this proves the value reached the ENCODER"
				else
					bad "video1.qp: under fixqp - where QP alone decides the bitrate - qp $rc_q_lo delivered ${rc_q_a} kbps and qp $rc_q_hi delivered ${rc_q_b} kbps (${rc_q_r}x). A 17-point QP spread cannot leave the bitrate flat, so the live write is graded applied (deferred:0) and is NOT reaching the encoder - the 340fb1f/ff28ee2 class, and the reply is lying to the caller about a restart not being needed. Measured on a T31 2026-08-22: the identical value applied at BOOT does work (qp 25 -> 108 kbps, qp 42 -> 17 kbps), so rc_live_apply's Get/SetChnAttrRcMode RMW stores iInitialQP where the next GetChnAttrRcMode reads it back but the running channel never re-programs. See dev_notes/TODO.md"
				fi
			fi
		fi

		# --- RC7: rc_mode, live on classic, restart-bound on the new API -----
		# rc_mode is the one rc key whose live-capability actually differs
		# between the two API generations, and it is a string, so it cannot
		# ride lv_section's generic flip (the "qa_probe" probe value would just
		# be rejected). Probed explicitly, and BOTH outcomes are asserted, not
		# just the interesting one: on classic the whole union is re-filled and
		# the encoder must report the new mode without a restart; on the new API
		# there is no rc-mode setter for a running channel, so the reply must
		# defer it AND the encoder must still be holding the OLD mode. That
		# second half is the part worth having - "persist-only" is only true if
		# nothing quietly reached the encoder anyway.
		rc_m_cur=$(jget "$LV_BASE" video.1.rc_mode)
		rc_m_held=$(jget "$LV_BASE" encoder.1.rc.rc_mode)
		if [ -z "$rc_m_cur" ]; then
			lv_mark_gated video "not-present-in-GET-/control" rc_mode
		else
			# cbr <-> vbr only: both exist on every SoC and neither needs a
			# substitution, so a mismatch below is never the translation table.
			if [ "$rc_m_cur" = cbr ]; then rc_m_new=vbr; else rc_m_new=cbr; fi
			rf="$OUTDIR/rc_post_rc_mode.json"; gf="$OUTDIR/rc_read_rc_mode.json"
			LV_PENDING="{\"video\":{\"1\":{\"rc_mode\":\"$rc_m_cur\"}}}"
			code=$(lv_post_r "{\"video\":{\"1\":{\"rc_mode\":\"$rc_m_new\"}}}" "$rf")
			lv_mark video rc_mode
			lv_get "$gf"
			rc_m_held2=$(jget "$gf" encoder.1.rc.rc_mode)
			if [ "$code" != "200" ]; then
				bad "video1.rc_mode: POST HTTP $code"
			elif [ "$(jget "$gf" video.1.rc_mode)" != "$rc_m_new" ]; then
				bad "video1.rc_mode=$rc_m_new did not round-trip through the config (reads back '$(jget "$gf" video.1.rc_mode)')"
			elif rc_live_has rc_mode; then
				if rc_defer_has "$rf" video1.rc_mode; then
					bad "video1.rc_mode is in caps.video_live on this $rc_plat build but the POST reply DEFERRED it (keys=$(jget "$rf" deferred_keys)) - the classic full-union SetChnAttrRcMode was refused. An H265 stream is refused BY DESIGN here (H264-only per the SDK header); this substream is $(jget "$LV_BASE" video.1.codec), so that exemption $( [ "$(jget "$LV_BASE" video.1.codec)" = h265 ] && echo 'DOES apply - re-read this as expected, not a bug' || echo 'does not apply' )"
				elif [ "$rc_m_held2" = "$(rc_mode_expect "$rc_m_new")" ]; then
					ok "video1.rc_mode $rc_m_cur -> $rc_m_new applied LIVE and the encoder switched with it (encoder.1.rc.rc_mode $rc_m_held -> $rc_m_held2) - no restart"
				else
					bad "video1.rc_mode=$rc_m_new was graded applied-live but the encoder holds rc_mode=$rc_m_held2 (expected $(rc_mode_expect "$rc_m_new")) - SetChnAttrRcMode returned success and the mode did not change"
				fi
			else
				if ! rc_defer_has "$rf" video1.rc_mode; then
					bad "video1.rc_mode is NOT in caps.video_live on this $rc_plat build, yet the POST reply did not defer it (deferred=$(jget "$rf" deferred)) - the reply claims a live apply for a key the platform has no runtime setter for"
				elif [ "$rc_m_held2" != "$rc_m_held" ]; then
					bad "video1.rc_mode was correctly deferred, but the encoder's HELD mode changed anyway ($rc_m_held -> $rc_m_held2) - something wrote the rc union outside the graded live path"
				else
					ok "video1.rc_mode $rc_m_cur -> $rc_m_new persists and is correctly reported DEFERRED on this $rc_plat build; the running encoder is untouched (still holding $rc_m_held2)"
				fi
			fi
			rc_pid_check video1.rc_mode
			lv_post "$LV_PENDING" >/dev/null; LV_PENDING=""
		fi

		# --- RC8a: the CONFIGURED profile must be the one in the bitstream ---
		# RC8 below proves the profile is correctly reported restart-bound. That is
		# a statement about the grading, not about the encoder: it would pass just
		# as happily on a daemon that persists videoN.profile and then hands the
		# SDK something else at bring-up. The SPS says which it is, and ffprobe
		# reads the SPS off the stream header without decoding a single frame - two
		# seconds, no capture, so unlike the rc probes this one is free.
		# Judged against the CURRENT (boot-applied) profile and therefore placed
		# BEFORE RC8's flip - after it the config would name a profile the running
		# encoder legitimately does not have yet.
		# Mapping is hal_ingenic.c's: 0 Baseline / 1 Main / >=2 High for H264, and
		# H265 is pinned to HEVC Main regardless of videoN.profile - so on an H265
		# stream the config value is genuinely not expected to appear, and saying
		# "mismatch" there would be a script bug, not a daemon one.
		rc_pf_now=$(jget "$LV_BASE" video.1.profile)
		rc_pf_codec=$(jget "$LV_BASE" video.1.codec)
		if [ -z "$rc_pf_now" ] || ! have ffprobe; then
			:
		else
			rc_pf_seen=$(timeout -k 5 30 ffprobe -v error -rtsp_transport tcp \
				-select_streams v:0 -show_entries stream=profile -of csv=p=0 \
				"$(rtsp_url "$PATH_SUB")" 2>/dev/null | head -1 | tr -d '\r')
			case "$rc_pf_codec" in
				h265) rc_pf_want="Main";;
				*)    case "$rc_pf_now" in 0) rc_pf_want="Baseline";; 1) rc_pf_want="Main";; *) rc_pf_want="High";; esac;;
			esac
			if [ -z "$rc_pf_seen" ]; then
				warn "video1.profile: ffprobe could not read a profile off the substream SPS - cannot confirm the configured profile reached the encoder"
			elif [ "$rc_pf_seen" = "$rc_pf_want" ]; then
				ok "video1.profile=$rc_pf_now ($rc_pf_codec) is the profile the SUBSTREAM actually advertises - ffprobe reads '$rc_pf_seen' from the SPS, so the configured value reached the encoder and not just the config file"
			else
				bad "video1.profile=$rc_pf_now ($rc_pf_codec) should produce a '$rc_pf_want' bitstream but the substream SPS advertises '$rc_pf_seen' - the profile is persisted and echoed by GET /control while the encoder runs a different one (the 340fb1f class, on the one video key no readback covers: no rc union carries profile). Check enc_create's IMPEncoderProfile mapping against config.c's 0/1/2 domain"
			fi
		fi

		# --- RC8: the restart-bound keys must SAY they are restart-bound -----
		# The deferred list is only worth anything if it is populated as well
		# as emptied. profile is the safest canary in the video block: it is
		# persist-only on every SoC (no rc union carries it, no runtime call
		# exists), it does not touch geometry, buffer counts or the RTSP
		# routing, and the running pipeline is untouched until a restart - so
		# even a run killed with the trap defeated leaves nothing worse than a
		# main-vs-high profile substream after the next reboot.
		rc_pf_cur=$(jget "$LV_BASE" video.1.profile)
		if [ -z "$rc_pf_cur" ]; then
			lv_mark_gated video "not-present-in-GET-/control" profile
		else
			rc_pf_new=$(flip_int 0 2 "$rc_pf_cur")
			rf="$OUTDIR/rc_post_profile.json"
			LV_PENDING="{\"video\":{\"1\":{\"profile\":$rc_pf_cur}}}"
			code=$(lv_post_r "{\"video\":{\"1\":{\"profile\":$rc_pf_new}}}" "$rf")
			lv_mark video profile
			if [ "$code" != "200" ]; then
				bad "video1.profile: POST HTTP $code"
			elif rc_defer_has "$rf" video1.profile; then
				ok "video1.profile=$rc_pf_new is reported DEFERRED (deferred=$(jget "$rf" deferred), keys=$(jget "$rf" deferred_keys)) - a restart-bound key correctly says so"
			else
				bad "video1.profile=$rc_pf_new changed but the POST reply does NOT list it as deferred (deferred=$(jget "$rf" deferred)) - no SoC has a runtime call for the H264 profile, so the reply is telling the caller a restart-bound change is already in effect"
			fi
			# unchanged re-post must report nothing: deferred is a subset of
			# `changed`, same contract as the applied echo (22832f1).
			code=$(lv_post_r "{\"video\":{\"1\":{\"profile\":$rc_pf_new}}}" "$OUTDIR/rc_post_profile_repost.json")
			[ "$(jget "$OUTDIR/rc_post_profile_repost.json" deferred)" = "0" ] \
				&& ok "re-POSTing the same video1.profile reports deferred:0 - the deferred list is a subset of CHANGED, not of accepted" \
				|| bad "re-POSTing an unchanged video1.profile still reports deferred=$(jget "$OUTDIR/rc_post_profile_repost.json" deferred) - deferred must be a subset of 'changed' (22832f1), so an idempotent POST would keep telling a UI a restart is pending"
			lv_post "$LV_PENDING" >/dev/null; LV_PENDING=""
		fi

		# --- RC9: per-channel isolation --------------------------------------
		# 22832f1 claims "strictly per channel ... verified live on hardware
		# with ch0=vbr/2000/q7 vs ch1=cbr/384/q2" - by hand, once. Make it
		# repeatable. Two directions, because they fail differently: a POST
		# that names only ch1 must leave ch0's held attrs byte-identical (a
		# wrong channel index would show up here), and a single POST naming
		# BOTH with DIFFERENT values must land each on its own channel (a
		# last-write-wins bug would show up here, and only here).
		# min_qp is the vehicle: unit-free, exactly comparable, and a +-1
		# nudge on the mainstream is the smallest perturbation that is still
		# observable in the readback.
		if ! rc_live_has min_qp; then
			info "  per-channel isolation: min_qp is not live on this $rc_plat build - would only re-test the config layer, skipped"
		elif [ -z "$(jget "$LV_BASE" encoder.0.rc.min_qp)" ] || [ -z "$(jget "$LV_BASE" encoder.1.rc.min_qp)" ]; then
			skip "per-channel isolation: both channels need a min_qp-carrying rc readback (ch0 mode $(jget "$LV_BASE" encoder.0.rc.rc_mode), ch1 mode $(jget "$LV_BASE" encoder.1.rc.rc_mode)) - not comparable on this camera"
		elif [ "$(jget "$LV_BASE" video.0.min_qp)" -lt 5 ] || [ "$(jget "$LV_BASE" video.1.min_qp)" -lt 5 ]; then
			# the probe nudges DOWNWARD (never above max_qp, whose bound the SDK
			# enforces); config clamps min_qp at 1, so a floor already near it
			# leaves no room for two distinct values
			skip "per-channel isolation: min_qp is already at/near the config floor (ch0 $(jget "$LV_BASE" video.0.min_qp), ch1 $(jget "$LV_BASE" video.1.min_qp)) - no room for two distinct probe values below it"
		else
			rc_i0=$(jget "$LV_BASE" video.0.min_qp); rc_i1=$(jget "$LV_BASE" video.1.min_qp)
			rc_h0=$(jget "$LV_BASE" encoder.0.rc.min_qp)
			rc_restore_both="{\"video\":{\"0\":{\"min_qp\":$rc_i0},\"1\":{\"min_qp\":$rc_i1}}}"
			LV_PENDING="$rc_restore_both"
			# (a) touch ch1 only - ch0 must not move
			lv_post "{\"video\":{\"1\":{\"min_qp\":$((rc_i1-1))}}}" >/dev/null
			gf="$OUTDIR/rc_iso_ch1only.json"; lv_get "$gf"
			rc_h0b=$(jget "$gf" encoder.0.rc.min_qp); rc_h1b=$(jget "$gf" encoder.1.rc.min_qp)
			if [ "$rc_h0b" != "$rc_h0" ]; then
				bad "per-channel isolation: a POST naming ONLY video1.min_qp moved the MAINSTREAM's held bound too (encoder.0.rc.min_qp $rc_h0 -> $rc_h0b) - the live apply is hitting the wrong encoder channel"
			elif [ "$rc_h1b" != "$((rc_i1-1))" ]; then
				bad "per-channel isolation: video1.min_qp=$((rc_i1-1)) did not land on ch1 (encoder.1.rc.min_qp=$rc_h1b)"
			else
				ok "per-channel isolation: a POST naming only video1.min_qp moved ch1 ($rc_h1b) and left ch0 untouched ($rc_h0b)"
			fi
			# (b) both channels, DIFFERENT values, in ONE request
			rc_n0=$((rc_i0-1)); rc_n1=$((rc_i1-2))
			if [ "$rc_n0" = "$rc_n1" ]; then rc_n1=$((rc_n1-1)); fi
			lv_post "{\"video\":{\"0\":{\"min_qp\":$rc_n0},\"1\":{\"min_qp\":$rc_n1}}}" >/dev/null
			gf="$OUTDIR/rc_iso_both.json"; lv_get "$gf"
			rc_g0=$(jget "$gf" encoder.0.rc.min_qp); rc_g1=$(jget "$gf" encoder.1.rc.min_qp)
			if [ "$rc_g0" = "$rc_n0" ] && [ "$rc_g1" = "$rc_n1" ]; then
				ok "per-channel isolation: one POST carrying ch0=$rc_n0 and ch1=$rc_n1 landed each value on its OWN encoder channel (held $rc_g0 / $rc_g1)"
			elif [ "$rc_g0" = "$rc_g1" ]; then
				bad "per-channel isolation: one POST carrying ch0=$rc_n0 and ch1=$rc_n1 left BOTH encoders holding $rc_g0 - last-write-wins across channels, exactly what 22832f1's per-channel claim rules out"
			else
				bad "per-channel isolation: ch0 wanted $rc_n0 holds $rc_g0, ch1 wanted $rc_n1 holds $rc_g1 - the two channels' live rc values are crossing over"
			fi
			rc_pid_check "per-channel isolation"
			lv_post "$rc_restore_both" >/dev/null; LV_PENDING=""
			gf="$OUTDIR/rc_iso_restore.json"; lv_get "$gf"
			[ "$(jget "$gf" encoder.0.rc.min_qp)" = "$rc_h0" ] \
				&& info "  per-channel isolation: both channels restored (ch0 held bound back to $rc_h0)" \
				|| warn "per-channel isolation: ch0's held min_qp is $(jget "$gf" encoder.0.rc.min_qp), expected $rc_h0 after restore"
		fi

		# --- RC10a: quality_lvl, live and measurable on classic only ---------
		# RC10 below round-trips quality_lvl through the config with the rest of
		# the tuning keys, and until now that was its ONLY coverage - which quietly
		# treated it as one uniform persist-only key when it is two different
		# things depending on the SoC generation (enc_caps.h):
		#   classic (T10-T30, incl. T23): part of ENC_LIVE_KEYS, applied to a
		#     RUNNING channel by the full rc-struct re-fill through
		#     SetChnAttrRcMode. It has a real, measured effect there.
		#   new API (T31/C100/T40/T41): no equivalent field in the rc struct at
		#     all. hal_ingenic.c logs "no equivalent field in this SoC's encoder
		#     API - values ignored" once and drops it. Persisting is the whole of
		#     the contract; there is nothing to measure and a measurement that
		#     found no effect would be reporting correct behaviour as a failure.
		# So the two cases are graded apart, on rc_live_has, like every other key
		# in this block.
		#
		# The classic direction comes from the 2026-08-21 T23 investigation
		# (dev_notes/T23_RATECONTROL_INVESTIGATION_2026-08-21.md, summarised in
		# docs/wiki/Rate-Control-Parameters.md): a HIGHER quality_lvl delivers a
		# LOWER bitrate - 1709 kbit/s at level 2 against 1243 at level 7 on the
		# same scene under vbr, -27%, and 1745 vs 1265 under smart. Naming runs
		# against intuition, so the probe asserts the measured direction, not the
		# intuitive one. Within-level spread was 0.2-4% in that investigation,
		# which is what sets the 10% bar below: ~3x the noise, well under the 27%
		# a working knob produced.
		#
		# Gated on the channel ALREADY running vbr or smart: quality_lvl lives in
		# the vbr union member and does nothing under cbr/fixqp. rc_mode is live on
		# classic, so this probe COULD switch the mode itself - it deliberately
		# does not. That would mean holding two live rc changes and two restores
		# open at once on a path nobody has exercised, and an honest skip beats a
		# probe whose failure mode is "left the substream in the wrong rc mode".
		#
		# NOT HARDWARE-VERIFIED. Written against the T23 numbers and the RC4
		# enc_measure() pattern, syntax- and logic-checked only; the only camera
		# available while this was written is a T31, where the branch cannot fire
		# by construction. The first run against a real T10-T30 camera is what
		# validates it - treat a verdict from this block as unproven until then.
		rc_ql_cur=$(jget "$LV_BASE" video.1.quality_lvl)
		rc_ql_mode=$(jget "$LV_BASE" encoder.1.rc.rc_mode)
		if [ -z "$rc_ql_cur" ]; then
			:   # RC10's round-trip reports its absence
		elif ! rc_live_has quality_lvl; then
			info "  video1.quality_lvl: not in caps.video_live on this $rc_plat build - the new-API rc structs have no equivalent field, so the daemon persists it and the HAL logs it as ignored. Config round-trip (RC10) is the entire contract here; there is no encoder behaviour to measure"
		elif ! rc_can_measure; then
			skip "video1.quality_lvl effect measurement needs ffmpeg+ffprobe - not found"
		elif [ "$rc_ql_mode" != vbr ] && [ "$rc_ql_mode" != smart ]; then
			skip "video1.quality_lvl: live on this $rc_plat build, but the substream runs rc_mode=$rc_ql_mode - quality_lvl sits in the vbr union member and has no effect outside vbr/smart. Re-run with the substream in vbr to measure it"
		else
			rc_ql_lo=2; rc_ql_hi=7          # the pair the T23 investigation measured
			rc_ql_a=""; rc_ql_b=""
			LV_PENDING="{\"video\":{\"1\":{\"quality_lvl\":$rc_ql_cur}}}"
			lv_mark video quality_lvl
			for rc_ql_v in "$rc_ql_lo" "$rc_ql_hi"; do
				rf="$OUTDIR/rc_post_quality_lvl_$rc_ql_v.json"
				code=$(lv_post_r "{\"video\":{\"1\":{\"quality_lvl\":$rc_ql_v}}}" "$rf")
				if [ "$code" != "200" ]; then
					bad "video1.quality_lvl=$rc_ql_v: POST(live) HTTP $code"; break; fi
				if rc_defer_has "$rf" video1.quality_lvl; then
					bad "video1.quality_lvl is in caps.video_live on this $rc_plat build but the POST reply DEFERRED it (keys=$(jget "$rf" deferred_keys)) - the classic full-union SetChnAttrRcMode re-fill was refused. An H265 channel is refused BY DESIGN (the classic live path is H264-only); this substream is $(jget "$LV_BASE" video.1.codec)"
					break; fi
				sleep "$rc_settle_s"
				rc_r=$(enc_measure "$PATH_SUB" "$rc_meas_dur" "rc10a_$rc_ql_v") || continue
				read -r rc_ql_kbps _ _ _ _ <<<"$rc_r"
				if [ "$rc_ql_v" = "$rc_ql_lo" ]; then rc_ql_a="$rc_ql_kbps"; else rc_ql_b="$rc_ql_kbps"; fi
			done
			rc_pid_check video1.quality_lvl
			lv_post "$LV_PENDING" >/dev/null; LV_PENDING=""
			if [ -z "$rc_ql_a" ] || [ -z "$rc_ql_b" ] || ! fcmp "${rc_ql_a:-0}" gt 0; then
				warn "video1.quality_lvl: could not measure both levels (lvl $rc_ql_lo=${rc_ql_a:-none}, lvl $rc_ql_hi=${rc_ql_b:-none} kbps) - the effect stays unverified on this camera"
			else
				rc_ql_r=$(awk -v a="$rc_ql_b" -v b="$rc_ql_a" 'BEGIN{printf "%.2f", a/b}')
				if fcmp "$rc_ql_r" le 0.90; then
					ok "video1.quality_lvl really moves the operating point on this $rc_plat build: level $rc_ql_lo delivered ${rc_ql_a} kbps and level $rc_ql_hi delivered ${rc_ql_b} kbps under rc_mode=$rc_ql_mode (${rc_ql_r}x) - higher level, lower bitrate, the direction the 2026-08-21 T23 measurement found (1709 -> 1243 kbit/s)"
				elif fcmp "$rc_ql_r" ge 0.98 && fcmp "$rc_ql_r" le 1.02; then
					bad "video1.quality_lvl: levels $rc_ql_lo and $rc_ql_hi delivered ${rc_ql_a} and ${rc_ql_b} kbps (${rc_ql_r}x) under rc_mode=$rc_ql_mode - indistinguishable. The key is advertised live on this $rc_plat build and the POSTs were graded applied (deferred:0), so it is being accepted, persisted and IGNORED (the 340fb1f/ff28ee2 class). The T23 reference moved 27% on the same pair; suspect the classic full-union re-fill not carrying qualityLvl into the active union member"
				else
					warn "video1.quality_lvl: level $rc_ql_lo delivered ${rc_ql_a} kbps and level $rc_ql_hi delivered ${rc_ql_b} kbps (${rc_ql_r}x) under rc_mode=$rc_ql_mode - the knob moves something, but not the >=10% drop the T23 reference (-27%) leads this check to expect, and not cleanly enough to call it broken either. Scene may be too static for the rate controller to have room; re-run on a busier picture"
				fi
			fi
		fi

		# --- RC10: the remaining videoN tuning keys, config round-trip --------
		# quality_lvl/change_pos/fluc_lvl/gop/max_gop are POST-able (F_CTRL) and
		# were never round-tripped by anything. They carry no restart-crash or
		# session-disruption risk - none of them changes geometry, codec,
		# buffer counts or the RTSP path - so the blanket "too risky to fuzz"
		# exclusion 8d used to apply to the whole video block never fitted
		# them; it fitted width/height/buffers/rtsp_path. fluc_lvl is new in
		# 3edb85e and is H265-only by SDK design, so on an H264 stream it
		# persists with no consumer - which is a documented outcome, not a
		# failure, and the round-trip is still what proves it is wired to the
		# config at all. Ranges are chosen so every midpoint is in-domain and
		# max_gop stays >= gop.
		LV_MODE=persist
		lv_section video1_tuning '{"video":{"1":' '}}' video.1 \
			"quality_lvl int 0 7" "change_pos int 50 100" "fluc_lvl int 0 4" \
			"gop int 25 100" "max_gop int 110 130"
		LV_MODE=live
	fi
	fi

	# --- audio.codec opus (opt-in build feature, 2026-08-04): also persist-only
	# like bitrate above. USE_STREAM_OPUS is a Buildroot-selectable option
	# (BR2_PACKAGE_TIMPS_STREAM_OPUS, default off fleet-wide) distinct from the
	# unrelated USE_PLAY_OPUS (local .opus sound-file playback) feature. When
	# compiled in, pacodec() accepts "opus"; when not, it silently falls through
	# to the AAC default - so the read-back value itself tells us whether this
	# build has the feature, no separate capability flag needed. Either outcome
	# is informational, not a failure: a build without the option is working
	# exactly as intended (default off). Always restore the ORIGINAL codec
	# afterward regardless of outcome - on a non-opus build "opus" quietly
	# becomes "aac", which could silently change a camera's actual audio codec
	# if the restore step were skipped. If the camera is ALREADY running opus
	# (e.g. a manually-tuned test unit), probe in the other direction instead
	# (away to aac and back) so the round-trip is still exercised rather than
	# silently no-op'ing - either direction proves the same thing. ---
	ac_cur=$(jget "$LV_BASE" audio.codec)
	if [ -n "$ac_cur" ]; then
		if [ "$ac_cur" = opus ]; then ac_probe=aac; else ac_probe=opus; fi
		LV_PENDING="{\"audio\":{\"codec\":\"$ac_cur\"}}"   # armed until restore
		code=$(lv_post "{\"audio\":{\"codec\":\"$ac_probe\"}}")
		lv_get "$OUTDIR/lv_opus.json"
		got=$(jget "$OUTDIR/lv_opus.json" audio.codec)
		if [ "$got" = "$ac_probe" ]; then
			ok "persist-only audio.codec=$ac_probe round-trips through config (USE_STREAM_OPUS compiled in, applies on restart)"
		elif [ "$code" = 200 ] && [ "$ac_probe" = opus ]; then
			info "  audio.codec=opus not accepted (got '$got') - USE_STREAM_OPUS not compiled into this build (expected: default off)"
		else
			warn "audio.codec=$ac_probe: got '$got' (HTTP $code)"
		fi
		# ledger: codec was POSTed + read back either way (the non-opus-build
		# outcome is a legitimate result of exercising it, not a skip)
		lv_mark audio codec
		lv_post "{\"audio\":{\"codec\":\"$ac_cur\"}}" >/dev/null   # restore
		lv_get "$OUTDIR/lv_opus_restore.json"
		[ "$(jget "$OUTDIR/lv_opus_restore.json" audio.codec)" = "$ac_cur" ] \
			&& info "  audio.codec: restored to $ac_cur" \
			|| warn "audio.codec: did not restore to original ($ac_cur)"
		LV_PENDING=""
	fi

	# --- shared restart/persist helpers (used by --test-rotation below and by
	# --test-encoder): restarting the daemon, forcing a key into /etc/timps.conf
	# over SSH even when the daemon is down, POST-then-confirm-before-restart,
	# and probing a stream once it is back. Hoisted out of the rotation block so
	# the encoder test reuses this exact machinery instead of growing a second,
	# subtly different copy. They only touch SSH when called.
	rot_restart() {
		sshx "/etc/init.d/S95timps restart >/dev/null 2>&1 || service timps restart >/dev/null 2>&1"
		local i up=0
		for i in $(seq 1 30); do
			sshx "pidof timpsd" >/dev/null 2>&1 && { up=1; break; }
			sleep 2
		done
		[ "$up" = "1" ] || return 1
		# pidof succeeding only proves the PROCESS exists - not that config
		# was durably persisted to /etc/timps.conf before this restart
		# re-read it, nor that the HTTP control server is listening yet.
		# On a freshly-flashed card (still busy with wear-leveling/journal
		# housekeeping) that gap was observed wide enough to race ahead of
		# the daemon: a stale-config video1 false-WARN here, and section
		# 8c's baseline GET false-FAILing right after (both reproduced
		# 2026-08, never under hand-paced SSH timing). Poll for a real
		# /control response before calling the daemon "back" - still
		# bounded, so a genuine hang/crash (control server never binds)
		# falls through to the return 1 below instead of a false pass.
		for i in $(seq 1 15); do
			[ "$(curlq 3 -o /dev/null -w '%{http_code}' "$(http_base)/control")" = "200" ] && return 0
			sleep 2
		done
		return 1
	}
	rot_set_conf() {  # $1=key $2=value -> force into /etc/timps.conf via SSH, works even if the daemon is down
		# Quote like the daemon's own writer does (config.c write_kv_line): the
		# loader strips ONE leading and ONE trailing character when they match,
		# and treats '#' after whitespace as a comment. Writing a bare value
		# here would leave a file the daemon reads back as something else -
		# and this helper runs when the daemon is DOWN, so nothing would
		# rewrite it correctly afterwards. Only values that need it are
		# quoted, so ordinary numeric settings stay readable in the file.
		local _v="$2"
		case "$_v" in
			""|*" "*|*"	"*|*"#"*|*";"*|'"'*|"'"*|*'"'|*"'") _v="\"$2\"";;
		esac
		sshx "grep -q '^$1' /etc/timps.conf 2>/dev/null && sed -i 's|^$1.*|$1 = $_v|' /etc/timps.conf || echo '$1 = $_v' >> /etc/timps.conf"
	}
	rot_apply() {  # $1=JSON body $2=jget path $3=expected value -> POST, then confirm
		# it actually landed before the caller fires a restart. This closes
		# the race from the other end: round-tripping POST->GET both catches
		# a genuine POST failure (instead of silently restarting into a
		# no-op and blaming the restart for it) and, like the hand-paced SSH
		# repro that never failed, naturally paces the POST-then-restart
		# sequence instead of firing both back-to-back.
		local body="$1" path="$2" want="$3" got
		lv_post "$body" >/dev/null
		lv_get "$OUTDIR/lv_rotation_confirm.json"
		got=$(jget "$OUTDIR/lv_rotation_confirm.json" "$path")
		[ "$got" = "$want" ]
	}
	rot_probe() {  # $1=rtsp path -> prints "codecxWIDTHxHEIGHT" or empty on failure
		# pidof succeeding only means the process exists, not that the
		# video pipeline is up yet (on-demand encoder start happens on
		# the FIRST client connection, plus sw-rotate init can add
		# real latency) - a couple of retries beats a fixed sleep.
		local out i
		for i in 1 2 3; do
			out=$(timeout 20 ffprobe -v error -rtsp_transport tcp -select_streams v:0 \
				-show_entries stream=codec_name,width,height -of csv=p=0 \
				"$(rtsp_url "$1")" 2>/dev/null | tr ',' 'x')
			[ -n "$out" ] && { echo "$out"; return; }
			sleep 3
		done
	}

	# --- rotation (opt-in: --test-rotation) ---------------------------------
	# video0.rotation is persist-only like bitrate above, AND SoC-gated:
	# caps.rotation is only present when this build has USE_ROTATE compiled
	# in, and only lists the degree values this SoC's rotation path actually
	# supports (0 always; 90/270 need a dim-swapping apply path - T31/T40/T41
	# hardware or T23 USE_SW_ROTATE; 180 an ISP flip). A build without
	# rotation support simply omits the key - that is not a failure, so this
	# skips cleanly rather than reporting FAIL on unsupported hardware.
	if [ "$TEST_ROTATION" = "1" ]; then
		# caps from $LV_BASE, not section 8's $cj - same --only-8b unbound-
		# variable trap as the spk gate above
		rot_caps=$(jget "$LV_BASE" caps.rotation)
		if [ -z "${rot_caps:-}" ]; then
			info "rotation: caps.rotation absent - not compiled into this build, skipping"
			lv_mark_gated video "caps.rotation-absent(USE_ROTATE-not-built)" rotation
		else
			info "rotation: SoC-supported values: $rot_caps"
			rot_caps_norm=$(echo "$rot_caps" | tr -d ' []')   # "[0, 90, 180, 270]" -> "0,90,180,270"
			rot_cur=$(jget "$LV_BASE" video.0.rotation)
			base_w=$(jget "$LV_BASE" video.0.width)
			base_h=$(jget "$LV_BASE" video.0.height)
			for rv in 90 180 270; do
				case ",$rot_caps_norm," in
				*",$rv,"*)
					lv_post "{\"video\":{\"0\":{\"rotation\":$rv}}}" >/dev/null
					lv_get "$OUTDIR/lv_rotation.json"
					got=$(jget "$OUTDIR/lv_rotation.json" video.0.rotation)
					if [ "$got" = "$rv" ]; then
						ok "persist-only video0.rotation=$rv round-trips through config (applies on restart)"
						if [ "$rv" = "90" ] || [ "$rv" = "270" ]; then
							# 90/270 swap width<->height in ms_vstream_eff_dims();
							# eff_width/eff_height must reflect that swap for the
							# PENDING (persisted, not-yet-restarted) config too.
							eff_w=$(jget "$OUTDIR/lv_rotation.json" video.0.eff_width)
							eff_h=$(jget "$OUTDIR/lv_rotation.json" video.0.eff_height)
							if [ "$eff_w" = "$base_h" ] && [ "$eff_h" = "$base_w" ]; then
								ok "  eff_width/eff_height correctly swapped for rotation=$rv (${eff_w}x${eff_h})"
							else
								bad "  eff dims NOT swapped for rotation=$rv (got ${eff_w}x${eff_h}, want ${base_h}x${base_w})"
							fi
						fi
					else
						bad "persist-only video0.rotation=$rv did not persist (got '$got', want '$rv')"
					fi
					;;
				esac
			done
			lv_post "{\"video\":{\"0\":{\"rotation\":${rot_cur:-0}}}}" >/dev/null   # restore
			# ledger: rotation counts as exercised only if the caps list held a
			# value the loop above could actually POST (a [0]-only SoC posts
			# nothing and must not be attested)
			case ",$rot_caps_norm," in
				*",90,"*|*",180,"*|*",270,"*) lv_mark video rotation;;
				*) lv_mark_gated video "caps.rotation-lists-only-0" rotation;;
			esac

			# --- real rotation verification (needs --ssh: requires an actual
			# daemon restart, since rotation is persist-only) ------------------
			# The config round-trip above only proves the API layer; it can't
			# catch a real hardware/encoder-init failure. This exact class of
			# bug shipped once: a T23 SW-rotate config that round-tripped fine
			# through /control but crashed the WHOLE daemon on restart via a
			# failed IMP_Encoder_YuvInit (no video/audio at all until someone
			# manually fixed the config file by hand). This restarts the
			# daemon for real with two configs and asserts it survives:
			#   1. video0 (mainstream) at its CURRENT native resolution/fps,
			#      rotated - reproduces whatever real risk THIS camera's
			#      actual config poses. Successfully-rotated OR safely-
			#      refused-and-unrotated are BOTH a pass; a dead/unreachable
			#      daemon is the only fail.
			#   2. video1 (substream) forced to a known-16-aligned safe size
			#      (704x576@10fps, within the documented safe envelope) -
			#      this one SHOULD actually rotate; if it doesn't, warn
			#      (not fail) since sw-rotate genuinely not working on a
			#      "safe" config is a real product gap, not a crash.
			# Restore is done via direct SSH config-file edit + restart, NOT
			# via the HTTP API - if case 1 crashes the daemon, /control is
			# unreachable, so the API-based restore above would silently no-op
			# and leave the bad value on disk for case 2 (and beyond) to
			# inherit. Editing /etc/timps.conf directly works whether or not
			# the daemon is currently up.
			if [ -n "$SSH_TARGET" ]; then
				echo "  -- real restart test (needs --ssh, may take ~1-2 min) --"
				sub1_cur=$(jget "$LV_BASE" video.1.rotation);  sub1_cur=${sub1_cur:-0}
				sub1_w=$(jget "$LV_BASE" video.1.width);       sub1_w=${sub1_w:-640}
				sub1_h=$(jget "$LV_BASE" video.1.height);      sub1_h=${sub1_h:-360}
				sub1_fps=$(jget "$LV_BASE" video.1.fps);       sub1_fps=${sub1_fps:-25}

				if echo ",$rot_caps_norm," | grep -q ",90,"; then
					# case 1: mainstream at native size, rotated - the crash-class check
					if rot_apply "{\"video\":{\"0\":{\"rotation\":90}}}" video.0.rotation 90; then
						if rot_restart; then
							probe0=$(rot_probe "$PATH_MAIN")
							if [ -n "$probe0" ]; then
								ok "rotation: daemon survived + streamed video0 @ native res with rotation=90 requested ($probe0)"
							else
								bad "rotation: daemon came up but video0 stream unreachable after rotation=90 at native res"
							fi
						else
							bad "rotation: daemon DID NOT SURVIVE restart with video0 rotation=90 at native res - this is the crash class this test exists to catch"
						fi
					else
						bad "rotation: POST video0.rotation=90 did not persist before restart - skipping restart test (config layer itself rejected/dropped the value)"
					fi
					rot_set_conf "video0.rotation" "${rot_cur:-0}"
					rot_restart >/dev/null 2>&1

					# case 2: substream forced to a known-16-aligned safe size, rotated
					if rot_apply "{\"video\":{\"1\":{\"rotation\":90,\"width\":704,\"height\":576,\"fps\":10}}}" video.1.rotation 90; then
						if rot_restart; then
							probe1=$(rot_probe "$PATH_SUB")
							if echo "$probe1" | grep -q "576x704"; then
								ok "rotation: video1 at a 16-aligned safe size (704x576@10fps) ACTUALLY rotated ($probe1)"
							elif [ -n "$probe1" ]; then
								warn "rotation: video1 survived + streamed but did NOT rotate as expected (got $probe1, want 576x704) - sw-rotate may not be functional on this SoC even within the documented safe envelope"
							else
								bad "rotation: daemon came up but video1 stream unreachable after a known-safe rotated config"
							fi
						else
							bad "rotation: daemon DID NOT SURVIVE restart with a known-safe-aligned video1 rotation config"
						fi
					else
						bad "rotation: POST video1 rotation=90/704x576/10fps did not persist before restart - skipping restart test (config layer itself rejected/dropped the value)"
					fi
					rot_set_conf "video1.rotation" "$sub1_cur"
					rot_set_conf "video1.width" "$sub1_w"
					rot_set_conf "video1.height" "$sub1_h"
					rot_set_conf "video1.fps" "$sub1_fps"
					rot_restart >/dev/null 2>&1 || warn "rotation: final restore-restart didn't come back within 60s on $SSH_TARGET - check the camera manually"
				else
					info "rotation: no 90/270 support on this SoC - skipping real restart test"
				fi
			else
				info "rotation: real restart verification needs --ssh (config round-trip above already checked)"
			fi
		fi
	else
		# --test-rotation off (the default in EVERY profile): rotation is not
		# exercised, and 8d must say so instead of attesting it - this exact
		# misattestation ("all F_CTRL fields tested" while rotation was never
		# POSTed) shipped in every standard run until 2026-08-18
		lv_mark_gated video "opt-in---test-rotation-not-set" rotation
	fi

	# --- 8g. Encoder settings, verified by MEASURING the substream ----------
	# (opt-in: --test-encoder, substream only)
	#
	# Everything else in 8b proves the config layer, and the rc block above
	# proves what the encoder HOLDS. Neither proves what comes down the wire -
	# and "held" is still an SDK struct, not a bitstream. This project's
	# most-repeated bug shape is a value accepted, persisted, faithfully echoed
	# and then ignored or silently coerced by the encoder (340fb1f, ff28ee2,
	# f003655, 0a8bb9f, 6ec766e, dd2221f, 51bf052, 30ecc74), so the measurement
	# stays, and it stays the only verdict that can catch a readback that
	# agrees with itself while the bitstream does something else.
	#
	# What changed with 22832f1: the way the new value gets INTO the encoder is
	# no longer always a restart. This block was written when every videoN.* key
	# was persist-only, so it hard-coded rot_apply+rot_restart around every
	# measurement. Now the route is per key and per platform, and the harness
	# picks it from caps.video_live:
	#   listed live  -> POST and measure. No restart, no downtime, ~2 min saved
	#                   per key, and - the actual point - it is the only way to
	#                   exercise the live code path at all, which no restart
	#                   measurement can ever reach.
	#   not listed   -> the original persist+restart+measure path, unchanged.
	#                   It is not legacy: it is the regression test for every
	#                   key and platform that stays restart-bound, and the only
	#                   check that the BOOT fill still honours the config.
	# Both routes end in the same enc_measure() comparison, so the verdicts
	# below stay comparable across platforms; only the "how it got there" line
	# differs, and it is printed.
	#
	# Substream only, on purpose: it carries the same encoder-config path as
	# the main stream at a fraction of the disruption if a restart goes badly.
	if [ "${TEST_ENCODER:-0}" = "1" ]; then
		# --ssh is only unavoidable on the restart route. When the platform
		# applies bitrate live there is nothing to restart and nothing to write
		# into /etc/timps.conf behind the daemon's back, so requiring SSH there
		# would refuse to run the one path that has never been measured.
		case "$(jget "$LV_BASE" caps.video_live)" in
			*'"bitrate"'*) enc_bitrate_live=1;;
			*)             enc_bitrate_live=0;;
		esac
		if [ -z "$SSH_TARGET" ] && [ "$enc_bitrate_live" = 0 ]; then
			skip "encoder verification needs --ssh: caps.video_live does not list bitrate on this build, so a daemon restart is the only way to get a new target into the encoder"
		else
			# enc_commit <json-body> <jget-path> <expected-value>
			# Get a value into the RUNNING encoder by whichever route this
			# platform offers, and SAY which one was taken - the reply's own
			# "deferred" grading decides, not a guess from the SoC. 0 = the
			# encoder has it and a measurement is meaningful, 1 = it does not
			# and the caller must not measure.
			enc_commit() {
				local body="$1" path="$2" want="$3" got d
				local rf="$OUTDIR/enc_commit_post.json" gf="$OUTDIR/enc_commit_get.json"
				lv_post_r "$body" "$rf" >/dev/null
				lv_get "$gf"
				got=$(jget "$gf" "$path")
				if [ "$got" != "$want" ]; then
					bad "encoder: POST $body did not persist ($path='$got', want '$want') - the config layer rejected or dropped it before the encoder could be reached"
					return 1
				fi
				d=$(jget "$rf" deferred)
				if [ "${d:-1}" = "0" ]; then
					ENC_ROUTE=live
					# 22832f1: a live rc write "takes effect at the next IDR/GOP",
					# and that latency is one of the things it left unverified.
					# Settle for a few GOPs before measuring so the window is not
					# polluted by the tail of the OLD rate - and so a verdict of
					# "did not follow" means the encoder never followed, not that
					# it had not got round to it yet.
					enc_settle=$(awk -v g="$(jget "$gf" video.1.gop)" -v f="$(jget "$gf" video.1.fps)" \
						'BEGIN{ s=(f>0&&g>0)? 3*g/f : 6; if(s<4)s=4; if(s>20)s=20; printf "%.0f", s }')
					info "  encoder: $path=$want went in LIVE (deferred:0) - no restart; settling ${enc_settle}s (~3 GOPs) for the next-IDR latency, then measuring the actual bitstream, which is the first check to judge the 22832f1 runtime path"
					sleep "$enc_settle"
					return 0
				fi
				if [ -z "$SSH_TARGET" ]; then
					skip "encoder: $path=$want came back deferred (keys=$(jget "$rf" deferred_keys)) and there is no --ssh to restart with - cannot measure this step"
					return 1
				fi
				ENC_ROUTE=restart
				info "  encoder: $path=$want is restart-bound here (deferred_keys=$(jget "$rf" deferred_keys)) - restarting for real to make it effective (~1-2 min)"
				rot_restart || { bad "encoder: the daemon did not come back after a restart with $path=$want"; return 1; }
				return 0
			}
			ENC_ROUTE=restart   # overwritten per enc_commit; names the route the verdicts judged
			# enc_measure is now a shared helper (see its definition near
			# ffwarn_count) - RC4 needs the identical real-bitstream
			# measurement for the live-apply path and duplicating it here
			# drifting silently apart from RC4's copy is exactly the failure
			# class FFWARN_RE's own comment warns about.
			enc_dur="${ENC_DUR:-25}"
			enc_bcur=$(jget "$LV_BASE" video.1.bitrate)
			enc_rcur=$(jget "$LV_BASE" video.1.rc_mode)
			if [ -z "${enc_bcur:-}" ]; then
				skip "encoder verification: video.1.bitrate not reported by /control"
			else
				# A bitrate TARGET is a ceiling, not a promise: it only binds when
				# the scene (and the videoN.min_qp floor) would otherwise spend MORE
				# than it. Raising the target on a stream that is already quality- or
				# content-limited changes nothing, and the old "is the measured value
				# closer to the new number or to the old one?" heuristic then FAILS a
				# perfectly healthy encoder. That is exactly what happened on a T31
				# whose 640x360 substream sat at ~238 kbps (I-frames pinned at the
				# min_qp=20 floor, P-frames all-skip on a static scene): 384 -> 1500
				# could not possibly move it, in a working OR a broken daemon.
				#
				# So: measure what the stream ACTUALLY delivers under the current
				# config first, then aim the new target well BELOW that. A lower
				# ceiling always binds, which makes "the encoder followed it" and
				# "the encoder ignored it" genuinely distinguishable.
				enc_pre_kbps=""; cv_pre=""
				if res0=$(enc_measure "$PATH_SUB" "$enc_dur" pre); then
					read -r enc_pre_kbps cv_pre _ _ _ <<<"$res0"
					info "  encoder test: substream currently DELIVERS ${enc_pre_kbps} kbps at a configured ${enc_bcur} kbps (per-second CV ${cv_pre})"
				fi
				if [ -n "${enc_pre_kbps:-}" ] && fcmp "$enc_pre_kbps" ge 64; then
					# 0.4x of what it delivers right now, floored at the config
					# layer's own minimum (videoN.bitrate clamps to >= 16)
					enc_bnew=$(awk -v m="$enc_pre_kbps" 'BEGIN{v=int(m*0.4); if(v<16)v=16; print v}')
					enc_dir=down
				else
					# no usable reference (stream unreachable, or already so low that
					# 0.4x lands under the clamp): fall back to the old distinct-value
					# probe - but an unprovable direction must not produce a hard FAIL,
					# see the verdict below.
					enc_bnew=$(awk -v c="$enc_bcur" 'BEGIN{ print (c>700)? 400 : 1500 }')
					enc_dir=blind
				fi
				info "  encoder test: substream bitrate ${enc_bcur} -> ${enc_bnew} kbps (aiming ${enc_dir}), rc_mode ${enc_rcur:-?} -> cbr"
				# Two enc_commit steps instead of one combined POST+restart.
				# Splitting them is what lets the bitrate measurement exercise the
				# LIVE path on a platform where bitrate is live but rc_mode is not
				# (every new-API SoC): one combined POST would carry a deferred
				# rc_mode along with it, drag the whole request onto the restart
				# route, and leave the runtime call untested by the only check in
				# this script that measures the actual bitstream.
				if ! enc_commit "{\"video\":{\"1\":{\"rc_mode\":\"cbr\"}}}" video.1.rc_mode cbr; then
					:   # enc_commit already said what went wrong
				elif ! enc_commit "{\"video\":{\"1\":{\"bitrate\":$enc_bnew}}}" video.1.bitrate "$enc_bnew"; then
					:
				else
					if res=$(enc_measure "$PATH_SUB" "$enc_dur" cbr); then
						read -r m_kbps cv_cbr _ _ _ <<<"$res"
						info "  measured substream: ${m_kbps} kbps over ${enc_dur}s (requested ${enc_bnew}, previously ${enc_bcur}), per-second CV ${cv_cbr}"
						hi=$(awk -v n="$enc_bnew" 'BEGIN{printf "%.0f", n*1.5}')
						if [ "$enc_dir" = down ]; then
							# the new ceiling is 0.4x of the measured delivered rate, so a
							# working encoder MUST come down to meet it.
							keep=$(awk -v b="$enc_pre_kbps" 'BEGIN{printf "%.0f", b*0.8}')
							if fcmp "$m_kbps" le "$hi"; then
								ok "encoder: the daemon actually FOLLOWS the substream bitrate target via the ${ENC_ROUTE} path - delivered ${enc_pre_kbps} -> ${m_kbps} kbps after the target was cut to ${enc_bnew} (within 1.5x of it)"
							elif fcmp "$m_kbps" ge "$keep"; then
								bad "encoder: substream still delivers ${m_kbps} kbps (it delivered ${enc_pre_kbps} before) after the target was cut to ${enc_bnew} via the ${ENC_ROUTE} path - the value was accepted, persisted and echoed but the encoder is ignoring it (the 340fb1f/ff28ee2 class). On the live path this is 22832f1s SetChnBitRate returning success without effect, which no readback can see"
							else
								warn "encoder: delivered ${m_kbps} kbps moved toward the requested ${enc_bnew} (from ${enc_pre_kbps}) but did not get within 1.5x of it - rate control is loose on this SoC/scene"
							fi
						else
							# blind (upward) probe: a raised ceiling only shows up if the
							# scene actually wants the bits, so neither outcome PROVES
							# anything. Report, never fail.
							lo=$(awk -v n="$enc_bnew" 'BEGIN{printf "%.0f", n*0.6}')
							if fcmp "$m_kbps" ge "$lo" && fcmp "$m_kbps" le "$hi"; then
								ok "encoder: the daemon actually DELIVERS the requested substream bitrate via the ${ENC_ROUTE} path (${m_kbps} kbps vs requested ${enc_bnew}, within 0.6-1.5x)"
							else
								warn "encoder: delivered ${m_kbps} kbps against a requested ${enc_bnew} (was configured ${enc_bcur}) - could not measure the stream beforehand, so this cannot distinguish an ignored target from a scene/min_qp-limited one; re-run when the pre-measurement succeeds"
							fi
						fi
						# --- does rc_mode do anything measurable? -------------
						if enc_commit "{\"video\":{\"1\":{\"rc_mode\":\"vbr\"}}}" video.1.rc_mode vbr; then
							if res2=$(enc_measure "$PATH_SUB" "$enc_dur" vbr); then
								read -r m2_kbps cv_vbr _ _ _ <<<"$res2"
								info "  measured substream under vbr: ${m2_kbps} kbps, per-second CV ${cv_vbr} (cbr was ${cv_cbr})"
								if fcmp "$cv_cbr" le 0 || fcmp "$cv_vbr" le 0; then
									warn "encoder: could not measure per-second bitrate variance in one of the modes - rc_mode effect unproven"
								else
									r=$(awk -v a="$cv_vbr" -v b="$cv_cbr" 'BEGIN{printf "%.2f", a/b}')
									if fcmp "$r" ge 1.3; then
										ok "encoder: rc_mode has a REAL effect - vbr's per-second bitrate varies ${r}x more than cbr's"
									elif fcmp "$r" le 0.77; then
										warn "encoder: vbr is measurably STEADIER than cbr (${r}x) - unexpected, but rc_mode is demonstrably doing something"
									else
										# same trap as the bitrate verdict above: if BOTH modes sit
										# far under their shared target, rate control has nothing to
										# do in either mode and the two are identical by physics,
										# not by a bug.
										if fcmp "$m_kbps" le "$(awk -v n="$enc_bnew" 'BEGIN{printf "%.0f", n*0.5}')" && \
											fcmp "$m2_kbps" le "$(awk -v n="$enc_bnew" 'BEGIN{printf "%.0f", n*0.5}')"; then
											info "  cbr and vbr look alike (${r}x, CV ${cv_cbr} vs ${cv_vbr}), but BOTH deliver under half the ${enc_bnew} kbps target - the stream is quality/content-limited (videoN.min_qp floor, static scene), so rate control has nothing to express in either mode. rc_mode effect unproven, not disproven."
										else
											# 2026-08-18: on T31/libimp 1.1.6 the two modes were traced
											# through the (unstripped) vendor library. IMP_Encoder_SetDefaultParam's
											# CBR and VBR fills differ in exactly ONE field - VBR's
											# uMaxBitRate = 4/3 x target - while initial QP, QP bounds,
											# eRcOptions and uMaxPictureSize are bit-identical; timps then
											# imposes the same videoN.min_qp/max_qp window in both modes.
											# 33% of peak headroom is simply less than this test can
											# resolve. So a small ratio here does NOT show the encoder
											# ignoring rc_mode, and the wording must not claim it does -
											# see dev_notes / the rc_mode analysis. Report it as
											# unresolved, which is what it is.
											warn "encoder: cbr and vbr bitrate variance too close to separate (${r}x, CV ${cv_cbr} vs ${cv_vbr}) - inconclusive, NOT evidence that rc_mode is ignored: on some SoCs the two modes differ only in peak headroom (VBR uMaxBitRate), which this metric cannot resolve. Verify via the encoder's own debug log (it dumps rcMode + attrVbr.uMaxBitRate at channel setup) rather than by measurement."
										fi
									fi
								fi
							else
								warn "encoder: no substream data captured under rc_mode=vbr - cannot compare rate-control behaviour"
							fi
						else
							bad "encoder: could not get rc_mode=vbr into the encoder (neither live nor via a restart) for the rate-control comparison"
						fi
					else
						bad "encoder: no substream data after applying bitrate=$enc_bnew via the ${ENC_ROUTE} path - the new encoder config may have broken the stream entirely"
					fi
				fi
				# --- surface the daemon's own encoder telemetry ---------------
				# control.c:1210-1231 has published left_pics/work_done/
				# ave_bitrate (IMP_Encoder_Query) all along and nothing has ever
				# looked at it. Informational: a growing left_pics is the
				# encoder falling behind its consumers.
				ej="$OUTDIR/enc_telemetry.json"
				if curlq 10 "$(http_base)/control" -o "$ej" && [ -s "$ej" ]; then
					for ch in 0 1; do
						lp=$(jget "$ej" "encoder.$ch.left_pics")
						[ -n "$lp" ] || continue
						info "  encoder telemetry chn$ch: left_pics=$lp work_done=$(jget "$ej" "encoder.$ch.work_done") cur_packs=$(jget "$ej" "encoder.$ch.cur_packs") ave_bitrate=$(jget "$ej" "encoder.$ch.ave_bitrate")"
						# what the encoder HOLDS after all of the above, next to
						# what came down the wire. On a live run this is the only
						# place the two are printed side by side, which is exactly
						# the diff c4e434f was written to make possible.
						rcm=$(jget "$ej" "encoder.$ch.rc.rc_mode")
						[ -n "$rcm" ] && info "    held rc: mode=$rcm bitrate=$(jget "$ej" "encoder.$ch.rc.bitrate") min_qp=$(jget "$ej" "encoder.$ch.rc.min_qp") max_qp=$(jget "$ej" "encoder.$ch.rc.max_qp") ip_delta=$(jget "$ej" "encoder.$ch.rc.ip_delta")"
					done
				fi
				# --- restore. With SSH, straight into /etc/timps.conf the way the
				# rotation test does, so it lands even if the daemon is unwell.
				# Without SSH (only reachable when the whole run went down the live
				# path, so the daemon is demonstrably healthy) a plain POST is both
				# the restore AND the re-apply, and there is nothing to restart.
				if [ -n "$SSH_TARGET" ]; then
					rot_set_conf "video1.bitrate" "$enc_bcur"
					rot_set_conf "video1.rc_mode" "${enc_rcur:-cbr}"
				else
					lv_post "{\"video\":{\"1\":{\"bitrate\":$enc_bcur,\"rc_mode\":\"${enc_rcur:-cbr}\"}}}" >/dev/null
				fi
				if [ -z "$SSH_TARGET" ] || rot_restart; then
					rj="$OUTDIR/enc_restore.json"; lv_get "$rj"
					if [ "$(jget "$rj" video.1.bitrate)" = "$enc_bcur" ] && [ "$(jget "$rj" video.1.rc_mode)" = "${enc_rcur:-cbr}" ]; then
						ok "encoder: restored substream bitrate=${enc_bcur} rc_mode=${enc_rcur:-cbr} and the daemon is healthy"
					else
						warn "encoder: restore did not read back as expected (bitrate=$(jget "$rj" video.1.bitrate), rc_mode=$(jget "$rj" video.1.rc_mode)) - check the camera's video1 settings by hand"
					fi
				else
					bad "encoder: the daemon did not come back after the final restore-restart - check $SSH_TARGET by hand (video1 settings were written to /etc/timps.conf)"
				fi
			fi
		fi
	fi

	fi
fi

fi
if want 8c osdvars osd-vars; then
# --- 8c. OSD custom placeholder (vars_file) round-trip ----------------------
hdr "8c. OSD custom placeholder (vars_file)"
if [ -z "$SSH_TARGET" ]; then
	info "OSD vars_file round-trip needs --ssh (writes a probe file on-device) - skipped"
elif ! have python3; then
	skip "OSD vars_file round-trip needs python3 (JSON read-back) - not found"
else
	OV_FILE="$OSD_VARS_FILE"
	OV_NAME="qa_probe_$$"
	OV_VAL="qa_$(date +%s)"
	OV_BASE="$OUTDIR/osdvars_base.json"
	if ! curlq 12 "$(http_base)/control" -o "$OV_BASE" || [ ! -s "$OV_BASE" ]; then
		bad "OSD vars_file test: cannot GET /control baseline - skipping"
	else
		# find the first stream/item with a live text field (default layout has
		# items 0-3 enabled at startup; 4-7 are disabled and restart-only to
		# enable, so skip those rather than trying to bring one up live)
		OV_STREAM=""; OV_ITEM=""; OV_ORIG=""
		for s in 0 1; do
			for i in 0 1 2 3; do
				v=$(jget "$OV_BASE" "osd$s.$i.text")
				if [ -n "$v" ]; then OV_STREAM=$s; OV_ITEM=$i; OV_ORIG="$v"; break 2; fi
			done
		done
		if [ -z "$OV_ITEM" ]; then
			warn "OSD vars_file test: no live text item found in /control status - skipping"
		else
			# Interruption safety, same pattern as section 8b: track the pending
			# restore and flush it from EXIT/INT/TERM.
			OV_PENDING=""
			ov_restore_pending() {
				[ -n "${OV_PENDING:-}" ] || return 0
				warn "interrupted mid OSD-vars test - restoring camera OSD text + probe file"
				curl -s -o /dev/null --max-time 12 -u "$HTTP_USER:$HTTP_PASS" \
					-X POST "$(http_base)/control" \
					-d "{\"osd$OV_STREAM\":{\"$OV_ITEM\":{\"text\":\"$OV_PENDING\"}}}" >/dev/null 2>&1 || true
				sshx "grep -v '^$OV_NAME ' '$OV_FILE' > '$OV_FILE.qa_tmp' 2>/dev/null && mv '$OV_FILE.qa_tmp' '$OV_FILE'" >/dev/null 2>&1 || true
				OV_PENDING=""
			}
			trap 'ov_restore_pending' EXIT
			trap 'ov_restore_pending; trap - INT;  kill -INT  $$'  INT
			trap 'ov_restore_pending; trap - TERM; kill -TERM $$' TERM
			OV_PENDING="$OV_ORIG"

			# Write the probe line the documented safe way: temp file + mv
			# (atomic rename), so this test doesn't itself demonstrate the
			# torn-read hazard it's implicitly guarding against.
			if ! sshx "echo '$OV_NAME = $OV_VAL' > '$OV_FILE.qa_tmp' && mv '$OV_FILE.qa_tmp' '$OV_FILE'"; then
				bad "OSD vars_file test: could not write probe file via SSH ($OV_FILE)"
			else
				code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 12 -u "$HTTP_USER:$HTTP_PASS" \
					-X POST "$(http_base)/control" \
					-d "{\"osd$OV_STREAM\":{\"$OV_ITEM\":{\"text\":\"{$OV_NAME}\"}}}")
				if [ "$code" != "200" ]; then
					bad "OSD vars_file test: POST(text={$OV_NAME}) HTTP $code"
				else
					sleep 2   # >= 1 OSD refresh tick (~1x/s)
					gf="$OUTDIR/osdvars_new.json"; curlq 12 "$(http_base)/control" -o "$gf"
					got=$(jget "$gf" "osd$OV_STREAM.$OV_ITEM.text")
					if [ "$got" = "{$OV_NAME}" ]; then
						ok "OSD vars_file: custom placeholder template stored & read back (osd$OV_STREAM.$OV_ITEM.text={$OV_NAME})"
						info "  wrote $OV_NAME=$OV_VAL to $OV_FILE via SSH; this only confirms the template round-trips through /control, not that the rendered pixels are correct - grab a snapshot yourself for visual confirmation if needed"
					else
						bad "OSD vars_file: template not applied (got '$got', want '{$OV_NAME}')"
					fi
				fi
				# restore original text and remove the probe line, regardless of
				# the outcome above
				curl -s -o /dev/null --max-time 12 -u "$HTTP_USER:$HTTP_PASS" \
					-X POST "$(http_base)/control" \
					-d "{\"osd$OV_STREAM\":{\"$OV_ITEM\":{\"text\":\"$OV_ORIG\"}}}" >/dev/null
				sshx "grep -v '^$OV_NAME ' '$OV_FILE' > '$OV_FILE.qa_tmp' 2>/dev/null && mv '$OV_FILE.qa_tmp' '$OV_FILE'" >/dev/null 2>&1
				OV_PENDING=""
				rf="$OUTDIR/osdvars_restore.json"; curlq 12 "$(http_base)/control" -o "$rf"
				rgot=$(jget "$rf" "osd$OV_STREAM.$OV_ITEM.text")
				if [ "$rgot" = "$OV_ORIG" ]; then info "  restored original text ('$OV_ORIG')"
				else warn "OSD vars_file test: original text not confirmed restored (got '$rgot', want '$OV_ORIG')"; fi
			fi
			trap - EXIT INT TERM
		fi
	fi
fi
fi
if want 8d fields fieldinventory; then
# --- 8d. Field-inventory drift check (Finding #1) ---------------------------
# scripts/timps-qa.sh's own section 8b used to enumerate every live-settable
# field with its OWN hand-written spec list (the lv_section "key type lo hi"
# calls above) - structurally the exact same pattern as the 13 hand-written
# per-section arrays just deleted from config.c/control.c
# (apply_ctrl_fields()'s doc comment). A field added to config.c with F_CTRL
# was silently never tested here unless someone remembered to ALSO update
# this script - and that had already happened (whole POST-able sections
# missing from 8b's coverage: osd.* globals, motion.hold_ms/skip_frames, most
# audio.* persist keys - all fixed in 8b above by this same change).
#
# This can't eliminate the hand-maintained list on the SCRIPT side (something
# has to say "8b's code tests these"), but it CAN stop it from drifting
# silently: GET /control?fields=1 (control_fields_json() in control.c) is the
# daemon's own authoritative F_CTRL inventory, walking the identical tables
# apply_ctrl_fields() applies from - so diffing our tested-set + documented
# allowlist against it turns any FUTURE gap into a loud warn instead of a
# silent one. Keep TESTED_*/ALLOW_* below in sync whenever a lv_section call
# above changes; that is now the ONLY place this can go stale unnoticed.
hdr "8d. Field-inventory drift check (GET /control?fields=1 vs 8b coverage)"
fj="$OUTDIR/fields.json"
if ! curlq 10 "$(http_base)/control?fields=1" -o "$fj" || [ ! -s "$fj" ]; then
	warn "field-inventory check: cannot GET /control?fields=1 - skipping (older build without this endpoint?)"
elif grep -qF '"caps"' "$fj" || ! grep -qF '"image":[' "$fj"; then
	# Shape guard: a pre-708ea08 daemon doesn't recognize the ?fields=1 query
	# param at all and just serves the normal GET /control status document
	# instead (200 OK, non-empty - the curlq check above passes fine, so the
	# branch above never fires). That document is a completely different
	# shape from control_fields_json()'s output: it has a top-level "caps"
	# key (the field-inventory doc never emits one), and its per-section
	# values are OBJECTS ("image":{"brightness":..}, "video":{"0":{...}})
	# rather than flat arrays of field names ("image":["brightness",...]).
	# Without this guard, jarr() below would happily iterate the normal
	# document's object KEYS as if they were F_CTRL field names - which is
	# exactly how read-only status keys like motion.available/video.0 got
	# misreported as "POST-able" drift (confirmed: 32 false warnings against
	# a real camera running an older build). Checking for "image":[ (array
	# form) plus absence of "caps" catches this before the diff ever runs -
	# same "older build lacks this capability" skip already used for SRT
	# (section 4b, srt.available=0) and ONVIF (port-closed) above.
	skip "field-inventory drift check: GET /control?fields=1 returned the normal status document, not the field-inventory shape (has a \"caps\" key and/or \"image\" isn't an array) - this daemon doesn't recognize ?fields=1 (older build, pre-708ea08) - skipping"
else
	# TESTED_<section>: the field names 8b's CODE contains a probe for
	# (live or persist-only). Since 2026-08-18 this is only the DECLARED map:
	# what 8b really did in THIS run comes from the runtime ledger
	# (lv_posted.txt / lv_gated.txt, written by 8b itself) and the verdicts
	# below judge against that, not against these lists. The lists earn their
	# keep as the promise side of the comparison: a field listed here that
	# the ledger shows neither posted nor gated is a STALE ATTESTATION -
	# exactly the failure that let "all F_CTRL fields tested" be reported
	# while rotation (opt-in-only) and spk_*/aec (caps-gated) were never
	# POSTed (both observed live, twice, on 2026-08-18).
	# rotation and spk_*/aec stay listed: their gates record them in
	# lv_gated.txt with a reason when they are off, which satisfies the
	# promise without attesting a test that never ran.
	TESTED_image="brightness contrast saturation sharpness hue vflip hflip running_mode anti_flicker ae_compensation max_again max_dgain sinter_strength temper_strength dpc_strength defog_strength drc_strength highlight_depress backlight_compensation core_wb_mode wb_rgain wb_bgain"
	TESTED_audio="volume gain alc_gain mute spk_volume spk_gain aec codec enabled samplerate channels bitrate high_pass agc ns agc_target_dbfs agc_compression_db force_stereo spk_enabled backchannel backchannel_codec backchannel_rate"
	TESTED_sensor=""
	TESTED_osd="monitor_stream font_path vars_file enabled supersample hinting"
	TESTED_osd_item="text x y font_size color transparency outline outline_color"
	TESTED_motion="sensitivity monitor_stream enabled hold_ms skip_frames"
	TESTED_record="segment_s pre_roll_s post_roll_s min_free_mb audio name dir"
	TESTED_timelapse="interval_s keep_days name dir"
	TESTED_daynight="day_gain night_gain day_confirm_s boot_probe probe_min_gap_s probe_confirm_s heartbeat_s heartbeat_max_s sun_sunrise_offset_min sun_sunset_offset_min time_night_start time_day_start sun_latitude sun_longitude"
	TESTED_video="bitrate rotation rc_mode qp min_qp max_qp quality_lvl change_pos i_bias_lvl fluc_lvl gop max_gop profile"
	TESTED_privacy="enabled x y w h color"

	# ALLOW_<section>: F_CTRL fields DELIBERATELY not round-tripped by 8b, with
	# why - matching the daynight.switch_cmd/motion.on_motion F_CTRL-exclusion
	# model already used in config.c (explicit exclusions, not silent gaps).
	# A field belongs here only because POSTing it in an unattended run is a
	# bad idea, never just "nobody got to it yet" (that case should WARN).
	ALLOW_image=""
	ALLOW_audio=""
	ALLOW_sensor="model i2c_addr fps width height"             # persist-only imaging config - risky to fuzz (all of sensor.*)
	ALLOW_osd=""
	ALLOW_osd_item="enabled type"                               # enabled: an item created at boot only - enabling live is a silent no-op until restart (see the caps-builder comment above control_get_json() in control.c). type: flips text<->logo live; a logo item with no configured logo_path is a real but uninteresting failure mode to induce by automated probing
	ALLOW_motion="cols rows"                                    # risky IVS grid rebuild, clamped to the SDK cell budget - would look like a mismatch here regardless
	ALLOW_record="enabled channel mode"                         # would start/stop capture or depend on stream count (see the comment above the record lv_section call)
	ALLOW_timelapse="enabled channel"                           # same reasoning as record above
	# The eight consolidated fields (2026-08-22) are ALLOWed rather than
	# TESTED: on a current daemon they are not F_CTRL at all, so they never
	# appear in the inventory and these words cost nothing - but the fleet
	# still runs pre-consolidation firmware where they DO appear, and there
	# they must be a documented exclusion, not eight drift warnings and not a
	# stale "tested" claim for a probe 8b no longer contains. Their read-only
	# assertion lives in 8b next to the daynight lv_section call.
	ALLOW_daynight="enabled interval_ms diagnose_thresholds probe_jump_pct probe_settle_s ref_delay_s ir_ratio_night ir_ratio_day ir_min_headroom boot_settle_s transition_s learn"   # enabled reflects the detection thread's own state (poll lag), not a value to force; interval_ms/diagnose_thresholds change timing or logging rather than a readable decision value; the rest are the 2026-08-22 constants (plus the retired learn), settable only on older firmware
	# video: the exclusion used to be the WHOLE encoder block, justified as
	# "geometry/codec/identity/routing changes carry restart-crash or
	# active-session-disruption risk beyond a plain config round-trip". That
	# blanket was written when every videoN.* key was persist-only and the only
	# way to prove one had any effect was --test-encoder's restart-and-measure.
	# It over-reached in both directions, and 22832f1/c4e434f made the gap
	# untenable: the rate-control keys are now applied to the LIVE encoder on
	# most SoCs and their arrival is directly readable (encoder.<n>.rc), so
	# excluding them means the newest, least-verified code in the daemon is the
	# one thing this script never touches. They have therefore graduated to real
	# coverage in 8b's rc block - live-graded against caps.video_live, or
	# persist-graded plus an assertion that the encoder did NOT move, per key,
	# per platform, with the reason recorded in the ledger either way.
	# What genuinely stays out, and why it is a different class from a QP bound:
	#   enabled/width/height/fps/buffers  reconfigure FrameSource and the encoder
	#     channel at the next bring-up; the rotation test (2565ff) exists because
	#     that combination has crashed the daemon on real hardware, and every one
	#     of these is the same restart-crash class, not a value round-trip.
	#   codec/rtsp_path                   change stream identity and routing: a
	#     probe value tears every connected client's session out from under it
	#     at the next restart, and rtsp_path would leave THIS script unable to
	#     find the stream again if a run died between POST and restore.
	# rotation stays listed in TESTED_video, not here: it is opt-in
	# (--test-rotation) and records its own gate reason when off.
	ALLOW_video="enabled codec width height fps buffers rtsp_path"
	ALLOW_privacy=""

	contains_word() { local n="$1"; shift; local w; for w in "$@"; do [ "$w" = "$n" ] && return 0; done; return 1; }

	# Runtime ledger from 8b (see the comment where it is written). Presence
	# of lv_posted.txt = 8b ran in THIS invocation; without it (e.g.
	# `--only 8d`, or 8b skipped for lack of python3) there is NOTHING this
	# check may attest as tested - it can only compare the inventory against
	# the declared map, and must say so.
	LVP="$OUTDIR/lv_posted.txt"; LVG="$OUTDIR/lv_gated.txt"
	lv_ledger=0; [ -f "$LVP" ] && lv_ledger=1
	posted_has() { grep -q "^$1 $2\$" "$LVP" 2>/dev/null; }
	gated_has()  { grep -q "^$1 $2 "  "$LVG" 2>/dev/null; }

	drift=0; stale=0; total=0; n_post=0; n_gate=0; n_allow=0
	for sec in image audio sensor osd osd_item motion record timelapse daynight video privacy; do
		fields=$(jarr "$fj" "$sec")
		[ -n "$fields" ] || continue
		tvar="TESTED_$sec"; avar="ALLOW_$sec"
		tested="${!tvar}"; allow="${!avar}"
		for f in $fields; do
			total=$((total+1))
			if [ "$lv_ledger" = 1 ]; then
				# BOTH directions against what actually happened:
				#   inventory ⊆ posted ∪ gated ∪ allow   (coverage, as before)
				#   tested-claim ⊆ posted ∪ gated        (no stale attestation)
				if posted_has "$sec" "$f"; then n_post=$((n_post+1))
				elif gated_has "$sec" "$f"; then n_gate=$((n_gate+1))
				elif contains_word "$f" $allow; then n_allow=$((n_allow+1))
				elif contains_word "$f" $tested; then
					warn "field-inventory STALE ATTESTATION: $sec.$f is declared tested by 8b, but this run neither POSTed it nor recorded a gate reason - the probe was silently skipped (or the declaration is stale). This is the exact hole that let skipped fields be reported as tested"
					stale=$((stale+1))
				else
					warn "field-inventory drift: $sec.$f is F_CTRL (POST-able) but 8b neither exercised nor gate-skipped it and it is not on the documented allowlist - add live/persist coverage or an explicit exclusion"
					drift=$((drift+1))
				fi
			else
				# no ledger: static map comparison only (the old, one-way check)
				if contains_word "$f" $tested; then :
				elif contains_word "$f" $allow; then :
				else
					warn "field-inventory drift: $sec.$f is F_CTRL (POST-able) but is in neither 8b's declared set nor the documented allowlist - add live/persist coverage or an explicit exclusion"
					drift=$((drift+1))
				fi
			fi
		done
	done
	if [ "$lv_ledger" = 1 ]; then
		if [ "$drift" -eq 0 ] && [ "$stale" -eq 0 ]; then
			ok "field-inventory: $total F_CTRL fields - $n_post exercised by 8b THIS RUN, $n_gate gated off with a recorded reason, $n_allow allowlisted (no drift, no stale attestation)"
			[ "$n_gate" -gt 0 ] && info "  gated off this run (NOT tested - see $LVG): $(awk '{printf "%s.%s(%s) ", $1, $2, $3}' "$LVG")"
		fi
	else
		[ "$drift" -eq 0 ] && ok "field-inventory: all $total F_CTRL fields are on 8b's declared coverage map or the allowlist (map check only)"
		info "  NOTE: 8b did not run in this invocation (no runtime ledger) - the line above checks the declared MAP against the inventory; it attests that nothing is missing from the map, NOT that anything was actually exercised"
	fi
fi

fi
if want 8e malformed robustness; then
# --- 8e. /control malformed-body robustness (Finding #5) --------------------
# control.c's JSON parsing (find_obj()/get_val()) is hand-rolled - targeted
# scanning, no library. Section 8b above only ever POSTs well-formed bodies.
# These 5 cases were each picked by reading find_obj()/get_val() first, to
# actually exercise a real edge in THIS implementation rather than generic
# JSON fuzzing. This is a LIVENESS/ROBUSTNESS check (does the daemon survive
# and keep answering), not a strict "must return exactly this HTTP status"
# contract - the parser has no documented error-response shape to pin down.
hdr "8e. /control malformed-body robustness"
mb_check() {  # <label> <body>
	local label="$1" body="$2" code gcode
	code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 8 -u "$HTTP_USER:$HTTP_PASS" \
		-X POST "$(http_base)/control" -d "$body")
	if [ -z "$code" ] || [ "$code" = "000" ]; then
		bad "$label: no HTTP response at all (connection dropped/timed out) - possible parser hang/crash"
		return
	fi
	# liveness proof: an IMMEDIATELY following GET must still return 200 -
	# this is the actual assertion, not the POST's own status code
	gcode=$(curl -s -o /dev/null -w '%{http_code}' --max-time 8 -u "$HTTP_USER:$HTTP_PASS" "$(http_base)/control")
	if [ "$gcode" = "200" ]; then
		ok "$label: POST got HTTP $code, daemon still alive + functioning (GET /control -> 200 right after)"
	else
		bad "$label: POST got HTTP $code, but the FOLLOWING GET /control returned '$gcode' - daemon wedged or crashed"
	fi
}

# 1. truncated JSON: find_obj()'s brace-depth loop never sees depth return to
#    0 before hitting the end of the buffer - must return NULL, not run past e.
mb_check "truncated JSON (unclosed braces)" '{"image":{"brightness":128'

# 2. unterminated string: get_val()'s quoted-string scan (`for (p++; p<e &&
#    *p!='"'; p++)`) must stop at the buffer end, not read/write past it,
#    when the closing quote never arrives.
mb_check "unterminated string (no closing quote)" '{"osd0":{"0":{"text":"never closes'

# 3. huge/overflow-prone number: pint()'s strtol()+cast-to-int path (the M11
#    hardening comment in config.c) must clamp, not misbehave, on a value
#    that overflows long/int well past any field's [lo,hi]. Unlike cases
#    1/2/4/5, this body is well-formed JSON that the parser WILL apply -
#    it just clamps image.brightness to a range boundary rather than
#    rejecting it. WHICH boundary is platform-dependent: on 32-bit targets
#    strtol saturates at LONG_MAX=INT_MAX and the hi-clamp lands on 255,
#    while on the 64-bit sim the LONG_MAX->int cast wraps negative and the
#    lo-clamp lands on 0 (verified on the sim 2026-08-18 - the old comment
#    claimed "255" unconditionally). Either way mb_check() (by design - a
#    liveness check, not a settings test) never restores anything. That
#    silently stranded brightness at the boundary on a real camera
#    (2026-08, seen live on cam-A/cam-K/cam-L after a QA run with nothing
#    else in the log to explain it) until traced here.
#
#    Self-contained (own capture/POST/trap via curlq, not lv_get/lv_post/
#    LV_PENDING) rather than depending on section 8b having run first -
#    this section is reachable on its own via --only 8e.
MB3_PENDING=""
mb3_restore_pending() {
	[ -n "${MB3_PENDING:-}" ] || return 0
	warn "interrupted mid overflow-prone-number test - restoring image.brightness"
	curl -s -o /dev/null --max-time 8 -u "$HTTP_USER:$HTTP_PASS" \
		-X POST "$(http_base)/control" -d "$MB3_PENDING" >/dev/null 2>&1 || true
	MB3_PENDING=""
}
# mb6 (the oversized-body regression test below) probes with a DISTINCT
# contrast value, so it needs the same stranding protection; bash keeps only
# one trap per signal, hence one shared trap calling both (each is a no-op
# while unarmed)
MB6_PENDING=""
mb6_restore_pending() {
	[ -n "${MB6_PENDING:-}" ] || return 0
	warn "interrupted mid oversized-body test - restoring image.contrast"
	curl -s -o /dev/null --max-time 8 -u "$HTTP_USER:$HTTP_PASS" \
		-X POST "$(http_base)/control" -d "$MB6_PENDING" >/dev/null 2>&1 || true
	MB6_PENDING=""
}
trap 'mb3_restore_pending; mb6_restore_pending' EXIT
trap 'mb3_restore_pending; mb6_restore_pending; trap - INT;  kill -INT  $$' INT
trap 'mb3_restore_pending; mb6_restore_pending; trap - TERM; kill -TERM $$' TERM
mb3_bf="$OUTDIR/mb3_before.json"; curlq 12 "$(http_base)/control" -o "$mb3_bf"
mb3_cur=$(jget "$mb3_bf" image.brightness)
if [ -n "$mb3_cur" ]; then
	MB3_PENDING="{\"image\":{\"brightness\":$mb3_cur}}"
	mb_check "overflow-prone number" '{"image":{"brightness":99999999999999999999999999}}'
	mb3_af="$OUTDIR/mb3_after.json"
	mb3_rcode=$(curl -s -o /dev/null -w '%{http_code}' --max-time 12 -u "$HTTP_USER:$HTTP_PASS" -X POST "$(http_base)/control" -d "$MB3_PENDING")
	curlq 12 "$(http_base)/control" -o "$mb3_af"; mb3_rgot=$(jget "$mb3_af" image.brightness)
	if [ "$mb3_rgot" != "$mb3_cur" ]; then
		mb3_rcode=$(curl -s -o /dev/null -w '%{http_code}' --max-time 12 -u "$HTTP_USER:$HTTP_PASS" -X POST "$(http_base)/control" -d "$MB3_PENDING")
		curlq 12 "$(http_base)/control" -o "$mb3_af"; mb3_rgot=$(jget "$mb3_af" image.brightness)
	fi
	if [ "$mb3_rgot" = "$mb3_cur" ]; then
		MB3_PENDING=""    # restore POSTed and verified - disarm the trap
	else
		warn "overflow-prone number: restoring image.brightness to '$mb3_cur' did not land (got '$mb3_rgot', HTTP $mb3_rcode) - camera may still sit at a clamp boundary (255 on 32-bit targets, 0 on the 64-bit sim)"
	fi
else
	warn "overflow-prone number: could not read current image.brightness before the test - skipping (would strand the camera at the clamped boundary)"
	mb_check "overflow-prone number" '{"image":{"brightness":99999999999999999999999999}}'
fi

# 4. completely unknown top-level section name: none of control_apply_json's
#    find_obj(s,e,"image"/"audio"/.../ &oend) calls match it - must be a
#    silent no-op, not a crash on an unrecognized key.
mb_check "unknown top-level section" '{"totally_bogus_section_xyz":{"foo":1,"bar":"baz"}}'

# 5. wrong JSON type where an object is expected: find_obj() requires the
#    value to start with '{' (`if (!p || p>=e || *p!='{') return NULL;`) -
#    handing it a JSON array for a section name must be rejected cleanly,
#    not misparsed as if it were an object.
mb_check "array instead of object" '{"image":[1,2,3]}'

# --- 6-8: regression tests for the three httpd request-handling bugs the
# code itself documents as FIXED (httpd.c around the /control POST branch).
# Each was a real shipped defect; none had a test, so any of them could come
# back silently. All three are exact-status assertions, which POST /control
# normally cannot provide (it answers 200 unconditionally) - these paths are
# the exception, because they are rejected BEFORE control_apply_json() runs.

# 6. body larger than the request buffer (httpd.c: "used to get silently
#    clamped and the truncated prefix applied ... yet the client still gets
#    200 OK" -> now 413). The probe body leads with a DISTINCT contrast value
#    so the clamp regression is detectable as data: if the old behaviour
#    returns, the truncated prefix (which contains the full contrast kv)
#    gets applied and the re-read exposes it - a 413 status alone could be
#    faked by an unrelated rejection. Buffer is 4096 (httpd.c conn_thread);
#    6000 bytes of body clears it whatever the header sizes are.
mb6_bf="$OUTDIR/mb6_before.json"; curlq 12 "$(http_base)/control" -o "$mb6_bf"
mb6_cur=$(jget "$mb6_bf" image.contrast)
if [ -z "$mb6_cur" ]; then
	warn "oversized-body test: could not read current image.contrast - skipping (a clamp regression would strand a probe value)"
else
	mb6_new=$(( mb6_cur == 190 ? 60 : 190 ))
	MB6_PENDING="{\"image\":{\"contrast\":$mb6_cur}}"
	mb6_pad=$(head -c 6000 /dev/zero | tr '\0' 'A')
	mb6_code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 8 -u "$HTTP_USER:$HTTP_PASS" \
		-X POST "$(http_base)/control" -d "{\"image\":{\"contrast\":$mb6_new,\"pad\":\"$mb6_pad\"}}")
	mb6_af="$OUTDIR/mb6_after.json"; curlq 12 "$(http_base)/control" -o "$mb6_af"
	mb6_got=$(jget "$mb6_af" image.contrast)
	if [ "$mb6_code" = "413" ] && [ "$mb6_got" = "$mb6_cur" ]; then
		ok "oversized body (6000B > 4096B buffer): rejected with 413 and NOTHING of the truncated prefix was applied (contrast still $mb6_got)"
		MB6_PENDING=""
	elif [ "$mb6_got" != "$mb6_cur" ]; then
		bad "oversized body: HTTP $mb6_code and the TRUNCATED PREFIX WAS APPLIED (contrast $mb6_cur -> $mb6_got) - the silent-clamp bug httpd.c documents as fixed is back"
		curl -s -o /dev/null --max-time 8 -u "$HTTP_USER:$HTTP_PASS" \
			-X POST "$(http_base)/control" -d "$MB6_PENDING" >/dev/null; MB6_PENDING=""
	else
		warn "oversized body: expected 413, got HTTP $mb6_code (nothing was applied, but the explicit reject is gone - check the clen>cap guard)"
		MB6_PENDING=""
	fi
fi

# 7. negative Content-Length ("-1"): httpd.c documents that it "used to slip
#    past this guard entirely" (clen > cap is false for negative clen) - the
#    fixed guard rejects clen<0 with 413. curl won't send a broken
#    Content-Length, so this is a raw /dev/tcp exchange (same idiom as the
#    preflight port probe / test_auth.sh's rtsp_status); Authorization
#    included because on a non-loopback camera the global auth gate answers
#    before the POST branch would.
mb7_line=$(
	exec 2>/dev/null
	exec 3<>"/dev/tcp/$CAM/$HTTP_PORT" || exit
	printf 'POST /control HTTP/1.1\r\nHost: %s\r\n%s\r\nContent-Length: -1\r\nConnection: close\r\n\r\n' \
		"$CAM" "$AUTH_HDR" >&3
	IFS= read -r -t 8 line <&3
	printf '%s' "$line"
	exec 3<&- 3>&-
)
case "$mb7_line" in
	*" 413 "*) ok "negative Content-Length (-1): rejected with 413 (the guard covers clen<0, daemon answered)";;
	*" 2"[0-9][0-9]" "*) bad "negative Content-Length (-1): got '$mb7_line' - a 2xx means the negative value slipped past the guard again (the pre-fix behaviour)";;
	"")        bad "negative Content-Length (-1): NO response within 8s - the connection may be parked in the body-read loop (worse than the original bug)";;
	*)         warn "negative Content-Length (-1): unexpected response '$mb7_line' (not 413, not a 2xx leak)";;
esac
mb7_g=$(curl -s -o /dev/null -w '%{http_code}' --max-time 8 -u "$HTTP_USER:$HTTP_PASS" "$(http_base)/control")
[ "$mb7_g" = "200" ] || bad "negative Content-Length: the FOLLOWING GET /control returned '$mb7_g' - daemon wedged"

# 8. HEAD /control: httpd.c documents "HEAD previously fell into the POST
#    branch below and ran control_apply_json(\"\")" - i.e. a HEAD used to
#    EXECUTE an (empty) apply. The fix gives HEAD GET semantics with the
#    body suppressed. Distinguishing signature is the Content-Length: GET
#    semantics announce the full status document (thousands of bytes); the
#    old POST-branch behaviour announced 11 ({"ok":true}).
mb8_hdr="$OUTDIR/mb8_head.txt"
mb8_code=$(curl -s -I -o "$mb8_hdr" -w '%{http_code}' --max-time 8 -u "$HTTP_USER:$HTTP_PASS" "$(http_base)/control")
mb8_cl=$(grep -i '^Content-Length:' "$mb8_hdr" 2>/dev/null | grep -oE '[0-9]+' | head -1)
if [ "$mb8_code" = "200" ] && [ "${mb8_cl:-0}" -gt 100 ]; then
	ok "HEAD /control: 200 with GET semantics (announces ${mb8_cl}B of status JSON, body suppressed) - not the POST branch"
elif [ "$mb8_code" = "200" ]; then
	bad "HEAD /control: 200 but Content-Length=${mb8_cl:-?} - the {\"ok\":true} shape, i.e. HEAD fell into the POST branch and ran control_apply_json(\"\") again (the pre-fix behaviour)"
else
	warn "HEAD /control: HTTP $mb8_code (want 200 with GET semantics; see $mb8_hdr)"
fi

# 9. mixed known + misspelled field: the typo must be NAMED in "ignored".
#    A body carrying only unknown keys has always been visible (422
#    unknown_fields); a body mixing one good key with one typo answered 200
#    accepted:1 and dropped the typo without a word, which is the shape a real
#    client actually produces. Exact assertions, unlike cases 1-5: this is a
#    documented reply field (docs/wiki/HTTP-Control-API.md), not the parser's
#    undocumented error behaviour. Re-posts the CURRENT brightness, so accepted
#    counts it while changed stays 0 and there is nothing to restore.
mb9_bf="$OUTDIR/mb9_before.json"; curlq 12 "$(http_base)/control" -o "$mb9_bf"
mb9_cur=$(jget "$mb9_bf" image.brightness)
if [ -z "$mb9_cur" ]; then
	warn "ignored-fields test: could not read current image.brightness - skipping"
else
	mb9_rf="$OUTDIR/mb9_reply.json"
	curl -s -o "$mb9_rf" --max-time 8 -u "$HTTP_USER:$HTTP_PASS" \
		-X POST "$(http_base)/control" \
		-d "{\"image\":{\"brightness\":$mb9_cur,\"brihtness\":9}}" >/dev/null
	mb9_acc=$(jget "$mb9_rf" accepted)
	if grep -q '"image.brihtness"' "$mb9_rf" 2>/dev/null && [ "${mb9_acc:-0}" -ge 1 ]; then
		ok "mixed known+unknown field: accepted=$mb9_acc and the typo is named in ignored[] - a partly-misspelled request no longer reads as a clean success"
	elif [ "${mb9_acc:-0}" -ge 1 ]; then
		bad "mixed known+unknown field: accepted=$mb9_acc but 'image.brihtness' is not in the reply's ignored[] ($(cat "$mb9_rf" 2>/dev/null | head -c 300)) - either this daemon predates the ignored[] array, or the ign_scan walk stopped covering the image section"
	else
		warn "mixed known+unknown field: accepted='$mb9_acc', expected the valid half to apply - reply in $mb9_rf"
	fi
	# the negative half: a fully-known body must report an EMPTY list, or the
	# array would just be noise a client learns to ignore
	curl -s -o "$mb9_rf" --max-time 8 -u "$HTTP_USER:$HTTP_PASS" \
		-X POST "$(http_base)/control" -d "{\"image\":{\"brightness\":$mb9_cur}}" >/dev/null
	if grep -q '"ignored":\[\]' "$mb9_rf" 2>/dev/null; then
		ok "fully-known request reports ignored:[] - the list stays empty when there is nothing to report"
	else
		bad "fully-known request did not report an empty ignored[] ($(cat "$mb9_rf" 2>/dev/null | head -c 300)) - a false positive there makes the whole field unusable"
	fi
fi

fi
# --- 8f. Pixel-verified hflip + forced chn0 relatch (opt-in) ----------------
# Two things nothing in this script has ever proven:
#
#  1. that image.hflip has any VISIBLE effect. Every other flip check in here
#     reads the value back out of /control, which only proves the daemon
#     accepted and persisted it - the ISP could be ignoring it entirely and
#     every test would still pass. Compare pixels instead: mirror the
#     unflipped snapshot and check it matches the flipped one.
#  2. that the flip SURVIVES a channel relatch. 8fb6fd3/9034d61 fixed exactly
#     this: on the 0->1 user-count edge chn0 gets re-latched and the ISP came
#     back with hflip/vflip/running_mode reset, so the picture silently
#     un-flipped the moment the last viewer left and a new one arrived. Force
#     that edge deliberately (idle past MS_IDLE_STOP_US=2s, hal_ingenic.c:72,
#     then attach a fresh client) and re-check the pixels.
#
# Needs a STATIC scene - a camera pointed at moving traffic will produce
# ambiguous PSNR deltas, which are reported as WARN, never as a false FAIL.
if want 8f flip; then
hdr "8f. Pixel-verified hflip + forced chn0 relatch (opt-in)"
if [ "$TEST_FLIP" != "1" ]; then
	info "flip/relatch test needs --test-flip (and a static scene - it compares snapshots pixel-by-pixel) - skipped"
elif ! have ffmpeg; then
	skip "flip/relatch test needs ffmpeg (PSNR comparison)"
else
	fl_dir="$OUTDIR/flip"; mkdir -p "$fl_dir"
	fl_snap() { curl -s --max-time 12 -u "$HTTP_USER:$HTTP_PASS" "$(http_base)/snapshot.jpg?chn=0" -o "$1" 2>/dev/null; [ -s "$1" ]; }
	fl_post() { curl -s -o /dev/null -w '%{http_code}' --max-time 12 -u "$HTTP_USER:$HTTP_PASS" -X POST "$(http_base)/control" -d "$1"; }
	# Interruption safety, same reasoning as 8b/9: a run killed between the
	# flip and the restore would leave the camera mirrored for good. Chain to
	# 8b's pending-restore if that section defined one, instead of silently
	# replacing its EXIT trap (bash keeps only one).
	FLIP_PENDING=""
	flip_restore_pending() {
		[ -n "${FLIP_PENDING:-}" ] || return 0
		warn "interrupted mid flip test - restoring image.hflip"
		curl -s -o /dev/null --max-time 8 -u "$HTTP_USER:$HTTP_PASS" -X POST "$(http_base)/control" -d "$FLIP_PENDING" >/dev/null 2>&1 || true
		FLIP_PENDING=""
	}
	trap 'flip_restore_pending; command -v lv_restore_pending >/dev/null 2>&1 && lv_restore_pending' EXIT

	fj="$fl_dir/base.json"
	if ! curlq 10 "$(http_base)/control" -o "$fj" || [ ! -s "$fj" ]; then
		bad "flip test: cannot GET /control baseline"
	else
	fl_cur=$(jget "$fj" image.hflip); fl_cur=${fl_cur:-0}
	FLIP_PENDING="{\"image\":{\"hflip\":$fl_cur}}"
	# --- is the scene static enough for a pixel comparison to mean anything?
	if fl_snap "$fl_dir/static_a.jpg"; then
		sleep 2
		fl_snap "$fl_dir/static_b.jpg"
		st_psnr=$(psnr_db "$fl_dir/static_a.jpg" "$fl_dir/static_b.jpg")
		if [ -z "$st_psnr" ]; then
			warn "flip test: cannot PSNR-compare two snapshots (size mismatch or unreadable JPEG) - results below may be unreliable"
		elif fcmp "$st_psnr" ge 28; then
			ok "flip test: scene is static enough for pixel comparison (${st_psnr}dB between two snapshots 2s apart)"
		else
			warn "flip test: scene is MOVING (${st_psnr}dB between two snapshots 2s apart) - flip verdicts below may be ambiguous; point the camera at a still scene for a clean result"
		fi
	else
		bad "flip test: /snapshot.jpg?chn=0 returned no data - cannot run"
	fi

	# --- does hflip visibly do anything at all? ---------------------------
	c0=$(fl_post '{"image":{"hflip":0}}'); sleep 2
	fl_snap "$fl_dir/r0.jpg" || bad "flip test: no snapshot after hflip=0 (HTTP $c0)"
	c1=$(fl_post '{"image":{"hflip":1}}'); sleep 2
	fl_snap "$fl_dir/r1.jpg" || bad "flip test: no snapshot after hflip=1 (HTTP $c1)"
	if [ -s "$fl_dir/r0.jpg" ] && [ -s "$fl_dir/r1.jpg" ]; then
		ffmpeg -hide_banner -nostdin -loglevel error -y -i "$fl_dir/r0.jpg" -vf hflip "$fl_dir/r0_mirrored.jpg" </dev/null 2>/dev/null
		p_mir=$(psnr_db "$fl_dir/r0_mirrored.jpg" "$fl_dir/r1.jpg")
		p_dir=$(psnr_db "$fl_dir/r0.jpg" "$fl_dir/r1.jpg")
		if [ -z "$p_mir" ] || [ -z "$p_dir" ]; then
			warn "flip test: PSNR comparison failed (mirrored=${p_mir:-?} direct=${p_dir:-?}) - cannot judge whether hflip did anything"
		else
			info "  hflip=1 snapshot vs mirrored-hflip=0 snapshot: ${p_mir}dB; vs the raw hflip=0 snapshot: ${p_dir}dB"
			d=$(awk -v a="$p_mir" -v b="$p_dir" 'BEGIN{printf "%.2f", a-b}')
			if fcmp "$d" ge 3; then
				ok "hflip is REAL: the picture actually mirrors (mirrored match beats direct match by ${d}dB) - not just a config value being echoed back"
			elif fcmp "$d" le -3; then
				bad "hflip=1 did NOT change the image: the flipped snapshot still matches the UNflipped one better (by $(awk -v x="$d" 'BEGIN{printf "%.2f",-x}')dB). The daemon accepts and reports the setting, but the ISP is ignoring it"
			else
				warn "hflip effect ambiguous (mirrored vs direct differ by only ${d}dB) - scene may be near-symmetric or moving; re-run against a static, asymmetric scene"
			fi
		fi
	fi

	# --- force a chn0 relatch and re-check ---------------------------------
	# The regression this exists for only fires on the 0->1 user-count edge,
	# so the pipeline must first be genuinely idle: wait for the daemon's own
	# subscriber count to reach 0, then stay idle well past MS_IDLE_STOP_US
	# (2s, hal_ingenic.c:72) so framesource/encoder really shut down, then
	# attach a fresh client to force the re-latch.
	waited=0
	while [ "$waited" -lt 20 ]; do
		hc=$(hub_clients); [ "${hc:-0}" -le 0 ] && break
		sleep 2; waited=$((waited+2))
	done
	[ "${hc:-0}" -le 0 ] && info "  relatch: zero subscribers, letting the pipeline idle-stop (>2x MS_IDLE_STOP_US)" \
		|| warn "flip test: ${hc:-?} subscriber(s) still attached - cannot guarantee a real 0->1 edge, relatch result below is weaker evidence"
	sleep 5
	timeout 15 ffprobe -v error -rtsp_transport "$RTSP_TRANSPORT" -i "$(rtsp_url "$PATH_MAIN")" \
		-select_streams v:0 -show_entries stream=width -of csv=p=0 -read_intervals '%+#2' >/dev/null 2>&1
	sleep 5
	if fl_snap "$fl_dir/relatch.jpg" && [ -s "$fl_dir/r1.jpg" ] && [ -s "$fl_dir/r0.jpg" ]; then
		p_still=$(psnr_db "$fl_dir/relatch.jpg" "$fl_dir/r1.jpg")     # still flipped?
		p_reset=$(psnr_db "$fl_dir/relatch.jpg" "$fl_dir/r0.jpg")     # back to unflipped?
		if [ -z "$p_still" ] || [ -z "$p_reset" ]; then
			warn "flip test: PSNR comparison after the relatch failed - cannot judge whether hflip survived"
		else
			d2=$(awk -v a="$p_still" -v b="$p_reset" 'BEGIN{printf "%.2f", a-b}')
			info "  post-relatch snapshot vs flipped reference: ${p_still}dB; vs unflipped reference: ${p_reset}dB"
			if fcmp "$d2" ge 3; then
				ok "hflip SURVIVED a forced chn0 relatch (still matches the flipped reference by ${d2}dB) - the ISP self-heal on the 0->1 edge is working"
			elif fcmp "$d2" le -3; then
				bad "hflip was LOST across the chn0 relatch: the picture went back to matching the UNflipped reference. This is the 8fb6fd3/9034d61 regression - the ISP silently drops hflip/vflip when the channel re-latches on the first new viewer"
			else
				warn "flip-after-relatch ambiguous (${d2}dB apart) - scene likely moved between snapshots; re-run against a static scene"
			fi
		fi
	else
		warn "flip test: no snapshot after the forced relatch - cannot judge flip persistence"
	fi

	# --- restore ------------------------------------------------------------
	rc=$(fl_post "{\"image\":{\"hflip\":$fl_cur}}")
	rj="$fl_dir/restore.json"; curlq 10 "$(http_base)/control" -o "$rj"
	if [ "$(jget "$rj" image.hflip)" = "$fl_cur" ]; then
		FLIP_PENDING=""; info "  restored image.hflip=$fl_cur"
	else
		warn "flip test: could not restore image.hflip to $fl_cur (HTTP $rc) - the camera may be left mirrored"
	fi
	fi
fi

fi
if want 9 events; then
# --- 9. /events SSE ---------------------------------------------------------
# Previously this just waited ~8s passively and warned "may be idle" if
# nothing arrived - a permanent soft-warn that never actually proved the
# push mechanism works (ambient traffic on an otherwise-idle camera is not
# guaranteed, so "nothing arrived" was indistinguishable from "broken").
# Now: open the SSE stream, POST a small harmless reversible live-settable
# change partway through the window (same image.brightness poke style as
# section 8's write round-trip / section 8b's live-settings pokes) which
# control.c's timps_apply_setting() pushes as a "config" SSE event
# (events_config_push -> httpd.c's events_stream: `event: config` /
# `data: {"key":"image.brightness","value":"N"}`), then assert THAT SPECIFIC
# event actually arrived - not just "any" event - within the window. Restore
# the original value afterward, same pattern as every other round-trip test
# in this script.
hdr "9. /events (SSE)"
ev="$OUTDIR/events.log"
# Interruption safety (same LV_PENDING/trap pattern as section 8b's
# lv_restore_pending, added after the 2026-08-02 cam-K incident: a run
# killed between POST(new) and POST(restore) stranded manual WB rgain/bgain
# at 32767, a full-magenta image surviving reboots). This test's own poke is
# exactly that same shape - an extreme, visibly-wrong image.brightness value
# on a REAL live camera - so it gets the same protection: track the pending
# restore body and flush it from EXIT/INT/TERM, not just from the normal
# fall-through path below. Confirmed missing here 2026-08: a real run against
# cam-A (192.168.1.100) got interrupted between the poke and the restore
# and left image.brightness=255 (blown-out image) until fixed by hand.
EV9_PENDING=""
ev9_restore_pending() {
	[ -n "${EV9_PENDING:-}" ] || return 0
	warn "interrupted mid /events test - restoring image.brightness"
	curl -s -o /dev/null --max-time 8 -u "$HTTP_USER:$HTTP_PASS" \
		-X POST "$(http_base)/control" -d "$EV9_PENDING" >/dev/null 2>&1 || true
	EV9_PENDING=""
}
trap 'ev9_restore_pending' EXIT
trap 'ev9_restore_pending; trap - INT;  kill -INT  $$' INT
trap 'ev9_restore_pending; trap - TERM; kill -TERM $$' TERM
timeout 8 curl -s -N -u "$HTTP_USER:$HTTP_PASS" "$(http_base)/events" > "$ev" 2>/dev/null &
evpid=$!
sleep 2   # let the SSE connection establish before poking a setting
ev9_base="$OUTDIR/events_base.json"
ev9_bri=""
if curlq 8 "$(http_base)/control" -o "$ev9_base" && [ -s "$ev9_base" ]; then
	ev9_bri=$(jget "$ev9_base" image.brightness)
fi
if [ -n "$ev9_bri" ]; then
	ev9_new=$(awk -v c="$ev9_bri" 'BEGIN{m=int((0+255)/2); print (m!=c)?m:(m<255?m+1:m-1)}')
	EV9_PENDING="{\"image\":{\"brightness\":$ev9_bri}}"    # armed until restore lands
	code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 8 -u "$HTTP_USER:$HTTP_PASS" \
		-X POST "$(http_base)/control" -d "{\"image\":{\"brightness\":$ev9_new}}")
	[ "$code" = "200" ] && info "  poked image.brightness $ev9_bri -> $ev9_new mid-window to provoke a \"config\" SSE event" \
		|| warn "/events test: brightness poke POST returned HTTP $code"
	# restore right away rather than waiting out the rest of the window first
	curl -s -o /dev/null --max-time 8 -u "$HTTP_USER:$HTTP_PASS" \
		-X POST "$(http_base)/control" -d "$EV9_PENDING" >/dev/null
	EV9_PENDING=""    # restore POSTed - disarm the trap
else
	warn "/events test: could not read current image.brightness from /control - cannot provoke an event this run"
fi
wait "$evpid" 2>/dev/null
lines=$(wc -l <"$ev" 2>/dev/null); lines=${lines:-0}
if [ -n "$ev9_bri" ]; then
	if grep -q '"key":"image.brightness"' "$ev" 2>/dev/null; then
		ok "/events: provoked image.brightness config event arrived within the window ($lines line(s) total) - push mechanism proven, not just ambient traffic"
	else
		bad "/events: $lines line(s) received but NONE reflect the image.brightness POST we just made - config-push mechanism not confirmed"
	fi
elif [ "$lines" -gt 0 ]; then
	warn "/events streamed $lines line(s) in 8s (could not provoke a change to verify against - see above; this only shows ambient traffic, not a proven push)"
else
	bad "/events produced no data in 8s, including after a live-settings POST that should have provoked a config event"
fi

fi
if want 10 onvif; then
# --- 10. ONVIF --------------------------------------------------------------
hdr "10. ONVIF"
if (exec 3<>"/dev/tcp/$CAM/$ONVIF_PORT") 2>/dev/null; then
	exec 3>&- 2>/dev/null

	# 10a: snapshot proxies for BOTH streams (the auth-free loopback-proxy fix)
	for pair in "0:image.cgi" "1:image1.cgi"; do
		chn=${pair%%:*}; pth=${pair##*:}
		f="$OUTDIR/onvif_snap_${chn}.jpg"
		code=$(curl -s -o "$f" -w '%{http_code}' --max-time 8 -u "$HTTP_USER:$HTTP_PASS" "http://$CAM:$ONVIF_PORT/onvif/$pth")
		m=$(head -c2 "$f" 2>/dev/null | od -An -tx1 | tr -d ' \n')
		if [ "$m" = "ffd8" ]; then
			ok "ONVIF snapshot chn$chn (/onvif/$pth) -> JPEG ($(wc -c <"$f")B)"
		elif [ "$code" = "401" ]; then
			# 401 = the /x snapshot CGI IS present and enforces WebUI auth (good;
			# means the symlink-into-/x works). An ONVIF NVR gets the JPEG because
			# onvif_simple_server appends ?token=<thingino-api.key> to the snapurl,
			# which the CGI accepts. Verify with that token when we have SSH.
			if [ -n "$SSH_TARGET" ]; then
				key=$(sshx "cat /etc/thingino-api.key 2>/dev/null")
				if [ -n "$key" ]; then
					code2=$(curl -s -o "$f" -w '%{http_code}' --max-time 8 "http://$CAM:$ONVIF_PORT/onvif/$pth?token=$key")
					m2=$(head -c2 "$f" 2>/dev/null | od -An -tx1 | tr -d ' \n')
					[ "$m2" = "ffd8" ] && ok "ONVIF snapshot chn$chn via ?token= -> JPEG ($(wc -c <"$f")B)" \
						|| warn "ONVIF chn$chn even with api-key token: HTTP $code2 (idle 'no frame' or CGI issue)"
				else warn "ONVIF chn$chn 401 (CGI present, auth-enforced); couldn't read /etc/thingino-api.key via SSH"; fi
			else
				info "ONVIF chn$chn /onvif/$pth: 401 = CGI present + auth-enforced (an ONVIF NVR authenticates via ?token=<api-key>; verify in ODM, or run with --ssh to test the token here)"
			fi
		elif [ "$code" = "200" ]; then
			warn "ONVIF /onvif/$pth HTTP 200 but not a JPEG (CGI/symlink missing? uhttpd fell through to the SPA)"
		else
			warn "ONVIF /onvif/$pth HTTP $code"
		fi
	done

	# 10b: device liveness
	resp=$(onvif_call device_service '<GetSystemDateAndTime xmlns="http://www.onvif.org/ver10/device/wsdl"/>')
	grep -qiE 'SystemDateAndTime|Envelope' <<<"$resp" && ok "ONVIF device_service responds (GetSystemDateAndTime)" \
		|| warn "ONVIF device_service gave no SOAP reply"

	# 10c: profiles - resolution/codec must match the real streams; fps/bitrate
	# are known-static in onvif_simple_server's XML templates (surfaced, not failed)
	have openssl || info "openssl absent -> ONVIF GetProfiles unauthenticated (may 401)"
	pr=$(onvif_call media_service '<GetProfiles xmlns="http://www.onvif.org/ver10/media/wsdl"/>')
	[ -z "$pr" ] && pr=$(onvif_call media '<GetProfiles xmlns="http://www.onvif.org/ver10/media/wsdl"/>')
	if grep -qiE 'Resolution|VideoEncoderConfiguration|Profiles' <<<"$pr"; then
		o_res=$(grep -oiE 'Width>[0-9]+</[a-z0-9:]*Width><[a-z0-9:]*Height>[0-9]+' <<<"$pr" \
			| sed -E 's/.*Width>([0-9]+).*Height>([0-9]+)/\1x\2/' | tr '\n' ' ')
		o_enc=$(grep -oiE '<[a-z0-9:]*Encoding>(H264|H265|JPEG|MPEG4)' <<<"$pr" | grep -oiE 'H264|H265|JPEG|MPEG4' | sort -u | tr '\n' ' ')
		o_fps=$(grep -oiE 'FrameRateLimit>[0-9.]+' <<<"$pr" | grep -oE '[0-9.]+' | tr '\n' ' ')
		o_br=$(grep -oiE 'BitrateLimit>[0-9]+'    <<<"$pr" | grep -oE '[0-9]+'   | tr '\n' ' ')
		info "ONVIF advertises: resolutions=[${o_res}] codecs=[${o_enc}] FrameRateLimit=[${o_fps}] BitrateLimit=[${o_br}]"
		# compare resolution + codec to the actual live streams
		for l in "main:$PATH_MAIN" "sub:$PATH_SUB"; do
			nm=${l%%:*}; pth=${l##*:}
			rl=$(ffprobe -v error -rtsp_transport "$RTSP_TRANSPORT" -select_streams v:0 \
				-show_entries stream=codec_name,width,height -of csv=p=0 "$(rtsp_url "$pth")" 2>/dev/null)
			cc=$(echo "$rl" | cut -d, -f1); cw=$(echo "$rl" | cut -d, -f2); ch=$(echo "$rl" | cut -d, -f3)
			[ -n "$cw" ] && { grep -qi "${cw}x${ch}" <<<"$o_res" \
				&& ok "ONVIF $nm resolution ${cw}x${ch} matches the real stream" \
				|| warn "ONVIF $nm resolution mismatch: real ${cw}x${ch}, advertised [${o_res}]"; }
			[ -n "$cc" ] && { grep -qi "$cc" <<<"$(echo "$o_enc" | tr 'A-Z' 'a-z' | sed 's/h26/h26/')" \
				&& ok "ONVIF $nm codec ($cc) advertised" \
				|| warn "ONVIF $nm codec mismatch: real $cc, advertised [${o_enc}]"; }
		done
		# fps/bitrate: compare the ADVERTISED values against the REAL ones
		# instead of pattern-matching on the template constants. The old
		# check warned whenever 30 or 5000 appeared - but 30 fps / 5000 kbps
		# are perfectly legitimate REAL rates, so a camera actually running
		# them was accused of advertising template defaults on the strength
		# of a value that cannot distinguish the two cases. The references
		# that CAN: NOM_FPS (section 2's /control video.<N>.fps - NOT
		# ffprobe's r_frame_rate, which over live RTSP reports the sensor
		# tick rate rather than the configured fps; see the comment at the
		# top of section 2) and the configured bitrates from /control.
		# Advertised==real is fine whatever the number; only a mismatch is
		# the template lie.
		o10_cj="$OUTDIR/onvif_ctrl.json"
		curlq 8 "$(http_base)/control" -o "$o10_cj" 2>/dev/null || true
		o10_reals_fps="${NOM_FPS[main]:-} ${NOM_FPS[sub]:-}"
		o10_reals_br="$(jget "$o10_cj" video.0.bitrate 2>/dev/null) $(jget "$o10_cj" video.1.bitrate 2>/dev/null)"
		# match_any <candidate> <tolerance-fraction> <ref...> - is the
		# candidate within tol of ANY reference?
		o10_match() { local v="$1" tol="$2"; shift 2; local r
			for r in "$@"; do
				[ -n "$r" ] || continue
				awk -v a="$v" -v b="$r" -v t="$tol" 'BEGIN{d=a-b; if(d<0)d=-d; exit !(b>0 && d/b<=t)}' && return 0
			done; return 1; }
		o10_fps_bad=""; o10_br_bad=""
		if [ -n "$(echo $o10_reals_fps)" ]; then
			for v in $o_fps; do o10_match "$v" 0.10 $o10_reals_fps || o10_fps_bad="$o10_fps_bad $v"; done
		fi
		if [ -n "$(echo $o10_reals_br)" ]; then
			# 10% tolerance like fps: a daemon that surfaces the real bitrate
			# reports the configured value, so anything looser only serves to
			# let the 5000-template pass against a coincidentally-close config
			# (at 20% it matched a configured 4256 - measured while testing)
			for v in $o_br; do o10_match "$v" 0.10 $o10_reals_br || o10_br_bad="$o10_br_bad $v"; done
		fi
		if [ -z "$(echo $o10_reals_fps)" ] && [ -z "$(echo $o10_reals_br)" ]; then
			info "ONVIF fps/bitrate advertised [${o_fps}] / [${o_br}] - no real reference available to compare (section 2 skipped and /control unreachable)"
		elif [ -n "$o10_fps_bad" ] || [ -n "$o10_br_bad" ]; then
			warn "ONVIF advertises fps [${o_fps}] / bitrate [${o_br}] but the real rates are fps [$(echo $o10_reals_fps)] / configured kbps [$(echo $o10_reals_br)] - mismatched values (${o10_fps_bad:+fps$o10_fps_bad }${o10_br_bad:+kbps$o10_br_bad}) look like onvif_simple_server template defaults, not the encoder's rate"
		else
			ok "ONVIF FrameRateLimit/BitrateLimit match the real encoder rates (fps [${o_fps}] vs [$(echo $o10_reals_fps)], kbps [${o_br}] vs [$(echo $o10_reals_br)])"
		fi
	else
		warn "ONVIF GetProfiles returned nothing/401 (WS-Security? needs openssl + ONVIF creds ${ONVIF_USER}/***)"
	fi
else skip "ONVIF port $ONVIF_PORT closed (ONVIF not built? add BR2_PACKAGE_THINGINO_ONVIF=y)"; fi

fi
if want 11 clip record; then
# --- 11. Recording clip -----------------------------------------------------
hdr "11. On-demand recording clip (/control record.clip)"
clip="/tmp/timps_qa_$$.mp4"
code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 30 -u "$HTTP_USER:$HTTP_PASS" \
	-X POST "$(http_base)/control" -d "{\"record\":{\"clip\":\"$clip\",\"seconds\":4}}")
if [ "$code" = "200" ]; then
	if [ -n "$SSH_TARGET" ]; then
		sz=$(sshx "wc -c < $clip 2>/dev/null || echo 0"); sz=${sz:-0}
		[ "${sz:-0}" -gt 2000 ] && ok "record.clip wrote ${sz}B fMP4 on device" || bad "record.clip file missing/empty on device"
		sshx "rm -f $clip" 2>/dev/null
	else
		# NOT a pass: POST /control answers 200 unconditionally (httpd.c -
		# record_clip()'s return code is dropped in control.c), so without
		# SSH there is no observable that distinguishes "clip written" from
		# "no SD card / bad path / USE_RECORD regression". The old ok here
		# was a guaranteed PASS that could not fail, counting as proof of a
		# recorder nobody looked at. The /control status has no clip-result
		# field either (record.file is the continuous recorder's) - if the
		# daemon ever surfaces one, this can become a host-side check.
		skip "record.clip: HTTP 200 proves only transport (the daemon answers 200 unconditionally and drops record_clip's return code) - pass --ssh to verify the file actually exists on device"
	fi
else warn "record.clip returned HTTP $code"; fi

fi
if want 12 reliability reconnect; then
# --- 12. Reliability: reconnect churn --------------------------------------
hdr "12. Reliability - reconnect churn ($RECONNECT_CYCLES cycles)"
for tr in tcp udp; do
	okc=0; badc=0; ttff_sum=0
	for i in $(seq 1 "$RECONNECT_CYCLES"); do
		lg="$OUTDIR/reconnect_${tr}_$i.log"
		st=$(date +%s.%N)
		if timeout -k 3 12 ffmpeg -hide_banner -nostdin -loglevel error -rtsp_transport "$tr" \
			-i "$(rtsp_url "$PATH_SUB")" -frames:v 5 -f null - </dev/null 2>"$lg"; then
			en=$(date +%s.%N); ttff=$(awk -v a="$st" -v b="$en" 'BEGIN{printf "%.2f",b-a}')
			okc=$((okc+1)); ttff_sum=$(awk -v s="$ttff_sum" -v t="$ttff" 'BEGIN{printf "%.2f",s+t}')
		else badc=$((badc+1)); fi
	done
	avg=$(awk -v s="$ttff_sum" -v n="$okc" 'BEGIN{printf "%.2f", (n>0?s/n:0)}')
	if [ "$badc" -eq 0 ]; then ok "reconnect/$tr ${okc}/${RECONNECT_CYCLES} ok, avg time-to-5-frames ${avg}s"
	elif [ "$okc" -gt 0 ]; then warn "reconnect/$tr ${okc} ok / ${badc} failed"
	else bad "reconnect/$tr all $RECONNECT_CYCLES failed"; fi
done

fi
# --- 12b. Session reaping after ungraceful client death (opt-in) ------------
# Regression test for the immortal-session class (6473848 "reap orphaned UDP
# sessions after 2x the advertised 60s session timeout", 265befb): a client
# that dies WITHOUT sending TEARDOWN. For UDP-transport RTSP this is genuinely
# undetectable at the socket layer - sendto() on an unconnected UDP socket
# never errors, however dead the peer is - so the only thing standing between
# a kill -9 and a permanently pinned session (a thread, an fd pair, a fanqueue,
# a hub subscription, one of only RTSP_MAX_CLIENTS=8 slots) is the reaper.
#
# Measured through hub_video_subs(), surfaced as "clients" in the /events
# stats frame - the daemon's own count of live video subscribers, which is
# exactly the number that must come back down. Nothing else in this script has
# ever asserted that a session ENDS.
if want 12b leak reap; then
hdr "12b. Session reaping after ungraceful client death (opt-in)"
if [ "$TEST_LEAK" != "1" ]; then
	info "session-reaping test needs --test-leak (holds the camera for several minutes waiting out reap timeouts) - skipped"
else
	# leak_phase <label> <reap_bound_s> <cmd...>
	#   start cmd as a client, confirm the daemon SEES it, kill -9 it (never a
	#   TEARDOWN, never a clean close), then poll until the count returns to
	#   the pre-test baseline or the bound expires.
	leak_phase() {
		local label="$1" bound="$2"; shift 2
		local base cur pid i attached=0 waited=0
		base=$(hub_clients)
		if [ -z "$base" ]; then
			warn "$label: cannot read \"clients\" from /events?stream=stats - skipping this phase"
			return
		fi
		"$@" </dev/null >/dev/null 2>&1 &
		pid=$!
		for i in $(seq 1 8); do
			cur=$(hub_clients)
			[ -n "$cur" ] && [ "$cur" -gt "$base" ] && { attached=1; break; }
			sleep 2
		done
		if [ "$attached" != "1" ]; then
			kill -9 "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
			warn "$label: the test client never showed up in the daemon's subscriber count (baseline $base) - cannot test reaping of a session that never started"
			return
		fi
		info "  $label: client attached (clients ${base} -> ${cur}), now killing it with SIGKILL - no TEARDOWN, no clean close"
		kill -9 "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
		local t0; t0=$(date +%s)
		while [ "$waited" -lt "$bound" ]; do
			sleep 5
			cur=$(hub_clients)
			waited=$(( $(date +%s) - t0 ))
			[ -n "$cur" ] && [ "$cur" -le "$base" ] && break
		done
		if [ -n "$cur" ] && [ "$cur" -le "$base" ]; then
			ok "$label: orphaned session reaped after ~${waited}s (clients back to $cur, bound ${bound}s)"
		else
			bad "$label: session NOT reaped within ${bound}s - clients still ${cur:-?} vs baseline ${base}. The killed client permanently holds a thread, fds, a fanqueue and one of the ${RTSP_CAP} RTSP slots (immortal-session regression)"
		fi
	}

	# RTSP over UDP: reaper bound is 2x RTSP_SESSION_TIMEOUT_S (rtsp.c:45,1361)
	# = 120s; allow 150s so a slow poll cycle is not reported as a leak.
	leak_phase "reap/rtsp-udp" 150 \
		ffmpeg -hide_banner -nostdin -loglevel error -rtsp_transport udp \
		-i "$(rtsp_url "$PATH_MAIN")" -f null -
	# HTTP fMP4: a SIGKILLed curl closes its socket, so the write path should
	# notice almost immediately; MS_STREAM_STALL_US (httpd.c:62-63, 371, 597)
	# is the 60s backstop for the half-open case. 90s covers both.
	leak_phase "reap/http-fmp4" 90 \
		curl -s -o /dev/null -u "$HTTP_USER:$HTTP_PASS" "$(http_base)/stream.mp4?chn=0"
	# SRT, only if this camera actually has it compiled in and enabled and this
	# host's ffmpeg speaks srt://. SRT_STALL_US (srt.c:57) is also 60s.
	lk="$OUTDIR/leak_srt_caps.json"
	if curlq 8 "$(http_base)/control" -o "$lk" && [ -s "$lk" ] \
	   && [ "$(jget "$lk" srt.available)" = "1" ] && [ "$(jget "$lk" srt.enabled)" = "1" ] \
	   && have ffmpeg && ffmpeg -hide_banner -protocols 2>/dev/null | grep -qiE '^[[:space:]]*srt[[:space:]]*$'; then
		leak_phase "reap/srt" 90 \
			ffmpeg -hide_banner -nostdin -loglevel error -i "srt://$CAM:$(jget "$lk" srt.port)" -f null -
	else
		skip "reap/srt: SRT not available+enabled on this camera, or this host's ffmpeg has no srt:// support"
	fi
fi

fi
if want 13 load; then
# --- 13. Load: concurrent-client ramp --------------------------------------
hdr "13. Load - concurrent client ramp [$LOAD_CLIENTS] x ${LOAD_DUR}s each"
max_stable=0
for n in $LOAD_CLIENTS; do
	pids=""; ldir="$OUTDIR/load_${n}"; mkdir -p "$ldir"
	lsnap0=""; [ -n "$SSH_TARGET" ] && lsnap0=$(dev_snap)
	for c in $(seq 1 "$n"); do
		timeout -k 5 "$((LOAD_DUR+6))" ffmpeg -hide_banner -nostdin -loglevel error -stats -rtsp_transport "$RTSP_TRANSPORT" \
			-i "$(rtsp_url "$PATH_MAIN")" -t "$LOAD_DUR" -an -f null - </dev/null >"$ldir/c${c}.out" 2>"$ldir/c${c}.log" &
		pids="$pids $!"
	done
	# shellcheck disable=SC2086
	wait $pids 2>/dev/null
	# collect per-client fps
	tf=0; okcli=0; failcli=0; minfps=1e9
	for c in $(seq 1 "$n"); do
		fr=$(grep -oE 'frame= *[0-9]+' "$ldir/c${c}.log" | tail -1 | grep -oE '[0-9]+')
		if [ -n "${fr:-}" ] && [ "$fr" -gt 0 ]; then
			fps=$(awk -v f="$fr" -v d="$LOAD_DUR" 'BEGIN{printf "%.1f",f/d}')
			okcli=$((okcli+1)); tf=$((tf+fr))
			fcmp "$fps" lt "$minfps" && minfps="$fps"
		else failcli=$((failcli+1)); fi
	done
	agg=$(awk -v f="$tf" -v d="$LOAD_DUR" 'BEGIN{printf "%.0f",f/d}')
	extra=""
	if [ -n "$SSH_TARGET" ]; then
		lsnap1=$(dev_snap)
		la=$(sshx "cut -d' ' -f1 /proc/loadavg 2>/dev/null")
		if [ -n "$lsnap1" ]; then
			read -r l_rss l_fd l_thr _ _ <<<"$lsnap1"
			read -r l_rss0 l_fd0 l_thr0 _ _ <<<"${lsnap0:-? ? ? 0 0}"
			# fd/thread counts at each ramp step: per-client cost should be
			# flat and, critically, should come back down between steps. A
			# step-over-step ratchet here is the same leak signature the soak
			# watches for, seen under a cleaner stimulus (every client of the
			# previous step is provably gone by now - `wait` returned).
			echo "$l_fd"  >> "$OUTDIR/load_fds.txt"
			echo "$l_thr" >> "$OUTDIR/load_threads.txt"
			extra=" | timpsd RSS ${l_rss0}->${l_rss}kB fds ${l_fd0}->${l_fd} threads ${l_thr0}->${l_thr} CPU $(dev_cpu_between "$lsnap0" "$lsnap1")% load ${la:-?}"
		fi
	fi
	nf="${NOM_FPS[main]:-0}"; lo=$(awk -v x="$nf" 'BEGIN{printf "%.1f",x*0.9}')
	# RTSP_MAX_CLIENTS (rtsp.c:32) is a HARD cap of 8: the 9th client is
	# rejected on purpose, with a "client limit (%d) reached, rejecting" log
	# line, because each client costs a thread plus a fanqueue. The `load`
	# profile ramps to 12 and 16, so those steps used to be reported as
	# "N failed (degrading)" - describing correctly-enforced admission control
	# as degradation, and burying a real degradation if one ever happened
	# there. Label the two cases apart. Not exposed via /control (checked),
	# hence the constant + citation; override with RTSP_CAP= if a build
	# changes it.
	if [ "$n" -gt "$RTSP_CAP" ]; then
		if [ "$okcli" -eq "$RTSP_CAP" ] && { fcmp "$nf" le 0 || fcmp "$minfps" ge "$lo"; }; then
			ok "load ${n} clients: at cap - exactly ${RTSP_CAP} served at full fps (min ${minfps}), ${failcli} correctly rejected by RTSP_MAX_CLIENTS=${RTSP_CAP} (rtsp.c:32)${extra}"
		elif [ "$okcli" -eq "$RTSP_CAP" ]; then
			warn "load ${n} clients: at cap (${RTSP_CAP} served, ${failcli} rejected as designed) but min fps ${minfps} is below 90% of nominal ${nf} - the served clients are degrading${extra}"
		elif [ "$okcli" -gt 0 ]; then
			warn "load ${n} clients: only ${okcli} served, expected the full cap of ${RTSP_CAP} before rejections start (${failcli} failed)${extra}"
		else
			bad "load ${n} clients: all failed - not cap enforcement, the server served nobody${extra}"; break
		fi
	elif [ "$failcli" -eq 0 ] && { fcmp "$nf" le 0 || fcmp "$minfps" ge "$lo"; }; then
		ok "load ${n} clients: all ok, min ${minfps} fps, aggregate ${agg} fps/s${extra}"
		max_stable="$n"
	elif [ "$okcli" -gt 0 ]; then
		warn "load ${n} clients: ${okcli} ok / ${failcli} failed, min fps ${minfps} (degrading, and below the ${RTSP_CAP}-client cap so this is NOT admission control)${extra}"
	else
		bad "load ${n} clients: all failed${extra}"; break
	fi
done
info "max stable concurrent clients (full fps, no failures): $max_stable (hard cap RTSP_MAX_CLIENTS=${RTSP_CAP})"
if [ -s "$OUTDIR/load_fds.txt" ]; then
	read -r gn gfirst glast gdelta gnondec gup <<<"$(leak_trend < "$OUTDIR/load_fds.txt")"
	if [ "${gn:-0}" -ge 3 ] && [ "$gdelta" -gt 0 ] && [ "$gnondec" = "1" ]; then
		bad "load ramp: timpsd fd count rose monotonically ${gfirst} -> ${glast} across $gn steps and never came back down - clients from earlier steps are not being released"
	elif [ "${gn:-0}" -ge 3 ]; then
		ok "load ramp: timpsd fd count returned to a stable level between steps (${gfirst} -> ${glast})"
	fi
fi

fi
# --- 13b. Hostile (stalled) client alongside healthy ones (opt-in) ----------
# The ramp above only ever creates WELL-BEHAVED clients, which is the easy
# case. The interesting one is a client that connects properly and then stops
# reading - a wedged viewer, a suspended laptop, a saturated wifi link. Three
# separate things must hold, and none of them were tested:
#
#   * healthy clients keep their frame rate (fanqueue is per-client, so one
#     slow consumer must not stall the shared producer);
#   * the KEYFRAME RATE for everyone else does not spike. A client whose queue
#     overflows asks for an IDR, and IDR requests are global to the shared
#     encoder - which is precisely why that request is rate-limited
#     (src/fanqueue.h:38-47). If the limiter regressed, one stalled client
#     silently doubles or triples everybody's bitrate, and nothing else in
#     this script would notice;
#   * memory stays bounded (perf audit P-08 estimated ~1.5-2.5 MB pinned per
#     stalled client, never measured until now, on a 32 MB-class SoC).
#
# rtsp-stall.py refreshes its stalled session every 12 s (overlapping), because
# the server reaps a TCP peer after 15 s of zero write progress - without that
# refresh only the first half of a 30 s phase would actually be hostile.
if want 13b hostile stalled; then
hdr "13b. Hostile (stalled) client alongside healthy clients (opt-in)"
if [ "$TEST_HOSTILE" != "1" ]; then
	info "hostile-client test needs --test-hostile - skipped"
elif ! have python3; then
	skip "hostile-client test needs python3 (scripts/rtsp-stall.py)"
else
	stall_py="$(dirname "$0")/rtsp-stall.py"
	if [ ! -f "$stall_py" ]; then
		skip "hostile-client test: scripts/rtsp-stall.py not found next to this script"
	else
		hdir="$OUTDIR/hostile"; mkdir -p "$hdir"
		HOST_N="${HOST_N:-2}"       # healthy clients per phase
		HOST_DUR="${HOST_DUR:-$LOAD_DUR}"
		# one measurement phase: N healthy clients, optionally with the stalled
		# one running alongside. Sets HP_MINFPS / HP_KEY / HP_FRAMES.
		hostile_phase() {
			local tag="$1" pids="" c fr kf
			for c in $(seq 1 "$HOST_N"); do
				timeout -k 5 "$((HOST_DUR+8))" ffmpeg -hide_banner -nostdin -loglevel error -stats \
					-rtsp_transport tcp -i "$(rtsp_url "$PATH_MAIN")" -t "$HOST_DUR" -an \
					-c copy -f matroska "$hdir/${tag}_c${c}.mkv" </dev/null >/dev/null 2>"$hdir/${tag}_c${c}.log" &
				pids="$pids $!"
			done
			# shellcheck disable=SC2086
			wait $pids 2>/dev/null
			HP_MINFPS=""; HP_KEY=0; HP_FRAMES=0
			for c in $(seq 1 "$HOST_N"); do
				fr=$(grep -oE 'frame= *[0-9]+' "$hdir/${tag}_c${c}.log" | tail -1 | grep -oE '[0-9]+')
				[ -n "${fr:-}" ] && [ "$fr" -gt 0 ] || continue
				local fps; fps=$(awk -v f="$fr" -v d="$HOST_DUR" 'BEGIN{printf "%.1f",f/d}')
				[ -z "$HP_MINFPS" ] && HP_MINFPS="$fps"
				fcmp "$fps" lt "$HP_MINFPS" && HP_MINFPS="$fps"
				# keyframe count on this client's own recording - the direct
				# observable for "did somebody force extra IDRs on us"
				kf=$(ffprobe -v error -select_streams v -show_entries frame=key_frame -of csv=p=0 \
					"$hdir/${tag}_c${c}.mkv" 2>/dev/null | grep -c '^1$')
				HP_KEY=$((HP_KEY + ${kf:-0})); HP_FRAMES=$((HP_FRAMES + fr))
			done
		}

		# --- baseline: healthy clients only ------------------------------
		info "  baseline: $HOST_N healthy clients, ${HOST_DUR}s, no stalled client"
		hsnap0=""; [ -n "$SSH_TARGET" ] && hsnap0=$(dev_snap)
		hostile_phase base
		base_minfps="$HP_MINFPS"; base_key="$HP_KEY"; base_frames="$HP_FRAMES"
		if [ -z "$base_minfps" ]; then
			bad "hostile test: the baseline healthy clients produced no frames - cannot compare anything"
		else
			info "  baseline: min ${base_minfps} fps, ${base_key} keyframes in ${base_frames} frames"

			# --- with a deliberately stalled client alongside -------------
			python3 "$stall_py" --host "$CAM" --port "$RTSP_PORT" --path "$PATH_MAIN" \
				--user "$RTSP_USER" --pw "$RTSP_PASS" --secs "$((HOST_DUR+30))" \
				> "$hdir/stall.out" 2>"$hdir/stall.err" &
			stall_pid=$!
			stalled=0
			for i in $(seq 1 10); do
				grep -q STALLED "$hdir/stall.out" 2>/dev/null && { stalled=1; break; }
				kill -0 "$stall_pid" 2>/dev/null || break
				sleep 1
			done
			if [ "$stalled" != "1" ]; then
				kill -9 "$stall_pid" 2>/dev/null; wait "$stall_pid" 2>/dev/null
				warn "hostile test: the stalled client never completed its RTSP handshake ($(head -1 "$hdir/stall.err" 2>/dev/null)) - nothing hostile to measure against"
			else
				ok "hostile test: stalled client established (DESCRIBE/SETUP/PLAY over interleaved TCP, then deaf - no reads, no TEARDOWN)"
				hostile_phase stalled
				kill -9 "$stall_pid" 2>/dev/null; wait "$stall_pid" 2>/dev/null
				# Distinguish client DEATH from client STARVATION before the
				# fps verdicts: a fatal ffmpeg muxer abort ("Can't write
				# packet with unknown timestamp") zeroes the frame counts
				# exactly like starvation would, but means something entirely
				# different - the server delivered data with a broken
				# timestamp, the client gave up. The 2026-08-11 cam-L FAIL
				# read "fell to 0.0 fps because ONE client stopped reading"
				# while both healthy clients had actually aborted in ~1s on
				# the v1.8.5 RTCP SR clock-domain regression (rtp.c) - the
				# starvation framing sent the investigation toward locks and
				# fanqueues first.
				h_aborted=$(grep -l 'Error muxing a packet\|unknown timestamp' \
					"$hdir"/stalled_c*.log 2>/dev/null | wc -l)
				[ "${h_aborted:-0}" -gt 0 ] && warn "hostile: ${h_aborted} healthy client(s) ABORTED on a fatal muxer/timestamp error (see $hdir/stalled_c*.log) - the verdict below reflects client death, not starvation; suspect a server-side timestamp anomaly (RTCP SR pairing, PTS discontinuity) before suspecting isolation"
				if [ -z "$HP_MINFPS" ]; then
					bad "hostile test: healthy clients produced NO frames while one stalled client was attached - a single stuck viewer took the stream down for everybody"
				else
					nf="${NOM_FPS[main]:-0}"
					info "  with stalled client: min ${HP_MINFPS} fps, ${HP_KEY} keyframes in ${HP_FRAMES} frames"
					# ISOLATION is a DIFFERENTIAL question: "did the stalled
					# client cost the healthy ones anything?" The only valid
					# reference is the baseline phase measured moments ago on
					# the same scene, same client count, same encoder settings.
					# Comparing against NOMINAL fps instead conflates isolation
					# with "does this SoC sustain its configured fps at all",
					# which is section 3's job - and produced a bogus isolation
					# FAIL on a T31L whose main stream simply runs at ~20 fps
					# (baseline 18.2 -> 18.1 with the stalled client attached,
					# i.e. isolation was holding perfectly).
					blo=$(awk -v b="$base_minfps" 'BEGIN{printf "%.1f", b*0.9}')
					if fcmp "$HP_MINFPS" ge "$blo"; then
						ok "hostile: healthy clients kept their frame rate (${HP_MINFPS} fps vs baseline ${base_minfps}) with a stalled client attached - per-client isolation is holding"
					else
						bad "hostile: healthy clients fell from ${base_minfps} to ${HP_MINFPS} fps because ONE client stopped reading - per-client isolation is not holding"
					fi
					# absolute fps is a SEPARATE observation, never an isolation
					# verdict: report it only if the baseline itself was already
					# below nominal (so the reader knows both phases were).
					if fcmp "$nf" gt 0 && fcmp "$base_minfps" lt "$(awk -v x="$nf" 'BEGIN{printf "%.1f",x*0.9}')"; then
						info "  (note: the BASELINE phase itself only reached ${base_minfps} fps against a nominal ${nf} - that is a stream/SoC throughput finding from section 3, not an isolation failure)"
					fi
					# keyframe density, normalised (client frame counts differ
					# slightly between phases). A forced-IDR storm shows up as a
					# multiple, so the threshold is deliberately generous.
					bkd=$(awk -v k="$base_key" -v f="$base_frames" 'BEGIN{printf "%.4f", (f>0)?k/f:0}')
					skd=$(awk -v k="$HP_KEY"   -v f="$HP_FRAMES"   'BEGIN{printf "%.4f", (f>0)?k/f:0}')
					info "  keyframe density: baseline ${bkd} vs with-stalled-client ${skd} (keyframes per frame)"
					if fcmp "$bkd" le 0; then
						warn "hostile: could not measure a baseline keyframe density - skipping the IDR-storm check"
					else
						ratio=$(awk -v a="$skd" -v b="$bkd" 'BEGIN{printf "%.2f", a/b}')
						if fcmp "$ratio" le 1.5; then
							ok "hostile: keyframe rate for healthy clients essentially unchanged (${ratio}x) - the global IDR-request rate limit is holding"
						elif fcmp "$ratio" le 2.5; then
							warn "hostile: keyframe rate for healthy clients rose ${ratio}x with one stalled client attached - the global IDR-request limiter may be leaking through (fanqueue.h:38-47)"
						else
							bad "hostile: keyframe rate for healthy clients rose ${ratio}x - ONE stalled client is forcing IDRs on the shared encoder and spiking everyone's bitrate (IDR rate-limit regression)"
						fi
					fi
				fi
			fi
			if [ -n "$hsnap0" ]; then
				hsnap1=$(dev_snap)
				if [ -n "$hsnap1" ]; then
					read -r h_rss0 h_fd0 h_thr0 _ _ <<<"$hsnap0"
					read -r h_rss1 h_fd1 h_thr1 _ _ <<<"$hsnap1"
					dr=$(( h_rss1 - h_rss0 ))
					info "  timpsd across the hostile test: RSS ${h_rss0}->${h_rss1}kB (delta ${dr}kB), fds ${h_fd0}->${h_fd1}, threads ${h_thr0}->${h_thr1}"
					if [ "$dr" -lt 4096 ]; then ok "hostile: timpsd memory stayed bounded (${dr}kB delta with a stalled client attached)"
					else warn "hostile: timpsd RSS grew ${dr}kB during the stalled-client test (P-08 estimated ~1.5-2.5MB pinned per stalled client - worth checking the fanqueue cap on a 32MB-class SoC)"; fi
				fi
			fi
		fi
		rm -f "$hdir"/*.mkv
	fi
fi

fi
# --- 14. Restart resilience -------------------------------------------------
if [ "$DO_RESTART" = "1" ] && want 14 restart; then
	hdr "14. Restart resilience"
	if [ -n "$SSH_TARGET" ]; then
		sshx "service timps restart >/dev/null 2>&1 || /etc/init.d/S95timps restart >/dev/null 2>&1" &
		rt0=$(date +%s.%N); recovered=0
		for i in $(seq 1 30); do
			if timeout 6 ffprobe -v error -rtsp_transport tcp -i "$(rtsp_url "$PATH_SUB")" -show_entries format=start_time -of csv=p=0 >/dev/null 2>&1; then
				rt1=$(date +%s.%N); recovered=$(awk -v a="$rt0" -v b="$rt1" 'BEGIN{printf "%.1f",b-a}'); break
			fi; sleep 2
		done
		[ "$recovered" != "0" ] && ok "streamer recovered ${recovered}s after restart" || bad "streamer did not recover within 60s"
	else skip "restart test needs --ssh"; fi
fi

# --- 14b. Fatal-signal handler test (opt-in: --test-crash, DESTRUCTIVE) -----
if want 14b crash fatalsignal; then
hdr "14b. Fatal-signal handler test (opt-in, destructive)"
if [ "$TEST_CRASH" != "1" ]; then
	info "fatal-signal handler test needs --test-crash (DESTRUCTIVE: kills the running timpsd) - skipped"
elif [ -z "$SSH_TARGET" ]; then
	skip "fatal-signal handler test needs --ssh"
else
	echo "  -- DESTRUCTIVE: sending SIGSEGV to the running timpsd, then restarting it --"
	# Key insight (no special build/debug flag needed): sigaction() catches
	# externally-sent signals exactly like a genuine internal fault, so
	# `kill -SEGV $(pidof timpsd)` over SSH exercises the REAL production
	# fatal_signal_handler() in main.c (altstack, /run/timps.crash write,
	# re-raise) - the same code path a real libimp/libaudioProcess crash
	# would take, not a simulated/test-only stand-in.
	pid0=$(sshx "pidof timpsd" 2>/dev/null)
	if [ -z "$pid0" ]; then
		bad "fatal-signal test: timpsd not running before the test (no pid to signal)"
	else
		# this run's own stale-crash check (section 1, preflight) already
		# reported on and renamed any pre-existing /run/timps.crash; clear again here
		# defensively so a leftover from a previous ABORTED --test-crash run
		# can't be mistaken for evidence THIS SIGSEGV produced a crash file
		sshx "rm -f /run/timps.crash" >/dev/null 2>&1
		sshx "kill -SEGV $pid0" >/dev/null 2>&1
		# bounded poll for the process to actually die - same idiom as
		# rot_restart's pidof poll in section 8b, never a raw sleep
		died=0
		for i in $(seq 1 15); do
			sshx "pidof timpsd" >/dev/null 2>&1 || { died=1; break; }
			sleep 1
		done
		if [ "$died" != "1" ]; then
			bad "fatal-signal test: timpsd (pid $pid0) still running 15s after SIGSEGV - handler did not die/re-raise as designed"
		else
			ok "fatal-signal test: timpsd (pid $pid0) died after SIGSEGV"
			# bounded poll for the crash file - the handler's write() happens
			# before the re-raise, but give the filesystem a moment regardless
			crashed=0
			for i in $(seq 1 10); do
				sshx "[ -s /run/timps.crash ]" >/dev/null 2>&1 && { crashed=1; break; }
				sleep 1
			done
			if [ "$crashed" != "1" ]; then
				bad "fatal-signal test: /run/timps.crash was not written within 10s of the SIGSEGV"
			else
				crashtxt=$(sshx "cat /run/timps.crash" 2>/dev/null)
				# main.c's fatal_signal_handler() format: a line containing
				# "*** timpsd FATAL: SIGSEGV si_addr=0x... pc=0x..." followed by
				# "fault address is in: ..." - assert the actual format, not
				# just "the file is non-empty".
				if printf '%s' "$crashtxt" | grep -q "SIGSEGV" && \
				   printf '%s' "$crashtxt" | grep -qE 'si_addr=0x[0-9a-f]+'; then
					ok "fatal-signal test: /run/timps.crash matches the handler's format (SIGSEGV + fault address)"
					info "  $(printf '%s' "$crashtxt" | grep -m1 'fault address is in:')"
				else
					bad "fatal-signal test: /run/timps.crash exists but doesn't match the handler's format (got: $(printf '%s' "$crashtxt" | head -c 200))"
				fi
			fi
		fi
		# restart for real and confirm it comes back HEALTHY - the same
		# /control-responds-not-just-pidof check rot_restart uses in 8b,
		# matching existing script idioms rather than inventing a new one
		sshx "/etc/init.d/S95timps restart >/dev/null 2>&1 || service timps restart >/dev/null 2>&1"
		up=0
		for i in $(seq 1 30); do sshx "pidof timpsd" >/dev/null 2>&1 && { up=1; break; }; sleep 2; done
		if [ "$up" != "1" ]; then
			bad "fatal-signal test: timpsd did not come back up after the post-crash restart"
		else
			healthy=0
			for i in $(seq 1 15); do
				[ "$(curlq 3 -o /dev/null -w '%{http_code}' "$(http_base)/control")" = "200" ] && { healthy=1; break; }
				sleep 2
			done
			[ "$healthy" = "1" ] && ok "fatal-signal test: timpsd came back up and /control responds after the post-crash restart" \
				|| bad "fatal-signal test: timpsd process is up but /control never responded within 30s post-restart"
		fi
		sshx "rm -f /run/timps.crash" >/dev/null 2>&1   # this run's own crash artifact - clear it so it isn't reported as "stale" on the NEXT qa run
	fi
fi
fi

# --- 14c. Real reboot: config / binary / version persistence (opt-in) -------
# The sharpest available test for the 2026-08 fleet incident class: fw_ota.sh
# reported "Firmware flashed successfully" on cameras whose /usr/bin/timpsd had
# demonstrably not changed. Section 1b surfaces the version string, but a
# version string is only evidence about the binary that is currently running -
# it says nothing about whether a config write actually reached flash, or
# whether what comes back after a power cycle is the same software at all.
#
# This reboots the camera for real and then asserts, across the reboot:
#   * a deliberate config change survived  (it reached flash, not just RAM)
#   * /etc/timps.conf's md5 is exactly what we left behind (nothing rewrote or
#     rolled back the config during shutdown/boot)
#   * /usr/bin/timpsd's md5 is UNCHANGED (nothing swapped the binary under us)
#   * the reported version string is unchanged
#   * nothing else in the whole /control document moved (a silent reset of
#     some unrelated setting is exactly the failure that hides for months)
if want 14c reboot; then
hdr "14c. Reboot persistence (opt-in, reboots the camera)"
if [ "$TEST_REBOOT" != "1" ]; then
	info "reboot-persistence test needs --test-reboot (the camera is REBOOTED and is offline for a minute or two) - skipped"
elif [ -z "$SSH_TARGET" ]; then
	skip "reboot-persistence test needs --ssh"
else
	echo "  -- the camera will now REBOOT; expect ~1-2 minutes of downtime --"
	rb_base="$OUTDIR/reboot_before.json"
	if ! curlq 12 "$(http_base)/control" -o "$rb_base" || [ ! -s "$rb_base" ]; then
		bad "reboot test: cannot GET the /control baseline - aborting before touching anything"
	else
		rb_ver0=$(jget "$rb_base" version)
		rb_bin0=$(sshx "md5sum /usr/bin/timpsd 2>/dev/null | cut -d' ' -f1")
		rb_cfg0=$(sshx "md5sum /etc/timps.conf 2>/dev/null | cut -d' ' -f1")
		info "  before: version='${rb_ver0:-?}' timpsd md5=${rb_bin0:-?} timps.conf md5=${rb_cfg0:-?}"

		# one safe, distinctive, persisted change - an OSD text if this camera
		# has a live one (visible, harmless), else the recording filename
		# template (equally persisted, no visual effect)
		rb_key=""; rb_path=""; rb_orig=""; rb_val="qa_reboot_$(date +%s)"
		for s in 0 1; do
			for i in 0 1 2 3; do
				v=$(jget "$rb_base" "osd$s.$i.text")
				if [ -n "$v" ]; then
					rb_key="{\"osd$s\":{\"$i\":{\"text\":\"$rb_val\"}}}"; rb_path="osd$s.$i.text"; rb_orig="$v"; break 2
				fi
			done
		done
		if [ -z "$rb_path" ]; then
			rb_orig=$(jget "$rb_base" record.name)
			rb_key="{\"record\":{\"name\":\"$rb_val\"}}"; rb_path="record.name"
		fi
		code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 12 -u "$HTTP_USER:$HTTP_PASS" \
			-X POST "$(http_base)/control" -d "$rb_key")
		sleep 2
		rb_mid="$OUTDIR/reboot_mid.json"; curlq 12 "$(http_base)/control" -o "$rb_mid"
		rb_cfg1=$(sshx "md5sum /etc/timps.conf 2>/dev/null | cut -d' ' -f1")
		if [ "$(jget "$rb_mid" "$rb_path")" != "$rb_val" ]; then
			bad "reboot test: the pre-reboot change to $rb_path did not even apply (HTTP $code) - nothing meaningful to check across a reboot"
		else
			ok "reboot test: set $rb_path='$rb_val' (was '$rb_orig'), /etc/timps.conf md5 ${rb_cfg0:-?} -> ${rb_cfg1:-?}"
			[ "$rb_cfg1" != "$rb_cfg0" ] || warn "reboot test: /etc/timps.conf md5 did NOT change after a config POST - the value may be living in RAM only (it will not survive the reboot)"

			# --- reboot -------------------------------------------------------
			# detach it: `reboot` tears the ssh session down under us, and a
			# non-zero rc from that is not a failure signal worth acting on
			sshx "(sleep 1; reboot) >/dev/null 2>&1 &" >/dev/null 2>&1 || true
			rb_t0=$(date +%s)
			# first wait for it to actually GO DOWN, so a camera that ignored
			# the reboot request is not mistaken for one that came back fast
			went_down=0
			for i in $(seq 1 30); do
				curlq 3 -o /dev/null "$(http_base)/control" >/dev/null 2>&1 || { went_down=1; break; }
				sleep 2
			done
			[ "$went_down" = "1" ] && info "  camera went down after $(( $(date +%s) - rb_t0 ))s, waiting for it to come back" \
				|| warn "reboot test: the camera never stopped answering - did it reboot at all? (continuing; the checks below still apply)"
			back=0
			for i in $(seq 1 90); do    # up to ~3 min, a real boot is much slower than a daemon restart
				[ "$(curlq 3 -o /dev/null -w '%{http_code}' "$(http_base)/control" 2>/dev/null)" = "200" ] && { back=1; break; }
				sleep 2
			done
			if [ "$back" != "1" ]; then
				bad "reboot test: the camera did not answer /control within ~3 minutes of the reboot - it did not come back on its own"
			else
				rb_secs=$(( $(date +%s) - rb_t0 ))
				ok "reboot test: camera came back and /control answered ${rb_secs}s after the reboot was issued"
				rb_after="$OUTDIR/reboot_after.json"; curlq 12 "$(http_base)/control" -o "$rb_after"
				rb_ver1=$(jget "$rb_after" version)
				rb_bin1=$(sshx "md5sum /usr/bin/timpsd 2>/dev/null | cut -d' ' -f1")
				rb_cfg2=$(sshx "md5sum /etc/timps.conf 2>/dev/null | cut -d' ' -f1")
				info "  after:  version='${rb_ver1:-?}' timpsd md5=${rb_bin1:-?} timps.conf md5=${rb_cfg2:-?}"

				[ -n "$rb_bin0" ] && [ "$rb_bin1" = "$rb_bin0" ] \
					&& ok "reboot: /usr/bin/timpsd is byte-identical across the reboot (md5 $rb_bin1)" \
					|| bad "reboot: /usr/bin/timpsd CHANGED across a plain reboot (${rb_bin0:-?} -> ${rb_bin1:-?}) - something is rewriting the binary at boot (pending-flash artifact?)"
				[ -n "$rb_ver0" ] && [ "$rb_ver1" = "$rb_ver0" ] \
					&& ok "reboot: reported version unchanged ('$rb_ver1')" \
					|| bad "reboot: reported version changed across a plain reboot ('${rb_ver0:-?}' -> '${rb_ver1:-?}')"
				[ -n "$rb_cfg1" ] && [ "$rb_cfg2" = "$rb_cfg1" ] \
					&& ok "reboot: /etc/timps.conf is byte-identical to what we left before the reboot (md5 $rb_cfg2)" \
					|| bad "reboot: /etc/timps.conf md5 changed across the reboot (${rb_cfg1:-?} -> ${rb_cfg2:-?}) - the config was rewritten or rolled back during shutdown/boot"
				got=$(jget "$rb_after" "$rb_path")
				[ "$got" = "$rb_val" ] \
					&& ok "reboot: the config change SURVIVED ($rb_path='$got') - the write really reached flash" \
					|| bad "reboot: the config change was LOST across the reboot ($rb_path='${got:-}', expected '$rb_val') - /control accepted and echoed a value that never reached flash"

				# --- whole-document diff -----------------------------------
				# Everything except the one key we changed (and the inherently
				# volatile status fields) must read identically. This is the
				# check that catches an unrelated setting silently resetting at
				# boot - the kind of thing nobody notices for months.
				if have python3; then
					python3 - "$rb_base" "$rb_after" "$rb_path" <<'PY' > "$OUTDIR/reboot_diff.txt" 2>/dev/null
import json,sys
a=json.load(open(sys.argv[1])); b=json.load(open(sys.argv[2])); changed=sys.argv[3]
# live/status values that legitimately differ after a reboot. VOL_ANY matches
# any path component (covers list elements, whose own last component is an
# index); VOL_PATH is for names that are ambiguous - "mode" is a live day/night
# state under daynight but a persisted setting under record, so only the
# specific status paths are excused.
VOL_ANY={"uptime_s","clients","subs","fps","kbps","bytes","free_mb","file","last_file",
         "count","last_t","total_gain","exposure","ae_luma","night_baseline","day_trigger",
         "stalled","active","recording","registered","left_pics","work_done",
         "cur_packs","left_stream_bytes","left_stream_frames","ave_bitrate",
         "drop_frames","drop_bytes","sun_computed_sunrise","sun_computed_sunset","temp"}
VOL_PATH={"daynight.mode","daynight.enabled","daynight.brightness","motion.enabled",
          "image.running_mode"}
out=[]
def walk(x,y,path=""):
    if isinstance(x,dict) and isinstance(y,dict):
        for k in sorted(set(x)|set(y)):
            walk(x.get(k),y.get(k),f"{path}.{k}" if path else k)
    elif isinstance(x,list) and isinstance(y,list):
        for i in range(max(len(x),len(y))):
            walk(x[i] if i<len(x) else None, y[i] if i<len(y) else None, f"{path}.{i}")
    elif x!=y:
        if path==changed or path in VOL_PATH: return
        if any(c in VOL_ANY for c in path.split(".")): return
        out.append(f"{path}: {x!r} -> {y!r}")
walk(a,b)
print("\n".join(out))
PY
					nd=$(grep -c . "$OUTDIR/reboot_diff.txt" 2>/dev/null); nd=${nd:-0}
					if [ "$nd" -eq 0 ]; then
						ok "reboot: the entire /control document is otherwise identical across the reboot (no setting silently reset at boot)"
					else
						bad "reboot: ${nd} /control field(s) changed across the reboot that we did not touch - a setting is being silently reset at boot. See $OUTDIR/reboot_diff.txt:"
						head -10 "$OUTDIR/reboot_diff.txt" | sed 's/^/    /' | tee -a "$SUMMARY"
					fi
				else
					info "  whole-document reboot diff needs python3 - skipped (the targeted checks above still ran)"
				fi

				# kernel boot log: the reboot is the one moment a driver-level
				# problem is most likely to appear, and logread will not have it
				rbd="$OUTDIR/reboot_dmesg.txt"
				# NOTE this runs right after a boot, so the buffer is nothing BUT
				# the boot preamble - which dmesg_capture excludes by design (see
				# its comment). Expect 0 findings here on a healthy camera; the
				# full untrimmed log is still saved for a human to read.
				if dm_res=$(dmesg_capture "$rbd" 300); then
					read -r kerr dm_scanned _ dm_rt <<<"$dm_res"
					[ "${kerr:-0}" -eq 0 ] && info "  post-reboot kernel log clean (${dm_scanned} post-bring-up line(s) scanned; full log saved to $rbd)" \
						|| warn "reboot: ${kerr} error-ish kernel log line(s) logged after sensor bring-up - see $dm_rt"
				fi

				# --- restore ------------------------------------------------
				rcode=$(curl -s -o /dev/null -w '%{http_code}' --max-time 12 -u "$HTTP_USER:$HTTP_PASS" \
					-X POST "$(http_base)/control" -d "$(printf '%s' "$rb_key" | sed "s|$rb_val|$rb_orig|")")
				sleep 2
				rrj="$OUTDIR/reboot_restore.json"; curlq 12 "$(http_base)/control" -o "$rrj"
				[ "$(jget "$rrj" "$rb_path")" = "$rb_orig" ] \
					&& info "  restored $rb_path='$rb_orig'" \
					|| warn "reboot test: could not restore $rb_path to '$rb_orig' (HTTP $rcode) - the camera is left with the probe value"
			fi
		fi
	fi
fi

fi
# --- 15. Soak ---------------------------------------------------------------
if [ "${SOAK_DUR:-0}" -gt 0 ] && want 15 soak; then
	hdr "15. Soak (${SOAK_DUR}s continuous, ${SOAK_SAMPLE}s slices)"
	soaklog="$OUTDIR/soak.log"; : > "$soaklog"
	slice="$SOAK_SAMPLE"; n_slices=$(( SOAK_DUR / slice )); [ "$n_slices" -lt 1 ] && n_slices=1
	err_total=0; bad_slices=0; rss_first=""; rss_last=""
	rec="$OUTDIR/rec_soak.mkv"; rlog="$OUTDIR/rec_soak.log"
	# Per-slice resource series. RSS alone (the old <2MB gate) cannot see the
	# leaks this codebase actually has: an unreaped session costs an fd pair, a
	# thread and a hub subscription - kilobytes. Judged on SHAPE (never goes
	# back down) rather than on any absolute count, since the healthy number
	# depends on client count, build and SoC. CPU is measured across the slice
	# itself by bracketing the recording, so it costs no extra wall time.
	fd_series="$OUTDIR/soak_fds.txt"; thr_series="$OUTDIR/soak_threads.txt"
	cpu_series="$OUTDIR/soak_cpu.txt"
	: > "$fd_series"; : > "$thr_series"; : > "$cpu_series"
	for s in $(seq 1 "$n_slices"); do
		snap0=""; [ -n "$SSH_TARGET" ] && snap0=$(dev_snap)
		timeout -k 5 "$((slice+6))" ffmpeg -hide_banner -nostdin -y -loglevel warning -rtsp_transport "$RTSP_TRANSPORT" \
			-i "$(rtsp_url "$PATH_MAIN")" -t "$slice" -c copy "$rec" </dev/null 2>"$rlog" || true
		[ -s "$rec" ] || bad_slices=$((bad_slices+1))
		# shared FFWARN_RE via ffwarn_count (this copy used to be missing
		# decode_slice|missed - see the helper's comment on pattern drift)
		e=$(ffwarn_count "$rlog")
		err_total=$((err_total+e))
		rss=""; nfd=""; nthr=""; cpu=""
		if [ -n "$SSH_TARGET" ]; then
			snap1=$(dev_snap)
			if [ -n "$snap1" ]; then
				read -r rss nfd nthr _ _ <<<"$snap1"
				cpu=$(dev_cpu_between "${snap0:-}" "$snap1")
				[ -z "$rss_first" ] && rss_first="$rss"; rss_last="$rss"
				echo "$nfd"  >> "$fd_series"
				echo "$nthr" >> "$thr_series"
				echo "$cpu"  >> "$cpu_series"
			fi
		fi
		echo "$(date +%H:%M:%S) slice $s/$n_slices err=$e rss=${rss:-?}kB fds=${nfd:-?} threads=${nthr:-?} cpu=${cpu:-?}% empty=$([ -s "$rec" ] && echo 0 || echo 1)" >> "$soaklog"
		rm -f "$rec"
		printf '\r  soak %d/%d  errors=%d  bad_slices=%d  rss=%skB fd=%s thr=%s cpu=%s%%     ' "$s" "$n_slices" "$err_total" "$bad_slices" "${rss:-?}" "${nfd:-?}" "${nthr:-?}" "${cpu:-?}"
	done
	echo
	[ "$bad_slices" -eq 0 ] && ok "soak: all $n_slices slices captured data" \
		|| bad "soak: $bad_slices/$n_slices slices captured NO data (stream dropped)"
	[ "$err_total" -eq 0 ] && ok "soak: zero decode/timestamp errors over ${SOAK_DUR}s" \
		|| warn "soak: $err_total decode/timestamp warnings over ${SOAK_DUR}s (see $soaklog)"
	if [ -n "$rss_first" ] && [ -n "$rss_last" ]; then
		grow=$(( rss_last - rss_first ))
		info "timpsd RSS ${rss_first}kB -> ${rss_last}kB (delta ${grow}kB over ${SOAK_DUR}s)"
		[ "$grow" -lt 2048 ] && ok "soak: no significant memory growth (<2MB)" \
			|| warn "soak: timpsd RSS grew ${grow}kB (possible leak - see $soaklog)"
	fi
	# fd / thread leak: judged on shape, not magnitude. A descriptor+thread
	# that is taken per session and never given back produces a series that
	# only ever rises - a few kB of RSS, invisible to the gate above, but the
	# exact fingerprint of every unreaped-session bug this project has had.
	for pair in "fds:$fd_series" "threads:$thr_series"; do
		what="${pair%%:*}"; sf="${pair##*:}"
		[ -s "$sf" ] || continue
		read -r ln lfirst llast ldelta lnondec lup <<<"$(leak_trend < "$sf")"
		[ "${ln:-0}" -ge 4 ] || { info "  soak $what: only ${ln:-0} sample(s) - too few to judge a trend"; continue; }
		if [ "$ldelta" -le 0 ]; then
			ok "soak $what: ${lfirst} -> ${llast} over $ln slices (no growth)"
		elif [ "$lnondec" = "1" ]; then
			bad "soak $what: ${lfirst} -> ${llast} (+${ldelta}) and the count NEVER went back down across $ln slices - monotonic growth is the per-session leak signature (fd/thread/hub-sub never released)"
		else
			warn "soak $what: ${lfirst} -> ${llast} (+${ldelta}) over $ln slices, but the series fluctuates (${lup} rises) - churn rather than a clean leak; check $soaklog"
		fi
	done
	if [ -s "$cpu_series" ]; then
		read -r cn cq1 cq4 cgrowth _ _ _ _ _ _ <<<"$(awk '{n++;s[n]=$1+0}
			END{ if(n<1){print "0 0 0 0 0 0 0 0 0 0"; exit}
			     q=int(n/4); if(q<1)q=1;
			     for(i=1;i<=q;i++)f+=s[i]; for(i=n-q+1;i<=n;i++)l+=s[i]; f/=q; l/=q;
			     printf "%d %.1f %.1f %.1f 0 0 0 0 0 0\n", n, f, l, l-f }' "$cpu_series")"
		info "timpsd CPU across soak: first-quarter avg ${cq1}% -> last-quarter avg ${cq4}% ($cn slices, see $cpu_series)"
		if [ "${cn:-0}" -ge 4 ]; then
			if fcmp "$cgrowth" ge 15; then bad "soak: timpsd CPU rose ${cgrowth} points across the soak (${cq1}% -> ${cq4}%) at constant load - work per client is growing (spin/backlog)"
			elif fcmp "$cgrowth" ge 5; then warn "soak: timpsd CPU drifted up ${cgrowth} points across the soak (${cq1}% -> ${cq4}%) at constant load"
			else ok "soak: timpsd CPU stable across the soak (${cq1}% -> ${cq4}%)"; fi
		fi
	fi
	# kernel-side context, only when something already went wrong - a driver/DMA
	# level stall shows up in dmesg and nowhere in logread or the capture logs
	if [ -n "$SSH_TARGET" ] && { [ "$bad_slices" -gt 0 ] || [ "$err_total" -gt 0 ]; }; then
		sk="$OUTDIR/soak_dmesg.txt"
		if dm_res=$(dmesg_capture "$sk" 200); then
			read -r kerr dm_scanned _ _ <<<"$dm_res"
			info "  soak had trouble - kernel log tail saved to $sk (${kerr} error-ish line(s) in ${dm_scanned} post-bring-up line(s))"
		fi
	fi
fi

# --- 15b. Long-session A/V drift --------------------------------------------
# Coverage gap this closes (found 2026-08-10 while validating the rtp.c fix
# "re-sample fresh clock for RTCP SR"): section 3 measures A/V skew over a
# fresh ~30s capture (too short to see slow accumulation) and section 15's soak
# reconnects every slice, which resets the per-session pts anchor - so NEITHER
# can observe drift that only builds up inside ONE long-lived session, which is
# precisely the failure mode of a stale-NTP<->RTP-pairing bug.
#
# Method: a SINGLE unbroken ffmpeg RTSP session, chopped into checkpoints by
# the segment muxer with -reset_timestamps 0 so each segment's packet
# timestamps stay relative to the ORIGINAL session start. That is the whole
# trick: without it every segment restarts near zero and each one reads ~0
# drift forever. Segments are analysed (and deleted) as they complete, so the
# run prints a live trend and disk use stays bounded on a multi-hour capture.
#
# The verdict is on the TREND, not on any single checkpoint: a drift that
# creeps from 0.02s to 0.35s over two hours is never above section 3's 0.40s
# "bad" line at any instant, yet it is unambiguously the bug.
if [ "${DRIFT_DUR:-0}" -gt 0 ] && want 15b drift; then
	hdr "15b. Long-session A/V drift (ONE ${DRIFT_DUR}s RTSP connection, ${DRIFT_SEG}s checkpoints)"
	ddir="$OUTDIR/drift"; mkdir -p "$ddir"
	dlog="$OUTDIR/drift_ffmpeg.log"; dseries="$OUTDIR/drift_series.txt"
	printf '# seg t_media_end_s skew_end_s skew_delta_in_seg_s\n' > "$dseries"
	nseg_expect=$(( DRIFT_DUR / DRIFT_SEG ))
	[ "$nseg_expect" -ge 4 ] || warn "drift: --drift-dur $DRIFT_DUR with --drift-seg $DRIFT_SEG gives only $nseg_expect checkpoint(s); the trend verdict needs >=4 (raise --drift-dur or lower --drift-seg)"
	info "drift: one unbroken RTSP ($RTSP_TRANSPORT) session, segmented every ${DRIFT_SEG}s with -reset_timestamps 0 so skew stays comparable ACROSS checkpoints"
	d_t0=$(date +%s)
	# No -copyts (same reasoning as analyze_stream): skew is a difference
	# between two tracks on the recorded timeline, so the absolute offset is
	# irrelevant - what matters is that the muxer does not re-zero per segment.
	timeout -k 15 "$((DRIFT_DUR+60))" ffmpeg -hide_banner -nostdin -y -loglevel warning \
		-rtsp_transport "$RTSP_TRANSPORT" -i "$(rtsp_url "$PATH_MAIN")" -t "$DRIFT_DUR" \
		-map 0 -c copy -f segment -segment_time "$DRIFT_SEG" -reset_timestamps 0 \
		-segment_format matroska "$ddir/seg_%04d.mkv" </dev/null 2>"$dlog" &
	d_pid=$!
	d_next=0; d_pts=0; d_noav=0; d_abort=0
	while :; do
		d_alive=1; kill -0 "$d_pid" 2>/dev/null || d_alive=0
		while :; do
			d_cur=$(printf '%s/seg_%04d.mkv' "$ddir" "$d_next")
			d_nxt=$(printf '%s/seg_%04d.mkv' "$ddir" "$((d_next+1))")
			[ -f "$d_cur" ] || break
			# A segment is only complete once its SUCCESSOR exists (the muxer
			# opens the next file at the boundary) - or once ffmpeg is gone, at
			# which point everything on disk is closed. Never probe the file
			# still being written to.
			[ -f "$d_nxt" ] || [ "$d_alive" = "0" ] || break
			d_csv="$ddir/pkts.csv"
			ffprobe -v error -show_entries packet=codec_type,pts_time -of csv=p=0 "$d_cur" 2>/dev/null > "$d_csv"
			read -r d_ss d_se d_dd <<<"$(av_skew "$d_csv")"
			read -r d_nv d_na d_tend <<<"$(awk -F, '
				$1=="video"{nv++; t=$2+0} $1=="audio"{na++}
				END{printf "%d %d %.1f", nv, na, t}' "$d_csv")"
			if [ "${d_nv:-0}" -eq 0 ] || [ "${d_na:-0}" -eq 0 ] || [ -z "${d_se:-}" ]; then
				d_noav=$((d_noav+1))
				info "  checkpoint $d_next: video=${d_nv:-0} audio=${d_na:-0} pkts - no A/V pair here, skew not measurable"
			else
				d_pts=$((d_pts+1))
				printf '%d %s %s %s\n' "$d_next" "$d_tend" "$d_se" "$d_dd" >> "$dseries"
				info "  checkpoint $d_next  t=${d_tend}s  A/V skew=${d_se}s  (moved ${d_dd}s within this segment; v=$d_nv a=$d_na pkts)"
				d_abs=$(awk -v d="$d_se" 'BEGIN{printf "%.3f", (d<0?-d:d)}')
				if fcmp "$d_abs" ge "${DRIFT_ABORT:-1.0}"; then
					bad "drift: |A/V skew| hit ${d_se}s at checkpoint $d_next (t=${d_tend}s) - past the ${DRIFT_ABORT:-1.0}s abort line, ending the session early (the defect is already demonstrated)"
					d_abort=1
				fi
			fi
			rm -f "$d_cur" "$d_csv"     # analysed -> reclaim disk immediately
			d_next=$((d_next+1))
			[ "$d_abort" = "1" ] && break
		done
		[ "$d_abort" = "1" ] && { kill -TERM "$d_pid" 2>/dev/null; sleep 2; kill -KILL "$d_pid" 2>/dev/null; break; }
		[ "$d_alive" = "0" ] && break
		sleep 10
	done
	wait "$d_pid" 2>/dev/null
	d_wall=$(( $(date +%s) - d_t0 ))

	# Did the single session actually survive? A short wall time with no abort
	# means the connection dropped - itself a finding, and it invalidates the
	# trend below.
	if [ "$d_abort" != "1" ] && [ "$d_wall" -lt "$((DRIFT_DUR - DRIFT_SEG))" ]; then
		bad "drift: the session ended after ${d_wall}s of the requested ${DRIFT_DUR}s - the single RTSP connection did not stay up (see $dlog)"
	elif [ "$d_abort" != "1" ]; then
		ok "drift: one RTSP connection held open for ${d_wall}s without reconnecting"
	fi
	[ "$d_noav" -eq 0 ] || warn "drift: $d_noav checkpoint(s) had no usable audio+video pair (stream stalled there, or this stream carries no audio)"
	d_ffe=$(ffwarn_count "$dlog")
	[ "$d_ffe" -eq 0 ] && ok "drift: no ffmpeg decode/timestamp warnings over the whole session" \
		|| warn "drift: $d_ffe ffmpeg decode/timestamp warning(s) during the session (see $dlog)"

	if [ "$d_pts" -lt 4 ]; then
		warn "drift: only $d_pts usable checkpoint(s) - not enough to judge a trend (need >=4); per-checkpoint values are in $dseries"
	else
		# Trend maths, all in one awk pass over the checkpoint series:
		#   growth  = mean(last quarter) - mean(first quarter). Quarter means
		#             instead of first-vs-last single values so ordinary jitter
		#             averages out; a real accumulation survives it.
		#   slope   = least-squares seconds-of-skew per hour of session, the
		#             same signal expressed as a rate (comparable across runs
		#             of different --drift-dur).
		#   monof   = fraction of consecutive checkpoint-to-checkpoint steps
		#             that moved in the trend's direction. This is the
		#             practical stand-in for "monotonically growing": real
		#             captures jitter, so demanding every single step increase
		#             would never fire. ~0.5 = noise, ->1.0 = a ratchet.
		#   band    = max-min, and maxabs = worst |skew| seen, for the separate
		#             absolute-level judgement below.
		read -r d_n d_q1 d_q4 d_growth d_agrowth d_rate d_mono d_maxabs d_band d_hours <<<"$(awk '
			/^#/{next} {n++; t[n]=$2+0; s[n]=$3+0}
			END{
				if(n<2){print "0 0 0 0 0 0 0 0 0 0"; exit}
				q=int(n/4); if(q<1)q=1;
				for(i=1;i<=q;i++)f+=s[i];
				for(i=n-q+1;i<=n;i++)l+=s[i];
				f/=q; l/=q; growth=l-f; ag=(growth<0?-growth:growth);
				for(i=1;i<=n;i++){sx+=t[i];sy+=s[i];sxx+=t[i]*t[i];sxy+=t[i]*s[i]}
				den=n*sxx-sx*sx; slope=(den!=0)?(n*sxy-sx*sy)/den:0;
				dir=(growth<0?-1:1); same=0;
				for(i=2;i<=n;i++){d=s[i]-s[i-1]; if(d*dir>0)same++}
				mono=(n>1)?same/(n-1):0;
				mx=s[1]; mn=s[1]; mabs=0;
				for(i=1;i<=n;i++){if(s[i]>mx)mx=s[i]; if(s[i]<mn)mn=s[i]; a=(s[i]<0?-s[i]:s[i]); if(a>mabs)mabs=a}
				span=t[n]-t[1]; hrs=(span>0)?span/3600:0;
				printf "%d %.3f %.3f %.3f %.3f %.3f %.2f %.3f %.3f %.2f\n", n, f, l, growth, ag, slope*3600, mono, mabs, mx-mn, hrs;
			}' "$dseries")"
		d_monopct=$(awk -v m="$d_mono" 'BEGIN{printf "%d", m*100}')
		info "drift trend over ${d_hours}h / $d_n checkpoints: first-quarter avg=${d_q1}s -> last-quarter avg=${d_q4}s (growth ${d_growth}s, slope ${d_rate}s/h extrapolated from a ${d_hours}h fit, ${d_monopct}% of steps moved with the trend, band ${d_band}s, max|skew| ${d_maxabs}s)"
		info "  per-checkpoint values: $dseries"
		# TREND verdict - the actual signature of the RTCP-SR class of bug.
		# Thresholds are on absolute growth over the session (what a viewer
		# experiences) and are cross-checked against monotonicity, so a large
		# but bursty/bounded wobble does not read the same as a slow ratchet.
		if fcmp "$d_agrowth" ge 0.50; then
			bad "drift TREND: A/V skew moved ${d_growth}s across the session (${d_rate}s/h) - unmistakably accumulating within one connection"
		elif fcmp "$d_agrowth" ge 0.25 && fcmp "$d_mono" ge 0.60; then
			bad "drift TREND: A/V skew grew ${d_growth}s and ${d_monopct}% of steps moved the same way - drift is accumulating, not jitter"
		elif fcmp "$d_agrowth" ge 0.15 && fcmp "$d_mono" ge 0.85; then
			bad "drift TREND: A/V skew grew only ${d_growth}s but ${d_monopct}% of steps moved the same way - a near-perfect ratchet, i.e. slow accumulation that a single-snapshot threshold would never catch"
		elif fcmp "$d_agrowth" ge 0.10; then
			warn "drift TREND: A/V skew moved ${d_growth}s across the session (${d_rate}s/h, ${d_monopct}% of steps with the trend) - watch it; re-run longer to tell a slow ratchet from a bounded wobble"
		else
			ok "drift TREND: A/V skew stayed bounded (${d_growth}s between first- and last-quarter averages, ${d_rate}s/h) - noisy but not accumulating"
		fi
		# ABSOLUTE-level verdict, same thresholds section 3 applies to a single
		# capture - reported separately because a large but CONSTANT offset is a
		# different defect from a growing one.
		if fcmp "$d_maxabs" le 0.15; then ok "drift LEVEL: worst A/V skew ${d_maxabs}s over the whole session (in sync)"
		elif fcmp "$d_maxabs" le 0.40; then warn "drift LEVEL: worst A/V skew ${d_maxabs}s (marginal offset; see the TREND verdict for whether it is growing)"
		else bad "drift LEVEL: worst A/V skew ${d_maxabs}s (out of sync)"; fi
	fi
	# same rm discipline as the soak loop - segments are analysed and deleted as
	# they complete, this only sweeps an aborted run's leftovers
	rm -f "$ddir"/seg_*.mkv "$ddir"/pkts*.csv
	rmdir "$ddir" 2>/dev/null || true
fi

# --- 16. On-device (SSH) ----------------------------------------------------
if [ -n "$SSH_TARGET" ] && want 16 ssh; then
	hdr "16. On-device checks (SSH)"
	ver=$(sshx "logread 2>/dev/null | grep -oE 'timps v[0-9.]+' | tail -1")
	[ -n "$ver" ] && ok "running $ver" || info "version string not found in logread"
	up=$(sshx "pidof timpsd >/dev/null && echo yes")
	[ "$up" = "yes" ] && ok "timpsd process alive" || bad "timpsd not running"
	# error-ish lines, minus unrelated-daemon noise (dropbear connection churn
	# from our own SSH probes, telegrambot with no config, etc.) so a real timps
	# fault isn't buried under benign matches. "re-asserting" (the daynight
	# ISP running_mode latch-kick retry, working exactly as designed) contains
	# "assert" as a bare substring and must be excluded too - found by hand
	# after this pattern flagged a perfectly healthy camera.
	errs=$(sshx "logread 2>/dev/null | grep -iE 'error|fail|assert|segfault|oom|IMP_.*failed' | grep -cviE 'dropbear|telegrambot|Exited normally|before auth|[0-9]+ fails|re-asserting'")
	[ "${errs:-0}" -le 2 ] && ok "logread: ${errs:-0} error-ish lines" || warn "logread: ${errs} error-ish lines (review with: logread | grep -iE 'error|fail')"

	# --- watchdog escalation: SILENT LIMBO, always a FAIL ---------------------
	# The generic error-ish grep above cannot match ANY of these: the daemon's
	# real escalation messages contain none of error/fail/assert/segfault/oom
	# ("chn0: encoder dead after N consecutive misses", "PollingStream idle
	# (rc=..., miss#N) - encoder emits no frames", "N consecutive
	# forced-recovery cycles never produced a frame - giving up on this
	# channel", "no audio frames received - disabling audio input"). Literals
	# verified against src/hal/hal_ingenic.c:1520,1540,1547,2363,2374,2380,
	# 3057,3089. This is the state where timpsd is UP, /control answers 200,
	# ports are open, and the camera produces nothing - the failure mode most
	# likely to be reported by a human as "the camera just stopped" and the
	# least likely to be caught by anything else in this script. Never a WARN.
	wd=$(sshx "logread 2>/dev/null | grep -cE 'encoder dead|PollingStream idle|forced-recovery|no audio frames received|giving up on this channel'")
	if [ "${wd:-0}" -eq 0 ]; then
		ok "logread: no watchdog-escalation lines (no encoder-dead / PollingStream-idle / forced-recovery / audio-disabled events)"
	else
		bad "logread: ${wd} watchdog-escalation line(s) - the daemon hit encoder-dead / PollingStream-idle / forced-recovery / no-audio limbo. First few:"
		sshx "logread 2>/dev/null | grep -E 'encoder dead|PollingStream idle|forced-recovery|no audio frames received|giving up on this channel' | tail -5" 2>/dev/null | sed 's/^/    /' | tee -a "$SUMMARY"
	fi

	# --- dmesg (kernel ring buffer), a surface logread does not cover ---------
	dmg="$OUTDIR/dmesg_tail.txt"
	if ! dm_res=$(dmesg_capture "$dmg" 300); then
		info "  dmesg not readable on this device - kernel-level check skipped"
	else
		read -r kerr dm_scanned dm_anchored dm_rt <<<"$dm_res"
		info "  kernel log tail saved to $dmg (driver/ISP-level messages; logread does not carry these)"
		if [ "$dm_anchored" = "1" ]; then
			info "  scanning the ${dm_scanned} line(s) logged AFTER sensor bring-up; the boot preamble is excluded by design (it always carries benign 'error'/'panic'/'watchdog' strings on these boards)"
		else
			info "  no boot marker in the buffer (it has wrapped past boot) - scanning all ${dm_scanned} captured line(s) as runtime messages"
		fi
		if [ "$kerr" -eq 0 ]; then
			[ "$dm_scanned" -eq 0 ] \
				&& ok "dmesg: the kernel logged NOTHING after the sensor started streaming (no driver/ISP complaints during this run)" \
				|| ok "dmesg: no kernel/driver error lines in the ${dm_scanned} post-boot line(s)"
		elif [ "$kerr" -le 3 ]; then warn "dmesg: ${kerr} kernel/driver error-ish line(s) logged DURING streaming - review $dm_rt"
		else bad "dmesg: ${kerr} kernel/driver error-ish lines logged DURING streaming - review $dm_rt (driver/DMA/ISP-level trouble is invisible to logread)"; fi
		[ "$kerr" -gt 0 ] && grep -iE "$DMESG_BAD_RE" "$dm_rt" | grep -ivE "$DMESG_BENIGN_RE" | tail -5 | sed 's/^/    /' | tee -a "$SUMMARY"
	fi

	# --- idle CPU / fd / thread baseline --------------------------------------
	# Section 16's own banner has always claimed "timpsd RSS/CPU" but CPU was
	# never sampled anywhere. At this point in the run every test client is
	# gone, so this is a genuine zero-client baseline: with the on-demand
	# pipeline stopped, timpsd should be close to idle. A hot spin (the
	# historical httpd 5Hz busy-discard loop, or a `continue` with no usleep)
	# shows up here and NOWHERE else - it degrades no stream and grows no RSS.
	idle=$(dev_proc_sample 5)
	if [ -n "$idle" ]; then
		read -r i_rss i_fd i_thr i_cpu <<<"$idle"
		info "timpsd at rest: RSS ${i_rss}kB, fds ${i_fd}, threads ${i_thr}, CPU ${i_cpu}% (5s window, zero clients)"
		if fcmp "$i_cpu" le 15; then ok "timpsd idle CPU ${i_cpu}% with no clients"
		elif fcmp "$i_cpu" le 40; then warn "timpsd idle CPU ${i_cpu}% with no clients (expected near-idle - possible busy-wait loop)"
		else bad "timpsd idle CPU ${i_cpu}% with NO clients connected - something is spinning (busy-wait / missing usleep)"; fi
	fi
	# config integrity: glued lines (two '=') or duplicate keys
	glued=$(sshx "sed 's/#.*//' /etc/timps.conf 2>/dev/null | grep -cE '=[^=]*='")
	dup=$(sshx "grep -vE '^[[:space:]]*#' /etc/timps.conf 2>/dev/null | sed 's/=.*//; s/[[:space:]]//g' | sort | uniq -d | grep -c .")
	[ "${glued:-0}" -eq 0 ] && ok "/etc/timps.conf: no glued 'a=b c=d' lines" || bad "/etc/timps.conf has ${glued} glued line(s) - config-write bug"
	[ "${dup:-0}" -eq 0 ] && ok "/etc/timps.conf: no duplicate keys" || warn "/etc/timps.conf has ${dup} duplicate key(s)"
	# rapid-write stress then re-check integrity.
	# Baseline the toggled key FIRST: the 20 unsorted background POSTs land in
	# arbitrary order, so whichever of agc=0/1 happens to land last gets
	# PERSISTED - without a restore that is a stranded setting, the exact
	# class (cam-K 2026-08-02) the LV_PENDING traps in 8b/8e/9 exist to
	# prevent, quietly reintroduced by this section's own stress load.
	agc_bj="$OUTDIR/agc_base.json"; curlq 8 "$(http_base)/control" -o "$agc_bj" 2>/dev/null || true
	agc0=$(jget "$agc_bj" audio.agc)
	info "  config-write stress: 20 rapid /control writes..."
	for i in $(seq 1 20); do
		curl -s -o /dev/null --max-time 5 -u "$HTTP_USER:$HTTP_PASS" -X POST "$(http_base)/control" \
			-d "{\"audio\":{\"agc\":$((i%2))}}" &
	done; wait 2>/dev/null
	glued2=$(sshx "sed 's/#.*//' /etc/timps.conf 2>/dev/null | grep -cE '=[^=]*='")
	[ "${glued2:-0}" -eq 0 ] && ok "after 20 rapid writes: config still clean (no glued lines)" || bad "rapid writes corrupted /etc/timps.conf (${glued2} glued) - config race not fixed"
	# restore agc to its pre-stress value and VERIFY (one retry, same
	# transient-failure allowance as ov_clamp_test's restore)
	if [ -n "${agc0:-}" ]; then
		curl -s -o /dev/null --max-time 8 -u "$HTTP_USER:$HTTP_PASS" -X POST "$(http_base)/control" \
			-d "{\"audio\":{\"agc\":$agc0}}" >/dev/null
		agc_rj="$OUTDIR/agc_restore.json"; curlq 8 "$(http_base)/control" -o "$agc_rj" 2>/dev/null || true
		if [ "$(jget "$agc_rj" audio.agc)" != "$agc0" ]; then
			curl -s -o /dev/null --max-time 8 -u "$HTTP_USER:$HTTP_PASS" -X POST "$(http_base)/control" \
				-d "{\"audio\":{\"agc\":$agc0}}" >/dev/null
			curlq 8 "$(http_base)/control" -o "$agc_rj" 2>/dev/null || true
		fi
		[ "$(jget "$agc_rj" audio.agc)" = "$agc0" ] \
			&& info "  restored audio.agc=$agc0 after the stress toggles" \
			|| warn "config-write stress: could not restore audio.agc to $agc0 - the camera keeps whichever toggle landed last"
	else
		warn "config-write stress: could not read audio.agc beforehand - the last of the 20 random toggles stays persisted (0 or 1)"
	fi
	# The agc toggles above are the exact live-DSP-toggle crash reproducer.
	# v1.4.5 made agc/ns/high_pass restart-required (persist-only) because
	# toggling them live raced libimp's internal audio thread -> UAF/SIGSEGV in
	# libaudioProcess.so. Confirm the daemon survived and treats them as
	# persist-only (no live-apply), not the removed v1.4.4 "queued" deferral.
	up2=$(sshx "pidof timpsd >/dev/null && echo yes")
	[ "$up2" = "yes" ] && ok "timpsd alive after AGC-toggle/config-write stress" \
		|| bad "timpsd DIED during rapid agc /control writes - live-DSP-toggle UAF regression"
	seg=$(sshx "dmesg 2>/dev/null | grep -cE 'libaudioProcess|SIGSEGV to timpsd|do_page_fault[^\n]*timpsd'")
	[ "${seg:-0}" -eq 0 ] && ok "no timpsd segfault signature in dmesg" \
		|| bad "dmesg shows timpsd segfault (${seg} lines) - libimp AGC/NS/HPF race back?"
	q=$(sshx "logread 2>/dev/null | grep -c 'queued for audio thread'")
	[ "${q:-0}" -eq 0 ] && ok "agc/ns/high_pass are persist-only (no live 'queued' applies)" \
		|| warn "${q} 'queued for audio thread' lines - build predates v1.4.5 (buggy deferred path)"
fi

# ----------------------------------------------------------------------------- summary
hdr "SUMMARY"
log "PASS=$PASS  WARN=$WARN  FAIL=$FAIL  SKIP=$SKIP"
log "logs + recordings in: $OUTDIR"
if [ "$FAIL" -gt 0 ]; then log "${c_red}RESULT: FAIL${c_rst}"; QA_EXIT=2
elif [ "$WARN" -gt 0 ]; then log "${c_yel}RESULT: PASS with warnings${c_rst}"; QA_EXIT=1
else log "${c_grn}RESULT: PASS${c_rst}"; QA_EXIT=0; fi

# Every run gets an HTML report - not an opt-in extra, so a report never goes
# missing just because generating it after the fact was forgotten. Best-effort:
# a report generation problem is worth a WARN, never worth failing an otherwise
# good QA run over.
qa_html_report="$(dirname "$0")/qa_html_report.py"
if have python3 && [ -f "$qa_html_report" ]; then
	if python3 "$qa_html_report" "$OUTDIR" -o "$OUTDIR/report.html" >"$OUTDIR/report_gen.log" 2>&1; then
		log "html report: $OUTDIR/report.html"
	else
		log "${c_yel}[WARN]${c_rst} html report generation failed - see $OUTDIR/report_gen.log"
	fi
else
	log "${c_yel}[WARN]${c_rst} html report skipped - python3 or qa_html_report.py not found"
fi

exit "$QA_EXIT"
