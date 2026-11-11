# fledit -- the text editor
# SPDX-License-Identifier: MIT

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation
PKGS = flui
PREFIX ?= /usr
DESTDIR ?=

OBJS = fledit.o syntax.o

fledit: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $$(pkg-config --libs $(PKGS))

fledit.o: fledit.c syntax.h
	$(CC) $(CFLAGS) $$(pkg-config --cflags $(PKGS)) -c -o $@ $<

syntax.o: syntax.c syntax.h
	$(CC) $(CFLAGS) -c -o $@ $<

# the line lexers, no display needed (SAN=1: under AddressSanitizer)
comma := ,
SANFLAGS = $(if $(SAN),-fsanitize=address$(comma)undefined)

tests/syntax_test: tests/syntax_test.c syntax.c syntax.h
	$(CC) $(CFLAGS) $(SANFLAGS) -o $@ tests/syntax_test.c syntax.c

check: tests/syntax_test
	tests/syntax_test

install: fledit
	install -Dm755 fledit $(DESTDIR)$(PREFIX)/bin/fledit
	install -Dm644 fledit.desktop $(DESTDIR)$(PREFIX)/share/applications/fledit.desktop
	install -Dm644 README $(DESTDIR)$(PREFIX)/share/doc/fledit/README

clean:
	rm -f fledit $(OBJS) tests/syntax_test

.PHONY: install check clean
