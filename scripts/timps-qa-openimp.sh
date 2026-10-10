# shellcheck shell=bash
# shellcheck disable=SC2154  # helpers and settings come from timps-qa.sh
# =============================================================================
# timps-qa-openimp.sh - section "openimp" of timps-qa.sh (sourced, never run
# directly). Opt-in only: `--profile openimp` or `--only openimp`.
#
#   scripts/timps-qa.sh --cam <ip> --profile openimp [--ssh root@<ip>]
#       [--include-experimental] [--allow-restart] [--allow-prod]
#
# What it tests, per caps tier (GET /control caps.roi / caps.fcrop / caps.image):
#   effective     real tests, a missing effect is FAIL
#   experimental  skipped unless --include-experimental; then a missing effect
#                 is INFO, not FAIL
#   unsupported   the POST must be refused (422 not_supported_on_soc or listed
#                 in "unsupported") and the stream must be unaffected
#   sim           (host sim) settings round-trips only, image tests SKIP
#   missing caps  (older timps) SKIP
#
# Tests and thresholds (image metrics in scripts/qa/openimp_vision.py):
#   OI-1  caps report: tiers, framesource keepalive vs caps.daynight.ir_idle,
#         daynight.ir_idle status object, rotation list.
#   OI-2  FrontCrop, per enabled stream (snapshot ?chn=S, else an RTSP frame):
#         crop off -> before; centered 50 % window and top-left 50 % window ->
#         after. PASS when the stream size is unchanged, NCC(after, the
#         matching sub-window of before) >= 0.85 AND it beats NCC(after, whole
#         before) and NCC(after, the other window) by >= 0.10. Streams with
#         caps.fcrop.zoomable[S]=0 (T20 main) must keep the full picture
#         (NCC(after, before) >= 0.85). After crop off the full picture must be
#         back. caps.fcrop.state must read on / off (waiting = WARN, the
#         picture check is then SKIPped). An RTSP
#         consumer is held on every zoomable stream meanwhile: T10/T20/T21
#         refuse a window while that stream is idle. caps.fcrop.live=0 (T23
#         driver without fcrop_upscale_pct): SKIP (start-only); with --allow-restart a persist round-trip only.
#   OI-3  Encoder ROI, per stream with caps.roi.streams[S]="ok": the aligned
#         (caps.roi.align) box of ~1/3 x 1/3 with the most texture in an OFF
#         I-frame. Median Laplacian energy over 3 I-frames OFF vs ON, inside
#         divided by outside (rel). +delta (min(20, qp_delta_eff max)):
#         rel <= 1 - 0.15*delta/20 = blurred. -delta (max(-15, qp_delta_eff
#         min)): rel >= 1.05 = sharpened; no measurable sharpening is only
#         WARN (a stream already near min_qp has nothing to gain; not under
#         fixqp), as is no effect at |delta| < 16 (T21) unless rc_mode is
#         fixqp/vbr, or weak blur under CBR. The wrong direction is FAIL at any
#         delta. A FAIL/no-effect is measured once more on a fresh RTSP
#         connection (new IDR); the second decides. The GET echo must
#         equal the POST and caps.roi.rejected[S] must not grow.
#   OI-4  OpenIMP-only image keys (colorfx scene dpc_strength defog_strength
#         drc_strength sinter_strength temper_strength ae_compensation): in
#         caps.image -> POST/GET/restore round-trip; not in caps -> one POST
#         must answer 422 not_supported_on_soc. Effects:
#           brightness 64 -> 192 -> 64 -> 192: mean luma must rise by >= 8
#             (WARN if not, FAIL if it falls; SKIP when the two 64 or the two
#             192 readings or the daynight state differ: the scene changed)
#           colorfx 1 (B/W): saturation <= 35 % of colorfx 0 (FAIL if not;
#             SKIP when the picture has no colour, e.g. night)
#           ae_compensation 40 -> 230 -> 40 -> 230: as brightness, but each
#             value waits 8 s (AE dead time) and settles up to 40 s
#           sinter/temper 0 vs 255: temporal noise numbers, INFO only
#           scene/dpc/defog/drc: round-trip only (no robust metric)
#   OI-5  rotation (restart-only, never changed here): when videoS.rotation
#         is 90/270, eff_width/eff_height swapped vs width/height (or the
#         refusal reported) and ffprobe of the stream equals eff dims.
#   OI-6  with --ssh: `timpsd --check-libs` (libimp flavour), daynight AE
#         reserve (hr=) lines in logread - INFO.
#
# Safety: the image keys, fcrop_* and the ROI slot used are saved from
# GET /control first and restored (POST + read-back compare) at the end and
# from the EXIT/INT/TERM traps. Nothing restart-only is changed; the script
# never restarts timpsd. Targets listed in QA_PROD_CAMS (space/comma list) or
# whose hostname (via --ssh) matches QA_PROD_RE (default "prod") are refused
# without --allow-prod. Needs a static, lit, textured scene; AE is given up to
# OI_SETTLE_MAX seconds (default 8) to settle after each change
# (ae_compensation: 8 s wait + up to 40 s).
# OI_SIM_IMAGES=1 runs the image checks against the host sim too (plumbing
# only: the sim neither crops nor encodes, its verdicts are INFO lines).
# =============================================================================

OI_DIR="$OUTDIR/openimp"; mkdir -p "$OI_DIR"
OI_VIS="$(dirname "${BASH_SOURCE[0]}")/qa/openimp_vision.py"
OI_SETTLE_MAX="${OI_SETTLE_MAX:-8}"
OI_P=0; OI_W=0; OI_F=0; OI_S=0; OI_I=0
oi_ok()   { OI_P=$((OI_P+1)); ok "$@"; }
oi_warn() { OI_W=$((OI_W+1)); warn "$@"; }
oi_bad()  { OI_F=$((OI_F+1)); bad "$@"; }
oi_skip() { OI_S=$((OI_S+1)); skip "$@"; }
oi_info() { OI_I=$((OI_I+1)); printf '  [INFO] %s\n' "$*" | tee -a "$SUMMARY"; }
# the sim neither crops nor encodes: its image verdicts are plumbing only
oi_soft() { if [ "${OI_SIM:-0}" = 1 ]; then shift; oi_info "(sim) $*"; else "$@"; fi; }
oi_flag() { case " ${OI_FLAGS:-} " in *" $1 "*) return 0;; esac; return 1; }

