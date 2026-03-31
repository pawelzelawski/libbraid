/*
 * test_reaper.c — unit tests for the idle reaper heap and reap logic
 *
 * Phase 5.4 tests: heap insert/remove/peek, heap_index consistency
 *                  after sift-up and sift-down.
 * Phase 5.5 tests: reaper_advance eligibility, min_connections floor,
 *                  future-entry stop, next_ms computation.
 *
 * See TESTING.md §3.5 for the full test catalogue.
 */

#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include "../include/braid.h"
#include "../src/braid_conn.h"
#include "../src/braid_internal.h"
#include "../src/braid_reaper.h"
#include "../src/braid_reconnect.h"
#include "../src/braid_table.h"
#include "test_harness.h"

/* ── standalone heap helpers (no pool needed) ────────────────────────── */

/*
 * make_idle_heap — allocate a standalone idle heap with given capacity.
 * Caller must call free_idle_heap() when done.
 */
static braid_idle_heap_t *
make_idle_heap(uint32_t cap)
{
	braid_idle_heap_t *heap;

	heap = calloc(1, sizeof(*heap));
	if (heap == NULL)
		return NULL;
	if (reaper_heap_init(heap, cap) != BRAID_OK) {
		free(heap);
		return NULL;
	}
	return heap;
}

static void
free_idle_heap(braid_idle_heap_t *heap)
{
	reaper_heap_destroy(heap);
	free(heap);
}

/*
 * make_conn — allocate a bare braid_conn_t for heap-only tests.
 * Sets last_active_ms and initialises heap_index to UINT32_MAX.
 * No fd or pool interaction. Caller must free().
 */
static braid_conn_t *
make_conn(uint64_t last_active_ms)
{
	braid_conn_t *conn;

	conn = calloc(1, sizeof(*conn));
	if (conn == NULL)
		return NULL;
	conn->last_active_ms = last_active_ms;
	conn->heap_index = UINT32_MAX;
	conn->fd = -1;
	return conn;
}

/*
 * heap_index_consistent — verify that every conn->heap_index matches
 * the entry's actual position in the heap array.
 * Returns 1 if consistent, 0 otherwise.
 */
static int
heap_index_consistent(braid_idle_heap_t *heap)
{
	uint32_t i;

	for (i = 0; i < heap->count; i++) {
		if (heap->entries[i].conn->heap_index != i)
			return 0;
	}
	return 1;
}

/* ── pool helpers (advance tests) ───────────────────────────────────── */

/*
 * make_reaper_pool — allocate a pool suitable for reaper_advance tests.
 * Initialises table, idle heap, and reconnect heap (reconnect heap is
 * required because conn_transition(→ DEAD) may push a reconnect entry
 * when live_count drops below min_connections).
 * Caller must call free_reaper_pool() when done.
 */
static braid_pool_t *
make_reaper_pool(uint32_t max, uint32_t min, uint32_t idle_reap_timeout)
{
	braid_pool_t *pool;

	pool = calloc(1, sizeof(*pool));
	if (pool == NULL)
		return NULL;

	pool->config.max_connections = max;
	pool->config.min_connections = min;
	pool->config.idle_reap_timeout = idle_reap_timeout;

	if (table_init(pool) != BRAID_OK) {
		free(pool);
		return NULL;
	}
	if (reaper_heap_init(&pool->idle, max) != BRAID_OK) {
		table_destroy(pool);
		free(pool);
		return NULL;
	}
	if (reconnect_heap_init(&pool->reconnect, max) != BRAID_OK) {
		reaper_heap_destroy(&pool->idle);
		table_destroy(pool);
		free(pool);
		return NULL;
	}
	return pool;
}

static void
free_reaper_pool(braid_pool_t *pool)
{
	reconnect_heap_destroy(&pool->reconnect);
	reaper_heap_destroy(&pool->idle);
	table_destroy(pool);
	free(pool);
}

/*
 * alloc_idle_conn — open a real fd, allocate a connection record, and
 * advance it to IDLE state with a controlled last_active_ms.
 *
 * Sets braid_test_clock_ms to last_active_ms before the IDLE transition
 * so that conn->last_active_ms and the heap entry match exactly.
 * The fd opened here is closed by conn_transition(→ DEAD) when the
 * connection is reaped or cleaned up.
 *
 * Returns a pointer to the in-table connection record, or NULL on failure.
 */
