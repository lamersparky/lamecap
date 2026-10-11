CC ?= cc
PKG_CONFIG ?= pkg-config
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
CFLAGS ?= -O2 -pipe
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Wformat=2 -Wshadow -Wconversion -D_FILE_OFFSET_BITS=64 $(shell $(PKG_CONFIG) --cflags gio-2.0 gstreamer-1.0)
LDLIBS += $(shell $(PKG_CONFIG) --libs gio-2.0 gstreamer-1.0)

.PHONY: all clean install check
all: lamecap

lamecap: src/lamecap.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

check:
	$(CC) $(CFLAGS) -fsyntax-only src/lamecap.c

install: lamecap
	install -Dm755 lamecap "$(DESTDIR)$(BINDIR)/lamecap"

clean:
	rm -f lamecap
