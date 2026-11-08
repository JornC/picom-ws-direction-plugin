// SPDX-License-Identifier: MPL-2.0
// Copyright (c) JornC

// Finding the monitor of a window's rect, and of a desktop: by its viewport, where the
// viewports say which monitor each desktop is on.

#include "fixture.h"

TEST(monitor_rect_has_point_edges) {
	struct ws_rect r = {.x1 = 10, .y1 = 20, .x2 = 30, .y2 = 40};
	CHECK(ws_rect_has_point(r, 10, 20));
	CHECK(ws_rect_has_point(r, 29, 39));
	CHECK(!ws_rect_has_point(r, 30, 20));
	CHECK(!ws_rect_has_point(r, 10, 40));
	CHECK(!ws_rect_has_point(r, 9, 25));
}

TEST(monitor_of_rect_none) {
	fx_reset();
	CHECK_INT(ws_rect_monitor((struct ws_rect){.x2 = 10, .y2 = 10}), -1);
}

TEST(monitor_of_rect_side_by_side) {
	fx_reset();
	fx_monitor(LANDSCAPE);
	fx_monitor(PORTRAIT);
	struct ws_rect inside = {.x1 = 100, .y1 = 500, .x2 = 900, .y2 = 1000};
	struct ws_rect across = {.x1 = 3000, .y1 = 500, .x2 = 3600, .y2 = 1000};
	struct ws_rect out_at_the_top = {.x1 = 100, .y1 = 400, .x2 = 900, .y2 = 1000};
	CHECK_INT(ws_rect_monitor(inside), 0);
	CHECK_INT(ws_rect_monitor(LANDSCAPE), 0);
	CHECK_INT(ws_rect_monitor(PORTRAIT), 1);
	// Across both monitors, or sticking out of one: the whole screen.
	CHECK_INT(ws_rect_monitor(across), -1);
	CHECK_INT(ws_rect_monitor(out_at_the_top), -1);
}

TEST(monitor_of_rect_stacked) {
	fx_reset();
	fx_monitor(TOP);
	fx_monitor(BOTTOM);
	struct ws_rect bottom = {.x1 = 10, .y1 = 1100, .x2 = 20, .y2 = 2160};
	struct ws_rect across = {.x1 = 10, .y1 = 1000, .x2 = 20, .y2 = 1200};
	CHECK_INT(ws_rect_monitor(bottom), 1);
	CHECK_INT(ws_rect_monitor(across), -1);
}

TEST(monitor_of_desktop_by_viewport) {
	fx_reset();
	int left = fx_monitor(LANDSCAPE), right = fx_monitor(PORTRAIT);
	unsigned one = fx_workspace("1", left), two = fx_workspace("2", right);
	CHECK(wsd.viewport_monitors);
	CHECK_INT(ws_desktop_monitor(one), left);
	CHECK_INT(ws_desktop_monitor(two), right);
	// A desktop without a viewport spans the whole screen.
	CHECK_INT(ws_desktop_monitor(2), -1);
	// So does every desktop under a window manager that sets no
	// _NET_DESKTOP_VIEWPORT.
	wsd.n_viewports = 0;
	ws_update_viewport_monitors();
	CHECK(!wsd.viewport_monitors);
	CHECK_INT(ws_desktop_monitor(one), -1);
}

TEST(monitor_equal_viewports_are_the_whole_screen) {
	// EWMH asks for 0,0 without large desktops: that says nothing about monitors,
	// even with a monitor at 0,0.
	fx_reset();
	fx_monitor(TOP);
	fx_monitor(BOTTOM);
	fx_workspace("1", 0);
	fx_workspace("2", 0);
	CHECK(!wsd.viewport_monitors);
	CHECK_INT(ws_desktop_monitor(0), -1);
	CHECK_INT(ws_desktop_monitor(1), -1);
}

TEST(monitor_viewports_not_at_monitor_corners) {
	// Large desktops: a viewport is a place on the desktop, not a monitor.
	fx_reset();
	fx_monitor(TOP);
	fx_monitor(BOTTOM);
	fx_workspace("1", 0);
	fx_workspace("2", 1);
	CHECK(wsd.viewport_monitors);
	wsd.viewports[2] = 1920;
	ws_update_viewport_monitors();
	CHECK(!wsd.viewport_monitors);
	CHECK_INT(ws_desktop_monitor(1), -1);
}

TEST(monitor_of_name) {
	fx_reset();
	fx_monitor(LANDSCAPE);
	fx_monitor(PORTRAIT);
	CHECK_INT(ws_monitor_of_name(2), 1);
	CHECK_INT(ws_monitor_of_name(XCB_NONE), -1);
	CHECK_INT(ws_monitor_of_name(7), -2);
}

TEST(monitor_find_desktop_by_name) {
	fx_reset();
	auto m = fx_monitor(LANDSCAPE);
	fx_workspace("1", m);
	fx_workspace("2:web", m);
	CHECK_INT(ws_find_desktop("2:web"), 1);
	CHECK_INT(ws_find_desktop("2"), -1);
}
