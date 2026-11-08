// SPDX-License-Identifier: MPL-2.0
// Copyright (c) JornC

/// picom loses the images of hidden windows when it reinitializes, and when the window
/// manager restarts and puts every window in a new frame. We remember the windows we saw
/// picom keep an image of while hidden: picom says when it sees a window hidden and its
/// rules keep its image (window_keeps_image), and whether it has one. picom can't say
/// that it lost one: it doesn't know what it had before it reinitialized, nor the rules
/// of a window it has not shown since. When one of them has no image any more, we freeze
/// the screen, the keyboard and the pointer: picom draws nothing, and keys and clicks
/// wait in the X server. We ask the window manager to show each hidden workspace that
/// has a window without an image, one per monitor at a time, wait until their windows
/// drew all of themselves since they showed and picom has their images, then show the
/// workspaces that were shown, put the pointer back where it was (i3 moves it to the
/// monitor with the focus), and let go. The window manager's idea of the workspace
/// before (i3's `workspace back_and_forth`) stays changed: no EWMH request or i3 command
/// sets it. We leave a monitor alone while it shows a workspace without windows: if the
/// window manager removes such a workspace once it is hidden, as i3 does, we could not
/// go back to it.

#include <stdlib.h>
#include <string.h>
#include <xcb/damage.h>
#include <xcb/xfixes.h>

#include "protocol.h"
#include "ws_direction.h"

/// While we get images back, we freeze the screen for this many seconds at a time, again
/// in every pass. So a pass comes at least this often.
#define WS_WARM_TICK 0.02
/// Before we start, the root window must not change for this long, so the window manager
/// is done: after a restart it puts the windows in new frames one by one.
#define WS_WARM_SETTLE_US (100 * 1000)
#define WS_WARM_SETTLE_MAX_US (2000 * 1000)
/// We wait at most this long for the windows of the workspaces we show to draw all of
/// themselves. The only clock: a program that draws only part of its window, or nothing,
/// never does.
#define WS_WARM_ROUND_MAX_US (500 * 1000)
/// After this long we go back to the workspaces that were shown, done or not.
#define WS_WARM_MAX_US (3000 * 1000)
/// We look at most at this many windows without an image in one pass.
#define WS_WARM_MAX_LOST 64

/// Whether any window is on workspace `name`.
static bool ws_workspace_has_windows(const char *name) {
	for (unsigned i = 0; i < wsd.n_clients; i++) {
		if (wsd.clients[i].workspace != NULL &&
		    strcmp(wsd.clients[i].workspace, name) == 0) {
			return true;
		}
	}
	return false;
}

/// A window of a workspace we show, which we wait for to draw.
struct ws_watch {
	xcb_window_t window;
	/// Set once it shows: what it draws from then on, and the part of it that it has
	/// not drawn yet.
	xcb_damage_damage_t damage;
	xcb_xfixes_region_t parts, missing;
	bool drawn;
};

static int ws_set_find(const xcb_window_t *set, unsigned n, xcb_window_t id) {
	for (unsigned i = 0; i < n; i++) {
		if (set[i] == id) {
			return (int)i;
		}
	}
	return -1;
}

static void ws_set_add(xcb_window_t **set, unsigned *n, xcb_window_t id) {
	if (ws_set_find(*set, *n, id) >= 0) {
		return;
	}
	*set = ws_grow(*set, *n + 1, sizeof(**set));
	(*set)[(*n)++] = id;
}

/// Forget the keepers the window manager no longer manages.
void ws_prune_keepers(void) {
	for (unsigned i = 0; i < wsd.n_keepers;) {
		if (ws_find_client(wsd.keepers[i]) == NULL) {
			wsd.keepers[i] = wsd.keepers[--wsd.n_keepers];
		} else {
			i++;
		}
	}
}

/// The monitor of workspace `desktop`, if it shows another workspace, else NULL.
static struct ws_monitor *ws_monitor_hiding(unsigned desktop) {
	auto m = ws_monitor_of(desktop);
	if (m == NULL || m->shown == NULL || strcmp(m->shown, wsd.names[desktop]) == 0) {
		return NULL;
	}
	return m;
}