oi_post() { curl -s -o "$2" -w '%{http_code}' --max-time 12 -u "$HTTP_USER:$HTTP_PASS" \
	-X POST "$(http_base)/control" -d "$1"; }
oi_get()  { curlq 12 "$(http_base)/control" -o "$1" && [ -s "$1" ]; }
oi_vis()  { python3 "$OI_VIS" "$@" 2>>"$OI_DIR/vision.err"; }
oi_kv()   { printf '%s\n' "$1" | tr ' ' '\n' | sed -n "s/^$2=//p" | tr -d '"' | head -1; }
oi_ints() { local v; for v in "$@"; do case "$v" in ''|*[!0-9]*) return 1;; esac; done; }
oi_reason() { printf '%s\n' "$1" | sed -n 's/.*reason="\{0,1\}\([^"]*\)"\{0,1\}$/\1/p'; }
# 422 not_supported_on_soc, or 200 with every posted key in "unsupported"
oi_refused() { case "$1" in 422) grep -q 'not_supported_on_soc' "$2";; 200) grep -q '"unsupported":\["' "$2";; *) false;; esac; }

# grade <tier> <verdict> <msg>: effective FAILs, experimental only informs
oi_grade() {
	case "$2" in
		PASS) oi_ok "$3";;
		INCONCLUSIVE) oi_skip "$3";;
		ERROR) oi_warn "$3";;
		*) case "$1" in experimental|sim) oi_info "($1 tier) $3";; *) oi_bad "$3";; esac;;
	esac
}

# one picture of stream $1 (or the default JPEG channel for "-") into $2
oi_grab() {
	local s="$1" f="$2" q="" code
	[ "$s" = "-" ] || q="?chn=$s"
	code=$(curl -s -o "$f" -w '%{http_code} %{content_type}' --max-time 10 \
		-u "$HTTP_USER:$HTTP_PASS" "$(http_base)/snapshot.jpg$q")
	case "$code" in "200 image/jpeg"*) return 0;; esac
	[ "$s" = "-" ] && return 1
	local p; p=$(jget "$OI_BASE" "video.$s.rtsp_path"); p="${p#/}"
	[ -n "$p" ] || return 1
	timeout 20 ffmpeg -v error -y -rtsp_transport tcp -i "$(rtsp_url "$p")" -an \
		-frames:v 1 -update 1 "${f%.*}.ppm" </dev/null >/dev/null 2>&1 \
		&& mv -f "${f%.*}.ppm" "$f"
}

# grab until two pictures 1 s apart are stable (AE settled); leaves the path in OI_LAST
oi_settle() {
	local s="$1" tag="$2" a b t=0 r
	a="$OI_DIR/${tag}_a.jpg"; b="$OI_DIR/${tag}_b.jpg"
	oi_grab "$s" "$a" || { OI_LAST=""; return 1; }
	while [ "$t" -lt "$OI_SETTLE_MAX" ]; do
		sleep 1; t=$((t+1))
		oi_grab "$s" "$b" || break
		r=$(oi_vis stable "$a" "$b")
		[ "$(oi_kv "$r" stable)" = 1 ] && { OI_LAST="$b"; return 0; }
		mv -f "$b" "$a"
	done
	OI_LAST="$a"
	info "  ($tag: picture not stable after ${OI_SETTLE_MAX}s - using the last grab)"
	return 0
}

# an RTSP consumer keeps stream $1's framesource enabled: T10/T20/T21 refuse a
# FrontCrop window while the stream that would show it is idle
OI_HOLD=()
oi_hold_start() {
	local p; p=$(jget "$OI_BASE" "video.$1.rtsp_path"); p="${p#/}"; [ -n "$p" ] || return 1
	timeout 900 ffmpeg -v quiet -rtsp_transport tcp -i "$(rtsp_url "$p")" -an -c copy -f null - \
		</dev/null >/dev/null 2>&1 &
	OI_HOLD+=("$!")
}
oi_hold_stop() {
	local p
	for p in ${OI_HOLD[@]+"${OI_HOLD[@]}"}; do kill "$p" 2>/dev/null; wait "$p" 2>/dev/null; done
	OI_HOLD=()
}

# the daynight boot probe (IR off for seconds, day/night flips) runs during the
# first ~45 s after a start and would fake or hide the ISP key effects
oi_dn_boot_wait() {
	local age wait_s="${OI_DN_BOOT_WAIT:-75}"
	[ -n "${SSH_TARGET:-}" ] || return 0
	[ "$(jget "$OI_BASE" daynight.enabled)" = 1 ] && [ "$(jget "$OI_BASE" daynight.boot_probe)" = 1 ] || return 0
	age=$(sshx 'p=$(pidof timpsd | cut -d" " -f1); [ -n "$p" ] && s=$(cut -d" " -f22 /proc/$p/stat) && u=$(cut -d. -f1 /proc/uptime) && echo $((u - s / 100))' 2>/dev/null | tr -d '\r')
	case "$age" in ''|*[!0-9]*) return 0;; esac
	[ "$age" -lt "$wait_s" ] || return 0
	info "timpsd up ${age}s: waiting $((wait_s - age))s for the daynight boot probe"
	sleep $((wait_s - age))
}

# --- save / restore ----------------------------------------------------------
OI_SAVE_BODY=""; OI_SAVE_PATHS=(); OI_SAVE_VALS=()
oi_save() {   # <get-path> <body-key> <var to append "key":value to>
	local v cur; v=$(jget "$OI_BASE" "$1"); [ -n "$v" ] || return 0
	OI_SAVE_PATHS+=("$1"); OI_SAVE_VALS+=("$v")
	cur="${!3}"; printf -v "$3" '%s%s"%s":%s' "$cur" "${cur:+,}" "$2" "$v"
}
oi_restore() {
	[ -n "${OI_SAVE_BODY:-}" ] || return 0
	local body="$OI_SAVE_BODY" code rf="$OI_DIR/restore.json" i got bad_keys=""
	OI_SAVE_BODY=""
	code=$(oi_post "$body" "$OI_DIR/restore_post.json")
	if ! oi_get "$rf"; then oi_warn "restore: POST HTTP $code but GET /control failed - check the camera ($body)"; return 1; fi
	for i in "${!OI_SAVE_PATHS[@]}"; do
		got=$(jget "$rf" "${OI_SAVE_PATHS[i]}")
		[ "$got" = "${OI_SAVE_VALS[i]}" ] || bad_keys="$bad_keys ${OI_SAVE_PATHS[i]}($got!=${OI_SAVE_VALS[i]})"
	done
	if [ -z "$bad_keys" ]; then
		oi_ok "restore: ${#OI_SAVE_PATHS[@]} saved setting(s) read back equal to before (HTTP $code)"
	else
		oi_bad "restore: values differ after the restore POST (HTTP $code):$bad_keys"
	fi
}

