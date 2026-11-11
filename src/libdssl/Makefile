# libdssl -- a native DSSL front end (DSSL to CSS). C11, no dependencies beyond libc and libm.
# SPDX-License-Identifier: MIT

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wshadow -Wno-unused-parameter -fPIC -fvisibility=hidden
LDFLAGS ?=
PREFIX ?= /usr
LIBDIR ?= $(PREFIX)/lib
DESTDIR ?=
VERSION = 0.1
SOVER = 0

SRCS = util.c parse.c value.c props.c gradient.c resolve.c emit_json.c emit_css.c vocab.c
OBJS = $(SRCS:.c=.o)
HDRS = dssl.h internal.h vocab.h
SHLIB = libdssl.so.$(SOVER)

all: libdssl.a $(SHLIB) libdssl.so dssl2css dssl.pc

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

libdssl.a: $(OBJS)
	rm -f $@
	$(AR) rcs $@ $^

$(SHLIB): $(OBJS)
	$(CC) -shared -Wl,-soname,$(SHLIB) $(LDFLAGS) -o $@ $^ -lm

libdssl.so: $(SHLIB)
	ln -sf $(SHLIB) $@

dssl2css: main.o libdssl.a
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ main.o libdssl.a -lm

dssl.pc: dssl.pc.in
	sed -e 's|@PREFIX@|$(PREFIX)|' -e 's|@LIBDIR@|$(LIBDIR)|' -e 's|@VERSION@|$(VERSION)|' $< > $@

tests/conform: tests/conform.c libdssl.a $(HDRS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/conform.c libdssl.a -lm

# the conformance corpus (corpus/ and expected/, see README): levels 1 and 2 plus the CSS lane,
# then every input cut at random points (must not crash)
check: tests/conform
	./tests/conform corpus expected tests/golden
	./tests/conform --fuzz corpus 12

# the same under AddressSanitizer and UBSan (separate objects, so the normal build is untouched)
SAN = -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined
asan:
	mkdir -p build-asan
	$(CC) -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wno-unused-parameter $(SAN) -o build-asan/conform tests/conform.c $(SRCS) -lm
	./build-asan/conform corpus expected tests/golden
	./build-asan/conform --fuzz corpus 24

valgrind: tests/conform
	valgrind -q --leak-check=full --errors-for-leak-kinds=all --error-exitcode=9 ./tests/conform --quiet corpus expected tests/golden
	valgrind -q --leak-check=full --errors-for-leak-kinds=all --error-exitcode=9 ./tests/conform --fuzz corpus 1

bench: tests/conform
	./tests/conform --bench corpus

install: all
	install -Dm755 dssl2css $(DESTDIR)$(PREFIX)/bin/dssl2css
	install -Dm644 dssl.h $(DESTDIR)$(PREFIX)/include/dssl.h
	install -Dm644 libdssl.a $(DESTDIR)$(LIBDIR)/libdssl.a
	install -Dm755 $(SHLIB) $(DESTDIR)$(LIBDIR)/$(SHLIB)
	ln -sf $(SHLIB) $(DESTDIR)$(LIBDIR)/libdssl.so
	install -Dm644 dssl.pc $(DESTDIR)$(LIBDIR)/pkgconfig/dssl.pc

clean:
	rm -rf *.o tests/conform libdssl.a libdssl.so* dssl2css dssl.pc build-asan

# vocab.c comes from the DSSL repo's word lists, see tools/genvocab.mjs
.PHONY: all check asan valgrind bench install clean
