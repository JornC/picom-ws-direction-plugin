// SPDX-License-Identifier: MPL-2.0
// Copyright (c) JornC

/// picom plugin that works out the direction of a workspace switch, so animation scripts
/// can slide windows the right way, and the monitor of a window's workspace, so they can
/// crop to it. Animation variables:
///
///   plugin-ws-direction-direction
///       1 if the new workspace sorts after the old one, -1 if before, 0 if the
///       animation was not caused by a workspace switch on the window's monitor.
///   plugin-ws-direction-monitor-x/y/width/height
///       the monitor that shows the window's workspace. For other windows and triggers:
///       the monitor that fully contains the window, else the whole screen (like
///       window-monitor-*). For a window that closes: always the whole screen, as the
///       window is gone and can't be looked up; use window-monitor-* there.
///
/// For workspaces it only uses EWMH data the window manager already sets:
/// _NET_CURRENT_DESKTOP, _NET_DESKTOP_NAMES and _NET_DESKTOP_VIEWPORT on the root window,
/// and _NET_WM_DESKTOP on client windows. Monitors come from RandR, a window's place from
/// GetGeometry.
///
/// The order in which a window manager changes these properties and maps and unmaps
/// windows is not specified, and those changes can reach picom over several frames. So we
/// don't act on events. We remember, per monitor, the workspaces it showed recently, and
/// when we last saw a part of each switch between them. On the first call in each frame
/// we read the root properties again and update that. When a window starts a show, open
/// or hide animation, we look for a recent switch to (or away from) its workspace on its
/// monitor. A window that shows on a workspace we didn't think was visible also counts as
/// a switch, because some window managers map the new windows before they update the root
/// properties.
///
/// We only learn about a switch when picom asks for a value, i.e. when a window starts
/// an animation whose script reads the variables. A switch that starts no animation (for
/// example one made while picom is unredirected, see unredir-if-possible) may not be
/// seen: we see it only if a window of the new workspace still starts an animation once
/// picom redirects the screen again. Otherwise the next switch is judged against the
/// last workspace we saw: back to that workspace gives 0; elsewhere the windows that
/// show get the sign relative to it, and the windows that hide get 0.
///
/// Each animation start costs one round trip to the X server, on picom's main thread.
/// Usually that is well under a millisecond, but the reply waits behind whatever the
/// server is doing (other clients, the window manager's own burst of requests during a
/// switch, picom's drawing of the previous frame). The first start in a frame can then
/// wait tens of milliseconds, for example when many windows open at once.
///
/// We connect to the display named by DISPLAY, like picom itself. The variables are only
/// added once the X connection is up. Without it, every animation whose script reads
/// them fails to compile: picom logs 'variable name "plugin-ws-direction-direction" is
/// not defined' and runs without that animation.
///
/// Give picom the plugin's path in its config file (plugins =
/// "/path/to/ws-direction.so";) or with --plugins. picom releases up to v13 find a bare
/// file name in their plugin directories only when it comes from --plugins, not from the
/// config file (fixed upstream in yshui/picom#1547).
///
/// Set WS_DIRECTION_LOG to a file to log every decision there, in picom's log format.

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <picom/api.h>
#include <xcb/randr.h>
#include <xcb/xcb.h>
#include <xcb/xcbext.h>

/// How long a workspace switch stays usable after we last saw a part of it (the root
/// window change, or a window of it that shows or hides). The window manager makes all
/// the changes for one switch at once, but they can reach picom over a few frames. This
/// is long enough to cover that, and short enough that an unrelated show or hide right
/// after the switch gets no direction.
#define WS_SWITCH_LIFETIME_US (100 * 1000)
/// A frame can take longer than WS_SWITCH_LIFETIME_US to render (e.g. with software
/// rendering). So a switch is also used for this many frames after the one we saw it
/// in, as long as they start within WS_SWITCH_MAX_LIFETIME_US.
#define WS_SWITCH_LIFETIME_FRAMES 2
#define WS_SWITCH_MAX_LIFETIME_US (250 * 1000)
/// How many workspaces we remember per monitor. When switching fast (e.g. scrolling on a
/// bar), the windows of a switch can still be showing and hiding while the next switches
/// are already done.
#define WS_HISTORY 8
#define WS_MAX_MONITORS 16
#define WS_MAX_DESKTOPS 256