/// Ask the window manager to show workspace `name`, with the EWMH request pagers use.
static void ws_request_workspace(const char *name) {
	auto desktop = ws_find_desktop(name);
	if (desktop < 0) {
		ws_log("Warm: workspace \"%s\" is gone", name);
		return;
	}
	xcb_client_message_event_t event = {
	    .response_type = XCB_CLIENT_MESSAGE,
	    .format = 32,
	    .window = wsd.root,
	    .type = wsd.a_current_desktop,
	    .data.data32 = {(uint32_t)desktop, XCB_CURRENT_TIME},
	};
	xcb_send_event(wsd.c, 0, wsd.root,
	               XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT,
	               (const char *)&event);
}

/// Wait for the windows of workspace `name` to draw.
static void ws_watch_workspace(const char *name) {
	for (unsigned i = 0; i < wsd.n_clients; i++) {
		auto c = &wsd.clients[i];
		if (c->workspace == NULL || strcmp(c->workspace, name) != 0) {
			continue;
		}
		wsd.warm.watch =
		    ws_grow(wsd.warm.watch, wsd.warm.n_watch + 1, sizeof(*wsd.warm.watch));
		wsd.warm.watch[wsd.warm.n_watch++] = (struct ws_watch){.window = c->id};
	}
}

static void ws_unwatch(void) {
	for (unsigned i = 0; i < wsd.warm.n_watch; i++) {
		auto watch = &wsd.warm.watch[i];
		if (watch->damage != XCB_NONE) {
			xcb_damage_destroy(wsd.c, watch->damage);
			xcb_xfixes_destroy_region(wsd.c, watch->parts);
			xcb_xfixes_destroy_region(wsd.c, watch->missing);
		}
	}
	free(wsd.warm.watch);
	wsd.warm.watch = NULL;
	wsd.warm.n_watch = 0;
}

/// Ask `window` and each of its shown windows inside it that can be drawn in to draw all
/// of themselves again, with an Expose event each: a program draws in whichever of its
/// windows it likes, and only one that gets the event draws. Sets `size` to the size of
/// `window`, if not NULL; to 0x0 if it isn't shown.
static void ws_expose_tree(xcb_window_t window, xcb_rectangle_t *size) {
	auto attr_cookie = xcb_get_window_attributes(wsd.c, window);
	auto geometry_cookie = xcb_get_geometry(wsd.c, window);
	auto tree_cookie = xcb_query_tree(wsd.c, window);
	auto attr = xcb_get_window_attributes_reply(wsd.c, attr_cookie, NULL);
	auto geometry = xcb_get_geometry_reply(wsd.c, geometry_cookie, NULL);
	auto tree = xcb_query_tree_reply(wsd.c, tree_cookie, NULL);
	if (size != NULL) {
		*size = (xcb_rectangle_t){};
	}
	if (attr != NULL && geometry != NULL && tree != NULL &&
	    attr->map_state == XCB_MAP_STATE_VIEWABLE &&
	    attr->_class == XCB_WINDOW_CLASS_INPUT_OUTPUT) {
		if (size != NULL) {
			*size = (xcb_rectangle_t){.width = geometry->width,
			                          .height = geometry->height};
		}
		union {
			xcb_expose_event_t expose;
			char bytes[32];
		} event = {.expose = {.response_type = XCB_EXPOSE,
		                      .window = window,
		                      .width = geometry->width,
		                      .height = geometry->height}};
		xcb_send_event(wsd.c, 0, window, XCB_EVENT_MASK_EXPOSURE, event.bytes);
		auto children = xcb_query_tree_children(tree);
		for (int i = 0; i < xcb_query_tree_children_length(tree); i++) {
			ws_expose_tree(children[i], NULL);
		}
	}
	free(attr);
	free(geometry);
	free(tree);
}

/// Start to follow what the window of `watch` draws, now that it shows. It may have
/// drawn before we could follow it, so we ask it to draw all of itself again. Our
/// requests reach the X server in order, after the one that showed the window: what
/// the X server put in the window when it showed it doesn't count.
static void ws_watch_start(struct ws_watch *watch) {
	watch->damage = xcb_generate_id(wsd.c);
	xcb_damage_create(wsd.c, watch->damage, watch->window,
	                  XCB_DAMAGE_REPORT_LEVEL_NON_EMPTY);
	// The X server reports all of a shown window as damaged when the damage is made.
	xcb_damage_subtract(wsd.c, watch->damage, XCB_NONE, XCB_NONE);
	xcb_rectangle_t size;
	ws_expose_tree(watch->window, &size);
	watch->parts = xcb_generate_id(wsd.c);
	xcb_xfixes_create_region(wsd.c, watch->parts, 0, NULL);
	watch->missing = xcb_generate_id(wsd.c);
	xcb_xfixes_create_region(wsd.c, watch->missing, 1, &size);
}

