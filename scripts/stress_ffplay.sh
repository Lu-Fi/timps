#!/usr/bin/env bash
# Open several small ffplay windows against the camera to stress-test the
# RTSP server, then close them all on a keypress.
#
# The camera IP is REQUIRED (as an argument or via CAM=...):
#   ./scripts/stress_ffplay.sh <ip>              # 5x ch0 over UDP
#   ./scripts/stress_ffplay.sh <ip> ch1 tcp      # 5x ch1 over TCP
#   ./scripts/stress_ffplay.sh <ip> ch0 udp 8    # 8 windows
#   CAM=<ip> ./scripts/stress_ffplay.sh ch0 tcp
#
# The other args are order-independent: ch0|ch1, tcp|udp, a number (count).
# Env overrides: USER/PASS (RTSP credentials), PORT (554).
set -uo pipefail

CAM="${CAM:-}"
PORT="${PORT:-554}"
CH=ch0
TR=udp
N=5

for a in "$@"; do
    case "$a" in
        ch0|ch1)         CH="$a" ;;
        tcp|udp)         TR="$a" ;;
        *.*.*.*)         CAM="$a" ;;
        ''|*[!0-9]*)     echo "ignoring unknown arg: $a" ;;
        *)               N="$a" ;;
    esac
done

[ -n "$CAM" ] || { echo "usage: $0 <camera-ip> [ch0|ch1] [tcp|udp] [count]   (or set CAM=<ip>)"; exit 1; }

command -v ffplay >/dev/null || { echo "ffplay not found (install ffmpeg)"; exit 1; }

AUTH=""
if [ -n "${USER:-}" ]; then AUTH="${USER}:${PASS:-}@"; fi
URL="rtsp://${AUTH}${CAM}:${PORT}/${CH}"

# tiled windows, bordered so they stay movable + resizable. Windows are laid
# out in a row; COLS controls how many per row before wrapping. Placement is
# done AFTER launch via wmctrl/xdotool because most WMs ignore SDL position
# hints (SDL2 centers windows by default).
W="${W:-420}"; H="${H:-240}"; GX=20; GY=60

# detect the usable screen size so windows never get moved off-screen
SCREEN_W="${SCREEN_W:-}"; SCREEN_H="${SCREEN_H:-}"
if [ -z "$SCREEN_W" ] || [ -z "$SCREEN_H" ]; then
    if command -v xdotool >/dev/null; then
        read -r SCREEN_W SCREEN_H < <(xdotool getdisplaygeometry 2>/dev/null) || true
    fi
fi
if [ -z "$SCREEN_W" ] && command -v xrandr >/dev/null; then
    geo=$(xrandr 2>/dev/null | awk '/\*/{print $1; exit}')
    SCREEN_W="${geo%x*}"; SCREEN_H="${geo#*x}"
fi
SCREEN_W="${SCREEN_W:-1920}"; SCREEN_H="${SCREEN_H:-1080}"

# columns per row that actually fit on the screen (env COLS overrides)
COLS="${COLS:-$(( (SCREEN_W - GX) / (W + 12) ))}"
[ "$COLS" -lt 1 ] 2>/dev/null && COLS=1
[ -z "$COLS" ] && COLS=1

MOVER=""
command -v wmctrl  >/dev/null && MOVER=wmctrl
[ -z "$MOVER" ] && command -v xdotool >/dev/null && MOVER=xdotool
[ -z "$MOVER" ] && echo "note: install 'wmctrl' or 'xdotool' to auto-tile the windows"

# position for window index i (1-based): tile left-to-right, wrap, clamp
win_xy() {
    local i="$1" col row
    col=$(( (i-1) % COLS )); row=$(( (i-1) / COLS ))
    X=$(( GX + col*(W+12) )); Y=$(( GY + row*(H+50) ))
    (( X + W > SCREEN_W )) && X=$(( SCREEN_W - W - 4 ))
    (( Y + H > SCREEN_H )) && Y=$(( SCREEN_H - H - 40 ))
    (( X < 0 )) && X=0; (( Y < 0 )) && Y=0
}

place() {  # place <title> <x> <y>  (move only, keep ffplay's own size)
    local title="$1" x="$2" y="$3" id
    case "$MOVER" in
        wmctrl)  wmctrl -r "$title" -e "0,${x},${y},-1,-1" 2>/dev/null ;;
        xdotool) id=$(xdotool search --name "$title" 2>/dev/null | head -1)
                 [ -n "$id" ] && xdotool windowmove "$id" "$x" "$y" 2>/dev/null ;;
    esac
}

echo ">> starting $N ffplay on rtsp://${CAM}:${PORT}/${CH} (${TR})  [screen ${SCREEN_W}x${SCREEN_H}, ${COLS}/row]"
pids=(); titles=()
for i in $(seq 1 "$N"); do
    title="ffplay #$i ${CH}/${TR}"
    win_xy "$i"
    SDL_VIDEO_WINDOW_POS="${X},${Y}" \
    ffplay -rtsp_transport "$TR" \
           -fflags nobuffer -flags low_delay -framedrop -avioflags direct \
           -x "$W" -y "$H" \
           -window_title "$title" \
           -loglevel warning "$URL" >/dev/null 2>&1 &
    pids+=($!); titles+=("$title")
    sleep 0.3
done

# give the windows a moment to map, then tile them
if [ -n "$MOVER" ]; then
    sleep 1.5
    for i in $(seq 1 "$N"); do
        win_xy "$i"
        place "${titles[$((i-1))]}" "$X" "$Y"
    done
    [ "$MOVER" = wmctrl ] && echo ">> visible ffplay windows: $(wmctrl -l 2>/dev/null | grep -c 'ffplay #')"
fi

echo ">> $N windows open. Press any key to close them all..."
read -r -n1 -s

echo ">> closing ffplay windows ..."
for p in "${pids[@]}"; do kill "$p" 2>/dev/null; done
sleep 0.5
for p in "${pids[@]}"; do kill -9 "$p" 2>/dev/null; done
wait 2>/dev/null
echo ">> done"