/// _NET_WM_DESKTOP value of windows that are on all desktops.
#define WS_ALL_DESKTOPS 0xFFFFFFFF

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

static const char *const ws_variable_names[] = {
    "direction", "monitor-x", "monitor-y", "monitor-width", "monitor-height",
};
#define WS_N_VARIABLES (sizeof(ws_variable_names) / sizeof(ws_variable_names[0]))

static struct {
	xcb_connection_t *c;
	xcb_window_t root;
	xcb_atom_t a_current_desktop, a_desktop_names, a_desktop_viewport, a_wm_desktop,
	    a_utf8_string, a_net_wm_name;
	struct ws_rect screen;
	FILE *log;

	/// The frame of the last call (UINT64_MAX before the first one), and when we saw
	/// it start.
	uint64_t frame;
	uint64_t frame_time_us;
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

	struct ws_monitor tracked[WS_MAX_MONITORS];
	unsigned n_tracked;
} wsd = {.current = -1, .frame = UINT64_MAX};

__attribute__((format(printf, 1, 2))) static void ws_log(const char *fmt, ...) {
	if (wsd.log == NULL) {
		return;
	}
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	char stamp[32];
	strftime(stamp, sizeof(stamp), "%m/%d/%Y %H:%M:%S", localtime(&now.tv_sec));
	fprintf(wsd.log, "[ %s.%03ld ws_direction DEBUG ] ", stamp, now.tv_nsec / 1000000);
	va_list args;
	va_start(args, fmt);
	vfprintf(wsd.log, fmt, args);
	va_end(args);
	fputc('\n', wsd.log);
	fflush(wsd.log);
}

static uint64_t ws_now_us(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000;
}

/// Take the reply to a request, waiting for it if it hasn't arrived yet. Returns NULL if
/// the request failed. The caller frees the reply.
static void *ws_reply(unsigned int sequence) {
	void *reply = NULL;
	xcb_generic_error_t *err = NULL;
	if (!xcb_poll_for_reply(wsd.c, sequence, &reply, &err)) {
		wsd.n_waits++;
		reply = xcb_wait_for_reply(wsd.c, sequence, &err);
	}
	free(err);
	return reply;
}

static xcb_atom_t ws_atom(const char *name) {
	auto reply = xcb_intern_atom_reply(
	    wsd.c, xcb_intern_atom(wsd.c, 0, (uint16_t)strlen(name), name), NULL);
	xcb_atom_t ret = reply ? reply->atom : XCB_ATOM_NONE;
	free(reply);
	return ret;
}

/// Ask for a property. Pair with ws_prop_reply.
static xcb_get_property_cookie_t
ws_prop_request(xcb_window_t w, xcb_atom_t atom, xcb_atom_t type, uint32_t max_len) {
	return xcb_get_property(wsd.c, 0, w, atom, type, 0, max_len);
}

/// Take the reply to ws_prop_request. Returns NULL if the property is not set. The
/// caller frees the reply.
static xcb_get_property_reply_t *ws_prop_reply(xcb_get_property_cookie_t cookie) {
	xcb_get_property_reply_t *reply = ws_reply(cookie.sequence);
	if (reply != NULL && (reply->type == XCB_ATOM_NONE || reply->value_len == 0)) {
		free(reply);
		return NULL;
	}
	return reply;
}

/// Read a property, in a round trip of its own. Returns NULL if it is not set.
static xcb_get_property_reply_t *
ws_get_prop(xcb_window_t w, xcb_atom_t atom, xcb_atom_t type, uint32_t max_len) {
	return ws_prop_reply(ws_prop_request(w, atom, type, max_len));
}

static struct ws_order ws_order_of(const char *name, unsigned position) {
	struct ws_order ret = {.position = position};
	for (const char *p = name; *p >= '0' && *p <= '9'; p++) {
		ret.numbered = true;
		ret.number = ret.number * 10 + (uint64_t)(*p - '0');
		if (ret.number > UINT32_MAX) {
			ret.number = UINT32_MAX;
		}
	}
	return ret;
}

