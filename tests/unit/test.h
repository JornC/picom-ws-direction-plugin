// SPDX-License-Identifier: MPL-2.0
// Copyright (c) JornC

/// A small test runner. TEST(name) { ... } defines a test; the runner finds it by itself.
/// A CHECK that fails ends its test. One file defines TEST_MAIN before it includes this
/// header, for main(). Give the binary a word to run only the tests whose name has it.

#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct test_case {
	const char *name;
	void (*fn)(void);
	struct test_case *next;
};

extern struct test_case *test_cases;
extern const char *test_current;
extern bool test_failed;

#define TEST(name)                                                                       \
	static void name(void);                                                          \
	__attribute__((constructor)) static void name##_add(void) {                      \
		static struct test_case c = {#name, name, NULL};                         \
		c.next = test_cases;                                                     \
		test_cases = &c;                                                         \
	}                                                                                \
	static void name(void)

#define TEST_FAIL(...)                                                                   \
	do {                                                                             \
		printf("FAIL  %s\n      %s:%d: ", test_current, __FILE__, __LINE__);     \
		printf(__VA_ARGS__);                                                     \
		putchar('\n');                                                           \
		test_failed = true;                                                      \
		return;                                                                  \
	} while (0)

#define TEST_STR(s) ((s) != NULL ? (s) : "(null)")

#define CHECK(cond)                                                                      \
	do {                                                                             \
		if (!(cond)) {                                                           \
			TEST_FAIL("%s", #cond);                                          \
		}                                                                        \
	} while (0)

#define CHECK_INT(a, b)                                                                  \
	do {                                                                             \
		long long a_ = (long long)(a), b_ = (long long)(b);                      \
		if (a_ != b_) {                                                          \
			TEST_FAIL("%s is %lld, not %lld", #a, a_, b_);                   \
		}                                                                        \
	} while (0)

#define CHECK_STR(a, b)                                                                  \
	do {                                                                             \
		const char *a_ = (a), *b_ = (b);                                         \
		if (a_ == NULL || strcmp(a_, b_) != 0) {                                 \
			TEST_FAIL("%s is \"%s\", not \"%s\"", #a, TEST_STR(a_), b_);     \
		}                                                                        \
	} while (0)

/// Checks that string `a` has `b` in it.
#define CHECK_HAS(a, b)                                                                  \
	do {                                                                             \
		const char *a_ = (a), *b_ = (b);                                         \
		if (a_ == NULL || strstr(a_, b_) == NULL) {                              \
			TEST_FAIL("%s is \"%s\", without \"%s\"", #a, TEST_STR(a_), b_); \
		}                                                                        \
	} while (0)

#ifdef TEST_MAIN
struct test_case *test_cases;
const char *test_current;
bool test_failed;

int main(int argc, char **argv) {
	// The constructors added the tests last first.
	struct test_case *list = NULL;
	while (test_cases != NULL) {
		auto next = test_cases->next;
		test_cases->next = list;
		list = test_cases;
		test_cases = next;
	}
	unsigned run = 0, failed = 0;
	for (auto t = list; t != NULL; t = t->next) {
		if (argc > 1 && strstr(t->name, argv[1]) == NULL) {
			continue;
		}
		test_current = t->name;
		test_failed = false;
		t->fn();
		run++;
		failed += test_failed;
	}
	printf("%u of %u tests passed\n", run - failed, run);
	return failed > 0 || run == 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
#endif
