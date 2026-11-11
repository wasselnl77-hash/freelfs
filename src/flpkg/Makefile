# flpkg -- the FreeLFS package manager
# SPDX-License-Identifier: MIT

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -D_GNU_SOURCE -Wall -Wextra -Wshadow -Wno-unused-parameter
LIBS = -lsqlite3 -lzstd -lcrypto

PREFIX ?= /usr
DESTDIR ?=

CORE = util.o hash.o version.o tar.o zst.o pkginfo.o spawn.o config.o pack.o pkgfile.o

all: flpkg

flpkg: main.o build.o db.o txn.o sysusers.o triggers.o cmd_pkg.o sign.o repo.o solve.o snapshot.o $(CORE)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

tests/test-core: tests/test-core.o $(CORE)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

check: flpkg tests/test-core
	./tests/test-core
	./tests/test-build.sh
	./tests/test-txn.sh
	./tests/test-repo.sh
	./tests/test-hook.sh

%.o: %.c flpkg.h db.h txn.h repo.h sign.h
	$(CC) $(CFLAGS) -c -o $@ $<

install: flpkg
	install -Dm755 flpkg $(DESTDIR)$(PREFIX)/bin/flpkg
	install -Dm755 recipe.sh $(DESTDIR)$(PREFIX)/lib/flpkg/recipe.sh
	install -Dm644 triggers $(DESTDIR)$(PREFIX)/lib/flpkg/triggers

clean:
	rm -f *.o tests/*.o flpkg tests/test-core

.PHONY: all check install clean
