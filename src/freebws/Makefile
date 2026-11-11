# FreeBWS -- the FreeLFS web browser (WPE WebKit + libflui)
# SPDX-License-Identifier: MIT

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu17 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -Wno-parentheses \
	-Wno-missing-field-initializers
PKGS = flui wayland-client wpe-webkit-2.0 wpebackend-fdo-1.0 glib-2.0 dssl libsoup-3.0 egl gbm
SRVPKGS = wayland-server
PREFIX ?= /usr
DESTDIR ?=
SCANNER ?= wayland-scanner
WLPROTO ?= $(shell pkg-config --variable=pkgdatadir wayland-protocols)
DMABUF_XML = $(WLPROTO)/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml
GEN = linux-dmabuf-unstable-v1-client-protocol.h linux-dmabuf-unstable-v1-client-protocol.c
OBJS = freebws.o tabstrip.o shm.o abp.o adblock.o config.o bookmarks.o downloads.o pages.o popup.o \
	linux-dmabuf-unstable-v1-client-protocol.o
HDRS = tabstrip.h adblock.h abp.h config.h bookmarks.h downloads.h pages.h popup.h
LISTS = easylist easyprivacy ublock-filters ublock-quick-fixes ublock-unbreak ublock-privacy

all: freebws abpconv

freebws: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $$(pkg-config --libs $(PKGS) $(SRVPKGS)) -lm

freebws.o: freebws.c $(HDRS) $(GEN)
	$(CC) $(CFLAGS) -I. $$(pkg-config --cflags $(PKGS)) -c -o $@ freebws.c
adblock.o: adblock.c $(HDRS)
	$(CC) $(CFLAGS) $$(pkg-config --cflags $(PKGS)) -c -o $@ adblock.c
bookmarks.o: bookmarks.c $(HDRS)
	$(CC) $(CFLAGS) $$(pkg-config --cflags glib-2.0) -c -o $@ bookmarks.c
downloads.o: downloads.c $(HDRS)
	$(CC) $(CFLAGS) $$(pkg-config --cflags $(PKGS)) -c -o $@ downloads.c
pages.o: pages.c $(HDRS)
	$(CC) $(CFLAGS) $$(pkg-config --cflags $(PKGS)) -c -o $@ pages.c
popup.o: popup.c popup.h
	$(CC) $(CFLAGS) $$(pkg-config --cflags flui wayland-client) -c -o $@ popup.c
abp.o: abp.c abp.h
	$(CC) $(CFLAGS) $$(pkg-config --cflags glib-2.0) -c -o $@ abp.c
config.o: config.c config.h
	$(CC) $(CFLAGS) $$(pkg-config --cflags glib-2.0) -c -o $@ config.c
tabstrip.o: tabstrip.c tabstrip.h
	$(CC) $(CFLAGS) $$(pkg-config --cflags flui) -c -o $@ tabstrip.c
shm.o: shm.c
	$(CC) $(CFLAGS) $$(pkg-config --cflags $(SRVPKGS)) -c -o $@ shm.c
linux-dmabuf-unstable-v1-client-protocol.o: linux-dmabuf-unstable-v1-client-protocol.c
	$(CC) $(CFLAGS) -c -o $@ $<

# the list converter on its own (tests, build time checks): no WebKit needed
abpconv: abpconv.c abp.c abp.h
	$(CC) $(CFLAGS) -o $@ abpconv.c abp.c $$(pkg-config --cflags --libs glib-2.0)

linux-dmabuf-unstable-v1-client-protocol.h: $(DMABUF_XML)
	$(SCANNER) client-header $< $@
linux-dmabuf-unstable-v1-client-protocol.c: $(DMABUF_XML)
	$(SCANNER) private-code $< $@

install: freebws abpconv
	install -Dm755 freebws $(DESTDIR)$(PREFIX)/bin/freebws
	install -Dm755 abpconv $(DESTDIR)$(PREFIX)/bin/freebws-abpconv
	install -Dm644 freebws.desktop $(DESTDIR)$(PREFIX)/share/applications/freebws.desktop
	for f in start.html test.html dssl-test.html dssl-native-test.html tabs-test.html adblock-test.html \
		scriptlets.js yt-ads.js count.js ui.css ui.js settings.html bookmarks.html downloads.html error.html; do \
		install -Dm644 data/$$f $(DESTDIR)$(PREFIX)/share/freebws/$$f; done
	install -d $(DESTDIR)$(PREFIX)/share/freebws/lists
	for l in $(LISTS); do \
		xz -dc data/lists/$$l.txt.xz > $(DESTDIR)$(PREFIX)/share/freebws/lists/$$l.txt; done
	install -Dm644 data/lists/lists.conf $(DESTDIR)$(PREFIX)/share/freebws/lists/lists.conf
	install -Dm644 data/lists/COPYING-GPL3 $(DESTDIR)$(PREFIX)/share/freebws/lists/COPYING-GPL3
	install -Dm644 data/lists/LICENSES $(DESTDIR)$(PREFIX)/share/freebws/lists/LICENSES
	install -Dm644 README $(DESTDIR)$(PREFIX)/share/doc/freebws/README

# the baked lists must be the ones whose sums are recorded
check-lists:
	cd data/lists && for l in $(LISTS); do xz -dc $$l.txt.xz | sha256sum | sed "s/-$$/$$l.txt/"; done | diff - SHA256SUMS

clean:
	rm -f freebws abpconv $(OBJS) $(GEN)

.PHONY: all install clean check-lists
