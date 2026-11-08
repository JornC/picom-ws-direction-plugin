// SPDX-License-Identifier: MPL-2.0
// Copyright (c) JornC

/// What the unit tests share: stand-ins for what plugin.c gives the other modules (the
/// state, the log and the clock), a fake picom API that records the animations started,
/// and helpers that set up monitors, workspaces and windows. Nothing here talks to an X
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

/// One pass at 60 Hz, in microseconds.
#define FRAME_US 16667

/// The last line the modules logged.
extern char test_log[512];

/// An animation the fake picom API started.
struct test_start {
	uint32_t window;
	const char *trigger;
};
extern struct test_start test_starts[64];
extern unsigned test_n_starts;

/// The variables ws_compute works out for a window, by name.
struct fx_values {
	double direction, distance, x, y, width, height, faces, face, selected, placed;
};

/// Forget everything: no monitors, workspaces or windows, a screen of 4520x1920.
void fx_reset(void);
/// Add a monitor. Returns its index.
int fx_monitor(struct ws_rect r);
/// Add a workspace on monitor `monitor`, with the monitor's corner as its viewport.
/// Returns its desktop.
unsigned fx_workspace(const char *name, int monitor);
/// Add a window the window manager manages, on desktop `desktop` (-1 for none), at `r`.
struct ws_client *fx_window(xcb_window_t id, int64_t desktop, struct ws_rect r);
/// Add a window on desktop `desktop`, in the middle of the monitor of the desktop.
struct ws_client *fx_window_on(xcb_window_t id, unsigned desktop);
/// The root window says the current desktop is now `desktop`.
void fx_switch(unsigned desktop);
/// Start the next pass, `us` microseconds after the last one.
void fx_pass(uint64_t us);
/// The order of desktop `desktop`.
struct ws_order fx_order(unsigned desktop);
/// What we know about the monitor of desktop `desktop`.
struct ws_monitor *fx_tracked(unsigned desktop);
/// Work out the variables of window `id` for an animation of `trigger`, in this pass.
struct fx_values fx_compute(xcb_window_t id, const char *trigger);
