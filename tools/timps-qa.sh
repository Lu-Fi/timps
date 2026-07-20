#!/usr/bin/env bash
# =============================================================================
# timps-qa.sh - comprehensive automated QA / stress test for a timps camera.
#
# Exercises the whole streamer from a Linux host over the network and reports
# on SYNCHRONISATION, STABILITY, RELIABILITY and LOAD:
#
#   1. Preflight ...... ping, ports, tool detection
#   2. Discovery ...... ffprobe every stream (codec/res/fps/audio)
#   2b. Auth .......... no-auth + wrong-pass must be blocked (RTSP + HTTP surfaces)
#   2c. Backchannel ... optional ONVIF audio backchannel advertised in SDP
#   3. Integrity+Sync . record each stream, measure fps, real-time rate,
#                       A/V drift, timestamp monotonicity, decode errors
#   4. HTTP fMP4 ...... /stream.mp4 (MSE feed) plays, fps/bitrate, errors
#   5. MJPEG .......... /stream.mjpeg frame rate + integrity
#   6. Snapshot ....... /snapshot.jpg?chn=N validity + latency + success rate
#   7. Audio .......... codec/rate, silence-gap scan
#   8. /control API ... status JSON, caps, safe write+persist round-trip
#   8b. Live settings . POST every live setting, read it back, verify applied
#   9. /events ........ SSE stream emits events
#  10. ONVIF .......... both snapshot proxies + GetProfiles (resolution/codec vs
#                       real stream, fps/bitrate surfaced with template note)
#  11. Recording ..... on-demand clip via /control record.clip
#  12. Reliability ... reconnect churn (TCP+UDP), time-to-first-frame
#  13. Load .......... concurrent-client ramp, per-client fps/drops, max stable
#  14. Restart ....... optional streamer restart + recovery time
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

PROFILE="${PROFILE:-standard}"
OUTDIR="${OUTDIR:-timps-qa-$(date +%Y%m%d-%H%M%S)}"

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
		-h|--help) usage;;
		*) echo "unknown option: $1" >&2; usage;;
	esac
