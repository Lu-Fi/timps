# timps (Tiny IMP Streamer) - minimal RTSP + fMP4 streamer for Ingenic SoCs
#
# Cross build for the camera (default). The Ingenic SoC is a MIPS target with
# musl libc - NOT the host x86 environment - so a cross toolchain is required:
#
#   make PLATFORM=T31 CROSS_COMPILE=mipsel-linux- IMP_INC=/path/to/ingenic-headers/T31/1.1.6/en
#
# Host test build with the simulation backend (no hardware, feeds files):
#
#   make sim
#
# SIM_CFLAGS: extra flags for the sim build ONLY (never the target). The
# day/night replay harness (scripts/dn-replay.py) uses it to compress time -
# see MS_CLOCK_SCALE in src/util.h - and to override DN_* constants:
#
#   make sim SIM_CFLAGS="-DMS_CLOCK_SCALE=30"
#
# Supported PLATFORM values: T10 T20 T21 T23 T30 T31 T40 T41 C100
#
# Small-RAM targets (e.g. T10, 64 MB): the memory footprint can be tuned via
# CFLAGS defines, e.g.
#   CFLAGS += -DMS_AU_BUF_MAX=524288 -DMS_JPEG_BUF_MAX=262144 \
#             -DMS_RTSP_QCAP=32 -DMS_MP4_QCAP=32 \
#             -DRTSP_MAX_CLIENTS=4 -DHTTP_MAX_CLIENTS=4
# (AU/JPEG buffers already auto-scale with the configured resolution.)

CROSS_COMPILE ?= mipsel-linux-
PLATFORM      ?= T31
IMP_LIB       ?=            # directory containing libimp.a/.so (adds -L)
SYSROOT       ?=            # optional --sysroot for the cross toolchain
USE_FAAC      ?= 0          # 1 = software AAC audio via libfaac (browser audio)
USE_CONTROL   ?= 1          # live control endpoint (/control); optional, on by default (0 = off)
USE_DAYNIGHT  ?= 1          # native automatic day/night detection thread; on by default (0 = off)
USE_RECORD    ?= 1          # local SD recording (fMP4 segments + /control clips); on by default (0 = off, saves ~11KB)
USE_TIMELAPSE ?= 1          # native timelapse (periodic JPEG shots to SD); on by default (0 = off, saves ~4KB)
USE_TLS       ?= 0          # 1 = HTTPS + RTSPS via mbedTLS (needs -lmbedtls...); off unless the lib is present
USE_SRT       ?= 0          # 1 = MPEG-TS over SRT output via libsrt; off unless the lib is present
USE_BACKCHANNEL ?= 0        # 1 = ONVIF audio backchannel (client->speaker via native IMP_AO); G.711 pure-C
USE_BC_AAC    ?= 0          # 1 = also decode AAC backchannel (needs libhelix-aac); implies USE_BACKCHANNEL
HELIXLIB      ?= -lhelix-aac # link flag for the helix AAC decoder (USE_BC_AAC)
HELIX_INC     ?=            # optional -I dir for aacdec.h/aaccommon.h (USE_BC_AAC)
USE_PLAY      ?= 0          # 1 = /run/timps/audio_out play-FIFO queue (system sounds via native IMP_AO); WAV + raw PCM16
USE_PLAY_OPUS ?= 0          # 1 = also decode Ogg-Opus in the play queue (needs opusfile); implies USE_PLAY
OPUSLIB       ?= -lopusfile -lopus -logg  # link flags for opusfile (USE_PLAY_OPUS)
OPUS_INC      ?=            # optional -I dir for <opus/opus.h> / <opus/opusfile.h> (USE_PLAY_OPUS / USE_STREAM_OPUS)
USE_STREAM_OPUS ?= 0        # 1 = offer Opus as an RTSP/RTP streaming audio codec (RFC 7587);
                            #     needs the bare libopus (encoder), NOT opusfile. Independent of
                            #     USE_PLAY_OPUS (local .opus playback). Default off = no opus stream
                            #     code compiled in and "opus" is not an accepted audio.codec value.
OPUS_ENC_LIB  ?= -lopus     # link flag for the libopus encoder (USE_STREAM_OPUS)
USE_ROTATE    ?= 0          # 1 = image rotation feature (real 90/270 transpose: HW on
                            #     T40/T41 + T31, opt-in SW on T23). 180 is NOT a rotation
                            #     value (use image.hflip+image.vflip). Default off = no
                            #     ROT_HAS_* macros defined, so all rotation code compiles
                            #     out (byte-identical build).
