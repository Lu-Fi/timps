#!/usr/bin/env bash
# Deploy timps to the camera and run it live. Requires an installed SSH key.
# The camera IP is REQUIRED via CAM=<ip>.
#
#   CAM=<ip> ./scripts/deploy.sh            # copy last-built binary + camera.conf, run (live log)
#   CAM=<ip> ./scripts/deploy.sh --build    # rebuild in thingino first, then deploy + run
#   CAM=<ip> ./scripts/deploy.sh --no-conf  # run against the camera's own /etc/timps.conf
#                                            # (production config baked in by the firmware
#                                            # build, incl. its own http.port etc.) instead
#                                            # of pushing scripts/camera.conf to /tmp
#
# Env overrides: CAM (camera IP, required), THINGINO (firmware tree),
# MS_OSD_TEST_STATIC=1 (forwarded to the camera: freezes OSD text after the
# first render, for isolating the TTF/bitmap rasterizer's CPU cost from
# IMP_OSD's per-frame hardware compositing cost - see imp_osd.c)
set -euo pipefail

CAM="${CAM:-}"
THINGINO="${THINGINO:-$HOME/thingino-firmware-LuFi}"
SRCDIR="$(cd "$(dirname "$0")/.." && pwd)"
[ -n "$CAM" ] || { echo "!! set the camera IP:  CAM=<ip> $0 [--build]"; exit 1; }

DO_BUILD=0
PUSH_CONF=1
for a in "$@"; do
    case "$a" in
        --build)   DO_BUILD=1 ;;
        --no-conf) PUSH_CONF=0 ;;
        -h|--help) grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $a (see --help)"; exit 1 ;;
    esac
done

# locate the package build dir / detect the camera profile from the output tree
OUTDIR="$(ls -d "$THINGINO"/output/*/*/build 2>/dev/null | head -1 | xargs -r dirname || true)"
[ -n "$OUTDIR" ] || { echo "!! no thingino output dir found under $THINGINO/output"; exit 1; }
PROFILE="$(basename "$OUTDIR" | sed -E 's/-[0-9].*$//')"

if [ "$DO_BUILD" = 1 ]; then
    # Build the libfaac dependency first: 'rebuild-timps' only rebuilds
    # timps itself, not new dependencies. Without libfaac in staging the
    # USE_FAAC path can't link and audio stays G.711. Cheap no-op once built.
    echo ">> ensuring libfaac is built (software AAC) ..."
    make -C "$THINGINO" CAMERA="$PROFILE" IP="$CAM" faac

    echo ">> rebuilding just the timps package (camera: $PROFILE) ..."
    # thingino's per-package target: dirclean + build + reinstall + finalize.
    make -C "$THINGINO" CAMERA="$PROFILE" IP="$CAM" rebuild-timps
fi

# newest timpsd across all output dirs (IP-suffixed or not)
BIN="$(ls -t "$THINGINO"/output/*/*/build/timps-*/timpsd 2>/dev/null | head -1 || true)"
[ -n "$BIN" ] || { echo "!! binary not found - run once with --build"; exit 1; }
echo ">> binary: $BIN"

# stop the running streamer FIRST (raptor is thingino's default now; it also
# frees the ISP/encoder so timps can grab it), and any old timps.
echo ">> stopping raptor + old timps on $CAM ..."
ssh root@"$CAM" '/etc/init.d/S31raptor stop 2>/dev/null; killall -9 timpsd 2>/dev/null; killall -q rwd rhd rwc prudynt 2>/dev/null; sleep 1; true'

echo ">> copying to $CAM:/tmp ..."
scp -O "$BIN" root@"$CAM":/tmp/timpsd

# --no-conf: run against the camera's own production config (baked into the
# firmware image at /etc/timps.conf, see thingino package/timps/timps.mk) so
# a stale dev http.port/etc. in scripts/camera.conf can't diverge from what
# the on-device WebUI actually expects. Default: push our dev config to /tmp
# and run isolated from /etc, as before.
if [ "$PUSH_CONF" = 1 ]; then
    scp -O "$SRCDIR/scripts/camera.conf" root@"$CAM":/tmp/timps.conf
    CONF=/tmp/timps.conf
else
    CONF=/etc/timps.conf
fi

# forward the OSD CPU-test knob if set locally (see comment above)
ENV_PREFIX=""
if [ -n "${MS_OSD_TEST_STATIC:-}" ]; then
    ENV_PREFIX="MS_OSD_TEST_STATIC=$MS_OSD_TEST_STATIC "
    echo ">> MS_OSD_TEST_STATIC=$MS_OSD_TEST_STATIC (OSD text will freeze after the first render)"
fi

echo ">> starting timps against $CONF (Ctrl-C stops it) ..."
ssh -t root@"$CAM" "chmod +x /tmp/timpsd; exec ${ENV_PREFIX}/tmp/timpsd -c $CONF -v"
