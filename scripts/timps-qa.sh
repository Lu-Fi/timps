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
#   8c. OSD vars_file . optional SSH: custom {placeholder} round-trip via the
#                       on-device osd.vars_file key=value mechanism
#   8d. Field drift ... diff GET /control?fields=1 (F_CTRL inventory) against
#                       8b's tested set + a documented allowlist
#   8e. Malformed body  5 negative-case POSTs against the hand-rolled JSON
#                       parser; asserts liveness, not a strict error contract
#   9. /events ........ SSE stream emits events, provoked by a live-settings POST
#  10. ONVIF .......... both snapshot proxies + GetProfiles (resolution/codec vs
#                       real stream, fps/bitrate surfaced with template note)
#  11. Recording ..... on-demand clip via /control record.clip
#  12. Reliability ... reconnect churn (TCP+UDP), time-to-first-frame
#  13. Load .......... concurrent-client ramp, per-client fps/drops, max stable
#  14. Restart ....... optional streamer restart + recovery time
#  14b. Fatal signal .. optional, DESTRUCTIVE: kill -SEGV + handler/restart verify
#  15. Soak .......... long capture with periodic health sampling
#  16. On-device ..... optional SSH: timpsd RSS/CPU, logread errors,
#                      /etc/timps.conf integrity (glued/duplicate keys)
#
# Everything is host-side (needs ffmpeg + ffprobe + curl). SSH is optional and
# only unlocks the on-device checks. All raw logs land in an output directory;
# a final table summarises PASS / WARN / FAIL and the process exit code
# reflects the worst result.
#
# Usage:   ./timps-qa.sh --cam 192.168.241.190 [--profile standard] [options]
#          CAM=192.168.241.190 ./timps-qa.sh
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

# Optional on-device access, e.g. SSH_TARGET="root@192.168.241.190"
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
SOAK_DUR="${SOAK_DUR:-0}"          # seconds of soak (0 = skip unless profile sets it)
SOAK_SAMPLE="${SOAK_SAMPLE:-60}"   # health sample interval during soak
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

usage() {
	sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'
	cat <<EOF

Options (also settable as env vars):
  --cam IP            camera address (required)
  --profile P         quick | standard | load | soak   (default: standard)
  --rtsp-user U       --rtsp-pass P    --http-user U   --http-pass P
  --expect-channels N assert audio channel count (e.g. 2 = stereo); FAIL on mismatch
  --main PATH         RTSP main path (default ch0)   --sub PATH (default ch1)
  --transport tcp|udp default tcp
  --ssh TARGET        e.g. root@IP  -> enables on-device checks
  --integ-dur S       --load-dur S  --load-clients "1 2 4 8"
  --reconnects N      --snaps N     --soak-dur S       --restart
  --only LIST         run only these sections (names or numbers, comma-sep),
                      e.g. --only onvif  or  --only 3,10 ; preflight always runs
  --out DIR           output directory
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
		--restart) DO_RESTART=1; shift;;
		--only) ONLY="$2"; shift 2;;
		--out) OUTDIR="$2"; shift 2;;
		--test-backchannel) TEST_BACKCHANNEL=1; shift;;
		--bc-test-freq) BC_TEST_FREQ="$2"; shift 2;;
		--bc-test-secs) BC_TEST_SECS="$2"; shift 2;;
		--test-rotation) TEST_ROTATION=1; shift;;
		--osd-vars-file) OSD_VARS_FILE="$2"; shift 2;;
		--test-crash) TEST_CRASH=1; shift;;
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

RU="$RTSP_USER"; RP="$RTSP_PASS"
rtsp_url() { printf 'rtsp://%s:%s@%s:%s/%s' "$RU" "$RP" "$CAM" "$RTSP_PORT" "$1"; }
http_base() { printf 'http://%s:%s' "$CAM" "$HTTP_PORT"; }
curlq() { curl -s --max-time "${1:-10}" -u "$HTTP_USER:$HTTP_PASS" "${@:2}"; }

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