/// Returns 1 if `a` sorts after `b`, -1 if it sorts before, 0 if they are the same.
static int ws_order_compare(struct ws_order a, struct ws_order b) {
	if (a.numbered != b.numbered) {
		return a.numbered ? -1 : 1;
	}
	if (a.numbered && a.number != b.number) {
		return a.number > b.number ? 1 : -1;
	}
	if (a.position != b.position) {
		return a.position > b.position ? 1 : -1;
	}
	return 0;
}

/// Find a desktop by name. Returns its index, or -1 if there is no such desktop.
static int64_t ws_find_desktop(const char *name) {
	for (unsigned i = 0; i < wsd.n_names; i++) {
		if (strcmp(wsd.names[i], name) == 0) {
			return i;
		}
	}
	return -1;
}

/// Get the viewport of a desktop. Returns false if the window manager didn't set one.
static bool ws_desktop_viewport(unsigned desktop, int32_t *x, int32_t *y) {
	if (desktop >= wsd.n_viewports / 2) {
		return false;
	}
	*x = (int32_t)wsd.viewports[desktop * 2];
	*y = (int32_t)wsd.viewports[desktop * 2 + 1];
	return true;
}

/// Index of the monitor that contains a viewport. Returns -1 if there is no viewport, or
/// it is not on any monitor. -1 stands for "all monitors".
static int ws_monitor_index(bool has_viewport, int32_t x, int32_t y) {
	if (!has_viewport) {
		return -1;
	}
	for (int i = 0; i < wsd.n_monitors; i++) {
		auto e = &wsd.monitors[i];
		if (e->x1 <= x && x < e->x2 && e->y1 <= y && y < e->y2) {
			return i;
		}
	}
	return -1;
}

/// Index of the monitor that shows a desktop, -1 for "all monitors".
static int ws_desktop_monitor(unsigned desktop) {
	int32_t x = 0, y = 0;
	bool has_viewport = ws_desktop_viewport(desktop, &x, &y);
	return ws_monitor_index(has_viewport, x, y);
}

/// Find what we know about the monitor of a desktop. Returns NULL if nothing.
static struct ws_monitor *ws_find_monitor(unsigned desktop) {
	auto index = ws_desktop_monitor(desktop);
	for (unsigned i = 0; i < wsd.n_tracked; i++) {
		auto m = &wsd.tracked[i];
		if (ws_monitor_index(m->has_viewport, m->viewport_x, m->viewport_y) == index) {
			return m;
		}
	}
	return NULL;
}

static struct ws_visit *ws_shown(struct ws_monitor *m) {
	return &m->visits[m->n_visits - 1];
}

/// Direction of the switch to `m->visits[k]`, `k` must be at least 1.
static int ws_switch_direction(const struct ws_monitor *m, unsigned k) {
	return ws_order_compare(m->visits[k].order, m->visits[k - 1].order);
}

static bool ws_switch_is_recent(const struct ws_visit *v) {
	if (v->time_us == 0) {
		return false;
	}
	auto age = wsd.frame_time_us - v->time_us;
	auto frames = wsd.frame - v->frame;
	return age < WS_SWITCH_LIFETIME_US ||
	       (frames <= WS_SWITCH_LIFETIME_FRAMES && age < WS_SWITCH_MAX_LIFETIME_US);
}

/// We saw another part of a switch, keep it usable.
static void ws_switch_touch(struct ws_visit *v) {
	v->time_us = wsd.frame_time_us;
	v->frame = wsd.frame;
}

/// Describe where we learned something: window `w`, or the root window if `w` is 0.
static void ws_source(char buf[static 32], xcb_window_t w) {
	if (w == XCB_NONE) {
		strcpy(buf, "the root window");
	} else {
		snprintf(buf, 32, "window %#010x", w);
	}
}

/// Index of the latest visit to workspace `name`, or -1.
static int ws_find_visit(const struct ws_monitor *m, const char *name) {
	for (int k = (int)m->n_visits - 1; k >= 0; k--) {
		if (strcmp(m->visits[k].name, name) == 0) {
			return k;
		}
	}
	return -1;
}

