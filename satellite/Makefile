# Cross-build for the Echo with the Buildroot toolchain of this repository.
# Run inside the build container (docker/build.sh make -C satellite) or
# directly on the host if the toolchain binaries run there.
#
# Headers come from the Buildroot sysroot. The libraries are the ones in the
# image (rootfs/out): PulseAudio, D-Bus and curl there are Amazon's builds,
# older than Buildroot's, and libshmbuffer exists nowhere else.

TOP     := ..
HOST    := $(TOP)/rootfs/br2echo/host/usr
CC      := $(HOST)/bin/arm-buildroot-linux-gnueabi-gcc
SYSROOT := $(HOST)/arm-buildroot-linux-gnueabi/sysroot
IMG     := $(TOP)/rootfs/out

CFLAGS  := -std=gnu99 -O2 -g -Wall -Wextra -Wshadow -Wformat=2 \
           -I$(SYSROOT)/usr/include/dbus-1.0 -I$(SYSROOT)/usr/lib/dbus-1.0/include
LDFLAGS := -Wl,-rpath-link,$(IMG)/usr/lib -Wl,-rpath-link,$(IMG)/lib \
           -Wl,-rpath-link,$(IMG)/usr/lib/pulseaudio -Wl,-rpath-link,$(IMG)/usr/local/lib \
           -Wl,-rpath,/usr/local/lib
LIBS    := $(IMG)/usr/lib/libdbus-1.so.3 $(IMG)/usr/lib/libpulse-simple.so.0 \
           $(IMG)/usr/lib/libpulse.so.0 $(IMG)/usr/lib/libcurl.so.4 \
           $(IMG)/usr/local/lib/libshmbuffer.so -lpthread -lm

OBJS := main.o api.o pb.o bus.o mic.o player.o

all: echo-satellite

echo-satellite: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

%.o: %.c sat.h
	$(CC) $(CFLAGS) -c -o $@ $<

# Diagnostic programs for the device, not part of the image.
tools: tools/sndctl

tools/sndctl: tools/sndctl.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(IMG)/usr/lib/libasound.so.2 -lpthread -lm -ldl -lrt

clean:
	rm -f echo-satellite $(OBJS) tools/sndctl

.PHONY: all clean tools
