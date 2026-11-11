# flsplash -- the boot screen
# SPDX-License-Identifier: MIT
#
# genart renders the logo layers (flsplash.dat) once at build time; flsplash
# has them built in. Needs FreeType and zlib to build, zlib to run.

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu17 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter
PKGS = freetype2
PREFIX ?= /usr
DESTDIR ?=

# the wallpaper's 3D F in the other accent colours (flsettings > Display > Wallpaper); green is
# wallpaper.flw itself. Same ids and dark-theme colours as fl_accents in libflui.
WALLS = teal:35a09b blue:4d8fd0 amber:d0a032 orange:e0793f rose:d4678c
WALLFILES = $(foreach w,$(WALLS),wallpaper-$(word 1,$(subst :, ,$(w))).flw)

all: flsplash $(WALLFILES)

genart: genart.c
	$(CC) $(CFLAGS) $$(pkg-config --cflags $(PKGS)) $(LDFLAGS) -o $@ $< $$(pkg-config --libs $(PKGS)) -lz -lm

flsplash.dat: genart
	./genart -o $@ -w wallpaper.flw

wallpaper-%.flw: genart
	./genart -W -a $(word 2,$(subst :, ,$(filter $*:%,$(WALLS)))) -w $@

flsplash: flsplash.c flsplash.dat
	$(CC) $(CFLAGS) -DARTFILE='"flsplash.dat"' $(LDFLAGS) -o $@ flsplash.c -lz -lm

install: all
	install -Dm755 flsplash $(DESTDIR)$(PREFIX)/sbin/flsplash
	install -Dm644 wallpaper.flw $(DESTDIR)$(PREFIX)/share/flcomp/wallpaper.flw
	for f in $(WALLFILES); do install -Dm644 $$f $(DESTDIR)$(PREFIX)/share/flcomp/$$f; done
	install -Dm644 README $(DESTDIR)$(PREFIX)/share/doc/flsplash/README

clean:
	$(RM) flsplash genart flsplash.dat wallpaper.flw $(WALLFILES)

.PHONY: all install clean
