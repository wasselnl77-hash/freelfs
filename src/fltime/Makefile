# fltime -- automatic time (fl-sntp) and the privileged time helper (fltime-set)
# SPDX-License-Identifier: MIT

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter
PREFIX ?= /usr
DESTDIR ?=

all: fl-sntp fltime-set

fl-sntp: fl-sntp.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

fltime-set: fltime-set.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

# the setuid bit and the group (seat) come from the recipe's owner=
install: all
	install -Dm755 fl-sntp $(DESTDIR)$(PREFIX)/sbin/fl-sntp
	install -Dm755 fltime-set $(DESTDIR)$(PREFIX)/bin/fltime-set
	install -Dm644 README $(DESTDIR)$(PREFIX)/share/doc/fltime/README

clean:
	rm -f fl-sntp fltime-set

.PHONY: all install clean
