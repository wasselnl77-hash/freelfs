# flsh -- the FreeLFS shell
# SPDX-License-Identifier: MIT

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -D_GNU_SOURCE -Wall -Wextra -Wshadow -Wno-unused-parameter

PREFIX ?= /usr
DESTDIR ?=

OBJS = main.o util.o parse.o var.o expand.o arith.o exec.o job.o builtin.o test.o \
	hist.o highlight.o complete.o edit.o prompt.o guard.o

all: flsh

flsh: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

%.o: %.c flsh.h
	$(CC) $(CFLAGS) -c -o $@ $<

check: flsh
	./tests/run.sh
	./tests/interactive.py

install: flsh
	install -Dm755 flsh $(DESTDIR)$(PREFIX)/bin/flsh
	install -Dm644 flshrc $(DESTDIR)/etc/flshrc
	install -d $(DESTDIR)$(PREFIX)/share/flsh/completions
	install -m644 completions/*.flsh $(DESTDIR)$(PREFIX)/share/flsh/completions/
	install -Dm644 README $(DESTDIR)$(PREFIX)/share/doc/flsh/README

clean:
	rm -f $(OBJS) flsh

.PHONY: all check install clean
