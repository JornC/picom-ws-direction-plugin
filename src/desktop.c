// SPDX-License-Identifier: MPL-2.0
// Copyright (c) JornC

/// The desktop picture: the monitors, the workspaces and the managed windows, read from
/// the X server.
///
/// Each read costs two round trips to the X server: one for the root window, the
/// top-level windows and the list of managed windows, one for the desktop, state, place,
/// map state and frame of each managed window, with the desktop names and the current
/// desktop asked again after them. If the names changed in between, the desktops of the
/// windows may not match them, and we read again. Usually a round trip is well under a
/// millisecond, but the reply waits behind whatever the server is doing (other clients,
/// the window manager's own burst of requests during a switch, picom's drawing of the
/// previous frame), so it can take tens of milliseconds. So the frame callback waits at
/// most WS_READ_WAIT_US for the replies. If they are not in by then, it pauses the screen
/// (picom draws nothing and starts no animation, and what changed meanwhile animates once
/// the pause ends) and takes them in a later pass. A start that picom asks us about (the
/// provider) waits for the replies, on picom's main thread; later starts in the same pass
/// cost none, except for a window the window manager doesn't manage: one more round trip
/// for its place.
///
/// A read is a picture at one moment, but the maps and unmaps that come between two reads
/// are what tells which switch each show and hide is part of. Our connection gets them as
/// events (SubstructureNotify on the root window), in the order the X server made them.
/// The sequence number of an event says after which of our requests the server made it,
/// so we know whether the read saw it: an event belongs to the read that asked about its
/// window after it. ws_reconcile takes the events of each read in order.

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
	for (unsigned i = 0; name != NULL && i < wsd.n_names; i++) {
		if (strcmp(wsd.names[i], name) == 0) {
			return i;
		}
	}
	return -1;
}

bool ws_rect_has_point(struct ws_rect r, int32_t x, int32_t y) {
	return r.x1 <= x && x < r.x2 && r.y1 <= y && y < r.y2;
}

/// The index of the monitor whose top left corner is at x, y, or -1.
static int ws_monitor_at_corner(int32_t x, int32_t y) {
	for (int i = 0; i < wsd.n_monitors; i++) {
		if (wsd.monitors[i].rect.x1 == x && wsd.monitors[i].rect.y1 == y) {
			return i;
		}
	}
	return -1;
}

/// Work out whether the viewports say which monitor each desktop is on. i3 sets the
/// viewport of each workspace to the corner of its output. Other window managers set
/// every viewport to 0,0 (EWMH asks that without large desktops), and then a desktop
/// spans every monitor; with large desktops, a viewport is a place on the desktop, not a
/// monitor. So we take them as monitors only where they differ and each one is the
/// corner of a monitor.
void ws_update_viewport_monitors(void) {
	auto n = wsd.n_viewports / 2;
	bool differ = false, corners = n > 0;
	for (unsigned d = 0; d < n; d++) {
		differ |= wsd.viewports[d * 2] != wsd.viewports[0] ||
		          wsd.viewports[d * 2 + 1] != wsd.viewports[1];
		corners &= ws_monitor_at_corner((int32_t)wsd.viewports[d * 2],
		                                (int32_t)wsd.viewports[d * 2 + 1]) >= 0;
	}
	wsd.viewport_monitors = differ && corners;
}

/// Index of the monitor that shows a desktop, -1 for "the whole screen".
int ws_desktop_monitor(unsigned desktop) {
	if (!wsd.viewport_monitors || desktop >= wsd.n_viewports / 2) {
		return -1;
	}
	return ws_monitor_at_corner((int32_t)wsd.viewports[desktop * 2],
	                            (int32_t)wsd.viewports[desktop * 2 + 1]);
}

/// Index of the monitor with RandR name `name`: -1 for XCB_NONE, the whole screen, and
/// -2 if there is no such monitor any more.
int ws_monitor_of_name(xcb_atom_t name) {
	if (name == XCB_NONE) {
		return -1;
	}
	for (int i = 0; i < wsd.n_monitors; i++) {
		if (wsd.monitors[i].name == name) {
			return i;
		}
	}
	return -2;
}

