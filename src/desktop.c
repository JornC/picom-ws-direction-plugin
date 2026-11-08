// SPDX-License-Identifier: MPL-2.0
// Copyright (c) JornC

/// The desktop picture: the monitors, the workspaces and the managed windows, read from
/// the X server.

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

/// Index of the monitor that contains a viewport. Returns -1 if there is no viewport, or
/// it is not on any monitor. -1 stands for "all monitors".
int ws_monitor_index(bool has_viewport, int32_t x, int32_t y) {
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
int ws_desktop_monitor(unsigned desktop) {
	int32_t x = 0, y = 0;
	bool has_viewport = ws_desktop_viewport(desktop, &x, &y);
	return ws_monitor_index(has_viewport, x, y);
}

/// Ask for _NET_WM_DESKTOP of a window. Pair with ws_window_desktop_reply.
xcb_get_property_cookie_t ws_window_desktop_request(xcb_window_t w) {
	return ws_prop_request(w, wsd.a_wm_desktop, XCB_ATOM_CARDINAL, 1);
}

/// Take the reply to ws_window_desktop_request. Returns -1 if the property is not set,
/// or not a single desktop we know.
int64_t ws_window_desktop_reply(xcb_get_property_cookie_t cookie) {
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

/// Ask for the root window properties, the screen size and the monitors. Pair with
/// ws_read_root_reply.
struct ws_root_requests ws_read_root_request(void) {
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
