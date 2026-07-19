#!/bin/bash
# build.sh - standalone cross-compilation for timps (Tiny IMP Streamer),
# modeled after prudynt-t's build.sh. Downloads the thingino toolchain on
# first use, fetches the vendor IMP libraries (gtxaspec/ingenic-lib) and the
# libc shim, then builds a single timpsd drop-in binary.
#
#   ./build.sh deps  <SOC> [options]   # toolchain + vendor libs (+ faac with -faac)
#   ./build.sh timps <SOC> [options]   # build timpsd
#   ./build.sh full  <SOC> [options]   # fresh deps + timps
set -e

# Capture whether TIMPS_CROSS was explicitly set before applying the default
_TIMPS_CROSS_EXPLICIT=${TIMPS_CROSS+set}

TOP=$(pwd)
NFS_SHARE="/nfs"

TOOLCHAIN_RELEASE="toolchain-x86_64"

# Pinned dependency revisions (kept in sync with the thingino packages)
INGENIC_LIB_VER="99ed33fd55fdf4fdfaa378a0924c5c6a7a31943b" # package/ingenic-lib
MUSLSHIM_VER="be103c48b47ce5491c4ae051793124f877d32f45"    # package/ingenic-musl
# faac: timps drives the modern faac_encoder_* API (the legacy faacEnc* API
# was removed upstream); that API landed AFTER the faac-1.50 release, so a
# post-1.50 master commit is required here (not the 1.50 tag).
FAAC_VER="b92b7f81e53b1027107c900b11609abf32a1fb1a"

# Libc selection: "uclibc" (default - matches this firmware tree's cameras;
# no shim needed, the vendor libs are uclibc-built) or "musl" (mainline
# thingino cameras; links the ingenic-musl shim).
LIBC_TYPE="uclibc"

# Option flags (filled by parse_flags)
USE_FAAC=0
DEBUG_BUILD=0
STATIC_BUILD=0
KERNEL4=0
CLEAN_ALL=0

parse_flags() {
	for arg in "$@"; do
		case "$arg" in
			--libc-uclibc) LIBC_TYPE="uclibc" ;;
			--libc-musl)   LIBC_TYPE="musl" ;;
			-faac)         USE_FAAC=1 ;;
			-debug)        DEBUG_BUILD=1 ;;
			-static)       STATIC_BUILD=1 ;;
			--kernel-4)    KERNEL4=1 ;;
			--clean-all)   CLEAN_ALL=1 ;;
		esac
	done
}

# Compiler defense-in-depth for this root-running network daemon (M14). Two
# central switches let a stubborn target toolchain dial it back:
#   HARDEN=0   -> no compiler hardening at all (bare build)
#   FORTIFY=0  -> keep stack-protector but drop only _FORTIFY_SOURCE
# (RELRO/BIND_NOW + noexecstack are linker-only and applied unconditionally in
# the ldflags below.)
#
# Why the libc split:
#   * The thingino uClibc toolchain is built --disable-libssp, so the SSP
#     symbols (__stack_chk_*) are unavailable, AND its headers lack the full
#     set of _FORTIFY_SOURCE *_chk wrappers - enabling either fails to link or
#     silently no-ops. So uClibc gets neither; use --libc-musl (or a
#     libssp-enabled toolchain) for full compiler hardening.
#   * musl carries SSP + FORTIFY; _FORTIFY_SOURCE needs the -Os/-O we pass.
HARDEN=${HARDEN:-1}
FORTIFY=${FORTIFY:-1}
LIBC_EXTRA_CFLAGS=""
apply_libc_env() {
	if [[ "$HARDEN" != "1" ]]; then
		LIBC_EXTRA_CFLAGS="-fno-stack-protector"
		return
	fi
	if [[ "$LIBC_TYPE" == "uclibc" ]]; then
		# uClibc: --disable-libssp + incomplete *_chk -> no SSP/FORTIFY here.
		LIBC_EXTRA_CFLAGS="-fno-stack-protector"
	else
		LIBC_EXTRA_CFLAGS="-fstack-protector-strong"
		if [[ "$FORTIFY" == "1" ]]; then
			LIBC_EXTRA_CFLAGS="$LIBC_EXTRA_CFLAGS -D_FORTIFY_SOURCE=2"
		fi
	fi
}

# Map SOC to xburst generation
get_xburst_generation() {
	local soc="$1"
	case "$soc" in
		T10 | T20 | T21 | T23 | T30 | T31 | C100)
			echo "xburst1"
			;;
		T40 | T41)
			echo "xburst2"
			;;
		*)
			echo "xburst1"  # Default to xburst1 for unknown SOCs
			;;
	esac
}

