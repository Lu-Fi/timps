#!/usr/bin/env bash
# osd-rot-test.sh - run a rotation/OSD test config on the camera HEADLESS for a
# few seconds and pull the results back to ./osd-test-out/ so they can be
# inspected offline (unlike deploy.sh, which streams live and needs Ctrl-C).
#
# Collects, per run, into scripts/../osd-test-out/:
#   <conf>_<stamp>.log        timps -v stderr/stdout (OSD group dims, IMP errors)
#   <conf>_<stamp>.dmesg.log  kernel ring buffer tail (libimp/IPU driver errors)
#   <conf>_<stamp>.jpg        one decoded frame off rtsp://<cam>/ch0 (OSD baked
#                             in) - only if ffmpeg is installed locally
#
# Usage:
#   CAM=<ip> CAMERA=<profile> ./scripts/osd-rot-test.sh [conf] [seconds]
#     conf     defaults to scripts/camera-square-osd.conf
#     seconds  how long timps runs before teardown (default 8)
#
# Requires: an SSH key on the camera, a timpsd already built for CAMERA
# (build once with: CAM=<ip> CAMERA=<profile> ./scripts/deploy.sh --build).
set -euo pipefail

CAM="${CAM:-}"
THINGINO="${THINGINO:-$HOME/thingino-firmware-LuFi}"
SRCDIR="$(cd "$(dirname "$0")/.." && pwd)"
CONF="${1:-$SRCDIR/scripts/camera-square-osd.conf}"
SECS="${2:-8}"
OUT="$SRCDIR/osd-test-out"
[ -n "$CAM" ]  || { echo "!! set the camera IP:  CAM=<ip> $0 [conf] [seconds]"; exit 1; }
[ -f "$CONF" ] || { echo "!! conf not found: $CONF"; exit 1; }

# camera profile (same disambiguation as deploy.sh)
if [ -n "${CAMERA:-}" ]; then
    PROFILE="$CAMERA"
else
    PROFILE="$(for d in "$THINGINO"/output/*/*-"$CAM"; do
                   [ -d "$d" ] && basename "$d" | sed -E 's/-[0-9].*$//'
               done | sort -u)"
fi
[ -n "$PROFILE" ] || { echo "!! no thingino profile for IP $CAM - build once first"; exit 1; }
BIN="$(ls -t "$THINGINO"/output/*/"$PROFILE"-*/build/timps-*/timpsd 2>/dev/null | head -1 || true)"
[ -n "$BIN" ] || { echo "!! timpsd not found for $PROFILE - run deploy.sh --build once"; exit 1; }

# ports from the conf (fallbacks match the shipped test conf)
RTSP_PORT="$(sed -n 's/^[[:space:]]*rtsp\.port[[:space:]]*=[[:space:]]*\([0-9]\+\).*/\1/p' "$CONF" | head -1)"; RTSP_PORT="${RTSP_PORT:-554}"

mkdir -p "$OUT"
STAMP="$(date +%Y%m%d-%H%M%S)"
NAME="$(basename "$CONF" .conf)_${STAMP}"
LOG="$OUT/$NAME.log"; DMESG="$OUT/$NAME.dmesg.log"; SHOT="$OUT/$NAME.jpg"

echo ">> profile=$PROFILE"
echo ">> bin=$BIN"
echo ">> conf=$CONF   run=${SECS}s   rtsp=$RTSP_PORT"
echo ">> out=$OUT"

echo ">> stopping other streamers + clearing dmesg/logcat on $CAM ..."
ssh root@"$CAM" '/etc/init.d/S31raptor stop 2>/dev/null; killall -9 timpsd 2>/dev/null; killall -q rwd rhd rwc prudynt 2>/dev/null; dmesg -c >/dev/null 2>&1; logcat -c 2>/dev/null; sleep 1; true'

echo ">> copying binary + conf to /tmp ..."
scp -O "$BIN"  root@"$CAM":/tmp/timpsd     >/dev/null
scp -O "$CONF" root@"$CAM":/tmp/timps.conf >/dev/null

# forward experiment/diagnostic env knobs to the camera if set locally
ENVSTR=""
[ -n "${MS_OSD_ROT_SWAP:-}" ]          && ENVSTR="${ENVSTR}MS_OSD_ROT_SWAP=$MS_OSD_ROT_SWAP "
[ -n "${MS_OSD_TEST_STATIC:-}" ]       && ENVSTR="${ENVSTR}MS_OSD_TEST_STATIC=$MS_OSD_TEST_STATIC "
[ -n "${MS_ROTATE_PICHEIGHT_MAX:-}" ]  && ENVSTR="${ENVSTR}MS_ROTATE_PICHEIGHT_MAX=$MS_ROTATE_PICHEIGHT_MAX "
[ -n "${MS_ROTATE_PRUDYNT_STYLE:-}" ]  && ENVSTR="${ENVSTR}MS_ROTATE_PRUDYNT_STYLE=$MS_ROTATE_PRUDYNT_STYLE "
[ -n "$ENVSTR" ] && echo ">> forwarding env: $ENVSTR"

