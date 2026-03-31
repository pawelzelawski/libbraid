/*
 * braid_reaper.c — idle reaper heap and reap logic
 *
 * Min-heap keyed on last_active_ms. Reaps connections exceeding
 * idle_reap_timeout, subject to the min_connections floor.
 * See ARCHITECTURE.md §7.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../include/braid.h"
#include "braid_conn.h"
#include "braid_internal.h"
#include "braid_reaper.h"

#define BRAID_DEFAULT_IDLE_REAP_TIMEOUT 300000 /* milliseconds */

/*
 * Test-visible call counters — incremented by insert and remove so that
 * test_state_machine.c can verify reaper heap entry/exit invariants.
 * Always compiled in; only exposed via braid_reaper.h under BRAID_TEST_CLOCK.
 */
int braid_test_reaper_insert_count = 0;
int braid_test_reaper_remove_count = 0;

/* ── min-heap helpers ─────────────────────────────────────────────────── */

/*
 * reaper_swap — exchange entries at positions a and b, maintaining
 * conn->heap_index on both connection records.
 */
static void
reaper_swap(braid_idle_heap_t *heap, uint32_t a, uint32_t b)
{
	braid_idle_entry_t tmp;

	tmp = heap->entries[a];
	heap->entries[a] = heap->entries[b];
	heap->entries[b] = tmp;
	heap->entries[a].conn->heap_index = a;
	heap->entries[b].conn->heap_index = b;
}

/*
 * reaper_sift_up — restore heap order by moving the entry at i toward
 * the root while it is smaller than its parent.
 */
static void
reaper_sift_up(braid_idle_heap_t *heap, uint32_t i)
{
	uint32_t parent;

	while (i > 0) {
		parent = (i - 1) / 2;
		if (heap->entries[parent].last_active_ms <=
		    heap->entries[i].last_active_ms)
			break;
		reaper_swap(heap, parent, i);
		i = parent;
	}
}

/*
 * reaper_sift_down — restore heap order by moving the entry at i down
 * toward the leaves, swapping with the smaller child each step.
 */
static void
reaper_sift_down(braid_idle_heap_t *heap, uint32_t i)
{
	uint32_t left, right, smallest;

	for (;;) {
		left = 2 * i + 1;
		right = 2 * i + 2;
		smallest = i;

		if (left < heap->count &&
		    heap->entries[left].last_active_ms <
			heap->entries[smallest].last_active_ms)
			smallest = left;
		if (right < heap->count &&
		    heap->entries[right].last_active_ms <
			heap->entries[smallest].last_active_ms)
			smallest = right;

		if (smallest == i)
			break;
		reaper_swap(heap, i, smallest);
		i = smallest;
	}
}

/* ── public functions ─────────────────────────────────────────────────── */

/*
 * reaper_heap_init — allocate cap entry slots and initialise count to zero.
 * Called once at pool creation. No allocation after this point.
 */
int
reaper_heap_init(braid_idle_heap_t *heap, uint32_t cap)
{
	if (cap == 0)
		return BRAID_ERR_INVAL;

	heap->entries = malloc(cap * sizeof(*heap->entries));
	if (heap->entries == NULL)
		return BRAID_ERR_NOMEM;
	heap->count = 0;
	heap->cap = cap;
	return BRAID_OK;
}

/*
 * reaper_heap_destroy — free the entry array. Called at pool destruction.
 */
void
reaper_heap_destroy(braid_idle_heap_t *heap)
{
	free(heap->entries);
	heap->entries = NULL;
	heap->count = 0;
	heap->cap = 0;
}

/*
 * reaper_heap_insert — O(log n) insert with sift-up.
 * Writes the assigned heap position into conn->heap_index at every swap.
 * Returns BRAID_ERR_EXHAUSTED if the heap is at capacity.
 */
int
reaper_heap_insert(braid_idle_heap_t *heap, braid_conn_t *conn)
{
	uint32_t i;

	if (heap->count >= heap->cap)
		return BRAID_ERR_EXHAUSTED;

	i = heap->count++;
	heap->entries[i].last_active_ms = conn->last_active_ms;
	heap->entries[i].conn = conn;
	conn->heap_index = i;
	reaper_sift_up(heap, i);
	braid_test_reaper_insert_count++;
	return BRAID_OK;
}

/*
 * reaper_heap_remove — O(log n) remove using the position stored in
 * conn->heap_index.  Swaps with the last entry, decrements count, then
 * attempts both sift-up and sift-down on the moved element — sift-down
 * alone is insufficient when the moved element is smaller than its new
 * parent.  Sets conn->heap_index = UINT32_MAX on completion.
 */
int
reaper_heap_remove(braid_idle_heap_t *heap, braid_conn_t *conn)
{
	uint32_t i;
	braid_conn_t *moved;

	if (heap->count == 0 || conn->heap_index == UINT32_MAX)
		return BRAID_ERR_INVAL;

	i = conn->heap_index;
	heap->count--;

	if (i < heap->count) {
		heap->entries[i] = heap->entries[heap->count];
		moved = heap->entries[i].conn;
		moved->heap_index = i;
		reaper_sift_up(heap, i);
		reaper_sift_down(heap, moved->heap_index);
	}

	conn->heap_index = UINT32_MAX;
	braid_test_reaper_remove_count++;
	return BRAID_OK;
}

/*
 * reaper_heap_peek — copy the root entry (minimum last_active_ms) to *out.
 * Returns BRAID_ERR_EXHAUSTED if the heap is empty.
 */
int
reaper_heap_peek(braid_idle_heap_t *heap, braid_idle_entry_t *out)
{
	if (heap->count == 0)
		return BRAID_ERR_EXHAUSTED;

	*out = heap->entries[0];
	return BRAID_OK;
}

/*
 * reaper_advance — reap eligible idle connections per ARCHITECTURE.md §7.2.
 *
 * Peeks the heap minimum on each iteration; stops when the heap is empty,
 * the minimum was active too recently, or the min_connections floor would
 * be breached.  conn_transition(→ CLOSING) removes the entry from the heap
 * as an IDLE-exit invariant, so the heap advances automatically each loop.
 */
int
reaper_advance(braid_pool_t *pool, uint64_t now_ms)
{
	braid_idle_entry_t entry;
	uint64_t timeout;

	timeout = pool->config.idle_reap_timeout != 0
		      ? (uint64_t)pool->config.idle_reap_timeout
		      : BRAID_DEFAULT_IDLE_REAP_TIMEOUT;

	while (reaper_heap_peek(&pool->idle, &entry) == BRAID_OK) {
		if (now_ms - entry.last_active_ms < timeout)
			break;
		if (pool->live_count <= pool->config.min_connections)
			break;
		conn_transition(pool, entry.conn, BRAID_STATE_CLOSING);
	}

	return BRAID_OK;
}