# --------------------------------------------------- stream integrity + A/V sync core
# analyze_stream <url> <label> <dur> <input-opts...>
analyze_stream() {
	local url="$1" label="$2" dur="$3"; shift 3
	local inopts=("$@")
	local seg="$OUTDIR/rec_${label}.mkv" err="$OUTDIR/rec_${label}.log"
	local t0 t1 wall
	info "$label: recording ${dur}s ..."
	t0=$(date +%s.%N)
	# -nostdin + timeout -k: ffmpeg over RTSP-TCP may ignore a lone SIGTERM, so
	# force a SIGKILL if -t doesn't self-stop. No -copyts (it breaks -t and isn't
	# needed: fps/rate/gaps/monotonicity/drift are all measured from the recorded
	# packet timeline regardless of the absolute offset).
	timeout -k 5 "$((dur+6))" ffmpeg -hide_banner -nostdin -y -loglevel warning "${inopts[@]}" \
		-i "$url" -t "$dur" -c copy "$seg" </dev/null 2>"$err" || true
	t1=$(date +%s.%N)
	wall=$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.3f", b-a}')

	if [ ! -s "$seg" ]; then
		bad "$label: no data captured (see $err)"; return 1
	fi
	local ffe
	ffe=$(grep -icE 'non-monotonous|discontinuit|corrupt|error while|decode_slice|concealing|invalid data|missed' "$err" 2>/dev/null || true)

	local probe="$OUTDIR/pkts_${label}.csv"
	# NOTE: ffprobe emits csv columns in a FIXED order (codec_type,pts_time here),
	# not the -show_entries order, so keep this to exactly the two fields the awk
	# below reads as $1 (codec_type) and $2 (pts_time).
	ffprobe -v error -show_entries packet=codec_type,pts_time \
		-of csv=p=0 "$seg" 2>/dev/null > "$probe"

	# awk: per codec_type -> count, first/last pts, max gap, non-monotonic count
	#
	# WARMUP_S excludes the connection/first-keyframe startup transient from
	# maxgap and A/V skew: a lone leading video keyframe (sent immediately so
	# the player has something to decode) followed by a real gap before audio
	# and steady video both start is NORMAL, not a freeze or growing drift -
	# fMP4 anchors both tracks to a shared t=0 at that first keyframe (unlike
	# RTSP, which gives each track its own independent PTS zero), so a client
	# that attaches mid-warmup sees a one-time skew that LOCKS once real media
	# starts, not one that grows. Judge freeze/skew from steady state
	# (first packet at/after WARMUP_S) instead of the raw first packet.
	local rep
	rep=$(awk -F, -v wall="$wall" -v warmup="${QA_WARMUP_S:-2}" '
	{
		ct=$1; p=$2+0;
		if(ct=="video"||ct=="audio"){
			n[ct]++;
			if(!(ct in first)){first[ct]=p; last[ct]=p; prev[ct]=p}
			if(!(ct in first_ss) && p>=warmup){first_ss[ct]=p}
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
		# A/V skew drift, measured from steady state (post-warmup) so the
		# one-time fMP4 startup transient does not read as growing drift
		if(("audio" in first_ss)&&("video" in first_ss)){
			ss=first_ss["audio"]-first_ss["video"]; se=last["audio"]-last["video"];
			printf "SKEW %.3f %.3f %.3f\n", ss, se, se-ss;
		} else if(("audio" in first)&&("video" in first)){
			ss=first["audio"]-first["video"]; se=last["audio"]-last["video"];
			printf "SKEW %.3f %.3f %.3f\n", ss, se, se-ss;
		}
	}' "$probe")

	info "$label: wall=${wall}s ffmpeg-warnings=$ffe"
	local vrate="" v_rt ratio
	v_rt=$(awk '$1=="video"{print $5}' <<<"$rep")
	while read -r ct n span rate rt maxgap nonmono; do
		case "$ct" in
		video)
			vrate="$rate"
			info "  video: pkts=$n span=${span}s fps=${rate} rt=${rt}x maxgap=${maxgap}s nonmono=$nonmono"
			# rt = media_span / wall. rt<1 is almost always RTSP connect + keyframe
			# SETUP overhead (benign, largest on short captures); rt>1 means media
			# arrives FASTER than real time = a wrong-clock / fast-forward bug.
			if fcmp "$rt" gt 1.20; then bad "$label video real-time rate ${rt}x >1.2 (fast-forward / wrong clock)"
			elif fcmp "$rt" lt 0.50; then bad "$label video real-time rate ${rt}x <0.5 (severe stall / packet loss)"
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
			fcmp "$ad" le 0.15 && ok "$label A/V drift ${drift}s (in sync)" \
				|| { fcmp "$ad" le 0.40 && warn "$label A/V drift ${drift}s (marginal)" \
				     || bad "$label A/V drift ${drift}s (out of sync / growing)"; }
			;;
		esac
	done <<< "$rep"

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
		NOM_FPS[$lbl]="$fnum"
		ok "$lbl advertises video + $( [ -n "$(ffprobe -v error -rtsp_transport "$RTSP_TRANSPORT" -select_streams a:0 -show_entries stream=codec_name -of csv=p=0 "$url" 2>/dev/null)" ] && echo audio || echo 'NO audio') (nominal ${fnum} fps)"
	else
		bad "$lbl: ffprobe could not open $url (see probe_${lbl}.err)"
	fi
done

fi
if want 2b auth; then
# --- 2b. Auth enforcement (no/wrong credentials must be blocked) -------------
hdr "2b. Auth enforcement (unauthenticated must be blocked)"
# HTTP surfaces: a request with NO Authorization header must get 401/403.
check_noauth() { # <url> <label>
	local code
	code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 8 "$1")
	case "$code" in
		401|403) ok "$2 blocks no-auth (HTTP $code)";;
		000)     warn "$2 unreachable (HTTP 000) - cannot judge";;
		*)       bad "$2 served WITHOUT auth (HTTP $code) - NOT protected";;
	esac
}
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
	*)       bad "/control accepted WRONG password (HTTP $wcode)";;
