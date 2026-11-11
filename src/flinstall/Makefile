# flinstall -- the FreeLFS installer: the window (flinstall) and its privileged helper
# SPDX-License-Identifier: MIT

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation
PKGS = flui dbus-1
PREFIX ?= /usr
DESTDIR ?=
# the Wi-Fi page is flsettings's (wifi.c, wifi.h); the recipe copies it here
WIFI ?= ../flsettings

all: flinstall flinstall-helper

wifi.c wifi.h:
	cp $(WIFI)/$@ $@

flinstall: flinstall.c wifi.c wifi.h common.h
	$(CC) $(CFLAGS) $$(pkg-config --cflags $(PKGS)) $(LDFLAGS) -o $@ flinstall.c wifi.c $$(pkg-config --libs $(PKGS))

flinstall-helper: helper.c common.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ helper.c -lcrypt

# the helper runs as root for the group seat (recipe owner=), like flpower and fltime-set; it
# refuses to touch a disk unless it runs on the live system
install: all
	install -Dm755 flinstall $(DESTDIR)$(PREFIX)/bin/flinstall
	install -Dm755 flinstall-helper $(DESTDIR)$(PREFIX)/libexec/flinstall-helper
	install -Dm644 flinstall.desktop $(DESTDIR)$(PREFIX)/share/applications/flinstall.desktop
	install -Dm644 README $(DESTDIR)$(PREFIX)/share/doc/flinstall/README

# needs a FreeLFS root with sfdisk, mkfs.btrfs, mkfs.fat, mtools and flboot (FLFS_IMAGE_ROOT);
# works on files under SCRATCH only, never on a device
check: flinstall-helper
	tests/image-test.sh $(SCRATCH) ./flinstall-helper

clean:
	rm -f flinstall flinstall-helper wifi.c wifi.h

.PHONY: all install check clean