/// The rect of a monitor, the whole screen for -1.
struct ws_rect ws_monitor_rect(int index) {
	return index >= 0 ? wsd.monitors[index].rect : wsd.screen;
}

/// Ask for _NET_WM_DESKTOP of a window. Pair with ws_window_desktop_reply.
static xcb_get_property_cookie_t ws_window_desktop_request(xcb_window_t w) {
	return ws_prop_request(w, wsd.a_wm_desktop, XCB_ATOM_CARDINAL, 1);
}

/// Take the reply to ws_window_desktop_request. Returns -2 for a window on all desktops,
/// -1 if the property is not set, or not a single desktop we know.
static int64_t ws_window_desktop_reply(xcb_get_property_cookie_t cookie) {
	auto prop = ws_prop_reply(cookie);
	int64_t ret = -1;
	if (prop != NULL && prop->format == 32) {
		ret = *(uint32_t *)xcb_get_property_value(prop);
	}
	free(prop);
	if (ret == WS_ALL_DESKTOPS) {
		return -2;
	}
	return ret >= wsd.n_names ? -1 : ret;
}

/// The requests of the second round trip for one managed window.
struct ws_window_requests {
	xcb_window_t id;
	xcb_get_property_cookie_t desktop, state;
	xcb_get_geometry_cookie_t geometry;
	xcb_translate_coordinates_cookie_t origin;
	xcb_get_window_attributes_cookie_t attributes;
	xcb_query_tree_cookie_t tree;
};

static void ws_free_clients(struct ws_client *clients, unsigned n) {
	for (unsigned i = 0; i < n; i++) {
		free(clients[i].workspace);
	}
	free(clients);
}

const struct ws_client *ws_find_client(xcb_window_t id) {
	for (unsigned i = 0; i < wsd.n_clients; i++) {
		if (wsd.clients[i].id == id) {
			return &wsd.clients[i];
		}
	}
	return NULL;
}

const struct ws_client *ws_find_client_by_frame(xcb_window_t frame) {
	for (unsigned i = 0; i < wsd.n_clients; i++) {
		if (wsd.clients[i].frame == frame) {
			return &wsd.clients[i];
		}
	}
	return NULL;
}

