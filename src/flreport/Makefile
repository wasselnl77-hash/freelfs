# flreport -- collects logs and hardware info into ~/freelfs-report-DATE.tar.gz
# SPDX-License-Identifier: MIT
PREFIX ?= /usr
DESTDIR ?=

all:

check:
	sh -n flreport
	./tests/report.sh

install:
	install -Dm755 flreport $(DESTDIR)$(PREFIX)/bin/flreport
	install -Dm644 README $(DESTDIR)$(PREFIX)/share/doc/flreport/README

.PHONY: all check install
