// SPDX-License-Identifier: MPL-2.0
// Copyright (c) JornC

/// The plugin's internal header: the types, the state and the functions that more than
/// one of its modules uses. desktop.c reads the monitors, the workspaces and the managed
/// windows from the X server. passes.c finds switches, starts ws-pass and works out the
/// variables. plugin.c hooks them up to picom and calls them in every pass.

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <picom/api.h>
#include <xcb/randr.h>
#include <xcb/xcb.h>

/// The plugin API the plugin needs is 0.WS_API_MINOR, as the picom fork numbers it.
#define WS_API_MINOR 4
#define WS_STR(x) #x
#define WS_XSTR(x) WS_STR(x)
#define WS_API_VERSION "0." WS_XSTR(WS_API_MINOR)

#if PICOM_API_MAJOR != 0 || PICOM_API_MINOR < WS_API_MINOR
// Not #error, which can't expand WS_API_VERSION.
static_assert(false, "picom/api.h is not compatible: ws-direction needs plugin "
                     "API " WS_API_VERSION " of the picom fork. To build against a "
                     "picom source tree, run make "
                     "PICOM_API_CFLAGS=-I/path/to/picom/include");
#endif

/// The custom trigger we start on the windows of the workspaces a switch passes.
#define WS_PASS_TRIGGER "ws-pass"

/// The animation variables, plugin-ws-direction-<name>, with names in ws_variable_names.
enum ws_variable {
	WS_VAR_DIRECTION,
	WS_VAR_DISTANCE,
	WS_VAR_MONITOR_X,
	WS_VAR_MONITOR_Y,
	WS_VAR_MONITOR_WIDTH,
	WS_VAR_MONITOR_HEIGHT,
	WS_VAR_FACES,
	WS_VAR_FACE,
	WS_VAR_SELECTED,
	WS_VAR_PLACED,
	WS_N_VARIABLES,
};

/// How many workspaces we remember per monitor. When switching fast (e.g. scrolling on a
/// bar), the windows of a switch can still be showing and hiding while the next switches
/// are already done.
#define WS_HISTORY 8
#define WS_MAX_MONITORS 16
#define WS_MAX_DESKTOPS 256

/// Where a workspace sorts. Workspaces whose name starts with a number sort by that
/// number. The others sort after those, by their position in _NET_DESKTOP_NAMES.
struct ws_order {
	bool numbered;
	uint64_t number;
	unsigned position;
};

/// A workspace that a monitor showed.
struct ws_visit {
	char *name;
	struct ws_order order;
	/// When we last saw a part of the switch to this workspace, 0 if we don't know
	/// what the monitor showed before it.
	uint64_t time_us;
	/// The frame in which we last saw a part of the switch to this workspace.
	uint64_t frame;
	/// Whether the root window told us about this switch, rather than a window that
	/// showed.
	bool from_root;
	/// Whether we started ws-pass for this switch.
	bool passes_started;
};

/// What we know about one monitor.
struct ws_monitor {
	/// Viewport of the workspace shown on this monitor. We match entries to monitors
	/// through this, so they stay valid when the monitor list changes.
	bool has_viewport;
	int32_t viewport_x, viewport_y;
	/// The workspaces this monitor showed, oldest first. The last one is shown now.
	/// Each one after the first is a switch from the one before it.
	struct ws_visit visits[WS_HISTORY];
	unsigned n_visits;
};

struct ws_rect {
	int32_t x1, y1, x2, y2;
};

/// A window the window manager manages.
struct ws_client {
	xcb_window_t id;
	/// Its desktop, -1 if it is not on one workspace.
	int64_t desktop;
	/// Where it is, in root coordinates.
	struct ws_rect rect;
	/// The name of its workspace, NULL if it is not on one.
	char *workspace;
	/// Whether it moved from another workspace since the read before.
	bool moved;
};

/// The requests for the root window properties, the screen size and the monitors, sent
/// together so they take one round trip. Pair ws_read_root_request with
/// ws_read_root_reply.
struct ws_root_requests {
	xcb_get_property_cookie_t current, viewports, names;
	xcb_get_geometry_cookie_t screen;
	xcb_randr_get_monitors_cookie_t monitors;
};

/// Everything the plugin knows, defined in plugin.c.
struct ws_state {
	const struct picom_api *api;
	xcb_connection_t *c;
	xcb_window_t root;
	xcb_atom_t a_current_desktop, a_desktop_names, a_desktop_viewport, a_wm_desktop,
	    a_utf8_string, a_net_wm_name, a_client_list, a_wm_state, a_wm_state_hidden;
	struct ws_rect screen;
	FILE *log;

	/// The pass picom is in (UINT64_MAX before the first one), and when it started.
	uint64_t frame;
	uint64_t frame_time_us;
	/// The pass in which we last read the root window and the managed windows.
	uint64_t read_frame;
	/// How often we waited for the X server while working out the variables of one
	/// window. For the log.
	unsigned n_waits;

	/// _NET_DESKTOP_NAMES, pointers into `names_buf`.
	const char *names[WS_MAX_DESKTOPS];
	unsigned n_names;
	char *names_buf;
	/// _NET_DESKTOP_VIEWPORT, as x, y pairs.
	uint32_t viewports[WS_MAX_DESKTOPS * 2];
	unsigned n_viewports;
	/// _NET_CURRENT_DESKTOP, -1 if not set.
	int64_t current;

	struct ws_rect monitors[WS_MAX_MONITORS];
	int n_monitors;

	/// The windows in _NET_CLIENT_LIST. Windows whose place could not be read are
	/// left out.
	struct ws_client *clients;
	unsigned n_clients;

	struct ws_monitor tracked[WS_MAX_MONITORS];
	unsigned n_tracked;
};

extern struct ws_state wsd;

/// `array` grown to `n` items of `size` bytes. Aborts when out of memory.
static inline void *ws_grow(void *array, size_t n, size_t size) {
	void *grown = realloc(array, n * size);
	if (grown == NULL) {
		abort();
	}
	return grown;
}

// plugin.c
__attribute__((format(printf, 1, 2))) void ws_log(const char *fmt, ...);
uint64_t ws_now_us(void);

// desktop.c
xcb_atom_t ws_atom(const char *name);
void *ws_reply(unsigned int sequence);
xcb_get_property_reply_t *
ws_get_prop(xcb_window_t w, xcb_atom_t atom, xcb_atom_t type, uint32_t max_len);
int64_t ws_find_desktop(const char *name);
bool ws_desktop_viewport(unsigned desktop, int32_t *x, int32_t *y);
int ws_monitor_index(bool has_viewport, int32_t x, int32_t y);
int ws_desktop_monitor(unsigned desktop);
bool ws_rect_has_point(struct ws_rect r, int32_t x, int32_t y);
const struct ws_client *ws_find_client(xcb_window_t id);
bool ws_read_root_reply(struct ws_root_requests requests);
bool ws_read_root(void);
void ws_seed_from_windows(void);
void ws_read(void);

// passes.c
struct ws_monitor *ws_get_monitor(unsigned desktop, xcb_window_t w);
void ws_refresh(struct ws_root_requests requests);
void ws_compute(const struct picom_animation_start *start,
                double values[static WS_N_VARIABLES]);
unsigned ws_unstarted_switch(const struct ws_monitor *m);
void ws_start_passes(void);