static braid_conn_t *
alloc_idle_conn(braid_pool_t *pool, uint64_t last_active_ms)
{
	braid_conn_t *conn;
	int fd;

	fd = open("/dev/null", O_RDONLY);
	if (fd < 0)
		return NULL;

	if (conn_alloc(pool, fd, &conn) != BRAID_OK) {
		close(fd);
		return NULL;
	}
	if (conn_transition(pool, conn, BRAID_STATE_INITIALIZING) != BRAID_OK)
		return NULL;

	braid_test_clock_ms = last_active_ms;
	if (conn_transition(pool, conn, BRAID_STATE_IDLE) != BRAID_OK)
		return NULL;

	return conn;
}

/* ── test cases ──────────────────────────────────────────────────────── */

/*
 * Insert three entries with last_active_ms values 300, 100, 200 (unsorted).
 * Peek must return the entry with the minimum last_active_ms (100) and
 * the correct conn pointer.
 */
static void
test_heap_insert_and_peek(void)
{
	braid_idle_heap_t *heap;
	braid_idle_entry_t entry;
	braid_conn_t *c100, *c200, *c300;

	heap = make_idle_heap(4);
	c300 = make_conn(300);
	c100 = make_conn(100);
	c200 = make_conn(200);
	if (heap == NULL || c300 == NULL || c100 == NULL || c200 == NULL) {
		CHECK("heap-insert-peek: allocation", 0);
		goto cleanup;
	}

	reaper_heap_insert(heap, c300);
	reaper_heap_insert(heap, c100);
	reaper_heap_insert(heap, c200);

	CHECK("heap-insert-peek: count is 3", heap->count == 3);
	CHECK_ERR("heap-insert-peek: peek ok", reaper_heap_peek(heap, &entry),
		  BRAID_OK);
	CHECK("heap-insert-peek: minimum is 100", entry.last_active_ms == 100);
	CHECK("heap-insert-peek: conn pointer matches", entry.conn == c100);

cleanup:
	free_idle_heap(heap);
	free(c300);
	free(c100);
	free(c200);
}

/*
 * Insert three entries in ascending order (100, 200, 300).
 * Remove the middle entry (c200).
 * Verify: count decrements, c200->heap_index == UINT32_MAX, heap minimum
 * unchanged (100), and all remaining heap_index values are consistent.
 */
static void
test_heap_remove_by_conn(void)
{
	braid_idle_heap_t *heap;
	braid_idle_entry_t entry;
	braid_conn_t *c100, *c200, *c300;

	heap = make_idle_heap(4);
	c100 = make_conn(100);
	c200 = make_conn(200);
	c300 = make_conn(300);
	if (heap == NULL || c100 == NULL || c200 == NULL || c300 == NULL) {
		CHECK("heap-remove: allocation", 0);
		goto cleanup;
	}

	reaper_heap_insert(heap, c100);
	reaper_heap_insert(heap, c200);
	reaper_heap_insert(heap, c300);

	CHECK_ERR("heap-remove: remove c200", reaper_heap_remove(heap, c200),
		  BRAID_OK);
	CHECK("heap-remove: count is 2", heap->count == 2);
	CHECK("heap-remove: c200->heap_index = UINT32_MAX",
	      c200->heap_index == UINT32_MAX);
	CHECK_ERR("heap-remove: peek ok", reaper_heap_peek(heap, &entry),
		  BRAID_OK);
	CHECK("heap-remove: minimum still 100", entry.last_active_ms == 100);
	CHECK("heap-remove: heap_index consistent",
	      heap_index_consistent(heap));

cleanup:
	free_idle_heap(heap);
	free(c100);
	free(c200);
	free(c300);
}

/*
 * Insert 4 entries in strictly descending order of last_active_ms
 * (100, 80, 60, 40) so that each insertion triggers a sift-up.
 * After every insert, verify all conn->heap_index values match their
 * actual positions in the heap array.
 * The final root must be the entry with last_active_ms == 40.
 */
static void
test_heap_index_consistency_after_sift_up(void)
{
	braid_idle_heap_t *heap;
	braid_conn_t *c[4];
	int i;

	heap = make_idle_heap(4);
	c[0] = make_conn(100);
	c[1] = make_conn(80);
	c[2] = make_conn(60);
	c[3] = make_conn(40);
	if (heap == NULL || c[0] == NULL || c[1] == NULL || c[2] == NULL ||
	    c[3] == NULL) {
		CHECK("heap-index-sift-up: allocation", 0);
		goto cleanup;
	}

	for (i = 0; i < 4; i++) {
		reaper_heap_insert(heap, c[i]);
		CHECK("heap-index-sift-up: consistent after insert",
		      heap_index_consistent(heap));
	}

	CHECK("heap-index-sift-up: minimum at root",
	      heap->entries[0].last_active_ms == 40);

cleanup:
	free_idle_heap(heap);
	for (i = 0; i < 4; i++)
		free(c[i]);
}