echo ">> starting timps headless (backgrounded on the camera) ..."
# 'env VAR=val prog' (not a leading assignment) - BusyBox ash on this firmware
# rejects a bare 'VAR=val prog' after setsid/exec.
ssh root@"$CAM" "chmod +x /tmp/timpsd; rm -f /tmp/timps-osd.log; \
    setsid env ${ENVSTR}/tmp/timpsd -c /tmp/timps.conf -v >/tmp/timps-osd.log 2>&1 & \
    echo \$! >/tmp/timps-osd.pid; true"

echo ">> warming up (5s) ..."
sleep 5

if command -v ffmpeg >/dev/null 2>&1; then
    echo ">> grabbing one frame off rtsp://$CAM:$RTSP_PORT/ch0 (OSD baked in) ..."
    ffmpeg -nostdin -loglevel error -rtsp_transport tcp -y \
        -i "rtsp://$CAM:$RTSP_PORT/ch0" -frames:v 1 "$SHOT" 2>>"$OUT/$NAME.ffmpeg.err" \
        && echo "   -> $SHOT" || echo "   (frame grab failed - see $NAME.ffmpeg.err)"
else
    echo ">> (ffmpeg not installed locally - skipping the frame grab; view rtsp://$CAM:$RTSP_PORT/ch0 in mpv)"
fi

# CPU snapshot while streaming (is the IPU spam a per-frame SW fallback = CPU?)
echo ">> CPU snapshot ..."
ssh root@"$CAM" 'echo "== loadavg =="; cat /proc/loadavg; echo "== top (timpsd) =="; top -bn1 2>/dev/null | grep -E "timpsd|CPU:|Mem:" | head' \
    > "$OUT/$NAME.cpu.txt" 2>&1 || true
grep -E 'loadavg|timpsd|CPU:' "$OUT/$NAME.cpu.txt" 2>/dev/null | head

REST=$(( SECS - 5 )); [ "$REST" -gt 0 ] && { echo ">> letting it run ${REST}s more ..."; sleep "$REST"; }

echo ">> collecting log + dmesg + logcat (IMP_LOG), stopping timps ..."
# logcat = Ingenic libimp's own IMP_LOG channel (often the only place the real
# OSD/IPU reason is printed). This device's logcat has no -d (dump) - it follows;
# it was cleared at start, so a short timed follow BEFORE we kill timps captures
# everything libimp logged during the run.
ssh root@"$CAM" 'command -v logcat >/dev/null 2>&1 && timeout 3 logcat >/tmp/timps-logcat.log 2>&1 || echo "(no logcat on device)" >/tmp/timps-logcat.log; true'
ssh root@"$CAM" 'kill "$(cat /tmp/timps-osd.pid 2>/dev/null)" 2>/dev/null; sleep 1; \
    killall -9 timpsd 2>/dev/null; dmesg | tail -n 160 >/tmp/timps-dmesg.log 2>&1; true'
scp -O root@"$CAM":/tmp/timps-osd.log    "$LOG"            >/dev/null 2>&1 || echo "   (log fetch failed)"
scp -O root@"$CAM":/tmp/timps-dmesg.log  "$DMESG"          >/dev/null 2>&1 || echo "   (dmesg fetch failed)"
scp -O root@"$CAM":/tmp/timps-logcat.log "$OUT/$NAME.logcat.log" >/dev/null 2>&1 || echo "   (logcat fetch failed)"

echo
echo "======================================================================"
echo ">> DONE. Ergebnisse in $OUT/ :"
echo "   log:   $(basename "$LOG")"
echo "   dmesg: $(basename "$DMESG")"
[ -f "$SHOT" ] && echo "   frame: $(basename "$SHOT")"
echo
echo ">> Kurzauszug rotate/OSD/IMP aus dem Log:"
grep -iE 'rotate|osd|scaler|picwidth|invalid|param|CreateChn|SetChn|framesource|encoder|error|warn' "$LOG" 2>/dev/null | head -40 || true
echo "======================================================================"
echo ">> Sag Claude Bescheid - er liest osd-test-out/ direkt aus."
