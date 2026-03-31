/*
 * test_reconnect.c — unit tests for the reconnection engine
 *
 * Phase 5.1 tests: reconnection heap push/pop ordering and peek.
 * Phase 5.2 tests: backoff algorithm (added when backoff is implemented).
 * Phase 5.3 tests: reconnect_advance, max_attempts, event firing.
 *
 * See TESTING.md §3.4 for the full test catalogue.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../include/braid.h"
#include "../src/braid_internal.h"
#include "../src/braid_reconnect.h"
#include "test_harness.h"

/* ── heap helpers ────────────────────────────────────────────────────── */
static braid_reconnect_heap_t *
make_heap(uint32_t cap)
{
	braid_reconnect_heap_t *heap;

	heap = calloc(1, sizeof(*heap));
	if (heap == NULL)
		return NULL;
	if (reconnect_heap_init(heap, cap) != BRAID_OK) {
		free(heap);
		return NULL;
	}
	return heap;
}

static void
free_heap(braid_reconnect_heap_t *heap)
{
	reconnect_heap_destroy(heap);
	free(heap);
}

static braid_reconnect_entry_t
make_entry(uint64_t next_retry_ms, uint32_t attempt)
{
	braid_reconnect_entry_t e;

	e.next_retry_ms = next_retry_ms;
	e.attempt = attempt;
	return e;
}

/* ── backoff helpers ─────────────────────────────────────────────────── */

/*
 * Allocate a minimal braid_pool_t with PRNG seeded and backoff config set.
 * No sub-structures are initialised — the pool is only suitable for
 * calling reconnect_backoff_delay(). Caller must free() the returned pointer.
 */
static braid_pool_t *
make_pool_for_backoff(uint64_t seed, uint32_t backoff_base,
		      uint32_t backoff_cap)
{
	braid_pool_t *pool;

	pool = calloc(1, sizeof(*pool));
	if (pool == NULL)
		return NULL;
	pool->prng = seed;
	pool->config.backoff_base = backoff_base;
	pool->config.backoff_cap = backoff_cap;
	return pool;
}

/* ── test cases ──────────────────────────────────────────────────────── */

/*
 * reconnect_heap_init must reject cap=0 to avoid zero-size allocation.
 */
static void
test_heap_init_rejects_zero_cap(void)
{
	braid_reconnect_heap_t heap;

	memset(&heap, 0, sizeof(heap));
	CHECK_ERR("init-zero: cap=0 rejected", reconnect_heap_init(&heap, 0),
		  BRAID_ERR_INVAL);
}

/*
 * Push three entries with next_retry_ms values 300, 100, 200 (unsorted).
 * Pop must return them in ascending order: 100, 200, 300.
 * Popping an empty heap must return BRAID_ERR_EXHAUSTED.
 */
static void
test_heap_push_pop_ordering(void)
{
	braid_reconnect_heap_t *heap;
	braid_reconnect_entry_t out;

	heap = make_heap(8);
	if (heap == NULL) {
		CHECK("heap_push_pop_ordering: alloc", 0);
		return;
	}

	CHECK_ERR("push 300ms attempt=2",
		  reconnect_heap_push(heap, make_entry(300, 2)), BRAID_OK);
	CHECK_ERR("push 100ms attempt=0",
		  reconnect_heap_push(heap, make_entry(100, 0)), BRAID_OK);
	CHECK_ERR("push 200ms attempt=1",
		  reconnect_heap_push(heap, make_entry(200, 1)), BRAID_OK);

	CHECK_ERR("pop first ok", reconnect_heap_pop(heap, &out), BRAID_OK);
	CHECK("pop first: next_retry_ms == 100", out.next_retry_ms == 100);
	CHECK("pop first: attempt == 0", out.attempt == 0);

	CHECK_ERR("pop second ok", reconnect_heap_pop(heap, &out), BRAID_OK);
	CHECK("pop second: next_retry_ms == 200", out.next_retry_ms == 200);
	CHECK("pop second: attempt == 1", out.attempt == 1);

	CHECK_ERR("pop third ok", reconnect_heap_pop(heap, &out), BRAID_OK);
	CHECK("pop third: next_retry_ms == 300", out.next_retry_ms == 300);
	CHECK("pop third: attempt == 2", out.attempt == 2);

	CHECK_ERR("pop empty returns EXHAUSTED", reconnect_heap_pop(heap, &out),
		  BRAID_ERR_EXHAUSTED);

	free_heap(heap);
}

