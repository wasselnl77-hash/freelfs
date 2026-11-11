# flupdate -- the update notifier (a shell script)
# SPDX-License-Identifier: MIT

PREFIX ?= /usr
DESTDIR ?=

all:

install:
	install -Dm755 flupdate-notify $(DESTDIR)$(PREFIX)/bin/flupdate-notify
	install -Dm644 README $(DESTDIR)$(PREFIX)/share/doc/flupdate/README

check:
	sh -n flupdate-notify
	./tests/notify.sh

clean:

.PHONY: all install check clean
