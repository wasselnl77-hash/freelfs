# flauth -- the password prompt for root actions (setuid helper + flui dialog)
# SPDX-License-Identifier: MIT

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter
PREFIX ?= /usr
DESTDIR ?=

all: flauth flauth-dialog

flauth: flauth.c flauth-check.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< -lcrypt

flauth-dialog: flauth-dialog.c
	$(CC) $(CFLAGS) $$(pkg-config --cflags flui xkbcommon) $(LDFLAGS) -o $@ $< $$(pkg-config --libs flui xkbcommon)

# the setuid bit and the group (seat) come from the recipe's owner=
install: all
	install -Dm755 flauth $(DESTDIR)$(PREFIX)/bin/flauth
	install -Dm755 flauth-dialog $(DESTDIR)$(PREFIX)/libexec/flauth-dialog
	install -Dm644 flauth-check.h $(DESTDIR)$(PREFIX)/include/flauth-check.h

clean:
	$(RM) flauth flauth-dialog

.PHONY: all install clean
