# flsettings -- theme, panel labels, display, mouse, keyboard, Wi-Fi
# SPDX-License-Identifier: MIT

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter
PKGS = flui wayland-client dbus-1 libpng libjpeg zlib
PREFIX ?= /usr
DESTDIR ?=
# the protocol is flcomp's; the recipe copies it here
PROTO ?= ../flcomp/protocol/fl-shell.xml

OBJS = flsettings.o wifi.o pages.o datetime.o snapshots.o wallpaper.o about.o workspaces.o fl-shell-protocol.o

flsettings: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $$(pkg-config --libs $(PKGS))

fl-shell-client-protocol.h: $(PROTO)
	wayland-scanner client-header $< $@

fl-shell-protocol.c: $(PROTO)
	wayland-scanner private-code $< $@

%.o: %.c fl-shell-client-protocol.h
	$(CC) $(CFLAGS) $$(pkg-config --cflags $(PKGS)) -c -o $@ $<

install: flsettings
	install -Dm755 flsettings $(DESTDIR)$(PREFIX)/bin/flsettings
	install -Dm644 flsettings.desktop $(DESTDIR)$(PREFIX)/share/applications/flsettings.desktop
	install -Dm644 README $(DESTDIR)$(PREFIX)/share/doc/flsettings/README
	install -Dm644 90-flsettings-backlight.rules $(DESTDIR)$(PREFIX)/lib/udev/rules.d/90-flsettings-backlight.rules

# the Workspaces page in a headless flcomp (needs ../flcomp built with tests/shmclient, python3):
# FLCOMP=, FLSETTINGS=, SHMCLIENT= name other programs
check: flsettings
	FLCOMP=$${FLCOMP:-../flcomp/flcomp} FLSETTINGS=$${FLSETTINGS:-./flsettings} python3 -I tests/workspaces.py

clean:
	rm -f flsettings $(OBJS) fl-shell-client-protocol.h fl-shell-protocol.c

.PHONY: install check clean