# Set toolchain variables based on SOC and selected libc
set_toolchain_for_soc() {
	local soc="$1"
	local xburst=$(get_xburst_generation "$soc")

	TOOLCHAIN_ARCHIVE="thingino-toolchain-x86_64_${xburst}_${LIBC_TYPE}_gcc15-linux-mipsel.tar.gz"
	TOOLCHAIN_URL="https://github.com/themactep/thingino-firmware/releases/download/${TOOLCHAIN_RELEASE}/${TOOLCHAIN_ARCHIVE}"
	TOOLCHAIN_SDK="${TOP}/toolchain/${xburst}-${LIBC_TYPE}/mipsel-thingino-linux-${LIBC_TYPE}_sdk-buildroot"
}

ensure_toolchain() {
	[[ -n "$_TIMPS_CROSS_EXPLICIT" ]] && return 0

	if [[ ! -d "${TOOLCHAIN_SDK}/bin" ]]; then
		echo "Thingino ${LIBC_TYPE} toolchain not found, downloading..."
		local xburst_dir=$(dirname "${TOOLCHAIN_SDK}")
		mkdir -p "${xburst_dir}"
		if command -v wget &>/dev/null; then
			wget -q --show-progress "${TOOLCHAIN_URL}" -O "${TOP}/toolchain/${TOOLCHAIN_ARCHIVE}"
		else
			curl -L --progress-bar "${TOOLCHAIN_URL}" -o "${TOP}/toolchain/${TOOLCHAIN_ARCHIVE}"
		fi
		# M15: integrity-check the downloaded toolchain (supply-chain). Set
		# TOOLCHAIN_SHA256 (env or here) to the hash published with the thingino
		# release to enforce it; unset only warns so first-time users aren't blocked.
		if [[ -n "${TOOLCHAIN_SHA256:-}" ]]; then
			echo "${TOOLCHAIN_SHA256}  ${TOP}/toolchain/${TOOLCHAIN_ARCHIVE}" | sha256sum -c - \
				|| { echo "ERROR: toolchain SHA256 mismatch - aborting"; rm -f "${TOP}/toolchain/${TOOLCHAIN_ARCHIVE}"; exit 1; }
		else
			echo "WARNING: toolchain not integrity-checked (export TOOLCHAIN_SHA256=<hash> to pin)"
		fi
		echo "Extracting toolchain to ${xburst_dir}/ ..."
		tar -xf "${TOP}/toolchain/${TOOLCHAIN_ARCHIVE}" -C "${xburst_dir}"
		rm -f "${TOP}/toolchain/${TOOLCHAIN_ARCHIVE}"
		if [[ -x "${TOOLCHAIN_SDK}/relocate-sdk.sh" ]]; then
			echo "Relocating SDK..."
			"${TOOLCHAIN_SDK}/relocate-sdk.sh"
		fi
		echo "Toolchain ready."
	fi

	if command -v ccache &>/dev/null; then
		export TIMPS_CROSS="ccache ${TOOLCHAIN_SDK}/bin/mipsel-linux-"
	else
		export TIMPS_CROSS="${TOOLCHAIN_SDK}/bin/mipsel-linux-"
	fi
}

# Vendor IMP library path inside gtxaspec/ingenic-lib for a SOC. Versions
# match thingino's package/ingenic-lib/ingenic-lib.mk (kernel 3.10 defaults;
# --kernel-4 selects the 4.4 SDK where it differs).
get_ingenic_lib_src() {
	local soc="$1"
	case "$soc" in
		T10)  echo "ingenic-lib/T10/lib/3.12.0/uclibc/4.7.2" ;;
		T20)  echo "ingenic-lib/T20/lib/3.12.0/uclibc/4.7.2" ;;
		T21)  echo "ingenic-lib/T21/lib/1.0.33/uclibc/4.7.2" ;;
		T23)  echo "ingenic-lib/T23/lib/1.3.0/uclibc/5.4.0" ;;
		T30)  echo "ingenic-lib/T30/lib/1.0.5/uclibc/4.7.2" ;;
		T31)
			if [[ $KERNEL4 -eq 1 ]]; then
				echo "ingenic-lib/T31/lib/1.1.5.2/uclibc/5.4.0"
			else
				echo "ingenic-lib/T31/lib/1.1.6/uclibc/5.4.0"
			fi
			;;
		C100) echo "ingenic-lib/C100/lib/2.1.0/uclibc/5.4.0" ;;
		T40)  echo "ingenic-lib/T40/lib/1.3.1/uclibc/7.2.0" ;;
		T41)  echo "ingenic-lib/T41/lib/1.2.6/uclibc/7.2.0" ;;
		*)    echo "" ;;
	esac
}

