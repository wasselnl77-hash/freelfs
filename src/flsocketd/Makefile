# flsocketd -- start a daemon when someone connects to its socket
# SPDX-License-Identifier: MIT

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu17 -D_GNU_SOURCE -Wall -Wextra
PREFIX ?= /usr
DESTDIR ?=

all: flsocketd

flsocketd: flsocketd.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

# runs the launcher against a toy daemon (a shell script that reads the passed socket)
check: flsocketd
	sh tests/check.sh ./flsocketd

install: all
	install -Dm755 flsocketd $(DESTDIR)$(PREFIX)/sbin/flsocketd

clean:
	$(RM) flsocketd

.PHONY: all check install clean
