#!/bin/sh
# Forward NEW lines of the Ingenic alog into syslog. logcat dumps the whole
# buffer and exits, so ship only what is past the marker. Rationale: see
# scripts/logcat-ship.sh in the timps repo history.
TAG=logcat
STATE=/tmp/.logcat-ship
DUMP=/tmp/.logcat-dump.$$

trap 'rm -f "$DUMP"' EXIT
logcat >"$DUMP" 2>/dev/null || exit 0
total=$(wc -l <"$DUMP" 2>/dev/null) || exit 0
[ "$total" -gt 0 ] || exit 0

sent=0
if [ -r "$STATE" ]; then
	read -r sent marker <"$STATE" 2>/dev/null
	case "$sent" in ''|*[!0-9]*) sent=0 ;; esac
	# marker must still sit at $sent, else the ring wrapped
	if [ "$sent" -gt 0 ] && [ "$sent" -le "$total" ]; then
		at=$(sed -n "${sent}p" "$DUMP" 2>/dev/null)
		[ "$at" = "$marker" ] || {
			logger -t "$TAG" "--- alog buffer wrapped or restarted; re-sending ${total} line(s) ---"
			sent=0
		}
	else
		[ "$sent" -gt "$total" ] && {
			logger -t "$TAG" "--- alog buffer shrank (${sent} -> ${total}); re-sending ---"
			sent=0
		}
	fi
fi

[ "$total" -gt "$sent" ] || exit 0

# STATE in tmpfs: a reboot re-sends the buffer once, which is what you want.
sed -n "$((sent + 1)),\$p" "$DUMP" | while IFS= read -r line; do
	[ -n "$line" ] && logger -t "$TAG" "$line"
done

printf '%s %s\n' "$total" "$(sed -n "${total}p" "$DUMP")" >"$STATE"
