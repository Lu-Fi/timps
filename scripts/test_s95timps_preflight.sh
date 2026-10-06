#!/bin/sh
# Host test for the firmware init script's --check-libs preflight.
# The script lives in thingino-firmware: package/timps/files/S95timps.
#
#   scripts/test_s95timps_preflight.sh <path/to/S95timps>
#
# A copy of S95timps gets DAEMON/PIDFILE/CONF pointed at a temp dir and its
# start-stop-daemon/logger calls renamed to stubs on PATH (busybox sh would
# run its own applets otherwise); timpsd is a compiled stub
# (a real binary, NUL bytes included). Runs under `sh` and, if present,
# `busybox sh` with busybox tr/grep/head. Exit 0 = all passed.
S95=${1:-$S95TIMPS}
[ -f "$S95" ] || {
	echo "usage: $0 <path/to/S95timps>" >&2
	exit 2
}
CC=${CC:-cc}
TOP=$(mktemp -d)
trap 'rm -rf "$TOP"' EXIT
mkdir "$TOP/bin"

# stub timpsd: --check-libs prints $STUB_OUT to stderr and exits $STUB_RC;
# anything else counts as a daemon start. -DOLD: no "--check-libs" string.
cat >"$TOP/stub.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv)
{
    FILE *f = fopen(getenv("STUB_LOG"), "a");
#ifdef OLD
    fprintf(f, "timpsd ran:");
    for (int i = 1; i < argc; i++) fprintf(f, " %s", argv[i]);
    fprintf(f, "\n");
    return 0;
#else
    if (argc > 1 && !strcmp(argv[1], "--check-libs")) {
        fprintf(f, "timpsd check-libs\n");
        const char *o = getenv("STUB_OUT");
        if (o && *o) fprintf(stderr, "%s\n", o);
        return atoi(getenv("STUB_RC"));
    }
    fprintf(f, "timpsd ran daemon\n");
    return 0;
#endif
}
EOF
$CC "$TOP/stub.c" -o "$TOP/timpsd-new" || exit 2
$CC -DOLD "$TOP/stub.c" -o "$TOP/timpsd-old" || exit 2

cat >"$TOP/bin/stub-ssd" <<'EOF'
#!/bin/sh
echo "ssd $*" >>"$STUB_LOG"
EOF
cat >"$TOP/bin/stub-logger" <<'EOF'
#!/bin/sh
echo "logger $*" >>"$STUB_LOG"
EOF
chmod +x "$TOP/bin/stub-ssd" "$TOP/bin/stub-logger"

pass=0 fail=0 n=0
ok() { pass=$((pass + 1)); echo "  PASS  $SH: $*"; }
bad() { fail=$((fail + 1)); echo "  FAIL  $SH: $*"; sed 's/^/        /' "$T/log" "$T/out" 2>/dev/null; }

# start <stub> <rc> <stderr text>: runs "S95timps start", sets $src
start() {
	n=$((n + 1))
	T="$TOP/t$n"
	mkdir "$T"
	cp "$TOP/timpsd-$1" "$T/timpsd"
	: >"$T/conf"
	sed -e "s|^DAEMON=.*|DAEMON=$T/timpsd|" -e "s|^PIDFILE=.*|PIDFILE=$T/pid|" \
		-e "s|^CONF=.*|CONF=$T/conf|" -e "s|start-stop-daemon |stub-ssd |g" \
		-e "s|logger |stub-logger |g" "$S95" >"$T/S95timps"
	: >"$T/log"
	STUB_LOG="$T/log" STUB_RC=$2 STUB_OUT=$3 PATH="$P" $SH "$T/S95timps" start >"$T/out" 2>&1
	src=$?
}
has() { grep -qF -- "$1" "$T/${2:-log}"; }

run_cases() {
	start new 0 ""
	if [ $src = 0 ] && has "timpsd check-libs" && has "ssd -S" && ! has "logger"; then
		ok "ok -> started"
	else bad "ok -> started"; fi

	m1="timpsd: libimp mismatch: this timpsd is built for OpenIMP (USE_OPENIMP=1) but /usr/lib/libimp.so is the Ingenic vendor libimp - install the vendor-flavour timpsd (USE_OPENIMP=0) or the OpenIMP libimp"
	start new 78 "$m1"
	if [ $src != 0 ] && ! has "ssd -S" && has "logger -t timps -p daemon.err $m1" &&
		has "timpsd not started (exit 78)" && has "$m1" out &&
		has "timpsd not started (exit 78)" out; then
		ok "78 -> not started, syslog + console"
	else bad "78 -> not started, syslog + console"; fi

	m2="timpsd: libimp lacks IMP_Encoder_GetStream - /usr/lib/libimp.so does not fit this timpsd"
	start new 79 "$m2"
	if [ $src != 0 ] && ! has "ssd -S" && has "logger -t timps -p daemon.err $m2" &&
		has "timpsd not started (exit 79)"; then
		ok "79 -> not started"
	else bad "79 -> not started"; fi

	start old 0 ""
	if [ $src = 0 ] && ! has "check-libs" && ! has "timpsd ran" && has "ssd -S"; then
		ok "older timpsd: never asked, started as before"
	else bad "older timpsd: never asked, started as before"; fi

	start new 2 "timps: invalid argument '--check-libs'"
	if [ $src = 0 ] && has "ssd -S" && has "daemon.warn timpsd --check-libs exited 2"; then
		ok "unknown rc 2 -> started, warning"
	else bad "unknown rc 2 -> started, warning"; fi

	start new 139 "can't resolve symbol 'IMP_Log_Get_Option'"
	if [ $src = 0 ] && has "ssd -S" && has "exited 139, starting anyway: can't resolve symbol"; then
		ok "crash/loader error -> started, warning"
	else bad "crash/loader error -> started, warning"; fi

	m3="timpsd: warning: dlopen(libimp.so): libimp.so: cannot open shared object file - libimp check skipped"
	start new 80 "$m3"
	if [ $src = 0 ] && has "ssd -S" && has "daemon.warn timpsd --check-libs exited 80, starting anyway: $m3"; then
		ok "80 (could not check) -> started, warning"
	else bad "80 (could not check) -> started, warning"; fi

	start new 1 ""
	if [ $src = 0 ] && has "ssd -S" && ! has "logger"; then
		ok "rc 1 without output -> started silently"
	else bad "rc 1 without output -> started silently"; fi

	# no daemon binary at all: nothing to ask, start as before
	start new 0 ""
	rm -f "$T/timpsd"; : >"$T/log"
	STUB_LOG="$T/log" PATH="$P" $SH "$T/S95timps" start >"$T/out" 2>&1
	if has "ssd -S" && ! has "check-libs"; then
		ok "missing binary -> start-stop-daemon as before"
	else bad "missing binary -> start-stop-daemon as before"; fi
}

SH=sh P="$TOP/bin:$PATH"
run_cases
if command -v busybox >/dev/null 2>&1; then
	mkdir "$TOP/bb"
	for a in tr grep head sed cat mkdir dirname rm; do ln -s "$(command -v busybox)" "$TOP/bb/$a"; done
	SH="busybox sh" P="$TOP/bin:$TOP/bb:$PATH"
	run_cases
fi
echo "S95timps preflight: $pass passed, $fail failed"
[ $fail = 0 ]
