# flcomp -- the FreeLFS Wayland compositor
# SPDX-License-Identifier: MIT

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -D_GNU_SOURCE -Wall -Wextra -Wshadow -Wno-unused-parameter

PREFIX ?= /usr
DESTDIR ?=

PKGS = wayland-server libdrm libinput libseat xkbcommon libudev egl gbm glesv2 libpng xcb xcb-xfixes xcb-composite xcb-res
PKG_CFLAGS = $(shell pkg-config --cflags $(PKGS))
PKG_LIBS = $(shell pkg-config --libs $(PKGS)) -lz
SCANNER ?= wayland-scanner
WLPROTO ?= $(shell pkg-config --variable=pkgdatadir wayland-protocols)
XWL_XML = $(WLPROTO)/staging/xwayland-shell/xwayland-shell-v1.xml
XDG_XML = $(WLPROTO)/stable/xdg-shell/xdg-shell.xml
DMABUF_XML = $(WLPROTO)/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml
FL_XML = protocol/fl-shell.xml
SC_XML = protocol/fl-screencopy.xml

GEN = xdg-shell-protocol.h xdg-shell-protocol.c fl-shell-protocol.h fl-shell-protocol.c \
	fl-screencopy-protocol.h fl-screencopy-protocol.c \
	linux-dmabuf-unstable-v1-protocol.h linux-dmabuf-unstable-v1-protocol.c \
	xwayland-shell-protocol.h xwayland-shell-protocol.c
OBJS = main.o render.o surface.o xdg.o seat.o flshell.o drm.o dmabuf.o gl.o config.o output.o media.o osd.o switcher.o shot.o lock.o power.o perf.o theme.o sound.o overview.o screencopy.o xwm.o xwsel.o xwdeco.o \
	xdg-shell-protocol.o fl-shell-protocol.o fl-screencopy-protocol.o linux-dmabuf-unstable-v1-protocol.o xwayland-shell-protocol.o

all: flcomp

flcomp: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(PKG_LIBS)

xdg-shell-protocol.h: $(XDG_XML)
	$(SCANNER) server-header $< $@
xdg-shell-protocol.c: $(XDG_XML)
	$(SCANNER) private-code $< $@
linux-dmabuf-unstable-v1-protocol.h: $(DMABUF_XML)
	$(SCANNER) server-header $< $@
linux-dmabuf-unstable-v1-protocol.c: $(DMABUF_XML)
	$(SCANNER) private-code $< $@
xwayland-shell-protocol.h: $(XWL_XML)
	$(SCANNER) server-header $< $@
xwayland-shell-protocol.c: $(XWL_XML)
	$(SCANNER) private-code $< $@
fl-shell-protocol.h: $(FL_XML)
	$(SCANNER) server-header $< $@
fl-shell-protocol.c: $(FL_XML)
	$(SCANNER) private-code $< $@
fl-screencopy-protocol.h: $(SC_XML)
	$(SCANNER) server-header $< $@
fl-screencopy-protocol.c: $(SC_XML)
	$(SCANNER) private-code $< $@

$(OBJS): comp.h xwm.h xwm-int.h $(GEN)

%.o: %.c
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -c -o $@ $<

# Headless test: starts flcomp with no display, connects tests/shmclient and
# checks the screenshot. No hardware needed.
check: flcomp tests/shmclient tests/xclient
	./tests/run.sh
	./tests/perf.sh
	./tests/x.sh

# the lock screen, the notification surface and power saving (needs python3)
check-lock: flcomp tests/shmclient
	./tests/lock.sh

# workspaces, the overview, screencopy (needs python3)
check-ws: flcomp tests/shmclient
	./tests/ws.sh

# X11 programs through Xwayland: windows, frames, popups, fullscreen, clipboard (needs python3, Xwayland)
check-x: flcomp tests/shmclient tests/xclient
	./tests/x.sh

# performance mode: a fullscreen window drawn alone (also part of `make check`)
check-perf: flcomp tests/shmclient
	./tests/perf.sh

# the keyboard shortcuts: snap, maximize, Alt-Tab switcher, Super+E/T, Print (needs python3)
check-keys: flcomp tests/shmclient
	./tests/keys.sh

tests/xclient: tests/xclient.c
	$(CC) $(CFLAGS) -Wno-unused-result -o $@ $< $(shell pkg-config --cflags --libs xcb)

tests/shmclient: tests/shmclient.c fl-shell-client-protocol.h fl-shell-client-protocol.c \
		fl-screencopy-client-protocol.h fl-screencopy-client-protocol.c \
		xdg-shell-client-protocol.h xdg-shell-client-protocol.c \
		linux-dmabuf-unstable-v1-client-protocol.h linux-dmabuf-unstable-v1-client-protocol.c
	$(CC) $(CFLAGS) -Wno-missing-field-initializers -I. $(shell pkg-config --cflags wayland-client libdrm) -o $@ $< \
		fl-shell-client-protocol.c fl-screencopy-client-protocol.c xdg-shell-client-protocol.c linux-dmabuf-unstable-v1-client-protocol.c \
		$(shell pkg-config --libs wayland-client)

fl-shell-client-protocol.h: $(FL_XML)
	$(SCANNER) client-header $< $@
fl-shell-client-protocol.c: $(FL_XML)
	$(SCANNER) private-code $< $@
fl-screencopy-client-protocol.h: $(SC_XML)
	$(SCANNER) client-header $< $@
fl-screencopy-client-protocol.c: $(SC_XML)
	$(SCANNER) private-code $< $@
linux-dmabuf-unstable-v1-client-protocol.h: $(DMABUF_XML)
	$(SCANNER) client-header $< $@
linux-dmabuf-unstable-v1-client-protocol.c: $(DMABUF_XML)
	$(SCANNER) private-code $< $@
xdg-shell-client-protocol.h: $(XDG_XML)
	$(SCANNER) client-header $< $@
xdg-shell-client-protocol.c: $(XDG_XML)
	$(SCANNER) private-code $< $@

install: flcomp
	install -Dm755 flcomp $(DESTDIR)$(PREFIX)/bin/flcomp
	install -Dm644 protocol/fl-shell.xml $(DESTDIR)$(PREFIX)/share/flcomp/fl-shell.xml
	install -Dm644 protocol/fl-screencopy.xml $(DESTDIR)$(PREFIX)/share/flcomp/fl-screencopy.xml
	install -Dm755 session $(DESTDIR)$(PREFIX)/lib/flcomp/session
	install -Dm644 desktop.sh $(DESTDIR)/etc/profile.d/desktop.sh
	install -Dm644 README $(DESTDIR)$(PREFIX)/share/doc/flcomp/README

clean:
	rm -f $(OBJS) flcomp tests/shmclient tests/xclient $(GEN) \
		fl-shell-client-protocol.[ch] fl-screencopy-client-protocol.[ch] xdg-shell-client-protocol.[ch] \
		linux-dmabuf-unstable-v1-client-protocol.[ch]

.PHONY: all check check-perf check-keys check-lock check-ws check-x install clean