# =============================================================================
hdr "OI. OpenIMP extensions: FrontCrop, encoder ROI, ISP keys (opt-in)"
OI_BASE="$OI_DIR/base.json"
oi_go=1
oi_prod=""
for t in $(printf '%s' "${QA_PROD_CAMS:-}" | tr ',' ' '); do [ "$t" = "$CAM" ] && oi_prod="listed in QA_PROD_CAMS"; done
if [ -z "$oi_prod" ] && [ -n "${SSH_TARGET:-}" ]; then
	oi_host=$(sshx hostname 2>/dev/null | tr -d '\r')
	[ -n "$oi_host" ] && info "camera hostname: $oi_host"
	printf '%s' "$oi_host" | grep -qiE "${QA_PROD_RE:-prod}" && oi_prod="hostname '$oi_host' matches '${QA_PROD_RE:-prod}'"
fi
if [ -n "$oi_prod" ] && ! oi_flag --allow-prod; then
	oi_skip "target $CAM is production-labelled ($oi_prod) - the section zooms/blurs the live picture; rerun with --allow-prod"
	oi_go=0
fi
if [ "$oi_go" = 1 ] && ! oi_get "$OI_BASE"; then
	oi_bad "cannot GET /control - OpenIMP section aborted"; oi_go=0
fi
OI_VISION=1
OI_VFR=-fps_mode   # ffmpeg < 5.1 only knows -vsync
ffmpeg -v quiet -f lavfi -i nullsrc=d=0.1 -fps_mode vfr -f null - </dev/null >/dev/null 2>&1 || OI_VFR=-vsync
if [ "$oi_go" = 1 ]; then
	if ! have python3 || [ ! -f "$OI_VIS" ]; then
		OI_VISION=0; info "python3 or $OI_VIS missing - image checks will SKIP"
	fi
fi

if [ "$oi_go" = 1 ]; then
# ---------------------------------------------------------------- OI-1 caps
OI_ROI_TIER=$(jget "$OI_BASE" caps.roi.tier)
OI_FC_TIER=$(jget "$OI_BASE" caps.fcrop.tier)
OI_FC_LIVE=$(jget "$OI_BASE" caps.fcrop.live)
OI_CAPS_IMG=$(jget "$OI_BASE" caps.image)
OI_SIM=0
[ "$OI_ROI_TIER" = sim ] || [ "$OI_FC_TIER" = sim ] && OI_SIM=1
[ "$OI_SIM" = 1 ] && [ "${OI_SIM_IMAGES:-0}" != 1 ] && OI_VISION=0
info "caps.roi.tier=${OI_ROI_TIER:-absent} caps.fcrop.tier=${OI_FC_TIER:-absent} live=${OI_FC_LIVE:-?} rotation=$(jget "$OI_BASE" caps.rotation)"
fk=$(jget "$OI_BASE" caps.framesource.keepalive); dni=$(jget "$OI_BASE" caps.daynight.ir_idle)
if [ -z "$fk" ]; then oi_skip "caps.framesource absent (timpsd predates general.fs_keepalive)"
elif [ -n "$dni" ] && [ "$(jget "$OI_BASE" caps.daynight.available)" = 1 ]; then
	[ "$dni" = "$fk" ] && oi_ok "caps.daynight.ir_idle=$dni follows caps.framesource.keepalive=$fk" \
		|| oi_bad "caps.daynight.ir_idle=$dni but caps.framesource.keepalive=$fk (Idle-Dark needs the keepalive)"
fi
iir=$(jget "$OI_BASE" daynight.ir_idle.reason)
[ -n "$iir" ] && oi_info "daynight.ir_idle: cfg=$(jget "$OI_BASE" daynight.ir_idle.cfg) available=$(jget "$OI_BASE" daynight.ir_idle.available) reason=$iir dark=$(jget "$OI_BASE" daynight.ir_idle.dark)"

# Everything changed below is saved first; one body restores it all.
oi_img_keys="brightness colorfx scene dpc_strength defog_strength drc_strength sinter_strength temper_strength ae_compensation"
oi_ib=""
for k in $oi_img_keys; do
	case "$OI_CAPS_IMG" in *"\"$k\""*) oi_save "image.$k" "$k" oi_ib;; esac
done
if [ -n "$OI_FC_TIER" ] && [ "$OI_FC_TIER" != unsupported ]; then
	for k in fcrop_enable fcrop_x fcrop_y fcrop_w fcrop_h; do oi_save "image.$k" "$k" oi_ib; done
fi
oi_body=""; [ -n "$oi_ib" ] && oi_body="\"image\":{$oi_ib}"
# ROI slot per stream: the first disabled one (else 3)
declare -A OI_SLOT
oi_rb=""
if [ -n "$OI_ROI_TIER" ]; then
	for s in 0 1; do
		[ "$(jget "$OI_BASE" "video.$s.enabled")" = 1 ] || continue
		n=3
		for c in 0 1 2 3; do [ "$(jget "$OI_BASE" "roi.$s.$c.enabled")" = 0 ] && { n=$c; break; }; done
		OI_SLOT[$s]=$n
		[ "$OI_ROI_TIER" = unsupported ] && continue   # a refused POST changes nothing
		oi_sb=""
		for k in enabled x y w h qp qp_mode; do oi_save "roi.$s.$n.$k" "$k" oi_sb; done
		[ -n "$oi_sb" ] && oi_rb="$oi_rb${oi_rb:+,}\"$s\":{\"$n\":{$oi_sb}}"
	done
