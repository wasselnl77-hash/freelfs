# flarchive -- Archives: open, extract and create archives (libarchive)
# SPDX-License-Identifier: MIT

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation
PKGS = flui libarchive
PREFIX ?= /usr
DESTDIR ?=

flarchive: flarchive.c
	$(CC) $(CFLAGS) $$(pkg-config --cflags $(PKGS)) $(LDFLAGS) -o $@ $< $$(pkg-config --libs $(PKGS))

# builds sample archives (zip, tar.gz, tar.zst, 7z when 7z is there) and runs the real program on
# them under a headless desktop: tests/flarchive_test.py (see its header)
check: flarchive
	FLARCHIVE=./flarchive python3 -I tests/flarchive_test.py

install: flarchive
	install -Dm755 flarchive $(DESTDIR)$(PREFIX)/bin/flarchive
	install -Dm644 flarchive.desktop $(DESTDIR)$(PREFIX)/share/applications/flarchive.desktop
	install -Dm644 README $(DESTDIR)$(PREFIX)/share/doc/flarchive/README

clean:
	rm -f flarchive

.PHONY: install check clean