/*
 * Push three entries. Peek must return the minimum without consuming it —
 * count remains unchanged. The subsequent pop must match the peek result.
 * Peeking an empty heap must return BRAID_ERR_EXHAUSTED.
 */
static void
test_heap_peek_returns_minimum(void)
{
	braid_reconnect_heap_t *heap;
	braid_reconnect_entry_t peek_out, pop_out;

	heap = make_heap(8);
	if (heap == NULL) {
		CHECK("heap_peek_returns_minimum: alloc", 0);
		return;
	}

	CHECK_ERR("peek empty returns EXHAUSTED",
		  reconnect_heap_peek(heap, &peek_out), BRAID_ERR_EXHAUSTED);

	CHECK_ERR("push 500ms", reconnect_heap_push(heap, make_entry(500, 0)),
		  BRAID_OK);
	CHECK_ERR("push 200ms", reconnect_heap_push(heap, make_entry(200, 1)),
		  BRAID_OK);
	CHECK_ERR("push 800ms", reconnect_heap_push(heap, make_entry(800, 2)),
		  BRAID_OK);

	CHECK_ERR("peek ok", reconnect_heap_peek(heap, &peek_out), BRAID_OK);
	CHECK("peek returns minimum (200ms)", peek_out.next_retry_ms == 200);
	CHECK("heap count unchanged after peek", heap->count == 3);

	CHECK_ERR("pop ok", reconnect_heap_pop(heap, &pop_out), BRAID_OK);
	CHECK("pop matches peek value",
	      pop_out.next_retry_ms == peek_out.next_retry_ms);
	CHECK("pop matches peek attempt", pop_out.attempt == peek_out.attempt);

	free_heap(heap);
}

/*
 * Push until capacity is exhausted; the next push must return
 * BRAID_ERR_EXHAUSTED.
 */
static void
test_heap_push_full_returns_error(void)
{
	braid_reconnect_heap_t *heap;
	uint32_t i;

	heap = make_heap(4);
	if (heap == NULL) {
		CHECK("heap_push_full: alloc", 0);
		return;
	}

	for (i = 0; i < 4; i++)
		CHECK_ERR("push within capacity",
			  reconnect_heap_push(heap, make_entry((uint64_t)i, i)),
			  BRAID_OK);

	CHECK_ERR("push beyond capacity returns EXHAUSTED",
		  reconnect_heap_push(heap, make_entry(99, 99)),
		  BRAID_ERR_EXHAUSTED);

	free_heap(heap);
}

/*
 * reconnect_heap_clear must reset count to zero, allowing subsequent
 * pushes without reallocation.
 */
static void
test_heap_clear_resets_count(void)
{
	braid_reconnect_heap_t *heap;
	braid_reconnect_entry_t out;
	uint32_t i;

	heap = make_heap(4);
	if (heap == NULL) {
		CHECK("heap_clear: alloc", 0);
		return;
	}

	for (i = 0; i < 4; i++)
		reconnect_heap_push(heap,
				    make_entry((uint64_t)(i + 1) * 100, i));

	CHECK("count == 4 before clear", heap->count == 4);
	reconnect_heap_clear(heap);
	CHECK("count == 0 after clear", heap->count == 0);

	CHECK_ERR("peek on cleared heap returns EXHAUSTED",
		  reconnect_heap_peek(heap, &out), BRAID_ERR_EXHAUSTED);

	/* Can push again after clear without hitting capacity */
	CHECK_ERR("push after clear ok",
		  reconnect_heap_push(heap, make_entry(50, 0)), BRAID_OK);
	CHECK("count == 1 after push-post-clear", heap->count == 1);

	free_heap(heap);
}

/* ── test suite entry point ──────────────────────────────────────────── */

/*
 * attempt=0 → exp=0, window = backoff_base × 1 = 100.
 * All 1000 sampled delays must be in [0, 100].
 */
static void
test_backoff_attempt_0(void)
{
	braid_pool_t *pool;
	uint64_t delay;
	int all_ok;
	uint32_t i;

	pool = make_pool_for_backoff(0x1234567890abcdefULL, 100, 30000);
	if (pool == NULL) {
		CHECK("backoff_attempt_0: alloc", 0);
		return;
	}
	all_ok = 1;
	for (i = 0; i < 1000; i++) {
		delay = reconnect_backoff_delay(pool, 0);
		if (delay > 100) {
			all_ok = 0;
			break;
		}
	}
	CHECK("backoff_attempt_0: all delays in [0, 100]", all_ok);
	free(pool);
}

