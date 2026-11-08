// SPDX-License-Identifier: MPL-2.0
// Copyright (c) JornC

// The variables a window gets when it starts an animation (ws_compute), with the root
// window and the windows already read in the pass.

#include "fixture.h"

/// Two monitors: 1 to 4 on the landscape one, 5 and 6 on the portrait one, each with one
/// window, 0x101 to 0x106.
static void two_monitors(void) {
	fx_reset();
	int left = fx_monitor(LANDSCAPE), right = fx_monitor(PORTRAIT);
	const char *names[] = {"1", "2", "3", "4", "5", "6"};
	for (unsigned d = 0; d < 6; d++) {
		fx_workspace(names[d], d < 4 ? left : right);
		fx_window_on(0x101 + d, d);
	}
}

/// One monitor with workspaces 1 to 5, each with one window, 0x101 to 0x105.
static void one_monitor(void) {
	fx_reset();
	auto m = fx_monitor(LANDSCAPE);
	const char *names[] = {"1", "2", "3", "4", "5"};
	for (unsigned d = 0; d < 5; d++) {
		fx_workspace(names[d], m);
		fx_window_on(0x101 + d, d);
	}
}

/// The switch from 1 to 3 on the landscape monitor.
static void switch_1_to_3(void) {
	two_monitors();
	fx_switch(0);
	fx_pass(FRAME_US);
	fx_switch(2);
}

TEST(variables_show_in_a_switch) {
	switch_1_to_3();
	auto v = fx_compute(0x103, "show");
	CHECK_INT(v.direction, 1);
	CHECK_INT(v.distance, 2);
	CHECK_INT(v.x, 0);
	CHECK_INT(v.y, 480);
	CHECK_INT(v.width, 3440);
	CHECK_INT(v.height, 1440);
	CHECK_INT(v.faces, 4);
	CHECK_INT(v.face, 2);
	CHECK_INT(v.selected, 1);
	CHECK_INT(v.placed, 1);
}

TEST(variables_hide_in_a_switch) {
	switch_1_to_3();
	auto v = fx_compute(0x101, "hide");
	CHECK_INT(v.direction, 1);
	CHECK_INT(v.distance, 2);
	CHECK_INT(v.face, 0);
}

TEST(variables_open_is_a_show) {
	switch_1_to_3();
	auto v = fx_compute(0x103, "open");
	CHECK_INT(v.direction, 1);
	CHECK_INT(v.face, 2);
}

TEST(variables_ws_pass_on_every_workspace_of_the_monitor) {
	switch_1_to_3();
	auto passed = fx_compute(0x102, "ws-pass");
	CHECK_INT(passed.direction, 1);
	CHECK_INT(passed.distance, 2);
	CHECK_INT(passed.face, 1);
	auto beyond = fx_compute(0x104, "ws-pass");
	CHECK_INT(beyond.direction, 1);
	CHECK_INT(beyond.face, 3);
	// The other monitor has no switch.
	auto other = fx_compute(0x105, "ws-pass");
	CHECK_INT(other.direction, 0);
	CHECK_INT(other.x, 3440);
	CHECK_INT(other.y, 0);
	CHECK_INT(other.width, 1080);
	CHECK_INT(other.height, 1920);
	CHECK_INT(other.faces, 2);
	CHECK_INT(other.selected, 0);
}

TEST(variables_switch_back) {
	two_monitors();
	fx_switch(2);
	fx_pass(FRAME_US);
	fx_switch(0);
	auto show = fx_compute(0x101, "show");
	CHECK_INT(show.direction, -1);
	CHECK_INT(show.distance, 2);
	CHECK_INT(show.face, -2);
	auto hide = fx_compute(0x103, "hide");
	CHECK_INT(hide.direction, -1);
	CHECK_INT(hide.face, 0);
}