/*
 * Insert 5 entries in order 50, 10, 30, 20, 40, which produces the heap
 * [10, 20, 30, 50, 40].  Remove the root (c10), causing c40 (the last
 * entry) to be placed at the root and sifted down.
 *
 * Expected heap after removal: [20, 40, 30, 50].
 * Verify: all conn->heap_index values are consistent, c10's heap_index
 * is UINT32_MAX, and the new root is 20.
 */
static void
test_heap_index_consistency_after_sift_down(void)
{
	braid_idle_heap_t *heap;
	braid_conn_t *c50, *c10, *c30, *c20, *c40;

	heap = make_idle_heap(5);
	c50 = make_conn(50);
	c10 = make_conn(10);
	c30 = make_conn(30);
	c20 = make_conn(20);
	c40 = make_conn(40);
	if (heap == NULL || c50 == NULL || c10 == NULL || c30 == NULL ||
	    c20 == NULL || c40 == NULL) {
		CHECK("heap-index-sift-down: allocation", 0);
		goto cleanup;
	}

	reaper_heap_insert(heap, c50);
	reaper_heap_insert(heap, c10);
	reaper_heap_insert(heap, c30);
	reaper_heap_insert(heap, c20);
	reaper_heap_insert(heap, c40);

	CHECK("heap-index-sift-down: initial root is 10",
	      heap->entries[0].last_active_ms == 10);
	CHECK("heap-index-sift-down: consistent after inserts",
	      heap_index_consistent(heap));

	CHECK_ERR("heap-index-sift-down: remove root",
		  reaper_heap_remove(heap, c10), BRAID_OK);

	CHECK("heap-index-sift-down: count is 4", heap->count == 4);
	CHECK("heap-index-sift-down: c10->heap_index = UINT32_MAX",
	      c10->heap_index == UINT32_MAX);
	CHECK("heap-index-sift-down: new root is 20",
	      heap->entries[0].last_active_ms == 20);
	CHECK("heap-index-sift-down: consistent after remove",
	      heap_index_consistent(heap));

cleanup:
	free_idle_heap(heap);
	free(c50);
	free(c10);
	free(c30);
	free(c20);
	free(c40);
}

/*
 * Two IDLE connections with last_active_ms 1000 and 2000.
 * idle_reap_timeout = 5000 ms, min_connections = 0.
 * With now_ms = 10000: both are eligible (9000 and 8000 >= 5000).
 * reaper_advance must reap both: heap empty, live_count zero.
 */
static void
test_reaper_advance_reaps_eligible(void)
{
	braid_pool_t *pool;

	pool = make_reaper_pool(4, 0, 5000);
	if (pool == NULL) {
		CHECK("reaper-advance-reaps: pool alloc", 0);
		return;
	}

	if (alloc_idle_conn(pool, 1000) == NULL ||
	    alloc_idle_conn(pool, 2000) == NULL) {
		CHECK("reaper-advance-reaps: alloc conns", 0);
		goto cleanup;
	}

	CHECK("reaper-advance-reaps: live_count=2 before",
	      pool->live_count == 2);
	CHECK("reaper-advance-reaps: heap count=2 before",
	      pool->idle.count == 2);

	reaper_advance(pool, 10000);

	CHECK("reaper-advance-reaps: heap empty after", pool->idle.count == 0);
	CHECK("reaper-advance-reaps: live_count=0 after",
	      pool->live_count == 0);

cleanup:
	free_reaper_pool(pool);
}

/*
 * Two IDLE connections, both past their reap deadline.
 * min_connections = 2, live_count = 2: the floor is at saturation.
 * reaper_advance must not reap any connection.
 */
