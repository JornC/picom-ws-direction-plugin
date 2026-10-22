# picom plugin: workspace slide direction and workspace monitor for animation scripts.
# Needs picom's plugin API 0.2 (add_script_variable). Until a picom release has it, point
# PICOM_API_CFLAGS at a picom tree that does, e.g. the plugin-hook-small worktree (the
# default here).
PICOM_API_CFLAGS ?= -I../picom/worktrees/plugin-hook-small/include
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu23 -Wall -Wextra -fPIC -fvisibility=hidden $(PICOM_API_CFLAGS) \
	$(shell pkg-config --cflags xcb xcb-randr)
LDLIBS = $(shell pkg-config --libs xcb xcb-randr)

build/ws-direction.so: ws_direction.c
	@mkdir -p build
	$(CC) $(CFLAGS) -shared -o $@ $< $(LDLIBS)

clean:
	rm -rf build

.PHONY: clean
