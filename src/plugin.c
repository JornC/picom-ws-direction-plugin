// SPDX-License-Identifier: MPL-2.0
// Copyright (c) JornC

/// picom plugin that works out the direction of a workspace switch, so animation scripts
/// can slide windows the right way, the monitor of a window's workspace, so they can
/// crop to it, and where the window's workspace is among the monitor's workspaces, so
/// they can draw the workspaces a switch passes. Its animation variables are
/// plugin-ws-direction-direction, -distance, -monitor-x, -monitor-y, -monitor-width,
/// -monitor-height, -faces, -face, -selected, -placed, -index, -shown and -unfolded (enum
/// ws_variable). The README, under Reference, and the man page say what each one is.
///
/// A window is on one workspace if the window manager lists it in _NET_CLIENT_LIST, its
/// _NET_WM_DESKTOP names one desktop, and it doesn't have _NET_WM_STATE_HIDDEN (i3 sets
/// that on a tab that isn't the selected one; it would not be seen on its workspace).
///
/// For workspaces it only uses EWMH data the window manager already sets:
/// _NET_CURRENT_DESKTOP, _NET_DESKTOP_NAMES, _NET_DESKTOP_VIEWPORT and _NET_CLIENT_LIST
/// on the root window, and _NET_WM_DESKTOP and _NET_WM_STATE on client windows, and the
/// _NET_ACTIVE_WINDOW request that pagers send. Monitors come from RandR, a window's
/// place from GetGeometry and TranslateCoordinates.
///
/// The order in which a window manager changes these properties and maps and unmaps
/// windows is not specified, and those changes can reach picom over several frames. So we
/// take them in the order the X server made them: our own connection gets the root
/// window's property changes and every map, unmap and change of a top-level window
/// (SubstructureNotify), and each one but a move or a restack makes the frame callback
/// read the root window and the managed windows again, before picom asks. passes.c finds
/// the switches on every monitor from that, and which switch each show and hide is part
/// of. picom calls us at the start of every pass (a frame callback), and we start ws-pass
/// there for a new switch. The first animation start in a pass reads too, unless the
/// frame callback just did, or the window closes.
///
/// picom runs passes also while it doesn't composite (see unredir-if-possible), so we see
/// those switches too, but picom doesn't draw their animations.
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
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "protocol.h"
#include "version.h"
#include "ws_direction.h"

/// How long a pass waits for the X server to answer a read of the root window and the
/// managed windows. An idle server answers well within it; one that is busy, often with
/// the switch itself, can take tens of milliseconds, and then we let picom go on, with
/// the screen paused, and take the replies in a later pass.
#define WS_READ_WAIT_US 1000
/// While a read waits, we pause the screen for this many seconds at a time, again in
/// every pass. So a pass comes at least this often.
#define WS_READ_TICK 0.002

static const char *const ws_variable_names[] = {
    [WS_VAR_DIRECTION] = "direction",
    [WS_VAR_DISTANCE] = "distance",
    [WS_VAR_MONITOR_X] = "monitor-x",
    [WS_VAR_MONITOR_Y] = "monitor-y",
    [WS_VAR_MONITOR_WIDTH] = "monitor-width",
    [WS_VAR_MONITOR_HEIGHT] = "monitor-height",
    [WS_VAR_FACES] = "faces",
    [WS_VAR_FACE] = "face",
    [WS_VAR_SELECTED] = "selected",
    [WS_VAR_PLACED] = "placed",
    [WS_VAR_INDEX] = "index",
    [WS_VAR_SHOWN] = "shown",
    [WS_VAR_UNFOLDED] = "unfolded",
};
static_assert(sizeof(ws_variable_names) / sizeof(ws_variable_names[0]) == WS_N_VARIABLES,
              "every variable needs a name");

struct ws_state wsd = {.current = -1, .frame = UINT64_MAX, .read_frame = UINT64_MAX};

