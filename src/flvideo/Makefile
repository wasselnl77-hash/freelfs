# flvideo -- the video player (libmpv, software render API)
# SPDX-License-Identifier: MIT

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation
PKGS = flui mpv
PREFIX ?= /usr
DESTDIR ?=

flvideo: flvideo.c
	$(CC) $(CFLAGS) $$(pkg-config --cflags $(PKGS)) $(LDFLAGS) -o $@ $< $$(pkg-config --libs $(PKGS)) -lm

# headless: plays a generated clip into a PPM without a compositor or sound card (tests/run.sh;
# needs the ffmpeg tool for the clip)
check: flvideo
	./tests/run.sh

install: flvideo
	install -Dm755 flvideo $(DESTDIR)$(PREFIX)/bin/flvideo
	install -Dm644 flvideo.desktop $(DESTDIR)$(PREFIX)/share/applications/flvideo.desktop
	install -Dm644 README $(DESTDIR)$(PREFIX)/share/doc/flvideo/README

clean:
	rm -f flvideo

.PHONY: check install clean
