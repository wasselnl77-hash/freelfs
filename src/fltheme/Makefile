# fltheme -- Themes: colour scheme, accent, fonts, window buttons, panel, animations, presets
# SPDX-License-Identifier: MIT

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter
# libflui from the system, or from the tree next to this one (make FLUI=../flui)
FLUI ?=
ifneq ($(FLUI),)
UI_CFLAGS = -I$(FLUI) $(shell pkg-config --cflags wayland-client xkbcommon)
UI_LIBS = -L$(FLUI) -lflui -Wl,-rpath,$(abspath $(FLUI)) $(shell pkg-config --libs wayland-client xkbcommon) -lm
else
UI_CFLAGS = $(shell pkg-config --cflags flui)
UI_LIBS = $(shell pkg-config --libs flui)
endif
PREFIX ?= /usr
DESTDIR ?=

OBJS = fltheme.o presets.o hints.o

all: fltheme

fltheme: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(UI_LIBS)

%.o: %.c fltheme.h
	$(CC) $(CFLAGS) $(UI_CFLAGS) -c -o $@ $<

# Contrast of every colour pair (WCAG), the keys of the presets, the file formats; no compositor needed
check: fltheme tests/contrast
	./tests/contrast
	./tests/presets.sh

# dark -> light -> dark, panel at the top, compact title bar, buttons at the left, in a headless
# flcomp with the real panel; needs flcomp, flpanel and Xwayland (run it in the build chroot)
check-switch: fltheme
	python3 -I tests/switch.py

tests/contrast: tests/contrast.c
	$(CC) $(CFLAGS) -I$(or $(FLUI),../flui) -o $@ $< -lm

install: fltheme
	install -Dm755 fltheme $(DESTDIR)$(PREFIX)/bin/fltheme
	install -Dm644 fltheme.desktop $(DESTDIR)$(PREFIX)/share/applications/fltheme.desktop
	install -Dm644 README $(DESTDIR)$(PREFIX)/share/doc/fltheme/README

clean:
	rm -f fltheme $(OBJS) tests/contrast

.PHONY: all check check-switch install clean