/// Whether the windows we watch drew all of themselves since they showed, and picom has
/// their images. After WS_WARM_ROUND_MAX_US, logs those that did not, and says yes.
static bool ws_watched_drawn(void) {
	auto waited = wsd.frame_time_us - wsd.warm.step_us;
	bool late = waited >= WS_WARM_ROUND_MAX_US;
	auto n = wsd.warm.n_watch;
	xcb_xfixes_fetch_region_cookie_t *cookies = calloc(n > 0 ? n : 1, sizeof(*cookies));
	if (cookies == NULL) {
		abort();
	}
	for (unsigned i = 0; i < n; i++) {
		auto watch = &wsd.warm.watch[i];
		if (watch->drawn) {
			continue;
		}
		auto c = ws_find_client(watch->window);
		if (c == NULL) {
			// The window manager no longer manages it: there is nothing to
			// wait for.
			watch->drawn = true;
			ws_log("Warm: window %#010x went away", watch->window);
			continue;
		}
		if (watch->damage == XCB_NONE) {
			if (c->viewable) {
				ws_watch_start(watch);
			}
			continue;
		}
		xcb_damage_subtract(wsd.c, watch->damage, XCB_NONE, watch->parts);
		xcb_xfixes_subtract_region(wsd.c, watch->missing, watch->parts, watch->missing);
		cookies[i] = xcb_xfixes_fetch_region(wsd.c, watch->missing);
	}
	unsigned n_waiting = 0;
	for (unsigned i = 0; i < n; i++) {
		auto watch = &wsd.warm.watch[i];
		if (cookies[i].sequence != 0) {
			auto r = xcb_xfixes_fetch_region_reply(wsd.c, cookies[i], NULL);
			if (r != NULL && r->extents.width == 0 &&
			    wsd.api->window_has_image(watch->window)) {
				watch->drawn = true;
				ws_log("Warm: window %#010x drew all of itself in %llu "
				       "ms",
				       watch->window, (unsigned long long)waited / 1000);
			}
			free(r);
		}
		if (!watch->drawn) {
			n_waiting++;
			if (late) {
				ws_log("Warm: window %#010x did not draw all of itself "
				       "in time",
				       watch->window);
			}
		}
	}
	free(cookies);
	return n_waiting == 0 || late;
}

/// Pick the workspaces to show: on each monitor that shows a workspace with windows,
/// those with a window picom has no image of. A monitor that shows a workspace without
/// windows is left alone: some window managers remove such a workspace once it is hidden,
/// and we could not go back to it.
static void ws_warm_plan(void) {
	auto warm = &wsd.warm;
	warm->current = wsd.current >= 0 && wsd.current < wsd.n_names
	                    ? strdup(wsd.names[wsd.current])
	                    : NULL;
	for (unsigned i = 0; i < wsd.n_tracked; i++) {
		auto m = &wsd.tracked[i];
		auto shown = m->shown;
		if (shown == NULL || !ws_workspace_has_windows(shown)) {
			continue;
		}
		auto index = ws_monitor_of_name(m->name);
		auto r = ws_monitor_rect(index);
		struct ws_warm_monitor wm = {.shown = strdup(shown)};
		for (unsigned d = 0; d < wsd.n_names; d++) {
			if (ws_desktop_monitor(d) != index || strcmp(wsd.names[d], shown) == 0) {
				continue;
			}
			bool missing = false;
			for (unsigned j = 0; j < wsd.n_clients && !missing; j++) {
				auto c = &wsd.clients[j];
				missing =
				    c->desktop == d && !wsd.api->window_has_image(c->id);
			}
			if (!missing) {
				continue;
			}
			wm.show = ws_grow(wm.show, wm.n_show + 1, sizeof(*wm.show));
			wm.show[wm.n_show++] = strdup(wsd.names[d]);
			ws_log("Warm: will show \"%s\" on the monitor at %d,%d, "
			       "which shows \"%s\"",
			       wsd.names[d], r.x1, r.y1, shown);
		}
		if (wm.n_show == 0) {
			free(wm.shown);
			continue;
		}
		warm->monitors[warm->n_monitors++] = wm;
	}
}

