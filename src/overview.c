// SPDX-License-Identifier: MPL-2.0
// Copyright (c) JornC

/// The overview. The ws-direction command (cmd.c) sends us one client message per
/// command, to the window that owns our selection (protocol.h): open unfolds the monitor
/// with the focus, every monitor, or one by its RandR name; close folds every monitor;
/// toggle does whichever changes the monitor with the focus. We answer each command, and
/// tell a command that waits when the overview closes. We start the custom trigger
/// ws-unfold on every window of the monitor's workspaces, again instead of ws-pass on
/// every switch on that monitor while it is unfolded, and ws-fold when it folds. For a
/// switch, ws_start_passes asks ws_overview_switch_trigger which of them to start. The
/// style draws the overview; it can hold its pose with picom's hold output. While a
/// monitor is unfolded a window of ours over it takes its clicks, so they don't land on
/// windows that are drawn elsewhere. A click of the first button on a window of one of
/// its workspaces picks that window, on the release: we ask the window manager to show it
/// and give it the focus, with the _NET_ACTIVE_WINDOW request pagers send, fold the other
/// unfolded monitors, and fold this one once the window's workspace shows. A click where
/// no window is drawn does nothing. The other buttons do nothing either, but a command
/// that waits for them hears of them, so the window manager config says what they do.
/// picom tells us which window it drew under the click (window_at): the X server only
/// knows where the windows really are. picom runs a pass when our connection has data, or
/// xcb has events of it in its queue (add_wakeup_fd), so we see the click while picom has
/// nothing to draw.

#include <stdlib.h>
#include <string.h>

#include "protocol.h"
#include "ws_direction.h"

/// Whether window `c` takes part when the monitor with index `index` switches: it is on
/// one of its workspaces. With `overview`, for ws-unfold and ws-fold, also a window on
/// all desktops that is on the monitor: the style draws it on the shown tile.
bool ws_overview_member(const struct ws_client *c, int index, bool overview) {
	if (c->desktop >= 0) {
		return ws_desktop_monitor((unsigned)c->desktop) == index;
	}
	// One that isn't viewable, like one in i3's scratchpad, is not drawn.
	return overview && c->sticky && c->viewable && ws_rect_monitor(c->rect) == index;
}

/// Start `trigger` on every window of the workspaces of monitor `m`, and on the windows
/// on all desktops there, not for a switch.
static void ws_start_on_monitor(struct ws_monitor *m, const char *trigger) {
	auto index = ws_monitor_of_name(m->name);
	unsigned members = 0, started = 0;
	bool unfold = strcmp(trigger, WS_UNFOLD_TRIGGER) == 0;
	m->n_waiting = 0;
	for (unsigned i = 0; i < wsd.n_clients; i++) {
		auto c = &wsd.clients[i];
		if (!ws_overview_member(c, index, true)) {
			continue;
		}
		members++;
		started += ws_start(c->id, trigger, m, 0, 0);
		if (unfold && !wsd.api->window_has_image(c->id)) {
			// picom has no image of it yet, as of a window that just opened,
			// and ignores the start: we start it once picom has one, see
			// ws_overview_retry.
			m->waiting =
			    ws_grow(m->waiting, m->n_waiting + 1, sizeof(*m->waiting));
			m->waiting[m->n_waiting++] = c->id;
		}
	}
	auto backdrops = ws_backdrops_start(m, trigger, 0, 0);
	auto r = ws_monitor_rect(index);
	ws_log(
	    "Monitor at %d,%d: started %s on %u of %u windows and %u backdrops (in pass "
	    "%llu)",
	    r.x1, r.y1, trigger, started, members, backdrops, (unsigned long long)wsd.frame);
	if (unfold) {
		ws_overview_started(m);
	}
}

/// In every pass: start ws-unfold on the windows of an unfolded monitor that picom had no
/// image of when we started it on the others, once it has one.
void ws_overview_retry(void) {
	for (unsigned i = 0; i < wsd.n_tracked; i++) {
		auto m = &wsd.tracked[i];
		auto index = ws_monitor_of_name(m->name);
		unsigned kept = 0;
		for (unsigned k = 0; m->unfolded && k < m->n_waiting; k++) {
			auto c = ws_find_client(m->waiting[k]);
			if (c == NULL || !ws_overview_member(c, index, true)) {
				continue;
			}
			if (!wsd.api->window_has_image(c->id)) {
				m->waiting[kept++] = c->id;
				continue;
			}
			ws_log("Unfold: picom has an image of window %#010x now", c->id);
			ws_start(c->id, WS_UNFOLD_TRIGGER, m, 0, 0);
		}
		m->n_waiting = kept;
	}
}