esac

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
				bad "acoustic loopback: tone NOT detected (delta ${delta}dB < 5) - speaker silent, muted, or too quiet (env-dependent)"
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

fi
if want 4 fmp4; then
# --- 4. HTTP fMP4 -----------------------------------------------------------
hdr "4. HTTP fMP4 (/stream.mp4)"
murl="$(http_base)/stream.mp4?chn=0"
# fetch with auth into ffmpeg via -headers
AUTH_HDR="Authorization: Basic $(printf '%s:%s' "$HTTP_USER" "$HTTP_PASS" | base64)"
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
timeout -k 5 "$((INTEG_DUR+5))" ffmpeg -hide_banner -nostdin -stats -y -loglevel warning -headers "$AUTH_HDR"$'\r\n' \
	-i "$mjurl" -t "$INTEG_DUR" -f null - </dev/null 2>"$mjlog" || true
frames=$(grep -oE 'frame= *[0-9]+' "$mjlog" | tail -1 | grep -oE '[0-9]+')
if [ -n "${frames:-}" ] && [ "$frames" -gt 0 ]; then
	fps=$(awk -v f="$frames" -v d="$INTEG_DUR" 'BEGIN{printf "%.1f", f/d}')
	ok "MJPEG delivered $frames frames (~${fps} fps)"
else bad "MJPEG produced no frames (see $mjlog)"; fi