__attribute__((format(printf, 1, 2))) void ws_log(const char *fmt, ...) {
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

/// Why the plugin could not start: to picom's standard error, and to the log.
__attribute__((format(printf, 1, 2))) static void ws_fail(const char *fmt, ...) {
	char reason[256];
	va_list args;
	va_start(args, fmt);
	vsnprintf(reason, sizeof(reason), fmt, args);
	va_end(args);
	fprintf(stderr, "ws-direction: %s\n", reason);
	ws_log("%s", reason);
}

uint64_t ws_now_us(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000;
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

/// Whether xcb has an event of ours in its queue, which it read while we waited for a
/// reply, or we took one that the next read must take in, or a command waits. picom asks
/// before it sleeps, see add_wakeup_fd.
static bool ws_pending([[maybe_unused]] void *user_data) {
	if (wsd.queued == NULL) {
		wsd.queued = xcb_poll_for_queued_event(wsd.c);
	}
	return wsd.queued != NULL || wsd.read.needed || wsd.n_commands > 0;
}

static xcb_generic_event_t *ws_next_event(void) {
	auto event = wsd.queued;
	wsd.queued = NULL;
	return event != NULL ? event : xcb_poll_for_event(wsd.c);
}

static void ws_add_event(enum ws_event_type type, xcb_window_t window, uint32_t sequence) {
	wsd.events = ws_grow(wsd.events, wsd.n_events + 1, sizeof(*wsd.events));
	wsd.events[wsd.n_events++] = (struct ws_event){type, window, sequence};
}

/// Take all events, so they don't pile up in the connection's queue: the root window's
/// property changes, the maps, unmaps and changes of top-level windows, and the clicks
/// while a monitor is unfolded. The server also sends every client a few, like
/// MappingNotify. Sets wsd.read.needed for a change that the
/// next read must take in.
void ws_take_events(void) {
	xcb_generic_event_t *event;
	while ((event = ws_next_event()) != NULL) {
		auto type = event->response_type & 0x7f;
		auto sequence = event->full_sequence;
		switch (type) {
		case XCB_PROPERTY_NOTIFY: {
			auto e = (xcb_property_notify_event_t *)event;
			if (e->atom == wsd.a_current_desktop) {
				ws_add_event(WS_EVENT_CURRENT, XCB_NONE, sequence);
			}
			bool root = e->atom == wsd.a_current_desktop ||
			            e->atom == wsd.a_desktop_names ||
			            e->atom == wsd.a_desktop_viewport ||
			            e->atom == wsd.a_client_list;
			wsd.read.needed |= root;
			break;
		}
		case XCB_CLIENT_MESSAGE: {
			auto e = (xcb_client_message_event_t *)event;
			if (e->window != wsd.owner || e->type != wsd.a_command ||
			    e->format != 32) {
				break;
			}
			wsd.commands =
			    ws_grow(wsd.commands, wsd.n_commands + 1, sizeof(*wsd.commands));
			memcpy(wsd.commands[wsd.n_commands++].data, e->data.data32,
			       sizeof(wsd.commands[0].data));
			break;
		}
		case XCB_SELECTION_CLEAR:
			ws_log("Another program took our selection: the ws-direction "
			       "command "
			       "talks to it now");
			break;
		case XCB_MAP_NOTIFY:
			ws_add_event(WS_EVENT_MAP,
			             ((xcb_map_notify_event_t *)event)->window, sequence);
			wsd.read.needed = true;
			break;
		case XCB_UNMAP_NOTIFY:
			ws_add_event(WS_EVENT_UNMAP,
			             ((xcb_unmap_notify_event_t *)event)->window, sequence);
			wsd.read.needed = true;
			break;
		case XCB_DESTROY_NOTIFY:
		case XCB_REPARENT_NOTIFY: wsd.read.needed = true; break;
		case XCB_CONFIGURE_NOTIFY:
		case XCB_GRAVITY_NOTIFY:
		case XCB_CIRCULATE_NOTIFY:
			// A move or a restack changes no workspace, and a start reads for
			// itself. The overview's input windows follow the stack.
			wsd.read.needed |= ws_any_unfolded();
			break;
		case XCB_BUTTON_PRESS:
		case XCB_BUTTON_RELEASE: {
			// The pick comes on the release.
			auto e = (xcb_button_press_event_t *)event;
			if (e->detail > 1 && type == XCB_BUTTON_PRESS && ws_any_unfolded()) {
				ws_button(e->detail);
			} else if (e->detail == 1 && type == XCB_BUTTON_PRESS) {
				wsd.click.pressed = true;
				wsd.click.x = e->root_x;
				wsd.click.y = e->root_y;
			} else if (e->detail == 1 && wsd.click.pressed) {
				wsd.click.pressed = false;
				wsd.click.pending = true;
				wsd.click.time = e->time;
			}
			break;
		}
		default: break;
		}
		free(event);
	}
}

/// picom calls this at the start of every pass, before it starts animations. When the
/// root window or a top-level window changed, read them again, record the switches, and
/// start ws-pass for a new one.
static void ws_frame(uint64_t frame, [[maybe_unused]] void *user_data) {
	wsd.frame = frame;
	wsd.frame_time_us = ws_now_us();
	ws_take_events();

	if (wsd.read.needed && wsd.read.step == WS_READ_IDLE) {
		wsd.read.needed = false;
		ws_read_start();
	}
	if (wsd.read.step != WS_READ_IDLE) {
		// Only the pass that starts a read waits; the pause brings the others.
		auto wait = wsd.read.start_frame == wsd.frame ? WS_READ_WAIT_US : 0;
		if (!ws_read_advance(wait)) {
			// The X server is busy, often with the switch itself. Instead of
			// holding up picom, pause the screen until it answers: picom
			// draws nothing and starts no animation meanwhile, and what
			// changed then animates once the pause ends.
			wsd.read.paused = true;
			wsd.api->pause_screen("plugin-ws-direction", WS_READ_TICK, 0);
			return;
		}
		if (wsd.read.paused) {
			wsd.read.paused = false;
			wsd.api->pause_screen("plugin-ws-direction", 0, 0);
		}
		ws_place_inputs();
	}
	// A read that picom's provider made, after the callback of its pass, may have
	// recorded switches too.
	if (ws_start_passes()) {
		ws_pick_done();
	}
	if (wsd.n_commands > 0) {
		ws_run_commands();
	}
	// After a read, here or in picom's provider.
	if (wsd.read_frame != wsd.refreshed_read && ws_any_unfolded()) {
		ws_overview_refresh();
	}
	wsd.refreshed_read = wsd.read_frame;
	ws_overview_retry();
	if (wsd.click.pending) {
		wsd.click.pending = false;
		if (ws_any_unfolded()) {
			ws_pick();
		}
	}
}

__attribute__((constructor)) static void ws_init(void) {
	auto log_path = getenv("WS_DIRECTION_LOG");
	if (log_path != NULL && *log_path) {
		wsd.log = fopen(log_path, "a");
	}
	ws_log("ws-direction %s", WS_DIRECTION_VERSION);
	auto api = wsd.api = picom_api_get_interfaces(0, WS_API_MINOR, "ws-direction");
	if (api == NULL) {
		ws_fail("picom's plugin API is not compatible: ws-direction needs plugin "
		        "API " WS_API_VERSION " of the picom fork");
		return;
	}
	int screen_num = 0;
	wsd.c = xcb_connect(NULL, &screen_num);
	if (xcb_connection_has_error(wsd.c)) {
		ws_fail("cannot connect to the X server, "
		        "not adding the plugin-ws-direction-* variables");
		xcb_disconnect(wsd.c);
		wsd.c = NULL;
		return;
	}
	auto it = xcb_setup_roots_iterator(xcb_get_setup(wsd.c));
	for (int i = screen_num; i > 0 && it.rem; i--) {
		xcb_screen_next(&it);
	}
	if (!it.rem) {
		ws_fail("the display's screen does not exist, "
		        "not adding the plugin-ws-direction-* variables");
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
	wsd.a_client_list = ws_atom("_NET_CLIENT_LIST");
	wsd.a_wm_state = ws_atom("_NET_WM_STATE");
	wsd.a_wm_state_hidden = ws_atom("_NET_WM_STATE_HIDDEN");
	wsd.a_active_window = ws_atom("_NET_ACTIVE_WINDOW");

	// Selected before the first read, so no change after it is missed.
	xcb_change_window_attributes(wsd.c, wsd.root, XCB_CW_EVENT_MASK,
	                             (uint32_t[]){XCB_EVENT_MASK_PROPERTY_CHANGE |
	                                          XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY});

	// Learn what each monitor shows now, before the first switch.
	wsd.frame_time_us = ws_now_us();
	ws_read();

	// Only now: a variable must never exist without the connection that gives it a
	// value, nor without the frame callback that tells it the pass.
	if (!api->add_frame_callback(ws_frame, NULL)) {
		ws_fail("could not add the frame callback, "
		        "not adding the plugin-ws-direction-* variables");
		return;
	}
	if (!api->add_wakeup_fd(xcb_get_file_descriptor(wsd.c), ws_pending, NULL)) {
		ws_fail("picom cannot watch the X connection, "
		        "not adding the plugin-ws-direction-* variables");
		return;
	}
	// So the user's config can use them without listing them in animation-triggers.
	// Like the variables, only once the frame callback, which starts them, is there.
	const char *const triggers[] = {WS_PASS_TRIGGER, WS_UNFOLD_TRIGGER, WS_FOLD_TRIGGER};
	for (size_t i = 0; i < sizeof(triggers) / sizeof(triggers[0]); i++) {
		if (!api->add_animation_trigger(triggers[i])) {
			ws_fail("could not declare the trigger %s", triggers[i]);
			return;
		}
	}
	for (uintptr_t i = 0; i < WS_N_VARIABLES; i++) {
		if (!api->add_animation_variable("plugin-ws-direction", ws_variable_names[i],
		                                 ws_provide, (void *)i)) {
			ws_fail("could not add plugin-ws-direction-%s", ws_variable_names[i]);
			return;
		}
	}
	if (!ws_overview_init(screen_num)) {
		ws_fail("another program owns the selection " WS_SELECTION_PREFIX "%d, "
		        "maybe "
		        "another picom with the plugin: the ws-direction command talks "
		        "to it, not "
		        "to this one",
		        screen_num);
	}
}
