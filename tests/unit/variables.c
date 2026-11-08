// SPDX-License-Identifier: MPL-2.0
// Copyright (c) JornC

// The variables a window gets when it starts an animation (ws_compute), with the root
// window and the windows already read in the pass.

#include "fixture.h"

/// Two monitors: 1 to 4 on the landscape one, 5 and 6 on the portrait one, each with one
/// window, 0x101 to 0x106. The landscape one shows 1, the portrait one 5.
static void two_monitors(void) {
	fx_reset();
	int left = fx_monitor(LANDSCAPE), right = fx_monitor(PORTRAIT);
	const char *names[] = {"1", "2", "3", "4", "5", "6"};
	for (unsigned d = 0; d < 6; d++) {
		fx_workspace(names[d], d < 4 ? left : right);
	}
	for (unsigned d = 0; d < 6; d++) {
		fx_window_on(0x101 + d, d);
	}
	fx_start((unsigned[]){0, 4}, 2, 0);
}

/// One monitor with workspaces 1 to 5, each with one window, 0x101 to 0x105, showing
/// `shown`. The viewports are all the same, so the monitor is the whole screen.
static void one_monitor(unsigned shown) {
	fx_reset();
	auto m = fx_monitor(LANDSCAPE);
	const char *names[] = {"1", "2", "3", "4", "5"};
	for (unsigned d = 0; d < 5; d++) {
		fx_workspace(names[d], m);
	}
	for (unsigned d = 0; d < 5; d++) {
		fx_window_on(0x101 + d, d);
	}
	fx_start((unsigned[]){shown}, 1, shown);
}

/// The switch from 1 to 3 on the landscape monitor, and ws-pass started for it.
static void switch_1_to_3(void) {
	two_monitors();
	fx_pass();
	fx_switch(2);
	ws_start_passes();
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
	CHECK_INT(v.index, 2);
	CHECK_INT(v.shown, 2);
	CHECK_INT(v.unfolded, 0);
}

TEST(variables_hide_in_a_switch) {
	switch_1_to_3();
	auto v = fx_compute(0x101, "hide");
	CHECK_INT(v.direction, 1);
	CHECK_INT(v.distance, 2);
	CHECK_INT(v.face, 0);
	CHECK_INT(v.index, 0);
	CHECK_INT(v.shown, 2);
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
	CHECK_INT(passed.index, 1);
	auto beyond = fx_compute(0x104, "ws-pass");
	CHECK_INT(beyond.direction, 1);
	CHECK_INT(beyond.face, 3);
	CHECK_INT(beyond.index, 3);
	// The other monitor has no switch, and the plugin started nothing there.
	auto other = fx_compute(0x105, "ws-pass");
	CHECK_INT(other.direction, 0);
	CHECK_INT(other.x, 3440);
	CHECK_INT(other.y, 0);
	CHECK_INT(other.width, 1080);
	CHECK_INT(other.height, 1920);
	CHECK_INT(other.faces, 2);
	CHECK_INT(other.selected, 0);
}

TEST(variables_ws_pass_only_in_its_pass) {
	// Why the plugin started an animation holds for the pass it started it in.
	switch_1_to_3();
	fx_pass();
	CHECK_INT(fx_compute(0x102, "ws-pass").direction, 0);
}

TEST(variables_custom_trigger_not_started_by_the_plugin) {
	// Started over D-Bus right after a switch: not part of it.
	switch_1_to_3();
	auto v = fx_compute(0x102, "wave");
	CHECK_INT(v.direction, 0);
	CHECK_INT(v.distance, 0);
	CHECK_INT(v.face, -1);
	CHECK_INT(v.index, 1);
	CHECK_INT(v.shown, 2);
}

TEST(variables_switch_back) {
	two_monitors();
	fx_switch(2);
	fx_pass();
	fx_switch(0);
	auto show = fx_compute(0x101, "show");
	CHECK_INT(show.direction, -1);
	CHECK_INT(show.distance, 2);
	CHECK_INT(show.face, -2);
	auto hide = fx_compute(0x103, "hide");
	CHECK_INT(hide.direction, -1);
	CHECK_INT(hide.face, 0);
}

TEST(variables_late_show_is_part_of_its_switch) {
	two_monitors();
	fx_current(1);
	fx_read();
	for (unsigned i = 0; i < 100; i++) {
		fx_pass();
	}
	fx_map(0x102);
	fx_read();
	auto v = fx_compute(0x102, "show");
	CHECK_INT(v.direction, 1);
	CHECK_INT(v.distance, 1);
	CHECK_INT(v.face, 1);
}

