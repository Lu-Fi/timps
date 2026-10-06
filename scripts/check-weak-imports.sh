#!/bin/sh
# check-weak-imports.sh - would this timpsd start on that libimp?
#
# usage: check-weak-imports.sh [-a] [-s] <timpsd> <libimp.so> [<lib.so>...]
#
# Lists every STRONG (GLOBAL) undefined dynamic symbol of <timpsd> that none of
# the given libraries defines. timpsd links with BIND_NOW as a PIE, so such a
# symbol is not a load error on uClibc: it resolves to 0 and the first call
# jumps to NULL. Weak imports (OpenIMP extensions, see hal_ingenic.c) are listed
# for information only - the code NULL-checks them.
#
# By default only the libimp namespace (IMP_*, OpenIMP_*, imp_*) is checked, so
# libc/mbedtls imports need not be passed. -a checks every strong import (pass
# the whole NEEDED set then).
#
# -s also fails on strong IMP-namespace undefs of the given libraries
# themselves that neither the binary nor another given library defines. The
# vendor libimp needs IMP_Log_Get_Option/imp_log_fun from libalog.so; an
# OpenIMP-flavour timpsd links only -limp, so on a vendor libimp that gap is a
# link matter (libalog is not in NEEDED), not something the source can weaken.
#
# exit: 0 clean, 1 missing strong imports, 2 usage/tool error.
# READELF overrides the readelf binary (default: ${CROSS_COMPILE}readelf, then
# readelf - any GNU readelf reads MIPS ELF).

all=0 strict=0
while [ $# -gt 0 ]; do
    case $1 in
        -a) all=1 ;;
        -s) strict=1 ;;
        -h|--help) sed -n '2,25p' "$0"; exit 0 ;;
        --) shift; break ;;
        -*) echo "unknown option $1" >&2; exit 2 ;;
        *) break ;;
    esac
    shift
done
[ $# -ge 2 ] || { sed -n '4p' "$0" >&2; exit 2; }
bin=$1; shift

RE=${READELF:-}
if [ -z "$RE" ]; then
    if [ -n "${CROSS_COMPILE:-}" ] && command -v "${CROSS_COMPILE}readelf" >/dev/null 2>&1; then
        RE=${CROSS_COMPILE}readelf
    else
        RE=readelf
    fi
fi
command -v "$RE" >/dev/null 2>&1 || { echo "readelf not found ($RE)" >&2; exit 2; }
for f in "$bin" "$@"; do
    [ -r "$f" ] || { echo "cannot read $f" >&2; exit 2; }
done

export LC_ALL=C
tmp=$(mktemp -d) || exit 2
trap 'rm -rf "$tmp"' EXIT INT TERM

# <bind> <name>, versions stripped
syms() { "$RE" -W --dyn-syms "$1" | awk 'NF>=8 && $1 ~ /:$/ {
    n=$8; sub(/@.*/,"",n); if (n!="") print ($7=="UND" ? "U" : "D"), $5, n }'; }

for l in "$@"; do syms "$l" >> "$tmp/libsyms"; done
[ -s "$tmp/libsyms" ] || { echo "no dynamic symbols in the given libraries" >&2; exit 2; }
awk '$1=="D" && $2!="LOCAL" {print $3}' "$tmp/libsyms" | sort -u > "$tmp/def"
syms "$bin" > "$tmp/binsyms"
awk '$1=="D" && $2!="LOCAL" {print $3}' "$tmp/binsyms" | sort -u > "$tmp/bindef"

ns='^(IMP_|OpenIMP_|imp_)'
pick() { if [ $all = 1 ]; then cat; else grep -E "$ns"; fi; }

awk '$1=="U" && $2=="GLOBAL" {print $3}' "$tmp/binsyms" | sort -u | pick \
    | comm -23 - "$tmp/def" > "$tmp/strong"
awk '$1=="U" && $2=="WEAK" {print $3}' "$tmp/binsyms" | sort -u | grep -E "$ns" \
    | comm -23 - "$tmp/def" > "$tmp/weak"
sort -u "$tmp/def" "$tmp/bindef" > "$tmp/alldef"
awk '$1=="U" && $2=="GLOBAL" {print $3}' "$tmp/libsyms" | sort -u | grep -E "$ns" \
    | comm -23 - "$tmp/alldef" > "$tmp/libgap"

rc=0
echo "== $(basename "$bin") vs$(for l in "$@"; do printf ' %s' "$(basename "$l")"; done)"
if [ -s "$tmp/strong" ]; then
    echo "STRONG imports not defined (NULL call at runtime):"
    sed 's/^/  /' "$tmp/strong"; rc=1
else
    echo "strong imports: all defined"
fi
if [ -s "$tmp/weak" ]; then
    echo "weak imports not defined (feature reports unsupported):"
    sed 's/^/  /' "$tmp/weak"
fi
if [ -s "$tmp/libgap" ]; then
    echo "library-side gaps (a library needs these, nobody given defines them;"
    echo "IMP_Log_Get_Option/imp_log_fun live in the vendor libalog.so):"
    sed 's/^/  /' "$tmp/libgap"
    [ $strict = 1 ] && rc=1
fi
echo "RESULT: $([ $rc = 0 ] && echo clean || echo MISSING)"
exit $rc
