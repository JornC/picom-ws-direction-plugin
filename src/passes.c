// SPDX-License-Identifier: MPL-2.0
// Copyright (c) JornC

/// Switches, ws-pass, and the variables.
///
/// Which workspace a monitor shows, and so its switches, comes from what the window
/// manager did, in the order it did it (ws_reconcile, after each read): a change of
/// _NET_CURRENT_DESKTOP to a workspace of the monitor, a map of a window of another of
/// its workspaces (a window manager maps only the windows of shown workspaces), and the
/// windows a read sees viewable. A switch S from A to B gives each window that existed
/// before it a pending show, if it is on B and was not viewable just before S, or a
/// pending hide, if it is on A and was viewable. The window's next map or unmap takes it:
/// that show or hide is part of S. Any other show or hide is part of no switch, however
/// soon after the switch it comes: a new window, a dialog, a window moved to another
/// workspace, a sibling of a fullscreen window. No clock is involved, so a window manager
/// that takes long, or a busy machine, gets the same answer.
///
/// On every switch we start the custom trigger ws-pass on all windows of the monitor:
/// those of the workspace switched away from, of the one switched to, of the workspaces
/// in between, and of its other workspaces. We start them in the pass in which we see the
/// switch, so they all begin together, also when the window manager hides and shows them
/// over more than one pass. A window that is viewable before and after the switch moved
/// along with it, and stays on screen, so it is left out. We declare ws-pass, ws-unfold
/// and ws-fold ourselves (add_animation_trigger), so a config uses them without listing
/// them in animation-triggers. A config that wants the windows in between drawn keeps
/// their images (keep-image-when-hidden) and gives them a ws-pass animation that
/// suppresses show and hide, so a show or hide that comes later does not restart them.
/// picom ignores the start for a window without a ws-pass animation, and for one it
/// cannot draw because it has no image yet; such a window gets its show or hide as
/// usual. While the monitor is unfolded into an overview, we start the trigger
/// overview.c gives instead.
///
/// For the triggers we start, we remember why (ws_start), and the variables
/// say that. A custom trigger we didn't start, such as one started over D-Bus, is part of
/// no switch.

#include <stdlib.h>
#include <string.h>

#include "ws_direction.h"

/// Index of the monitor that fully contains a window, -1 for "all monitors".
int ws_rect_monitor(struct ws_rect r) {
	for (int i = 0; i < wsd.n_monitors; i++) {
		auto e = &wsd.monitors[i].rect;
		if (e->x1 <= r.x1 && r.x2 <= e->x2 && e->y1 <= r.y1 && r.y2 <= e->y2) {
			return i;
		}
	}
	return -1;
}

static xcb_atom_t ws_monitor_name(int index) {
	return index >= 0 ? wsd.monitors[index].name : XCB_NONE;
}

/// What we know about the monitor with index `index` (-1 for the whole screen), or NULL.
struct ws_monitor *ws_find_monitor(int index) {
	auto name = ws_monitor_name(index);
	for (unsigned i = 0; i < wsd.n_tracked; i++) {
		if (wsd.tracked[i].name == name) {
			return &wsd.tracked[i];
		}
	}
	return NULL;
}

/// What we know about the monitor of desktop `desktop`, or NULL.
struct ws_monitor *ws_monitor_of(unsigned desktop) {
	return ws_find_monitor(ws_desktop_monitor(desktop));
}

/// What we know about the monitor with index `index`, made if it is new. Returns NULL if
/// we track too many monitors already.
struct ws_monitor *ws_get_monitor(int index) {
	auto m = ws_find_monitor(index);
	if (m == NULL && wsd.n_tracked < WS_MAX_MONITORS) {
		m = &wsd.tracked[wsd.n_tracked++];
		*m = (struct ws_monitor){.name = ws_monitor_name(index)};
	}
	return m;
}

static void ws_free_list(char **list, unsigned n) {
	for (unsigned i = 0; i < n; i++) {
		free(list[i]);
	}
	free(list);
}

static void ws_free_switch(struct ws_switch *s) {
	free(s->from);
	free(s->to);
	ws_free_list(s->list, s->n_list);
}