static struct ws_visit ws_visit_of(unsigned desktop) {
	struct ws_visit ret = {
	    .name = strdup(wsd.names[desktop]),
	    .order = ws_order_of(wsd.names[desktop], desktop),
	};
	if (ret.name == NULL) {
		abort();
	}
	return ret;
}

/// Record a switch to `desktop` on monitor `m`, as visit `k` (at most `m->n_visits`).
/// `w` is the window we learned this from, or 0 if it was the root window.
static void ws_add_switch(struct ws_monitor *m, unsigned k, unsigned desktop, xcb_window_t w) {
	if (m->n_visits == WS_HISTORY) {
		// Forget the oldest one.
		free(m->visits[0].name);
		memmove(&m->visits[0], &m->visits[1], sizeof(m->visits[0]) * (WS_HISTORY - 1));
		m->n_visits--;
		k--;
	}
	memmove(&m->visits[k + 1], &m->visits[k], sizeof(m->visits[0]) * (m->n_visits - k));
	m->n_visits++;
	m->visits[k] = ws_visit_of(desktop);
	m->visits[k].from_root = w == XCB_NONE;
	ws_switch_touch(&m->visits[k]);

	char source[32];
	ws_source(source, w);
	ws_log("Workspace switch on monitor at %d,%d: \"%s\" -> \"%s\", direction %d "
	       "(from %s)",
	       m->viewport_x, m->viewport_y, m->visits[k - 1].name, m->visits[k].name,
	       ws_switch_direction(m, k), source);
}

/// Get what we know about the monitor of `desktop`. If we know nothing yet, assume it
/// shows `desktop`. `w` is the window we learned this from, or 0 if it was the root
/// window. Returns NULL if we track too many monitors already.
static struct ws_monitor *ws_get_monitor(unsigned desktop, xcb_window_t w) {
	auto m = ws_find_monitor(desktop);
	if (m == NULL) {
		if (wsd.n_tracked == WS_MAX_MONITORS) {
			return NULL;
		}
		m = &wsd.tracked[wsd.n_tracked++];
		*m = (struct ws_monitor){.n_visits = 1};
		m->visits[0] = ws_visit_of(desktop);
		char source[32];
		ws_source(source, w);
		ws_log("Workspace \"%s\" is shown (from %s)", wsd.names[desktop], source);
	}
	// Workspaces can move between monitors, and change their position in the list.
	m->has_viewport = ws_desktop_viewport(desktop, &m->viewport_x, &m->viewport_y);
	for (unsigned k = 0; k < m->n_visits; k++) {
		auto position = ws_find_desktop(m->visits[k].name);
		if (position >= 0) {
			m->visits[k].order =
			    ws_order_of(m->visits[k].name, (unsigned)position);
		}
	}
	return m;
}

/// Ask for _NET_WM_DESKTOP of a window. Pair with ws_window_desktop_reply.
static xcb_get_property_cookie_t ws_window_desktop_request(xcb_window_t w) {
	return ws_prop_request(w, wsd.a_wm_desktop, XCB_ATOM_CARDINAL, 1);
}

/// Take the reply to ws_window_desktop_request. Returns -1 if the property is not set,
/// or not a single desktop we know.
static int64_t ws_window_desktop_reply(xcb_get_property_cookie_t cookie) {
	auto prop = ws_prop_reply(cookie);
	int64_t ret = -1;
	if (prop != NULL && prop->format == 32) {
		ret = *(uint32_t *)xcb_get_property_value(prop);
	}
	free(prop);
	if (ret == WS_ALL_DESKTOPS || ret >= wsd.n_names) {
		return -1;
	}
	return ret;
}

/// Read _NET_WM_DESKTOP of a window, in a round trip of its own.
static int64_t ws_window_desktop(xcb_window_t w) {
	return ws_window_desktop_reply(ws_window_desktop_request(w));
}

/// The requests for the root window properties, the screen size and the monitors, sent
/// together so they take one round trip. Pair ws_read_root_request with
/// ws_read_root_reply.
struct ws_root_requests {
	xcb_get_property_cookie_t current, viewports, names;
	xcb_get_geometry_cookie_t screen;
	xcb_randr_get_monitors_cookie_t monitors;
};

