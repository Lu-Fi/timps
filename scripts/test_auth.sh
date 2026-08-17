#!/usr/bin/env bash
# =============================================================================
# test_auth.sh - authentication NEGATIVE tests for timps ("fail closed").
#
# Goal: prove that WITHOUT valid credentials NONE of the network surfaces hand
# out data, and (as a sharpness counter-check) that WITH valid credentials they
# do let you in. A single leaked unauthenticated access makes the whole run
# fail (exit != 0).
#
# Surfaces covered (see src/rtsp/rtsp.c, src/mp4/httpd.c, src/srt.c):
#   RTSP  DESCRIBE + SETUP        - digest/basic, 401 without/with wrong pass
#   HTTP  /snapshot.jpg           - Basic (or ?token=)
#   HTTP  /stream.mp4  (MSE feed) - Basic (or ?token=)
#   HTTP  /stream.mjpeg           - Basic (or ?token=)
#   HTTP  /control (GET and POST) - Basic / token
#   HTTP  /events  (SSE)          - Basic / token
#   SRT   (optional)              - passphrase/streamid, skipped if no tooling
#
# Each check prints a PASS / FAIL / SKIP line. Exit code:
#   0  every surface failed closed (and correct creds still worked)
#   1  at least one surface leaked unauthenticated access, OR rejected the
#      correct credentials (test not sharp / broken auth)
#   2  target completely unreachable (nothing could be judged)
#
# IMPORTANT - localhost trust:
#   httpd.c treats every client from 127.0.0.0/8 as "local" and SKIPS HTTP auth
#   on purpose (the on-device WebUI must always reach it). So HTTP negative
#   tests are meaningless over loopback and are SKIPPED there automatically -
#   run this against a NON-loopback address (a real camera, or the sim reached
#   via its LAN IP) to exercise them. RTSP has no such bypass and is always
#   tested.
#
# Usage:
#   ./scripts/test_auth.sh --host 192.168.1.100
#   HOST=192.168.1.100 RTSP_USER=thingino RTSP_PASS=thingino ./scripts/test_auth.sh
#
# Options (all also settable as env vars):
#   --host H            target host/IP            (default 127.0.0.1)
#   --rtsp-port N       RTSP port                 (default 554)
#   --http-port N       HTTP port                 (default 8880)
#   --rtsp-user U / --rtsp-pass P                 (default thingino/thingino)
#   --http-user U / --http-pass P                 (default = rtsp creds)
#   --main PATH         RTSP main path            (default ch0)
#   --srt-port N        SRT port (enables SRT test if tooling present)
#   --timeout S         per-request timeout       (default 6)
#   -h | --help
# =============================================================================
set -u

# ----------------------------------------------------------------- config
HOST="${HOST:-127.0.0.1}"
RTSP_PORT="${RTSP_PORT:-554}"
HTTP_PORT="${HTTP_PORT:-8880}"
RTSP_USER="${RTSP_USER:-thingino}"
RTSP_PASS="${RTSP_PASS:-thingino}"
HTTP_USER="${HTTP_USER:-${RTSP_USER}}"
HTTP_PASS="${HTTP_PASS:-${RTSP_PASS}}"
PATH_MAIN="${PATH_MAIN:-ch0}"
SRT_PORT="${SRT_PORT:-}"
TIMEOUT="${TIMEOUT:-6}"

while [ $# -gt 0 ]; do
	case "$1" in
		--host)      HOST="$2"; shift 2;;
		--rtsp-port) RTSP_PORT="$2"; shift 2;;
		--http-port) HTTP_PORT="$2"; shift 2;;
		--rtsp-user) RTSP_USER="$2"; shift 2;;
		--rtsp-pass) RTSP_PASS="$2"; shift 2;;
		--http-user) HTTP_USER="$2"; shift 2;;
		--http-pass) HTTP_PASS="$2"; shift 2;;
		--main)      PATH_MAIN="$2"; shift 2;;
		--srt-port)  SRT_PORT="$2"; shift 2;;
		--timeout)   TIMEOUT="$2"; shift 2;;
		-h|--help)   sed -n '2,60p' "$0" | sed 's/^# \{0,1\}//'; exit 0;;
		*) echo "unknown option: $1" >&2; exit 2;;
	esac