fi
if want 6 snapshot; then
# --- 6. Snapshot ------------------------------------------------------------
hdr "6. Snapshot (/snapshot.jpg) x$SNAP_COUNT"
for chn in 0 1; do
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
		else badc=$((badc+1)); fi
	done
	avg=$(awk -v s="$tsum" -v n="$SNAP_COUNT" 'BEGIN{printf "%.3f",s/n}')
	if [ "$badc" -eq 0 ]; then ok "chn$chn snapshots ${okc}/${SNAP_COUNT} valid JPEG, avg ${avg}s, min ${minb}B"
	elif [ "$okc" -gt 0 ]; then warn "chn$chn snapshots ${okc} ok / ${badc} bad (avg ${avg}s)"
	else bad "chn$chn snapshots all $SNAP_COUNT failed"; fi
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
	# safe write round-trip: read image.brightness, write same value back, re-read
	bri=$(grep -oE '"brightness"[^,}]*' "$cj" | head -1 | grep -oE '[0-9]+' | head -1)
	if [ -n "${bri:-}" ]; then
		code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 8 -u "$HTTP_USER:$HTTP_PASS" \
			-X POST "$(http_base)/control" -d "{\"image\":{\"brightness\":$bri}}")
		[ "$code" = "200" ] && ok "/control write round-trip (brightness=$bri) accepted" \
			|| warn "/control write returned HTTP $code"
	else info "  brightness not found; skipped write round-trip"; fi
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

	# POST a JSON body, echo the HTTP status
	lv_post() { curl -s -o /dev/null -w '%{http_code}' --max-time 12 \
		-u "$HTTP_USER:$HTTP_PASS" -X POST "$(http_base)/control" -d "$1"; }
	lv_get()  { curlq 12 "$(http_base)/control" -o "$1"; }
	# Interruption safety: /control POSTs PERSIST to the camera's flash config.
	# A run killed between POST(new) and POST(restore) used to strand the test
	# values on a live camera (seen 2026-08-02: cam-wyze left with manual WB
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

	# lv_section <label> <wrap-open> <wrap-close> <read-prefix> <spec...>
	#   spec = "key type [lo hi]"   type: int|bool|hex|str
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
		for ((i=0;i<n;i++)); do
			got=$(jget "$gf" "${P[i]}")
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
	# rgain/bgain=32767 that hit cam-wyze on 2026-08-02. ---
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
	# are 0..100, aec is a bool; all round-trip through config. ---
	case "$(jget "$cj" caps.audio)" in
		*spk_volume*)
			lv_section audio_spk '{"audio":' '}' audio \
				"spk_volume int 0 100" "spk_gain int 0 100" "aec bool"
			;;
		*)
			info "audio spk_*/aec: caps.audio has no spk_volume (no AO pipeline in this build) - skipping"
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
	# numeric thresholds + tunables (day/night gain thresholds, gain %, delay);
	# "enabled" reflects the thread's own state (poll lag) so it is left out ---
	lv_section daynight '{"daynight":' '}' daynight \
		"total_gain_day_threshold int 100 900" \
		"total_gain_night_threshold int 2000 8000" \
		"day_gain_pct int 0 100" "baseline_delay_s int 0 60" \
		"boot_settle_s int 0 60" "boot_settle_max_s int 10 300" \
		"boot_stable_pct int 0 100" "night_reconfirm_s int 0 7200" \
		"probe_max_skip_s int 3600 604800" \
		"sun_sunrise_offset_min int -1440 1440" \
		"sun_sunset_offset_min int -1440 1440"

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
		[ "$(jget "$gf" daynight.dn_mode)" = "sun" ]              && dnp=$((dnp+1)) || bad "daynight.mode not applied (dn_mode='$(jget "$gf" daynight.dn_mode)', want 'sun')"
		[ "$(jget "$gf" daynight.time_night_start)" = "19:30" ]  && dnp=$((dnp+1)) || bad "daynight.time_night_start not applied (got '$(jget "$gf" daynight.time_night_start)')"
		[ "$(jget "$gf" daynight.time_day_start)" = "06:30" ]    && dnp=$((dnp+1)) || bad "daynight.time_day_start not applied (got '$(jget "$gf" daynight.time_day_start)')"
		[ "$(jget "$gf" daynight.sun_latitude)" = "52.5" ]       && dnp=$((dnp+1)) || bad "daynight.sun_latitude not applied (got '$(jget "$gf" daynight.sun_latitude)')"
		[ "$(jget "$gf" daynight.sun_longitude)" = "13.5" ]      && dnp=$((dnp+1)) || bad "daynight.sun_longitude not applied (got '$(jget "$gf" daynight.sun_longitude)')"
		[ "$dnp" = 5 ] && ok "daynight TIME/SUN: mode+time_night_start+time_day_start+sun_latitude+sun_longitude applied & read back (HTTP $code)"
		lv_post "$dn_restore" >/dev/null; LV_PENDING=""
		rf="$OUTDIR/lv_daynight_timesun_restore.json"; lv_get "$rf"
		[ "$(jget "$rf" daynight.dn_mode)" = "$dn_mode_cur" ] \
			&& info "  daynight TIME/SUN: restored mode to $dn_mode_cur" \
			|| warn "daynight TIME/SUN: mode did not restore to $dn_mode_cur"
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
	ov_clamp_test daynight   '{"daynight":' '}' daynight   day_gain_pct     -50      0
	ov_clamp_test daynight   '{"daynight":' '}' daynight   probe_max_skip_s 1        3600
	ov_clamp_test daynight   '{"daynight":' '}' daynight   probe_max_skip_s 99999999 604800

	# --- persist-only (restart-required) sanity: these must NOT be advertised as
	# live but MUST still round-trip through the config. One representative key
	# each; the daemon reports the new value (persisted) even though the running
	# pipeline is untouched until restart. ---
	pv_cur=$(jget "$LV_BASE" video.0.bitrate)
	if [ -n "$pv_cur" ]; then
		pv_new=$(flip_int 512 8000 "$pv_cur")
		LV_PENDING="{\"video\":{\"0\":{\"bitrate\":$pv_cur}}}"   # armed until restore
		code=$(lv_post "{\"video\":{\"0\":{\"bitrate\":$pv_new}}}")
		lv_get "$OUTDIR/lv_persist.json"
		got=$(jget "$OUTDIR/lv_persist.json" video.0.bitrate)
		[ "$got" = "$pv_new" ] && ok "persist-only video0.bitrate round-trips through config ($pv_new, applies on restart)" \
			|| bad "persist-only video0.bitrate did not persist (got '$got', want '$pv_new')"
		lv_post "{\"video\":{\"0\":{\"bitrate\":$pv_cur}}}" >/dev/null   # restore
		LV_PENDING=""
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
		lv_post "{\"audio\":{\"codec\":\"$ac_cur\"}}" >/dev/null   # restore
		lv_get "$OUTDIR/lv_opus_restore.json"
		[ "$(jget "$OUTDIR/lv_opus_restore.json" audio.codec)" = "$ac_cur" ] \
			&& info "  audio.codec: restored to $ac_cur" \
			|| warn "audio.codec: did not restore to original ($ac_cur)"
		LV_PENDING=""
	fi

	# --- rotation (opt-in: --test-rotation) ---------------------------------
	# video0.rotation is persist-only like bitrate above, AND SoC-gated:
	# caps.rotation is only present when this build has USE_ROTATE compiled
	# in, and only lists the degree values this SoC's rotation path actually
	# supports (0 always; 90/270 need a dim-swapping apply path - T31/T40/T41
	# hardware or T23 USE_SW_ROTATE; 180 an ISP flip). A build without
	# rotation support simply omits the key - that is not a failure, so this
	# skips cleanly rather than reporting FAIL on unsupported hardware.
	if [ "$TEST_ROTATION" = "1" ]; then
		rot_caps=$(jget "$cj" caps.rotation)
		if [ -z "${rot_caps:-}" ]; then
			info "rotation: caps.rotation absent - not compiled into this build, skipping"
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
					sshx "grep -q '^$1' /etc/timps.conf 2>/dev/null && sed -i 's|^$1.*|$1 = $2|' /etc/timps.conf || echo '$1 = $2' >> /etc/timps.conf"
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
	# TESTED_<section>: every field name 8b's code above actually POSTs +
	# verifies (live or persist-only) for that section.
	TESTED_image="brightness contrast saturation sharpness hue vflip hflip running_mode anti_flicker ae_compensation max_again max_dgain sinter_strength temper_strength dpc_strength defog_strength drc_strength highlight_depress backlight_compensation core_wb_mode wb_rgain wb_bgain"
	TESTED_audio="volume gain alc_gain mute spk_volume spk_gain aec codec enabled samplerate channels bitrate high_pass agc ns agc_target_dbfs agc_compression_db force_stereo spk_enabled backchannel backchannel_codec backchannel_rate"
	TESTED_sensor=""
	TESTED_osd="monitor_stream font_path vars_file enabled supersample hinting"
	TESTED_osd_item="text x y font_size color transparency outline outline_color"
	TESTED_motion="sensitivity monitor_stream enabled hold_ms skip_frames"
	TESTED_record="segment_s pre_roll_s post_roll_s min_free_mb audio name dir"
	TESTED_timelapse="interval_s keep_days name dir"
	TESTED_daynight="total_gain_day_threshold total_gain_night_threshold day_gain_pct baseline_delay_s boot_settle_s boot_settle_max_s boot_stable_pct night_reconfirm_s probe_max_skip_s sun_sunrise_offset_min sun_sunset_offset_min time_night_start time_day_start sun_latitude sun_longitude"
	TESTED_video="bitrate rotation"
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
	ALLOW_daynight="enabled threshold_low threshold_high hysteresis interval_ms transition_s"   # enabled reflects the detection thread's own state (poll lag), not a value to force; the rest is the legacy brightness-ONLY fallback path (only exercised when gain telemetry is unavailable) - out of scope for this gain-based test bench
	ALLOW_video="enabled codec width height fps rc_mode gop max_gop profile qp min_qp max_qp buffers rtsp_path"   # geometry/codec/identity/routing changes carry restart-crash or active-session-disruption risk beyond a plain config round-trip; rotation (the highest-risk one) already gets the deep --test-rotation real-restart treatment and bitrate gets a representative persist round-trip above - duplicating that pattern across every remaining encoder-tuning knob isn't what this drift check is for
	ALLOW_privacy=""

	contains_word() { local n="$1"; shift; local w; for w in "$@"; do [ "$w" = "$n" ] && return 0; done; return 1; }

	drift=0; total=0
	for sec in image audio sensor osd osd_item motion record timelapse daynight video privacy; do
		fields=$(jarr "$fj" "$sec")
		[ -n "$fields" ] || continue
		tvar="TESTED_$sec"; avar="ALLOW_$sec"
		tested="${!tvar}"; allow="${!avar}"
		for f in $fields; do
			total=$((total+1))
			if contains_word "$f" $tested; then :
			elif contains_word "$f" $allow; then :
			else
				warn "field-inventory drift: $sec.$f is F_CTRL (POST-able) but is in neither 8b's tested set nor the documented allowlist above - add live/persist coverage or an explicit exclusion"
				drift=$((drift+1))
			fi
		done
	done
	[ "$drift" -eq 0 ] && ok "field-inventory: all $total F_CTRL fields across 11 sections are either tested in 8b or explicitly allowlisted (no drift)"
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
#    that overflows long/int well past any field's [lo,hi].
mb_check "overflow-prone number" '{"image":{"brightness":99999999999999999999999999}}'

