# picom plugin: workspace slide direction and workspace monitor for animation scripts.
# Needs picom's plugin API 0.2 (add_animation_variable). If the installed picom doesn't
# have it, point PICOM_API_CFLAGS at a picom tree that does, e.g.
#   make PICOM_API_CFLAGS=-I/path/to/picom/include
PICOM_API_CFLAGS ?=
CFLAGS ?= -O2 -g
override CFLAGS += -std=gnu23 -Wall -Wextra -fPIC -fvisibility=hidden $(PICOM_API_CFLAGS) \
	$(shell pkg-config --cflags xcb xcb-randr)
LDLIBS = $(shell pkg-config --libs xcb xcb-randr)

build/ws-direction.so: ws_direction.c
	@mkdir -p build
	$(CC) $(CFLAGS) -shared -o $@ $< $(LDLIBS)

clean:
	rm -rf build

.PHONY: clean