# Shallow-clone a repo into 3rdparty/ and check out a pinned revision
clone_pinned() {
	local repo="$1" dir="$2" rev="$3"
	if [[ ! -d "$dir" ]]; then
		echo "Cloning ${repo##*/}..."
		git clone --depth=1 "$repo" "$dir"
	fi
	cd "$dir"
	# Drop local modifications (e.g. the meson.build sed) so re-runs are clean
	git reset --hard -q HEAD 2>/dev/null || true
	git clean -fdq 2>/dev/null || true
	git rev-parse -q --verify "${rev}^{commit}" >/dev/null 2>&1 || git fetch --depth=1 origin "$rev"
	git checkout -q "$rev"
	cd - >/dev/null
}

deps() {
	local soc="$1"
	shift # Remove SOC from arguments

	parse_flags "$@"
	set_toolchain_for_soc "$soc"
	ensure_toolchain
	apply_libc_env

	if [[ $CLEAN_ALL -eq 1 ]]; then
		echo "Cleaning 3rdparty/ (requested via --clean-all)"
		rm -rf 3rdparty
	fi
	mkdir -p 3rdparty/install/lib 3rdparty/install/include
	local bare_cross="${TIMPS_CROSS#ccache }"

	echo "Import vendor IMP libraries (ingenic-lib)"
	cd 3rdparty
	clone_pinned https://github.com/gtxaspec/ingenic-lib ingenic-lib "$INGENIC_LIB_VER"

	local lib_src=$(get_ingenic_lib_src "$soc")
	if [[ -z "$lib_src" ]]; then
		echo "Unsupported or unspecified SoC model: $soc"
		exit 1
	fi
	if [[ ! -d "$lib_src" ]]; then
		echo "!! $lib_src not found in ingenic-lib; available versions:"
		ls "ingenic-lib/$soc/lib" 2>/dev/null || true
		exit 1
	fi
	echo "use $lib_src"
	cp -Pf "$lib_src"/* "$TOP/3rdparty/install/lib/"
	cd "$TOP"

	# The vendor libs are uclibc-built; on musl the shim supplies the missing
	# glibc/uclibc compat symbols (__assert, pthread cancel hooks, ...).
	if [[ "$LIBC_TYPE" == "musl" ]]; then
		echo "Build libmuslshim (ingenic-musl)"
		cd 3rdparty
		clone_pinned https://github.com/gtxaspec/ingenic-musl ingenic-musl "$MUSLSHIM_VER"
		cd ingenic-musl
		make CC="${TIMPS_CROSS}gcc" -j$(nproc) static
		make CC="${TIMPS_CROSS}gcc" -j$(nproc)
		cp libmuslshim.* "$TOP/3rdparty/install/lib/"
		cd "$TOP"
	fi

	if [[ $USE_FAAC -eq 1 ]]; then
		echo "Build faac (software AAC encoder)"
		cd 3rdparty
		clone_pinned https://github.com/knik0/faac.git faac "$FAAC_VER"
		cd faac
		# faac uses meson; newer meson rejects the 'c_std=gnu99,c99' list
		sed -i "s/'c_std=gnu99,c99'/'c_std=gnu99'/g" meson.build
		# Meson treats binary values as single executable paths - split ccache
		# from the compiler using array syntax so "ccache <prefix>gcc" works.
		local meson_c="'${bare_cross}gcc'"
		if [[ "$TIMPS_CROSS" != "$bare_cross" ]]; then
			meson_c="['ccache', '${bare_cross}gcc']"
		fi
		cat > meson-cross.ini <<-CROSSFILE
			[binaries]
			c = ${meson_c}
			ar = '${bare_cross}ar'
			strip = '${bare_cross}strip'
			pkg-config = 'pkg-config'

			[host_machine]
			system = 'linux'
			cpu_family = 'mips'
			cpu = 'mipsel'
			endian = 'little'
		CROSSFILE
		rm -rf builddir
		CFLAGS="-ffast-math $LIBC_EXTRA_CFLAGS" meson setup builddir \
			--cross-file meson-cross.ini \
			--prefix="$TOP/3rdparty/install" \
			--default-library=static \
			-Db_lto=false \
			-Dfrontend=false \
			-Dmax-channels=2
		ninja -C builddir -j$(nproc)
		ninja -C builddir install
		cd "$TOP"
	fi

	echo "Dependencies ready in 3rdparty/install/"
}

timps() {
	local soc="$1"
	shift # Remove SOC from arguments

	parse_flags "$@"
	set_toolchain_for_soc "$soc"
	ensure_toolchain
	apply_libc_env
	echo "Build timps for $soc (libc: $LIBC_TYPE)"

	cd "$TOP"
	make clean

	# Set debug or release build flags
	local optimization="-Os"
	if [[ $DEBUG_BUILD -eq 1 ]]; then
		echo "Building with debug information (no optimization, debug symbols)"
		optimization="-g -O0"
	fi

	# Base CFLAGS mirror the Makefile defaults (overriding CFLAGS on the make
	# command line replaces them). The Makefile itself adds -DHAL_INGENIC,
	# -DPLATFORM_<SOC> and the include paths. -ffp-contract=off is critical on
	# XBurst FPUs (no reliable fused madd).
	local cflags="-std=c11 -D_GNU_SOURCE $optimization -Wall -Wextra \
		-Wno-unused-parameter -Wno-misleading-indentation -Wno-stringop-truncation \
		-ffunction-sections -fdata-sections -ffp-contract=off $LIBC_EXTRA_CFLAGS \
		-isystem $TOP/3rdparty/install/include"
	if [[ $KERNEL4 -eq 1 ]]; then
		cflags="$cflags -DKERNEL_VERSION_4"
	fi

	# -no-pie: the vendor static archives are non-PIC; the thingino toolchain
	# defaults to PIE, which cannot link them (R_MIPS_26 relocation errors).
	# RELRO+BIND_NOW + noexecstack (M14): read-only GOT after relocation and a
	# non-executable stack - linker-only, safe with -no-pie and both libcs.
	local ldflags="-Wl,--gc-sections -no-pie -Wl,-z,relro,-z,now -Wl,-z,noexecstack"
	if [[ $STATIC_BUILD -eq 1 ]]; then
		ldflags="$ldflags -static -static-libgcc"
	fi

	# Static vendor archives for a single self-contained drop-in; the shim
	# resolves uclibc->musl compat symbols (not needed on uclibc itself).
	local implibs="-Wl,--start-group -l:libimp.a -l:libalog.a -l:libsysutils.a -Wl,--end-group"
	if [[ "$LIBC_TYPE" == "musl" ]]; then
		implibs="$implibs -l:libmuslshim.a"
	fi

	/usr/bin/make -j$(nproc) \
		CROSS_COMPILE="${TIMPS_CROSS}" \
		PLATFORM="$soc" \
		IMP_LIB="$TOP/3rdparty/install/lib" \
		IMPLIBS="$implibs" \
		CFLAGS="$cflags" \
		LDFLAGS="$ldflags" \
		USE_FAAC=$USE_FAAC \
		USE_CONTROL=${USE_CONTROL:-1} \
		USE_DAYNIGHT=${USE_DAYNIGHT:-1} \
		-C "$TOP" target

	if [[ $DEBUG_BUILD -eq 0 ]]; then
		"${TIMPS_CROSS#ccache }strip" timpsd
	fi
	ls -lh timpsd

	if [ -d "$NFS_SHARE" ]; then
		echo "DONE. COPYING BINARY TO $NFS_SHARE"
		SOC_LOWER=$(echo "$soc" | tr '[:upper:]' '[:lower:]')
		cp -vf timpsd "$NFS_SHARE/timpsd-$SOC_LOWER"
	fi
}

usage() {
	echo "Standalone timps Build"
	echo "Usage: ./build.sh deps <platform> [options]"
	echo "       ./build.sh timps <platform> [options]"
	echo "       ./build.sh full <platform> [options]"
	echo ""
	echo "Platforms: T10, T20, T21, T23, T30, T31, C100, T40, T41"
	echo "Options:"
	echo "  -faac:          Software AAC audio via libfaac (built in deps, linked static)"
	echo "  -static:        Fully static binary (default: dynamic libc + static vendor libs)"
	echo "  -debug:         Debug build (no optimization, debug symbols, no strip)"
	echo "  --kernel-4:     Target the 4.4.94 kernel (T31: SDK 1.1.5.2 libs)"
	echo "  --libc-musl:    Thingino musl toolchain + ingenic-musl shim (mainline thingino)"
	echo "  --libc-uclibc:  Thingino uClibc toolchain (default)"
	echo "  --clean-all:    (deps) wipe 3rdparty/ first"
	echo ""
	echo "Env: TIMPS_CROSS overrides the cross prefix (skips toolchain download),"
	echo "     USE_CONTROL/USE_DAYNIGHT (default 1) forwarded to make,"
	echo "     HARDEN=0 disables compiler hardening, FORTIFY=0 drops only"
	echo "     _FORTIFY_SOURCE (M14; RELRO/NOW/noexecstack stay on regardless)."
	exit 1
}

if [ $# -eq 0 ]; then
	usage
elif [[ "$1" == "deps" ]]; then
	[ $# -ge 2 ] || usage
	deps "${@:2}"
elif [[ "$1" == "timps" ]]; then
	[ $# -ge 2 ] || usage
	timps "${@:2}"
elif [[ "$1" == "full" ]]; then
	[ $# -ge 2 ] || usage
	echo "Removing 3rdparty for a fresh full build..."
	rm -rf "${TOP}/3rdparty"
	deps "${@:2}"
	timps "${@:2}"
else
	usage
fi

exit 0
