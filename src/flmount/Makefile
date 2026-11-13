# flmount and flpersist -- removable drives and the live session's persistent home
# SPDX-License-Identifier: MIT

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu17 -D_GNU_SOURCE -Wall -Wextra -Wno-format-truncation
PREFIX ?= /usr
DESTDIR ?=

all: flmount flpersist

flmount: flmount.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

flpersist: flpersist.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

# the setuid bit and the group (seat) come from the recipe's owner=
install: all
	install -Dm755 flmount $(DESTDIR)$(PREFIX)/bin/flmount
	install -Dm755 flpersist $(DESTDIR)$(PREFIX)/bin/flpersist
	install -Dm644 90-flmount.rules $(DESTDIR)$(PREFIX)/lib/udev/rules.d/90-flmount.rules

clean:
	$(RM) flmount flpersist

.PHONY: all install clean