TEST(variables_no_switch_after_its_lifetime) {
	two_monitors();
	fx_switch(0);
	fx_pass(FRAME_US);
	fx_switch(1);
	fx_pass(50000);
	fx_pass(50000);
	fx_pass(50000);
	auto v = fx_compute(0x102, "show");
	CHECK_INT(v.direction, 0);
	CHECK_INT(v.distance, 0);
	CHECK_INT(v.face, 0);
}

TEST(variables_own_change_is_never_a_switch) {
	switch_1_to_3();
	const char *triggers[] = {"size", "position", "color", "increase-opacity",
	                          "decrease-opacity"};
	for (unsigned i = 0; i < sizeof(triggers) / sizeof(triggers[0]); i++) {
		auto v = fx_compute(0x103, triggers[i]);
		CHECK_INT(v.direction, 0);
		CHECK_INT(v.distance, 0);
		CHECK_INT(v.face, 0);
	}
	// Its face counts from the workspace shown now.
	CHECK_INT(fx_compute(0x101, "size").face, -2);
}

TEST(variables_close_gets_the_whole_screen) {
	switch_1_to_3();
	auto v = fx_compute(0x101, "close");
	CHECK_INT(v.direction, 0);
	CHECK_INT(v.x, 0);
	CHECK_INT(v.y, 0);
	CHECK_INT(v.width, 4520);
	CHECK_INT(v.height, 1920);
	CHECK_INT(v.faces, 0);
	CHECK_INT(v.face, 0);
	CHECK_INT(v.placed, 1);
}

TEST(variables_window_on_all_desktops) {
	switch_1_to_3();
	fx_window(0x200, -1, (struct ws_rect){.x1 = 3500, .y1 = 100, .x2 = 3600, .y2 = 200});
	fx_window(0x201, -1, (struct ws_rect){.x1 = 3000, .y1 = 500, .x2 = 3600, .y2 = 600});
	auto inside = fx_compute(0x200, "show");
	CHECK_INT(inside.direction, 0);
	CHECK_INT(inside.x, 3440);
	CHECK_INT(inside.faces, 2);
	CHECK_INT(inside.face, 0);
	CHECK_INT(inside.placed, 1);
	auto across = fx_compute(0x201, "show");
	CHECK_INT(across.x, 0);
	CHECK_INT(across.width, 4520);
	CHECK_INT(across.faces, 0);
}

TEST(variables_window_not_on_its_monitor) {
	// A window moved to workspace 6 while it was hidden keeps its place on the other
	// monitor until 6 shows.
	two_monitors();
	fx_switch(0);
	fx_window(0x200, 5, (struct ws_rect){.x1 = 100, .y1 = 600, .x2 = 300, .y2 = 700});
	auto v = fx_compute(0x200, "size");
	CHECK_INT(v.placed, 0);
	CHECK_INT(v.x, 3440);
}

TEST(variables_window_shows_before_the_root_window_says) {
	two_monitors();
	fx_switch(0);
	fx_pass(FRAME_US);
	auto show = fx_compute(0x102, "show");
	CHECK_INT(show.direction, 1);
	CHECK_INT(show.distance, 1);
	CHECK_INT(show.face, 1);
	auto m = fx_tracked(0);
	CHECK_INT(m->n_visits, 2);
	CHECK(!m->visits[1].from_root);
	CHECK_HAS(test_log, "\"1\" -> \"2\", direction 1, distance 1");
	CHECK_HAS(test_log, "(from window 0x00000102");
	// Then the root window says so too: the same switch.
	fx_switch(1);
	CHECK_INT(m->n_visits, 2);
	CHECK_INT(ws_unstarted_switch(m), 1);
	CHECK_INT(fx_compute(0x101, "hide").direction, 1);
}

