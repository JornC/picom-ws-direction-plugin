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
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "version.h"
#include "ws_direction.h"

static const char *const ws_variable_names[] = {
    [WS_VAR_DIRECTION] = "direction",           [WS_VAR_MONITOR_X] = "monitor-x",
    [WS_VAR_MONITOR_Y] = "monitor-y",           [WS_VAR_MONITOR_WIDTH] = "monitor-width",
    [WS_VAR_MONITOR_HEIGHT] = "monitor-height",
};
static_assert(sizeof(ws_variable_names) / sizeof(ws_variable_names[0]) == WS_N_VARIABLES,
              "every variable needs a name");

struct ws_state wsd = {.current = -1, .frame = UINT64_MAX};

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

__attribute__((constructor)) static void ws_init(void) {
	auto log_path = getenv("WS_DIRECTION_LOG");
	if (log_path != NULL && *log_path) {
		wsd.log = fopen(log_path, "a");
	}
	ws_log("ws-direction %s", WS_DIRECTION_VERSION);
	auto api = picom_api_get_interfaces(0, WS_API_MINOR, "ws-direction");
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
			ws_fail("could not add plugin-ws-direction-%s", ws_variable_names[i]);
			return;
		}
	}
}
