# flpdf -- the document viewer (PDF, XPS, EPUB, CBZ through MuPDF)
# SPDX-License-Identifier: MIT

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -Wno-clobbered
PKGS = flui mupdf
PREFIX ?= /usr
DESTDIR ?=

flpdf: flpdf.c
	$(CC) $(CFLAGS) $$(pkg-config --cflags $(PKGS)) $(LDFLAGS) -o $@ $< $$(pkg-config --libs $(PKGS))

# headless: opens a generated PDF without a compositor (tests/run.sh; needs ghostscript for the PDF)
check: flpdf
	./tests/run.sh

install: flpdf
	install -Dm755 flpdf $(DESTDIR)$(PREFIX)/bin/flpdf
	install -Dm644 flpdf.desktop $(DESTDIR)$(PREFIX)/share/applications/flpdf.desktop
	install -Dm644 README $(DESTDIR)$(PREFIX)/share/doc/flpdf/README

clean:
	rm -f flpdf

.PHONY: check install clean
