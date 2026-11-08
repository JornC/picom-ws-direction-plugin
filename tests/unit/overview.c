// SPDX-License-Identifier: MPL-2.0
// Copyright (c) JornC

// The overview: it starts ws-unfold again when the workspaces or windows of an unfolded
// monitor change, and takes the windows on all desktops along, on the shown tile.

#include "fixture.h"

/// Two monitors: 1 to 4 on the landscape one, 5 and 6 on the portrait one, each with one
/// window, 0x101 to 0x106, and 0x300 on all desktops, on the landscape one. The
/// landscape one shows 2, and is unfolded.
static struct ws_monitor *unfolded(void) {
	fx_reset();
	int left = fx_monitor(LANDSCAPE), right = fx_monitor(PORTRAIT);
	const char *names[] = {"1", "2", "3", "4", "5", "6"};
	for (unsigned d = 0; d < 6; d++) {
		fx_workspace(names[d], d < 4 ? left : right);
	}
	for (unsigned d = 0; d < 6; d++) {
		fx_window_on(0x101 + d, d);
	}
	auto sticky = fx_window(
	    0x300, -1, (struct ws_rect){.x1 = 100, .y1 = 600, .x2 = 300, .y2 = 700});
	sticky->sticky = true;
	sticky->viewable = true;
	fx_start((unsigned[]){1, 4}, 2, 1);
	auto m = fx_tracked(0);
	m->unfolded = true;
	ws_overview_refresh();
	return m;
}

TEST(overview_unfold_takes_windows_on_all_desktops) {
	unfolded();
	CHECK_INT(test_n_starts, 5);
	bool sticky = false;
	for (unsigned i = 0; i < test_n_starts; i++) {
		CHECK_STR(test_starts[i].trigger, "ws-unfold");
		sticky |= test_starts[i].window == 0x300;
	}
	CHECK(sticky);
	// It is drawn on the shown tile.
	auto v = fx_compute(0x300, "ws-unfold");
	CHECK_INT(v.index, 1);
	CHECK_INT(v.shown, 1);
	CHECK_INT(v.face, 0);
	CHECK_INT(v.faces, 4);
	CHECK_INT(v.unfolded, 1);
	CHECK_INT(v.y, 480);
	CHECK_INT(v.placed, 1);
	// Outside the overview it is on no workspace.
	auto own = fx_compute(0x300, "size");
	CHECK_INT(own.index, 0);
	CHECK_INT(own.shown, 0);
}

TEST(overview_hidden_window_on_all_desktops_not_drawn) {
	// i3 puts a window in its scratchpad on all desktops, unmapped.
	unfolded();
	fx_window(0x301, -1, (struct ws_rect){.x1 = 100, .y1 = 600, .x2 = 300, .y2 = 700})->sticky =
	    true;
	test_n_starts = 0;
	auto m = fx_tracked(0);
	free(m->unfolded_as);
	m->unfolded_as = NULL;
	ws_overview_refresh();
	CHECK_INT(test_n_starts, 5);
	for (unsigned i = 0; i < test_n_starts; i++) {
		CHECK(test_starts[i].window != 0x301);
	}
}

TEST(overview_unchanged_starts_nothing) {
	unfolded();
	test_n_starts = 0;
	fx_pass();
	fx_read();
	ws_overview_refresh();
	CHECK_INT(test_n_starts, 0);
}

TEST(overview_refresh_when_a_window_opens) {
	unfolded();
	test_n_starts = 0;
	fx_pass();
	fx_window_on(0x200, 2);
	fx_map(0x200);
	fx_read();
	ws_overview_refresh();
	CHECK_INT(test_n_starts, 6);
	CHECK_STR(test_starts[0].trigger, "ws-unfold");
	// Not for a switch.
	CHECK_INT(fx_compute(0x200, "ws-unfold").direction, 0);
	test_n_starts = 0;
	ws_overview_refresh();
	CHECK_INT(test_n_starts, 0);
}

TEST(overview_refresh_when_a_window_closes_or_moves) {
	unfolded();
	test_n_starts = 0;
	fx_close(0x103);
	fx_read();
	ws_overview_refresh();
	CHECK_INT(test_n_starts, 4);
	test_n_starts = 0;
	auto c = fx_client(0x104);
	free(c->workspace);
	c->workspace = strdup("1");
	c->desktop = 0;
	fx_read();
	ws_overview_refresh();
	CHECK_INT(test_n_starts, 4);
}

TEST(overview_refresh_not_for_the_other_monitor) {
	unfolded();
	test_n_starts = 0;
	fx_window_on(0x200, 5);
	fx_read();
	ws_overview_refresh();
	CHECK_INT(test_n_starts, 0);
}

TEST(overview_windows_on_all_desktops_not_in_ws_pass) {
	unfolded()->unfolded = false;
	test_n_starts = 0;
	fx_pass();
	fx_switch(2);
	ws_start_passes();
	CHECK_INT(test_n_starts, 4);
	for (unsigned i = 0; i < test_n_starts; i++) {
		CHECK(test_starts[i].window != 0x300);
	}
}
