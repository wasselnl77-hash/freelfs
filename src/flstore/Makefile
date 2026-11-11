# flstore -- the Software Store
# SPDX-License-Identifier: MIT

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter
PKGS = flui libxml-2.0 libpng zlib libjpeg libwebp
PREFIX ?= /usr
DESTDIR ?=
SRCS = flstore.c views.c img.c index.c

all: flstore

flstore: $(SRCS) flstore.h store.h
	$(CC) $(CFLAGS) $$(pkg-config --cflags $(PKGS)) $(LDFLAGS) -o $@ $(SRCS) $$(pkg-config --libs $(PKGS))

install: all
	install -Dm755 flstore $(DESTDIR)$(PREFIX)/bin/flstore
	install -Dm644 flstore.desktop $(DESTDIR)$(PREFIX)/share/applications/flstore.desktop
	install -Dm644 README $(DESTDIR)$(PREFIX)/share/doc/flstore/README

# the catalog indexer against a small appstream file: no network, no display
check: flstore
	./tests/index.sh ./flstore

clean:
	rm -f flstore

.PHONY: all install check clean
