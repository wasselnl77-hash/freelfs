# fldisplay -- list and change display outputs through flcomp (fl_shell 9)
# SPDX-License-Identifier: MIT

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu17 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type
PREFIX ?= /usr
DESTDIR ?=
SCANNER ?= wayland-scanner
XML = ../flcomp/protocol/fl-shell.xml
PKGS = wayland-client

all: fldisplay

fl-shell-client-protocol.h: $(XML)
	$(SCANNER) client-header $< $@

fl-shell-client-protocol.c: $(XML)
	$(SCANNER) private-code $< $@

fldisplay: fldisplay.c fl-shell-client-protocol.h fl-shell-client-protocol.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $(shell pkg-config --cflags $(PKGS)) $(LDFLAGS) -o $@ fldisplay.c fl-shell-client-protocol.c $(shell pkg-config --libs $(PKGS))

# list / set / revert / nightlight against a headless flcomp with two outputs
check: fldisplay
	sh tests/check.sh

install: all
	install -Dm755 fldisplay $(DESTDIR)$(PREFIX)/bin/fldisplay

clean:
	$(RM) fldisplay fl-shell-client-protocol.h fl-shell-client-protocol.c

.PHONY: all check install clean
