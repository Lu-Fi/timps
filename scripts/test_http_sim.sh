#!/usr/bin/env bash
# test_http_sim.sh - HTTP behaviour checks against the host sim (make test-http-sim):
# the DNS-rebinding Host gate, the PNA preflight consent, and the rate-limited
# /control persistence. Starts its own sim on 18554/18880 with a scratch config.
# Host-gate checks need a non-loopback address (loopback peers are exempt);
# without one they are skipped. Exit code: 0 = all PASS.
set -u
SIM=${1:-./timpsd-sim}     # $2: a sim built with short CTL_* limits, $3: with SIM_ROT_ENV (Makefile)
D=$(mktemp -d)
trap 'kill $simpid 2>/dev/null; wait $simpid 2>/dev/null; rm -rf "$D"' EXIT
CONF=$D/timps.conf
sed -e 's/= 8554/= 18554/;s/= 8880/= 18880/' scripts/test_auth.conf > "$CONF"
echo "http.token = simtoken123" >> "$CONF"
"$SIM" -c "$CONF" > "$D/sim.log" 2>&1 &
simpid=$!
for _ in $(seq 50); do curl -s -o /dev/null http://127.0.0.1:18880/control && break; sleep 0.1; done

pass=0; fail=0; skip=0
ok()  { echo "PASS $*"; pass=$((pass+1)); }
bad() { echo "FAIL $*"; fail=$((fail+1)); }
sk()  { echo "SKIP $*"; skip=$((skip+1)); }
expect() { # name want got
    if [ "$2" = "$3" ]; then ok "$1 ($3)"; else bad "$1: want $2 got $3"; fi
}
code() { curl -s -o /dev/null -w '%{http_code}' --max-time 5 "$@"; }

IP=$(ip -4 route get 1.1.1.1 2>/dev/null | sed -n 's/.* src \([0-9.]*\).*/\1/p')
if [ -n "$IP" ]; then
    U="http://$IP:18880"
    HN=$(hostname)
    expect "Host = IP literal"          200 "$(code -u webuser:webpass "$U/control")"
    expect "Digest from the nonce's peer" 200 "$(code --digest -u webuser:webpass "$U/control")"
    expect "Host = foreign name"        421 "$(code -u webuser:webpass -H 'Host: attacker.example' "$U/control")"
    expect "foreign name, snapshot"     421 "$(code -H 'Host: attacker.example:18880' "$U/snapshot.jpg?token=simtoken123")"
    expect "foreign name, preflight"    421 "$(code -X OPTIONS -H 'Host: attacker.example' -H 'Origin: http://attacker.example' "$U/control")"
    expect "Host = hostname.local"      200 "$(code -u webuser:webpass -H "Host: $HN.local:18880" "$U/control")"
    expect "Host = localhost"           200 "$(code -u webuser:webpass -H 'Host: localhost' "$U/control")"
    pna() { curl -s -i -X OPTIONS --max-time 5 -H "Origin: $1" \
            -H 'Access-Control-Request-Private-Network: true' "$U/control$2" |
            grep -ci '^Access-Control-Allow-Private-Network: true'; }
    expect "PNA consent, cross-site"    0 "$(pna https://attacker.example '')"
    expect "PNA consent, same host"     1 "$(pna "http://$IP" '')"
    expect "PNA consent, token"         1 "$(pna https://attacker.example '?token=simtoken123')"
else
    sk "Host gate: no non-loopback IPv4 address"
fi
expect "loopback peer is exempt" 200 "$(code -H 'Host: attacker.example' http://127.0.0.1:18880/control)"