/// Free what monitor `m` holds.
void ws_free_monitor(struct ws_monitor *m) {
	free(m->shown);
	free(m->pick);
	free(m->unfolded_as);
	free(m->waiting);
	ws_free_list(m->list, m->n_list);
	for (unsigned k = 0; k < m->n_switches; k++) {
		ws_free_switch(&m->switches[k]);
	}
}

/// The switch with id `id` on monitor `m`, or NULL if it is not there (any more).
const struct ws_switch *ws_find_switch(const struct ws_monitor *m, uint64_t id) {
	for (unsigned k = 0; m != NULL && id != 0 && k < m->n_switches; k++) {
		if (m->switches[k].id == id) {
			return &m->switches[k];
		}
	}
	return NULL;
}

/// The workspaces of the monitor with index `index`, in the order of their desktops
/// (under i3 that is their order on the output). Returns how many.
unsigned ws_monitor_list(int index, const char *list[static WS_MAX_DESKTOPS]) {
	unsigned n = 0;
	for (unsigned d = 0; d < wsd.n_names; d++) {
		if (ws_desktop_monitor(d) == index) {
			list[n++] = wsd.names[d];
		}
	}
	return n;
}

/// Where `name` is in `list`, or -1.
int ws_list_find(const char *const *list, unsigned n, const char *name) {
	for (unsigned i = 0; name != NULL && i < n; i++) {
		if (strcmp(list[i], name) == 0) {
			return (int)i;
		}
	}
	return -1;
}

/// `list` with the names of `more` that it doesn't have, each after the name it came
/// after in `more`. Returns how many; `out` has room for 2 * WS_MAX_DESKTOPS.
static unsigned ws_merge(const char *const *list, unsigned n, const char *const *more,
                         unsigned n_more, const char **out) {
	memcpy(out, list, n * sizeof(*out));
	for (unsigned i = 0; i < n_more; i++) {
		if (ws_list_find(out, n, more[i]) >= 0) {
			continue;
		}
		unsigned at = 0;
		for (int j = (int)i - 1; j >= 0; j--) {
			auto before = ws_list_find(out, n, more[j]);
			if (before >= 0) {
				at = (unsigned)before + 1;
				break;
			}
		}
		memmove(&out[at + 1], &out[at], (n - at) * sizeof(*out));
		out[at] = more[i];
		n++;
	}
	return n;
}

/// The way from workspace `from` to workspace `to` in `list`. The distance counts the
/// steps between them in the list. Both 0 if either is not in it.
struct ws_move
ws_steps(const char *from, const char *to, const char *const *list, unsigned n) {
	int a = ws_list_find(list, n, from), b = ws_list_find(list, n, to);
	if (a < 0 || b < 0 || a == b) {
		return (struct ws_move){0};
	}
	return (struct ws_move){.direction = b > a ? 1 : -1,
	                        .distance = (unsigned)(b > a ? b - a : a - b)};
}

/// The workspaces of switches `first` to `last` (the same switch for one), merged as for
/// one switch, in `out` (room for 2 * WS_MAX_DESKTOPS). Returns how many.
static unsigned
ws_span_list(const struct ws_switch *first, const struct ws_switch *last, const char **out) {
	return ws_merge((const char *const *)last->list, last->n_list,
	                (const char *const *)first->list, first->n_list, out);
}

static char **ws_copy_list(const char *const *list, unsigned n) {
	char **ret = calloc(n + 1, sizeof(*ret));
	if (ret == NULL) {
		abort();
	}
	for (unsigned i = 0; i < n; i++) {
		ret[i] = strdup(list[i]);
		if (ret[i] == NULL) {
			abort();
		}
	}
	return ret;
}

static char *ws_strdup(const char *s) {
	char *ret = strdup(s);
	if (ret == NULL) {
		abort();
	}
	return ret;
}

/// A show or hide was part of switch `id`.
static void ws_realize(uint64_t id) {
	for (unsigned i = 0; id != 0 && i < wsd.n_tracked; i++) {
		auto m = &wsd.tracked[i];
		for (unsigned k = 0; k < m->n_switches; k++) {
			m->switches[k].realized |= m->switches[k].id == id;
		}
	}
}

