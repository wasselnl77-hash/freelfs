# flsnap -- btrfs snapshots of the root file system
# SPDX-License-Identifier: MIT

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -D_GNU_SOURCE -Wall -Wextra -Wshadow
PREFIX ?= /usr
DESTDIR ?=

all: flsnap

flsnap: flsnap.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

check: flsnap
	./tests/test-flsnap.sh

# flsnap is setuid root for the group seat (the recipe's owner=), like fltime-set
install: flsnap
	install -Dm755 flsnap $(DESTDIR)$(PREFIX)/bin/flsnap
	install -Dm755 flsnap-notice $(DESTDIR)$(PREFIX)/bin/flsnap-notice
	install -Dm755 hook-pre-transaction $(DESTDIR)$(PREFIX)/libexec/flpkg/pre-transaction.d/10-flsnap
	install -Dm644 flsnap.conf $(DESTDIR)/etc/flsnap.conf
	install -Dm644 flsnap-notice.sh $(DESTDIR)/etc/profile.d/flsnap-notice.sh
	install -Dm644 README $(DESTDIR)$(PREFIX)/share/doc/flsnap/README

clean:
	rm -f flsnap

.PHONY: all check install clean
