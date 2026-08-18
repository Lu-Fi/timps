#!/usr/bin/env bash
# dn-irprobe.sh - measure the IR-CONTRIBUTION RATIO on a camera fleet.
#
# THE QUESTION. In night mode the camera lights its own scene, so an absolute
# exposure reading says nothing about ambient light - that is the defect at the
# root of this whole subsystem. But switching the IR illuminator off for a few
# seconds turns the interference into the measurement:
#
#     r = D(IR off) / D(IR on)
#
#     r >> 1   removing the IR made it much darker -> the IR was doing the
#              work -> genuinely night
#     r ~= 1   removing the IR changed nothing -> the room supplies the light
#              -> day, whatever the absolute level happens to be
#
# r is dimensionless, so it needs no per-camera calibration - which is the
# whole point, given that genuine daylight spans a factor of 63 across this
# fleet at one instant. Midday spot checks gave 25.1 in a dark cellar against
# 1.27/1.41 in rooms that should have been day and 0.96/1.00 in daylight. What
# is still missing, and what this collects, is r ACROSS a transition: how it
# behaves while it is actually getting light.
#
# WHAT IT COSTS. Each probe darkens the image for about 8 s (the AE settles in
# 3-4 s, measured). Motion detection may see that. Nothing else moves: the
# IR-cut filter is NOT touched, so there is no audible click and no mechanical
# wear - only a GPIO write to the illuminator.
#
# SAFETY. The illuminator is restored three ways: on the normal path, from an
# EXIT/INT/TERM trap, and from a detached watchdog that fires after 60 s even
# if the shell is killed outright. A probe that dies still leaves the camera
# lit.
#
#   ./scripts/dn-irprobe.sh install <ip>...   # cron every 20 min, 21:00-09:00
#   ./scripts/dn-irprobe.sh once    <ip>...   # one probe now, print the ratio
#   ./scripts/dn-irprobe.sh status  <ip>...
#   ./scripts/dn-irprobe.sh fetch   <ip>...   # -> dn-ir-<host>.csv here
#   ./scripts/dn-irprobe.sh remove  <ip>...
set -uo pipefail
SSH="ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=4"
CMD="${1:-}"; shift || true

read -r -d '' PROBE <<'EOF'
#!/bin/sh
# IR-contribution probe - see scripts/dn-irprobe.sh in the timps repo.
# The output must SURVIVE A REBOOT: /tmp is tmpfs, and losing a night of
# measurements to an unlucky restart costs a whole day (it did once). Prefer
# the SD card, fall back to the /etc overlay - which is small on some boards
# (576 KB free measured), hence the reduced MAX below: 600 lines = 300 probes
# = about eight nights at ~27 KB.
OUT=""
for d in /mnt/mmcblk0p1 /media/mmcblk0p1 /sdcard; do
  [ -d "$d" ] && [ -w "$d" ] && { OUT="$d/dn-ir.csv"; break; }
done
[ -n "$OUT" ] || OUT=/etc/dn-ir.csv
# carry over anything the earlier /tmp-based version already collected
[ -f /tmp/dn-ir.csv ] && [ ! -f "$OUT" ] && cp /tmp/dn-ir.csv "$OUT" 2>/dev/null
MAX=600
SETTLE=8

# which illuminator does this board have?
L=""
for t in ir850 ir940 ir; do
  [ -n "$(light $t gpio 2>/dev/null)" ] && { L=$t; break; }
done
[ -n "$L" ] || exit 0
# only meaningful while the camera is in night mode with the IR actually on
[ "$(light $L read 2>/dev/null)" = "1" ] || exit 0

read_isp() {
  D=$(cat /proc/jz/isp/isp-m0 2>/dev/null || cat /proc/jz/isp/isp_info 2>/dev/null)
  [ -n "$D" ] || return 1
  f() { echo "$D" | sed -n "s/^$1 : *\([0-9]*\).*/\1/p" | head -1; }
  M=$(echo "$D" | sed -n 's/.*ISP Runing Mode : *//p' | tr -d ' \r' | head -1)
  echo "$M,$(f 'SENSOR Integration Time'),$(f 'SENSOR Max Integration Time'),$(f 'SENSOR analog gain'),$(f 'SENSOR digital gain'),$(f 'ISP digital gain'),$(f 'MAX SENSOR analog gain'),$(f 'MAX ISP digital gain')"
}

[ -f "$OUT" ] || echo "epoch,phase,mode,int,max_int,again,dgain,ispdgain,max_again,max_ispdgain" > "$OUT"
[ "$(wc -l < "$OUT" 2>/dev/null || echo 0)" -lt "$MAX" ] || exit 0

ON=$(read_isp) || exit 0
# watchdog first: the light comes back even if this shell is killed outright
( sleep 60; light $L on ) >/dev/null 2>&1 &
WD=$!
trap "light $L on 2>/dev/null" EXIT INT TERM

T=$(date +%s)
light $L off 2>/dev/null
sleep $SETTLE
OFF=$(read_isp)
light $L on 2>/dev/null
kill $WD 2>/dev/null

echo "$T,on,$ON"   >> "$OUT"
[ -n "$OFF" ] && echo "$T,off,$OFF" >> "$OUT"
EOF

