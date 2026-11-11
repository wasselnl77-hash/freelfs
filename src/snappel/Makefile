# snappel -- the screenshot tool
# SPDX-License-Identifier: MIT

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -Wno-missing-field-initializers
PKGS = flui wayland-client libpng
PREFIX ?= /usr
DESTDIR ?=
# the protocol is flcomp's; the recipe copies it here
PROTO ?= ../flcomp/protocol/fl-screencopy.xml
SCANNER ?= wayland-scanner

OBJS = snappel.o img.o cap.o fl-screencopy-protocol.o

all: snappel

snappel: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $$(pkg-config --libs $(PKGS)) -lm

fl-screencopy-client-protocol.h: $(PROTO)
	$(SCANNER) client-header $< $@
fl-screencopy-protocol.c: $(PROTO)
	$(SCANNER) private-code $< $@

%.o: %.c img.h cap.h fl-screencopy-client-protocol.h
	$(CC) $(CFLAGS) $$(pkg-config --cflags $(PKGS)) -c -o $@ $<

# the annotation tools on in-memory pictures, no display
tests/ops_test: tests/ops_test.c img.c img.h
	$(CC) $(CFLAGS) $$(pkg-config --cflags flui libpng) $(LDFLAGS) -o $@ tests/ops_test.c img.c $$(pkg-config --libs flui libpng) -lm

# the tools, then the real thing against a headless flcomp (../flcomp built; needs python3)
check: snappel tests/ops_test
	./tests/ops_test
	./tests/run.sh

install: snappel
	install -Dm755 snappel $(DESTDIR)$(PREFIX)/bin/snappel
	install -Dm644 snappel.desktop $(DESTDIR)$(PREFIX)/share/applications/snappel.desktop
	install -Dm644 README $(DESTDIR)$(PREFIX)/share/doc/snappel/README

clean:
	rm -f snappel $(OBJS) tests/ops_test fl-screencopy-client-protocol.h fl-screencopy-protocol.c

.PHONY: all check install clean
