#!/usr/bin/env bash
# dn-isp-probe.sh - measure whether the AE actually shortens the INTEGRATION
# TIME once its gain rails at the [24.8] floor.
#
# WHY THIS EXISTS. The 2026-08-17 day/night redesign decides on an exposure
# index, D = total_gain * integration_time / max_integration_time, rather than
# on total_gain alone - because total_gain has a hard floor at 256 (1.0x) and
# is therefore blind to any further brightening once the AE rails there. That
# is a claim about how these ISPs behave, and the whole redesign leans on it,
# so it is worth measuring rather than assuming. A daytime snapshot already
# confirms the bright end (cameras sitting at gain 256 with the integration
# time at 5-9% of max). What this records is the other half: a camera in NIGHT
# mode whose own illuminator lights the scene enough to rail the gain - the
# cam-J / cam-H regime, resting at 256-268, where no gain-based
# trigger can ever work. If the integration time moves there, the index has
# range exactly where the old design was blind. If it does not, the index
# degrades to plain gain (no harm, no gain either) and the heartbeat is what
# carries those cameras - see dev_notes/DAYNIGHT_REDESIGN_2026-08-17.md §9.1.
#
# WHAT IT INSTALLS, so it can be undone by hand if this script is not around:
#   /etc/dn-isp-log.sh          one-shot sampler, ~30 lines of ash
#   /etc/cron/crontabs/root     one added line: "* * * * * /etc/dn-isp-log.sh"
#   /tmp/dn-isp.csv             the samples - TMPFS, capped at 4000 lines
# Nothing else is touched. /etc is a jffs2 overlay so the cron line survives a
# reboot (which is the point - the interesting hours are unattended); /tmp does
# not, so a reboot costs the samples taken so far and nothing else.
#
#   ./scripts/dn-isp-probe.sh install <ip>...
#   ./scripts/dn-isp-probe.sh status  <ip>...
#   ./scripts/dn-isp-probe.sh fetch   <ip>...     # -> dn-isp-<host>.csv here
#   ./scripts/dn-isp-probe.sh report  <csv>...    # analyse fetched files
#   ./scripts/dn-isp-probe.sh remove  <ip>...     # full uninstall
set -uo pipefail

SSH="ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=4"
CMD="${1:-}"; shift || true

# The on-camera sampler. Deliberately one shot per invocation with no daemon
# and no lock: cron restarts it every minute, so there is nothing to leak,
# nothing to supervise, and a reboot resumes it by itself.
read -r -d '' LOGGER <<'EOF'
#!/bin/sh
# day/night exposure-index measurement - see scripts/dn-isp-probe.sh in timps
OUT=/tmp/dn-isp.csv
MAX=4000                      # ~66 h at one sample a minute, ~250 KB of tmpfs
# isp-m0 on T31/T23, isp_info on T20 - the same split daynight.c's dn_read()
# falls back over. Reading only the first would have collected nothing at all
# from cam-K and cam-J, which are the two cameras this measurement
# most needs (dark cellars, light switched on by hand).
D=$(cat /proc/jz/isp/isp-m0 2>/dev/null || cat /proc/jz/isp/isp_info 2>/dev/null)
[ -n "$D" ] || exit 0
[ -f "$OUT" ] || echo "epoch,mode,int,max_int,again,dgain,ispdgain" > "$OUT"
[ "$(wc -l < "$OUT" 2>/dev/null || echo 0)" -lt "$MAX" ] || exit 0
f() { echo "$D" | sed -n "s/^$1 : *\([0-9]*\).*/\1/p" | head -1; }
M=$(echo "$D" | sed -n 's/.*ISP Runing Mode : *//p' | tr -d ' \r' | head -1)
echo "$(date +%s),${M:-?},$(f 'SENSOR Integration Time'),$(f 'SENSOR Max Integration Time'),$(f 'SENSOR analog gain'),$(f 'SENSOR digital gain'),$(f 'ISP digital gain')" >> "$OUT"
EOF