fi
[ -n "$oi_rb" ] && oi_body="$oi_body${oi_body:+,}\"roi\":{$oi_rb}"
OI_SAVE_BODY="{$oi_body}"
printf '%s\n' "$OI_SAVE_BODY" > "$OI_DIR/saved_settings.json"
info "saved ${#OI_SAVE_PATHS[@]} setting(s) for the restore: $OI_DIR/saved_settings.json"
qa_traps 'oi_hold_stop; oi_restore'

oi_dn_boot_wait

# ---------------------------------------------------------------- OI-4 ISP keys
oi_isp_val() { case "$1" in colorfx) [ "$2" = 1 ] && echo 0 || echo 1;; scene) [ "$2" = 2 ] && echo 0 || echo 2;;
	*) [ "$2" = 64 ] && echo 192 || echo 64;; esac; }
oi_out=""
for k in colorfx scene dpc_strength defog_strength drc_strength sinter_strength temper_strength ae_compensation; do
	case "$OI_CAPS_IMG" in
	*"\"$k\""*)
		cur=$(jget "$OI_BASE" "image.$k"); new=$(oi_isp_val "$k" "$cur")
		code=$(oi_post "{\"image\":{\"$k\":$new}}" "$OI_DIR/isp_$k.json")
		oi_get "$OI_DIR/isp_${k}_get.json"; got=$(jget "$OI_DIR/isp_${k}_get.json" "image.$k")
		oi_post "{\"image\":{\"$k\":$cur}}" "$OI_DIR/isp_${k}_restore.json" >/dev/null
		if [ "$code" = 200 ] && [ "$got" = "$new" ]; then oi_ok "image.$k round-trip $cur -> $new -> $cur"
		else oi_bad "image.$k: POST $new gave HTTP $code, read back '$got'"; fi;;
	*) oi_out="$oi_out $k";;
	esac
done
if [ -n "$oi_out" ]; then
	b=""; for k in $oi_out; do b="$b${b:+,}\"$k\":0"; done
	code=$(oi_post "{\"image\":{$b}}" "$OI_DIR/isp_unsupported.json")
	if [ "$code" = 422 ] && grep -q not_supported_on_soc "$OI_DIR/isp_unsupported.json"; then
		oi_ok "not in caps.image (vendor libimp or SoC without them), refused with 422:$oi_out"
	else oi_bad "keys outside caps.image ($oi_out) answered HTTP $code: $(head -c 200 "$OI_DIR/isp_unsupported.json")"; fi
fi

# image effects (default JPEG channel)
oi_mean_at() {   # <key> <value> -> mean luma (or empty)
	oi_post "{\"image\":{\"$1\":$2}}" /dev/null >/dev/null
	# T31 AE: ~6 s dead time, then ~3 luma/s (local: runs in a $(...) subshell)
	[ "$1" = ae_compensation ] && { sleep 8; OI_SETTLE_MAX=40; }
	oi_settle - "isp_$1_$2" >&2 || return 1
	oi_kv "$(oi_vis stats "$OI_LAST")" mean
}
oi_dn_state() { oi_get "$1" && printf '%s/%s' "$(jget "$1" daynight.mode)" "$(jget "$1" daynight.night_baseline)"; }
oi_lift() {   # <key> <lo> <hi> <restore>: lo, hi, lo, hi; a moving scene is INCONCLUSIVE
	local lo hi lo2 hi2 r msg dn0 dn1
	dn0=$(oi_dn_state "$OI_DIR/dn_$1_0.json")
	lo=$(oi_mean_at "$1" "$2"); hi=$(oi_mean_at "$1" "$3"); lo2=$(oi_mean_at "$1" "$2"); hi2=$(oi_mean_at "$1" "$3")
	oi_post "{\"image\":{\"$1\":$4}}" /dev/null >/dev/null
	dn1=$(oi_dn_state "$OI_DIR/dn_$1_1.json")
	if [ -z "$lo" ] || [ -z "$hi" ] || [ -z "$lo2" ] || [ -z "$hi2" ]; then oi_skip "image.$1 effect: no snapshot"; return; fi
	if [ "$dn0" != "$dn1" ]; then
		oi_skip "image.$1 $2 -> $3: day/night changed during the test (mode/night_baseline $dn0 -> $dn1) - inconclusive"
		return
	fi
	r=$(oi_vis lift --lo "$lo" --hi "$hi" --lo2 "$lo2" --hi2 "$hi2")
	msg="image.$1 $2 -> $3 -> $2 -> $3: $(oi_reason "$r")"
	case "$(oi_kv "$r" verdict)" in
		PASS) oi_ok "$msg";;
		FAIL) oi_soft oi_bad "$msg";;
		INCONCLUSIVE) oi_skip "$msg - rerun in steady light";;
		*) oi_soft oi_warn "$msg (AE or a saturated scene can mask it)";;
	esac
}
if [ "$OI_VISION" = 0 ]; then
	oi_skip "ISP image effects: $([ "$OI_SIM" = 1 ] && echo 'sim tier - no real ISP' || echo 'no python3')"
elif ! oi_grab - "$OI_DIR/isp_probe.jpg"; then
	oi_skip "ISP image effects: /snapshot.jpg unavailable"
