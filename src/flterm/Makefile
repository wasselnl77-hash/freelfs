# flterm -- the FreeLFS terminal
# SPDX-License-Identifier: MIT

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter
PKGS = flui
PREFIX ?= /usr
DESTDIR ?=

OBJS = main.o term.o

flterm: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $$(pkg-config --libs $(PKGS)) -lutil

%.o: %.c flterm.h
	$(CC) $(CFLAGS) $$(pkg-config --cflags $(PKGS)) -c -o $@ $<

install: flterm
	install -Dm755 flterm $(DESTDIR)$(PREFIX)/bin/flterm
	install -Dm644 flterm.desktop $(DESTDIR)$(PREFIX)/share/applications/flterm.desktop
	install -Dm644 README $(DESTDIR)$(PREFIX)/share/doc/flterm/README

clean:
	rm -f flterm $(OBJS)

.PHONY: install clean
