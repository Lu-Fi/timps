#!/bin/sh
# Forward NEW lines of the Ingenic alog (logcat) into syslog, so they reach the
# central collector alongside timps' own log.
#
# WHY INCREMENTAL. logcat is not a stream: it dumps the accumulated alog buffer
# and returns immediately (measured: 0 s, identical output on back-to-back
# runs). Shipping the whole dump on a schedule would re-send everything every
# time. But the buffer is not static either - it grows with runtime events, and
# by very different amounts: over the same 61 hours of uptime the quietest
# camera in this fleet held 145 lines and the loudest 1106, with warning/error
# counts from 16 to 253. Those later lines are exactly the runtime faults worth
# having, and a one-shot capture at daemon start would miss all of them.
#
# So: dump, work out how much of it we have already sent, forward the rest.
#
# WHY THE MARKER IS A LINE PLUS A COUNT. The alog is a ring. If it wraps between
# two runs, a stored line COUNT alone silently under-reports - the same trap that
# made the QA harness read a wrapped 64 KB syslog ring as "the hook never ran".
# So the marker also stores the last line we shipped; if that line is no longer
# where it should be, the buffer was truncated or restarted and everything is
# re-sent, with an explicit gap notice rather than a silent hole.
#
# Volume is not a concern: the loudest camera here accumulates about 30 KB a day.

TAG=logcat
STATE=/tmp/.logcat-ship            # tmpfs on purpose - see the restart note below
DUMP=/tmp/.logcat-dump.$$

trap 'rm -f "$DUMP"' EXIT
logcat >"$DUMP" 2>/dev/null || exit 0
total=$(wc -l <"$DUMP" 2>/dev/null) || exit 0
[ "$total" -gt 0 ] || exit 0

sent=0
if [ -r "$STATE" ]; then
	read -r sent marker <"$STATE" 2>/dev/null
	case "$sent" in ''|*[!0-9]*) sent=0 ;; esac
	# The marker line must still sit at position $sent. If it does not, the ring
	# wrapped or the daemon restarted and the offset is meaningless.
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

# STATE lives in /tmp (tmpfs), so a reboot re-sends the whole buffer once. That
# is deliberate: after a restart the startup sequence is the part worth having,
# and it is ~15 KB.
sed -n "$((sent + 1)),\$p" "$DUMP" | while IFS= read -r line; do
	[ -n "$line" ] && logger -t "$TAG" "$line"
done

printf '%s %s\n' "$total" "$(sed -n "${total}p" "$DUMP")" >"$STATE"
