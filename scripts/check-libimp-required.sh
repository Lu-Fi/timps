#!/bin/sh
# check-libimp-required.sh - would timpsd's libimp start check refuse this lib?
#
# usage: check-libimp-required.sh [-H] [--expect vendor|openimp] <libimp.so>...
#
# Reads LIBIMP_REQUIRED / LIBIMP_OPENIMP_MARKERS from src/hal/libimp_check.h
# and, per library, lists required names it does not export and the flavour
# the markers say. A supported libimp that fails here would be refused at
# start (exit 79), so a new libimp version must pass before it ships.
# --expect fails when the detected flavour differs (exit 78 at start).
# -H also checks that every required name is declared in each IMP header set
# the Makefile picks per PLATFORM.
#
# exit: 0 clean, 1 a library fails, 2 usage/tool error.
# READELF overrides the readelf binary (any GNU readelf reads MIPS ELF).
top=$(cd "$(dirname "$0")/.." && pwd)
hdr=$top/src/hal/libimp_check.h
expect= headers=0
while [ $# -gt 0 ]; do
	case $1 in
	-H) headers=1 ;;
	--expect) expect=$2; shift ;;
	-h | --help) sed -n '2,16p' "$0"; exit 0 ;;
	-*) echo "unknown option $1" >&2; exit 2 ;;
	*) break ;;
	esac
	shift
done
[ $# -ge 1 ] || [ $headers = 1 ] || { sed -n '4p' "$0" >&2; exit 2; }
case $expect in '' | vendor | openimp) ;; *) echo "--expect vendor|openimp" >&2; exit 2 ;; esac
RE=${READELF:-readelf}
command -v "$RE" >/dev/null 2>&1 || { echo "readelf not found ($RE)" >&2; exit 2; }
export LC_ALL=C

# X(name) entries of one macro body (continuation lines up to the first blank)
names() {
	sed -n "/#define $1(X)/,/^\$/p" "$hdr" | grep -o 'X([A-Za-z0-9_]*)' | sed 's/X(\(.*\))/\1/'
}
req=$(names LIBIMP_REQUIRED)
mark=$(names LIBIMP_OPENIMP_MARKERS)
[ -n "$req" ] && [ -n "$mark" ] || { echo "cannot parse $hdr" >&2; exit 2; }

rc=0
if [ $headers = 1 ]; then
	for d in T10:T20/3.12.0/zh T20:T20/3.12.0/zh T21:T21/1.0.33/zh T23:T23/1.3.0/en \
		T30:T30/1.0.5/zh T31:T31/1.1.6/en T40:T40/1.3.1/en T41:T41/1.2.6/en C100:C100/2.1.0/en; do
		soc=${d%%:*} dir=$top/include/${d#*:}/imp
		miss=
		for n in $req; do
			grep -qw "$n" "$dir"/*.h 2>/dev/null || miss="$miss $n"
		done
		if [ -n "$miss" ]; then
			echo "headers $soc: not declared:$miss"; rc=1
		else
			echo "headers $soc: all $(echo $req | wc -w) declared"
		fi
	done
fi

for lib; do
	[ -r "$lib" ] || { echo "cannot read $lib" >&2; exit 2; }
	def=$("$RE" -W --dyn-syms "$lib" | awk 'NF>=8 && $1 ~ /:$/ && $7!="UND" {n=$8; sub(/@.*/,"",n); print n}' | sort -u)
	miss=
	for n in $req; do
		echo "$def" | grep -qx "$n" || miss="$miss $n"
	done
	fl=vendor
	for n in $mark; do
		echo "$def" | grep -qx "$n" && { fl=openimp; break; }
	done
	st=ok
	[ -n "$miss" ] && { st="MISSING:$miss"; rc=1; }
	[ -n "$expect" ] && [ "$fl" != "$expect" ] && { st="$st FLAVOUR(expected $expect)"; rc=1; }
	echo "$fl $st $lib"
done
exit $rc
