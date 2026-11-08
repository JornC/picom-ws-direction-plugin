// SPDX-License-Identifier: MPL-2.0
// Copyright (c) JornC

// Switches as the root window tells them: what each monitor showed, how long a switch
// counts as recent, how many switches we remember, and on which windows a switch starts
// ws-pass.

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

/// One monitor with workspaces 1 to 9, each with one window, 0x101 to 0x109.
static void nine_workspaces(void) {
	fx_reset();
	auto m = fx_monitor(LANDSCAPE);
	const char *names[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9"};
	for (unsigned d = 0; d < 9; d++) {
		fx_workspace(names[d], m);
		fx_window_on(0x101 + d, d);
	}
}

TEST(switch_first_read_is_no_switch) {
	two_monitors();
	fx_switch(0);
	CHECK_INT(wsd.n_tracked, 1);
	auto m = fx_tracked(0);
	CHECK_INT(m->n_visits, 1);
	CHECK_STR(m->visits[0].name, "1");
	CHECK_INT(ws_unstarted_switch(m), 0);
	CHECK_STR(test_log, "Workspace \"1\" is shown (from the root window)");
}

TEST(switch_from_the_root_window) {
	two_monitors();
	fx_switch(0);
	fx_pass(FRAME_US);
	fx_switch(2);
	auto m = fx_tracked(0);
	CHECK_INT(m->n_visits, 2);
	CHECK_STR(m->visits[1].name, "3");
	CHECK(m->visits[1].from_root);
	CHECK_INT(ws_unstarted_switch(m), 1);
	CHECK_HAS(test_log, "\"1\" -> \"3\", direction 1, distance 2");
	CHECK_HAS(test_log, "(from the root window");
}

TEST(switch_recent_for_100ms) {
	two_monitors();
	fx_switch(0);
	fx_pass(FRAME_US);
	fx_switch(1);
	auto m = fx_tracked(0);
	fx_pass(50000);
	CHECK_INT(ws_unstarted_switch(m), 1);
	// 110 ms, but only 2 passes later.
	fx_pass(60000);
	CHECK_INT(ws_unstarted_switch(m), 1);
	fx_pass(10000);
	CHECK_INT(ws_unstarted_switch(m), 0);
}

TEST(switch_recent_for_2_slow_passes) {
	two_monitors();
	fx_switch(0);
	fx_pass(FRAME_US);
	fx_switch(1);
	auto m = fx_tracked(0);
	fx_pass(200000);
	CHECK_INT(ws_unstarted_switch(m), 1);
	fx_pass(60000);
	CHECK_INT(ws_unstarted_switch(m), 0);
}

TEST(switch_root_change_keeps_it_recent) {
	two_monitors();
	fx_switch(0);
	fx_pass(FRAME_US);
	fx_switch(1);
	auto m = fx_tracked(0);
	fx_pass(40000);
	fx_pass(40000);
	// The window manager is still updating the root window for this switch.
	ws_track_switch(true);
	fx_pass(40000);
	fx_pass(40000);
	CHECK_INT(ws_unstarted_switch(m), 1);
	fx_pass(40000);
	fx_pass(40000);
	ws_track_switch(false);
	fx_pass(40000);
	CHECK_INT(ws_unstarted_switch(m), 0);
}

TEST(switch_needs_a_current_desktop) {
	two_monitors();
	wsd.current = -1;
	ws_track_switch(true);
	CHECK_INT(wsd.n_tracked, 0);
	wsd.current = 6;
	ws_track_switch(true);
	CHECK_INT(wsd.n_tracked, 0);
}

TEST(switch_each_monitor_its_own) {
	two_monitors();
	fx_switch(0);
	fx_pass(FRAME_US);
	// The focus goes to the other monitor, which shows 5: no switch.
	fx_switch(4);
	CHECK_INT(wsd.n_tracked, 2);
	CHECK_INT(fx_tracked(4)->n_visits, 1);
	fx_pass(FRAME_US);
	fx_switch(5);
	CHECK_INT(fx_tracked(4)->n_visits, 2);
	CHECK_HAS(test_log, "\"5\" -> \"6\", direction 1, distance 1");
	fx_pass(FRAME_US);
	// Back to the first monitor, which still shows 1.
	fx_switch(0);
	CHECK_INT(fx_tracked(0)->n_visits, 1);
	CHECK_INT(fx_tracked(4)->n_visits, 2);
}

TEST(switch_history_keeps_the_last_8) {
	nine_workspaces();
	fx_switch(0);
	for (unsigned i = 1; i <= 20; i++) {
		fx_pass(FRAME_US);
		fx_switch(i % 9);
	}
	auto m = fx_tracked(0);
	CHECK_INT(m->n_visits, WS_HISTORY);
	const char *want[] = {"5", "6", "7", "8", "9", "1", "2", "3"};
	for (unsigned k = 0; k < sizeof(want) / sizeof(want[0]); k++) {
		CHECK_STR(m->visits[k].name, want[k]);
		CHECK(m->visits[k].from_root);
	}
	CHECK_INT(ws_unstarted_switch(m), WS_HISTORY - 1);
	// From 9 to 1 goes back past all the others.
	auto wrap = ws_steps(m->visits[4].order, m->visits[5].order, 0);
	CHECK_INT(wrap.direction, -1);
	CHECK_INT(wrap.distance, 8);
}

TEST(switch_out_of_the_history_gets_no_direction) {
	nine_workspaces();
	fx_switch(0);
	for (unsigned d = 1; d < 9; d++) {
		fx_pass(FRAME_US);
		fx_switch(d);
	}
	auto m = fx_tracked(0);
	CHECK_STR(m->visits[0].name, "2");
	// The window of 1 hides late: its switch is forgotten.
	auto late = fx_compute(0x101, "hide");
	CHECK_INT(late.direction, 0);
	CHECK_INT(late.distance, 0);
	// The window of 8 hides in time, with the switch from 8 to 9.
	auto in_time = fx_compute(0x108, "hide");
	CHECK_INT(in_time.direction, 1);
	CHECK_INT(in_time.distance, 1);
}

TEST(passes_start_on_the_monitor_of_the_switch) {
	two_monitors();
	fx_window(0x200, -1, LANDSCAPE);
	fx_window_on(0x300, 2)->moved = true;
	fx_switch(0);
	fx_pass(FRAME_US);
	fx_switch(2);
	ws_start_passes();
	// The windows of 1 to 4. Not those of 5 and 6, on the other monitor, not the
	// window on all desktops, and not the one that moved to 3 with the switch.
	CHECK_INT(test_n_starts, 4);
	for (unsigned i = 0; i < 4; i++) {
		CHECK_INT(test_starts[i].window, 0x101 + i);
		CHECK_STR(test_starts[i].trigger, "ws-pass");
	}
	CHECK_HAS(test_log, "Switch \"1\" -> \"3\" has 4 windows, started ws-pass on 4");
	// Once per switch.
	fx_pass(FRAME_US);
	ws_start_passes();
	CHECK_INT(test_n_starts, 4);
}

TEST(passes_none_for_an_old_switch) {
	two_monitors();
	fx_switch(0);
	fx_pass(FRAME_US);
	fx_switch(1);
	fx_pass(300000);
	ws_start_passes();
	CHECK_INT(test_n_starts, 0);
}

TEST(passes_old_workspace_on_another_monitor) {
	two_monitors();
	fx_switch(0);
	fx_pass(FRAME_US);
	fx_switch(1);
	// In the same pass, workspace 1 moved to the portrait monitor. Its window still
	// takes part, by the name of the workspace.
	wsd.viewports[0] = 3440;
	wsd.viewports[1] = 0;
	ws_start_passes();
	CHECK_INT(test_n_starts, 4);
	CHECK_INT(test_starts[0].window, 0x101);
}
