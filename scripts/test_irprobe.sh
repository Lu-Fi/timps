#!/bin/sh
# Host test for the firmware's timps-irprobe hook (daynight.irprobe_cmd).
# The hook lives in thingino-firmware: package/timps/files/timps-irprobe.
#
#   scripts/test_irprobe.sh <path/to/timps-irprobe>
#
# Stubs `light` and `jct` on PATH; the hook's /run and 60 s watchdog are
# redirected via STAMP_DIR and IRPROBE_WATCHDOG_S. Runs every case under
# `sh` and, if present, `busybox sh`. Exit 0 = all passed.
HOOK=${1:-$IRPROBE}
[ -f "$HOOK" ] || {
	echo "usage: $0 <path/to/timps-irprobe>" >&2
	exit 2
}
HOOK=$(cd "$(dirname "$HOOK")" && pwd)/$(basename "$HOOK")

TOP=$(mktemp -d)
trap 'rm -rf "$TOP"' EXIT
mkdir "$TOP/bin"

# light <type> on|off|gpio; pins in $STUB_PINS, "<type>:<mode>" in $STUB_FAIL fails
cat >"$TOP/bin/light" <<'EOF'
#!/bin/sh
case " $STUB_PINS " in *" $1 "*) pin=1 ;; *) pin= ;; esac
case "$2" in
gpio) [ -n "$pin" ] && echo 42 || exit 1 ;;
on | off)
	[ -n "$pin" ] || exit 1
	case " $STUB_FAIL " in *" $1:$2 "*) exit 1 ;; esac
	echo "$2" >"$T/state.$1"
	echo "$1 $2" >>"$T/log"
	;;
*) exit 1 ;;
esac
EOF
# jct <file> get <key>; file holds key=value lines
cat >"$TOP/bin/jct" <<'EOF'
#!/bin/sh
[ "$2" = get ] && [ -f "$1" ] || exit 1
v=$(sed -n "s/^$3=//p" "$1")
[ -n "$v" ] && echo "$v"
EOF
chmod +x "$TOP/bin/light" "$TOP/bin/jct"
PATH="$TOP/bin:$PATH"
export PATH

pass=0
fail=0
n=0

ok() {
	pass=$((pass + 1))
	echo "  PASS  $SH: $*"
}
bad() {
	fail=$((fail + 1))
	echo "  FAIL  $SH: $*"
}

# setup <pins> [key=value...]: fresh dir, all pinned lights "on"
setup() {
	n=$((n + 1))
	T="$TOP/t$n"
	mkdir "$T"
	STUB_PINS=$1
	STUB_FAIL=
	shift
	: >"$T/cfg"
	for kv; do echo "daynight.controls.$kv" >>"$T/cfg"; done
	for p in $STUB_PINS; do echo on >"$T/state.$p"; done
	: >"$T/log"
	export T STUB_PINS STUB_FAIL
}

hook() {
	THINGINO_CONFIG="$T/cfg" STAMP_DIR="$T" IRPROBE_WATCHDOG_S=${WD:-1} \
		$SH "$HOOK" "$@" >/dev/null 2>&1
}

st() { cat "$T/state.$1" 2>/dev/null || echo none; }

# expect <name> <type=state...>
expect() {
	name=$1
	shift
	for e; do
		got=$(st "${e%=*}")
		[ "$got" = "${e#*=}" ] || {
			bad "$name: ${e%=*} is $got, want ${e#*=}"
			return
		}
	done
	ok "$name"
}

run_cases() {
	setup "ir850"
	hook off
	rc=$?
	[ $rc -eq 0 ] || bad "single: off rc=$rc"
	expect "single IR: off" ir850=off
	[ -f "$T/timps-irprobe" ] && ok "single IR: stamp written" || bad "single IR: no stamp"
	sleep 2
	expect "single IR: watchdog relights after delay" ir850=on
	[ "$(grep -c 'ir850 on' "$T/log")" -eq 1 ] && ok "single IR: one watchdog" ||
		bad "single IR: $(grep -c 'ir850 on' "$T/log") relights"

	setup "ir850 ir940"
	WD=30 hook off
	expect "dual IR: off switches both" ir850=off ir940=off
	hook on
	rc=$?
	[ $rc -eq 0 ] || bad "dual IR: on rc=$rc"
	expect "dual IR: on switches both" ir850=on ir940=on
	[ ! -e "$T/timps-irprobe" ] && ok "dual IR: on removes stamp" || bad "dual IR: stamp left"

	setup "ir850 ir940" ir940=false
	WD=30 hook off
	expect "controls.ir940=false: only ir850" ir850=off ir940=on

	setup "ir940 white" white=true
	WD=30 hook off
	expect "white enabled: switched with IR" ir940=off white=off
	hook on
	expect "white enabled: back on" ir940=on white=on

	setup "ir940 white"
	WD=30 hook off
	expect "white default (false): left alone" ir940=off white=on

	setup ""
	hook off
	rc=$?
	[ $rc -eq 1 ] && ok "no pins: off exit 1" || bad "no pins: off rc=$rc"
	hook on
	rc=$?
	[ $rc -eq 1 ] && ok "no pins: on exit 1" || bad "no pins: on rc=$rc"
	[ ! -e "$T/timps-irprobe" ] && ok "no pins: no stamp" || bad "no pins: stamp written"

	setup "ir850 ir940 white" ir850=false ir940=false
	hook off
	rc=$?
	[ $rc -eq 1 ] && ok "all disabled: exit 1" || bad "all disabled: rc=$rc"
	expect "all disabled: untouched" ir850=on ir940=on white=on

	setup "ir850 ir940" white=true
	STUB_FAIL="ir940:off"
	hook off
	rc=$?
	[ $rc -ne 0 ] && ok "failing light: non-zero ($rc)" || bad "failing light: rc=0"
	expect "failing light: rolled back" ir850=on ir940=on
	[ ! -e "$T/timps-irprobe" ] && ok "failing light: stamp removed" || bad "failing light: stamp left"
	STUB_FAIL="ir850:on"
	echo off >"$T/state.ir850"
	echo off >"$T/state.ir940"
	hook on
	rc=$?
	[ $rc -ne 0 ] && ok "failing on: non-zero ($rc)" || bad "failing on: rc=0"
	expect "failing on: others still switched" ir940=on
	STUB_FAIL=

	setup "ir850"
	hook off
	hook on
	echo off >"$T/state.ir850" # marks a later watchdog relight
	sleep 2
	expect "off then on: watchdog cancelled" ir850=off

	setup "ir850"
	WD=2 hook off
	sleep 1
	WD=2 hook off
	sleep 1.5
	expect "newer off: older watchdog void" ir850=off
	sleep 1.2
	expect "newer off: newer watchdog fires" ir850=on

	setup "ir850"
	hook bogus
	rc=$?
	[ $rc -eq 2 ] && ok "unknown verb: exit 2" || bad "unknown verb: rc=$rc"
	expect "unknown verb: untouched" ir850=on
}

for SH in sh "busybox sh"; do
	command -v "${SH%% *}" >/dev/null 2>&1 || {
		echo "  SKIP  $SH not found"
		continue
	}
	if $SH -n "$HOOK"; then ok "syntax"; else bad "syntax"; fi
	run_cases
done
sleep 1 # let stray 1 s watchdogs finish before the temp dir goes
echo "irprobe: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
