// SPDX-License-Identifier: MPL-2.0
// Copyright (c) JornC

/// Switches, and the variables.
/// An animation is part of a recent switch on the window's monitor when it is a show or
/// open of a window of the workspace switched to, a hide of a window of the workspace
/// switched away from, or any trigger that picom does not fire for a change of the window
/// itself, such as a custom trigger started with StartAnimations (see picom(1)).

#include <stdlib.h>
#include <string.h>

#include "ws_direction.h"

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

/// Index of the monitor that fully contains a window, -1 for "all monitors".
static int ws_rect_monitor(struct ws_rect r) {
	for (int i = 0; i < wsd.n_monitors; i++) {
		auto e = &wsd.monitors[i];
		if (e->x1 <= r.x1 && r.x2 <= e->x2 && e->y1 <= r.y1 && r.y2 <= e->y2) {
			return i;
		}
	}
	return -1;
}

/// The rect of a monitor, the whole screen for -1.
static struct ws_rect ws_monitor_rect(int index) {
	return index >= 0 ? wsd.monitors[index] : wsd.screen;
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

/// A workspace switch as a window sees it: which way it went, and how many workspaces of
/// the monitor it moved. Both are 0 if the window's animation is not part of a switch.
struct ws_move {
	int direction;
	unsigned distance;
};

/// The way from workspace `from` to workspace `to` on the monitor with index `index`. The
/// distance counts the workspaces of the monitor that sort between the two, as they are
/// now: with i3, an empty workspace that isn't shown doesn't exist, so it doesn't count.
static struct ws_move ws_steps(struct ws_order from, struct ws_order to, int index) {
	struct ws_move ret = {.direction = ws_order_compare(to, from)};
	if (ret.direction == 0) {
		return ret;
	}
	auto low = ret.direction > 0 ? from : to;
	auto high = ret.direction > 0 ? to : from;
	ret.distance = 1;
	for (unsigned d = 0; d < wsd.n_names; d++) {
		auto order = ws_order_of(wsd.names[d], d);
		if (ws_desktop_monitor(d) == index && ws_order_compare(order, low) > 0 &&
		    ws_order_compare(order, high) < 0) {
			ret.distance++;
		}
	}
	return ret;
}

/// The switch to `m->visits[k]`, `k` must be at least 1.
static struct ws_move ws_switch_move(const struct ws_monitor *m, unsigned k) {
	auto index = ws_monitor_index(m->has_viewport, m->viewport_x, m->viewport_y);
	return ws_steps(m->visits[k - 1].order, m->visits[k].order, index);
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
	auto move = ws_switch_move(m, k);
	ws_log("Workspace switch on monitor at %d,%d: \"%s\" -> \"%s\", direction %d, "
	       "distance %u (from %s)",
	       m->viewport_x, m->viewport_y, m->visits[k - 1].name, m->visits[k].name,
	       move.direction, move.distance, source);
}

/// Get what we know about the monitor of `desktop`. If we know nothing yet, assume it
/// shows `desktop`. `w` is the window we learned this from, or 0 if it was the root
/// window. Returns NULL if we track too many monitors already.
struct ws_monitor *ws_get_monitor(unsigned desktop, xcb_window_t w) {
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
	// Workspaces can move between monitors.
	m->has_viewport = ws_desktop_viewport(desktop, &m->viewport_x, &m->viewport_y);
	return m;
}

/// Workspaces can change their position in _NET_DESKTOP_NAMES. Sort the ones we remember
/// by where they are now.
static void ws_update_orders(void) {
	for (unsigned i = 0; i < wsd.n_tracked; i++) {
		auto m = &wsd.tracked[i];
		for (unsigned k = 0; k < m->n_visits; k++) {
			auto position = ws_find_desktop(m->visits[k].name);
			if (position >= 0) {
				m->visits[k].order =
				    ws_order_of(m->visits[k].name, (unsigned)position);
			}
		}
	}
}

/// Take the root window replies, and record a switch if the monitor of the current
/// desktop now shows another workspace.
void ws_refresh(struct ws_root_requests requests) {
	bool root_changed = ws_read_root_reply(requests);
	ws_update_orders();
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

/// The switch that a window of workspace `desktop` that shows is part of. Sets `*mp` to
/// what we know about the monitor of `desktop`, and returns `k` for the switch to
/// `(*mp)->visits[k]`, or 0 if the window is not part of a switch.
static unsigned ws_switch_for_show(xcb_window_t w, unsigned desktop,
                                   struct ws_monitor **mp, const char **reason) {
	auto name = wsd.names[desktop];
	auto m = *mp = ws_get_monitor(desktop, w);
	if (m == NULL) {
		*reason = "too many monitors";
		return 0;
	}
	auto k = ws_find_visit(m, name);
	if (k >= 1 && ws_switch_is_recent(&m->visits[k])) {
		*reason = "workspace was switched to";
		ws_switch_touch(&m->visits[k]);
		return (unsigned)k;
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
	*reason = "workspace was switched to, the root window did not say so yet";
	return (unsigned)ws_find_visit(m, name);
}

/// The switch that a window of workspace `desktop` that hides is part of. Like
/// ws_switch_for_show.
static unsigned
ws_switch_for_hide(unsigned desktop, struct ws_monitor **mp, const char **reason) {
	auto m = *mp = ws_find_monitor(desktop);
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
		return k;
	}
	*reason = "no recent switch away from its workspace";
	return 0;
}

/// The switch that any other animation of a window of workspace `desktop` is part of: the
/// latest switch on its monitor, if it is recent. Like ws_switch_for_show.
static unsigned
ws_switch_latest(unsigned desktop, struct ws_monitor **mp, const char **reason) {
	auto m = *mp = ws_find_monitor(desktop);
	if (m == NULL) {
		*reason = "nothing known about its monitor";
		return 0;
	}
	auto k = m->n_visits - 1;
	if (k == 0 || !ws_switch_is_recent(&m->visits[k])) {
		*reason = "no recent switch on its monitor";
		return 0;
	}
	*reason = "started during a switch on its monitor";
	ws_switch_touch(&m->visits[k]);
	return k;
}

/// Whether picom fires `trigger` for a change of the window itself. Such an animation is
/// never part of a switch.
static bool ws_is_own_change(const char *trigger) {
	static const char *const names[] = {
	    "size", "position", "increase-opacity", "decrease-opacity", "color",
	};
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (strcmp(trigger, names[i]) == 0) {
			return true;
		}
	}
	return false;
}

/// Read where a window is, with its border, in a round trip of its own. Returns false if
/// the window is gone.
static bool ws_window_rect(xcb_window_t w, struct ws_rect *r) {
	xcb_get_geometry_reply_t *g = ws_reply(xcb_get_geometry(wsd.c, w).sequence);
	if (g == NULL) {
		return false;
	}
	*r = (struct ws_rect){
	    .x1 = g->x,
	    .y1 = g->y,
	    .x2 = g->x + g->width + g->border_width * 2,
	    .y2 = g->y + g->height + g->border_width * 2,
	};
	free(g);
	return true;
}

/// Where a window is, for a window that starts an animation: on which desktop (-1 if it
/// is not on one workspace), its place, and the index of its monitor (-1 for the whole
/// screen). Nothing is known about a window that closes.
struct ws_place {
	int64_t desktop;
	struct ws_rect rect;
	int monitor;
};

static struct ws_place
ws_place_of(const struct picom_animation_start *start, xcb_window_t client, bool gone) {
	struct ws_place ret = {.desktop = -1, .rect = wsd.screen, .monitor = -1};
	if (gone) {
		return ret;
	}
	auto c = ws_find_client(client);
	if (c != NULL) {
		ret.desktop = c->desktop;
		ret.rect = c->rect;
	} else if (!ws_window_rect(start->window, &ret.rect)) {
		return ret;
	}
	ret.monitor = ret.desktop >= 0 ? ws_desktop_monitor((unsigned)ret.desktop)
	                               : ws_rect_monitor(ret.rect);
	return ret;
}

/// The face of a window of workspace `desktop` on the monitor with index `index`. The
/// reference workspace is the one `m` showed before the switch to `m->visits[k]`, or the
/// one it shows now if `k` is 0.
static int ws_face(unsigned desktop, int index, struct ws_monitor *m, unsigned k) {
	auto order = ws_order_of(wsd.names[desktop], desktop);
	auto reference = order;
	if (k > 0) {
		reference = m->visits[k - 1].order;
	} else if (m != NULL) {
		reference = ws_shown(m)->order;
	}
	auto steps = ws_steps(reference, order, index);
	return steps.direction * (int)steps.distance;
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

/// Work out all our variables for a window that starts an animation.
void ws_compute(const struct picom_animation_start *start,
                double values[static WS_N_VARIABLES]) {
	auto now = ws_now_us();
	wsd.n_waits = 0;
	bool show =
	    strcmp(start->trigger, "open") == 0 || strcmp(start->trigger, "show") == 0;
	bool hide = strcmp(start->trigger, "hide") == 0;
	// A window that closes is gone (destroyed, or reparented away). There is nothing
	// to ask about it.
	bool gone = strcmp(start->trigger, "close") == 0;
	xcb_window_t client = start->client != XCB_NONE ? start->client : start->window;

	// The root window and the managed windows are read again on the first call in
	// each frame, and the windows that start in the same frame share that.
	if (start->frame != wsd.frame) {
		auto root = ws_read_root_request();
		auto clients = ws_client_list_request();
		wsd.frame = start->frame;
		wsd.frame_time_us = now;
		ws_refresh(root);
		ws_read_clients(clients);
	}

	auto place = ws_place_of(start, client, gone);
	auto desktop = place.desktop;
	auto monitor_rect = ws_monitor_rect(place.monitor);

	struct ws_monitor *m = NULL;
	unsigned k = 0;
	const char *reason;
	if (desktop < 0) {
		reason = gone ? "window is gone" : "window is not on one workspace";
	} else if (show) {
		k = ws_switch_for_show(client, (unsigned)desktop, &m, &reason);
	} else if (hide) {
		k = ws_switch_for_hide((unsigned)desktop, &m, &reason);
	} else if (ws_is_own_change(start->trigger)) {
		m = ws_find_monitor((unsigned)desktop);
		reason = "a change of the window itself";
	} else {
		k = ws_switch_latest((unsigned)desktop, &m, &reason);
	}
	auto move = k > 0 ? ws_switch_move(m, k) : (struct ws_move){0};

	int face = 0;
	bool placed = true;
	if (desktop >= 0) {
		face = ws_face((unsigned)desktop, place.monitor, m, k);
		auto r = place.rect;
		placed = ws_rect_has_point(monitor_rect, r.x1 + (r.x2 - r.x1) / 2,
		                           r.y1 + (r.y2 - r.y1) / 2);
	}
	unsigned faces = 0;
	for (unsigned d = 0; d < wsd.n_names; d++) {
		faces += ws_desktop_monitor(d) == place.monitor;
	}
	bool selected = wsd.current >= 0 && wsd.current < wsd.n_names &&
	                ws_desktop_monitor((unsigned)wsd.current) == place.monitor;

	values[WS_VAR_DIRECTION] = move.direction;
	values[WS_VAR_DISTANCE] = move.distance;
	values[WS_VAR_MONITOR_X] = monitor_rect.x1;
	values[WS_VAR_MONITOR_Y] = monitor_rect.y1;
	values[WS_VAR_MONITOR_WIDTH] = monitor_rect.x2 - monitor_rect.x1;
	values[WS_VAR_MONITOR_HEIGHT] = monitor_rect.y2 - monitor_rect.y1;
	values[WS_VAR_FACES] = faces;
	values[WS_VAR_FACE] = face;
	values[WS_VAR_SELECTED] = selected;
	values[WS_VAR_PLACED] = placed;

	if (wsd.log != NULL) {
		// Measured before the name lookup, which only the log needs.
		auto took = ws_now_us() - now;
		auto waits = wsd.n_waits;
		char *name = ws_window_name(client);
		ws_log("%s of window %#010x (%s): direction = %d (%s), "
		       "distance %u, face %d of %u, selected %d, placed %d, "
		       "monitor %d,%d %dx%d, %llu us, %u waits for X",
		       start->trigger, start->window, name, move.direction, reason,
		       move.distance, face, faces, selected, placed, monitor_rect.x1,
		       monitor_rect.y1, monitor_rect.x2 - monitor_rect.x1,
		       monitor_rect.y2 - monitor_rect.y1, (unsigned long long)took, waits);
		free(name);
	}

	// Drop the events the server sends to every client (MappingNotify). Nothing reads
	// them otherwise, and they would pile up in the connection's queue.
	xcb_generic_event_t *event;
	while ((event = xcb_poll_for_queued_event(wsd.c)) != NULL) {
		free(event);
	}
}