/// Show the workspaces of the current round. Returns false if there are none left.
static bool ws_warm_show_round(void) {
	auto warm = &wsd.warm;
	ws_unwatch();
	bool any = false;
	for (unsigned i = 0; i < warm->n_monitors; i++) {
		auto wm = &warm->monitors[i];
		if (warm->round < wm->n_show) {
			ws_request_workspace(wm->show[warm->round]);
			ws_watch_workspace(wm->show[warm->round]);
			any = true;
		}
	}
	xcb_flush(wsd.c);
	warm->step_us = wsd.frame_time_us;
	return any;
}

/// Show the workspaces that were shown, the one with the focus last.
static void ws_warm_restore(void) {
	auto warm = &wsd.warm;
	ws_unwatch();
	for (unsigned i = 0; i < warm->n_monitors; i++) {
		ws_request_workspace(warm->monitors[i].shown);
		ws_watch_workspace(warm->monitors[i].shown);
	}
	if (warm->current != NULL) {
		ws_request_workspace(warm->current);
	}
	xcb_flush(wsd.c);
	warm->step = WS_WARM_RESTORE;
	warm->step_us = wsd.frame_time_us;
}

/// Whether the workspace that had the focus has it again.
static bool ws_warm_current_back(void) {
	auto current = wsd.warm.current;
	return current == NULL || (wsd.current >= 0 && wsd.current < wsd.n_names &&
	                           strcmp(wsd.names[wsd.current], current) == 0);
}

/// Whether we showed workspace `name` to get images back.
static bool ws_warm_showed(const char *name) {
	for (unsigned i = 0; i < wsd.warm.n_monitors; i++) {
		for (unsigned j = 0; j < wsd.warm.monitors[i].n_show; j++) {
			if (strcmp(wsd.warm.monitors[i].show[j], name) == 0) {
				return true;
			}
		}
	}
	return false;
}

static void ws_warm_finish(void) {
	auto warm = &wsd.warm;
	ws_unwatch();
	// While we hold the pointer, so the window manager hears of no pointer crossing
	// into a window, and doesn't focus it.
	if (warm->pointer_saved) {
		xcb_warp_pointer(wsd.c, XCB_NONE, wsd.root, 0, 0, 0, 0, warm->pointer_x,
		                 warm->pointer_y);
	}
	xcb_ungrab_pointer(wsd.c, XCB_CURRENT_TIME);
	xcb_ungrab_keyboard(wsd.c, XCB_CURRENT_TIME);
	xcb_flush(wsd.c);
	wsd.api->pause_screen("plugin-ws-direction", 0, PICOM_PAUSE_FREEZE);
	ws_log("Warm: done in %llu ms, %u rounds",
	       (unsigned long long)(wsd.frame_time_us - warm->start_us) / 1000, warm->round);
	// A keeper of a workspace we showed that has no image now does not keep it, its
	// rules changed: forget it, or we would try again and again.
	for (unsigned i = 0; i < wsd.n_keepers;) {
		auto c = ws_find_client(wsd.keepers[i]);
		if (c != NULL && c->workspace != NULL && ws_warm_showed(c->workspace) &&
		    !wsd.api->window_has_image(c->id)) {
			ws_log("Warm: window %#010x does not keep its image", c->id);
			wsd.keepers[i] = wsd.keepers[--wsd.n_keepers];
		} else {
			i++;
		}
	}
	for (unsigned i = 0; i < warm->n_monitors; i++) {
		for (unsigned j = 0; j < warm->monitors[i].n_show; j++) {
			free(warm->monitors[i].show[j]);
		}
		free(warm->monitors[i].show);
		free(warm->monitors[i].shown);
	}
	warm->n_monitors = 0;
	for (unsigned i = 0; i < warm->n_waiters; i++) {
		ws_answer(warm->waiters[i], WS_ANSWER_DONE, 0);
	}
	warm->n_waiters = 0;
	free(warm->current);
	warm->current = NULL;
	warm->step = WS_WARM_IDLE;
}

