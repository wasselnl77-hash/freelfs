# flinitrd -- the init of the FreeLFS live initramfs. One static program.
# SPDX-License-Identifier: MIT
CC ?= cc
CFLAGS ?= -O2
CFLAGS += -std=gnu17 -Wall -Wextra -Wno-unused-parameter

all: flinitrd

flinitrd: flinitrd.c
	$(CC) $(CFLAGS) -static -o $@ $<
	strip $@

install: flinitrd
	install -Dm755 flinitrd $(DESTDIR)/usr/lib/flinitrd/init
	install -Dm644 README $(DESTDIR)/usr/share/doc/flinitrd/README

clean:
	rm -f flinitrd

.PHONY: all install clean
