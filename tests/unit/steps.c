// SPDX-License-Identifier: MPL-2.0
// Copyright (c) JornC

// The steps of a switch on a monitor: its direction, the workspaces it passes and its
// distance; and where a workspace is among those of its monitor (index), and how far it
// is from the one the monitor showed (face).

#include "fixture.h"

/// One monitor with workspaces 1 to 5.
static int one_monitor(void) {
	fx_reset();
	auto m = fx_monitor(LANDSCAPE);
	const char *names[] = {"1", "2", "3", "4", "5"};
	for (unsigned d = 0; d < 5; d++) {
		fx_workspace(names[d], m);
	}
	return m;
}

TEST(steps_one_monitor) {
	auto m = one_monitor();
	auto up = ws_steps(fx_order(0), fx_order(3), m);
	CHECK_INT(up.direction, 1);
	CHECK_INT(up.distance, 3);
	auto down = ws_steps(fx_order(3), fx_order(0), m);
	CHECK_INT(down.direction, -1);
	CHECK_INT(down.distance, 3);
	auto next = ws_steps(fx_order(1), fx_order(2), m);
	CHECK_INT(next.direction, 1);
	CHECK_INT(next.distance, 1);
	auto none = ws_steps(fx_order(2), fx_order(2), m);
	CHECK_INT(none.direction, 0);
	CHECK_INT(none.distance, 0);
}

TEST(steps_wrap_around_goes_back) {
	// i3's next_on_output goes from the last workspace to the first. That is a switch
	// back past all the others.
	auto m = one_monitor();
	auto wrap = ws_steps(fx_order(4), fx_order(0), m);
	CHECK_INT(wrap.direction, -1);
	CHECK_INT(wrap.distance, 4);
}

TEST(steps_passes_only_between) {
	auto m = one_monitor();
	struct ws_order from = fx_order(0), to = fx_order(3);
	CHECK(!ws_passes(from, to, m, 0));
	CHECK(ws_passes(from, to, m, 1));
	CHECK(ws_passes(from, to, m, 2));
	CHECK(!ws_passes(from, to, m, 3));
	CHECK(!ws_passes(from, to, m, 4));
	// The same going back.
	CHECK(ws_passes(to, from, m, 1));
	CHECK(!ws_passes(to, from, m, 4));
}

TEST(steps_two_monitors_count_their_own) {
	fx_reset();
	int left = fx_monitor(LANDSCAPE), right = fx_monitor(PORTRAIT);
	fx_workspace("1", left);
	fx_workspace("2", left);
	fx_workspace("3", right);
	fx_workspace("4", left);
	fx_workspace("5", right);
	auto l = ws_steps(fx_order(0), fx_order(3), left);
	CHECK_INT(l.direction, 1);
	CHECK_INT(l.distance, 2);
	CHECK(!ws_passes(fx_order(0), fx_order(3), left, 2));
	auto r = ws_steps(fx_order(2), fx_order(4), right);
	CHECK_INT(r.direction, 1);
	CHECK_INT(r.distance, 1);
}

TEST(steps_stacked_monitors) {
	fx_reset();
	int top = fx_monitor(TOP), bottom = fx_monitor(BOTTOM);
	fx_workspace("1", top);
	fx_workspace("2", bottom);
	fx_workspace("3", bottom);
	fx_workspace("4", top);
	fx_workspace("5", bottom);
	auto b = ws_steps(fx_order(4), fx_order(1), bottom);
	CHECK_INT(b.direction, -1);
	CHECK_INT(b.distance, 2);
	auto t = ws_steps(fx_order(0), fx_order(3), top);
	CHECK_INT(t.distance, 1);
}

TEST(steps_unnumbered_workspaces) {
	fx_reset();
	auto m = fx_monitor(LANDSCAPE);
	fx_workspace("1", m);
	fx_workspace("2", m);
	fx_workspace("mail", m);
	fx_workspace("chat", m);
	auto a = ws_steps(fx_order(1), fx_order(2), m);
	CHECK_INT(a.direction, 1);
	CHECK_INT(a.distance, 1);
	auto b = ws_steps(fx_order(0), fx_order(3), m);
	CHECK_INT(b.direction, 1);
	CHECK_INT(b.distance, 3);
	auto c = ws_steps(fx_order(3), fx_order(2), m);
	CHECK_INT(c.direction, -1);
	CHECK_INT(c.distance, 1);
}

TEST(steps_count_only_workspaces_that_exist) {
	// i3 removed workspace 3 when it was hidden empty; a switch from it remembers its
	// order.
	fx_reset();
	auto m = fx_monitor(LANDSCAPE);
	fx_workspace("1", m);
	fx_workspace("2", m);
	fx_workspace("4", m);
	fx_workspace("5", m);
	auto gone = ws_order_of("3", 2);
	auto from_gone = ws_steps(gone, fx_order(3), m);
	CHECK_INT(from_gone.direction, 1);
	CHECK_INT(from_gone.distance, 2);
	auto to_gone = ws_steps(fx_order(0), gone, m);
	CHECK_INT(to_gone.direction, 1);
	CHECK_INT(to_gone.distance, 2);
}

TEST(steps_without_viewports_count_all) {
	one_monitor();
	wsd.n_viewports = 0;
	auto all = ws_steps(fx_order(0), fx_order(4), -1);
	CHECK_INT(all.direction, 1);
	CHECK_INT(all.distance, 4);
}

TEST(face_without_a_monitor_is_zero) {
	auto m = one_monitor();
	CHECK_INT(ws_face(3, m, NULL, 0), 0);
}

TEST(face_from_the_shown_workspace) {
	auto m = one_monitor();
	fx_switch(1);
	auto t = fx_tracked(1);
	CHECK(t != NULL);
	CHECK_INT(ws_face(3, m, t, 0), 2);
	CHECK_INT(ws_face(0, m, t, 0), -1);
	CHECK_INT(ws_face(1, m, t, 0), 0);
}

TEST(face_from_the_workspace_before_the_switch) {
	auto m = one_monitor();
	fx_switch(1);
	fx_pass(FRAME_US);
	fx_switch(4);
	auto t = fx_tracked(4);
	CHECK_INT(t->n_visits, 2);
	// The switch to visits[1] is from 2: so 2 is face 0, and 5 is face 3.
	CHECK_INT(ws_face(4, m, t, 1), 3);
	CHECK_INT(ws_face(2, m, t, 1), 1);
	CHECK_INT(ws_face(1, m, t, 1), 0);
	CHECK_INT(ws_face(0, m, t, 1), -1);
	// Outside the switch, from the shown workspace, 5.
	CHECK_INT(ws_face(1, m, t, 0), -3);
}
