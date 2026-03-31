/*
 * test_harness.h — minimal test harness for libbraid
 *
 * Provides CHECK() and CHECK_ERR() macros, pass/fail counters,
 * and a summary printer. No external test framework required.
 *
 * Usage:
 *   CHECK("description", expression_that_is_nonzero_on_success);
 *   CHECK_ERR("description", function_call(), EXPECTED_RETURN_CODE);
 *
 * See TECH_STACK.md §6.1.
 */

#ifndef TEST_HARNESS_H
#define TEST_HARNESS_H

#include <stdio.h>

/* Counters — defined in run_tests.c; declared extern here. */
extern int tests_passed;
extern int tests_failed;

/*
 * CHECK(name, expr) — assert expr is true (non-zero).
 * Increments tests_passed on success, tests_failed on failure.
 */
#define CHECK(name, expr)                                                      \
	do {                                                                   \
		int _r = (expr) ? 0 : 1;                                       \
		if (_r == 0) {                                                 \
			tests_passed++;                                        \
		} else {                                                       \
			fprintf(stderr, "FAIL: %s\n", (name));                 \
			tests_failed++;                                        \
		}                                                              \
	} while (0)

/*
 * CHECK_ERR(name, call, expected) — assert call returns expected code.
 * Prints actual vs expected on failure.
 */
#define CHECK_ERR(name, call, expected)                                        \
	do {                                                                   \
		int _rc = (call);                                              \
		if (_rc == (expected)) {                                       \
			tests_passed++;                                        \
		} else {                                                       \
			fprintf(stderr, "FAIL: %s — expected %d, got %d\n",    \
				(name), (expected), _rc);                      \
			tests_failed++;                                        \
		}                                                              \
	} while (0)

#endif /* TEST_HARNESS_H */
