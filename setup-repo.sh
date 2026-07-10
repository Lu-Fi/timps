#!/bin/sh
# setup-repo.sh - prepare this working tree for the first push to the existing
# GitHub repo https://github.com/Lu-Fi/timps ("timps" - Tiny IMP Streamer).
# The Ingenic headers come from the gtxaspec/ingenic-headers SUBMODULE (same
# pattern as prudynt-t uses in thingino).
#
# !!! RUN THIS YOURSELF ON YOUR HOST !!!
# It needs network access to github.com (git submodule add clones the header
# repo; git push needs your GitHub login) and it rewrites what is tracked by
# git. Nothing here is done automatically by the build; review the commands
# before running.
#
# What it does:
#   1. git init (if this is not a git repo yet)
#   2. removes the vendored include/ tree from index + working copy
#      (the same content comes back as a submodule checkout)
#   3. registers https://github.com/gtxaspec/ingenic-headers as submodule
#      at path "include" (matches .gitmodules; the Makefile's IMP_INC default
#      ./include/<SoC>/<ver>/<lang> works unchanged, e.g. include/T31/1.1.6/en)
#   4. commits everything and pushes branch "main" to Lu-Fi/timps
#
# Afterwards:
#   - pin package/timps/timps.mk in the thingino fork to the pushed commit
#     (TIMPS_VERSION = <commit hash> instead of "main")
#   - consumers clone with:
#       git clone --recurse-submodules https://github.com/Lu-Fi/timps

set -eu
cd "$(dirname "$0")"

# 1) make sure we are a git repo
if [ ! -d .git ]; then
    git init
fi

# 2) drop the vendored headers from the working tree AND from the index.
#    (Until you run this script the headers stay vendored so the local
#    standalone build keeps working.)
git rm -r --cached include 2>/dev/null || true
rm -rf include

# 3) add the header submodule at the same path (NETWORK: clones from GitHub).
#    --force reuses a leftover .git/modules/include from an earlier attempt
#    (safe here: same gtxaspec/ingenic-headers remote).
git submodule add --force https://github.com/gtxaspec/ingenic-headers include

# 4) stage and commit the lean tree (.gitignore keeps build artifacts out)
git add -A
git commit -m "timps: initial import (Tiny IMP Streamer), Ingenic headers via ingenic-headers submodule"

# 5) push to the EXISTING GitHub repo (NETWORK: needs your GitHub login)
git branch -M main
git remote add origin https://github.com/Lu-Fi/timps.git 2>/dev/null \
    || git remote set-url origin https://github.com/Lu-Fi/timps.git
git push -u origin main

echo
echo "Done. Next step: pin the thingino fork to this commit:"
echo "  thingino-firmware-LuFi/package/timps/timps.mk"
echo "    TIMPS_VERSION = $(git rev-parse HEAD)"