else
	case "$OI_CAPS_IMG" in *'"brightness"'*) oi_lift brightness 64 192 "$(jget "$OI_BASE" image.brightness)";; esac
	case "$OI_CAPS_IMG" in *'"ae_compensation"'*) oi_lift ae_compensation 40 230 "$(jget "$OI_BASE" image.ae_compensation)";; esac
	case "$OI_CAPS_IMG" in *'"colorfx"'*)
		c0=$(jget "$OI_BASE" image.colorfx)
		oi_post '{"image":{"colorfx":0}}' /dev/null >/dev/null; oi_settle - isp_cfx0; s0=$(oi_kv "$(oi_vis stats "$OI_LAST")" sat)
		oi_post '{"image":{"colorfx":1}}' /dev/null >/dev/null; oi_settle - isp_cfx1; s1=$(oi_kv "$(oi_vis stats "$OI_LAST")" sat)
		oi_post "{\"image\":{\"colorfx\":$c0}}" /dev/null >/dev/null
		if [ -z "$s0" ] || [ -z "$s1" ]; then oi_skip "colorfx effect: no snapshot"
		elif fcmp "$s0" lt 4; then oi_skip "colorfx effect: picture has no colour (sat $s0; night/IR?)"
		elif fcmp "$s1" le "$(awk -v a="$s0" 'BEGIN{print a*0.35}')"; then oi_ok "colorfx 1 (B/W): saturation $s0 -> $s1 (<= 35 %)"
		else oi_soft oi_bad "colorfx 1 (B/W): saturation $s0 -> $s1 - the mode had no visible effect"; fi;;
	esac
	for k in sinter_strength temper_strength; do
		case "$OI_CAPS_IMG" in *"\"$k\""*)
			c0=$(jget "$OI_BASE" "image.$k"); nz=""
			for v in 0 255; do
				oi_post "{\"image\":{\"$k\":$v}}" /dev/null >/dev/null; sleep 2
				fl=(); for i in 1 2 3; do oi_grab - "$OI_DIR/nr_${k}_${v}_$i.jpg" && fl+=("$OI_DIR/nr_${k}_${v}_$i.jpg"); sleep 0.4; done
				[ "${#fl[@]}" -ge 2 ] && nz="$nz $v:$(oi_kv "$(oi_vis noise "${fl[@]}")" tnoise)"
			done
			oi_post "{\"image\":{\"$k\":$c0}}" /dev/null >/dev/null
			oi_info "image.$k temporal noise (strength:noise):$nz (lighting/gain dependent, not graded)";;
		esac
	done
	oi_info "image.scene/dpc/defog/drc: round-trip only (no robust picture metric)"
fi

# ---------------------------------------------------------------- OI-2 FrontCrop
oi_fc_post() { oi_post "{\"image\":{\"fcrop_enable\":$1,\"fcrop_x\":$2,\"fcrop_y\":$3,\"fcrop_w\":$4,\"fcrop_h\":$5}}" "$6"; }
if [ -z "$OI_FC_TIER" ]; then
	oi_skip "FrontCrop: caps.fcrop absent (timpsd predates it)"
