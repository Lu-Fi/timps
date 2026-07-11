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
IMP_INC ?= $(INC_ROOT)/T23/1.1.0/zh
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
BIN := timpsd

BASE := src/util.c src/log.c src/config.c src/frame.c src/fanqueue.c src/net.c \
        src/hub.c src/md5.c src/auth.c src/codec/nal.c src/codec/vparam.c src/codec/aac.c src/codec/g711.c \
        src/rtsp/rtp.c src/rtsp/rtsp.c src/mp4/fmp4.c src/mp4/httpd.c src/main.c

TARGET_SRC := $(BASE) src/hal/osd_text.c src/hal/msttf.c src/hal/osd_vars.c src/hal/hal_ingenic.c \
              src/hal/imp_osd.c src/hal/imp_motion.c src/control.c src/daynight.c
SIM_SRC    := $(BASE) src/hal/osd_text.c src/hal/hal_sim.c src/control.c src/daynight.c

# -Os + gc-sections keeps the binary small; static libimp for a single dropin.
CFLAGS  ?= -std=c11 -D_GNU_SOURCE -Os -Wall -Wextra -Wno-unused-parameter -Wno-misleading-indentation \
           -Wno-stringop-truncation -ffunction-sections -fdata-sections
LDFLAGS ?= -Wl,--gc-sections
LIBS    ?= -lpthread -lrt -lm

# vendor libs: static drop-in by default (adjust to your SDK: alog/sysutils/muslshim)
IMPLIBS ?= -l:libimp.a -l:libalog.a -l:libsysutils.a

.PHONY: all target sim clean strip

all: target

target:
	$(CC) $(CFLAGS) $(if $(SYSROOT),--sysroot=$(SYSROOT)) \
	  $(if $(filter 1,$(USE_FAAC)),-DUSE_FAAC) \
	  $(if $(filter 1,$(USE_CONTROL)),-DUSE_CONTROL) \
	  $(if $(filter 1,$(USE_DAYNIGHT)),-DUSE_DAYNIGHT) \
	  -DHAL_INGENIC -DPLATFORM_$(PLATFORM) -Isrc -I$(IMP_INC) \
	  $(TARGET_SRC) $(LDFLAGS) $(if $(IMP_LIB),-L$(IMP_LIB)) $(IMPLIBS) \
	  $(if $(filter 1,$(USE_FAAC)),-l:libfaac.a) $(LIBS) -o $(BIN)
	@echo "built $(BIN) for $(PLATFORM) (USE_FAAC=$(USE_FAAC) USE_CONTROL=$(USE_CONTROL) USE_DAYNIGHT=$(USE_DAYNIGHT))"

sim:
	$(HOSTCC) $(CFLAGS) $(if $(filter 1,$(USE_CONTROL)),-DUSE_CONTROL) \
	  $(if $(filter 1,$(USE_DAYNIGHT)),-DUSE_DAYNIGHT) \
	  -Isrc $(SIM_SRC) $(LDFLAGS) -lpthread -lm -o $(BIN)-sim
	@echo "built $(BIN)-sim (host simulation backend, USE_CONTROL=$(USE_CONTROL) USE_DAYNIGHT=$(USE_DAYNIGHT))"

strip: target
	$(CROSS_COMPILE)strip $(BIN)

clean:
	rm -f $(BIN) $(BIN)-sim