/// Per monitor, while ws_reconcile runs: its index, its workspaces in the read before,
/// and the position of the event at which we last recorded a switch on it.
struct ws_scan_monitor {
	struct ws_monitor *m;
	int index;
	char **before;
	unsigned n_before;
	int last_switch_at;
};

/// Per window, while ws_reconcile runs: whether it was in the read before, whether it is
/// viewable at the event we are at, and the position of an unmap not part of a switch.
struct ws_scan_client {
	bool existed, viewable;
	int unmapped_at;
};

struct ws_scan {
	struct ws_scan_monitor monitors[WS_MAX_MONITORS];
	unsigned n_monitors;
	struct ws_scan_client *clients;
};

static struct ws_scan_monitor *ws_scan_monitor(struct ws_scan *scan, int index) {
	for (unsigned i = 0; i < scan->n_monitors; i++) {
		if (scan->monitors[i].index == index) {
			return &scan->monitors[i];
		}
	}
	return NULL;
}

/// Record a switch on monitor `sm->m` to workspace `to`, at the event at position `at`,
/// and give the windows of its two workspaces their pending show or hide.
static void ws_add_switch(struct ws_scan *scan, struct ws_scan_monitor *sm,
                          const char *to, int at, const char *source) {
	auto m = sm->m;
	if (m->n_switches == WS_HISTORY) {
		// Forget the oldest one.
		ws_free_switch(&m->switches[0]);
		memmove(&m->switches[0], &m->switches[1],
		        sizeof(m->switches[0]) * (WS_HISTORY - 1));
		m->n_switches--;
	}
	// The window manager went on before it showed or hid anything for the switch
	// before, as i3 does for "workspace 2; workspace 4": one move.
	auto prev = m->n_switches > 0 ? &m->switches[m->n_switches - 1] : NULL;
	bool chained = prev != NULL && !prev->realized && strcmp(prev->to, m->shown) == 0;
	auto prev_id = prev != NULL ? prev->id : 0;
	auto start = chained ? prev->start : wsd.last_switch + 1;
	const char *now[WS_MAX_DESKTOPS];
	auto n_now = ws_monitor_list(sm->index, now);
	const char *list[2 * WS_MAX_DESKTOPS];
	auto n = ws_merge(now, n_now, (const char *const *)sm->before, sm->n_before, list);
	auto s = &m->switches[m->n_switches++];
	*s = (struct ws_switch){
	    .id = ++wsd.last_switch,
	    .from = m->shown,
	    .to = ws_strdup(to),
	    .list = ws_copy_list(list, n),
	    .n_list = n,
	    .start = start,
	};
	s->move = ws_steps(s->from, s->to, list, n);
	m->shown = ws_strdup(to);

	for (unsigned i = 0; i < wsd.n_clients; i++) {
		auto c = &wsd.clients[i];
		auto sc = &scan->clients[i];
		if (c->workspace == NULL || !sc->existed) {
			continue;
		}
		if (chained && c->pending_hide == prev_id && strcmp(c->workspace, s->to) != 0) {
			c->pending_hide = s->id;
		}
		if (strcmp(c->workspace, s->to) == 0) {
			c->pending_hide = 0;
			c->pending_show = sc->viewable ? 0 : s->id;
			if (sc->viewable) {
				c->along = s->id;
			}
		} else if (strcmp(c->workspace, s->from) == 0) {
			c->pending_show = 0;
			c->pending_hide = sc->viewable ? s->id : 0;
			// A window manager that hides the windows of the old workspace
			// before it shows the new one: those hides are part of it too.
			if (!sc->viewable && sc->unmapped_at > sm->last_switch_at &&
			    c->last_hide == 0) {
				c->last_hide = s->id;
				s->realized = true;
				sc->unmapped_at = -1;
			}
		}
	}
	sm->last_switch_at = at;

	auto r = ws_monitor_rect(sm->index);
	ws_log("Workspace switch on monitor at %d,%d: \"%s\" -> \"%s\", direction %d, "
	       "distance %u (from %s, in pass %llu)",
	       r.x1, r.y1, s->from, s->to, s->move.direction, s->move.distance, source,
	       (unsigned long long)wsd.frame);
}