else
	sw=$(jget "$OI_BASE" caps.fcrop.sensor.0); sh=$(jget "$OI_BASE" caps.fcrop.sensor.1)
	zm=$(jget "$OI_BASE" caps.fcrop.zoom_max)
	# 50 % window, a multiple of 16 wide, stream aspect kept, inside zoom_max
	mw=$(jget "$OI_BASE" caps.fcrop.min_win.0); mh=$(jget "$OI_BASE" caps.fcrop.min_win.1)
	ww=$(awk -v W="${sw:-0}" -v z="${zm:-0}" -v m="${mw:-0}" 'BEGIN{w=W/2; if(z>0 && W/w>z) w=W/z*1.05; if(w<m) w=m; w=int((w+15)/16)*16; if(w>W) w=W; print w}')
	wh=$(awk -v w="$ww" -v W="${sw:-1}" -v H="${sh:-0}" -v m="${mh:-0}" 'BEGIN{h=int(w*H/W/2+0.5)*2; if(h<m) h=m+m%2; if(h>H) h=H; print h}')
	wx=$(( ( (${sw:-0} - ww) / 4 ) * 2 )); wy=$(( ( (${sh:-0} - wh) / 4 ) * 2 ))
	WIN_A="$wx,$wy,$ww,$wh"; WIN_B="0,0,$ww,$wh"
	case "$OI_FC_TIER" in
	unsupported)
		code=$(oi_fc_post 1 "$wx" "$wy" "$ww" "$wh" "$OI_DIR/fc_unsup.json")
		if oi_refused "$code" "$OI_DIR/fc_unsup.json"; then oi_ok "FrontCrop unsupported: POST refused (HTTP $code)"
		else oi_bad "FrontCrop unsupported but POST answered HTTP $code: $(head -c 200 "$OI_DIR/fc_unsup.json")"; fi
		oi_get "$OI_DIR/fc_unsup_get.json"
		[ "$(jget "$OI_DIR/fc_unsup_get.json" caps.fcrop.state)" = unsupported ] && oi_ok "caps.fcrop.state stays unsupported" \
			|| oi_bad "caps.fcrop.state=$(jget "$OI_DIR/fc_unsup_get.json" caps.fcrop.state) after a refused POST"
		oi_skip "FrontCrop image tests: tier unsupported";;
	experimental|effective|sim)
		if [ "$OI_FC_TIER" = experimental ] && ! oi_flag --include-experimental; then
			oi_skip "FrontCrop: tier experimental - rerun with --include-experimental"
		elif [ "${OI_FC_LIVE:-0}" != 1 ]; then
			if oi_flag --allow-restart; then
				code=$(oi_fc_post 1 "$wx" "$wy" "$ww" "$wh" "$OI_DIR/fc_start.json")
				oi_get "$OI_DIR/fc_start_get.json"
				[ "$code" = 200 ] && [ "$(jget "$OI_DIR/fc_start_get.json" image.fcrop_w)" = "$ww" ] \
					&& oi_ok "FrontCrop start-only: window persisted (HTTP $code); applies after /etc/init.d/S95timps restart - NOT done by this script" \
					|| oi_bad "FrontCrop start-only: POST HTTP $code, fcrop_w read back $(jget "$OI_DIR/fc_start_get.json" image.fcrop_w)"
			else
				oi_skip "FrontCrop: caps.fcrop.live=0 (start-only, T23 driver without fcrop_upscale_pct) - image test needs a restart; --allow-restart checks the persist only"
			fi
		elif [ -z "$sw" ] || [ "${sw:-0}" -le 0 ]; then
			oi_skip "FrontCrop: caps.fcrop.sensor unknown"
		elif [ "$ww" -ge "$sw" ] && [ "$wh" -ge "$sh" ]; then
			oi_skip "FrontCrop: no window smaller than the sensor ${sw}x${sh} is allowed (the largest stream already is the sensor and fcrop_upscale_pct is 0) - nothing to crop"
		else
			info "FrontCrop windows (sensor ${sw}x${sh}): A=$WIN_A B=$WIN_B"
			hs=""
			for s in 0 1; do
				[ "$(jget "$OI_BASE" "video.$s.enabled")" = 1 ] && [ "$(jget "$OI_BASE" "caps.fcrop.zoomable.$s")" = 1 ] \
					&& oi_hold_start "$s" && hs="$hs $s"
			done
			[ -n "$hs" ] && { info "FrontCrop: holding an RTSP consumer on stream(s)$hs"; sleep 3; }
			code=$(oi_fc_post 1 "$wx" "$wy" "$ww" "$wh" "$OI_DIR/fc_a.json"); oi_get "$OI_DIR/fc_a_get.json"
			st=$(jget "$OI_DIR/fc_a_get.json" caps.fcrop.state); rt=""; fc_wait=0
			if [ "$code" = 200 ] && [ "$st" != on ]; then
				# a re-POST of the unchanged window retries it (the consumer may have been slow to start)
				sleep 2; code=$(oi_fc_post 1 "$wx" "$wy" "$ww" "$wh" "$OI_DIR/fc_a2.json"); oi_get "$OI_DIR/fc_a2_get.json"
				st=$(jget "$OI_DIR/fc_a2_get.json" caps.fcrop.state); rt=" (after a retry)"
			fi
			if [ "$code" = 200 ] && [ "$st" = on ]; then oi_ok "FrontCrop POST window A: HTTP 200, caps.fcrop.state=on$rt"
			elif [ "$code" = 200 ] && [ "$st" = waiting ]; then fc_wait=1; oi_warn "FrontCrop POST window A: state=waiting - no stream that can show it ran (consumer on stream(s)${hs:- none})"
			else oi_bad "FrontCrop POST window A: HTTP $code, state=$st"; fi
			code=$(oi_fc_post 0 "$wx" "$wy" "$ww" "$wh" "$OI_DIR/fc_off.json"); oi_get "$OI_DIR/fc_off_get.json"
			st=$(jget "$OI_DIR/fc_off_get.json" caps.fcrop.state)
			[ "$st" = off ] && oi_ok "FrontCrop off: caps.fcrop.state=off" || oi_bad "FrontCrop off: state=$st (HTTP $code)"
			if [ "$OI_VISION" = 0 ]; then
				oi_skip "FrontCrop picture check: $([ "$OI_SIM" = 1 ] && echo 'sim tier - the sim does not crop' || echo 'no python3')"
			elif [ "$fc_wait" = 1 ]; then
				oi_skip "FrontCrop picture check: window A stayed waiting, already reported above"
			else
				for s in 0 1; do
					[ "$(jget "$OI_BASE" "video.$s.enabled")" = 1 ] || continue
					zb=$(jget "$OI_BASE" "caps.fcrop.zoomable.$s"); exp=crop; [ "$zb" = 0 ] && exp=full
					oi_fc_post 0 "$wx" "$wy" "$ww" "$wh" /dev/null >/dev/null
					oi_settle "$s" "fc${s}_before" || { oi_skip "FrontCrop stream $s: no picture (snapshot ?chn=$s / RTSP)"; continue; }
					before="$OI_LAST"
					for w in A B; do
						rotv=$(jget "$OI_BASE" "video.$s.rotation")
						if [ "$w" = B ] && [ "$exp" = crop ] && [ -n "$rotv" ] && [ "$rotv" != 0 ]; then
							oi_skip "FrontCrop stream $s window B: stream rotated $rotv, the corner window is in sensor orientation and not graded"
							continue
						fi
						if [ "$w" = A ]; then win=$WIN_A; alt=$WIN_B; else win=$WIN_B; alt=$WIN_A; fi
						IFS=, read -r x y ww2 hh2 <<<"$win"
						oi_fc_post 1 "$x" "$y" "$ww2" "$hh2" /dev/null >/dev/null
						oi_settle "$s" "fc${s}_$w" || { oi_skip "FrontCrop stream $s window $w: no picture"; continue; }
						r=$(oi_vis crop --before "$before" --after "$OI_LAST" --win "$win" --alt "$alt" --sensor "$sw,$sh" --expect "$exp")
						printf 'stream %s window %s: %s\n' "$s" "$w" "$r" >> "$OI_DIR/fcrop_metrics.txt"
						oi_grade "$OI_FC_TIER" "$(oi_kv "$r" verdict)" "FrontCrop stream $s window $w ($win, expect $exp): $(oi_reason "$r") [ncc win $(oi_kv "$r" ncc_win) full $(oi_kv "$r" ncc_full) alt $(oi_kv "$r" ncc_alt), $(oi_kv "$r" size_after)]"
					done
					oi_fc_post 0 "$wx" "$wy" "$ww" "$wh" /dev/null >/dev/null
					oi_settle "$s" "fc${s}_after_off" || continue
					r=$(oi_vis crop --before "$before" --after "$OI_LAST" --win "$WIN_A" --sensor "$sw,$sh" --expect full)
					oi_grade "$OI_FC_TIER" "$(oi_kv "$r" verdict)" "FrontCrop stream $s off again: $(oi_reason "$r") [ncc full $(oi_kv "$r" ncc_full)]"
				done
			fi
			oi_hold_stop
		fi;;
	*) oi_warn "FrontCrop: unknown tier '$OI_FC_TIER'";;
	esac
fi

