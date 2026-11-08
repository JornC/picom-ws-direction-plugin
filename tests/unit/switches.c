// SPDX-License-Identifier: MPL-2.0
// Copyright (c) JornC

// Switches as the window manager makes them: which workspace each monitor shows, which
// switch each show and hide is part of, how many switches we remember, and on which
// windows a switch starts ws-pass.

#include "fixture.h"

/// Two monitors: 1 to 4 on the landscape one, 5 and 6 on the portrait one, each with one
/// window, 0x101 to 0x106. The landscape one shows 1, the portrait one 5, and 1 is the
/// current desktop.
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

/// One monitor with workspaces 1 to 9, each with one window, 0x101 to 0x109. It shows 1.
static void nine_workspaces(void) {
	fx_reset();
	auto m = fx_monitor(LANDSCAPE);
	const char *names[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9"};
	for (unsigned d = 0; d < 9; d++) {
		fx_workspace(names[d], m);
	}
	for (unsigned d = 0; d < 9; d++) {
		fx_window_on(0x101 + d, d);
	}
	fx_start((unsigned[]){0}, 1, 0);
}

TEST(switch_first_read_is_no_switch) {
	two_monitors();
	CHECK_INT(wsd.n_tracked, 2);
	CHECK_STR(fx_tracked(0)->shown, "1");
	CHECK_STR(fx_tracked(4)->shown, "5");
	CHECK_INT(fx_tracked(0)->n_switches, 0);
	CHECK_INT(fx_tracked(4)->n_switches, 0);
}

TEST(switch_from_the_root_window) {
	two_monitors();
	fx_pass();
	fx_switch(2);
	auto m = fx_tracked(0);
	CHECK_INT(m->n_switches, 1);
	CHECK_STR(m->switches[0].from, "1");
	CHECK_STR(m->switches[0].to, "3");
	CHECK_STR(m->shown, "3");
	CHECK_HAS(test_log, "\"1\" -> \"3\", direction 1, distance 2");
	CHECK_HAS(test_log, "(from the root window");
}

TEST(switch_show_and_hide_belong_to_it) {
	two_monitors();
	fx_switch(2);
	auto id = fx_last_switch(0);
	CHECK(id != 0);
	CHECK_INT(fx_client(0x103)->last_show, id);
	CHECK_INT(fx_client(0x101)->last_hide, id);
	CHECK_INT(fx_client(0x102)->last_show, 0);
}

TEST(switch_late_parts_still_belong_to_it) {
	// The root window says so first. The window manager maps and unmaps the windows
	// many passes and seconds later: no clock, so they are still part of it.
	two_monitors();
	fx_current(1);
	fx_read();
	auto id = fx_last_switch(0);
	CHECK(id != 0);
	for (unsigned i = 0; i < 300; i++) {
		fx_pass();
	}
	fx_map(0x102);
	fx_read();
	fx_pass();
	fx_unmap(0x101);
	fx_read();
	CHECK_INT(fx_tracked(0)->n_switches, 1);
	CHECK_INT(fx_client(0x102)->last_show, id);
	CHECK_INT(fx_client(0x101)->last_hide, id);
}

TEST(switch_new_window_right_after_is_not_part_of_it) {
	two_monitors();
	fx_switch(1);
	fx_window_on(0x200, 1);
	fx_map(0x200);
	fx_read();
	CHECK_INT(fx_client(0x200)->last_show, 0);
	CHECK_INT(fx_tracked(0)->n_switches, 1);
}

TEST(switch_new_window_in_the_same_read_is_not_part_of_it) {
	// It opens while the window manager makes the switch, in the same read.
	two_monitors();
	fx_window_on(0x200, 1);
	// It maps with the windows of 2.
	fx_switch_events(1);
	fx_read();
	CHECK_INT(fx_client(0x102)->last_show, fx_last_switch(0));
	CHECK_INT(fx_client(0x200)->last_show, 0);
}

TEST(switch_sibling_of_a_fullscreen_window_is_not_part_of_it) {
	two_monitors();
	fx_window_on(0x201, 1);
	fx_read();
	fx_switch(1);
	auto id = fx_last_switch(0);
	CHECK_INT(fx_client(0x201)->last_show, id);
	// Fullscreen on and off, long after.
	fx_unmap(0x201);
	fx_read();
	CHECK_INT(fx_client(0x201)->last_hide, 0);
	fx_map(0x201);
	fx_read();
	CHECK_INT(fx_client(0x201)->last_show, 0);
}

TEST(switch_window_moved_along_has_no_show_or_hide) {
	two_monitors();
	// i3: move container to workspace 2; workspace 2. The window stays mapped.
	auto c = fx_client(0x101);
	free(c->workspace);
	c->workspace = strdup("2");
	c->desktop = 1;
	fx_switch(1);
	c = fx_client(0x101);
	CHECK(c->viewable);
	CHECK_INT(c->along, fx_last_switch(0));
	CHECK_INT(c->pending_hide, 0);
	CHECK_INT(c->pending_show, 0);
}

TEST(switch_by_a_map_before_the_root_window_says) {
	// A window manager that maps the new windows first.
	two_monitors();
	fx_map(0x102);
	fx_read();
	auto m = fx_tracked(0);
	CHECK_INT(m->n_switches, 1);
	CHECK_STR(m->shown, "2");
	CHECK_HAS(test_log, "\"1\" -> \"2\", direction 1, distance 1");
	CHECK_HAS(test_log, "(from a map of window 0x00000102");
	CHECK_INT(fx_client(0x102)->last_show, m->switches[0].id);
	// Then the root window says so too, and the old window hides: the same switch.
	fx_current(1);
	fx_read();
	fx_unmap(0x101);
	fx_read();
	CHECK_INT(m->n_switches, 1);
	CHECK_INT(fx_client(0x101)->last_hide, m->switches[0].id);
}

TEST(switch_unmap_before_the_switch_is_part_of_it) {
	// A window manager that unmaps the old windows before anything else.
	two_monitors();
	fx_unmap(0x101);
	fx_current(1);
	fx_map(0x102);
	fx_read();
	auto id = fx_last_switch(0);
	CHECK(id != 0);
	CHECK_INT(fx_client(0x101)->last_hide, id);
	CHECK_INT(fx_client(0x102)->last_show, id);
}

TEST(switch_on_a_monitor_without_the_focus) {
	// One i3 command switches the portrait monitor and goes back to the landscape
	// one: the current desktop ends where it was, the maps tell.
	two_monitors();
	fx_current(5);
	fx_map(0x106);
	fx_unmap(0x105);
	fx_current(0);
	fx_read();
	auto m = fx_tracked(4);
	CHECK_INT(m->n_switches, 1);
	CHECK_STR(m->shown, "6");
	CHECK_INT(fx_tracked(0)->n_switches, 0);
	CHECK_INT(fx_client(0x106)->last_show, m->switches[0].id);
	CHECK_INT(fx_client(0x105)->last_hide, m->switches[0].id);
	ws_start_passes();
	CHECK_INT(test_n_starts, 2);
	// Then the focus moves there: no switch.
	test_n_starts = 0;
	fx_pass();
	fx_current(5);
	fx_read();
	CHECK_INT(m->n_switches, 1);
	ws_start_passes();
	CHECK_INT(test_n_starts, 0);
}

TEST(switch_focus_to_the_other_monitor_is_none) {
	two_monitors();
	fx_current(4);
	fx_read();
	CHECK_INT(fx_tracked(4)->n_switches, 0);
	CHECK_INT(fx_tracked(0)->n_switches, 0);
	fx_switch(5);
	CHECK_INT(fx_tracked(4)->n_switches, 1);
	CHECK_HAS(test_log, "\"5\" -> \"6\", direction 1, distance 1");
	fx_current(0);
	fx_read();
	CHECK_INT(fx_tracked(0)->n_switches, 0);
}

TEST(switch_fast_with_one_read) {
	// 1 -> 2 -> 4, all before one read: the map of 2's window says 2 was shown.
	two_monitors();
	fx_switch_events(1);
	fx_switch_events(3);
	fx_read();
	auto m = fx_tracked(0);
	CHECK_INT(m->n_switches, 2);
	CHECK_STR(m->switches[0].to, "2");
	CHECK_STR(m->switches[1].from, "2");
	CHECK_STR(m->switches[1].to, "4");
	auto two = fx_client(0x102);
	CHECK_INT(two->last_show, m->switches[0].id);
	CHECK_INT(two->last_hide, m->switches[1].id);
	CHECK_INT(fx_client(0x101)->last_hide, m->switches[0].id);
	CHECK_INT(fx_client(0x104)->last_show, m->switches[1].id);
	// ws-pass starts once, for 1 -> 4.
	ws_start_passes();
	CHECK_HAS(test_log, "Switch \"1\" -> \"4\" has 4 windows");
}

TEST(switch_chained_before_anything_showed) {
	// i3 makes "workspace 2; workspace 4" one move: the current desktop changes
	// twice, then it shows 4 and hides 1. A read between the two changes sees a
	// switch to 2 that nothing shows: the hides and shows are part of 1 -> 4.
	two_monitors();
	fx_current(1);
	fx_read();
	fx_current(3);
	fx_map(0x104);
	fx_unmap(0x101);
	fx_read();
	auto m = fx_tracked(0);
	CHECK_INT(m->n_switches, 2);
	CHECK_INT(m->switches[1].start, m->switches[0].id);
	auto hide = fx_compute(0x101, "hide");
	CHECK_INT(hide.direction, 1);
	CHECK_INT(hide.distance, 3);
	CHECK_INT(hide.face, 0);
	auto show = fx_compute(0x104, "show");
	CHECK_INT(show.direction, 1);
	CHECK_INT(show.distance, 3);
	CHECK_INT(show.face, 3);
}

TEST(switch_not_chained_after_something_showed) {
	two_monitors();
	fx_switch(1);
	fx_switch(3);
	auto m = fx_tracked(0);
	CHECK_INT(m->switches[1].start, m->switches[1].id);
	auto show = fx_compute(0x104, "show");
	CHECK_INT(show.distance, 2);
	CHECK_INT(show.face, 2);
}

TEST(switch_fast_back_where_it_came_from) {
	two_monitors();
	fx_switch_events(1);
	fx_switch_events(0);
	fx_read();
	CHECK_INT(fx_tracked(0)->n_switches, 2);
	ws_start_passes();
	CHECK_INT(test_n_starts, 0);
	CHECK_HAS(test_log, "went back where they came from");
}

TEST(switch_to_an_empty_workspace) {
	// i3 makes workspace 7 for the switch; it has no windows. Back on 1, i3 removes
	// it.
	two_monitors();
	auto left = ws_desktop_monitor(0);
	wsd.names[6] = "7";
	wsd.viewports[12] = (uint32_t)wsd.monitors[left].rect.x1;
	wsd.viewports[13] = (uint32_t)wsd.monitors[left].rect.y1;
	wsd.n_names = 7;
	wsd.n_viewports = 14;
	fx_switch(6);
	auto m = fx_tracked(0);
	CHECK_STR(m->shown, "7");
	CHECK_INT(m->switches[0].move.direction, 1);
	CHECK_INT(m->switches[0].move.distance, 4);
	CHECK_INT(fx_client(0x101)->last_hide, m->switches[0].id);
	fx_current(0);
	fx_map(0x101);
	wsd.n_names = 6;
	wsd.n_viewports = 12;
	fx_read();
	CHECK_INT(m->n_switches, 2);
	// 7 is gone, but the switch from it counts it where it was.
	CHECK_INT(m->switches[1].move.direction, -1);
	CHECK_INT(m->switches[1].move.distance, 4);
	CHECK_INT(fx_client(0x101)->last_show, m->switches[1].id);
}

TEST(switch_empty_workspace_is_the_shown_one) {
	// No current desktop on this monitor, no window on screen there: the one
	// workspace without windows is shown, as i3 removes the hidden empty ones.
	two_monitors();
	auto right = ws_desktop_monitor(4);
	wsd.names[6] = "7";
	wsd.viewports[12] = (uint32_t)wsd.monitors[right].rect.x1;
	wsd.viewports[13] = (uint32_t)wsd.monitors[right].rect.y1;
	wsd.n_names = 7;
	wsd.n_viewports = 14;
	fx_unmap(0x105);
	fx_read();
	CHECK_STR(fx_tracked(4)->shown, "7");
	CHECK_INT(fx_client(0x105)->last_hide, fx_last_switch(4));
}

TEST(switch_halfway_is_not_guessed) {
	// The old windows are unmapped, the new ones not mapped yet, and the root window
	// says nothing: we can't tell, so the monitor keeps showing 5.
	two_monitors();
	fx_unmap(0x105);
	fx_read();
	auto m = fx_tracked(4);
	CHECK_STR(m->shown, "5");
	CHECK_INT(m->n_switches, 0);
	fx_map(0x106);
	fx_read();
	CHECK_STR(m->shown, "6");
	CHECK_INT(m->n_switches, 1);
}

TEST(switch_shown_workspace_gone_is_logged) {
	// Workspace 5 is gone from the names, and its window is on no workspace now. The
	// portrait monitor shows no window, and 6 has one: we can't tell what it shows.
	two_monitors();
	fx_unmap(0x105);
	wsd.names[4] = "6";
	wsd.n_names = 5;
	wsd.n_viewports = 10;
	auto five = fx_client(0x105);
	free(five->workspace);
	five->workspace = NULL;
	five->desktop = -1;
	fx_client(0x106)->desktop = 4;
	fx_read();
	CHECK_HAS(test_log, "\"5\", which it showed, is gone");
	fx_compute(0x106, "size");
	CHECK_HAS(test_log, "we don't know which of its workspaces it shows");
}

TEST(switch_without_a_current_desktop) {
	// The windows on screen tell.
	fx_reset();
	auto m = fx_monitor(LANDSCAPE);
	fx_workspace("1", m);
	fx_workspace("2", m);
	fx_window_on(0x101, 0)->viewable = true;
	fx_window_on(0x102, 1);
	fx_read();
	CHECK_STR(fx_tracked(0)->shown, "1");
	fx_map(0x102);
	fx_unmap(0x101);
	fx_read();
	CHECK_STR(fx_tracked(0)->shown, "2");
	CHECK_INT(fx_tracked(0)->n_switches, 1);
}

TEST(switch_history_keeps_the_last_8) {
	nine_workspaces();
	for (unsigned i = 1; i <= 20; i++) {
		fx_pass();
		fx_switch(i % 9);
	}
	auto m = fx_tracked(0);
	CHECK_INT(m->n_switches, WS_HISTORY);
	const char *want[] = {"4", "5", "6", "7", "8", "9", "1", "2"};
	for (unsigned k = 0; k < sizeof(want) / sizeof(want[0]); k++) {
		CHECK_STR(m->switches[k].from, want[k]);
	}
	CHECK_STR(m->switches[WS_HISTORY - 1].to, "3");
	// From 9 to 1 goes back past all the others.
	CHECK_INT(m->switches[5].move.direction, -1);
	CHECK_INT(m->switches[5].move.distance, 8);
}

TEST(switch_out_of_the_history_gets_no_direction) {
	nine_workspaces();
	fx_switch(1);
	for (unsigned d = 2; d < 9; d++) {
		fx_pass();
		fx_switch(d);
	}
	fx_pass();
	fx_switch(0);
	auto m = fx_tracked(0);
	CHECK_STR(m->switches[0].from, "2");
	// The hide of 1's window is part of the switch to 2, which is forgotten.
	CHECK_INT(fx_compute(0x101, "hide").direction, 0);
	// The hide of 9's window is part of the switch from 9 to 1.
	auto in_time = fx_compute(0x109, "hide");
	CHECK_INT(in_time.direction, -1);
	CHECK_INT(in_time.distance, 8);
}

TEST(passes_start_on_the_monitor_of_the_switch) {
	two_monitors();
	fx_window(0x200, -1, LANDSCAPE)->viewable = true;
	fx_read();
	fx_pass();
	fx_switch(2);
	ws_start_passes();
	ws_start_passes();
	// The windows of 1 to 4. Not those of 5 and 6, on the other monitor, and not the
	// window on all desktops.
	CHECK_INT(test_n_starts, 4);
	for (unsigned i = 0; i < 4; i++) {
		CHECK_INT(test_starts[i].window, 0x101 + i);
		CHECK_STR(test_starts[i].trigger, "ws-pass");
	}
	CHECK_HAS(test_log, "Switch \"1\" -> \"3\" has 4 windows, started ws-pass on 4");
	// Once per switch.
	fx_pass();
	ws_start_passes();
	CHECK_INT(test_n_starts, 4);
}

TEST(passes_not_for_a_window_that_moved_along) {
	two_monitors();
	fx_window_on(0x300, 0)->viewable = true;
	fx_read();
	auto c = fx_client(0x300);
	free(c->workspace);
	c->workspace = strdup("3");
	c->desktop = 2;
	fx_pass();
	fx_switch(2);
	ws_start_passes();
	CHECK_INT(test_n_starts, 4);
	for (unsigned i = 0; i < test_n_starts; i++) {
		CHECK(test_starts[i].window != 0x300);
	}
}

TEST(passes_old_workspace_on_another_monitor) {
	two_monitors();
	fx_pass();
	fx_switch_events(1);
	// In the same read, workspace 1 moved to the portrait monitor. Its window still
	// takes part, by the name of the workspace.
	wsd.viewports[0] = 3440;
	wsd.viewports[1] = 0;
	ws_start_passes();
	ws_update_viewport_monitors();
	fx_read();
	ws_start_passes();
	CHECK_INT(test_n_starts, 4);
	CHECK_INT(test_starts[0].window, 0x101);
}

TEST(monitors_keep_their_switches_by_name) {
	two_monitors();
	fx_switch(1);
	CHECK_INT(fx_tracked(0)->n_switches, 1);
	// RandR lists the monitors the other way around now.
	auto a = wsd.monitors[0];
	wsd.monitors[0] = wsd.monitors[1];
	wsd.monitors[1] = a;
	fx_read();
	CHECK_INT(fx_tracked(0)->n_switches, 1);
	CHECK_INT(fx_tracked(4)->n_switches, 0);
	// The portrait monitor goes, and its workspaces move to the landscape one.
	wsd.monitors[0] = wsd.monitors[1];
	wsd.n_monitors = 1;
	for (unsigned d = 4; d < 6; d++) {
		wsd.viewports[d * 2] = 0;
		wsd.viewports[d * 2 + 1] = 480;
	}
	ws_update_viewport_monitors();
	fx_read();
	for (unsigned i = 0; i < wsd.n_tracked; i++) {
		CHECK(wsd.tracked[i].name != 2);
	}
}
