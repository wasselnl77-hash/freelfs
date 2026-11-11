# flgm -- General Manager, the task manager
# SPDX-License-Identifier: MIT

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation
PKGS = flui
PREFIX ?= /usr
DESTDIR ?=

OBJS = gm.o perf.o sys.o proc.o startup.o

all: general-manager

general-manager: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $$(pkg-config --libs $(PKGS)) -lm

%.o: %.c gm.h gmui.h
	$(CC) $(CFLAGS) $$(pkg-config --cflags $(PKGS)) -c -o $@ $<

install: all
	install -Dm755 general-manager $(DESTDIR)$(PREFIX)/bin/general-manager
	install -Dm644 general-manager.desktop $(DESTDIR)$(PREFIX)/share/applications/general-manager.desktop
	install -Dm644 README $(DESTDIR)$(PREFIX)/share/doc/general-manager/README

# the data layer against the host's /proc and a scratch autostart directory, then the window
# rendered headless; the script says what it needs
check: all
	./tests/check.sh

clean:
	rm -f general-manager $(OBJS)

.PHONY: all install check clean