/*
 * attempt=5 → exp=5, window = min(30000, 100 × 32) = 3200.
 * All 1000 sampled delays must be in [0, 3200].
 */
static void
test_backoff_attempt_5(void)
{
	braid_pool_t *pool;
	uint64_t delay;
	int all_ok;
	uint32_t i;

	pool = make_pool_for_backoff(0xdeadbeefcafe0000ULL, 100, 30000);
	if (pool == NULL) {
		CHECK("backoff_attempt_5: alloc", 0);
		return;
	}
	all_ok = 1;
	for (i = 0; i < 1000; i++) {
		delay = reconnect_backoff_delay(pool, 5);
		if (delay > 3200) {
			all_ok = 0;
			break;
		}
	}
	CHECK("backoff_attempt_5: all delays in [0, 3200]", all_ok);
	free(pool);
}

/*
 * attempt=31 → exp capped at 31; window = 100 × 2^31 = 214748364800,
 * then capped to backoff_cap (30000). UBSan verifies no overflow.
 * All 1000 sampled delays must be in [0, 30000].
 */
static void
test_backoff_attempt_31(void)
{
	braid_pool_t *pool;
	uint64_t delay;
	int all_ok;
	uint32_t i;

	pool = make_pool_for_backoff(0xabcdef1234567890ULL, 100, 30000);
	if (pool == NULL) {
		CHECK("backoff_attempt_31: alloc", 0);
		return;
	}
	all_ok = 1;
	for (i = 0; i < 1000; i++) {
		delay = reconnect_backoff_delay(pool, 31);
		if (delay > 30000) {
			all_ok = 0;
			break;
		}
	}
	CHECK("backoff_attempt_31: all delays in [0, 30000]", all_ok);
	free(pool);
}

/*
 * attempt=64 → exponent clamped to 31 (overflow guard).
 * With identical PRNG seed, attempt=64 must produce the same first delay
 * as attempt=31 — proving that both hit the same window=30000 path.
 * Without the clamp, (uint64_t)base << 64 would be undefined behaviour.
 */
static void
test_backoff_attempt_64(void)
{
	braid_pool_t *pool31, *pool64;
	uint64_t delay31, delay64;

	pool31 = make_pool_for_backoff(0xfeedfacedeadbeefULL, 100, 30000);
	pool64 = make_pool_for_backoff(0xfeedfacedeadbeefULL, 100, 30000);
	if (pool31 == NULL || pool64 == NULL) {
		CHECK("backoff_attempt_64: alloc", 0);
		free(pool31);
		free(pool64);
		return;
	}
	delay31 = reconnect_backoff_delay(pool31, 31);
	delay64 = reconnect_backoff_delay(pool64, 64);
	CHECK("backoff_attempt_64: delay matches attempt 31",
	      delay31 == delay64);
	CHECK("backoff_attempt_64: delay <= 30000", delay64 <= 30000);
	free(pool31);
	free(pool64);
}

/*
 * backoff_cap is always an upper bound regardless of attempt.
 * At attempt=50 the uncapped window would be astronomically large;
 * cap enforcement must ensure all 1000 sampled delays are <= 30000.
 */
static void
test_backoff_cap_respected(void)
{
	braid_pool_t *pool;
	uint64_t delay;
	int all_ok;
	uint32_t i;

	pool = make_pool_for_backoff(0xcafebabe12345678ULL, 100, 30000);
	if (pool == NULL) {
		CHECK("backoff_cap_respected: alloc", 0);
		return;
	}
	all_ok = 1;
	for (i = 0; i < 1000; i++) {
		delay = reconnect_backoff_delay(pool, 50);
		if (delay > 30000) {
			all_ok = 0;
			break;
		}
	}
	CHECK("backoff_cap_respected: all delays <= backoff_cap", all_ok);
	free(pool);
}

/* ── test suite entry point ──────────────────────────────────────────── */

void
run_reconnect_tests(void)
{
	test_heap_init_rejects_zero_cap();
	test_heap_push_pop_ordering();
	test_heap_peek_returns_minimum();
	test_heap_push_full_returns_error();
	test_heap_clear_resets_count();
	test_backoff_attempt_0();
	test_backoff_attempt_5();
	test_backoff_attempt_31();
	test_backoff_attempt_64();
	test_backoff_cap_respected();
}
