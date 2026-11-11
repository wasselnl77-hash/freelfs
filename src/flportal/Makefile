# flportal -- the xdg-desktop-portal backend of FreeLFS
# SPDX-License-Identifier: MIT

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation
PKGS = flui gio-2.0 gio-unix-2.0
PREFIX ?= /usr
DESTDIR ?=

OBJS = main.o filechooser.o appchooser.o ui.o
SERVICE = org.freedesktop.impl.portal.desktop.freelfs.service

all: flportal $(SERVICE)

flportal: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $$(pkg-config --libs $(PKGS)) $(LDLIBS)

%.o: %.c flportal.h
	$(CC) $(CFLAGS) $$(pkg-config --cflags $(PKGS)) -c -o $@ $<

$(SERVICE): $(SERVICE).in
	sed -e 's|@PREFIX@|$(PREFIX)|' $< > $@

install: all
	install -Dm755 flportal $(DESTDIR)$(PREFIX)/libexec/flportal
	install -Dm644 freelfs.portal $(DESTDIR)$(PREFIX)/share/xdg-desktop-portal/portals/freelfs.portal
	install -Dm644 freelfs-portals.conf $(DESTDIR)$(PREFIX)/share/xdg-desktop-portal/freelfs-portals.conf
	install -Dm644 freelfs-portals.conf $(DESTDIR)$(PREFIX)/share/xdg-desktop-portal/portals.conf
	install -Dm644 $(SERVICE) $(DESTDIR)$(PREFIX)/share/dbus-1/services/$(SERVICE)
	install -Dm644 README $(DESTDIR)$(PREFIX)/share/doc/flportal/README

# The backend on a private session bus with the real xdg-desktop-portal in front of it, and
# the dialogs rendered headless (borrows libraries and the frontend from a built root file
# system, ROOTFS=...): tests/portal.sh
check: all
	./tests/portal.sh

clean:
	rm -f flportal $(OBJS) $(SERVICE)

.PHONY: all install check clean