# ---------------------------------------------------------------- OI-3 ROI
oi_iframes() {   # <stream> <tag> <scale> -> OI_FRAMES=(...)
	local s="$1" tag="$2" sc="$3" p gop fps to vf
	OI_FRAMES=()
	p=$(jget "$OI_BASE" "video.$s.rtsp_path"); p="${p#/}"
	gop=$(jget "$OI_BASE" "video.$s.gop"); fps=$(jget "$OI_BASE" "video.$s.fps")
	to=$(awk -v g="${gop:-30}" -v f="${fps:-15}" 'BEGIN{t=int(5*g/(f>0?f:15))+15; print (t>90?90:t)}')
	vf="select='eq(pict_type\,I)'"; [ "$sc" = 2 ] && vf="$vf,scale=iw/2:ih/2"
	rm -f "$OI_DIR/${tag}"_*.pgm
	# the first I-frame can be the hub's cached IDR from before the change: drop it
	timeout "$to" ffmpeg -v error -y -rtsp_transport tcp -i "$(rtsp_url "$p")" -an \
		-vf "$vf" $OI_VFR vfr -pix_fmt gray -frames:v 4 "$OI_DIR/${tag}_%d.pgm" \
		</dev/null >>"$OI_DIR/ffmpeg_roi.log" 2>&1
	local i; for i in 2 3 4; do [ -s "$OI_DIR/${tag}_$i.pgm" ] && OI_FRAMES+=("$OI_DIR/${tag}_$i.pgm"); done
	[ "${#OI_FRAMES[@]}" -ge 2 ]
}
oi_roi_post() {  # <s> <n> <en> <x> <y> <w> <h> <qp> <out>
	oi_post "{\"roi\":{\"$1\":{\"$2\":{\"enabled\":$3,\"x\":$4,\"y\":$5,\"w\":$6,\"h\":$7,\"qp\":$8,\"qp_mode\":1}}}}" "$9"
}
oi_roi_measure() {  # <s> <dir> <tag> <qp> <rc_mode> [label]; uses OFF rx ry rw rh sc -> OI_R OI_V OI_HINT
	OI_R=""; OI_V=""; OI_HINT=""
	oi_iframes "$1" "$3" "$sc" || return 1
	OI_R=$(oi_vis roi --off "${OFF[@]}" --on "${OI_FRAMES[@]}" --rect "$((rx/sc)),$((ry/sc)),$((rw/sc)),$((rh/sc))" --dir "$2" \
		--delta "$4" --rc "$5")
	printf 'stream %s %s%s: %s\n' "$1" "$2" "${6:-}" "$OI_R" >> "$OI_DIR/roi_metrics.txt"
	OI_V=$(oi_kv "$OI_R" verdict)
	[ "$2" = sharp ] && [ "$OI_V" = NOEFFECT ] && [ "$OI_ROI_TIER" = effective ] && [ "$5" != fixqp ] \
		&& { OI_V=WARN; OI_HINT=" - base QP may already be near min_qp"; }
	return 0
}
if [ -z "$OI_ROI_TIER" ]; then
	oi_skip "ROI: caps.roi absent (timpsd predates it)"
elif [ "$OI_ROI_TIER" = experimental ] && ! oi_flag --include-experimental; then
	oi_skip "ROI: tier experimental - rerun with --include-experimental"
else
	al=$(jget "$OI_BASE" caps.roi.align); dmin=$(jget "$OI_BASE" caps.roi.qp_delta_eff.0); dmax=$(jget "$OI_BASE" caps.roi.qp_delta_eff.1)
	[ "${al:-0}" = 16 ] && oi_ok "caps.roi.align=16 (macroblock)" || oi_warn "caps.roi.align=${al:-absent}, expected 16"
	dpos=$(( ${dmax:-20} < 20 ? ${dmax:-20} : 20 )); dneg=$(( ${dmin:--15} > -15 ? ${dmin:--15} : -15 ))
	for s in 0 1; do
		[ -n "${OI_SLOT[$s]:-}" ] || continue
		n=${OI_SLOT[$s]}; sst=$(jget "$OI_BASE" "caps.roi.streams.$s")
		W=$(jget "$OI_BASE" "video.$s.width"); H=$(jget "$OI_BASE" "video.$s.height")
		rx=$(( W/3/16*16 )); ry=$(( H/3/16*16 )); rw=$(( W/3/16*16 )); rh=$(( H/3/16*16 ))
		rej0=$(jget "$OI_BASE" "caps.roi.rejected.$s")
		if [ "$OI_ROI_TIER" = unsupported ] || [ "$sst" = unsupported ]; then
			code=$(oi_roi_post "$s" "$n" 1 "$rx" "$ry" "$rw" "$rh" "$dpos" "$OI_DIR/roi${s}_unsup.json")
			oi_refused "$code" "$OI_DIR/roi${s}_unsup.json" && oi_ok "ROI stream $s unsupported: POST refused (HTTP $code)" \
				|| oi_bad "ROI stream $s unsupported but POST answered HTTP $code: $(head -c 200 "$OI_DIR/roi${s}_unsup.json")"
			if oi_iframes "$s" "roi${s}_unsup" 1; then oi_ok "ROI stream $s: stream still delivers I-frames after the refused POST"
			else oi_skip "ROI stream $s: stream check after the refused POST not possible"; fi
			continue
		fi
		if [ "$sst" != ok ]; then oi_skip "ROI stream $s: caps.roi.streams[$s]=$sst (only H.264 unrotated streams apply regions)"; continue; fi
		sc=1; [ "${W:-0}" -gt 1280 ] && sc=2
		if [ "$OI_VISION" = 1 ]; then
			oi_roi_post "$s" "$n" 0 0 0 16 16 0 /dev/null >/dev/null
			if ! oi_iframes "$s" "roi${s}_off" "$sc"; then oi_skip "ROI stream $s: could not grab I-frames (see $OI_DIR/ffmpeg_roi.log)"; continue; fi
			OFF=("${OI_FRAMES[@]}")
			r=$(oi_vis region --img "${OFF[0]}" --align $((16 / sc)))
			if ! oi_ints "$(oi_kv "$r" x)" "$(oi_kv "$r" y)" "$(oi_kv "$r" w)" "$(oi_kv "$r" h)"; then
				oi_skip "ROI stream $s: no textured box ($(oi_reason "$r"))"; continue
			fi
			rx=$(( $(oi_kv "$r" x) * sc )); ry=$(( $(oi_kv "$r" y) * sc )); rw=$(( $(oi_kv "$r" w) * sc )); rh=$(( $(oi_kv "$r" h) * sc ))
			info "ROI stream $s slot $n: textured box ${rx},${ry} ${rw}x${rh} (share $(oi_kv "$r" share) of the frame's detail)"
		fi
		code=$(oi_roi_post "$s" "$n" 1 "$rx" "$ry" "$rw" "$rh" "$dpos" "$OI_DIR/roi${s}_pos.json")
		oi_get "$OI_DIR/roi${s}_pos_get.json"; g="$OI_DIR/roi${s}_pos_get.json"
		echo_ok=1; for kv in "enabled:1" "x:$rx" "y:$ry" "w:$rw" "h:$rh" "qp:$dpos" "qp_mode:1"; do
			[ "$(jget "$g" "roi.$s.$n.${kv%%:*}")" = "${kv#*:}" ] || echo_ok=0; done
		[ "$code" = 200 ] && [ "$echo_ok" = 1 ] && oi_ok "ROI stream $s: POST region (qp $dpos) echoed back unchanged" \
			|| oi_bad "ROI stream $s: POST HTTP $code, echo mismatch (see $g)"
		[ $(( rx % 16 + ry % 16 + rw % 16 + rh % 16 )) = 0 ] || oi_warn "ROI stream $s: rect not 16-aligned"
		if [ "$OI_VISION" = 0 ]; then
			oi_skip "ROI stream $s picture check: $([ "$OI_SIM" = 1 ] && echo 'sim tier - the sim does not encode' || echo 'no python3')"
		else
			sleep 1
			for d in blur sharp; do
				[ "$d" = sharp ] && oi_roi_post "$s" "$n" 1 "$rx" "$ry" "$rw" "$rh" "$dneg" /dev/null >/dev/null
				qd=$dpos; [ "$d" = sharp ] && qd=$dneg
				rcm=$(jget "$OI_BASE" "video.$s.rc_mode")
				if ! oi_roi_measure "$s" "$d" "roi${s}_$d" "$qd" "$rcm"; then oi_skip "ROI stream $s $d: could not grab I-frames"; continue; fi
				r=$OI_R; v=$OI_V; hint=$OI_HINT; note=""
				# the region applies from the next IDR and CBR noise moves rel: a fresh connection (new IDR) decides
				case "$v" in FAIL|NOEFFECT)
					sleep 1
					if oi_roi_measure "$s" "$d" "roi${s}_${d}_re" "$qd" "$rcm" " recheck"; then
						rr=$(oi_vis recheck --v1 "$v" --rel1 "$(oi_kv "$r" rel)" --v2 "$OI_V" --rel2 "$(oi_kv "$OI_R" rel)")
						rv=$(oi_kv "$rr" verdict)
						if [ "$rv" = "$OI_V" ]; then r=$OI_R; v=$OI_V; hint=$OI_HINT
						elif [ "$rv" = WARN ]; then v=WARN; hint=""; fi
						note=" ($(oi_reason "$rr"))"
					else note=" (recheck: no I-frames, first measurement kept)"; fi;;
				esac
				msg="ROI stream $s qp $qd ($d): $(oi_reason "$r") [in $(oi_kv "$r" ratio_in) out $(oi_kv "$r" ratio_out)]$note"
				if [ "$v" = WARN ]; then oi_warn "$msg$hint"; else oi_grade "$OI_ROI_TIER" "$v" "$msg"; fi
			done
		fi
		oi_roi_post "$s" "$n" 0 "$rx" "$ry" "$rw" "$rh" "$dpos" /dev/null >/dev/null
		oi_get "$OI_DIR/roi${s}_end.json"; rej1=$(jget "$OI_DIR/roi${s}_end.json" "caps.roi.rejected.$s")
		[ "${rej1:-0}" = "${rej0:-0}" ] && oi_ok "ROI stream $s: caps.roi.rejected unchanged (${rej0:-0})" \
			|| oi_bad "ROI stream $s: libimp rejected regions during the test (caps.roi.rejected ${rej0:-0} -> $rej1)"
	done
	[ "${#OI_SLOT[@]}" -gt 0 ] || oi_skip "ROI: no enabled stream"