/// One pass while we get images back.
void ws_warm_advance(void) {
	auto warm = &wsd.warm;
	auto now = wsd.frame_time_us;
	wsd.api->pause_screen("plugin-ws-direction", WS_WARM_TICK, PICOM_PAUSE_FREEZE);
	switch (warm->step) {
	case WS_WARM_IDLE: return;
	case WS_WARM_SETTLE:
		if (now - wsd.root_change_us < WS_WARM_SETTLE_US &&
		    now - warm->step_us < WS_WARM_SETTLE_MAX_US) {
			return;
		}
		ws_warm_plan();
		warm->step = WS_WARM_SHOW;
		warm->round = 0;
		if (!ws_warm_show_round()) {
			ws_warm_finish();
		}
		return;
	case WS_WARM_SHOW:
		if (!ws_watched_drawn()) {
			return;
		}
		warm->round++;
		if (now - warm->start_us >= WS_WARM_MAX_US || !ws_warm_show_round()) {
			ws_warm_restore();
		}
		return;
	case WS_WARM_RESTORE:
		// The window manager moves the pointer with the focus as it shows a
		// workspace, and sets the current desktop with it. So once the current
		// desktop is back, we can put the pointer back.
		if (ws_watched_drawn() && (ws_warm_current_back() ||
		                           now - warm->step_us >= WS_WARM_ROUND_MAX_US)) {
			ws_warm_finish();
		}
		return;
	}
}

/// Freeze the pointer and the keyboard: what the user clicks and types meanwhile waits
/// in the X server, and goes to the windows that are there once we let go, not to a
/// window of a workspace we show. Remembers where the pointer is.
static bool ws_grab_input(void) {
	auto pointer_cookie =
	    xcb_grab_pointer(wsd.c, 0, wsd.root, 0, XCB_GRAB_MODE_SYNC,
	                     XCB_GRAB_MODE_ASYNC, XCB_NONE, XCB_NONE, XCB_CURRENT_TIME);
	auto keyboard_cookie = xcb_grab_keyboard(wsd.c, 0, wsd.root, XCB_CURRENT_TIME,
	                                         XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_SYNC);
	auto where_cookie = xcb_query_pointer(wsd.c, wsd.root);
	xcb_grab_pointer_reply_t *pointer = ws_reply(pointer_cookie.sequence);
	xcb_grab_keyboard_reply_t *keyboard = ws_reply(keyboard_cookie.sequence);
	xcb_query_pointer_reply_t *where = ws_reply(where_cookie.sequence);
	int pointer_status = pointer != NULL ? pointer->status : -1;
	int keyboard_status = keyboard != NULL ? keyboard->status : -1;
	bool ok = pointer_status == XCB_GRAB_STATUS_SUCCESS &&
	          keyboard_status == XCB_GRAB_STATUS_SUCCESS;
	if (!ok) {
		xcb_ungrab_pointer(wsd.c, XCB_CURRENT_TIME);
		xcb_ungrab_keyboard(wsd.c, XCB_CURRENT_TIME);
		xcb_flush(wsd.c);
		if (!wsd.grab_failed) {
			ws_log("Warm: cannot freeze the pointer (status %d) and the "
			       "keyboard "
			       "(status %d), trying again later",
			       pointer_status, keyboard_status);
		}
	}
	wsd.grab_failed = !ok;
	wsd.warm.pointer_saved = ok && where != NULL && where->same_screen;
	if (wsd.warm.pointer_saved) {
		wsd.warm.pointer_x = where->root_x;
		wsd.warm.pointer_y = where->root_y;
	}
	free(pointer);
	free(keyboard);
	free(where);
	return ok;
}

