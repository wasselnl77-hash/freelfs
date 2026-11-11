# flgreet -- the login screen
# SPDX-License-Identifier: MIT

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter
PKGS = flui
PREFIX ?= /usr
DESTDIR ?=

all: flgreet flgreet-ui

# flgreet runs as root: no toolkit, no extra libraries
flgreet: flgreet.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

flgreet-ui: flgreet-ui.c
	$(CC) $(CFLAGS) $$(pkg-config --cflags $(PKGS)) $(LDFLAGS) -o $@ $< $$(pkg-config --libs $(PKGS)) -lcrypt

install: all
	install -Dm755 flgreet $(DESTDIR)$(PREFIX)/sbin/flgreet
	install -Dm755 flgreet-ui $(DESTDIR)$(PREFIX)/libexec/flgreet-ui
	install -Dm644 README $(DESTDIR)$(PREFIX)/share/doc/flgreet/README

clean:
	rm -f flgreet flgreet-ui

.PHONY: all install clean