TEST(variables_root_window_skipped_a_switch) {
	// Switching fast, from 1 to 2 to 3: we read the root window after both.
	switch_1_to_3();
	auto two = fx_compute(0x102, "show");
	auto m = fx_tracked(0);
	CHECK_INT(m->n_visits, 3);
	CHECK_STR(m->visits[1].name, "2");
	CHECK_INT(two.direction, 1);
	CHECK_INT(two.distance, 1);
	CHECK_INT(two.face, 1);
	auto three = fx_compute(0x103, "show");
	CHECK_INT(three.direction, 1);
	CHECK_INT(three.distance, 1);
	CHECK_INT(three.face, 1);
}

TEST(variables_many_switches_in_a_row) {
	// 1 to 2 to 3 to 4, a pass apart. The windows show and hide late, and each one
	// gets the switch it is part of.
	two_monitors();
	fx_switch(0);
	for (unsigned d = 1; d < 4; d++) {
		fx_pass(FRAME_US);
		fx_switch(d);
	}
	auto hide1 = fx_compute(0x101, "hide");
	CHECK_INT(hide1.direction, 1);
	CHECK_INT(hide1.distance, 1);
	CHECK_INT(hide1.face, 0);
	auto hide2 = fx_compute(0x102, "hide");
	CHECK_INT(hide2.direction, 1);
	CHECK_INT(hide2.face, 0);
	auto show3 = fx_compute(0x103, "show");
	CHECK_INT(show3.direction, 1);
	CHECK_INT(show3.face, 1);
	auto show4 = fx_compute(0x104, "show");
	CHECK_INT(show4.direction, 1);
	CHECK_INT(show4.distance, 1);
	CHECK_INT(show4.face, 1);
}

TEST(variables_stacked_monitors) {
	fx_reset();
	int top = fx_monitor(TOP), bottom = fx_monitor(BOTTOM);
	const char *names[] = {"1", "2", "3", "4", "5"};
	for (unsigned d = 0; d < 5; d++) {
		fx_workspace(names[d], d < 2 ? top : bottom);
		fx_window_on(0x101 + d, d);
	}
	fx_switch(2);
	fx_pass(FRAME_US);
	fx_switch(4);
	auto v = fx_compute(0x105, "show");
	CHECK_INT(v.direction, 1);
	CHECK_INT(v.distance, 2);
	CHECK_INT(v.x, 0);
	CHECK_INT(v.y, 1080);
	CHECK_INT(v.width, 1920);
	CHECK_INT(v.height, 1080);
	CHECK_INT(v.faces, 3);
	CHECK_INT(v.face, 2);
	CHECK_INT(v.selected, 1);
	auto up = fx_compute(0x101, "size");
	CHECK_INT(up.y, 0);
	CHECK_INT(up.faces, 2);
	CHECK_INT(up.selected, 0);
}

TEST(variables_one_monitor_wrap_around) {
	one_monitor();
	fx_switch(4);
	fx_pass(FRAME_US);
	fx_switch(0);
	auto show = fx_compute(0x101, "show");
	CHECK_INT(show.direction, -1);
	CHECK_INT(show.distance, 4);
	CHECK_INT(show.face, -4);
	CHECK_INT(show.faces, 5);
	auto hide = fx_compute(0x105, "hide");
	CHECK_INT(hide.direction, -1);
	CHECK_INT(hide.face, 0);
}

TEST(variables_without_viewports) {
	// A window manager that sets no _NET_DESKTOP_VIEWPORT: one monitor, the screen.
	one_monitor();
	wsd.n_viewports = 0;
	fx_switch(0);
	fx_pass(FRAME_US);
	fx_switch(2);
	auto v = fx_compute(0x103, "show");
	CHECK_INT(v.direction, 1);
	CHECK_INT(v.distance, 2);
	CHECK_INT(v.x, 0);
	CHECK_INT(v.y, 0);
	CHECK_INT(v.width, 4520);
	CHECK_INT(v.height, 1920);
	CHECK_INT(v.faces, 5);
	CHECK_INT(v.face, 2);
	CHECK_INT(v.selected, 1);
	CHECK_INT(v.placed, 1);
}