static struct ws_root_requests ws_read_root_request(void) {
	return (struct ws_root_requests){
	    .current = ws_prop_request(wsd.root, wsd.a_current_desktop, XCB_ATOM_CARDINAL, 1),
	    .viewports = ws_prop_request(wsd.root, wsd.a_desktop_viewport,
	                                 XCB_ATOM_CARDINAL, WS_MAX_DESKTOPS * 2),
	    .names = ws_prop_request(wsd.root, wsd.a_desktop_names, wsd.a_utf8_string,
	                             UINT32_MAX / 4),
	    .screen = xcb_get_geometry(wsd.c, wsd.root),
	    .monitors = xcb_randr_get_monitors(wsd.c, wsd.root, 1),
	};
}

/// Take the replies to ws_read_root_request. Returns whether the root properties changed
/// since the last time.
static bool ws_read_root_reply(struct ws_root_requests requests) {
	bool changed = false;

	auto current = ws_prop_reply(requests.current);
	int64_t new_current = -1;
	if (current != NULL && current->format == 32) {
		new_current = *(uint32_t *)xcb_get_property_value(current);
	}
	free(current);
	changed |= new_current != wsd.current;
	wsd.current = new_current;

	auto viewports = ws_prop_reply(requests.viewports);
	unsigned n_viewports = 0;
	if (viewports != NULL && viewports->format == 32) {
		n_viewports = (unsigned)xcb_get_property_value_length(viewports) / 4;
		n_viewports =
		    n_viewports > WS_MAX_DESKTOPS * 2 ? WS_MAX_DESKTOPS * 2 : n_viewports;
	}
	if (n_viewports != wsd.n_viewports ||
	    (n_viewports && memcmp(wsd.viewports, xcb_get_property_value(viewports),
	                           n_viewports * 4) != 0)) {
		changed = true;
		if (n_viewports) {
			memcpy(wsd.viewports, xcb_get_property_value(viewports),
			       n_viewports * 4);
		}
		wsd.n_viewports = n_viewports;
	}
	free(viewports);

	// The names are a list of strings, each one ending with a NUL. We add one more
	// NUL in case the last one doesn't have it.
	auto names = ws_prop_reply(requests.names);
	size_t len = names ? (size_t)xcb_get_property_value_length(names) : 0;
	char *buf = calloc(len + 1, 1);
	if (buf == NULL) {
		abort();
	}
	if (len) {
		memcpy(buf, xcb_get_property_value(names), len);
	}
	free(names);
	size_t old_len = 0;
	for (unsigned i = 0; i < wsd.n_names; i++) {
		old_len += strlen(wsd.names[i]) + 1;
	}
	if (wsd.names_buf == NULL || len != old_len || memcmp(buf, wsd.names_buf, len) != 0) {
		changed = true;
	}
	free(wsd.names_buf);
	wsd.names_buf = buf;
	wsd.n_names = 0;
	for (size_t start = 0; start < len && wsd.n_names < WS_MAX_DESKTOPS;) {
		wsd.names[wsd.n_names++] = &buf[start];
		start += strlen(&buf[start]) + 1;
	}

	xcb_get_geometry_reply_t *screen = ws_reply(requests.screen.sequence);
	if (screen != NULL) {
		wsd.screen = (struct ws_rect){.x2 = screen->width, .y2 = screen->height};
		free(screen);
	}

	xcb_randr_get_monitors_reply_t *monitors = ws_reply(requests.monitors.sequence);
	wsd.n_monitors = 0;
	if (monitors != NULL) {
		auto it = xcb_randr_get_monitors_monitors_iterator(monitors);
		for (; it.rem && wsd.n_monitors < WS_MAX_MONITORS;
		     xcb_randr_monitor_info_next(&it)) {
			wsd.monitors[wsd.n_monitors++] = (struct ws_rect){
			    .x1 = it.data->x,
			    .y1 = it.data->y,
			    .x2 = it.data->x + it.data->width,
			    .y2 = it.data->y + it.data->height,
			};
		}
		free(monitors);
	}
	return changed;
}

/// Read the root window properties and the monitors, in one round trip.
static bool ws_read_root(void) {
	return ws_read_root_reply(ws_read_root_request());
}

