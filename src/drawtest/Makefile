# fl-drawtest -- M6: EGL on KMS through seatd
# SPDX-License-Identifier: MIT

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -Wall -Wextra -Wno-unused-parameter
PKGS = libdrm gbm egl glesv2 libseat
PREFIX ?= /usr
DESTDIR ?=

fl-drawtest: drawtest.c
	$(CC) $(CFLAGS) $$(pkg-config --cflags $(PKGS)) $(LDFLAGS) -o $@ $< $$(pkg-config --libs $(PKGS))

install: fl-drawtest
	install -Dm755 fl-drawtest $(DESTDIR)$(PREFIX)/bin/fl-drawtest

clean:
	rm -f fl-drawtest

.PHONY: install clean
