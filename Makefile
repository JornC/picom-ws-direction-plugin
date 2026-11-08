# picom plugin: workspace slide direction and workspace monitor for animation scripts.
# Needs plugin API 0.5 of the picom fork (add_animation_variable, add_frame_callback,
# start_animation, add_animation_trigger and pause_screen). make finds the installed
# picom's headers with pkg-config picom-api. If the installed picom doesn't have the API,
# point PICOM_API_CFLAGS at a picom tree that does, e.g.
#   make PICOM_API_CFLAGS=-I/path/to/picom/include
PKG_CONFIG ?= pkg-config
# picom-api.pc's Cflags name include/picom, but the code includes <picom/api.h>, so the
# include directory itself goes along, for a picom installed outside the default paths.
ifeq ($(PICOM_API_CFLAGS),)
PICOM_API_CFLAGS := $(strip $(shell $(PKG_CONFIG) --cflags picom-api 2>/dev/null) \
	$(addprefix -I,$(shell $(PKG_CONFIG) --variable=includedir picom-api 2>/dev/null)))
endif
# Expands to nothing, or stops make where the headers are needed.
picom_api_missing = picom's plugin API headers not found: install a picom that has them \
	(pkg-config picom-api), or run make PICOM_API_CFLAGS=-I/path/to/picom/include
picom_api_check = $(if $(PICOM_API_CFLAGS),,$(error $(picom_api_missing)))

# The version is in the file VERSION. In a git checkout, git describe adds how many
# commits came after the tag v$(VERSION) and the commit, e.g. 0.1.0-12-gabc1234, or only
# the commit while there is no such tag. A release tarball has no .git: it builds as
# VERSION.
BASE_VERSION := $(shell cat VERSION)
GIT_DESCRIBE := $(if $(wildcard .git),$(shell git describe --tags --match \
	'v$(BASE_VERSION)' --always --dirty 2>/dev/null))
VERSION := $(BASE_VERSION)$(if $(GIT_DESCRIBE),-g$(GIT_DESCRIBE))
ifneq ($(filter v$(BASE_VERSION)%,$(GIT_DESCRIBE)),)
VERSION := $(GIT_DESCRIBE:v%=%)
endif

CFLAGS ?= -O2 -g
# SANITIZE=address,undefined builds with those sanitizers, which stop the program at the
# first error. Run make clean when you change it, so every object is built the same way.
ifneq ($(SANITIZE),)
override CFLAGS += -fsanitize=$(SANITIZE) -fno-sanitize-recover=all -fno-omit-frame-pointer
endif
override CFLAGS += -std=gnu23 -Wall -Wextra -Wfatal-errors -Ibuild $(PICOM_API_CFLAGS) \
	$(shell $(PKG_CONFIG) --cflags xcb xcb-randr)
LDLIBS = $(shell $(PKG_CONFIG) --libs xcb xcb-randr)

# The plugin is every file in src/.
PLUGIN_OBJS = $(patsubst src/%.c,build/%.o,$(wildcard src/*.c))

all: build/ws-direction.so

build/ws-direction.so: $(PLUGIN_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -shared -o $@ $^ $(LDLIBS)

build/%.o: src/%.c
	@mkdir -p build
	$(picom_api_check)
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -fvisibility=hidden -MMD -MP -c -o $@ $<

-include $(wildcard build/*.d)

# Written again only when the version changes, so only then what includes it rebuilds.
build/version.h: FORCE
	@mkdir -p build
	@echo '#define WS_DIRECTION_VERSION "$(VERSION)"' >$@.new
	@if cmp -s $@.new $@; then rm $@.new; else mv $@.new $@; fi
build/plugin.o: build/version.h
FORCE:

# Puts the plugin and the styles where picom finds them by name, for
#   plugins = "ws-direction.so";   and   @include "ws-direction/slide.conf"
# It overwrites what is in STYLEDIR: keep your own copies of styles next to picom.conf
# instead. By default for one user: the plugin and the styles in picom's config
# directory, where picom finds plugins and includes, and the man page under PREFIX, once
# man/ws-direction.1 exists. A package sets DESTDIR, PREFIX=/usr, STYLEDIR, and
# PLUGINDIR to picom's plugin directory: picom/plugins under the library directory picom
# was installed to, e.g. /usr/lib/picom/plugins. The picom fork finds the plugin there
# by name; picom releases need its full path in plugins. E.g.
#   make install DESTDIR=... PREFIX=/usr PLUGINDIR=/usr/lib/picom/plugins \
#       STYLEDIR=/etc/xdg/picom/include/ws-direction
PICOM_CONFIG_DIR ?= $(or $(XDG_CONFIG_HOME),$(HOME)/.config)/picom
PLUGINDIR ?= $(PICOM_CONFIG_DIR)/plugins
STYLEDIR ?= $(PICOM_CONFIG_DIR)/include/ws-direction
PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/share/man

STYLES = $(notdir $(wildcard styles/*.conf))
MAN = $(wildcard man/ws-direction.1)

install: all
	install -Dm755 build/ws-direction.so $(DESTDIR)$(PLUGINDIR)/ws-direction.so
	install -Dm644 -t $(DESTDIR)$(STYLEDIR) $(addprefix styles/,$(STYLES))
	$(if $(MAN),install -Dm644 $(MAN) $(DESTDIR)$(MANDIR)/man1/ws-direction.1)

# Removes what install puts in place, and STYLEDIR if that leaves it empty.
uninstall:
	rm -f $(DESTDIR)$(PLUGINDIR)/ws-direction.so \
		$(addprefix $(DESTDIR)$(STYLEDIR)/,$(STYLES)) \
		$(if $(MAN),$(DESTDIR)$(MANDIR)/man1/ws-direction.1)
	rmdir --ignore-fail-on-non-empty $(DESTDIR)$(STYLEDIR) 2>/dev/null || true

# clang-format output differs between major versions; the code is formatted with 23.
CLANG_FORMAT ?= clang-format
FORMAT_FILES = $(wildcard src/*.c src/*.h tests/*.c tests/unit/*.c tests/unit/*.h)

format:
	$(CLANG_FORMAT) -i $(FORMAT_FILES)

format-check:
	$(CLANG_FORMAT) --dry-run --Werror $(FORMAT_FILES)

# shellcheck on the shell scripts.
lint:
	shellcheck tests/*.sh docs/*.sh

clean:
	rm -rf build

.PHONY: all install uninstall format format-check lint clean FORCE