/// The workspaces and windows of the monitor with index `index`, as a string.
static char *ws_overview_of(int index) {
	const char *list[WS_MAX_DESKTOPS];
	auto n = ws_monitor_list(index, list);
	size_t len = 1;
	for (unsigned i = 0; i < n; i++) {
		len += strlen(list[i]) + 1;
	}
	for (unsigned i = 0; i < wsd.n_clients; i++) {
		auto c = &wsd.clients[i];
		len += ws_overview_member(c, index, true)
		           ? 12 + (c->workspace ? strlen(c->workspace) : 1)
			   : 0;
	}
	char *ret = malloc(len), *p = ret;
	if (ret == NULL) {
		abort();
	}
	*p = '\0';
	for (unsigned i = 0; i < n; i++) {
		p += sprintf(p, "%s\n", list[i]);
	}
	for (unsigned i = 0; i < wsd.n_clients; i++) {
		auto c = &wsd.clients[i];
		if (ws_overview_member(c, index, true)) {
			p += sprintf(p, "%x:%s\n", c->id, c->workspace ? c->workspace : "*");
		}
	}
	return ret;
}

/// We started ws-unfold on the windows of monitor `m`: remember what they were.
void ws_overview_started(struct ws_monitor *m) {
	free(m->unfolded_as);
	m->unfolded_as = ws_overview_of(ws_monitor_of_name(m->name));
}

/// After a read: start ws-unfold again on an unfolded monitor whose workspaces or windows
/// changed since we last started it, so the tiles follow (a window opened, closed or
/// moved, or a workspace came or went). A style goes on from where each window is by
/// carrying its variables.
void ws_overview_refresh(void) {
	for (unsigned i = 0; i < wsd.n_tracked; i++) {
		auto m = &wsd.tracked[i];
		if (!m->unfolded) {
			continue;
		}
		char *now = ws_overview_of(ws_monitor_of_name(m->name));
		bool same = m->unfolded_as != NULL && strcmp(now, m->unfolded_as) == 0;
		free(now);
		if (!same) {
			ws_log("Unfold: the workspaces or windows of an unfolded monitor "
			       "changed");
			ws_start_on_monitor(m, WS_UNFOLD_TRIGGER);
		}
	}
}

bool ws_any_unfolded(void) {
	for (unsigned i = 0; i < wsd.n_tracked; i++) {
		if (wsd.tracked[i].unfolded) {
			return true;
		}
	}
	return false;
}

/// While a monitor is unfolded, its clicks are ours: windows are not where the overview
/// draws them, so a click would land on the wrong one. An InputOnly window over the
/// monitor takes them (see ws_pick), and only there: the other monitors work as usual.
/// It is stacked right above the highest window the overview draws on the monitor, so
/// what is above that, like a menu or a notification, keeps its clicks. Returns false if
/// the X server would not make it.
static bool ws_input_create(struct ws_monitor *m) {
	auto r = ws_monitor_rect(ws_monitor_of_name(m->name));
	m->input = xcb_generate_id(wsd.c);
	m->input_rect = r;
	auto err = xcb_request_check(
	    wsd.c,
	    xcb_create_window_checked(
	        wsd.c, 0, m->input, wsd.root, (int16_t)r.x1, (int16_t)r.y1,
	        (uint16_t)(r.x2 - r.x1), (uint16_t)(r.y2 - r.y1), 0, XCB_WINDOW_CLASS_INPUT_ONLY,
	        XCB_COPY_FROM_PARENT, XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK,
	        (uint32_t[]){1, XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE}));
	if (err == NULL) {
		err = xcb_request_check(wsd.c, xcb_map_window_checked(wsd.c, m->input));
	}
	if (err != NULL) {
		ws_log("Unfold: cannot make the window that takes the clicks (error %u)",
		       err->error_code);
		free(err);
		xcb_destroy_window(wsd.c, m->input);
		m->input = XCB_NONE;
		return false;
	}
	ws_place_inputs();
	return true;
}

static void ws_input_destroy(struct ws_monitor *m) {
	if (m->input != XCB_NONE) {
		xcb_destroy_window(wsd.c, m->input);
		xcb_flush(wsd.c);
		m->input = XCB_NONE;
	}
}

/// Fold monitor `m`, not for a switch: start ws-fold, and give back its clicks.
static void ws_fold(struct ws_monitor *m) {
	m->unfolded = false;
	m->n_waiting = 0;
	free(m->unfolded_as);
	m->unfolded_as = NULL;
	ws_input_destroy(m);
	ws_start_on_monitor(m, WS_FOLD_TRIGGER);
}

