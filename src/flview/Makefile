# flview -- the image viewer
# SPDX-License-Identifier: MIT

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation
PKGS = flui libpng libjpeg libwebp libwebpdemux
PREFIX ?= /usr
DESTDIR ?=

flview: flview.c
	$(CC) $(CFLAGS) $$(pkg-config --cflags $(PKGS)) $(LDFLAGS) -o $@ $< $$(pkg-config --libs $(PKGS)) -lm

# headless: decodes a generated folder, drives the viewer to a PPM, no compositor (tests/run.sh)
check: flview
	./tests/run.sh

install: flview
	install -Dm755 flview $(DESTDIR)$(PREFIX)/bin/flview
	install -Dm644 flview.desktop $(DESTDIR)$(PREFIX)/share/applications/flview.desktop
	install -Dm644 README $(DESTDIR)$(PREFIX)/share/doc/flview/README

clean:
	rm -f flview

.PHONY: check install clean
