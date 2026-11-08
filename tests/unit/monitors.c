// SPDX-License-Identifier: MPL-2.0
// Copyright (c) JornC

// Finding the monitor of a point (a workspace's viewport), of a window's rect, and of a
// desktop.

#include "fixture.h"

TEST(monitor_rect_has_point_edges) {
	struct ws_rect r = {.x1 = 10, .y1 = 20, .x2 = 30, .y2 = 40};
	CHECK(ws_rect_has_point(r, 10, 20));
	CHECK(ws_rect_has_point(r, 29, 39));
	CHECK(!ws_rect_has_point(r, 30, 20));
	CHECK(!ws_rect_has_point(r, 10, 40));
	CHECK(!ws_rect_has_point(r, 9, 25));
}

TEST(monitor_of_point_side_by_side) {
	fx_reset();
	fx_monitor(LANDSCAPE);
	fx_monitor(PORTRAIT);
	CHECK_INT(ws_monitor_index(true, 0, 480), 0);
	CHECK_INT(ws_monitor_index(true, 3439, 1919), 0);
	CHECK_INT(ws_monitor_index(true, 3440, 0), 1);
	// Above the landscape monitor is no monitor.
	CHECK_INT(ws_monitor_index(true, 0, 0), -1);
	CHECK_INT(ws_monitor_index(false, 0, 480), -1);
}

TEST(monitor_of_point_stacked) {
	fx_reset();
	fx_monitor(TOP);
	fx_monitor(BOTTOM);
	CHECK_INT(ws_monitor_index(true, 0, 1079), 0);
	CHECK_INT(ws_monitor_index(true, 0, 1080), 1);
	CHECK_INT(ws_monitor_index(true, 1920, 0), -1);
}

TEST(monitor_of_point_one_monitor) {
	fx_reset();
	fx_monitor(TOP);
	CHECK_INT(ws_monitor_index(true, 0, 0), 0);
	CHECK_INT(ws_monitor_index(true, 1919, 1079), 0);
	CHECK_INT(ws_monitor_index(true, 1920, 1079), -1);
}

TEST(monitor_of_point_no_monitors) {
	fx_reset();
	CHECK_INT(ws_monitor_index(true, 0, 0), -1);
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
	CHECK_INT(ws_desktop_monitor(one), left);
	CHECK_INT(ws_desktop_monitor(two), right);
	int32_t x = 0, y = 0;
	CHECK(ws_desktop_viewport(two, &x, &y));
	CHECK_INT(x, 3440);
	CHECK_INT(y, 0);
	// A desktop without a viewport is on all monitors.
	CHECK(!ws_desktop_viewport(2, &x, &y));
	CHECK_INT(ws_desktop_monitor(2), -1);
	// So is every desktop under a window manager that sets no _NET_DESKTOP_VIEWPORT.
	wsd.n_viewports = 0;
	CHECK_INT(ws_desktop_monitor(one), -1);
}

TEST(monitor_find_desktop_by_name) {
	fx_reset();
	auto m = fx_monitor(LANDSCAPE);
	fx_workspace("1", m);
	fx_workspace("2:web", m);
	CHECK_INT(ws_find_desktop("2:web"), 1);
	CHECK_INT(ws_find_desktop("2"), -1);
}
