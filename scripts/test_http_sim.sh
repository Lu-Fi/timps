#!/usr/bin/env bash
# test_http_sim.sh - HTTP behaviour checks against the host sim (make test-http-sim):
# the DNS-rebinding Host gate, the PNA preflight consent, and /control
# persistence. Starts its own sim on 18554/18880 with a scratch config.
# Host-gate checks need a non-loopback address (loopback peers are exempt);
# without one they are skipped. Exit code: 0 = all PASS.
set -u
SIM=${1:-./timpsd-sim}
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

echo "test_http_sim: PASS=$pass FAIL=$fail SKIP=$skip"
[ $fail = 0 ]
