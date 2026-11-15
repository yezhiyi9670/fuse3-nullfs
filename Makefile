# nullfs - a "black hole" filesystem for Linux (FUSE 3)
#
#   make            build ./nullfs
#   make install    install to $(PREFIX)/bin (default /usr/local)
#   make clean

CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -Wall -Wextra -D_FILE_OFFSET_BITS=64
LDFLAGS ?=

PKG_CONFIG ?= pkg-config
FUSE_CFLAGS := $(shell $(PKG_CONFIG) --cflags fuse3 2>/dev/null || echo -I/usr/include/fuse3)
FUSE_LIBS   := $(shell $(PKG_CONFIG) --libs fuse3 2>/dev/null || echo -lfuse3)

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin

all: nullfs

nullfs: nullfs.c
	$(CC) $(CFLAGS) $(FUSE_CFLAGS) -pthread -o $@ nullfs.c $(LDFLAGS) $(FUSE_LIBS) -pthread

install: nullfs
	install -D -m 0755 nullfs $(DESTDIR)$(BINDIR)/nullfs

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/nullfs

clean:
	rm -f nullfs

.PHONY: all install uninstall clean