done
[ -n "$CAM" ] || { echo "ERROR: --cam <ip> is required"; usage; }

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
		grep -oE "\"${2##*.}\"[[:space:]]*:[[:space:]]*[^,}]+" "$1" | head -1 | sed 's/.*:[[:space:]]*//; s/"//g'
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
	local rep
	rep=$(awk -F, -v wall="$wall" '
	{
		ct=$1; p=$2+0;
		if(ct=="video"||ct=="audio"){
			n[ct]++;
			if(!(ct in first)){first[ct]=p; last[ct]=p; prev[ct]=p}
			g=p-prev[ct]; if(g>maxgap[ct])maxgap[ct]=g;
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
		# A/V skew drift
		if(("audio" in first)&&("video" in first)){
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

ping -c1 -W2 "$CAM" >/dev/null 2>&1 && ok "camera $CAM reachable (ping)" || warn "ping $CAM failed (may be firewalled)"
for p in "$RTSP_PORT" "$HTTP_PORT"; do
	if (exec 3<>"/dev/tcp/$CAM/$p") 2>/dev/null; then ok "tcp port $p open"; exec 3>&- 2>/dev/null
	else bad "tcp port $p closed/unreachable"; fi
done

have ffprobe || { bad "ffprobe missing - aborting stream tests"; }

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

fi
if want 2c backchannel bc; then
# --- 2c. ONVIF audio backchannel (SDP advertise) ----------------------------
hdr "2c. Audio backchannel (optional)"
# A DESCRIBE carrying the ONVIF Require header must yield an m=audio backchannel
# media (a=sendonly, trackID=2) IFF the feature is built+enabled AND /bin/iac is
# present. Optional feature -> a missing backchannel is info, not FAIL.
bcsdp="$OUTDIR/backchannel.sdp"
if curl -s --max-time 10 -u "$RTSP_USER:$RTSP_PASS" \
        --request DESCRIBE --header "Require: www.onvif.org/ver20/backchannel" \
        "rtsp://$CAM:$RTSP_PORT/$PATH_MAIN" > "$bcsdp" 2>/dev/null && [ -s "$bcsdp" ]; then
	if grep -q "a=sendonly" "$bcsdp" && grep -q "trackID=2" "$bcsdp"; then
		bcname=$(grep -oE 'a=rtpmap:[0-9]+ [A-Za-z0-9-]+' "$bcsdp" | tail -1 | awk '{print $2}')
		ok "backchannel advertised (${bcname:-?}, trackID=2, a=sendonly)"
	else
		info "no backchannel in SDP (feature off, not built, or /bin/iac absent) - optional"
	fi
else
	info "DESCRIBE+Require not answered by curl (RTSP auth/curl-RTSP?) - skipped"
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
		code=$(lv_post "${wo}{$body}${wc}")
		if [ "$code" != "200" ]; then bad "$label: POST(new) HTTP $code"; return; fi
		gf="$OUTDIR/lv_${label}_new.json"; lv_get "$gf"
		for ((i=0;i<n;i++)); do
			got=$(jget "$gf" "${P[i]}")
			if [ "$got" = "${N[i]}" ]; then pass=$((pass+1))
			else bad "$label: ${P[i]##*.} not applied (got '$got', want '${N[i]}')"; fi
		done
		[ "$pass" = "$n" ] && ok "$label: all $n live key(s) applied & read back (${P[*]##*.})"
		# restore originals and confirm they revert
		code=$(lv_post "${wo}{$rbody}${wc}")
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
	# HAL supports it). uchar knobs 0..255, highlight/backlight 0..10, WB gains
	# 0..65535, hflip/vflip/running_mode bool, anti_flicker 0..2, core_wb 0..1 ---
	lv_section image '{"image":' '}' image \
		"brightness int 0 255" "contrast int 0 255" "saturation int 0 255" \
		"sharpness int 0 255" "hue int 0 255" "ae_compensation int 0 255" \
		"max_again int 0 255" "max_dgain int 0 255" "sinter_strength int 0 255" \
		"temper_strength int 0 255" "dpc_strength int 0 255" "defog_strength int 0 255" \
		"drc_strength int 0 255" "highlight_depress int 0 10" \
		"backlight_compensation int 0 10" "wb_rgain int 0 65535" "wb_bgain int 0 65535" \
		"hflip bool" "vflip bool" "running_mode bool" "anti_flicker int 0 2" \
		"core_wb_mode int 0 1"

	# --- audio: only the keys the HAL applies LIVE (caps.audio). volume/gain/
	# alc_gain are raw ints; mute is the live publish gate. The restart-only
	# audio keys (codec/samplerate/agc/ns/...) are covered by the persist check ---
	lv_section audio '{"audio":' '}' audio \
		"volume int 0 100" "gain int 0 100" "alc_gain int 0 100" "mute bool"

	# --- osd stream 0 item 0: the live text-overlay leaf keys (caps.osd).
	# font_size clamps 8..256, transparency 0..255, colors are 0xAARRGGBB hex ---
	lv_section osd0.0 '{"osd0":{"0":' '}}' osd0.0 \
		"text str" "x int 0 200" "y int 0 200" "font_size int 8 256" \
		"transparency int 0 255" "outline int 0 4" "color hex" "outline_color hex"

	# --- privacy cover mask stream 0 region 0: applied live when an OSD group
	# exists (else persisted). enabled bool, geometry px, color hex ---
	lv_section privacy.0.0 '{"privacy":{"0":{"0":' '}}}' privacy.0.0 \
		"enabled bool" "x int 0 200" "y int 0 200" "w int 1 200" "h int 1 200" "color hex"

	# --- motion: all applied live (the HAL recreates the IVS grid). sensitivity
	# 0..255, monitor_stream 0..1, enabled bool. cols/rows are omitted: they are
	# clamped to the SDK cell budget, which would look like a mismatch here.
	# LV_RESTORE_SOFT: on hardware motion.sensitivity is reported from the live
	# IVS status, not g_cfg - after restoring motion to disabled the grid is torn
	# down and that read-back goes stale (config is still correctly restored), so
	# a partial restore here is expected and reported as info, not a warning ---
	LV_RESTORE_SOFT=1
	lv_section motion '{"motion":' '}' motion \
		"sensitivity int 0 255" "monitor_stream int 0 1" "enabled bool"
	LV_RESTORE_SOFT=0

	# --- daynight: the detection thread reads these live from config. Test the
	# numeric thresholds + tunables (day/night gain thresholds, gain %, delay);
	# "enabled" reflects the thread's own state (poll lag) so it is left out ---
	lv_section daynight '{"daynight":' '}' daynight \
		"total_gain_day_threshold int 100 900" \
		"total_gain_night_threshold int 2000 8000" \
		"day_gain_pct int 0 100" "baseline_delay_s int 0 60"

	# --- record: the running recorder reads these live. enabled/mode/channel are
	# left out (they would start/stop capture or depend on stream count); the
	# rolls/segment/min-free/audio/name round-trip live. seg 0..86400, pre 0..60,
	# post 0..300, min_free 0..1048576 ---
	lv_section record '{"record":' '}' record \
		"segment_s int 10 600" "pre_roll_s int 0 60" "post_roll_s int 0 300" \
		"min_free_mb int 50 2000" "audio bool" "name str"

	# --- timelapse: the running timelapse thread reads these live. interval_s
	# >=1, keep_days >=0; enabled/channel left out for the same reason as record ---
	lv_section timelapse '{"timelapse":' '}' timelapse \
		"interval_s int 1 3600" "keep_days int 0 365" "name str"

	# --- persist-only (restart-required) sanity: these must NOT be advertised as
	# live but MUST still round-trip through the config. One representative key
	# each; the daemon reports the new value (persisted) even though the running
	# pipeline is untouched until restart. ---
	pv_cur=$(jget "$LV_BASE" video.0.bitrate)
	if [ -n "$pv_cur" ]; then
		pv_new=$(flip_int 512 8000 "$pv_cur")
		code=$(lv_post "{\"video\":{\"0\":{\"bitrate\":$pv_new}}}")
		lv_get "$OUTDIR/lv_persist.json"
		got=$(jget "$OUTDIR/lv_persist.json" video.0.bitrate)
		[ "$got" = "$pv_new" ] && ok "persist-only video0.bitrate round-trips through config ($pv_new, applies on restart)" \
			|| bad "persist-only video0.bitrate did not persist (got '$got', want '$pv_new')"
		lv_post "{\"video\":{\"0\":{\"bitrate\":$pv_cur}}}" >/dev/null   # restore
	fi

	fi
fi

fi
if want 9 events; then
# --- 9. /events SSE ---------------------------------------------------------
hdr "9. /events (SSE)"
ev="$OUTDIR/events.log"
timeout 8 curl -s -N -u "$HTTP_USER:$HTTP_PASS" "$(http_base)/events" > "$ev" 2>/dev/null || true
if [ -s "$ev" ]; then ok "/events streamed $(wc -l <"$ev") line(s) in 8s"
else warn "/events produced no data in 8s (may be idle - no config changes)"; fi

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
		e=$(grep -icE 'non-monotonous|discontinuit|corrupt|error while|concealing|invalid data' "$rlog" 2>/dev/null || echo 0)
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
	# fault isn't buried under benign matches
	errs=$(sshx "logread 2>/dev/null | grep -iE 'error|fail|assert|segfault|oom|IMP_.*failed' | grep -cviE 'dropbear|telegrambot|Exited normally|before auth|[0-9]+ fails'")
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
