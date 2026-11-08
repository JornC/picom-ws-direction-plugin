// SPDX-License-Identifier: MPL-2.0
// Copyright (c) JornC

/// What the unit tests share: stand-ins for what plugin.c gives the other modules (the
/// state, the log and the clock), a picom API that records the animations it starts and
/// whose window_has_image says yes, and helpers that set up monitors, workspaces and
/// windows, and play what a window manager does: maps, unmaps and changes
/// of the current desktop, as the events a read takes in. Nothing here talks to an X
/// server: the tests call only functions that work on the state.

#pragma once

#include "test.h"
#include "ws_direction.h"

/// The harness's two monitors: a landscape one, and a portrait one to its right.
#define LANDSCAPE ((struct ws_rect){.x1 = 0, .y1 = 480, .x2 = 3440, .y2 = 1920})
#define PORTRAIT ((struct ws_rect){.x1 = 3440, .y1 = 0, .x2 = 4520, .y2 = 1920})
/// Two monitors, one above the other.
#define TOP ((struct ws_rect){.x1 = 0, .y1 = 0, .x2 = 1920, .y2 = 1080})
#define BOTTOM ((struct ws_rect){.x1 = 0, .y1 = 1080, .x2 = 1920, .y2 = 2160})

/// The last line the modules logged.
extern char test_log[512];

/// An animation the plugin started.
struct test_start {
	uint32_t window;
	const char *trigger;
};
extern struct test_start test_starts[64];
extern unsigned test_n_starts;

/// The variables ws_compute works out for a window, by name.
struct fx_values {
	double direction, distance, x, y, width, height, faces, face, selected, placed,
	    index, shown, unfolded;
};

/// Forget everything: no monitors, workspaces or windows, a screen of 4520x1920.
void fx_reset(void);
/// Add a monitor. Its RandR name is its index plus 1. Returns its index.
int fx_monitor(struct ws_rect r);
/// Add a workspace on monitor `monitor`, with the monitor's corner as its viewport.
/// Returns its desktop.
unsigned fx_workspace(const char *name, int monitor);
/// Add a window the window manager manages, on desktop `desktop` (-1 for none), at `r`,
/// not viewable.
struct ws_client *fx_window(xcb_window_t id, int64_t desktop, struct ws_rect r);
/// Add a window on desktop `desktop`, in the middle of the monitor of the desktop.
struct ws_client *fx_window_on(xcb_window_t id, unsigned desktop);
/// The window `id` of the windows the window manager manages.
struct ws_client *fx_client(xcb_window_t id);
/// The window manager maps or unmaps window `id`.
void fx_map(xcb_window_t id);
void fx_unmap(xcb_window_t id);
/// Window `id` closes: the window manager no longer lists it.
void fx_close(xcb_window_t id);
/// The window manager sets _NET_CURRENT_DESKTOP to `desktop`.
void fx_current(unsigned desktop);
/// Read the root window and the windows, and take the events since the last read.
void fx_read(void);
/// The first read, with desktops `shown` on screen (their windows viewable), and
/// `current` the current desktop.
void fx_start(const unsigned *shown, unsigned n_shown, unsigned current);
/// A switch to `desktop` as i3 makes it: the current desktop, then it maps the windows
/// of `desktop` and unmaps the other viewable windows of that monitor. No read.
void fx_switch_events(unsigned desktop);
/// fx_switch_events, then a read.
void fx_switch(unsigned desktop);
/// Start the next pass.
void fx_pass(void);
/// What we know about the monitor of desktop `desktop`.
struct ws_monitor *fx_tracked(unsigned desktop);
/// The id of the latest switch on the monitor of desktop `desktop`, 0 if none.
uint64_t fx_last_switch(unsigned desktop);
/// Work out the variables of window `id` for an animation of `trigger`, in this pass.
struct fx_values fx_compute(xcb_window_t id, const char *trigger);
