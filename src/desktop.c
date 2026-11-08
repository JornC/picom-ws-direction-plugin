// SPDX-License-Identifier: MPL-2.0
// Copyright (c) JornC

/// The desktop picture: the monitors, the workspaces and the managed windows, read from
/// the X server.
///
/// In a pass in which nothing changed, the frame callback only takes what the X server
/// already sent us, without waiting. Each read costs two round trips to the X server: one
/// for the root window and the list of managed windows, one for the desktop, state and
/// place of each managed window. Usually a round trip is well under a millisecond, but
/// the reply waits behind whatever the server is doing (other clients, the window
/// manager's own burst of requests during a switch, picom's drawing of the previous
/// frame), so it can take tens of milliseconds. So the frame callback waits at most
/// WS_READ_WAIT_US for the replies. If they are not in by then, it pauses the screen
/// (picom draws nothing and starts no animation, and what changed meanwhile animates once
/// the pause ends) and takes them in a later pass, then starts the switch. A start that
/// picom asks us about (the provider) waits for the replies, on picom's main thread;
/// later starts in the same pass cost none, except for a window the window manager
/// doesn't manage: one more round trip for its place.

#include <inttypes.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>

#include <xcb/xcbext.h>

#include "ws_direction.h"

/// _NET_WM_DESKTOP value of windows that are on all desktops.
#define WS_ALL_DESKTOPS 0xFFFFFFFF

/// The atom `name`, in a round trip of its own, or XCB_ATOM_NONE.
xcb_atom_t ws_atom(const char *name) {
	auto reply = xcb_intern_atom_reply(
	    wsd.c, xcb_intern_atom(wsd.c, 0, (uint16_t)strlen(name), name), NULL);
	xcb_atom_t ret = reply != NULL ? reply->atom : XCB_ATOM_NONE;
	free(reply);
	return ret;
}

/// The requests for the desktop, the state and the place of a managed window.
struct ws_window_requests {
	xcb_window_t id;
	xcb_get_property_cookie_t desktop, state;
	xcb_get_geometry_cookie_t geometry;
	xcb_translate_coordinates_cookie_t origin;
};