USE_SW_ROTATE ?= 0          # 1 = opt-in software 90/270 rotation on T23 (CPU transpose +
                            #     unbound YuvEncode; no HW OSD/privacy on rotated streams).
                            #     Only effective with PLATFORM=T23; default off = byte-identical build.
                            #     Implies USE_ROTATE (the SW path is part of the rotation feature).
ifeq ($(USE_SW_ROTATE),1)
USE_ROTATE    := 1
endif
USE_OSD_HINTING ?= 0        # 1 = compile in the opt-in geometric OSD-text autohinter
                            #     (autohint_glyph() and friends in src/hal/msttf.c).
                            #     Still gated at runtime by the osd.hinting config key
                            #     either way; default off = measured ~2.1KB smaller
                            #     .text (T31/GCC 16.1.0/-Os) and osd.hinting=1 in
                            #     timps.conf is accepted but a no-op.
USE_TRACE     ?= 0          # 1 = compile in src/trace.c, the opt-in send-pipeline
                            #     latency instrumentation (see src/trace.h). DEVELOPER
                            #     TOOL, not for production camera images: default off
                            #     means trace.c is not even compiled, ms_trace_on()
                            #     etc. become static-false inline stubs the compiler
                            #     dead-code-eliminates at -Os, and general.trace(_ms)
                            #     in timps.conf are accepted but a no-op - exactly the
                            #     USE_OSD_HINTING pattern above. Still not a menuconfig
                            #     option either way; turn it on with
                            #     `make USE_TRACE=1 ...` for a debug build only.
HOSTCC        ?= cc

# Vendored Ingenic IMP headers (from gtxaspec/ingenic-headers) live under
# ./include/<SoC>/<ver>/<lang>. Pick the matching set per platform, like the
# original prudynt Makefile did. Override IMP_INC to use your own headers.
INC_ROOT ?= ./include
ifeq ($(PLATFORM),T31)
IMP_INC ?= $(INC_ROOT)/T31/1.1.6/en
else ifeq ($(PLATFORM),C100)
IMP_INC ?= $(INC_ROOT)/C100/2.1.0/en
else ifeq ($(PLATFORM),T21)
IMP_INC ?= $(INC_ROOT)/T21/1.0.33/zh
# P-08: smaller-RAM/older-generation SoC - halve the per-queue byte backstop
# (default 2 MB -> 1 MB) so 8 HTTP + 8 RTSP + 8 SRT all-stalled worst case is
# ~24 MB instead of ~48 MB. Lower bitrates on these boards fit comfortably.
PLATFORM_CFLAGS += -DFQ_MAX_BYTES=1048576
else ifeq ($(PLATFORM),T23)
# MUST be 1.1.2+ to match the libimp thingino ships for T23 (SDK 1.3.0, see
# ingenic-lib.mk). The 1.1.0 header lacks the trailing 'fcrop' member of
# IMPFSChnAttr (added in 1.1.2); building with it makes the struct 20 bytes
# short, libimp 1.3.0 then reads stack garbage as the frame-crop and the
# framesource silently delivers NO frames (encoder PollingStream times out
# forever). fs_create() has a compile-time tripwire against this.
IMP_INC ?= $(INC_ROOT)/T23/1.3.0/en
else ifeq ($(PLATFORM),T30)
IMP_INC ?= $(INC_ROOT)/T30/1.0.5/zh
else ifeq ($(PLATFORM),T40)
IMP_INC ?= $(INC_ROOT)/T40/1.2.0/zh
else ifeq ($(PLATFORM),T41)
IMP_INC ?= $(INC_ROOT)/T41/1.2.0/zh
else ifeq ($(PLATFORM),T20)
IMP_INC ?= $(INC_ROOT)/T20/3.12.0/zh
PLATFORM_CFLAGS += -DFQ_MAX_BYTES=1048576   # P-08: see T21 note above
else ifeq ($(PLATFORM),T10)
IMP_INC ?= $(INC_ROOT)/T20/3.12.0/zh
PLATFORM_CFLAGS += -DFQ_MAX_BYTES=1048576   # P-08: see T21 note above
else
IMP_INC ?= $(INC_ROOT)/T31/1.1.6/en
endif

