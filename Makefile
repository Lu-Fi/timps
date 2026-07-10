# ============================================================================
# timps Makefile
#
# Cross-compile for Ingenic SoC cameras (MIPS32r2, uClibc/musl).
#
# Usage:
#   make                         — build with default (mips-linux-uclibc-gnu-)
#   make CROSS_COMPILE=mips-linux-gnu-  CFLAGS_EXTRA=-muclibc
#   make CROSS_COMPILE=mipsel-linux-    # T41 (little-endian)
#   make HOST=1                  — native host build (no SDK, for CI checks)
#
# Required variables:
#   SDK     — path to the Ingenic SDK root (contains include/ and lib/)
#             Defaults to ./sdk
#             Headers expected at:  $(SDK)/include/imp/*.h
#             Libraries expected at: $(SDK)/lib/uclibc/libimp.so
#                                    $(SDK)/lib/uclibc/libalog.so
# ============================================================================

PROG        := timps
SDK         ?= ./sdk
CROSS_COMPILE ?= mips-linux-uclibc-gnu-

ifeq ($(HOST),1)
CC          := gcc
STRIP       := strip
CFLAGS_ARCH :=
SDK_INC     := -I$(SDK)/include
SDK_LIBS    :=
CFLAGS_EXTRA += -DTIMPS_HOST_BUILD
else
CC          := $(CROSS_COMPILE)gcc
STRIP       := $(CROSS_COMPILE)strip
CFLAGS_ARCH := -march=mips32r2 -mfp32
SDK_INC     := -I$(SDK)/include
SDK_LIBS    := $(SDK)/lib/uclibc/libimp.so \
               $(SDK)/lib/uclibc/libalog.so
endif

# Optional: link against libfaac for AAC encoding
#  FAAC_CFLAGS := -DHAVE_FAAC $(shell pkg-config --cflags faac)
#  FAAC_LIBS   := $(shell pkg-config --libs faac)

CFLAGS   := -O2 -Wall -Wextra -Wno-unused-parameter \
            $(CFLAGS_ARCH) $(SDK_INC) $(CFLAGS_EXTRA)
LDFLAGS  := -lpthread -lm -lrt -ldl

SRCS := src/main.c    \
        src/config.c  \
        src/auth.c    \
        src/rtp.c     \
        src/fmp4.c    \
        src/stream.c  \
        src/encoder.c \
        src/audio.c   \
        src/imp_init.c\
        src/osd.c     \
        src/motion.c  \
        src/rtsp.c    \
        src/http.c

OBJS := $(SRCS:src/%.c=build/%.o)

.PHONY: all clean strip install

all: $(PROG)

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -c $< -o $@

$(PROG): $(OBJS)
	$(CC) $(CFLAGS_ARCH) $(CFLAGS_EXTRA) -o $@ $^ \
	    $(SDK_LIBS) $(LDFLAGS)

strip: $(PROG)
	$(STRIP) -s $(PROG)

build:
	mkdir -p build

install: $(PROG)
	install -D -m 755 $(PROG) $(DESTDIR)/usr/sbin/$(PROG)
	install -D -m 644 timps.conf.example \
	    $(DESTDIR)/etc/timps.conf.example

clean:
	rm -rf build $(PROG)
