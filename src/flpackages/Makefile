# flpackages -- the package app, and flpkg-seat, its setuid helper
# SPDX-License-Identifier: MIT

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter
PKGS = flui
PREFIX ?= /usr
DESTDIR ?=

all: flpackages flpkg-seat

flpackages: flpackages.c
	$(CC) $(CFLAGS) $$(pkg-config --cflags $(PKGS)) $(LDFLAGS) -o $@ $< $$(pkg-config --libs $(PKGS))

flpkg-seat: flpkg-seat.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

# the recipe sets flpkg-seat to root:seat 4750 (owner=)
install: all
	install -Dm755 flpackages $(DESTDIR)$(PREFIX)/bin/flpackages
	install -Dm755 flpkg-seat $(DESTDIR)$(PREFIX)/bin/flpkg-seat
	install -Dm644 flpackages.desktop $(DESTDIR)$(PREFIX)/share/applications/flpackages.desktop
	install -Dm644 README $(DESTDIR)$(PREFIX)/share/doc/flpackages/README

check: flpkg-seat
	./tests/seat.sh

clean:
	rm -f flpackages flpkg-seat

.PHONY: all install check clean
