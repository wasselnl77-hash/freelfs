# fllock -- the lock screen, and fllock-check, its password checker
# SPDX-License-Identifier: MIT

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter
PKGS = flui wayland-client
PREFIX ?= /usr
DESTDIR ?=
# the protocol is flcomp's; the recipe copies it here
PROTO ?= ../flcomp/protocol/fl-shell.xml

all: fllock fllock-check

fllock: fllock.o fl-shell-protocol.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $$(pkg-config --libs $(PKGS))

# the checker needs libcrypt and nothing else; it runs as root
fllock-check: fllock-check.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< -lcrypt

fl-shell-client-protocol.h: $(PROTO)
	wayland-scanner client-header $< $@

fl-shell-protocol.c: $(PROTO)
	wayland-scanner private-code $< $@

fllock.o: fllock.c fl-shell-client-protocol.h
	$(CC) $(CFLAGS) $$(pkg-config --cflags $(PKGS)) -c -o $@ $<

fl-shell-protocol.o: fl-shell-protocol.c
	$(CC) $(CFLAGS) $$(pkg-config --cflags $(PKGS)) -c -o $@ $<

# fllock-check is setuid root, group seat (4750): the recipe's owner= line does that when the
# package is installed; here it only gets mode 755
install: all
	install -Dm755 fllock $(DESTDIR)$(PREFIX)/bin/fllock
	install -Dm755 fllock-check $(DESTDIR)$(PREFIX)/libexec/fllock-check
	install -Dm644 README $(DESTDIR)$(PREFIX)/share/doc/fllock/README

# the checker against a made-up shadow file (no root, no real passwords)
check: fllock-check
	./tests/check.sh

# fllock end to end under a headless flcomp (needs ../flcomp built, python3, openssl)
e2e: all
	./tests/e2e.sh

clean:
	rm -f fllock fllock-check fllock.o fl-shell-protocol.o fl-shell-client-protocol.h fl-shell-protocol.c

.PHONY: all install check e2e clean
