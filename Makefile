.POSIX:
.SUFFIXES:

include config.mk

# flags for compiling
DWLCPPFLAGS = -Isrc/ -DWLR_USE_UNSTABLE -D_POSIX_C_SOURCE=200809L -DVERSION=\"$(VERSION)\"
DWLDEVCFLAGS = -pedantic -Wall -Wextra -Wdeclaration-after-statement -Wno-unused-parameter -Wno-sign-compare -Wshadow -Wunused-macros\
	-Werror=strict-prototypes -Werror=implicit -Werror=return-type -Werror=incompatible-pointer-types

# CFLAGS / LDFLAGS
PKGS      = wlroots wayland-server xkbcommon libinput
DWLCFLAGS = `$(PKG_CONFIG) --cflags $(PKGS)` $(DWLCPPFLAGS) $(DWLDEVCFLAGS) $(CFLAGS)
LDLIBS    = `$(PKG_CONFIG) --libs $(PKGS)` $(LIBS)

all: bin dwl

dwl: bin/dwl.o bin/util.o bin/subprocess.o
	$(CC) $^ $(LDLIBS) $(LDFLAGS) $(DWLCFLAGS) -o bin/$@

bin/dwl.o: src/dwl.c config.mk src/client.h src/xdg-shell-protocol.h src/wlr-layer-shell-unstable-v1-protocol.h

bin/util.o: src/util.c src/util.h

bin/subprocess.o: src/subprocess.c src/xdg-shell-protocol.h

# wayland-scanner is a tool which generates C headers and rigging for Wayland
# protocols, which are specified in XML. wlroots requires you to rig these up
# to your build system yourself and provide them in the include path.
WAYLAND_SCANNER   = `$(PKG_CONFIG) --variable=wayland_scanner wayland-scanner`
WAYLAND_PROTOCOLS = `$(PKG_CONFIG) --variable=pkgdatadir wayland-protocols`

src/xdg-shell-protocol.h:
	$(WAYLAND_SCANNER) server-header \
		$(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@

src/wlr-layer-shell-unstable-v1-protocol.h:
	$(WAYLAND_SCANNER) server-header \
		protocols/wlr-layer-shell-unstable-v1.xml $@

bin:
	mkdir $@/

clean:
	rm -f src/*-protocol.h
	rm -r bin/

dist: clean
	mkdir -p dwl-$(VERSION)
	cp -R LICENSE* Makefile README.md client.h config.def.h\
		config.mk protocols dwl.1 dwl.c util.c util.h\
		dwl-$(VERSION)
	tar -caf dwl-$(VERSION).tar.gz dwl-$(VERSION)
	rm -rf dwl-$(VERSION)

install: dwl
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp -f dwl $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/dwl
	mkdir -p $(DESTDIR)$(MANDIR)/man1
	cp -f dwl.1 $(DESTDIR)$(MANDIR)/man1
	chmod 644 $(DESTDIR)$(MANDIR)/man1/dwl.1

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/dwl $(DESTDIR)$(MANDIR)/man1/dwl.1

.DELETE_ON_ERROR:
.SUFFIXES: .c .o
bin/%.o:
	$(CC) $(CPPFLAGS) $(DWLCFLAGS) -c $< -o $@ 
