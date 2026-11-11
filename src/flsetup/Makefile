# flsetup -- first boot keyboard choice
# SPDX-License-Identifier: MIT

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter
PKGS = flui
PREFIX ?= /usr
DESTDIR ?=

flsetup: flsetup.c
	$(CC) $(CFLAGS) $$(pkg-config --cflags $(PKGS)) $(LDFLAGS) -o $@ $< $$(pkg-config --libs $(PKGS))

install: flsetup
	install -Dm755 flsetup $(DESTDIR)$(PREFIX)/bin/flsetup
	install -Dm644 README $(DESTDIR)$(PREFIX)/share/doc/flsetup/README

clean:
	rm -f flsetup

.PHONY: install clean