CC  := $(CROSS_COMPILE)gcc
CXX := $(CROSS_COMPILE)g++
BIN := timpsd

# SRT (libsrt) is C++: compile the C sources to .o with gcc (correct C), then
# LINK the final binary with g++ so libstdc++ is resolved and static-linked
# (-static-libstdc++, passed via IMPLIBS) reliably. A single gcc pass left
# libstdc++.so.6 dynamic (-Bstatic / -l:libstdc++.a both did), and a single g++
# pass mis-compiled the C as C++. Non-SRT builds link with gcc.
ifeq ($(USE_SRT),1)
LINK_DRV := $(CXX)
else
LINK_DRV := $(CC)
endif

BASE := src/util.c src/log.c src/config.c src/frame.c src/fanqueue.c src/net.c \
        src/hub.c src/md5.c src/auth.c src/codec/nal.c src/codec/vparam.c src/codec/aac.c src/codec/g711.c \
        src/rtsp/rtp.c src/rtsp/rtsp.c src/mp4/fmp4.c src/mp4/httpd.c src/record.c src/timelapse.c src/srt.c src/main.c \
        $(if $(filter 1,$(USE_TRACE)),src/trace.c)

TARGET_SRC := $(BASE) src/hal/osd_text.c src/hal/msttf.c src/hal/osd_vars.c src/hal/hal_ingenic.c \
              src/hal/imp_osd.c src/hal/imp_motion.c src/control.c src/events.c src/daynight.c \
              $(if $(filter 1,$(USE_SW_ROTATE)),src/hal/nv12_rot.c)
SIM_SRC    := $(BASE) src/hal/osd_text.c src/hal/hal_sim.c src/hal/imp_motion.c src/control.c src/events.c src/daynight.c

# full target source list (+ optional tls.c) and the .o names (unique basenames)
# for the compile-then-link two-step
# USE_BC_AAC implies the backchannel feature; USE_PLAY_OPUS implies the play queue
ifeq ($(USE_BC_AAC),1)
USE_BACKCHANNEL := 1
endif
ifeq ($(USE_PLAY_OPUS),1)
USE_PLAY := 1
endif
# speaker.c (native IMP_AO owner) + the shared resampler are pulled in whenever
# either audio-output producer is built.
USE_AUDIO_OUT :=
ifeq ($(USE_BACKCHANNEL),1)
USE_AUDIO_OUT := 1
endif
ifeq ($(USE_PLAY),1)
USE_AUDIO_OUT := 1
endif
TARGET_ALLSRC := $(TARGET_SRC) $(if $(filter 1,$(USE_TLS)),src/tls.c) \
                 $(if $(filter 1,$(USE_BACKCHANNEL)),src/rtsp/backchannel.c) \
                 $(if $(filter 1,$(USE_AUDIO_OUT)),src/rtsp/speaker.c src/codec/resample.c)
TARGET_OBJS   := $(notdir $(TARGET_ALLSRC:.c=.o))

# --- Build hardening (M14) --------------------------------------------------
# Compiler/linker defense-in-depth for the root-running network daemon. All of
# it flows through two central switches so a stubborn target toolchain can dial
# it back without touching the recipes:
#
#   HARDEN=0    turn off ALL compiler hardening (bare build)
#   FORTIFY=0   keep the rest but drop only _FORTIFY_SOURCE
#
# Notes on the individual flags:
#   * -D_FORTIFY_SOURCE=2 needs an optimising build to do anything; we compile
#     with -Os here, so it is effective. It also needs libc _*_chk wrappers -
#     glibc/musl have them, uClibc's are incomplete (see build.sh), hence the
#     FORTIFY switch.
#   * -fstack-protector-strong needs libssp in the toolchain. The host and the
#     thingino musl toolchain have it; the thingino uClibc toolchain is built
#     --disable-libssp (build.sh disables SSP there and manages this itself).
#   * -Wl,-z,relro -Wl,-z,now (full RELRO) and -Wl,-z,noexecstack (NX stack)
#     are linker-only and safe with -no-pie under both libcs.
#
# build.sh drives the MIPS cross build and passes its own CFLAGS/LDFLAGS (which
# override the defaults below), so it carries an equivalent, libc-aware copy of
# this logic. These defaults cover `make sim` and any direct `make target`.
HARDEN  ?= 1
FORTIFY ?= 1