install_one() {
    local ip="$1"
    local host
    host=$($SSH "root@$ip" hostname 2>/dev/null) || { echo "!! $ip unreachable"; return 1; }
    printf '%s\n' "$LOGGER" | $SSH "root@$ip" 'cat > /etc/dn-isp-log.sh && chmod +x /etc/dn-isp-log.sh' || {
        echo "!! $host: cannot write /etc/dn-isp-log.sh"; return 1; }
    # idempotent: strip any previous line before adding, so re-running does not
    # accumulate duplicate cron entries
    $SSH "root@$ip" '
        C=/etc/cron/crontabs/root
        [ -f "$C" ] || : > "$C"
        grep -v "dn-isp-log.sh" "$C" > "$C.new" 2>/dev/null || : > "$C.new"
        echo "* * * * * /etc/dn-isp-log.sh" >> "$C.new"
        mv "$C.new" "$C"
        /etc/dn-isp-log.sh
        # NO "kill -HUP crond" here. busybox crond installs no SIGHUP handler,
        # so the default action applies and HUP KILLS IT - which is exactly
        # what happened on cam-H the first time this script ran.
        # It rescans the crontab directory by mtime on its own, and mv above
        # changed the mtime, so there is nothing to signal. Restart it only if
        # it is not running at all.
        pgrep crond >/dev/null 2>&1 || {
            if [ -x /etc/init.d/S*cron* ]; then /etc/init.d/S*cron* start >/dev/null 2>&1
            else /usr/sbin/crond -b -c /etc/cron/crontabs; fi
            echo "(crond was not running - restarted)"
        }
        echo "installed, $(wc -l < /tmp/dn-isp.csv 2>/dev/null || echo 0) line(s)"' \
        | sed "s/^/   $host: /"
}

status_one() {
    local ip="$1"
    $SSH "root@$ip" '
        H=$(hostname)
        N=$(wc -l < /tmp/dn-isp.csv 2>/dev/null || echo 0)
        C=$(grep -c dn-isp-log.sh /etc/cron/crontabs/root 2>/dev/null || echo 0)
        MODES=$(tail -n +2 /tmp/dn-isp.csv 2>/dev/null | cut -d, -f2 | sort | uniq -c | tr "\n" " ")
        printf "%-20s cron=%s samples=%-5s %s\n" "$H" "$C" "$N" "$MODES"' 2>/dev/null \
        || echo "!! $ip unreachable"
}

fetch_one() {
    local ip="$1" host
    host=$($SSH "root@$ip" hostname 2>/dev/null) || { echo "!! $ip unreachable"; return 1; }
    if $SSH "root@$ip" 'cat /tmp/dn-isp.csv' > "dn-isp-$host.csv" 2>/dev/null &&
       [ -s "dn-isp-$host.csv" ]; then
        echo "   $host: $(($(wc -l < "dn-isp-$host.csv") - 1)) samples -> dn-isp-$host.csv"
    else
        rm -f "dn-isp-$host.csv"; echo "   $host: no samples yet"
    fi
}

remove_one() {
    local ip="$1"
    $SSH "root@$ip" '
        C=/etc/cron/crontabs/root
        if [ -f "$C" ]; then
            grep -v "dn-isp-log.sh" "$C" > "$C.new"; mv "$C.new" "$C"
        fi
        rm -f /etc/dn-isp-log.sh /tmp/dn-isp.csv
        echo "$(hostname): removed"' 2>/dev/null | sed 's/^/   /' \
        || echo "!! $ip unreachable"
}

case "$CMD" in
    install) echo "installing sampler (1/min, tmpfs, capped):"; for ip; do install_one "$ip"; done ;;
    status)  for ip; do status_one "$ip"; done ;;
    fetch)   echo "fetching:"; for ip; do fetch_one "$ip"; done ;;
    remove)  echo "removing:"; for ip; do remove_one "$ip"; done ;;
    report)  python3 "$(dirname "$0")/dn-isp-report.py" "$@" ;;
    *) grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 1 ;;
esac