/// Desktop of a top-level window, or of the client window in it when the window manager
/// puts clients in frames. Sets `*client` to the window that has it.
static int64_t ws_toplevel_desktop(xcb_window_t w, xcb_window_t *client) {
	*client = w;
	auto desktop = ws_window_desktop(w);
	if (desktop >= 0) {
		return desktop;
	}
	auto tree = xcb_query_tree_reply(wsd.c, xcb_query_tree(wsd.c, w), NULL);
	if (tree == NULL) {
		return -1;
	}
	auto children = xcb_query_tree_children(tree);
	for (int i = 0; i < xcb_query_tree_children_length(tree) && desktop < 0; i++) {
		desktop = ws_window_desktop(children[i]);
		*client = children[i];
	}
	free(tree);
	return desktop;
}

/// Find out which workspace each monitor shows, from the windows on screen.
static void ws_seed_from_windows(void) {
	auto tree = xcb_query_tree_reply(wsd.c, xcb_query_tree(wsd.c, wsd.root), NULL);
	if (tree == NULL) {
		return;
	}
	auto children = xcb_query_tree_children(tree);
	for (int i = 0; i < xcb_query_tree_children_length(tree); i++) {
		auto attr = xcb_get_window_attributes_reply(
		    wsd.c, xcb_get_window_attributes(wsd.c, children[i]), NULL);
		// Not filtering on override-redirect: i3 sets it on its frames. Menus and
		// tooltips have no desktop, so they are skipped anyway.
		bool on_screen = attr != NULL && attr->map_state == XCB_MAP_STATE_VIEWABLE;
		free(attr);
		xcb_window_t client = XCB_NONE;
		auto desktop = on_screen ? ws_toplevel_desktop(children[i], &client) : -1;
		if (desktop >= 0) {
			ws_get_monitor((unsigned)desktop, client);
		}
	}
	free(tree);
}

/// Take the root window replies, and record a switch if the monitor of the current
/// desktop now shows another workspace.
static void ws_refresh(struct ws_root_requests requests) {
	bool root_changed = ws_read_root_reply(requests);
	if (wsd.current < 0 || wsd.current >= wsd.n_names) {
		return;
	}
	auto desktop = (unsigned)wsd.current;
	auto m = ws_get_monitor(desktop, XCB_NONE);
	if (m == NULL) {
		return;
	}
	auto shown = ws_shown(m);
	if (strcmp(shown->name, wsd.names[desktop]) != 0) {
		ws_add_switch(m, m->n_visits, desktop, XCB_NONE);
	} else if (root_changed && ws_switch_is_recent(shown)) {
		// The window manager is still updating the root window for this switch.
		ws_switch_touch(shown);
	}
}

/// Direction for a window of workspace `desktop` that shows.
static int ws_direction_for_show(xcb_window_t w, unsigned desktop, const char **reason) {
	auto name = wsd.names[desktop];
	auto m = ws_get_monitor(desktop, w);
	if (m == NULL) {
		*reason = "too many monitors";
		return 0;
	}
	auto k = ws_find_visit(m, name);
	if (k >= 1 && ws_switch_is_recent(&m->visits[k])) {
		*reason = "workspace was switched to";
		ws_switch_touch(&m->visits[k]);
		return ws_switch_direction(m, (unsigned)k);
	}
	if (k == (int)m->n_visits - 1) {
		*reason = "no recent switch to its workspace";
		return 0;
	}

	// A window manager only shows windows of visible workspaces, so this is a switch
	// the root window didn't tell us about. Either the root window is behind (some
	// window managers map the new windows first), and this switch comes after the
	// last one we know. Or the root window is ahead (when switching fast, we may read
	// it after the next switch is done), and this switch came before it.
	auto shown = ws_shown(m);
	k = (int)m->n_visits;
	if (m->n_visits >= 2 && shown->from_root && ws_switch_is_recent(shown)) {
		k = (int)m->n_visits - 1;
	}
	ws_add_switch(m, (unsigned)k, desktop, w);
	k = ws_find_visit(m, name);
	*reason = "workspace was switched to, the root window did not say so yet";
	return ws_switch_direction(m, (unsigned)k);
}