/// Monitor `sm->m` shows workspace `name` from the event at position `at` on: a switch
/// if it showed another one.
static void ws_saw_shown(struct ws_scan *scan, struct ws_scan_monitor *sm,
                         const char *name, int at, const char *source) {
	auto m = sm->m;
	if (m->shown != NULL && strcmp(m->shown, name) == 0) {
		return;
	}
	if (m->shown == NULL) {
		m->shown = ws_strdup(name);
		auto r = ws_monitor_rect(sm->index);
		ws_log("Workspace \"%s\" is shown on monitor at %d,%d (from %s)", name,
		       r.x1, r.y1, source);
		return;
	}
	// The switches we make to get images back are not switches: we go back to where
	// we were, and draw nothing meanwhile.
	if (wsd.warm.step != WS_WARM_IDLE) {
		return;
	}
	ws_add_switch(scan, sm, name, at, source);
}

/// Which workspace the read says monitor `sm` shows: the one with viewable windows, else
/// the one of its workspaces that has no windows (i3 removes an empty workspace once it
/// is hidden, so that one must be shown). The current desktop counts as shown, and
/// decides between two. NULL if we can't tell, as while the window manager is halfway
/// through a switch.
static const char *ws_read_shown(const struct ws_scan_monitor *sm) {
	const char *list[WS_MAX_DESKTOPS];
	auto n = ws_monitor_list(sm->index, list);
	bool candidate[WS_MAX_DESKTOPS] = {0}, has_windows[WS_MAX_DESKTOPS] = {0};
	for (unsigned i = 0; i < wsd.n_clients; i++) {
		auto c = &wsd.clients[i];
		auto k = ws_list_find(list, n, c->workspace);
		if (k >= 0) {
			has_windows[k] = true;
			candidate[k] |= c->viewable;
		}
	}
	const char *current =
	    wsd.current >= 0 && wsd.current < wsd.n_names ? wsd.names[wsd.current] : NULL;
	auto k = ws_list_find(list, n, current);
	if (k >= 0) {
		candidate[k] = true;
	}
	unsigned n_candidates = 0, n_empty = 0;
	const char *one = NULL, *empty = NULL;
	for (unsigned i = 0; i < n; i++) {
		n_candidates += candidate[i];
		one = candidate[i] ? list[i] : one;
		n_empty += !has_windows[i];
		empty = !has_windows[i] ? list[i] : empty;
	}
	if (n_candidates == 0) {
		return n_empty == 1 ? empty : NULL;
	}
	if (n_candidates == 1) {
		return one;
	}
	auto shown = ws_list_find(list, n, sm->m->shown);
	if (shown >= 0 && candidate[shown]) {
		return sm->m->shown;
	}
	return k >= 0 ? current : NULL;
}

static struct ws_scan_monitor *
ws_scan_monitor_of(struct ws_scan *scan, const struct ws_client *c) {
	auto d = c->desktop;
	return d >= 0 ? ws_scan_monitor(scan, ws_desktop_monitor((unsigned)d)) : NULL;
}