/// Start getting images back for the windows `lost`, which `why` says how they lost
/// their image, for the log. Returns 0 if we started, -1 if all of them show, as picom
/// may not have seen yet, and WS_FAILED_GRAB if we can't freeze the pointer and the
/// keyboard.
static int ws_warm_start(const xcb_window_t *lost, unsigned n_lost, const char *why) {
	if (n_lost == 0) {
		return -1;
	}
	// picom may not have seen yet that a window shows. The X server knows.
	xcb_get_window_attributes_cookie_t cookies[WS_WARM_MAX_LOST];
	for (unsigned i = 0; i < n_lost; i++) {
		cookies[i] = xcb_get_window_attributes(wsd.c, lost[i]);
	}
	unsigned n_hidden = 0;
	for (unsigned i = 0; i < n_lost; i++) {
		xcb_get_window_attributes_reply_t *attr = ws_reply(cookies[i].sequence);
		n_hidden += attr != NULL && attr->map_state != XCB_MAP_STATE_VIEWABLE;
		free(attr);
	}
	if (n_hidden == 0) {
		return -1;
	}
	if (!ws_grab_input()) {
		return WS_FAILED_GRAB;
	}
	ws_log("Warm: %u hidden windows %s, freezing the screen, the keyboard and the "
	       "pointer "
	       "(at %d,%d) to show their workspaces once (in pass %llu)",
	       n_hidden, why, wsd.warm.pointer_x, wsd.warm.pointer_y,
	       (unsigned long long)wsd.frame);
	wsd.warm.step = WS_WARM_SETTLE;
	wsd.warm.start_us = wsd.warm.step_us = wsd.frame_time_us;
	// picom lost the images of the backdrops too.
	ws_backdrops_redo();
	wsd.api->pause_screen("plugin-ws-direction", WS_WARM_TICK, PICOM_PAUSE_FREEZE);
	return 0;
}

/// The hidden windows without an image on monitors that show a workspace with windows,
/// to which we can go back: with `keepers_only`, only those we saw picom keep an image
/// of. Learns the keepers on the way.
static unsigned
ws_warm_missing(bool keepers_only, xcb_window_t lost[static WS_WARM_MAX_LOST]) {
	unsigned n_lost = 0;
	for (unsigned i = 0; i < wsd.n_clients; i++) {
		auto c = &wsd.clients[i];
		// A window the window manager didn't hide yet still has the image of the
		// shown window.
		auto m = c->desktop >= 0 && !c->viewable
		             ? ws_monitor_hiding((unsigned)c->desktop)
		             : NULL;
		if (m == NULL) {
			continue;
		}
		bool image = wsd.api->window_has_image(c->id);
		// Only once picom sees it hidden and its rules keep its image is the
		// image it has a kept one.
		if (image && wsd.api->window_keeps_image(c->id)) {
			ws_set_add(&wsd.keepers, &wsd.n_keepers, c->id);
		} else if (!image &&
		           (!keepers_only ||
		            ws_set_find(wsd.keepers, wsd.n_keepers, c->id) >= 0) &&
		           n_lost < WS_WARM_MAX_LOST && ws_workspace_has_windows(m->shown)) {
			lost[n_lost++] = c->id;
		}
	}
	return n_lost;
}

/// Learn which hidden windows picom keeps an image of, and start getting images back if
/// one of them lost it: when picom reinitializes, or the window manager restarts and puts
/// the windows in new frames. Unless the root window property says "off". Only for
/// monitors that show a workspace with windows, to which we can go back. For the others
/// we start once they do, in the pass of that switch, which then shows without its
/// animation.
void ws_warm_check(void) {
	xcb_window_t lost[WS_WARM_MAX_LOST];
	auto n_lost = ws_warm_missing(true, lost);
	if (!wsd.warm_off) {
		ws_warm_start(lost, n_lost, "lost their image");
	}
}

/// The command asks us to get images back now, for every hidden window without one, and
/// to answer window `reply` once we are done.
void ws_warm_command(xcb_window_t reply) {
	if (ws_any_unfolded()) {
		ws_log("Warm: asked while a monitor is unfolded");
		ws_answer(reply, WS_ANSWER_FAILED, WS_FAILED_UNFOLDED);
		return;
	}
	xcb_window_t lost[WS_WARM_MAX_LOST];
	auto n_lost = ws_warm_missing(false, lost);
	auto started = ws_warm_start(lost, n_lost, "have no image, as asked");
	if (started < 0) {
		ws_log("Warm: asked, and every hidden window has an image");
		ws_answer(reply, WS_ANSWER_DONE, 0);
	} else if (started > 0) {
		ws_answer(reply, WS_ANSWER_FAILED, (uint32_t)started);
	} else if (reply != XCB_NONE) {
		ws_set_add(&wsd.warm.waiters, &wsd.warm.n_waiters, reply);
	}
}