done

# normalise leading slash on the RTSP path
case "$PATH_MAIN" in /*) : ;; *) PATH_MAIN="/$PATH_MAIN";; esac

# ----------------------------------------------------------------- output
if [ -t 1 ]; then C_G=$'\e[32m'; C_R=$'\e[31m'; C_Y=$'\e[33m'; C_B=$'\e[1m'; C_0=$'\e[0m'
else C_G=; C_R=; C_Y=; C_B=; C_0=; fi
N_PASS=0; N_FAIL=0; N_SKIP=0
pass() { N_PASS=$((N_PASS+1)); echo "${C_G}PASS${C_0} $*"; }
fail() { N_FAIL=$((N_FAIL+1)); echo "${C_R}FAIL${C_0} $*"; }
skip() { N_SKIP=$((N_SKIP+1)); echo "${C_Y}SKIP${C_0} $*"; }
hdr()  { echo; echo "${C_B}== $* ==${C_0}"; }

b64() { printf '%s' "$1" | base64 | tr -d '\n'; }

# ----------------------------------------------------------------- loopback?
# httpd.c: (ntohl(peer) & 0xFF000000)==0x7F000000  -> whole 127.0.0.0/8, plus
# ::1 / localhost resolve there. In that case HTTP auth is intentionally off.
is_loopback() {
	case "$1" in
		127.*|::1|localhost|localhost.localdomain) return 0;;
	esac
	# resolve a hostname if getent is available
	if command -v getent >/dev/null 2>&1; then
		local ip; ip=$(getent hosts "$1" 2>/dev/null | awk '{print $1; exit}')
		case "$ip" in 127.*|::1) return 0;; esac
	fi
	return 1
}
LOOPBACK=0; is_loopback "$HOST" && LOOPBACK=1
# FORCE_HTTP=1 runs the HTTP negative tests even against a loopback target.
# Only meaningful when the server enforces auth for local clients too (a mock,
# a CI proxy, or a build with the 127.0.0.0/8 bypass disabled) - against the
# stock daemon on loopback it will (correctly) report the intended bypass as a
# leak. Off by default.
[ "${FORCE_HTTP:-0}" = "1" ] && LOOPBACK=0

# =============================================================================
# RTSP  (raw request over bash /dev/tcp; no ffmpeg needed)
# returns the numeric status of the first response line, or "000" on connect/
# read failure.
# =============================================================================
rtsp_status() { # <method> <extra-header-or-empty>
	# run the whole exchange in a subshell so fd 3 stays local and the shell's
	# own "connect: Connection refused" redirection error is swallowed too.
	local method="$1" auth="$2" out
	out=$(
		exec 2>/dev/null
		exec 3<>"/dev/tcp/${HOST}/${RTSP_PORT}" || { echo "000"; exit; }
		{
			printf '%s rtsp://%s:%s%s RTSP/1.0\r\n' "$method" "$HOST" "$RTSP_PORT" "$PATH_MAIN"
			printf 'CSeq: 1\r\n'
			[ "$method" = SETUP ] && printf 'Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n'
			[ -n "$auth" ] && printf '%s\r\n' "$auth"
			printf '\r\n'
		} >&3 || { echo "000"; exit; }
		local line=""
		IFS= read -r -t "$TIMEOUT" line <&3
		# first line looks like: RTSP/1.0 401 Unauthorized
		printf '%s' "$line" | awk '{print $2}'
	)
	case "$out" in ''|*[!0-9]*) echo "000";; *) echo "$out";; esac
}

rtsp_reachable() {
	( exec 2>/dev/null; exec 3<>"/dev/tcp/${HOST}/${RTSP_PORT}" ) 2>/dev/null
}

test_rtsp() {
	hdr "RTSP  (rtsp://${HOST}:${RTSP_PORT}${PATH_MAIN})"
	if ! rtsp_reachable; then
		skip "RTSP port ${RTSP_PORT} not reachable - RTSP checks skipped"
		return
	fi
	local good bad
	good="Authorization: Basic $(b64 "${RTSP_USER}:${RTSP_PASS}")"
	bad="Authorization: Basic $(b64 "${RTSP_USER}:wrong_$$")"

	# --- negatives: must NOT be 200 -------------------------------------
	local c
	c=$(rtsp_status DESCRIBE "")
	case "$c" in
		401) pass "RTSP DESCRIBE without credentials -> 401";;
		000) skip "RTSP DESCRIBE no-cred: no response (RTSP $c)";;
		200) fail "RTSP DESCRIBE served WITHOUT credentials (RTSP 200) - NOT protected";;
		*)   pass "RTSP DESCRIBE without credentials rejected (RTSP $c)";;
	esac

	c=$(rtsp_status DESCRIBE "$bad")
	case "$c" in
		401) pass "RTSP DESCRIBE with WRONG password -> 401";;
		000) skip "RTSP DESCRIBE wrong-pass: no response (RTSP $c)";;
		200) fail "RTSP DESCRIBE accepted WRONG password (RTSP 200)";;
		*)   pass "RTSP DESCRIBE with WRONG password rejected (RTSP $c)";;
	esac

	c=$(rtsp_status SETUP "")
	case "$c" in
		401) pass "RTSP SETUP without credentials -> 401";;
		000) skip "RTSP SETUP no-cred: no response (RTSP $c)";;
		200) fail "RTSP SETUP served WITHOUT credentials (RTSP 200) - NOT protected";;
		*)   pass "RTSP SETUP without credentials rejected (RTSP $c)";;
	esac

	# --- positive counter-test: correct creds must be accepted ----------
	c=$(rtsp_status DESCRIBE "$good")
	case "$c" in
		200)     pass "RTSP DESCRIBE with correct credentials -> 200 (test is sharp)";;
		401|403) fail "RTSP DESCRIBE REJECTED correct credentials (RTSP $c) - auth broken/misconfigured";;
		000)     skip "RTSP DESCRIBE correct-cred: no response (RTSP $c)";;
		404)     skip "RTSP DESCRIBE correct-cred -> 404 (path ${PATH_MAIN} absent?) - auth passed";;
		*)       skip "RTSP DESCRIBE correct-cred -> RTSP $c (auth passed, non-200 backend state)";;
	esac
}

# =============================================================================
# HTTP  (curl)
# =============================================================================
http_url() { echo "http://${HOST}:${HTTP_PORT}$1"; }

http_code() { # <curl-args...>  -> numeric HTTP status (000 on failure)
	curl -s -o /dev/null -w '%{http_code}' --max-time "$TIMEOUT" "$@"
}

# negative: expect a rejection (401/403); a 2xx = leaked, everything else is
# reported as skip/info (unreachable, or backend-level 5xx that still proves
# auth ran before the body).
http_negative() { # <path> <label> [extra curl args...]
	local path="$1" label="$2"; shift 2
	local code; code=$(http_code "$@" "$(http_url "$path")")
	case "$code" in
		401|403) pass "$label blocks unauthenticated request (HTTP $code)";;
		000)     skip "$label unreachable (HTTP 000)";;
		2*)      fail "$label served WITHOUT auth (HTTP $code) - NOT protected";;
		*)       skip "$label -> HTTP $code (not a 2xx leak, but no explicit 401/403)";;
	esac
}

# positive counter-test: correct creds must NOT be rejected. A non-401/403
# (including 5xx from a sim without a real media source) proves the credentials
# were accepted, which is all this check asserts.
http_positive() { # <path> <label> [extra curl args...]
	local path="$1" label="$2"; shift 2
	local code; code=$(http_code -u "${HTTP_USER}:${HTTP_PASS}" "$@" "$(http_url "$path")")
	case "$code" in
		401|403) fail "$label REJECTED correct credentials (HTTP $code) - auth broken/misconfigured";;
		000)     skip "$label unreachable for positive test (HTTP 000)";;
		*)        pass "$label accepts correct credentials (HTTP $code)";;
	esac
}

http_reachable() {
	local code; code=$(http_code "$(http_url "/")")
	[ "$code" != "000" ]
}

test_http() {
	hdr "HTTP  (http://${HOST}:${HTTP_PORT})"
	if ! http_reachable; then
		skip "HTTP port ${HTTP_PORT} not reachable - HTTP checks skipped"
		return
	fi
	if [ "$LOOPBACK" = "1" ]; then
		skip "HTTP negative tests skipped: ${HOST} is loopback, httpd trusts 127.0.0.0/8 (see header). Run against a non-loopback address to exercise HTTP auth."
		# still show that the surface is up + serves with creds
		http_positive "/control" "/control (reachability)"
		return
	fi

	# --- negatives (no Authorization header at all) ---------------------
	http_negative "/snapshot.jpg?chn=0" "/snapshot.jpg"
	http_negative "/stream.mp4?chn=0"   "/stream.mp4"
	http_negative "/stream.mjpeg?chn=0" "/stream.mjpeg"
	http_negative "/control"            "/control (GET)"
	http_negative "/control"            "/control (POST)" -X POST --data '{"image":{"contrast":50}}'
	http_negative "/events?stream=motion" "/events"

	# --- negatives with a WRONG password (proves 'any credential' is not accepted)
	local wcode
	wcode=$(http_code -u "${HTTP_USER}:wrong_$$" "$(http_url "/control")")
	case "$wcode" in
		401|403) pass "/control rejects WRONG password (HTTP $wcode)";;
		000)     skip "/control unreachable for wrong-pass test (HTTP 000)";;
		2*)      fail "/control accepted WRONG password (HTTP $wcode)";;
		*)       skip "/control wrong-pass -> HTTP $wcode";;
	esac

	# --- positive counter-tests (correct creds must be accepted) --------
	http_positive "/snapshot.jpg?chn=0" "/snapshot.jpg"
	http_positive "/control"            "/control"
}

# =============================================================================
# SRT  (optional - needs srt-live-transmit or ffmpeg with libsrt; the sim has
# SRT disabled, so this normally just skips)
# =============================================================================
test_srt() {
	[ -n "$SRT_PORT" ] || return
	hdr "SRT  (srt://${HOST}:${SRT_PORT})"
	local tool=""
	command -v srt-live-transmit >/dev/null 2>&1 && tool="srt-live-transmit"
	if [ -z "$tool" ]; then
		skip "SRT test needs srt-live-transmit (or ffmpeg+libsrt) - not installed"
		return
	fi
	# A caller with neither passphrase nor the configured streamid must be
	# rejected during the handshake. Success (rc 0 = data flowing) = leak.
	if timeout "$TIMEOUT" "$tool" -q "srt://${HOST}:${SRT_PORT}?mode=caller" /dev/null >/dev/null 2>&1; then
		fail "SRT accepted a caller WITHOUT passphrase/streamid - NOT protected"
	else
		pass "SRT rejects caller without passphrase/streamid"
	fi
}

# ----------------------------------------------------------------- run
echo "${C_B}timps auth (fail-closed) test${C_0}"
echo "target: ${HOST}  rtsp:${RTSP_PORT}  http:${HTTP_PORT}  loopback:${LOOPBACK}"

test_rtsp
test_http
test_srt

hdr "Summary"
echo "PASS=${N_PASS}  FAIL=${N_FAIL}  SKIP=${N_SKIP}"

if [ "$N_FAIL" -gt 0 ]; then
	echo "${C_R}RESULT: FAIL - unauthenticated access leaked or correct credentials rejected${C_0}"
	exit 1
fi
if [ "$N_PASS" -eq 0 ]; then
	echo "${C_Y}RESULT: INCONCLUSIVE - nothing could be tested (target unreachable?)${C_0}"
	exit 2
fi
echo "${C_G}RESULT: OK - all tested surfaces fail closed${C_0}"
exit 0
