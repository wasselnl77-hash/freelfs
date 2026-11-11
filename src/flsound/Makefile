# flsound -- the FreeLFS system sounds (synthesized, played through PipeWire)
# SPDX-License-Identifier: MIT

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter
PKGS = libpipewire-0.3
PREFIX ?= /usr
DESTDIR ?=

flsound: flsound.c
	$(CC) $(CFLAGS) $$(pkg-config --cflags $(PKGS)) $(LDFLAGS) -o $@ $< $$(pkg-config --libs $(PKGS)) -lm

# no PipeWire needed: writes the five sounds as WAV files and checks their length, level and ends
check: flsound
	./tests/check.py ./flsound

install: flsound
	install -Dm755 flsound $(DESTDIR)$(PREFIX)/bin/flsound
	install -Dm644 README $(DESTDIR)$(PREFIX)/share/doc/flsound/README

clean:
	rm -f flsound

.PHONY: check install clean
