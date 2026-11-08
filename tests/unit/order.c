// SPDX-License-Identifier: MPL-2.0
// Copyright (c) JornC

// Where a workspace sorts: workspaces whose name starts with a number sort by it, the
// others after them, by their position in _NET_DESKTOP_NAMES.

#include "fixture.h"

TEST(order_numbers_sort_by_value) {
	struct ws_order two = ws_order_of("2", 0), ten = ws_order_of("10", 1);
	CHECK_INT(ws_order_compare(two, ten), -1);
	CHECK_INT(ws_order_compare(ten, two), 1);
}

TEST(order_number_at_the_start_of_the_name) {
	auto web = ws_order_of("1:web", 5);
	CHECK(web.numbered);
	CHECK_INT(web.number, 1);
	CHECK_INT(web.position, 5);
	CHECK_INT(ws_order_of("10: mail", 0).number, 10);
	CHECK_INT(ws_order_of("007", 0).number, 7);
}

TEST(order_unnumbered_after_numbered) {
	struct ws_order mail = ws_order_of("mail", 0), last = ws_order_of("99", 3);
	CHECK(!mail.numbered);
	CHECK_INT(ws_order_compare(mail, last), 1);
	CHECK_INT(ws_order_compare(last, mail), -1);
}

TEST(order_unnumbered_by_position) {
	CHECK_INT(ws_order_compare(ws_order_of("web", 4), ws_order_of("chat", 2)), 1);
	CHECK_INT(ws_order_compare(ws_order_of("chat", 2), ws_order_of("web", 4)), -1);
}

TEST(order_same_number_by_position) {
	CHECK_INT(ws_order_compare(ws_order_of("1:b", 1), ws_order_of("1:a", 3)), -1);
}

TEST(order_same_workspace) {
	auto three = ws_order_of("3", 2);
	CHECK_INT(ws_order_compare(three, three), 0);
}

TEST(order_names_that_are_not_numbers) {
	CHECK(!ws_order_of("", 0).numbered);
	CHECK(!ws_order_of(" 1", 0).numbered);
	CHECK(!ws_order_of("-1", 0).numbered);
	CHECK(!ws_order_of("x1", 0).numbered);
}

TEST(order_huge_number_stops_at_uint32_max) {
	auto huge = ws_order_of("123456789012345678901234567890", 0);
	CHECK(huge.numbered);
	CHECK_INT(huge.number, UINT32_MAX);
	// The same number then, so the position decides.
	CHECK_INT(ws_order_compare(ws_order_of("4294967295", 1), huge), 1);
	CHECK_INT(ws_order_compare(ws_order_of("4294967294", 1), huge), -1);
}