/// Start reading the root window and the managed windows again: send the first round
/// trip. ws_read_advance takes the replies.
void ws_read_start(void) {
	wsd.read.root = (struct ws_root_requests){
	    .viewports = ws_prop_request(wsd.root, wsd.a_desktop_viewport,
	                                 XCB_ATOM_CARDINAL, WS_MAX_DESKTOPS * 2),
	    .names = ws_prop_request(wsd.root, wsd.a_desktop_names, wsd.a_utf8_string,
	                             UINT32_MAX / 4),
	    .list = ws_prop_request(wsd.root, wsd.a_client_list, XCB_ATOM_WINDOW,
	                            UINT32_MAX / 4),
	    .screen = xcb_get_geometry(wsd.c, wsd.root),
	    .monitors = xcb_randr_get_monitors(wsd.c, wsd.root, 1),
	    .tree = xcb_query_tree(wsd.c, wsd.root),
	};
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

/// Take the first round trip's replies, and send the second round trip: for every
/// managed window its desktop, state, place, map state and parent, then the desktop
/// names and the current desktop again.
static void ws_read_windows_request(void) {
	auto root = &wsd.read.root;
	wsd.read.names = ws_prop_reply(root->names);
	auto tree = xcb_query_tree_reply(wsd.c, root->tree, NULL);
	wsd.read.n_toplevels = 0;
	wsd.read.toplevels = NULL;
	if (tree != NULL) {
		auto n = (unsigned)xcb_query_tree_children_length(tree);
		wsd.read.toplevels = malloc(n * sizeof(xcb_window_t) + 1);
		if (wsd.read.toplevels == NULL) {
			abort();
		}
		memcpy(wsd.read.toplevels, xcb_query_tree_children(tree),
		       n * sizeof(xcb_window_t));
		wsd.read.n_toplevels = n;
		free(tree);
	}
	auto list = ws_prop_reply(root->list);
	unsigned n = list != NULL && list->format == 32
	                 ? (unsigned)xcb_get_property_value_length(list) / 4
	                 : 0;
	wsd.read.windows = calloc(n + 1, sizeof(*wsd.read.windows));
	if (wsd.read.windows == NULL) {
		abort();
	}
	for (unsigned i = 0; i < n; i++) {
		xcb_window_t id = ((const xcb_window_t *)xcb_get_property_value(list))[i];
		wsd.read.windows[i] = (struct ws_window_requests){
		    .id = id,
		    .desktop = ws_window_desktop_request(id),
		    .state = ws_prop_request(id, wsd.a_wm_state, XCB_ATOM_ATOM, 64),
		    .geometry = xcb_get_geometry(wsd.c, id),
		    .origin = xcb_translate_coordinates(wsd.c, id, wsd.root, 0, 0),
		    .attributes = xcb_get_window_attributes(wsd.c, id),
		    .tree = xcb_query_tree(wsd.c, id),
		};
	}
	wsd.read.n_windows = n;
	free(list);
	wsd.read.current =
	    ws_prop_request(wsd.root, wsd.a_current_desktop, XCB_ATOM_CARDINAL, 1);
	wsd.read.names_again = ws_prop_request(wsd.root, wsd.a_desktop_names,
	                                       wsd.a_utf8_string, UINT32_MAX / 4);
	wsd.read.sync = xcb_get_input_focus(wsd.c);
	wsd.read.step = WS_READ_WINDOWS;
	xcb_flush(wsd.c);
}

/// Throw away the replies of a read whose second round trip is in.
static void ws_read_drop(void) {
	for (unsigned i = 0; i < wsd.read.n_windows; i++) {
		auto r = &wsd.read.windows[i];
		unsigned int sequences[] = {
		    r->desktop.sequence, r->state.sequence,      r->geometry.sequence,
		    r->origin.sequence,  r->attributes.sequence, r->tree.sequence,
		};
		for (size_t j = 0; j < sizeof(sequences) / sizeof(sequences[0]); j++) {
			xcb_discard_reply(wsd.c, sequences[j]);
		}
	}
	xcb_discard_reply(wsd.c, wsd.read.current.sequence);
	auto root = &wsd.read.root;
	free(ws_reply(root->viewports.sequence));
	free(ws_reply(root->screen.sequence));
	free(ws_reply(root->monitors.sequence));
	free(wsd.read.names);
	wsd.read.names = NULL;
	free(wsd.read.toplevels);
	wsd.read.toplevels = NULL;
	free(wsd.read.windows);
	wsd.read.windows = NULL;
	wsd.read.n_windows = 0;
	wsd.read.step = WS_READ_IDLE;
}

static bool
ws_same_value(const xcb_get_property_reply_t *a, const xcb_get_property_reply_t *b) {
	if (a == NULL || b == NULL) {
		return a == b;
	}
	auto len = xcb_get_property_value_length(a);
	return len == xcb_get_property_value_length(b) &&
	       memcmp(xcb_get_property_value(a), xcb_get_property_value(b), (size_t)len) == 0;
}

/// Take _NET_DESKTOP_NAMES. The names are a list of strings, each one ending with a NUL.
/// We add one more NUL in case the last one doesn't have it.
static void ws_take_names(const xcb_get_property_reply_t *names) {
	size_t len = names ? (size_t)xcb_get_property_value_length(names) : 0;
	char *buf = calloc(len + 1, 1);
	if (buf == NULL) {
		abort();
	}
	if (len) {
		memcpy(buf, xcb_get_property_value(names), len);
	}
	free(wsd.names_buf);
	wsd.names_buf = buf;
	wsd.n_names = 0;
	for (size_t start = 0; start < len && wsd.n_names < WS_MAX_DESKTOPS;) {
		wsd.names[wsd.n_names++] = &buf[start];
		start += strlen(&buf[start]) + 1;
	}
}

/// Take the root window's replies of the read.
static void ws_take_root(void) {
	auto root = &wsd.read.root;
	ws_take_names(wsd.read.names);
	free(wsd.read.names);
	wsd.read.names = NULL;

	auto current = ws_prop_reply(wsd.read.current);
	wsd.current = current != NULL && current->format == 32
	                  ? (int64_t)*(uint32_t *)xcb_get_property_value(current)
	                  : -1;
	free(current);

	auto viewports = ws_prop_reply(root->viewports);
	wsd.n_viewports = 0;
	if (viewports != NULL && viewports->format == 32) {
		auto n = (unsigned)xcb_get_property_value_length(viewports) / 4;
		wsd.n_viewports = n > WS_MAX_DESKTOPS * 2 ? WS_MAX_DESKTOPS * 2 : n;
		memcpy(wsd.viewports, xcb_get_property_value(viewports), wsd.n_viewports * 4);
	}
	free(viewports);

	xcb_get_geometry_reply_t *screen = ws_reply(root->screen.sequence);
	if (screen != NULL) {
		wsd.screen = (struct ws_rect){.x2 = screen->width, .y2 = screen->height};
		free(screen);
	}

	xcb_randr_get_monitors_reply_t *monitors = ws_reply(root->monitors.sequence);
	wsd.n_monitors = 0;
	if (monitors != NULL) {
		auto it = xcb_randr_get_monitors_monitors_iterator(monitors);
		for (; it.rem && wsd.n_monitors < WS_MAX_MONITORS;
		     xcb_randr_monitor_info_next(&it)) {
			wsd.monitors[wsd.n_monitors++] = (struct ws_output){
			    .name = it.data->name,
			    .rect = {.x1 = it.data->x,
			             .y1 = it.data->y,
			             .x2 = it.data->x + it.data->width,
			             .y2 = it.data->y + it.data->height},
			};
		}
		free(monitors);
	}
	ws_update_viewport_monitors();
}

static bool ws_is_toplevel(xcb_window_t w) {
	for (unsigned i = 0; i < wsd.read.n_toplevels; i++) {
		if (wsd.read.toplevels[i] == w) {
			return true;
		}
	}
	return false;
}

/// The top-level window that holds window `w`, whose parent is `parent`: `w` itself, or
/// the frame the window manager put it in. Most window managers use one frame; for more,
/// we go up the tree, one round trip per step.
static xcb_window_t ws_toplevel_of(xcb_window_t w, xcb_window_t parent) {
	while (parent != XCB_NONE && parent != wsd.root && !ws_is_toplevel(w)) {
		w = parent;
		auto tree = xcb_query_tree_reply(wsd.c, xcb_query_tree(wsd.c, w), NULL);
		parent = tree != NULL ? tree->parent : XCB_NONE;
		free(tree);
	}
	return w;
}

/// Take the replies of the second round trip: they are the managed windows now. Sets
/// `samples[i]` to the sequence number of the request that asked for the map state of
/// window `i`.
static void ws_take_windows(uint32_t *samples) {
	auto n = wsd.read.n_windows;
	wsd.clients = malloc((n + 1) * sizeof(*wsd.clients));
	wsd.n_clients = 0;
	if (wsd.clients == NULL) {
		abort();
	}
	for (unsigned i = 0; i < n; i++) {
		auto r = &wsd.read.windows[i];
		auto desktop = ws_window_desktop_reply(r->desktop);
		bool sticky = desktop == -2;
		desktop = sticky ? -1 : desktop;
		auto state = ws_prop_reply(r->state);
		if (state != NULL && state->format == 32) {
			const xcb_atom_t *atoms = xcb_get_property_value(state);
			auto n_atoms = xcb_get_property_value_length(state) / 4;
			for (int j = 0; j < n_atoms; j++) {
				if (atoms[j] == wsd.a_wm_state_hidden) {
					desktop = -1;
					sticky = false;
				}
			}
		}
		free(state);
		xcb_get_geometry_reply_t *g = ws_reply(r->geometry.sequence);
		xcb_translate_coordinates_reply_t *origin = ws_reply(r->origin.sequence);
		xcb_get_window_attributes_reply_t *attr = ws_reply(r->attributes.sequence);
		xcb_query_tree_reply_t *tree = ws_reply(r->tree.sequence);
		if (g != NULL && origin != NULL && attr != NULL && tree != NULL) {
			char *workspace = desktop >= 0 ? strdup(wsd.names[desktop]) : NULL;
			if (desktop >= 0 && workspace == NULL) {
				abort();
			}
			samples[wsd.n_clients] = r->attributes.sequence;
			wsd.clients[wsd.n_clients++] = (struct ws_client){
			    .id = r->id,
			    .frame = ws_toplevel_of(r->id, tree->parent),
			    .desktop = desktop,
			    .rect = {.x1 = origin->dst_x,
			             .y1 = origin->dst_y,
			             .x2 = origin->dst_x + g->width,
			             .y2 = origin->dst_y + g->height},
			    .workspace = workspace,
			    .sticky = sticky,
			    .viewable = attr->map_state == XCB_MAP_STATE_VIEWABLE,
			};
		}
		free(g);
		free(origin);
		free(attr);
		free(tree);
	}
}

/// Whether the read saw event `e`: the read asked about its window after the X server
/// made it. `samples` as ws_take_windows sets it.
static bool ws_read_saw(const struct ws_event *e, const uint32_t *samples,
                        uint32_t current, uint32_t end) {
	// The sequence numbers wrap around; their difference doesn't.
	if (e->type == WS_EVENT_CURRENT) {
		return (int32_t)(e->sequence - current) < 0;
	}
	for (unsigned i = 0; i < wsd.n_clients; i++) {
		if (wsd.clients[i].frame == e->window) {
			return (int32_t)(e->sequence - samples[i]) < 0;
		}
	}
	return (int32_t)(e->sequence - end) < 0;
}

/// Take the replies of the read, waiting up to `wait_us` for each of its round trips
/// (see ws_read_wait), and record the switches since the read before. Returns whether
/// the read is done; if not, call it again.
bool ws_read_advance(uint64_t wait_us) {
	while (true) {
		if (wsd.read.step == WS_READ_ROOT) {
			if (!ws_read_wait(wait_us)) {
				return false;
			}
			ws_read_windows_request();
		}
		if (!ws_read_wait(wait_us)) {
			return false;
		}
		auto names = ws_prop_reply(wsd.read.names_again);
		bool same = ws_same_value(names, wsd.read.names);
		free(names);
		if (same) {
			break;
		}
		// The desktops of the windows may be numbers into the new names.
		ws_log("The desktop names changed during the read, reading again");
		ws_read_drop();
		ws_read_start();
	}

	auto prev = wsd.clients;
	auto n_prev = wsd.n_clients;
	auto prev_current = wsd.current;
	ws_take_root();
	auto samples = malloc((wsd.read.n_windows + 1) * sizeof(uint32_t));
	if (samples == NULL) {
		abort();
	}
	ws_take_windows(samples);
	free(wsd.read.windows);
	wsd.read.windows = NULL;
	wsd.read.n_windows = 0;
	free(wsd.stack);
	wsd.stack = wsd.read.toplevels;
	wsd.n_stack = wsd.read.n_toplevels;
	wsd.read.toplevels = NULL;

	// The events up to our last request are in xcb's queue now. Those the read saw
	// are its own; the others wait for the next read.
	ws_take_events();
	struct ws_event *seen = malloc((wsd.n_events + 1) * sizeof(*seen));
	if (seen == NULL) {
		abort();
	}
	unsigned n_seen = 0, n_left = 0;
	for (unsigned i = 0; i < wsd.n_events; i++) {
		if (ws_read_saw(&wsd.events[i], samples, wsd.read.current.sequence,
		                wsd.read.names_again.sequence)) {
			seen[n_seen++] = wsd.events[i];
		} else {
			wsd.events[n_left++] = wsd.events[i];
		}
	}
	wsd.n_events = n_left;
	free(samples);

	ws_reconcile(prev, n_prev, seen, n_seen, prev_current);
	free(seen);
	ws_free_clients(prev, n_prev);
	wsd.read.step = WS_READ_IDLE;
	wsd.read_frame = wsd.frame;
	ws_log("Read the root window and %u windows in %" PRIu64 " us%s", wsd.n_clients,
	       ws_now_us() - wsd.read.start_us, wsd.read.paused ? ", the screen paused" : "");
	return true;
}

/// Read the root window and the managed windows again, in two round trips, waiting for
/// the replies, and record the switches since the read before. A read that waits already
/// is finished.
void ws_read(void) {
	if (wsd.read.step == WS_READ_IDLE) {
		ws_read_start();
	}
	ws_read_advance(UINT64_MAX);
}
