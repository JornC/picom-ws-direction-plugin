// SPDX-License-Identifier: MPL-2.0
// Copyright (c) JornC

// The steps of a switch on a monitor: its direction and distance, in the list of the
// monitor's workspaces; and that list.

#include "fixture.h"

/// One monitor with workspaces 1 to 5. Returns its list.
static unsigned one_monitor(const char *list[static WS_MAX_DESKTOPS]) {
	fx_reset();
	auto m = fx_monitor(LANDSCAPE);
	const char *names[] = {"1", "2", "3", "4", "5"};
	for (unsigned d = 0; d < 5; d++) {
		fx_workspace(names[d], m);
	}
	return ws_monitor_list(-1, list);
}

TEST(steps_one_monitor) {
	const char *list[WS_MAX_DESKTOPS];
	auto n = one_monitor(list);
	CHECK_INT(n, 5);
	auto up = ws_steps("1", "4", list, n);
	CHECK_INT(up.direction, 1);
	CHECK_INT(up.distance, 3);
	auto down = ws_steps("4", "1", list, n);
	CHECK_INT(down.direction, -1);
	CHECK_INT(down.distance, 3);
	auto next = ws_steps("2", "3", list, n);
	CHECK_INT(next.direction, 1);
	CHECK_INT(next.distance, 1);
	auto none = ws_steps("3", "3", list, n);
	CHECK_INT(none.direction, 0);
	CHECK_INT(none.distance, 0);
}

TEST(steps_wrap_around_goes_back) {
	// i3's next_on_output goes from the last workspace to the first. That is a switch
	// back past all the others.
	const char *list[WS_MAX_DESKTOPS];
	auto n = one_monitor(list);
	auto wrap = ws_steps("5", "1", list, n);
	CHECK_INT(wrap.direction, -1);
	CHECK_INT(wrap.distance, 4);
}

TEST(steps_not_in_the_list) {
	const char *list[WS_MAX_DESKTOPS];
	auto n = one_monitor(list);
	auto gone = ws_steps("1", "9", list, n);
	CHECK_INT(gone.direction, 0);
	CHECK_INT(gone.distance, 0);
	CHECK_INT(ws_steps(NULL, "1", list, n).direction, 0);
}

TEST(steps_two_monitors_count_their_own) {
	fx_reset();
	int left = fx_monitor(LANDSCAPE), right = fx_monitor(PORTRAIT);
	fx_workspace("1", left);
	fx_workspace("2", left);
	fx_workspace("3", right);
	fx_workspace("4", left);
	fx_workspace("5", right);
	const char *l[WS_MAX_DESKTOPS], *r[WS_MAX_DESKTOPS];
	unsigned n_l = ws_monitor_list(left, l), n_r = ws_monitor_list(right, r);
	CHECK_INT(n_l, 3);
	CHECK_INT(n_r, 2);
	auto a = ws_steps("1", "4", l, n_l);
	CHECK_INT(a.direction, 1);
	CHECK_INT(a.distance, 2);
	auto b = ws_steps("3", "5", r, n_r);
	CHECK_INT(b.direction, 1);
	CHECK_INT(b.distance, 1);
}

TEST(steps_stacked_monitors) {
	fx_reset();
	int top = fx_monitor(TOP), bottom = fx_monitor(BOTTOM);
	fx_workspace("1", top);
	fx_workspace("2", bottom);
	fx_workspace("3", bottom);
	fx_workspace("4", top);
	fx_workspace("5", bottom);
	const char *b[WS_MAX_DESKTOPS], *t[WS_MAX_DESKTOPS];
	unsigned n_b = ws_monitor_list(bottom, b), n_t = ws_monitor_list(top, t);
	auto down = ws_steps("5", "2", b, n_b);
	CHECK_INT(down.direction, -1);
	CHECK_INT(down.distance, 2);
	CHECK_INT(ws_steps("1", "4", t, n_t).distance, 1);
}

TEST(steps_follow_the_desktop_order) {
	// The order is that of the desktops, not of the numbers in the names: under i3
	// they are the same, and other window managers number as they like.
	fx_reset();
	auto m = fx_monitor(LANDSCAPE);
	const char *names[] = {"10", "mail", "2", "1:web", "chat"};
	for (unsigned d = 0; d < 5; d++) {
		fx_workspace(names[d], m);
	}
	const char *list[WS_MAX_DESKTOPS];
	auto n = ws_monitor_list(-1, list);
	for (unsigned d = 0; d < 5; d++) {
		CHECK_STR(list[d], names[d]);
	}
	auto a = ws_steps("10", "2", list, n);
	CHECK_INT(a.direction, 1);
	CHECK_INT(a.distance, 2);
}

TEST(steps_switch_from_a_workspace_that_is_gone) {
	// DPT shows the empty 3, then switches to 5; i3 removes 3 in the same read. The
	// switch counts 3 where it was in the read before.
	fx_reset();
	auto m = fx_monitor(LANDSCAPE);
	fx_monitor(PORTRAIT);
	const char *names[] = {"1", "2", "3", "5", "6"};
	for (unsigned d = 0; d < 5; d++) {
		fx_workspace(names[d], d < 4 ? m : 1);
	}
	fx_window_on(0x101, 0);
	fx_window_on(0x105, 3);
	fx_window_on(0x106, 4);
	fx_start((unsigned[]){2, 4}, 2, 2);
	CHECK_STR(fx_tracked(0)->shown, "3");
	fx_current(2);
	fx_map(0x105);
	wsd.names[2] = "5";
	wsd.names[3] = "6";
	wsd.viewports[6] = 3440;
	wsd.viewports[7] = 0;
	wsd.n_names = 4;
	wsd.n_viewports = 8;
	auto c = fx_client(0x105);
	c->desktop = 2;
	c = fx_client(0x106);
	c->desktop = 3;
	ws_update_viewport_monitors();
	fx_read();
	auto t = fx_tracked(0);
	CHECK_INT(t->n_switches, 1);
	CHECK_INT(t->switches[0].move.direction, 1);
	CHECK_INT(t->switches[0].move.distance, 1);
	CHECK_INT(t->switches[0].n_list, 4);
	CHECK_STR(t->switches[0].list[2], "3");
	auto v = fx_compute(0x101, "size");
	CHECK_INT(v.faces, 3);
	CHECK_INT(v.index, 0);
	CHECK_INT(v.shown, 2);
}