/// Keep each input window over its monitor, right above the highest window the overview
/// draws there, as the last read saw the stack. Called after every read.
void ws_place_inputs(void) {
	for (unsigned i = 0; i < wsd.n_tracked; i++) {
		auto m = &wsd.tracked[i];
		if (m->input == XCB_NONE) {
			continue;
		}
		auto index = ws_monitor_of_name(m->name);
		auto r = ws_monitor_rect(index);
		if (memcmp(&r, &m->input_rect, sizeof(r)) != 0) {
			m->input_rect = r;
			xcb_configure_window(
			    wsd.c, m->input,
			    XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
			        XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
			    (uint32_t[]){(uint32_t)r.x1, (uint32_t)r.y1,
			                 (uint32_t)(r.x2 - r.x1), (uint32_t)(r.y2 - r.y1)});
		}
		// The stack is bottom to top.
		int highest = -1, input = -1;
		for (unsigned k = 0; k < wsd.n_stack; k++) {
			if (wsd.stack[k] == m->input) {
				input = (int)k;
				continue;
			}
			auto c = ws_find_client_by_frame(wsd.stack[k]);
			bool drawn = c != NULL &&
			             (c->desktop >= 0
			                  ? ws_desktop_monitor((unsigned)c->desktop) == index
			                  : ws_rect_monitor(c->rect) == index);
			if (drawn) {
				highest = (int)k;
			}
		}
		if (highest < 0 && input == (int)wsd.n_stack - 1) {
			continue;
		}
		if (highest >= 0 && input == highest + 1) {
			continue;
		}
		if (highest >= 0) {
			xcb_configure_window(
			    wsd.c, m->input,
			    XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE,
			    (uint32_t[]){wsd.stack[highest], XCB_STACK_MODE_ABOVE});
		} else {
			xcb_configure_window(wsd.c, m->input, XCB_CONFIG_WINDOW_STACK_MODE,
			                     (uint32_t[]){XCB_STACK_MODE_ABOVE});
		}
	}
	xcb_flush(wsd.c);
}

/// Send answer `code` with `detail` to window `w` of a command.
void ws_answer(xcb_window_t w, uint32_t code, uint32_t detail) {
	if (w == XCB_NONE) {
		return;
	}
	xcb_client_message_event_t event = {
	    .response_type = XCB_CLIENT_MESSAGE,
	    .format = 32,
	    .window = w,
	    .type = wsd.a_answer,
	    .data.data32 = {code, detail},
	};
	// If the command is gone, the X server sends us an error, which we drop.
	xcb_send_event(wsd.c, 0, w, XCB_EVENT_MASK_NO_EVENT, (const char *)&event);
	xcb_flush(wsd.c);
}

/// Answer the commands that wait, and forget them: all of them, or with `buttons` only
/// those that wait for the other buttons too.
static void ws_answer_waiters(uint32_t code, uint32_t detail, bool buttons) {
	unsigned kept = 0;
	for (unsigned i = 0; i < wsd.n_waiters; i++) {
		auto w = wsd.waiters[i];
		if (buttons && !(w.flags & WS_WAIT_BUTTONS)) {
			wsd.waiters[kept++] = w;
			continue;
		}
		ws_answer(w.window, code, detail);
	}
	wsd.n_waiters = kept;
}

/// A button other than the first was pressed while a monitor was unfolded.
void ws_button(uint8_t button) {
	ws_log("Unfold: button %u pressed", button);
	ws_answer_waiters(WS_ANSWER_BUTTON, button, true);
}

/// A click folded the overview of a monitor. Once none is unfolded, tell the
/// commands that wait.
void ws_pick_done(void) {
	if (ws_any_unfolded()) {
		return;
	}
	ws_answer_waiters(WS_ANSWER_PICKED, 0, false);
}

/// Ask the window manager to show window `w` and give it the focus, with the EWMH request
/// pagers send. For a window of a hidden workspace, it shows that workspace (i3 does).
static void ws_request_window(xcb_window_t w, xcb_timestamp_t time) {
	xcb_client_message_event_t event = {
	    .response_type = XCB_CLIENT_MESSAGE,
	    .format = 32,
	    .window = w,
	    .type = wsd.a_active_window,
	    // 2: the request comes from a pager, for the user.
	    .data.data32 = {2, time, XCB_NONE},
	};
	xcb_send_event(wsd.c, 0, wsd.root,
	               XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT,
	               (const char *)&event);
	xcb_flush(wsd.c);
}

