# flboot -- the FreeLFS UEFI boot loader
# SPDX-License-Identifier: MIT
#
# Built with the ordinary Linux toolchain: compiled as position-independent
# freestanding code, linked as an ELF shared object at address 0 with our
# own linker script, then converted to a PE image by objcopy.

ARCH ?= $(shell uname -m)
CC ?= cc
OBJCOPY ?= objcopy
PREFIX ?= /usr
DESTDIR ?=

CFLAGS = -O2 -g -std=c17 -Wall -Wextra -Wno-unused-parameter \
	-ffreestanding -fpic -fshort-wchar -fno-stack-protector -fno-strict-aliasing \
	-fno-stack-check -fno-asynchronous-unwind-tables -fvisibility=hidden
LDFLAGS = -nostdlib -shared -Bsymbolic -znocombreloc -znorelro -T efi-$(ARCH).lds

ifeq ($(ARCH),x86_64)
CFLAGS += -mno-red-zone -maccumulate-outgoing-args
EFI_TARGET = efi-app-x86_64
EFI_NAME = BOOTX64.EFI
endif

# The menu font: Terminus (SIL Open Font License), from kbd.
FONT ?= /usr/share/consolefonts/Lat2-Terminus16.psfu.gz

OBJS = start.o main.o lib.o fs.o conf.o gfx.o menu.o boot.o font.o

all: flboot.efi

flboot.so: $(OBJS) efi-$(ARCH).lds
	$(CC) $(LDFLAGS) -o $@ $(OBJS) -lgcc

flboot.efi: flboot.so
	$(OBJCOPY) -j .text -j .reloc -j .data -j .dynamic -j .rela -j .dynsym -j .dynstr \
		-O $(EFI_TARGET) --subsystem=10 $< $@
	@bad=$$(readelf -rW $< | awk '/^[0-9a-f]+ +[0-9a-f]+ / && $$3 !~ /_RELATIVE$$/'); \
		[ -z "$$bad" ] || { echo "flboot.so needs relocations flboot cannot apply:" >&2; \
			echo "$$bad" >&2; rm -f $@; exit 1; }

font.c: mkfont.py $(FONT)
	python3 mkfont.py $(FONT) > $@

%.o: %.c efi.h flboot.h
	$(CC) $(CFLAGS) -c -o $@ $<

# the real menu code on a stand-in firmware (no UEFI needed); SHOTS=DIR keeps screenshots
check:
	FONT=$(FONT) tests/menu-test.sh $(SHOTS)

install: flboot.efi
	install -Dm644 flboot.efi $(DESTDIR)$(PREFIX)/lib/flboot/flboot.efi
	install -Dm755 flboot-install $(DESTDIR)$(PREFIX)/bin/flboot-install
	install -Dm644 README $(DESTDIR)$(PREFIX)/share/doc/flboot/README

clean:
	rm -f *.o font.c flboot.so flboot.efi

.PHONY: all check install clean
