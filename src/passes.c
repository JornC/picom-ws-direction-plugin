// SPDX-License-Identifier: MPL-2.0
// Copyright (c) JornC

/// Switches, and the variables.

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

/// Take the root window replies, and record a switch if the monitor of the current
/// desktop now shows another workspace.
void ws_refresh(struct ws_root_requests requests) {
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
	values[WS_VAR_DIRECTION] = direction;
	values[WS_VAR_MONITOR_X] = rect.x1;
	values[WS_VAR_MONITOR_Y] = rect.y1;
	values[WS_VAR_MONITOR_WIDTH] = rect.x2 - rect.x1;
	values[WS_VAR_MONITOR_HEIGHT] = rect.y2 - rect.y1;

	if (wsd.log != NULL) {
		// Measured before the name lookup, which only the log needs.
		auto took = ws_now_us() - now;
		auto waits = wsd.n_waits;
		char *name = ws_window_name(client);
		ws_log("%s of window %#010x (%s): direction = %d (%s), "
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
