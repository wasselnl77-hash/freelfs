# flfiles -- the file manager
# SPDX-License-Identifier: MIT

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation
PKGS = flui
PREFIX ?= /usr
DESTDIR ?=

flfiles: flfiles.c
	$(CC) $(CFLAGS) $$(pkg-config --cflags $(PKGS)) $(LDFLAGS) -o $@ $< $$(pkg-config --libs $(PKGS))

install: flfiles
	install -Dm755 flfiles $(DESTDIR)$(PREFIX)/bin/flfiles
	install -Dm644 flfiles.desktop $(DESTDIR)$(PREFIX)/share/applications/flfiles.desktop
	install -Dm644 mimeapps.list $(DESTDIR)$(PREFIX)/share/applications/mimeapps.list
	install -Dm644 README $(DESTDIR)$(PREFIX)/share/doc/flfiles/README

clean:
	rm -f flfiles

.PHONY: install clean