/// Take a read: the managed windows are in wsd.clients, those of the read before in
/// `prev`, and `events` are the maps, unmaps and current desktop changes between the two,
/// in order. Record the switches on every monitor, and which switch each window's show
/// and hide is part of.
void ws_reconcile(struct ws_client *prev, unsigned n_prev, struct ws_event *events,
                  unsigned n_events, int64_t prev_current) {
	struct ws_scan scan = {0};
	scan.clients = calloc(wsd.n_clients + 1, sizeof(*scan.clients));
	if (scan.clients == NULL) {
		abort();
	}
	for (unsigned i = 0; i < wsd.n_clients; i++) {
		auto c = &wsd.clients[i];
		scan.clients[i].unmapped_at = -1;
		for (unsigned j = 0; j < n_prev; j++) {
			if (prev[j].id == c->id) {
				scan.clients[i] =
				    (struct ws_scan_client){.existed = true,
				                            .viewable = prev[j].viewable,
				                            .unmapped_at = -1};
				c->pending_show = prev[j].pending_show;
				c->pending_hide = prev[j].pending_hide;
				c->last_show = prev[j].last_show;
				c->last_hide = prev[j].last_hide;
				c->along = prev[j].along;
				break;
			}
		}
	}

	// Forget the monitors that are gone, and their switches. The whole screen is one
	// while no desktop is on a monitor of its own.
	for (unsigned i = 0; i < wsd.n_tracked;) {
		auto m = &wsd.tracked[i];
		auto index = ws_monitor_of_name(m->name);
		bool used = index != -1 || wsd.n_names == 0;
		for (unsigned d = 0; d < wsd.n_names && !used; d++) {
			used = ws_desktop_monitor(d) == -1;
		}
		if (index == -2 || !used) {
			ws_log("Monitor %u is gone, forgetting its switches",
			       (unsigned)m->name);
			if (m->input != XCB_NONE) {
				xcb_destroy_window(wsd.c, m->input);
			}
			ws_free_monitor(m);
			wsd.tracked[i] = wsd.tracked[--wsd.n_tracked];
		} else {
			i++;
		}
	}
	for (unsigned d = 0; d < wsd.n_names; d++) {
		auto index = ws_desktop_monitor(d);
		if (ws_scan_monitor(&scan, index) != NULL) {
			continue;
		}
		auto m = ws_get_monitor(index);
		if (m == NULL) {
			continue;
		}
		scan.monitors[scan.n_monitors++] = (struct ws_scan_monitor){
		    .m = m,
		    .index = index,
		    .before = m->list,
		    .n_before = m->n_list,
		    .last_switch_at = -1,
		};
	}

	int last_current = -1;
	for (unsigned i = 0; i < n_events; i++) {
		if (events[i].type == WS_EVENT_CURRENT) {
			last_current = (int)i;
		}
	}
	const char *current =
	    wsd.current >= 0 && wsd.current < wsd.n_names ? wsd.names[wsd.current] : NULL;
	auto current_monitor =
	    current != NULL
	        ? ws_scan_monitor(&scan, ws_desktop_monitor((unsigned)wsd.current))
	        : NULL;
	bool warm = wsd.warm.step != WS_WARM_IDLE;

	for (unsigned i = 0; i < n_events; i++) {
		auto e = &events[i];
		if (e->type == WS_EVENT_CURRENT) {
			// Only the last change tells where it went: we read the value
			// once.
			if ((int)i == last_current && current_monitor != NULL) {
				ws_saw_shown(&scan, current_monitor, current, (int)i,
				             "the root window");
			}
			continue;
		}
		int k = -1;
		for (unsigned j = 0; j < wsd.n_clients && k < 0; j++) {
			k = wsd.clients[j].frame == e->window ? (int)j : -1;
		}
		if (k < 0) {
			// A window that is gone, or that the window manager doesn't
			// manage.
			continue;
		}
		auto c = &wsd.clients[k];
		auto sc = &scan.clients[k];
		if (e->type == WS_EVENT_MAP) {
			auto sm = ws_scan_monitor_of(&scan, c);
			if (sm != NULL) {
				char source[48];
				snprintf(source, sizeof(source), "a map of window %#010x",
				         c->id);
				ws_saw_shown(&scan, sm, c->workspace, (int)i, source);
			}
			sc->viewable = true;
			c->last_show = warm ? 0 : c->pending_show;
			c->pending_show = 0;
			ws_realize(c->last_show);
		} else {
			sc->viewable = false;
			c->last_hide = warm ? 0 : c->pending_hide;
			c->pending_hide = 0;
			ws_realize(c->last_hide);
			sc->unmapped_at = c->last_hide == 0 ? (int)i : -1;
		}
	}
	if (last_current < 0 && wsd.current != prev_current && current_monitor != NULL) {
		ws_saw_shown(&scan, current_monitor, current, (int)n_events, "the root window");
	}
	for (unsigned i = 0; i < scan.n_monitors; i++) {
		auto sm = &scan.monitors[i];
		auto shown = ws_read_shown(sm);
		if (shown != NULL) {
			ws_saw_shown(&scan, sm, shown, (int)n_events, "the windows on screen");
		} else if (sm->m->shown != NULL && ws_find_desktop(sm->m->shown) < 0) {
			auto r = ws_monitor_rect(sm->index);
			ws_log("Monitor at %d,%d: \"%s\", which it showed, is gone, and "
			       "we "
			       "can't tell which workspace it shows",
			       r.x1, r.y1, sm->m->shown);
		}
	}
	// A change no event told us about, for windows of a read whose events we missed.
	for (unsigned i = 0; i < wsd.n_clients; i++) {
		auto c = &wsd.clients[i];
		auto sc = &scan.clients[i];
		if (sc->existed && sc->viewable != c->viewable) {
			if (c->viewable) {
				c->last_show = warm ? 0 : c->pending_show;
				c->pending_show = 0;
				ws_realize(c->last_show);
			} else {
				c->last_hide = warm ? 0 : c->pending_hide;
				c->pending_hide = 0;
				ws_realize(c->last_hide);
			}
		}
	}

	for (unsigned i = 0; i < scan.n_monitors; i++) {
		auto sm = &scan.monitors[i];
		const char *list[WS_MAX_DESKTOPS];
		auto n = ws_monitor_list(sm->index, list);
		ws_free_list(sm->m->list, sm->m->n_list);
		sm->m->list = ws_copy_list(list, n);
		sm->m->n_list = n;
	}
	free(scan.clients);
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

static bool ws_is_ours(const char *trigger) {
	return strcmp(trigger, WS_PASS_TRIGGER) == 0 ||
	       strcmp(trigger, WS_UNFOLD_TRIGGER) == 0 ||
	       strcmp(trigger, WS_FOLD_TRIGGER) == 0;
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

/// Start `trigger` on window `w`, and remember why: for monitor `m` and its switches
/// `first` to `last` (0 for none). Returns whether picom started it.
bool ws_start(xcb_window_t w, const char *trigger, const struct ws_monitor *m,
              uint64_t first, uint64_t last) {
	if (wsd.starts_frame != wsd.frame) {
		wsd.starts_frame = wsd.frame;
		wsd.n_starts = 0;
	}
	if (!wsd.api->start_animation(w, trigger)) {
		return false;
	}
	wsd.starts = ws_grow(wsd.starts, wsd.n_starts + 1, sizeof(*wsd.starts));
	wsd.starts[wsd.n_starts++] = (struct ws_start){
	    .window = w,
	    .trigger = trigger,
	    .monitor = m != NULL ? m->name : XCB_NONE,
	    .first = first,
	    .last = last,
	};
	return true;
}

/// Why we started `trigger` on window `window` or `client` in this pass, or NULL.
static const struct ws_start *
ws_find_start(uint64_t frame, xcb_window_t window, xcb_window_t client, const char *trigger) {
	if (frame != wsd.starts_frame) {
		return NULL;
	}
	for (unsigned i = 0; i < wsd.n_starts; i++) {
		auto s = &wsd.starts[i];
		if ((s->window == window || s->window == client) &&
		    strcmp(s->trigger, trigger) == 0) {
			return s;
		}
	}
	return NULL;
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
	// to ask about it: we take what the last read that had it knew.
	bool gone = strcmp(start->trigger, "close") == 0;
	// picom may not know the client window of a frame yet.
	xcb_window_t client = start->client != XCB_NONE ? start->client : start->window;

	// The windows that start in the same pass share one read.
	if (!gone && start->frame != wsd.read_frame) {
		ws_read();
	}

	auto c = ws_find_client(client);
	if (c == NULL) {
		c = ws_find_client_by_frame(start->window);
	}
	// A window the last read no longer found, like one that hides because it closes:
	// what the read before knew of it.
	bool lost = false;
	if (c == NULL) {
		c = ws_find_gone(start->window);
		lost = c != NULL;
	}
	// A backdrop has the variables of a window of its workspace.
	auto backdrop = c == NULL ? ws_backdrop_workspace(start->window) : NULL;
	int64_t desktop = backdrop != NULL ? ws_find_desktop(backdrop)
	                  : c == NULL      ? -1
	                  : lost           ? ws_find_desktop(c->workspace)
	                                   : c->desktop;
	struct ws_rect rect = wsd.screen;
	bool known = c != NULL || backdrop != NULL ||
	             (!gone && ws_window_rect(start->window, &rect));
	if (c != NULL) {
		rect = c->rect;
	} else if (backdrop != NULL && desktop >= 0) {
		rect = ws_monitor_rect(ws_desktop_monitor((unsigned)desktop));
	}
	int monitor = desktop >= 0 ? ws_desktop_monitor((unsigned)desktop)
	              : known      ? ws_rect_monitor(rect)
	                           : -1;
	auto own = !gone && ws_is_ours(start->trigger)
	               ? ws_find_start(start->frame, start->window, client, start->trigger)
	               : NULL;
	bool sticky = false;
	if (c != NULL && c->sticky && own != NULL) {
		// The overview draws a window on all desktops on the shown tile of the
		// monitor we started it for.
		auto ms = ws_find_monitor(ws_monitor_of_name(own->monitor));
		if (ms != NULL && ws_find_desktop(ms->shown) >= 0) {
			sticky = true;
			desktop = ws_find_desktop(ms->shown);
			monitor = ws_monitor_of_name(own->monitor);
		}
	}
	auto monitor_rect = ws_monitor_rect(monitor);
	auto m = ws_find_monitor(monitor);
	const char *workspace = desktop >= 0 ? wsd.names[desktop] : NULL;

	const struct ws_switch *first = NULL, *last = NULL;
	const char *reason;
	if (gone) {
		reason = c != NULL ? "window is gone, as the last read saw it"
		                   : "window is gone";
	} else if (desktop < 0) {
		reason = "window is not on one workspace";
	} else if ((show || hide) && c == NULL) {
		reason = "a backdrop we show or hide";
	} else if (show || hide) {
		last = ws_find_switch(m, show ? c->last_show : c->last_hide);
		first = last != NULL ? ws_find_switch(m, last->start) : NULL;
		first = first != NULL ? first : last;
		reason = last == NULL ? "not part of a switch"
		         : show       ? "workspace was switched to"
		                      : "workspace was switched away from";
	} else if (ws_is_own_change(start->trigger)) {
		reason = "a change of the window itself";
	} else if (ws_is_ours(start->trigger)) {
		if (own != NULL) {
			auto ms = ws_find_monitor(ws_monitor_of_name(own->monitor));
			first = ws_find_switch(ms, own->first);
			last = ws_find_switch(ms, own->last);
		}
		reason = own == NULL    ? "not started by the plugin"
		         : sticky       ? "on all desktops, drawn on the shown workspace"
		         : last == NULL ? "started by the plugin, not for a switch"
		                        : "started by the plugin for a switch";
	} else {
		reason = "not started by the plugin";
	}
	if (first == NULL || last == NULL) {
		first = last = NULL;
	}

	const char *list[WS_MAX_DESKTOPS];
	auto n = ws_monitor_list(monitor, list);
	auto shown = m != NULL ? ws_list_find(list, n, m->shown) : -1;
	if (desktop >= 0 && shown < 0) {
		ws_log("Monitor at %d,%d: we don't know which of its workspaces it shows",
		       monitor_rect.x1, monitor_rect.y1);
	}
	struct ws_move move = {0};
	int face = 0;
	if (desktop >= 0 && last != NULL) {
		const char *span[2 * WS_MAX_DESKTOPS];
		auto n_span = ws_span_list(first, last, span);
		move = ws_steps(first->from, last->to, span, n_span);
		auto steps = ws_steps(first->from, workspace, span, n_span);
		face = steps.direction * (int)steps.distance;
	} else if (desktop >= 0 && shown >= 0) {
		auto steps = ws_steps(m->shown, workspace, list, n);
		face = steps.direction * (int)steps.distance;
	}
	bool placed = true;
	if (desktop >= 0) {
		placed = ws_rect_has_point(monitor_rect, rect.x1 + (rect.x2 - rect.x1) / 2,
		                           rect.y1 + (rect.y2 - rect.y1) / 2);
	}
	auto faces = known ? n : 0;
	bool selected = wsd.current >= 0 && wsd.current < wsd.n_names &&
	                ws_desktop_monitor((unsigned)wsd.current) == monitor;
	auto index = desktop >= 0 ? ws_list_find(list, n, workspace) : 0;
	if (desktop < 0) {
		shown = 0;
	}
	bool unfolded = desktop >= 0 && m != NULL && m->unfolded;

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
	values[WS_VAR_INDEX] = index;
	values[WS_VAR_SHOWN] = shown;
	values[WS_VAR_UNFOLDED] = unfolded;
	values[WS_VAR_BACKDROP] = backdrop != NULL;

	if (wsd.log != NULL) {
		// Measured before the name lookup, which only the log needs.
		auto took = ws_now_us() - now;
		auto waits = wsd.n_waits;
		char *name = gone ? strdup("") : ws_window_name(client);
		ws_log("%s of window %#010x (%s): direction = %d (%s), "
		       "distance %u, face %d of %u, selected %d, placed %d, "
		       "monitor %d,%d %dx%d, index %d, shown %d, unfolded %d, "
		       "%llu us, %u waits for X, pass %llu",
		       start->trigger, start->window, name, move.direction, reason,
		       move.distance, face, faces, selected, placed, monitor_rect.x1,
		       monitor_rect.y1, monitor_rect.x2 - monitor_rect.x1,
		       monitor_rect.y2 - monitor_rect.y1, index, shown, unfolded,
		       (unsigned long long)took, waits, (unsigned long long)start->frame);
		free(name);
	}
}

/// Start ws-pass on all windows of the switches we didn't start it for yet, once, on each
/// monitor: those we saw since we last started it, as one move from where the first one
/// came from to where the last one went. The window manager may hide and show the
/// windows of the two workspaces of a switch over more than one pass; this way they all
/// start together. All windows of the monitor take part, also those of workspaces the
/// switch doesn't reach: when a switch comes while the last one still runs, every window
/// of the monitor is then in the same animation, and a style can carry their positions
/// over. A window that was viewable before the switch and is viewable after it moved
/// along with it, so it is left out. While the monitor is unfolded, it starts the trigger
/// ws_overview_switch_trigger gives instead of ws-pass. Returns whether that folded a
/// monitor for a click, see ws_pick.
bool ws_start_passes(void) {
	bool folded = false;
	for (unsigned i = 0; i < wsd.n_tracked; i++) {
		auto m = &wsd.tracked[i];
		unsigned k = m->n_switches;
		while (k > 0 && !m->switches[k - 1].passes_started) {
			k--;
			m->switches[k].passes_started = true;
		}
		if (k == m->n_switches) {
			continue;
		}
		auto first = &m->switches[k];
		auto last = &m->switches[m->n_switches - 1];
		if (strcmp(first->from, last->to) == 0) {
			ws_log("Switches \"%s\" -> ... -> \"%s\" went back where they "
			       "came "
			       "from, starting nothing",
			       first->from, last->to);
			continue;
		}
		auto trigger = ws_overview_switch_trigger(m, last, &folded);
		if (trigger == NULL) {
			trigger = WS_PASS_TRIGGER;
		}
		auto index = ws_monitor_of_name(m->name);
		bool overview = strcmp(trigger, WS_PASS_TRIGGER) != 0;
		unsigned members = 0, started = 0;
		for (unsigned j = 0; j < wsd.n_clients; j++) {
			auto c = &wsd.clients[j];
			bool in = ws_overview_member(c, index, overview) ||
			          (c->workspace != NULL &&
			           (strcmp(c->workspace, first->from) == 0 ||
			            strcmp(c->workspace, last->to) == 0));
			bool along = c->viewable && c->along >= first->id &&
			             c->workspace != NULL &&
			             strcmp(c->workspace, last->to) == 0;
			if (!in || along) {
				continue;
			}
			members++;
			started += ws_start(c->id, trigger, m, first->id, last->id);
		}
		if (members > 0) {
			ws_log("Switch \"%s\" -> \"%s\" has %u windows, "
			       "started %s on %u (in pass %llu)",
			       first->from, last->to, members, trigger, started,
			       (unsigned long long)wsd.frame);
		}
		auto backdrops = ws_backdrops_start(m, trigger, first->id, last->id);
		if (backdrops > 0) {
			ws_log("  and on %u backdrops", backdrops);
		}
		if (strcmp(trigger, WS_UNFOLD_TRIGGER) == 0) {
			ws_overview_started(m);
		}
	}
	return folded;
}