fi

# ---------------------------------------------------------------- OI-5 rotation
for s in 0 1; do
	rot=$(jget "$OI_BASE" "video.$s.rotation"); [ -n "$rot" ] || continue
	[ "$(jget "$OI_BASE" "video.$s.enabled")" = 1 ] || continue
	case "$rot" in
	90|270)
		W=$(jget "$OI_BASE" "video.$s.width"); H=$(jget "$OI_BASE" "video.$s.height")
		ew=$(jget "$OI_BASE" "video.$s.eff_width"); eh=$(jget "$OI_BASE" "video.$s.eff_height")
		p=$(jget "$OI_BASE" "video.$s.rtsp_path"); p="${p#/}"
		dims=$(timeout 15 ffprobe -v error -rtsp_transport tcp -select_streams v:0 -show_entries stream=width,height -of csv=p=0:s=x "$(rtsp_url "$p")" 2>/dev/null | head -1)
		if [ "$ew" = "$H" ] && [ "$eh" = "$W" ]; then
			[ "$dims" = "${ew}x${eh}" ] && oi_ok "rotation $rot on stream $s: eff ${ew}x${eh} swapped and the stream is $dims" \
				|| oi_bad "rotation $rot on stream $s: eff ${ew}x${eh} but the stream is ${dims:-unreadable}"
		else
			oi_info "rotation $rot on stream $s refused at start (eff ${ew}x${eh} unswapped; see the 'refusing FS-rotate' log line), stream ${dims:-?}"
		fi;;
	*) oi_skip "rotation stream $s: not configured rotated (videoN.rotation is restart-only, the QA never changes it)";;
	esac
done

# ---------------------------------------------------------------- OI-6 on-device
if [ -n "${SSH_TARGET:-}" ]; then
	cl=$(sshx 'timpsd --check-libs 2>&1; echo "rc=$?"' 2>/dev/null)
	clrc=$(printf '%s\n' "$cl" | sed -n 's/^rc=//p' | tail -1)
	printf '%s\n' "$cl" > "$OI_DIR/check_libs.txt"
	if [ "$clrc" = 0 ]; then oi_ok "timpsd --check-libs: $(printf '%s\n' "$cl" | grep -m1 'libimp')"
	else oi_bad "timpsd --check-libs rc=${clrc:-?}: $(printf '%s\n' "$cl" | grep -v '^rc=' | head -2 | tr '\n' ' ')"; fi
	hr=$(sshx "logread 2>/dev/null | grep -o 'hr=-\{0,1\}[0-9]*' | tail -20" 2>/dev/null | tr '\n' ' ')
	[ -n "$hr" ] && oi_info "daynight AE reserve (last hr= values): $hr" || oi_info "no daynight hr= lines in logread"
else
	oi_skip "on-device checks (--check-libs, AE reserve): no --ssh"
fi

oi_restore
log "  openimp section: PASS=$OI_P WARN=$OI_W FAIL=$OI_F SKIP=$OI_S INFO=$OI_I (details in $OI_DIR)"
fi
