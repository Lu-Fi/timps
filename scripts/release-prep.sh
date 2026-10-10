#!/bin/sh
# Release prep: CHANGELOG heading + links, doc version headers, and the
# "since the next release" markers in docs/ and README.md.
#
#   scripts/release-prep.sh [--check] [VERSION [DATE]]
#   VERSION=1.10.0 DATE=2026-10-11 scripts/release-prep.sh
#
# Idempotent. If the newest CHANGELOG release is untagged and differs from
# VERSION, the earlier prep is retargeted (heading, links, "since vOLD").
# --check changes nothing and exits 1 if anything is left.
set -eu

CHECK=0
[ "${1:-}" = "--check" ] && { CHECK=1; shift; }
VERSION=${1:-${VERSION:-1.10.0}}
DATE=${2:-${DATE:-2026-10-11}}
VERSION=${VERSION#v}

cd "$(dirname "$0")/.."
CL=CHANGELOG.md
REPO_URL=https://github.com/Lu-Fi/timps

prev=$(sed -n 's/^## \[\([0-9][0-9A-Za-z.+-]*\)\] - .*/\1/p' "$CL" | head -n1)
tagged() { git rev-parse -q --verify "refs/tags/v$1" >/dev/null 2>&1; }

if [ "$prev" = "$VERSION" ]; then
	mode=same
elif tagged "$prev"; then
	mode=new
else
	mode=retarget
fi
OLD=$prev

DOCS=$(git ls-files docs README.md | grep -E '\.md$')

if [ "$CHECK" = 1 ]; then
	bad=0
	say() { echo "LEFT: $*"; bad=1; }
	[ "$mode" = new ] && say "$CL: no [$VERSION] heading yet"
	[ "$mode" = retarget ] && say "$CL: prepared for $OLD, not $VERSION (untagged)"
	grep -q "^## \[$VERSION\] - $DATE\$" "$CL" || say "$CL: heading is not '## [$VERSION] - $DATE'"
	grep -q '^## \[Unreleased\]' "$CL" || say "$CL: no [Unreleased] heading"
	grep -q "^\[$VERSION\]: " "$CL" || say "$CL: no [$VERSION] compare link"
	grep -q "Applies to timps v$VERSION (source: \`main\`, $DATE)" docs/ai/config-keys.md ||
		say "docs/ai/config-keys.md: header"
	grep -q "Applies to timps v$VERSION (source: \`main\`, $DATE)" docs/ai/reference.md ||
		say "docs/ai/reference.md: header"
	grep -q "Version v$VERSION ($DATE)" docs/ai/feature-list-de.md ||
		say "docs/ai/feature-list-de.md: header"
	# shellcheck disable=SC2086
	left=$(perl -CSD -Mutf8 -0ne 'while (/(?:the\s+next\s+(?:release|version)|dem\s+n\x{e4}chsten\s+Release|`\[Unreleased\]`\s+in\s+`CHANGELOG)/gi) { my $l = 1 + (substr($_, 0, $-[0]) =~ tr/\n//); print "$ARGV:$l\n" }' $DOCS)
	[ -z "$left" ] || say "markers:
$left"
	src=$(git ls-files src | xargs grep -nliE 'next (release|version)' || true)
	[ -z "$src" ] || echo "NOTE (src, not changed by this script): $src"
	tagged "$VERSION" && echo "NOTE: tag v$VERSION already exists"
	[ "$bad" = 0 ] && echo "release-prep: nothing left for v$VERSION ($DATE)"
	exit "$bad"
fi

# CHANGELOG
case $mode in
new)
	awk '/^## \[Unreleased\]/{n++} END{exit n==1?0:1}' "$CL" || { echo "$CL: need exactly one [Unreleased]" >&2; exit 1; }
	V="$VERSION" D="$DATE" perl -0pi -e 's/^## \[Unreleased\]\n/## [Unreleased]\n\n## [$ENV{V}] - $ENV{D}\n/m' "$CL"
	;;
retarget)
	O="$OLD" V="$VERSION" D="$DATE" perl -0pi -e 's/^## \[\Q$ENV{O}\E\] - \S+$/## [$ENV{V}] - $ENV{D}/m; s/^\[\Q$ENV{O}\E\]: .*\n//m' "$CL"
	;;
same)
	V="$VERSION" D="$DATE" perl -0pi -e 's/^## \[\Q$ENV{V}\E\] - \S+$/## [$ENV{V}] - $ENV{D}/m' "$CL"
	;;
esac
# compare links: [Unreleased] and [VERSION] above the first version link
base=$(sed -n 's/^## \[\([0-9][0-9A-Za-z.+-]*\)\] - .*/\1/p' "$CL" | sed -n 2p)
V="$VERSION" B="$base" U="$REPO_URL" perl -0pi -e '
	s/^\[Unreleased\]: .*\n//mg;
	s/^\[\Q$ENV{V}\E\]: .*\n//mg;
	s/^(?=\[\d)/[Unreleased]: $ENV{U}\/compare\/v$ENV{V}...HEAD\n[$ENV{V}]: $ENV{U}\/compare\/v$ENV{B}...v$ENV{V}\n/m;' "$CL"

# doc headers
V="$VERSION" D="$DATE" perl -pi -e 's/Applies to timps v[0-9][0-9A-Za-z.+-]* \(source: `main`, [0-9-]+\)/Applies to timps v$ENV{V} (source: `main`, $ENV{D})/' \
	docs/ai/config-keys.md docs/ai/reference.md
V="$VERSION" D="$DATE" perl -pi -e 's/Version v[0-9][0-9A-Za-z.+-]* \([0-9-]+\);/Version v$ENV{V} ($ENV{D});/' \
	docs/ai/feature-list-de.md

# markers; a line break inside the phrase is kept in front of the version
# shellcheck disable=SC2086
V="$VERSION" O="$OLD" M="$mode" perl -CSD -Mutf8 -0pi -e '
	sub ver { my $lead = shift; if (defined $lead) { for (@_) { return "$1v$ENV{V}" if /(\n[ \t>]*)\z/ } } ($lead // "") eq "" ? "v$ENV{V}" : " v$ENV{V}" }
	s/(Changes listed under\s+)`\[(?:Unreleased|\Q$ENV{O}\E)\]`(\s+in\s+`CHANGELOG\.md`)/$1`[$ENV{V}]`$2/g;
	s/(?:(?<=\S)([ \t]+))?\bthe(\s+)next(\s+)(?:release|version)\b/ver($1,$2,$3)/gie;
	s/(?:(?<=\S)([ \t]+))?\bdem(\s+)n\x{e4}chsten(\s+)Release\b/ver($1,$2,$3)/ge;
	if ($ENV{M} eq "retarget") { s/\bv\Q$ENV{O}\E(?![\d.]*\d)/v$ENV{V}/g }' $DOCS

echo "release-prep: $mode -> v$VERSION ($DATE)"