ifeq ($(HARDEN),1)
HARDEN_CFLAGS  := -fstack-protector-strong $(if $(filter 1,$(FORTIFY)),-D_FORTIFY_SOURCE=2)
HARDEN_LDFLAGS := -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack
else
HARDEN_CFLAGS  :=
HARDEN_LDFLAGS := -Wl,-z,noexecstack
endif

# -Os + gc-sections keeps the binary small; static libimp for a single dropin.
CFLAGS  ?= -std=c11 -D_GNU_SOURCE -Os -Wall -Wextra -Wno-unused-parameter -Wno-misleading-indentation \
           -Wno-stringop-truncation -ffunction-sections -fdata-sections $(HARDEN_CFLAGS)
LDFLAGS ?= -Wl,--gc-sections $(HARDEN_LDFLAGS)
LIBS    ?= -lpthread -lrt -lm

# Version baked into the binary (timps -v / startup log). git-describe for local
# builds; the buildroot package overrides it with VERSION=$(TIMPS_VERSION).
VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo 0.1.0)

# vendor libs: static drop-in by default (adjust to your SDK: alog/sysutils/muslshim)
IMPLIBS ?= -l:libimp.a -l:libalog.a -l:libsysutils.a

# software AAC (libfaac) link flag: static archive by default for a single
# self-contained drop-in; override to shared (e.g. FAACLIB=-lfaac) when building
# against a distro/buildroot that only ships libfaac.so.
FAACLIB ?= -l:libfaac.a

.PHONY: all target sim clean strip test-auth dn-props

all: target

target:
	$(CC) $(CFLAGS) $(if $(SYSROOT),--sysroot=$(SYSROOT)) \
	  $(if $(filter 1,$(USE_FAAC)),-DUSE_FAAC) \
	  $(if $(filter 1,$(USE_CONTROL)),-DUSE_CONTROL) \
	  $(if $(filter 1,$(USE_DAYNIGHT)),-DUSE_DAYNIGHT) \
	  $(if $(filter 1,$(USE_RECORD)),-DUSE_RECORD) \
	  $(if $(filter 1,$(USE_TIMELAPSE)),-DUSE_TIMELAPSE) \
	  $(if $(filter 1,$(USE_TLS)),-DUSE_TLS) \
	  $(if $(filter 1,$(USE_SRT)),-DUSE_SRT) \
	  $(if $(filter 1,$(USE_BACKCHANNEL)),-DUSE_BACKCHANNEL) \
	  $(if $(filter 1,$(USE_BC_AAC)),-DUSE_BC_AAC $(if $(HELIX_INC),-I$(HELIX_INC))) \
	  $(if $(filter 1,$(USE_PLAY)),-DUSE_PLAY) \
	  $(if $(filter 1,$(USE_PLAY_OPUS)),-DUSE_PLAY_OPUS $(if $(OPUS_INC),-I$(OPUS_INC) -I$(OPUS_INC)/opus)) \
	  $(if $(filter 1,$(USE_STREAM_OPUS)),-DUSE_STREAM_OPUS $(if $(OPUS_INC),-I$(OPUS_INC))) \
	  $(if $(filter 1,$(USE_ROTATE)),-DUSE_ROTATE) \
	  $(if $(filter 1,$(USE_SW_ROTATE)),-DMS_ENABLE_SW_ROTATE) \
	  $(if $(filter 1,$(USE_OSD_HINTING)),-DUSE_OSD_HINTING) \
	  $(if $(filter 1,$(USE_TRACE)),-DUSE_TRACE) \
	  -DHAL_INGENIC -DPLATFORM_$(PLATFORM) $(PLATFORM_CFLAGS) -DMS_VERSION='"$(VERSION)"' -Isrc -I$(IMP_INC) -I$(IMP_INC)/imp \
	  -c $(TARGET_ALLSRC)
	$(LINK_DRV) $(TARGET_OBJS) \
	  $(LDFLAGS) $(if $(IMP_LIB),-L$(IMP_LIB)) $(IMPLIBS) \
	  $(if $(filter 1,$(USE_FAAC)),$(FAACLIB)) $(if $(filter 1,$(USE_BC_AAC)),$(HELIXLIB)) \
	  $(if $(filter 1,$(USE_PLAY_OPUS)),$(OPUSLIB)) \
	  $(if $(filter 1,$(USE_STREAM_OPUS)),$(OPUS_ENC_LIB)) $(LIBS) -o $(BIN)
	@rm -f $(TARGET_OBJS)
	@echo "built $(BIN) for $(PLATFORM) (USE_FAAC=$(USE_FAAC) USE_CONTROL=$(USE_CONTROL) USE_DAYNIGHT=$(USE_DAYNIGHT) USE_RECORD=$(USE_RECORD) USE_TIMELAPSE=$(USE_TIMELAPSE) USE_TLS=$(USE_TLS) USE_SRT=$(USE_SRT) USE_BACKCHANNEL=$(USE_BACKCHANNEL) USE_BC_AAC=$(USE_BC_AAC) USE_PLAY=$(USE_PLAY) USE_PLAY_OPUS=$(USE_PLAY_OPUS) USE_STREAM_OPUS=$(USE_STREAM_OPUS) USE_ROTATE=$(USE_ROTATE) USE_SW_ROTATE=$(USE_SW_ROTATE) USE_OSD_HINTING=$(USE_OSD_HINTING) USE_TRACE=$(USE_TRACE))"

