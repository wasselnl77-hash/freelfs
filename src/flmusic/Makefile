# flmusic -- the music player
# SPDX-License-Identifier: MIT

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation
PKGS = flui gstreamer-1.0 gstreamer-pbutils-1.0 dbus-1
PREFIX ?= /usr
DESTDIR ?=

OBJS = flmusic.o mpris.o

flmusic: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $$(pkg-config --libs $(PKGS))

%.o: %.c mpris.h
	$(CC) $(CFLAGS) $$(pkg-config --cflags $(PKGS)) -c -o $@ $<

# headless: synthesised audio, a private session bus, MPRIS driven by flmusic --ctl (tests/run.sh)
check: flmusic
	./tests/run.sh

install: flmusic
	install -Dm755 flmusic $(DESTDIR)$(PREFIX)/bin/flmusic
	install -Dm644 flmusic.desktop $(DESTDIR)$(PREFIX)/share/applications/flmusic.desktop
	install -Dm644 README $(DESTDIR)$(PREFIX)/share/doc/flmusic/README

clean:
	rm -f flmusic $(OBJS)

.PHONY: check install clean
