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
USE_TLS       ?= 0          # 1 = HTTPS + RTSPS via mbedTLS (needs -lmbedtls...); off unless the lib is present
USE_SRT       ?= 0          # 1 = MPEG-TS over SRT output via libsrt; off unless the lib is present
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
else ifeq ($(PLATFORM),T10)
IMP_INC ?= $(INC_ROOT)/T20/3.12.0/zh
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
        src/rtsp/rtp.c src/rtsp/rtsp.c src/mp4/fmp4.c src/mp4/httpd.c src/record.c src/timelapse.c src/srt.c src/main.c

TARGET_SRC := $(BASE) src/hal/osd_text.c src/hal/msttf.c src/hal/osd_vars.c src/hal/hal_ingenic.c \
              src/hal/imp_osd.c src/hal/imp_motion.c src/control.c src/events.c src/daynight.c
SIM_SRC    := $(BASE) src/hal/osd_text.c src/hal/hal_sim.c src/hal/imp_motion.c src/control.c src/events.c src/daynight.c

# full target source list (+ optional tls.c) and the .o names (unique basenames)
# for the compile-then-link two-step
TARGET_ALLSRC := $(TARGET_SRC) $(if $(filter 1,$(USE_TLS)),src/tls.c)
TARGET_OBJS   := $(notdir $(TARGET_ALLSRC:.c=.o))

# -Os + gc-sections keeps the binary small; static libimp for a single dropin.
CFLAGS  ?= -std=c11 -D_GNU_SOURCE -Os -Wall -Wextra -Wno-unused-parameter -Wno-misleading-indentation \
           -Wno-stringop-truncation -ffunction-sections -fdata-sections
LDFLAGS ?= -Wl,--gc-sections
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

.PHONY: all target sim clean strip

all: target

target:
	$(CC) $(CFLAGS) $(if $(SYSROOT),--sysroot=$(SYSROOT)) \
	  $(if $(filter 1,$(USE_FAAC)),-DUSE_FAAC) \
	  $(if $(filter 1,$(USE_CONTROL)),-DUSE_CONTROL) \
	  $(if $(filter 1,$(USE_DAYNIGHT)),-DUSE_DAYNIGHT) \
	  $(if $(filter 1,$(USE_TLS)),-DUSE_TLS) \
	  $(if $(filter 1,$(USE_SRT)),-DUSE_SRT) \
	  -DHAL_INGENIC -DPLATFORM_$(PLATFORM) -DMS_VERSION='"$(VERSION)"' -Isrc -I$(IMP_INC) -I$(IMP_INC)/imp \
	  -c $(TARGET_ALLSRC)
	$(LINK_DRV) $(TARGET_OBJS) \
	  $(LDFLAGS) $(if $(IMP_LIB),-L$(IMP_LIB)) $(IMPLIBS) \
	  $(if $(filter 1,$(USE_FAAC)),$(FAACLIB)) $(LIBS) -o $(BIN)
	@rm -f $(TARGET_OBJS)
	@echo "built $(BIN) for $(PLATFORM) (USE_FAAC=$(USE_FAAC) USE_CONTROL=$(USE_CONTROL) USE_DAYNIGHT=$(USE_DAYNIGHT) USE_TLS=$(USE_TLS) USE_SRT=$(USE_SRT))"

sim:
	$(HOSTCC) $(CFLAGS) -DMS_VERSION='"$(VERSION)"' $(if $(filter 1,$(USE_CONTROL)),-DUSE_CONTROL) \
	  $(if $(filter 1,$(USE_DAYNIGHT)),-DUSE_DAYNIGHT) \
	  -Isrc $(SIM_SRC) $(LDFLAGS) -lpthread -lm -o $(BIN)-sim
	@echo "built $(BIN)-sim (host simulation backend, USE_CONTROL=$(USE_CONTROL) USE_DAYNIGHT=$(USE_DAYNIGHT))"

strip: target
	$(CROSS_COMPILE)strip $(BIN)

clean:
	rm -f $(BIN) $(BIN)-sim
