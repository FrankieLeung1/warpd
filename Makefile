COMMIT=$(shell git rev-parse --short HEAD)
VERSION=1.0.3-beta
DESTDIR=
PREFIX=/usr

CFLAGS=-g\
	-Wall\
	-Wextra\
	-pedantic\
	-std=c99\
	-Wno-unused-function\
	-Wno-unused-parameter\
	-Wno-deprecated-declarations\
	-DVERSION=\"$(VERSION)\"\
	-DCOMMIT=\"$(COMMIT)\"

ifeq ($(shell uname), Darwin)
	PLATFORM_FLAGS=-framework cocoa

	PLATFORM_FILES=$(shell find src/platform/macos/*.m)
	PLATFORM_OBJECTS=$(PLATFORM_FILES:.m=.o)

	PREFIX=/usr/local
else
	CFLAGS+=-I/usr/include/freetype2

	PLATFORM_FLAGS=-lXfixes\
			-lXext\
			-lXi\
			-lXtst\
			-lX11\
			-lXft

	PLATFORM_FILES=$(shell find src/platform/X/*.c)
	PLATFORM_OBJECTS=$(PLATFORM_FILES:.c=.o)
endif

FILES=$(shell find src/*.c)
OBJECTS=$(FILES:.c=.o) $(PLATFORM_OBJECTS)

all: $(OBJECTS)
	-mkdir bin
	$(CC)  $(CFLAGS) -o bin/warpd $(OBJECTS) $(PLATFORM_FLAGS)
fmt:
	find . -name '*.[chm]' ! -name 'cfg.[ch]'|xargs clang-format -i
assets:
	./gen_assets.py 
clean:
	rm $(OBJECTS)
install:
	install -m644 warpd.1.gz $(DESTDIR)/$(PREFIX)/share/man/man1/
	install -m755 bin/warpd $(DESTDIR)/$(PREFIX)/bin/
uninstall:
	rm $(DESTDIR)/$(PREFIX)/share/man/man1/warpd.1.gz\
		$(DESTDIR)/$(PREFIX)/bin/warpd

.PHONY: all platform assets install uninstall bin