# a /control body must not depend on the header bytes in front of it
CK=$(head -c 3800 /dev/zero | tr '\0' a)
expect "POST behind 3.8 KB of cookies" 200 "$(code -H "Cookie: s=$CK" -X POST -d '{"image":{"contrast":120}}' http://127.0.0.1:18880/control)"
expect "POST body over 8 KB" 413 "$(code -X POST -d "{\"a\":\"$(head -c 9000 /dev/zero | tr '\0' x)\"}" http://127.0.0.1:18880/control)"

# /control persistence: first change written at once, a burst coalesced into
# one write at the end of the window, restart-required keys and shutdown flush
L=http://127.0.0.1:18880/control
post() { curl -s --max-time 5 -X POST "$L" -d "$1" | sed -n 's/.*"pending":\([0-9]*\).*/\1/p'; }
inconf() { sed -n "s/^$1 *= *//p" "$CONF"; }
sleep 3.5                                   # clear any window the checks above opened
expect "single POST: pending"          0  "$(post '{"image":{"brightness":11}}')"
expect "single POST: in the file"      11 "$(inconf image.brightness)"
expect "burst POST: pending"           1  "$(post '{"image":{"brightness":12}}')"
post '{"image":{"brightness":13}}' >/dev/null
expect "burst: file not yet rewritten" 11 "$(inconf image.brightness)"
expect "GET persist_pending"           1  "$(curl -s $L | sed -n 's/.*"persist_pending":\([0-9]*\).*/\1/p')"
sleep 4.5
expect "burst: last value after the window" 13 "$(inconf image.brightness)"
expect "GET persist_pending drained"   0  "$(curl -s $L | sed -n 's/.*"persist_pending":\([0-9]*\).*/\1/p')"
post '{"image":{"brightness":14}}' >/dev/null
post '{"image":{"brightness":15}}' >/dev/null
expect "restart-required key: pending" 0  "$(post '{"sensor":{"fps":20}}')"
expect "restart-required key: in the file" 20 "$(inconf sensor.fps)"
expect "...and took the pending set along" 15 "$(inconf image.brightness)"
expect "before shutdown: pending"      1  "$(post '{"image":{"brightness":16}}')"
kill $simpid; wait $simpid 2>/dev/null
expect "shutdown flush"                16 "$(inconf image.brightness)"

# wedged libimp call on the apply path (sim hook, short limits, a reboot
# marker that cannot be created so the escalation never reboots the host)
HSIM=${2:-}
if [ -n "$HSIM" ] && [ -x "$HSIM" ]; then
    sed -i '/^image.brightness/d;/^sensor.fps/d' "$CONF"
    TIMPS_SIM_CONTROL_HANG=image.hue:20 "$HSIM" -c "$CONF" > "$D/hang.log" 2>&1 &
    simpid=$!
    for _ in $(seq 50); do curl -s -o /dev/null $L && break; sleep 0.1; done
    curl -s -o /dev/null --max-time 30 -X POST "$L" -d '{"image":{"hue":99}}' &
    hpid=$!
    sleep 0.5
    t0=$(date +%s%N)
    r=$(curl -s -w ' %{http_code}' --max-time 10 -X POST "$L" -d '{"image":{"brightness":17}}')
    ms=$(( ($(date +%s%N) - t0) / 1000000 ))
    expect "POST behind a wedged apply: 503" 503 "${r##* }"
    expect "...reason busy" 1 "$(echo "$r" | grep -c '"reason":"busy"')"
    expect "...after the lock timeout, not never" 1 "$([ $ms -ge 1500 ] && [ $ms -lt 6000 ] && echo 1 || echo "0 (${ms} ms)")"
    expect "GET still answers" 200 "$(code $L)"
    gone=0
    for _ in $(seq 120); do kill -0 $simpid 2>/dev/null || { gone=1; break; }; sleep 0.1; done
    expect "supervisor ends the process" 1 "$gone"
    expect "...and says why" 1 "$(grep -c 'inside libimp for' "$D/hang.log")"
    expect "...through the watchdog escalation" 1 "$(grep -c 'runtime watchdog exited' "$D/hang.log")"
    kill $hpid 2>/dev/null; wait $hpid 2>/dev/null
else
    sk "wedged-apply supervisor: no hang sim given"
fi

# /control error codes against a sim that refuses rotations like T10/T20/T21
# OpenIMP (sub stream only, 16-aligned, <= 704x576)
RSIM=${3:-}
if [ -n "$RSIM" ] && [ -x "$RSIM" ]; then
    sed -i '/^video1.rotation/d;/^video1.width/d;/^video1.height/d' "$CONF"
    printf 'video1.width = 640\nvideo1.height = 360\nvideo1.rotation = 90\n' >> "$CONF"
    "$RSIM" -c "$CONF" > "$D/rot.log" 2>&1 &
    simpid=$!
    for _ in $(seq 50); do curl -s -o /dev/null $L && break; sleep 0.1; done
    jget() { python3 -c "import json,sys; j=json.load(sys.stdin); print($1)"; }
    perr() { curl -s --max-time 5 -X POST "$L" -d "$1" | jget "j.get('errors',{}).get('$2','-')"; }
    expect "caps.err_v"                      1   "$(curl -s $L | jget "j['caps']['err_v']")"
    expect "boot: 640x360 refused, state"    203 "$(curl -s $L | jget "j['video']['1']['rotation_state']")"
    expect "boot: unrotated stream, state"   0   "$(curl -s $L | jget "j['video']['0']['rotation_state']")"
    expect "boot: refused stream, path"      off "$(curl -s $L | jget "j['video']['1']['rotate_path']")"
    expect "POST main-stream rotation"       202 "$(perr '{"video":{"0":{"rotation":90}}}' video0.rotation)"
    expect "POST 16-aligned sub stream"      -   "$(perr '{"video":{"1":{"height":368}}}' video1.rotation)"
    expect "POST oversized sub stream"       204 "$(perr '{"video":{"1":{"width":1280,"height":720}}}' video1.rotation)"
    expect "POST unrelated key: no repeat"   -   "$(perr '{"video":{"1":{"bitrate":600}}}' video1.rotation)"
    expect "POST rotation 180 (T40/T41 only)" 201 "$(perr '{"video":{"1":{"rotation":180}}}' video1.rotation)"
    expect "POST rotation 45"                101 "$(perr '{"video":{"1":{"rotation":45}}}' video1.rotation)"
    expect "POST null value"                 101 "$(perr '{"image":{"brightness":null}}' image.brightness)"
    expect "POST unknown key"                102 "$(perr '{"image":{"bogus":1}}' image.bogus)"
    expect "POST file-only key"              103 "$(perr '{"video":{"0":{"imp_chn":0}}}' video0.imp_chn)"
    expect "clean POST has no errors key"    0   "$(curl -s -X POST $L -d '{"image":{"contrast":121}}' | grep -c '"errors"')"
    kill $simpid; wait $simpid 2>/dev/null
    sed -i '/^video[01].rotation/d;/^video1.width/d;/^video1.height/d' "$CONF"
    printf 'video1.width = 640\nvideo1.height = 368\nvideo1.rotation = 90\n' >> "$CONF"
    "$RSIM" -c "$CONF" > "$D/rot2.log" 2>&1 &
    simpid=$!
    for _ in $(seq 50); do curl -s -o /dev/null $L && break; sleep 0.1; done
    expect "boot: rotated stream, path"      hw  "$(curl -s $L | jget "j['video']['1']['rotate_path']")"
    expect "...eff_width swapped"            368 "$(curl -s $L | jget "j['video']['1']['eff_width']")"
    expect "boot: unrotated stream, path"    off "$(curl -s $L | jget "j['video']['0']['rotate_path']")"
    kill $simpid; wait $simpid 2>/dev/null
else
    sk "error codes: no rotation sim given"
fi

echo "test_http_sim: PASS=$pass FAIL=$fail SKIP=$skip"
[ $fail = 0 ]