/// Direction for a window of workspace `desktop` that hides.
static int ws_direction_for_hide(unsigned desktop, const char **reason) {
	auto m = ws_find_monitor(desktop);
	if (m == NULL) {
		*reason = "nothing known about its monitor";
		return 0;
	}
	// Find the latest switch away from this workspace.
	for (unsigned k = m->n_visits - 1; k >= 1; k--) {
		if (strcmp(m->visits[k - 1].name, wsd.names[desktop]) != 0) {
			continue;
		}
		if (!ws_switch_is_recent(&m->visits[k])) {
			break;
		}
		*reason = "workspace was switched away from";
		ws_switch_touch(&m->visits[k]);
		return ws_switch_direction(m, k);
	}
	*reason = "no recent switch away from its workspace";
	return 0;
}

/// The monitor that fully contains the window whose geometry was asked for, or the
/// whole screen.
static struct ws_rect ws_window_monitor(xcb_get_geometry_cookie_t cookie) {
	xcb_get_geometry_reply_t *g = ws_reply(cookie.sequence);
	if (g == NULL) {
		return wsd.screen;
	}
	struct ws_rect r = {
	    .x1 = g->x,
	    .y1 = g->y,
	    .x2 = g->x + g->width + g->border_width * 2,
	    .y2 = g->y + g->height + g->border_width * 2,
	};
	free(g);
	for (int i = 0; i < wsd.n_monitors; i++) {
		auto e = &wsd.monitors[i];
		if (e->x1 <= r.x1 && r.x2 <= e->x2 && e->y1 <= r.y1 && r.y2 <= e->y2) {
			return *e;
		}
	}
	return wsd.screen;
}

/// The name of a window, for the log.
static char *ws_window_name(xcb_window_t w) {
	auto prop = ws_get_prop(w, wsd.a_net_wm_name, wsd.a_utf8_string, 256);
	if (prop == NULL) {
		prop = ws_get_prop(w, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 256);
	}
	if (prop == NULL) {
		return strdup("");
	}
	auto len = (size_t)xcb_get_property_value_length(prop);
	char *ret = strndup(xcb_get_property_value(prop), len);
	free(prop);
	return ret;
}

/// Work out all our variables for a window that starts an animation. Every request goes
/// out before we wait for any reply, so a start costs one round trip to the X server.
static void ws_compute(const struct picom_animation_start *start, double *values) {
	auto now = ws_now_us();
	wsd.n_waits = 0;
	bool show =
	    strcmp(start->trigger, "open") == 0 || strcmp(start->trigger, "show") == 0;
	bool hide = strcmp(start->trigger, "hide") == 0;
	// A window that closes is gone (destroyed, or reparented away). There is nothing
	// to ask about it.
	bool gone = strcmp(start->trigger, "close") == 0;
	xcb_window_t client = start->client != XCB_NONE ? start->client : start->window;

	// The root window is read again on the first call in each frame, and the windows
	// that start in the same frame share that. Only shows and hides can be part of a
	// workspace switch. The window's own geometry is asked for at the same time, so
	// that a window without a single workspace costs no second round trip.
	bool new_frame = start->frame != wsd.frame;
	struct ws_root_requests root = {0};
	xcb_get_property_cookie_t desktop_request = {0};
	xcb_get_geometry_cookie_t geometry_request = {0};
	if (new_frame) {
		root = ws_read_root_request();
	}
	if (show || hide) {
		desktop_request = ws_window_desktop_request(client);
	}
	if (!gone) {
		geometry_request = xcb_get_geometry(wsd.c, start->window);
	}

	if (new_frame) {
		wsd.frame = start->frame;
		wsd.frame_time_us = now;
		ws_refresh(root);
	}
	const char *reason = "window is not on one workspace";
	int direction = 0;
	int monitor = -1;
	if (show || hide) {
		auto desktop = ws_window_desktop_reply(desktop_request);
		if (desktop >= 0) {
			monitor = ws_desktop_monitor((unsigned)desktop);
			direction =
			    show ? ws_direction_for_show(client, (unsigned)desktop, &reason)
			         : ws_direction_for_hide((unsigned)desktop, &reason);
		}
	} else {
		reason = "not a show or hide";
	}
	auto rect = wsd.screen;
	if (monitor >= 0) {
		rect = wsd.monitors[monitor];
		xcb_discard_reply(wsd.c, geometry_request.sequence);
	} else if (!gone) {
		rect = ws_window_monitor(geometry_request);
	}
	values[0] = direction;
	values[1] = rect.x1;
	values[2] = rect.y1;
	values[3] = rect.x2 - rect.x1;
	values[4] = rect.y2 - rect.y1;

	if (wsd.log != NULL) {
		// Measured before the name lookup, which only the log needs.
		auto took = ws_now_us() - now;
		auto waits = wsd.n_waits;
		char *name = ws_window_name(client);
		ws_log("%s of window %#010x (%s): workspace-direction = %d (%s), "
		       "workspace monitor %d, %llu us, %u waits for X",
		       start->trigger, start->window, name, direction, reason, monitor,
		       (unsigned long long)took, waits);
		free(name);
	}

	// Drop the events the server sends to every client (MappingNotify). Nothing reads
	// them otherwise, and they would pile up in the connection's queue.
	xcb_generic_event_t *event;
	while ((event = xcb_poll_for_queued_event(wsd.c)) != NULL) {
		free(event);
	}
}

