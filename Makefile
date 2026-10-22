# Protocol XMLs are vendored in protocol/ instead of taken from wayland-protocols:
# Ubuntu 24.04 ships 1.34, which has no ext-image-copy-capture yet.
PREFIX ?= /usr/local
PROTOS := $(wildcard protocol/*.xml)
NAMES := $(basename $(notdir $(PROTOS)))
HDRS := $(NAMES:%=proto/%.h)
SRCS := $(NAMES:%=proto/%.c)

CFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter

wl-castlog: wl-castlog.c $(HDRS) $(SRCS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Iproto -o $@ wl-castlog.c $(SRCS) $(shell pkg-config --cflags --libs wayland-client) $(LDFLAGS)

proto/%.h: protocol/%.xml | proto
	wayland-scanner client-header $< $@
proto/%.c: protocol/%.xml | proto
	wayland-scanner private-code $< $@
proto:
	mkdir -p $@

install: wl-castlog
	install -Dm755 wl-castlog $(DESTDIR)$(PREFIX)/bin/wl-castlog

clean:
	rm -rf wl-castlog proto
.PHONY: install clean