TEST(variables_new_window_right_after_a_switch) {
	two_monitors();
	fx_switch(1);
	fx_pass();
	fx_window_on(0x200, 1);
	fx_map(0x200);
	fx_read();
	auto v = fx_compute(0x200, "open");
	CHECK_INT(v.direction, 0);
	CHECK_INT(v.distance, 0);
	CHECK_INT(v.face, 0);
	CHECK_INT(v.index, 1);
	CHECK_INT(v.shown, 1);
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

TEST(variables_close_gets_the_monitor_it_was_on) {
	// The read in the pass picom starts the close no longer finds the window; the
	// read before had it on 3.
	switch_1_to_3();
	fx_pass();
	fx_unmap(0x103);
	fx_close(0x103);
	fx_read();
	auto v = fx_compute(0x103, "close");
	CHECK_INT(v.direction, 0);
	CHECK_INT(v.x, 0);
	CHECK_INT(v.y, 480);
	CHECK_INT(v.width, 3440);
	CHECK_INT(v.height, 1440);
	CHECK_INT(v.faces, 4);
	CHECK_INT(v.face, 0);
	CHECK_INT(v.placed, 1);
	CHECK_INT(v.index, 2);
	CHECK_INT(v.shown, 2);
}

TEST(variables_hide_of_a_window_that_closes) {
	// i3 unmaps the frame of a window that closes before it destroys it: picom fires
	// hide, after a read that no longer finds the window.
	switch_1_to_3();
	fx_pass();
	fx_unmap(0x103);
	fx_close(0x103);
	fx_read();
	auto v = fx_compute(0x103, "hide");
	CHECK_INT(v.direction, 0);
	CHECK_INT(v.y, 480);
	CHECK_INT(v.width, 3440);
	CHECK_INT(v.index, 2);
}

TEST(variables_close_before_a_read_saw_it_go) {
	switch_1_to_3();
	auto v = fx_compute(0x103, "close");
	CHECK_INT(v.direction, 0);
	CHECK_INT(v.y, 480);
	CHECK_INT(v.width, 3440);
	CHECK_INT(v.index, 2);
}

TEST(variables_close_of_a_window_no_read_had) {
	switch_1_to_3();
	auto v = fx_compute(0x999, "close");
	CHECK_INT(v.direction, 0);
	CHECK_INT(v.x, 0);
	CHECK_INT(v.y, 0);
	CHECK_INT(v.width, 4520);
	CHECK_INT(v.height, 1920);
	CHECK_INT(v.faces, 0);
	CHECK_INT(v.index, 0);
}

TEST(variables_frame_whose_client_picom_does_not_know) {
	// picom gives the frame and no client window: the plugin finds the client by its
	// frame.
	two_monitors();
	fx_client(0x102)->frame = 0x902;
	fx_read();
	fx_pass();
	fx_switch(1);
	auto v = fx_compute_frame(0x902, "show");
	CHECK_INT(v.direction, 1);
	CHECK_INT(v.face, 1);
	CHECK_INT(v.index, 1);
	CHECK_INT(v.y, 480);
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
	CHECK_INT(inside.index, 0);
	auto across = fx_compute(0x201, "show");
	CHECK_INT(across.x, 0);
	CHECK_INT(across.width, 4520);
	CHECK_INT(across.faces, 0);
}

TEST(variables_window_not_on_its_monitor) {
	// A window moved to workspace 6 while it was hidden keeps its place on the other
	// monitor until 6 shows.
	two_monitors();
	fx_window(0x200, 5, (struct ws_rect){.x1 = 100, .y1 = 600, .x2 = 300, .y2 = 700});
	auto v = fx_compute(0x200, "size");
	CHECK_INT(v.placed, 0);
	CHECK_INT(v.x, 3440);
	CHECK_INT(v.index, 1);
}

TEST(variables_unfolded) {
	two_monitors();
	fx_tracked(0)->unfolded = true;
	CHECK_INT(fx_compute(0x102, "size").unfolded, 1);
	CHECK_INT(fx_compute(0x105, "size").unfolded, 0);
}

TEST(variables_unfold_right_after_a_switch) {
	// The overview opens right after the switch from 1 to 3. The plugin started
	// ws-unfold for the overview, not for the switch: no direction. A style goes on
	// from the switch's pose by carrying its variables.
	switch_1_to_3();
	fx_pass();
	auto m = fx_tracked(0);
	m->unfolded = true;
	ws_start(0x102, WS_UNFOLD_TRIGGER, m, 0, 0);
	auto v = fx_compute(0x102, "ws-unfold");
	CHECK_INT(v.direction, 0);
	CHECK_INT(v.face, -1);
	CHECK_INT(v.index, 1);
	CHECK_INT(v.shown, 2);
	CHECK_INT(v.unfolded, 1);
}

TEST(variables_unfold_for_a_switch_while_unfolded) {
	two_monitors();
	fx_tracked(0)->unfolded = true;
	fx_pass();
	fx_switch(2);
	ws_start_passes();
	auto v = fx_compute(0x102, "ws-unfold");
	CHECK_INT(v.direction, 1);
	CHECK_INT(v.distance, 2);
	CHECK_INT(v.face, 1);
	CHECK_INT(v.index, 1);
	CHECK_INT(v.shown, 2);
	CHECK_INT(v.unfolded, 1);
}

TEST(variables_window_shows_before_the_root_window_says) {
	two_monitors();
	fx_pass();
	fx_map(0x102);
	fx_read();
	auto show = fx_compute(0x102, "show");
	CHECK_INT(show.direction, 1);
	CHECK_INT(show.distance, 1);
	CHECK_INT(show.face, 1);
	// Then the root window says so too, and the old window hides: the same switch.
	fx_current(1);
	fx_unmap(0x101);
	fx_read();
	CHECK_INT(fx_tracked(0)->n_switches, 1);
	CHECK_INT(fx_compute(0x101, "hide").direction, 1);
}

TEST(variables_fast_switches_with_one_read) {
	// 1 to 2 to 3, read after both: the windows of 2 showed and hid in between.
	two_monitors();
	fx_switch_events(1);
	fx_switch_events(2);
	fx_read();
	auto show2 = fx_compute(0x102, "show");
	CHECK_INT(show2.direction, 1);
	CHECK_INT(show2.distance, 1);
	CHECK_INT(show2.face, 1);
	auto hide2 = fx_compute(0x102, "hide");
	CHECK_INT(hide2.direction, 1);
	CHECK_INT(hide2.distance, 1);
	CHECK_INT(hide2.face, 0);
	auto show3 = fx_compute(0x103, "show");
	CHECK_INT(show3.direction, 1);
	CHECK_INT(show3.distance, 1);
	CHECK_INT(show3.face, 1);
	// ws-pass goes from 1 to 3 at once.
	ws_start_passes();
	auto pass = fx_compute(0x103, "ws-pass");
	CHECK_INT(pass.direction, 1);
	CHECK_INT(pass.distance, 2);
	CHECK_INT(pass.face, 2);
}

TEST(variables_many_switches_in_a_row) {
	// 1 to 2 to 3 to 4, a read apart. Each window gets the switch it is part of.
	two_monitors();
	for (unsigned d = 1; d < 4; d++) {
		fx_pass();
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
	CHECK_INT(show4.index, 3);
}

TEST(variables_stacked_monitors) {
	fx_reset();
	int top = fx_monitor(TOP), bottom = fx_monitor(BOTTOM);
	const char *names[] = {"1", "2", "3", "4", "5"};
	for (unsigned d = 0; d < 5; d++) {
		fx_workspace(names[d], d < 2 ? top : bottom);
	}
	for (unsigned d = 0; d < 5; d++) {
		fx_window_on(0x101 + d, d);
	}
	fx_start((unsigned[]){0, 2}, 2, 2);
	fx_pass();
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
	CHECK_INT(v.index, 2);
	CHECK_INT(v.selected, 1);
	auto up = fx_compute(0x101, "size");
	CHECK_INT(up.y, 0);
	CHECK_INT(up.faces, 2);
	CHECK_INT(up.selected, 0);
}

TEST(variables_one_monitor_wrap_around) {
	one_monitor(4);
	fx_pass();
	fx_switch(0);
	auto show = fx_compute(0x101, "show");
	CHECK_INT(show.direction, -1);
	CHECK_INT(show.distance, 4);
	CHECK_INT(show.face, -4);
	CHECK_INT(show.faces, 5);
	CHECK_INT(show.index, 0);
	auto hide = fx_compute(0x105, "hide");
	CHECK_INT(hide.direction, -1);
	CHECK_INT(hide.face, 0);
	CHECK_INT(hide.index, 4);
	CHECK_INT(hide.shown, 0);
}

TEST(variables_equal_viewports_are_the_whole_screen) {
	// Every viewport is 0,480: they don't say which monitor a desktop is on, so each
	// one spans the whole screen.
	one_monitor(0);
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

TEST(variables_without_viewports) {
	// A window manager that sets no _NET_DESKTOP_VIEWPORT: one monitor, the screen.
	fx_reset();
	auto m = fx_monitor(LANDSCAPE);
	const char *names[] = {"1", "2", "3"};
	for (unsigned d = 0; d < 3; d++) {
		fx_workspace(names[d], m);
	}
	for (unsigned d = 0; d < 3; d++) {
		fx_window_on(0x101 + d, d);
	}
	wsd.n_viewports = 0;
	ws_update_viewport_monitors();
	fx_start((unsigned[]){0}, 1, 0);
	fx_switch(2);
	auto v = fx_compute(0x103, "show");
	CHECK_INT(v.direction, 1);
	CHECK_INT(v.distance, 2);
	CHECK_INT(v.x, 0);
	CHECK_INT(v.width, 4520);
	CHECK_INT(v.faces, 3);
}