# 4. completely unknown top-level section name: none of control_apply_json's
#    find_obj(s,e,"image"/"audio"/.../ &oend) calls match it - must be a
#    silent no-op, not a crash on an unrecognized key.
mb_check "unknown top-level section" '{"totally_bogus_section_xyz":{"foo":1,"bar":"baz"}}'

# 5. wrong JSON type where an object is expected: find_obj() requires the
#    value to start with '{' (`if (!p || p>=e || *p!='{') return NULL;`) -
#    handing it a JSON array for a section name must be rejected cleanly,
#    not misparsed as if it were an object.
mb_check "array instead of object" '{"image":[1,2,3]}'

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
# lv_restore_pending, added after the 2026-08-02 cam-wyze incident: a run
# killed between POST(new) and POST(restore) stranded manual WB rgain/bgain
# at 32767, a full-magenta image surviving reboots). This test's own poke is
# exactly that same shape - an extreme, visibly-wrong image.brightness value
# on a REAL live camera - so it gets the same protection: track the pending
# restore body and flush it from EXIT/INT/TERM, not just from the normal
# fall-through path below. Confirmed missing here 2026-08: a real run against
# Garage (192.168.241.190) got interrupted between the poke and the restore
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
		# fps/bitrate: expected to be the daemon template defaults, not the real rate
		if grep -qE '(^| )30( |$)' <<<" $o_fps " || grep -qE '(^| )5000( |$)' <<<" $o_br "; then
			warn "ONVIF FrameRateLimit/BitrateLimit are the onvif_simple_server template defaults (30/5000), NOT the real encoder rate - needs a daemon-side patch (ffprobe shows the true fps)"
		else
			info "ONVIF fps/bitrate: [${o_fps}] / [${o_br}] (daemon now surfaces real values)"
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
	else ok "record.clip accepted (HTTP 200); enable --ssh to verify the file on device"; fi
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
if want 13 load; then
# --- 13. Load: concurrent-client ramp --------------------------------------
hdr "13. Load - concurrent client ramp [$LOAD_CLIENTS] x ${LOAD_DUR}s each"
max_stable=0
for n in $LOAD_CLIENTS; do
	pids=""; ldir="$OUTDIR/load_${n}"; mkdir -p "$ldir"
	[ -n "$SSH_TARGET" ] && rss0=$(sshx "cat /proc/\$(pidof timpsd)/status 2>/dev/null | awk '/VmRSS/{print \$2}'")
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
		rss1=$(sshx "cat /proc/\$(pidof timpsd)/status 2>/dev/null | awk '/VmRSS/{print \$2}'")
		la=$(sshx "cut -d' ' -f1 /proc/loadavg 2>/dev/null")
		extra=" | timpsd RSS ${rss0:-?}->${rss1:-?}kB load ${la:-?}"
	fi
	nf="${NOM_FPS[main]:-0}"; lo=$(awk -v x="$nf" 'BEGIN{printf "%.1f",x*0.9}')
	if [ "$failcli" -eq 0 ] && { fcmp "$nf" le 0 || fcmp "$minfps" ge "$lo"; }; then
		ok "load ${n} clients: all ok, min ${minfps} fps, aggregate ${agg} fps/s${extra}"
		max_stable="$n"
	elif [ "$okcli" -gt 0 ]; then
		warn "load ${n} clients: ${okcli} ok / ${failcli} failed, min fps ${minfps} (degrading)${extra}"
	else
		bad "load ${n} clients: all failed${extra}"; break
	fi