sim:
	$(HOSTCC) $(CFLAGS) -DMS_VERSION='"$(VERSION)"' $(if $(filter 1,$(USE_CONTROL)),-DUSE_CONTROL) \
	  $(if $(filter 1,$(USE_DAYNIGHT)),-DUSE_DAYNIGHT) \
	  $(if $(filter 1,$(USE_RECORD)),-DUSE_RECORD) \
	  $(if $(filter 1,$(USE_TIMELAPSE)),-DUSE_TIMELAPSE) \
	  $(if $(filter 1,$(USE_ROTATE)),-DUSE_ROTATE) \
	  $(if $(filter 1,$(USE_TRACE)),-DUSE_TRACE) \
	  $(SIM_CFLAGS) \
	  -Isrc $(SIM_SRC) $(LDFLAGS) -lpthread -lm -o $(BIN)-sim
	@echo "built $(BIN)-sim (host simulation backend, USE_CONTROL=$(USE_CONTROL) USE_DAYNIGHT=$(USE_DAYNIGHT) USE_RECORD=$(USE_RECORD) USE_TIMELAPSE=$(USE_TIMELAPSE) USE_ROTATE=$(USE_ROTATE) USE_TRACE=$(USE_TRACE))"

# Property test for the day/night probe schedule (design-notes section 5).
# dn_next_probe() is pure and dependency-free by construction, so this needs
# no HAL, no config, no threads and no sim binary - just the header and libc.
# That is the entire point of the collapse: the schedule can be interrogated
# with counterfactual evidence a replay can never produce. Runs in well under
# a second; scripts/dn-replay.py runs it as corpus entry 00.
dn-props:
	$(HOSTCC) $(CFLAGS) -Isrc tests/dn-probe-props.c -lm -o dn-probe-props
	@echo "built dn-probe-props (run it, or: ./scripts/dn-replay.py --all scripts/dn-scenarios)"

# Self-contained authentication fail-closed test: build the host sim (with
# /control), start it on unprivileged ports, run scripts/test_auth.sh against
# it, then stop it. Exit code propagates (non-zero if any surface leaked
# unauthenticated access). NOTE: over loopback the HTTP negative tests are
# skipped by design (httpd trusts 127.0.0.0/8) - RTSP is fully exercised here;
# point scripts/test_auth.sh at a real device's LAN IP for the HTTP negatives.
test-auth:
	$(MAKE) sim USE_CONTROL=1
	@echo "== starting $(BIN)-sim for auth test =="
	@./$(BIN)-sim -c scripts/test_auth.conf >/tmp/timps-test-auth-sim.log 2>&1 & \
	 simpid=$$!; \
	 trap 'kill $$simpid 2>/dev/null; wait $$simpid 2>/dev/null' EXIT; \
	 sleep 2; \
	 HOST=127.0.0.1 RTSP_PORT=8554 HTTP_PORT=8880 \
	   RTSP_USER=thingino RTSP_PASS=thingino HTTP_USER=webuser HTTP_PASS=webpass \
	   PATH_MAIN=ch0 scripts/test_auth.sh; \
	 rc=$$?; \
	 kill $$simpid 2>/dev/null; wait $$simpid 2>/dev/null; \
	 exit $$rc

strip: target
	$(CROSS_COMPILE)strip $(BIN)

clean:
	rm -f $(BIN) $(BIN)-sim
