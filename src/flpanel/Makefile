# flpanel -- bottom panel and start menu
# SPDX-License-Identifier: MIT

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter
PKGS = flui wayland-client dbus-1
PREFIX ?= /usr
DESTDIR ?=
# the protocol is flcomp's; the recipe copies it here
PROTO ?= ../flcomp/protocol/fl-shell.xml

OBJS = panel.o menu.o flyout.o notify.o battery.o perfmode.o deskicons.o wsmenu.o fl-shell-protocol.o dialog.o ctxmenu.o \
	fileinfo.o quick.o

all: flpanel flnotify flpower

flpanel: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $$(pkg-config --libs $(PKGS))

fl-shell-client-protocol.h: $(PROTO)
	wayland-scanner client-header $< $@

fl-shell-protocol.c: $(PROTO)
	wayland-scanner private-code $< $@

%.o: %.c flpanel.h sound.h fl-shell-client-protocol.h
	$(CC) $(CFLAGS) $$(pkg-config --cflags $(PKGS)) -c -o $@ $<

# the notification command line tool: libdbus only
flnotify: flnotify.c
	$(CC) $(CFLAGS) $$(pkg-config --cflags dbus-1) $(LDFLAGS) -o $@ $< $$(pkg-config --libs dbus-1)

# setuid root, group seat (the recipe's owner= line): off, restart, saver on, saver off
flpower: flpower.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

install: all
	install -Dm755 flpanel $(DESTDIR)$(PREFIX)/bin/flpanel
	install -Dm755 flpower $(DESTDIR)$(PREFIX)/bin/flpower
	install -Dm755 flnotify $(DESTDIR)$(PREFIX)/bin/flnotify
	install -Dm644 README $(DESTDIR)$(PREFIX)/share/doc/flpanel/README

# flnotify and the notification service against a private session bus, headless
# (needs dbus-daemon and python3): tests/notify.sh
check: all
	./tests/power.sh
	./tests/notify.sh
	./tests/pager.sh

clean:
	rm -f flpanel flnotify flpower $(OBJS) fl-shell-client-protocol.h fl-shell-protocol.c

.PHONY: all install check clean