/// Take the reply to a request, waiting for it if it hasn't arrived yet. Returns NULL if
/// the request failed. The caller frees the reply.
void *ws_reply(unsigned int sequence) {
	void *reply = NULL;
	xcb_generic_error_t *err = NULL;
	if (!xcb_poll_for_reply(wsd.c, sequence, &reply, &err)) {
		wsd.n_waits++;
		reply = xcb_wait_for_reply(wsd.c, sequence, &err);
	}
	free(err);
	return reply;
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
xcb_get_property_reply_t *
ws_get_prop(xcb_window_t w, xcb_atom_t atom, xcb_atom_t type, uint32_t max_len) {
	return ws_prop_reply(ws_prop_request(w, atom, type, max_len));
}

/// Find a desktop by name. Returns its index, or -1 if there is no such desktop.
int64_t ws_find_desktop(const char *name) {
	for (unsigned i = 0; i < wsd.n_names; i++) {
		if (strcmp(wsd.names[i], name) == 0) {
			return i;
		}
	}
	return -1;
}

/// Get the viewport of a desktop. Returns false if the window manager didn't set one.
bool ws_desktop_viewport(unsigned desktop, int32_t *x, int32_t *y) {
	if (desktop >= wsd.n_viewports / 2) {
		return false;
	}
	*x = (int32_t)wsd.viewports[desktop * 2];
	*y = (int32_t)wsd.viewports[desktop * 2 + 1];
	return true;
}

bool ws_rect_has_point(struct ws_rect r, int32_t x, int32_t y) {
	return r.x1 <= x && x < r.x2 && r.y1 <= y && y < r.y2;
}

/// Index of the monitor that contains a viewport. Returns -1 if there is no viewport, or
/// it is not on any monitor. -1 stands for "all monitors".
int ws_monitor_index(bool has_viewport, int32_t x, int32_t y) {
	if (!has_viewport) {
		return -1;
	}
	for (int i = 0; i < wsd.n_monitors; i++) {
		if (ws_rect_has_point(wsd.monitors[i], x, y)) {
			return i;
		}
	}
	return -1;
}

/// Index of the monitor that shows a desktop, -1 for "all monitors".
int ws_desktop_monitor(unsigned desktop) {
	int32_t x = 0, y = 0;
	bool has_viewport = ws_desktop_viewport(desktop, &x, &y);
	return ws_monitor_index(has_viewport, x, y);
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

/// Ask for _NET_CLIENT_LIST. Pair with ws_read_windows_request.
static xcb_get_property_cookie_t ws_client_list_request(void) {
	return ws_prop_request(wsd.root, wsd.a_client_list, XCB_ATOM_WINDOW, UINT32_MAX / 4);
}

static const struct ws_client *
ws_find_client_in(const struct ws_client *clients, unsigned n, xcb_window_t id) {
	for (unsigned i = 0; i < n; i++) {
		if (clients[i].id == id) {
			return &clients[i];
		}
	}
	return NULL;
}

const struct ws_client *ws_find_client(xcb_window_t id) {
	return ws_find_client_in(wsd.clients, wsd.n_clients, id);
}

static void ws_free_clients(struct ws_client *clients, unsigned n) {
	for (unsigned i = 0; i < n; i++) {
		free(clients[i].workspace);
	}
	free(clients);
}

/// Whether a window that was `before` at the read before, and is on workspace
/// `workspace` now, moved from one workspace to another.
static bool ws_moved(const struct ws_client *before, const char *workspace) {
	return before != NULL && before->workspace != NULL && workspace != NULL &&
	       strcmp(before->workspace, workspace) != 0;
}

/// Take the reply to ws_client_list_request, and ask for the desktop, the state and the
/// place of every managed window, all in one more round trip. Pair with
/// ws_read_windows_reply.
static void ws_read_windows_request(xcb_get_property_cookie_t list_request) {
	auto list = ws_prop_reply(list_request);
	wsd.read.windows = NULL;
	wsd.read.n_windows = 0;
	if (list == NULL || list->format != 32) {
		free(list);
		return;
	}
	auto n = (unsigned)xcb_get_property_value_length(list) / 4;
	const xcb_window_t *ids = xcb_get_property_value(list);
	wsd.read.windows = calloc(n, sizeof(*wsd.read.windows));
	if (wsd.read.windows == NULL && n > 0) {
		abort();
	}
	for (unsigned i = 0; i < n; i++) {
		wsd.read.windows[i] = (struct ws_window_requests){
		    .id = ids[i],
		    .desktop = ws_window_desktop_request(ids[i]),
		    .state = ws_prop_request(ids[i], wsd.a_wm_state, XCB_ATOM_ATOM, 64),
		    .geometry = xcb_get_geometry(wsd.c, ids[i]),
		    .origin = xcb_translate_coordinates(wsd.c, ids[i], wsd.root, 0, 0),
		};
	}
	wsd.read.n_windows = n;
	free(list);
}

/// Take the replies to ws_read_windows_request: they are the managed windows now.
static void ws_read_windows_reply(void) {
	auto old = wsd.clients;
	auto n_old = wsd.n_clients;
	auto n = wsd.read.n_windows;
	wsd.clients = malloc(n * sizeof(*wsd.clients));
	wsd.n_clients = 0;
	if (wsd.clients == NULL && n > 0) {
		abort();
	}
	for (unsigned i = 0; i < n; i++) {
		auto r = &wsd.read.windows[i];
		auto desktop = ws_window_desktop_reply(r->desktop);
		auto state = ws_prop_reply(r->state);
		if (state != NULL && state->format == 32) {
			const xcb_atom_t *atoms = xcb_get_property_value(state);
			auto n_atoms = xcb_get_property_value_length(state) / 4;
			for (int j = 0; j < n_atoms; j++) {
				if (atoms[j] == wsd.a_wm_state_hidden) {
					desktop = -1;
				}
			}
		}
		free(state);
		xcb_get_geometry_reply_t *g = ws_reply(r->geometry.sequence);
		xcb_translate_coordinates_reply_t *origin = ws_reply(r->origin.sequence);
		if (g != NULL && origin != NULL) {
			char *workspace = desktop >= 0 ? strdup(wsd.names[desktop]) : NULL;
			if (desktop >= 0 && workspace == NULL) {
				abort();
			}
			wsd.clients[wsd.n_clients++] = (struct ws_client){
			    .id = r->id,
			    .desktop = desktop,
			    .rect = {.x1 = origin->dst_x,
			             .y1 = origin->dst_y,
			             .x2 = origin->dst_x + g->width,
			             .y2 = origin->dst_y + g->height},
			    .workspace = workspace,
			    .moved = ws_moved(ws_find_client_in(old, n_old, r->id), workspace),
			};
		}
		free(g);
		free(origin);
	}
	free(wsd.read.windows);
	wsd.read.windows = NULL;
	wsd.read.n_windows = 0;
	ws_free_clients(old, n_old);
}

/// Ask for the root window properties, the screen size and the monitors. Pair with
/// ws_read_root_reply.
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
bool ws_read_root_reply(struct ws_root_requests requests) {
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
bool ws_read_root(void) {
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
void ws_seed_from_windows(void) {
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

/// Start reading the root window and the managed windows again: ask for the root window
/// and the window list. ws_read_advance takes the replies.
void ws_read_start(void) {
	wsd.read.root = ws_read_root_request();
	wsd.read.list = ws_client_list_request();
	// Its reply comes after those of all requests before it.
	wsd.read.sync = xcb_get_input_focus(wsd.c);
	wsd.read.step = WS_READ_ROOT;
	wsd.read.start_us = ws_now_us();
	wsd.read.start_frame = wsd.frame;
	xcb_flush(wsd.c);
}

/// Wait up to `wait_us` for the replies of the step the read is in, for as long as it
/// takes with UINT64_MAX. Returns whether they are in.
static bool ws_read_wait(uint64_t wait_us) {
	if (wait_us == UINT64_MAX) {
		free(ws_reply(wsd.read.sync.sequence));
		return true;
	}
	auto deadline = ws_now_us() + wait_us;
	void *reply = NULL;
	xcb_generic_error_t *err = NULL;
	while (!xcb_poll_for_reply(wsd.c, wsd.read.sync.sequence, &reply, &err)) {
		auto now = ws_now_us();
		if (now >= deadline) {
			return false;
		}
		struct pollfd fd = {.fd = xcb_get_file_descriptor(wsd.c), .events = POLLIN};
		poll(&fd, 1, (int)((deadline - now + 999) / 1000));
	}
	free(reply);
	free(err);
	return true;
}

/// Take the replies of the read, waiting up to `wait_us` for each of its two round trips
/// (see ws_read_wait), and record a switch if the monitor of the current desktop now
/// shows another workspace. Returns whether the read is done; if not, call it again.
bool ws_read_advance(uint64_t wait_us) {
	if (wsd.read.step == WS_READ_ROOT) {
		if (!ws_read_wait(wait_us)) {
			return false;
		}
		// The root window's replies wait in the queue until those of the windows
		// are in too, so a switch is recorded in the pass that starts it.
		ws_read_windows_request(wsd.read.list);
		wsd.read.sync = xcb_get_input_focus(wsd.c);
		wsd.read.step = WS_READ_WINDOWS;
		xcb_flush(wsd.c);
	}
	if (!ws_read_wait(wait_us)) {
		return false;
	}
	ws_refresh(wsd.read.root);
	ws_read_windows_reply();
	wsd.read.step = WS_READ_IDLE;
	wsd.read_frame = wsd.frame;
	ws_log("Read the root window and %u windows in %" PRIu64 " us%s", wsd.n_clients,
	       ws_now_us() - wsd.read.start_us, wsd.read.paused ? ", the screen paused" : "");
	return true;
}

/// Read the root window and the managed windows again, in two round trips, waiting for
/// the replies, and record a switch if the monitor of the current desktop now shows
/// another workspace. A read that waits already is finished.
void ws_read(void) {
	if (wsd.read.step == WS_READ_IDLE) {
		ws_read_start();
	}
	ws_read_advance(UINT64_MAX);
}
