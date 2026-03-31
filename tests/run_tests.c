/*
 * run_tests.c — libbraid test binary entry point
 *
 * Calls all registered test suite functions in order.
 * Prints summary line and exits 0 if all pass, 1 if any fail.
 *
 * Test suites are registered by adding a forward declaration and a call
 * below as each phase's tests are implemented.
 *
 * See TECH_STACK.md §6.2.
 */

#include <stdio.h>
#include <stdlib.h>

#include "test_harness.h"

/* Pass/fail counters — declared extern in test_harness.h. */
int tests_passed = 0;
int tests_failed = 0;

/*
 * Mock clock state — defined here so it is linked into the test binary.
 * Only active when compiled with -DBRAID_TEST_CLOCK.
 */
#ifdef BRAID_TEST_CLOCK
#include <stdint.h>
uint64_t braid_test_clock_ms = 0;
#endif

/*
 * Forward declarations for test suite entry points.
 * Uncomment each as the corresponding phase is implemented.
 */
void run_table_tests(void); /* Phase 2 */
void run_state_machine_tests(void); /* Phase 3 */
void run_wait_queue_tests(void); /* Phase 4 */
/* void run_reconnect_tests(void);    */ /* Phase 5 */
/* void run_reaper_tests(void);       */ /* Phase 5 */
/* void run_pool_tests(void);         */ /* Phase 6 */
/* void run_integration_tests(void);  */ /* Phase 8 */

int
main(void)
{
	run_table_tests(); /* Phase 2 */
	run_state_machine_tests(); /* Phase 3 */
	run_wait_queue_tests(); /* Phase 4 */

	printf("%d/%d tests passed\n", tests_passed,
	       tests_passed + tests_failed);

	return (tests_failed > 0) ? 1 : 0;
}
