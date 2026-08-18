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
#
# Extra env (no flags):
#   HTTP_TOKEN          a valid /control token -> also tests token ACCEPTANCE
#                       (rejection of a wrong token is always tested)
#   SRT_PASSPHRASE / SRT_STREAMID
#                       the real SRT secrets -> adds the SRT positive test
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

	# --- Digest (httpd.c's 401 offers "Digest ... qop=auth" FIRST; auth.c
	# implements it with nonce tracking + realm binding). curl -u sends
	# Basic, so until 2026-08-18 nothing here had ever exercised the digest
	# verifier - a regression in it would only have surfaced on a real NVR.
	local dcode
	dcode=$(http_code --digest -u "${HTTP_USER}:wrong_$$" "$(http_url "/control")")
	case "$dcode" in
		401|403) pass "/control rejects WRONG password via Digest (HTTP $dcode)";;
		000)     skip "/control unreachable for digest wrong-pass test";;
		2*)      fail "/control accepted WRONG password via Digest (HTTP $dcode)";;
		*)       skip "/control digest wrong-pass -> HTTP $dcode";;
	esac
	dcode=$(http_code --digest -u "${HTTP_USER}:${HTTP_PASS}" "$(http_url "/control")")
	case "$dcode" in
		2*)      pass "/control accepts correct credentials via Digest (HTTP $dcode)";;
		401|403) fail "/control REJECTS correct credentials via Digest (HTTP $dcode) - the advertised digest scheme doesn't verify";;
		000)     skip "/control unreachable for digest positive test";;
		*)       skip "/control digest positive -> HTTP $dcode";;
	esac

	# --- Digest realm binding (auth.c: a realm the server never issued must
	# be rejected BEFORE hashing - the realm is an HA1 input, so accepting a
	# forged one enables cross-service digest replay). curl always echoes
	# the server's realm; forging one needs a hand-rolled header. The
	# correct-realm twin proves the hand-rolled arithmetic itself, so the
	# wrong-realm 401 is attributable to the binding. Fresh nonce for each
	# attempt - the server tracks nonces, and reuse would test nonce state
	# instead of the realm.
	if command -v md5sum >/dev/null 2>&1; then
		dg_md5() { printf '%s' "$1" | md5sum | cut -d' ' -f1; }
		dg_try() {  # <realm> -> http code of a self-computed RFC-2069 digest GET /control
			local realm="$1" nonce ha1 ha2 resp
			nonce=$(curl -s -D - -o /dev/null --max-time "$TIMEOUT" "$(http_url "/control")" 2>/dev/null \
				| grep -oiE 'nonce="[0-9a-f]+"' | head -1 | cut -d'"' -f2)
			[ -n "$nonce" ] || { echo "nononce"; return; }
			ha1=$(dg_md5 "${HTTP_USER}:${realm}:${HTTP_PASS}")
			ha2=$(dg_md5 "GET:/control")
			resp=$(dg_md5 "${ha1}:${nonce}:${ha2}")
			http_code -H "Authorization: Digest username=\"${HTTP_USER}\", realm=\"${realm}\", nonce=\"${nonce}\", uri=\"/control\", response=\"${resp}\"" \
				"$(http_url "/control")"
		}
		local dg_good dg_bad
		dg_good=$(dg_try "timps")     # AUTH_REALM, src/auth.h
		if [ "$dg_good" = "nononce" ]; then
			skip "digest realm-binding: no Digest challenge in the 401 - cannot craft a request"
		elif [ "${dg_good#2}" = "$dg_good" ]; then
			skip "digest realm-binding: even the correct-realm hand-rolled digest got HTTP $dg_good - crafting no longer matches the server, wrong-realm verdict would be meaningless"
		else
			dg_bad=$(dg_try "forged_realm_$$")
			case "$dg_bad" in
				401|403) pass "digest realm binding holds: correct response over a FORGED realm rejected (HTTP $dg_bad; correct realm passed with $dg_good)";;
				2*)      fail "digest realm binding BROKEN: a response computed over a never-issued realm was accepted (HTTP $dg_bad)";;
				*)       skip "digest realm-binding: forged-realm request -> HTTP $dg_bad";;
			esac
		fi
	else
		skip "digest realm-binding test needs md5sum"
	fi

	# --- token surface (?token= / X-Timps-Token, httpd.c http_check_token):
	# a wrong token must fall through to the 401; the real one (HTTP_TOKEN
	# env - the per-boot secret lives on-device in http.token_file) must
	# unlock /control in both transport forms (separate parse paths).
	http_negative "/control?token=qa_wrong_$$" "/control (wrong ?token=)"
	if [ -n "${HTTP_TOKEN:-}" ]; then
		local tcode
		tcode=$(http_code "$(http_url "/control?token=${HTTP_TOKEN}")")
		case "$tcode" in
			2*) pass "/control accepts the real token via ?token= (HTTP $tcode)";;
			*)  fail "/control rejects the real token via ?token= (HTTP $tcode) - token auth broken or HTTP_TOKEN stale";;
		esac
		tcode=$(http_code -H "X-Timps-Token: ${HTTP_TOKEN}" "$(http_url "/control")")
		case "$tcode" in
			2*) pass "/control accepts the real token via X-Timps-Token header (HTTP $tcode)";;
			*)  fail "/control rejects the real token via the header (HTTP $tcode)";;
		esac
	else
		skip "token acceptance: set HTTP_TOKEN=<contents of http.token_file> to test (wrong-token rejection was tested above)"
	fi

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
	#
	# The verdict must NOT be "any nonzero exit = protected": a closed/
	# filtered port, a typo'd --srt-port, a timeout with no handshake at all
	# and a tool-level error all exit nonzero too, and each of those used to
	# count as PASS - an SRT listener that was never reached "passed" its
	# auth test. Keep the tool's output (no -q) and require an actual
	# REJECTION SIGNATURE in it before crediting the handshake-level reject;
	# everything else is a SKIP, because nothing about auth was observed.
	local srt_out srt_rc
	srt_out=$(timeout "$TIMEOUT" "$tool" "srt://${HOST}:${SRT_PORT}?mode=caller" /dev/null 2>&1 >/dev/null)
	srt_rc=$?
	if [ "$srt_rc" -eq 0 ]; then
		fail "SRT accepted a caller WITHOUT passphrase/streamid - NOT protected"
	elif printf '%s' "$srt_out" | grep -qiE 'reject|denied|SECURITYRES|KMREQ|wrong password|bad password|Connection setup failure'; then
		pass "SRT rejects caller without passphrase/streamid (handshake-level reject: $(printf '%s' "$srt_out" | grep -m1 -ioE '[^:]*reject[^,;]*|Connection setup failure[^,;]*' | head -c 80))"
	elif [ "$srt_rc" -eq 124 ]; then
		skip "SRT: no handshake response within ${TIMEOUT}s (timeout) - port closed/filtered or wrong --srt-port; nothing about auth was observed"
	else
		skip "SRT: caller failed without a recognizable reject signature (rc=$srt_rc: $(printf '%s' "$srt_out" | head -1 | head -c 100)) - cannot attribute the failure to auth"
	fi

	# positive counter-test, same sharpness idea as RTSP/HTTP above: only
	# possible when the caller is given the real secrets (never stored in
	# this repo; SRT_PASSPHRASE / SRT_STREAMID env)
	if [ -n "${SRT_PASSPHRASE:-}" ] || [ -n "${SRT_STREAMID:-}" ]; then
		local q="srt://${HOST}:${SRT_PORT}?mode=caller"
		[ -n "${SRT_PASSPHRASE:-}" ] && q="$q&passphrase=${SRT_PASSPHRASE}"
		[ -n "${SRT_STREAMID:-}" ]   && q="$q&streamid=${SRT_STREAMID}"
		if timeout "$TIMEOUT" "$tool" "$q" /dev/null >/dev/null 2>&1; then
			pass "SRT accepts the caller WITH the configured passphrase/streamid (test is sharp)"
		else
			fail "SRT rejected the caller WITH the configured passphrase/streamid - secrets wrong, or auth rejects everyone (negatives above pass for the wrong reason)"
		fi
	else
		skip "SRT positive test: set SRT_PASSPHRASE/SRT_STREAMID to prove correct credentials are accepted"
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