done
info "max stable concurrent clients (full fps, no failures): $max_stable"

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

# --- 15. Soak ---------------------------------------------------------------
if [ "${SOAK_DUR:-0}" -gt 0 ] && want 15 soak; then
	hdr "15. Soak (${SOAK_DUR}s continuous, ${SOAK_SAMPLE}s slices)"
	soaklog="$OUTDIR/soak.log"; : > "$soaklog"
	slice="$SOAK_SAMPLE"; n_slices=$(( SOAK_DUR / slice )); [ "$n_slices" -lt 1 ] && n_slices=1
	err_total=0; bad_slices=0; rss_first=""; rss_last=""
	rec="$OUTDIR/rec_soak.mkv"; rlog="$OUTDIR/rec_soak.log"
	for s in $(seq 1 "$n_slices"); do
		timeout -k 5 "$((slice+6))" ffmpeg -hide_banner -nostdin -y -loglevel warning -rtsp_transport "$RTSP_TRANSPORT" \
			-i "$(rtsp_url "$PATH_MAIN")" -t "$slice" -c copy "$rec" </dev/null 2>"$rlog" || true
		[ -s "$rec" ] || bad_slices=$((bad_slices+1))
		# grep -c already prints "0" on no match (and exits 1); do NOT chain
		# "|| echo 0" here - that fires on the exit-1 and yields a two-line "0\n0",
		# which then makes err_total=$((err_total+e)) a fatal arithmetic SYNTAX
		# error that tears out of the whole soak loop (zero slices logged).
		e=$(grep -icE 'non-monotonous|discontinuit|corrupt|error while|concealing|invalid data' "$rlog" 2>/dev/null); e=${e:-0}
		err_total=$((err_total+e))
		rss=""
		if [ -n "$SSH_TARGET" ]; then
			rss=$(sshx "awk '/VmRSS/{print \$2}' /proc/\$(pidof timpsd)/status 2>/dev/null")
			[ -z "$rss_first" ] && rss_first="$rss"; rss_last="$rss"
		fi
		echo "$(date +%H:%M:%S) slice $s/$n_slices err=$e rss=${rss:-?}kB empty=$([ -s "$rec" ] && echo 0 || echo 1)" >> "$soaklog"
		rm -f "$rec"
		printf '\r  soak %d/%d  errors=%d  bad_slices=%d  rss=%skB     ' "$s" "$n_slices" "$err_total" "$bad_slices" "${rss:-?}"
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
	# config integrity: glued lines (two '=') or duplicate keys
	glued=$(sshx "sed 's/#.*//' /etc/timps.conf 2>/dev/null | grep -cE '=[^=]*='")
	dup=$(sshx "grep -vE '^[[:space:]]*#' /etc/timps.conf 2>/dev/null | sed 's/=.*//; s/[[:space:]]//g' | sort | uniq -d | grep -c .")
	[ "${glued:-0}" -eq 0 ] && ok "/etc/timps.conf: no glued 'a=b c=d' lines" || bad "/etc/timps.conf has ${glued} glued line(s) - config-write bug"
	[ "${dup:-0}" -eq 0 ] && ok "/etc/timps.conf: no duplicate keys" || warn "/etc/timps.conf has ${dup} duplicate key(s)"
	# rapid-write stress then re-check integrity
	info "  config-write stress: 20 rapid /control writes..."
	for i in $(seq 1 20); do
		curl -s -o /dev/null --max-time 5 -u "$HTTP_USER:$HTTP_PASS" -X POST "$(http_base)/control" \
			-d "{\"audio\":{\"agc\":$((i%2))}}" &
	done; wait 2>/dev/null
	glued2=$(sshx "sed 's/#.*//' /etc/timps.conf 2>/dev/null | grep -cE '=[^=]*='")
	[ "${glued2:-0}" -eq 0 ] && ok "after 20 rapid writes: config still clean (no glued lines)" || bad "rapid writes corrupted /etc/timps.conf (${glued2} glued) - config race not fixed"
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
if [ "$FAIL" -gt 0 ]; then log "${c_red}RESULT: FAIL${c_rst}"; exit 2
elif [ "$WARN" -gt 0 ]; then log "${c_yel}RESULT: PASS with warnings${c_rst}"; exit 1
else log "${c_grn}RESULT: PASS${c_rst}"; exit 0; fi