/// Ask the window manager to show desktop `desktop`, with the EWMH request pagers send.
static void ws_request_desktop(unsigned desktop, xcb_timestamp_t time) {
	xcb_client_message_event_t event = {
	    .response_type = XCB_CLIENT_MESSAGE,
	    .format = 32,
	    .window = wsd.root,
	    .type = wsd.a_current_desktop,
	    .data.data32 = {desktop, time},
	};
	xcb_send_event(wsd.c, 0, wsd.root,
	               XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT,
	               (const char *)&event);
	xcb_flush(wsd.c);
}

/// A click while a monitor is unfolded: pick the window that picom drew there, if it is
/// on a workspace of an unfolded monitor. The other unfolded monitors fold at once. Its
/// monitor folds when its workspace shows, at once if it shows already;
/// ws_overview_switch_trigger folds it for a switch.
void ws_pick(void) {
	uint32_t client = XCB_NONE;
	uint32_t window = wsd.api->window_at(wsd.click.x, wsd.click.y, &client);
	auto c = client != XCB_NONE ? ws_find_client(client) : NULL;
	// A backdrop picks its workspace.
	const char *workspace = c != NULL ? c->workspace : ws_backdrop_workspace(window);
	auto desktop = ws_find_desktop(workspace);
	auto m = desktop >= 0 ? ws_monitor_of((unsigned)desktop) : NULL;
	if (m == NULL || !m->unfolded) {
		ws_log("Unfold: the click at %d,%d is on no window of an unfolded "
		       "monitor (window %#010x, client %#010x)",
		       wsd.click.x, wsd.click.y, window, client);
		return;
	}
	if (c != NULL) {
		ws_log("Unfold: the click at %d,%d picks window %#010x of workspace "
		       "\"%s\"",
		       wsd.click.x, wsd.click.y, client, workspace);
		ws_request_window(client, wsd.click.time);
	} else {
		ws_log("Unfold: the click at %d,%d picks workspace \"%s\" by its "
		       "backdrop",
		       wsd.click.x, wsd.click.y, workspace);
		ws_request_desktop((unsigned)desktop, wsd.click.time);
	}
	for (unsigned i = 0; i < wsd.n_tracked; i++) {
		auto other = &wsd.tracked[i];
		if (other != m && other->unfolded) {
			free(other->pick);
			other->pick = NULL;
			ws_fold(other);
		}
	}
	free(m->pick);
	m->pick = NULL;
	if (m->shown != NULL && strcmp(m->shown, workspace) == 0) {
		ws_fold(m);
		ws_pick_done();
		return;
	}
	m->pick = strdup(workspace);
	if (m->pick == NULL) {
		abort();
	}
}

/// The trigger to start on the windows of switch `s` instead of ws-pass: ws-fold for the
/// switch to the workspace a click picked, which folds the monitor and sets `*folded`,
/// and ws-unfold while the monitor is unfolded. NULL for neither. Either way the pick is
/// used up.
const char *
ws_overview_switch_trigger(struct ws_monitor *m, const struct ws_switch *s, bool *folded) {
	bool picked = m->pick != NULL && strcmp(s->to, m->pick) == 0;
	free(m->pick);
	m->pick = NULL;
	if (picked) {
		m->unfolded = false;
		free(m->unfolded_as);
		m->unfolded_as = NULL;
		ws_input_destroy(m);
		*folded = true;
		return WS_FOLD_TRIGGER;
	}
	return m->unfolded ? WS_UNFOLD_TRIGGER : NULL;
}

/// Fold every unfolded monitor, not for a switch.
static void ws_fold_all(void) {
	for (unsigned i = 0; i < wsd.n_tracked; i++) {
		auto m = &wsd.tracked[i];
		free(m->pick);
		m->pick = NULL;
		if (m->unfolded) {
			ws_fold(m);
		}
	}
}

/// Whether any desktop is on the monitor with index `index`.
static bool ws_has_workspaces(int index) {
	for (unsigned d = 0; d < wsd.n_names; d++) {
		if (ws_desktop_monitor(d) == index) {
			return true;
		}
	}
	return false;
}

