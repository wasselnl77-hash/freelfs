# libflui -- the FreeLFS desktop toolkit
# SPDX-License-Identifier: MIT

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -fPIC -fvisibility=hidden
LDFLAGS ?=
PKGS = wayland-client freetype2 harfbuzz fontconfig xkbcommon
PREFIX ?= /usr
LIBDIR ?= $(PREFIX)/lib
DESTDIR ?=
VERSION = 0.1
SOVER = 0

XDG_XML = $(shell pkg-config --variable=pkgdatadir wayland-protocols)/stable/xdg-shell/xdg-shell.xml

SRCS = gfx.c text.c conf.c ui.c kbd.c worldmap.c worldmap_data.c wl.c xdg-shell-protocol.c
OBJS = $(SRCS:.c=.o)
LIB = libflui.so.$(SOVER)

all: $(LIB) libflui.so flui.pc

xdg-shell-client-protocol.h: $(XDG_XML)
	wayland-scanner client-header $< $@

xdg-shell-protocol.c: $(XDG_XML)
	wayland-scanner private-code $< $@

%.o: %.c flui.h internal.h flpalette.h xdg-shell-client-protocol.h
	$(CC) $(CFLAGS) $$(pkg-config --cflags $(PKGS)) -c -o $@ $<

$(LIB): $(OBJS)
	$(CC) -shared -Wl,-soname,$(LIB) $(LDFLAGS) -o $@ $(OBJS) $$(pkg-config --libs $(PKGS)) -lm

flui.pc: flui.pc.in
	sed -e 's|@PREFIX@|$(PREFIX)|' -e 's|@LIBDIR@|$(LIBDIR)|' -e 's|@VERSION@|$(VERSION)|' $< > $@

install: all
	install -Dm755 $(LIB) $(DESTDIR)$(LIBDIR)/$(LIB)
	ln -sf $(LIB) $(DESTDIR)$(LIBDIR)/libflui.so
	install -Dm644 flui.h $(DESTDIR)$(PREFIX)/include/flui.h
	install -Dm644 flpalette.h $(DESTDIR)$(PREFIX)/include/flpalette.h
	install -Dm644 flui.pc $(DESTDIR)$(LIBDIR)/pkgconfig/flui.pc
	install -Dm644 README $(DESTDIR)$(PREFIX)/share/doc/flui/README

clean:
	rm -f $(OBJS) $(LIB) libflui.so flui.pc xdg-shell-client-protocol.h xdg-shell-protocol.c

.PHONY: all install clean

# for building the clients against this tree before it is installed
libflui.so: $(LIB)
	ln -sf $(LIB) $@
