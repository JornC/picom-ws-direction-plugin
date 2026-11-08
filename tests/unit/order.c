// SPDX-License-Identifier: MPL-2.0
// Copyright (c) JornC

// Where a workspace is among those of its monitor: index, shown and faces, in the order
// of the desktops.

#include "fixture.h"

TEST(order_index_of_mixed_names) {
	fx_reset();
	auto m = fx_monitor(LANDSCAPE);
	const char *names[] = {"10", "mail", "2", "1:web", "chat"};
	for (unsigned d = 0; d < 5; d++) {
		fx_workspace(names[d], m);
	}
	for (unsigned d = 0; d < 5; d++) {
		fx_window_on(0x101 + d, d);
	}
	fx_start((unsigned[]){2}, 1, 2);
	for (unsigned d = 0; d < 5; d++) {
		auto v = fx_compute(0x101 + d, "size");
		CHECK_INT(v.index, d);
		CHECK_INT(v.shown, 2);
		CHECK_INT(v.faces, 5);
		CHECK_INT(v.face, (int)d - 2);
	}
}

TEST(order_index_two_monitors) {
	fx_reset();
	int left = fx_monitor(LANDSCAPE), right = fx_monitor(PORTRAIT);
	fx_workspace("1", left);
	fx_workspace("2", right);
	fx_workspace("3", right);
	fx_workspace("4", left);
	fx_workspace("5", right);
	for (unsigned d = 0; d < 5; d++) {
		fx_window_on(0x101 + d, d);
	}
	fx_start((unsigned[]){0, 1}, 2, 0);
	unsigned want[] = {0, 0, 1, 1, 2};
	for (unsigned d = 0; d < 5; d++) {
		CHECK_INT(fx_compute(0x101 + d, "size").index, want[d]);
	}
	CHECK_INT(fx_compute(0x104, "size").faces, 2);
	CHECK_INT(fx_compute(0x105, "size").faces, 3);
}

TEST(order_face_from_the_workspace_before_the_switch) {
	fx_reset();
	auto m = fx_monitor(LANDSCAPE);
	const char *names[] = {"1", "2", "3", "4", "5"};
	for (unsigned d = 0; d < 5; d++) {
		fx_workspace(names[d], m);
	}
	for (unsigned d = 0; d < 5; d++) {
		fx_window_on(0x101 + d, d);
	}
	fx_start((unsigned[]){1}, 1, 1);
	fx_pass();
	fx_switch(4);
	ws_start_passes();
	// The switch is from 2: so 2 is face 0, and 5 is face 3.
	CHECK_INT(fx_compute(0x105, "ws-pass").face, 3);
	CHECK_INT(fx_compute(0x103, "ws-pass").face, 1);
	CHECK_INT(fx_compute(0x102, "ws-pass").face, 0);
	CHECK_INT(fx_compute(0x101, "ws-pass").face, -1);
	// Outside the switch, from the shown workspace, 5.
	CHECK_INT(fx_compute(0x102, "size").face, -3);
}