install_one() {
    local ip="$1" host
    host=$($SSH "root@$ip" hostname 2>/dev/null) || { echo "!! $ip nicht erreichbar"; return 1; }
    printf '%s\n' "$PROBE" | $SSH "root@$ip" 'cat > /etc/dn-irprobe.sh && chmod +x /etc/dn-irprobe.sh' || {
        echo "!! $host: /etc/dn-irprobe.sh nicht schreibbar"; return 1; }
    $SSH "root@$ip" '
        C=/etc/cron/crontabs/root
        [ -f "$C" ] || : > "$C"
        grep -v "dn-irprobe.sh" "$C" > "$C.new" 2>/dev/null || : > "$C.new"
        # Alle 20 min, 19:00-09:00. Der Start lag urspruenglich bei 21:00, weil die
        # Abenddaemmerung am 2026-08-17 erst um 20:55 einsetzte - unter dichter
        # Bewoelkung verdunkelt es aber deutlich frueher, und ein zu spaeter Start
        # verpasst genau den Uebergang, den die Messung braucht. Zwei Stunden
        # Vorlauf kosten nichts: die Probe steigt sofort wieder aus, solange die
        # Kamera im Tagmodus ist (die IR-Beleuchtung ist dann aus).
        # Zwei Kadenzen: alle 5 min durch die Abenddaemmerung (20:00-21:59),
        # alle 20 min fuer den Rest der Nacht. Ein natuerlicher Uebergang dauert
        # rund eine Stunde (Faktor 2,2 ueber 67 min am 2026-08-17 gemessen) -
        # mit 20-Minuten-Abstand liegen darin drei Punkte, zu wenig, um den
        # Verlauf des IR-Verhaeltnisses ueber den Uebergang zu zeichnen. Fuenf
        # Minuten geben rund zwoelf. Ausserhalb der Daemmerung reicht die grobe
        # Kadenz: dort aendert sich nichts, was man verpassen koennte.
        echo "*/5  20,21 * * * /etc/dn-irprobe.sh" >> "$C.new"
        echo "*/20 19,22,23,0,1,2,3,4,5,6,7,8,9 * * * /etc/dn-irprobe.sh" >> "$C.new"
        mv "$C.new" "$C"
        # KEIN kill -HUP: busybox crond hat keinen SIGHUP-Handler, das Signal
        # beendet es. Es liest das Verzeichnis per mtime ohnehin neu.
        pgrep crond >/dev/null 2>&1 || /etc/init.d/S50crond start >/dev/null 2>&1
        echo "installiert, Illuminator=$(for t in ir850 ir940 ir; do [ -n "$(light $t gpio 2>/dev/null)" ] && echo $t && break; done)"' \
        | sed "s/^/   $host: /"
}

case "$CMD" in
    install) echo "IR-Verhaeltnismessung, alle 5 min 20-22 Uhr (Daemmerung), sonst alle 20 min bis 09:00:"; for ip; do install_one "$ip"; done ;;
    once)    for ip; do $SSH "root@$ip" '/etc/dn-irprobe.sh; F=$(ls /mnt/mmcblk0p1/dn-ir.csv /media/mmcblk0p1/dn-ir.csv /sdcard/dn-ir.csv /etc/dn-ir.csv /tmp/dn-ir.csv 2>/dev/null | head -1); tail -2 "$F"' 2>/dev/null | sed "s/^/   /"; done ;;
    status)  for ip; do $SSH "root@$ip" 'F=$(ls /mnt/mmcblk0p1/dn-ir.csv /media/mmcblk0p1/dn-ir.csv /sdcard/dn-ir.csv /etc/dn-ir.csv /tmp/dn-ir.csv 2>/dev/null | head -1); printf "%-20s cron=%s Proben=%-4s abgelegt=%s\n" "$(hostname)" "$(grep -c dn-irprobe /etc/cron/crontabs/root 2>/dev/null)" "$(( ($(wc -l < "$F" 2>/dev/null || echo 1) - 1) / 2 ))" "${F:-keine}"' 2>/dev/null || echo "!! $ip"; done ;;
    fetch)   for ip; do h=$($SSH "root@$ip" hostname 2>/dev/null); $SSH "root@$ip" 'F=$(ls /mnt/mmcblk0p1/dn-ir.csv /media/mmcblk0p1/dn-ir.csv /sdcard/dn-ir.csv /etc/dn-ir.csv /tmp/dn-ir.csv 2>/dev/null | head -1); cat "$F"' > "dn-ir-$h.csv" 2>/dev/null && echo "   $h: $(( ($(wc -l < dn-ir-$h.csv) - 1) / 2 )) Proben"; done ;;
    remove)  for ip; do $SSH "root@$ip" 'C=/etc/cron/crontabs/root; grep -v dn-irprobe "$C" > "$C.new"; mv "$C.new" "$C"; rm -f /etc/dn-irprobe.sh /tmp/dn-ir.csv /etc/dn-ir.csv /mnt/mmcblk0p1/dn-ir.csv /media/mmcblk0p1/dn-ir.csv /sdcard/dn-ir.csv; echo "$(hostname): entfernt"' 2>/dev/null | sed 's/^/   /'; done ;;
    *) grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 1 ;;
esac