/// Unfold the monitors of `target` (enum ws_target), `output` the atom of a RandR name.
/// Returns 0, or why it could not (enum ws_failure).
static uint32_t ws_open(uint32_t target, xcb_atom_t output) {
	int indexes[WS_MAX_MONITORS + 1];
	unsigned n = 0;
	if (target == WS_TARGET_FOCUS) {
		if (wsd.current < 0 || wsd.current >= wsd.n_names) {
			return WS_FAILED_NO_CURRENT;
		}
		indexes[n++] = ws_desktop_monitor((unsigned)wsd.current);
	} else if (target == WS_TARGET_ALL) {
		for (int index = -1; index < wsd.n_monitors; index++) {
			if (ws_has_workspaces(index)) {
				indexes[n++] = index;
			}
		}
	} else if (target == WS_TARGET_OUTPUT) {
		auto index = ws_monitor_of_name(output);
		if (output == XCB_NONE || index < 0 || !ws_has_workspaces(index)) {
			return WS_FAILED_NO_OUTPUT;
		}
		indexes[n++] = index;
	} else {
		return WS_FAILED_UNKNOWN;
	}
	for (unsigned i = 0; i < n; i++) {
		auto m = ws_get_monitor(indexes[i]);
		if (m == NULL || m->unfolded) {
			continue;
		}
		if (!ws_input_create(m)) {
			return WS_FAILED_INPUT;
		}
		m->unfolded = true;
		ws_start_on_monitor(m, WS_UNFOLD_TRIGGER);
	}
	return 0;
}

/// Do the commands that came, in order, and answer them.
void ws_run_commands(void) {
	for (unsigned i = 0; i < wsd.n_commands; i++) {
		auto data = wsd.commands[i].data;
		uint32_t command = data[0], target = data[1], flags = data[4];
		xcb_atom_t output = data[2];
		xcb_window_t reply = data[3];
		uint32_t failed = 0;
		ws_log("Unfold: command %u, target %u, flags %u", command, target, flags);
		if (command == WS_COMMAND_OPEN) {
			failed = ws_open(target, output);
			if (failed == 0 && (flags & WS_WAIT) && reply != XCB_NONE) {
				wsd.waiters = ws_grow(wsd.waiters, wsd.n_waiters + 1,
				                      sizeof(*wsd.waiters));
				wsd.waiters[wsd.n_waiters++] =
				    (struct ws_waiter){.window = reply, .flags = flags};
			}
		} else if (command == WS_COMMAND_WARM) {
			// Answered once the images are back.
			ws_warm_command(reply);
			continue;
		} else if (command == WS_COMMAND_CLOSE || command == WS_COMMAND_TOGGLE) {
			// A command that waits hears of every close and toggle, whatever
			// it does.
			ws_answer_waiters(WS_ANSWER_CLOSED, 0, false);
			auto focused = wsd.current >= 0 && wsd.current < wsd.n_names
			                   ? ws_monitor_of((unsigned)wsd.current)
			                   : NULL;
			if (command == WS_COMMAND_CLOSE ||
			    (focused != NULL && focused->unfolded)) {
				ws_fold_all();
			} else {
				failed = ws_open(WS_TARGET_FOCUS, XCB_NONE);
			}
		} else {
			failed = WS_FAILED_UNKNOWN;
		}
		if (failed != 0) {
			ws_log("Unfold: command %u failed (%u)", command, failed);
		}
		ws_answer(reply, failed != 0 ? WS_ANSWER_FAILED : WS_ANSWER_DONE, failed);
	}
	wsd.n_commands = 0;
}

/// Take the selection the command looks for, on screen `screen`, with a window of our
/// own that says our protocol version. Returns false if another program has it.
bool ws_overview_init(int screen) {
	char name[32];
	snprintf(name, sizeof(name), WS_SELECTION_PREFIX "%d", screen);
	auto selection = ws_atom(name);
	wsd.a_command = ws_atom(WS_COMMAND_MESSAGE);
	wsd.a_answer = ws_atom(WS_ANSWER_MESSAGE);
	wsd.owner = xcb_generate_id(wsd.c);
	xcb_create_window(wsd.c, 0, wsd.owner, wsd.root, -1, -1, 1, 1, 0,
	                  XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT,
	                  XCB_CW_OVERRIDE_REDIRECT, (uint32_t[]){1});
	uint32_t version = WS_PROTOCOL_VERSION;
	xcb_change_property(wsd.c, XCB_PROP_MODE_REPLACE, wsd.owner,
	                    ws_atom(WS_VERSION_PROPERTY), XCB_ATOM_CARDINAL, 32, 1, &version);
	xcb_set_selection_owner(wsd.c, wsd.owner, selection, XCB_CURRENT_TIME);
	auto owner = xcb_get_selection_owner_reply(
	    wsd.c, xcb_get_selection_owner(wsd.c, selection), NULL);
	bool ok = owner != NULL && owner->owner == wsd.owner;
	free(owner);
	return ok;
}