/// picom asks for each variable on its own, `user_data` is its index. We work them all
/// out on the first call for a window in a frame, and answer the other calls from that.
static double ws_provide(const struct picom_animation_start *start, void *user_data) {
	static struct {
		uint64_t frame;
		uint32_t window;
		double values[WS_N_VARIABLES];
	} last;
	if (start->frame != last.frame || start->window != last.window) {
		last.frame = start->frame;
		last.window = start->window;
		ws_compute(start, last.values);
	}
	return last.values[(uintptr_t)user_data];
}

__attribute__((constructor)) static void ws_init(void) {
	auto log_path = getenv("WS_DIRECTION_LOG");
	if (log_path != NULL && *log_path) {
		wsd.log = fopen(log_path, "a");
	}
	auto api = picom_api_get_interfaces(0, 2, "ws-direction");
	if (api == NULL) {
		fprintf(stderr, "ws-direction: picom is too old, "
		                "it needs plugin API 0.2\n");
		return;
	}

	int screen_num = 0;
	wsd.c = xcb_connect(NULL, &screen_num);
	if (xcb_connection_has_error(wsd.c)) {
		fprintf(stderr, "ws-direction: cannot connect to the X server, "
		                "not adding the workspace-* variables\n");
		xcb_disconnect(wsd.c);
		wsd.c = NULL;
		return;
	}
	auto it = xcb_setup_roots_iterator(xcb_get_setup(wsd.c));
	for (; screen_num > 0 && it.rem; screen_num--) {
		xcb_screen_next(&it);
	}
	if (!it.rem) {
		fprintf(stderr, "ws-direction: the display's screen does not exist, "
		                "not adding the workspace-* variables\n");
		xcb_disconnect(wsd.c);
		wsd.c = NULL;
		return;
	}
	wsd.root = it.data->root;
	wsd.a_current_desktop = ws_atom("_NET_CURRENT_DESKTOP");
	wsd.a_desktop_names = ws_atom("_NET_DESKTOP_NAMES");
	wsd.a_desktop_viewport = ws_atom("_NET_DESKTOP_VIEWPORT");
	wsd.a_wm_desktop = ws_atom("_NET_WM_DESKTOP");
	wsd.a_utf8_string = ws_atom("UTF8_STRING");
	wsd.a_net_wm_name = ws_atom("_NET_WM_NAME");

	// Learn what each monitor shows now, before the first switch.
	wsd.frame_time_us = ws_now_us();
	ws_read_root();
	ws_seed_from_windows();
	if (wsd.current >= 0 && wsd.current < wsd.n_names) {
		ws_get_monitor((unsigned)wsd.current, XCB_NONE);
	}

	// Only now: a variable must never exist without the connection that gives it a
	// value.
	for (uintptr_t i = 0; i < WS_N_VARIABLES; i++) {
		if (!api->add_animation_variable("plugin-ws-direction", ws_variable_names[i],
		                                 ws_provide, (void *)i)) {
			fprintf(stderr, "ws-direction: could not add plugin-ws-direction-%s\n",
			        ws_variable_names[i]);
			return;
		}
	}
}
