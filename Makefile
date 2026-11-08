# picom plugin: workspace slide direction and workspace monitor for animation scripts.
# Needs picom's plugin API 0.2 (add_animation_variable). If the installed picom doesn't
# have it, point PICOM_API_CFLAGS at a picom tree that does, e.g.
#   make PICOM_API_CFLAGS=-I/path/to/picom/include
PICOM_API_CFLAGS ?=
CFLAGS ?= -O2 -g
override CFLAGS += -std=gnu23 -Wall -Wextra -Wfatal-errors $(PICOM_API_CFLAGS) \
	$(shell pkg-config --cflags xcb xcb-randr)
LDLIBS = $(shell pkg-config --libs xcb xcb-randr)

# The plugin is every file in src/.
PLUGIN_OBJS = $(patsubst src/%.c,build/%.o,$(wildcard src/*.c))

build/ws-direction.so: $(PLUGIN_OBJS)
	$(CC) $(CFLAGS) -shared -o $@ $^ $(LDLIBS)

build/%.o: src/%.c
	@mkdir -p build
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -MMD -MP -c -o $@ $<

-include $(wildcard build/*.d)

# Puts the plugin and the styles where picom finds them by name, for
#   plugins = "ws-direction.so";   and   @include "ws-direction/slide.conf"
PICOM_CONFIG_DIR ?= $(or $(XDG_CONFIG_HOME),$(HOME)/.config)/picom

install: build/ws-direction.so
	install -Dm755 build/ws-direction.so $(PICOM_CONFIG_DIR)/plugins/ws-direction.so
	install -Dm644 -t $(PICOM_CONFIG_DIR)/include/ws-direction styles/*.conf

clean:
	rm -rf build

.PHONY: install clean
