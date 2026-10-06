#!/bin/sh
# Host test for the libimp_check.c dlopen/dlsym glue (the HAL_INGENIC half):
# fake vendor/OpenIMP libimp.so files, a harness linked against libimp.so like
# timpsd, both build flavours, daemon and --check-libs mode. Host libc (glibc)
# stands in for uClibc here. Exit 0 = all passed.
#
#   scripts/test_libimp_dl.sh [cc]
CC=${1:-cc}
cd "$(dirname "$0")/.." || exit 2
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

cat >"$T/fake.c" <<'EOF'
#include "hal/libimp_check.h"
#define DEF(f) void f(void) {}
LIBIMP_REQUIRED(DEF)
#ifdef OPENIMP
LIBIMP_OPENIMP_MARKERS(DEF)
#endif
EOF
cat >"$T/main.c" <<'EOF'
#include <stdio.h>
#include <string.h>
#include "hal/libimp_check.h"
#include "log.h"
extern void IMP_System_Init(void);
int main(int argc, char **argv)
{
    void (*volatile keep)(void) = IMP_System_Init;   /* a NEEDED libimp */
    (void)keep;
    log_set_syslog(0);
    return libimp_preflight(argc > 1 && !strcmp(argv[1], "--check-libs"));
}
EOF
fakelib() { # dir cflags
	mkdir -p "$T/$1"
	$CC -shared -fPIC -Isrc $2 "$T/fake.c" -o "$T/$1/libimp.so" || exit 2
}
fakelib vendor ""
fakelib openimp "-DOPENIMP"
# a vendor libimp without IMP_Encoder_GetStream
mkdir -p "$T/nogs"
{
	echo '#define DEF(f) void f(void) {}'
	grep -o 'X([A-Za-z_]*)' src/hal/libimp_check.h | sed -n 's/X(\(IMP_[A-Za-z_]*\))/\1/p' |
		grep -vx IMP_Encoder_GetStream | sed 's/.*/DEF(&)/'
} >"$T/nogs.c"
$CC -shared -fPIC -Isrc "$T/nogs.c" -o "$T/nogs/libimp.so" || exit 2

# harness per flavour; IMP headers are only needed for the sizeof() tie
# and parse as plain C on the host
INC="-Iinclude/T31/1.1.6/en -Iinclude/T31/1.1.6/en/imp"
for fl in vendor openimp; do
	d=""; [ $fl = openimp ] && d="-DUSE_OPENIMP"
	$CC -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -DHAL_INGENIC $d \
		-Isrc $INC "$T/main.c" src/hal/libimp_check.c src/log.c \
		-L"$T/vendor" -limp -ldl -lpthread -o "$T/h-$fl" || exit 2
done
# dlopen fails: the daemon starts, --check-libs says it could not check
$CC -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -DHAL_INGENIC \
	-DLIBIMP_SONAME='"libimp-absent.so"' -Isrc $INC "$T/main.c" src/hal/libimp_check.c \
	src/log.c -L"$T/vendor" -limp -ldl -lpthread -o "$T/h-noopen" || exit 2

pass=0 fail=0
run() { # want_rc want_text build lib mode
	out=$(LD_LIBRARY_PATH="$T/$4" "$T/h-$3" $5 2>&1)
	rc=$?
	if [ "$rc" = "$1" ] && echo "$out" | grep -qF -- "$2"; then
		pass=$((pass + 1)); echo "  PASS  $3 build on $4 lib ${5:-daemon}: rc=$rc"
	else
		fail=$((fail + 1)); echo "  FAIL  $3 build on $4 lib ${5:-daemon}: rc=$rc (want $1)"
		echo "$out" | sed 's/^/        /'
	fi
	[ -n "$VERBOSE" ] && echo "$out" | sed 's/^/        | /'
}
run 0 "libimp OK (vendor build" vendor vendor --check-libs
run 0 "libimp OK (OpenIMP build" openimp openimp --check-libs
run 78 "built for OpenIMP (USE_OPENIMP=1) but" openimp vendor --check-libs
run 78 "built for the Ingenic vendor libimp (USE_OPENIMP=0) but" vendor openimp --check-libs
run 79 "libimp lacks IMP_Encoder_GetStream" vendor nogs --check-libs
run 0 "" vendor vendor ""
run 0 "" openimp openimp ""
run 78 "not starting: the libimp does not fit this build (exit 78)" openimp vendor ""
run 78 "is OpenIMP - install the OpenIMP-flavour timpsd" vendor openimp ""
run 79 "not starting: the libimp does not fit this build (exit 79)" vendor nogs ""
run 80 "libimp check skipped" noopen vendor --check-libs
run 0 "libimp check skipped" noopen vendor ""
# the daemon mode logs (syslog + stderr), --check-libs prints only
out=$(LD_LIBRARY_PATH="$T/vendor" "$T/h-openimp" --check-libs 2>&1)
case $out in
*"[LIBIMP]"*) fail=$((fail + 1)); echo "  FAIL  --check-libs went through the logger" ;;
*) pass=$((pass + 1)); echo "  PASS  --check-libs output is plain" ;;
esac
echo "libimp dl glue: $pass passed, $fail failed"
[ $fail = 0 ]
