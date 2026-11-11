# flcrash -- the "Something went wrong" screen
# SPDX-License-Identifier: MIT

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter
PKGS = flui libseat libinput libdrm
PREFIX ?= /usr
DESTDIR ?=

flcrash: flcrash.c
	$(CC) $(CFLAGS) $$(pkg-config --cflags $(PKGS)) $(LDFLAGS) -o $@ $< $$(pkg-config --libs $(PKGS))

# the egg: paths, console keys, flsh hook (FLSH=..), desktop window (FLCOMP=..); see tests/egg.py
check: flcrash
	python3 -I tests/egg.py

install: flcrash
	install -Dm755 flcrash $(DESTDIR)$(PREFIX)/bin/flcrash
	install -Dm755 flcrash-test $(DESTDIR)$(PREFIX)/bin/flcrash-test
	install -Dm644 README $(DESTDIR)$(PREFIX)/share/doc/flcrash/README

clean:
	rm -f flcrash

.PHONY: check install clean