static void
test_reaper_advance_respects_floor(void)
{
	braid_pool_t *pool;
	braid_conn_t *conn1, *conn2;

	pool = make_reaper_pool(4, 2, 5000);
	if (pool == NULL) {
		CHECK("reaper-floor: pool alloc", 0);
		return;
	}

	conn1 = alloc_idle_conn(pool, 1000);
	conn2 = alloc_idle_conn(pool, 2000);
	if (conn1 == NULL || conn2 == NULL) {
		CHECK("reaper-floor: alloc conns", 0);
		goto cleanup;
	}

	/* live_count(2) <= min_connections(2) — nothing should be reaped. */
	reaper_advance(pool, 10000);

	CHECK("reaper-floor: heap count unchanged", pool->idle.count == 2);
	CHECK("reaper-floor: live_count unchanged", pool->live_count == 2);

	/* Manually transition surviving connections to DEAD for clean teardown.
	 */
	conn_transition(pool, conn1, BRAID_STATE_CLOSING);
	conn_transition(pool, conn2, BRAID_STATE_CLOSING);

cleanup:
	free_reaper_pool(pool);
}

/*
 * One eligible connection (last_active_ms=1000) and one future connection
 * (last_active_ms=8000) with idle_reap_timeout=5000 and now_ms=10000.
 * Eligible: 10000-1000=9000 >= 5000. Future: 10000-8000=2000 < 5000.
 * reaper_advance must reap the eligible one and leave the future entry.
 */
static void
test_reaper_advance_stops_at_future(void)
{
	braid_pool_t *pool;
	braid_conn_t *conn_future;

	pool = make_reaper_pool(4, 0, 5000);
	if (pool == NULL) {
		CHECK("reaper-stops-future: pool alloc", 0);
		return;
	}

	if (alloc_idle_conn(pool, 1000) == NULL) {
		CHECK("reaper-stops-future: alloc eligible", 0);
		goto cleanup;
	}
	conn_future = alloc_idle_conn(pool, 8000);
	if (conn_future == NULL) {
		CHECK("reaper-stops-future: alloc future", 0);
		goto cleanup;
	}

	CHECK("reaper-stops-future: live_count=2 before",
	      pool->live_count == 2);

	reaper_advance(pool, 10000);

	CHECK("reaper-stops-future: eligible reaped, heap count=1",
	      pool->idle.count == 1);
	CHECK("reaper-stops-future: live_count=1 after", pool->live_count == 1);
	CHECK("reaper-stops-future: remaining entry is future",
	      pool->idle.entries[0].last_active_ms == 8000);

	conn_transition(pool, conn_future, BRAID_STATE_CLOSING);

cleanup:
	free_reaper_pool(pool);
}

/*
 * One IDLE connection with last_active_ms=1000, idle_reap_timeout=5000 ms,
 * now_ms=4000: not yet eligible (4000-1000=3000 < 5000).
 * reaper_advance must not reap it.  The next reap event is at:
 *   last_active_ms + idle_reap_timeout - now_ms = 1000 + 5000 - 4000 = 2000 ms.
 * Verify by peeking the heap and computing the value directly.
 */
static void
test_next_ms_computed_correctly(void)
{
	braid_pool_t *pool;
	braid_idle_entry_t entry;
	braid_conn_t *conn;
	uint64_t now_ms, expected_next_ms;

	pool = make_reaper_pool(4, 0, 5000);
	if (pool == NULL) {
		CHECK("next-ms: pool alloc", 0);
		return;
	}

	conn = alloc_idle_conn(pool, 1000);
	if (conn == NULL) {
		CHECK("next-ms: alloc conn", 0);
		goto cleanup;
	}

	now_ms = 4000;
	expected_next_ms = 1000 + 5000 - 4000; /* 2000 */

	reaper_advance(pool, now_ms);

	CHECK("next-ms: connection not reaped", pool->live_count == 1);
	CHECK("next-ms: heap still has entry", pool->idle.count == 1);

	CHECK_ERR("next-ms: peek ok", reaper_heap_peek(&pool->idle, &entry),
		  BRAID_OK);
	CHECK("next-ms: entry has correct last_active_ms",
	      entry.last_active_ms == 1000);
	CHECK("next-ms: computed next_ms is correct",
	      entry.last_active_ms + (uint64_t)pool->config.idle_reap_timeout -
		      now_ms ==
		  expected_next_ms);

	conn_transition(pool, conn, BRAID_STATE_CLOSING);

cleanup:
	free_reaper_pool(pool);
}

/* ── test entry point ────────────────────────────────────────────────── */

void
run_reaper_tests(void)
{
	test_heap_insert_and_peek();
	test_heap_remove_by_conn();
	test_heap_index_consistency_after_sift_up();
	test_heap_index_consistency_after_sift_down();
	test_reaper_advance_reaps_eligible();
	test_reaper_advance_respects_floor();
	test_reaper_advance_stops_at_future();
	test_next_ms_computed_correctly();
}
