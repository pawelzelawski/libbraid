/*
 * braid_reconnect.c — reconnection heap and backoff algorithm
 *
 * Min-heap keyed on next_retry_ms. Full jitter exponential backoff.
 * See ARCHITECTURE.md §6.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../include/braid.h"
#include "braid_internal.h"
#include "braid_reconnect.h"

/* Default backoff values — applied when config fields are zero. */
#define BRAID_DEFAULT_BACKOFF_BASE 100 /* milliseconds */
#define BRAID_DEFAULT_BACKOFF_CAP 30000 /* milliseconds */

/* ── PRNG helpers ─────────────────────────────────────────────────────── */

/*
 * pool_prng_next — advance the per-pool xorshift64 PRNG and return the next
 * pseudo-random value. See ARCHITECTURE.md §6.2.
 *
 * pool->prng must be seeded to a non-zero value before the first call. In
 * production this is done in braid_pool_create() via getentropy(); in tests
 * the seed is written directly to pool->prng.
 */
static uint64_t
pool_prng_next(braid_pool_t *pool)
{
	uint64_t x = pool->prng;

	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	pool->prng = x;
	return x;
}

/*
 * pool_prng_uniform — return a pseudo-random value in [0, window] inclusive.
 * Returns 0 when window is 0.
 */
static uint64_t
pool_prng_uniform(braid_pool_t *pool, uint64_t window)
{
	if (window == 0)
		return 0;
	return pool_prng_next(pool) % (window + 1);
}

/* ── min-heap helpers ─────────────────────────────────────────────────── */

/*
 * heap_swap — exchange two entries by index.
 */
static void
heap_swap(braid_reconnect_heap_t *heap, uint32_t a, uint32_t b)
{
	braid_reconnect_entry_t tmp;

	tmp = heap->entries[a];
	heap->entries[a] = heap->entries[b];
	heap->entries[b] = tmp;
}

/*
 * heap_bubble_up — restore heap order after inserting at index i.
 * Moves the entry at i up until its parent is no larger, or it reaches root.
 */
static void
heap_bubble_up(braid_reconnect_heap_t *heap, uint32_t i)
{
	uint32_t parent;

	while (i > 0) {
		parent = (i - 1) / 2;
		if (heap->entries[parent].next_retry_ms <=
		    heap->entries[i].next_retry_ms)
			break;
		heap_swap(heap, parent, i);
		i = parent;
	}
}

/*
 * heap_sift_down — restore heap order after replacing root with last entry.
 * Moves the entry at i down, swapping with the smaller child, until the
 * heap property is restored.
 */
static void
heap_sift_down(braid_reconnect_heap_t *heap, uint32_t i)
{
	uint32_t left, right, smallest;

	for (;;) {
		left = 2 * i + 1;
		right = 2 * i + 2;
		smallest = i;

		if (left < heap->count &&
		    heap->entries[left].next_retry_ms <
			heap->entries[smallest].next_retry_ms)
			smallest = left;
		if (right < heap->count &&
		    heap->entries[right].next_retry_ms <
			heap->entries[smallest].next_retry_ms)
			smallest = right;

		if (smallest == i)
			break;
		heap_swap(heap, i, smallest);
		i = smallest;
	}
}

/* ── public functions ─────────────────────────────────────────────────── */

/*
 * reconnect_heap_init — allocate cap entry slots and set count to zero.
 * Called once from braid_pool_create(). No allocation after this point.
 */
int
reconnect_heap_init(braid_reconnect_heap_t *heap, uint32_t cap)
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
 * reconnect_heap_destroy — free the entry array. Called from
 * braid_pool_destroy().
 */
void
reconnect_heap_destroy(braid_reconnect_heap_t *heap)
{
	free(heap->entries);
	heap->entries = NULL;
	heap->count = 0;
	heap->cap = 0;
}

/*
 * reconnect_heap_push — O(log n) insert with bubble-up.
 * Returns BRAID_ERR_EXHAUSTED if the heap is at capacity.
 */
int
reconnect_heap_push(braid_reconnect_heap_t *heap, braid_reconnect_entry_t entry)
{
	uint32_t i;

	if (heap->count >= heap->cap)
		return BRAID_ERR_EXHAUSTED;

	i = heap->count++;
	heap->entries[i] = entry;
	heap_bubble_up(heap, i);
	return BRAID_OK;
}

/*
 * reconnect_heap_peek — read the minimum next_retry_ms without removing it.
 * Returns BRAID_ERR_EXHAUSTED if the heap is empty.
 */
int
reconnect_heap_peek(braid_reconnect_heap_t *heap, braid_reconnect_entry_t *out)
{
	if (heap->count == 0)
		return BRAID_ERR_EXHAUSTED;

	*out = heap->entries[0];
	return BRAID_OK;
}

/*
 * reconnect_heap_pop — O(log n) delete-min with sift-down.
 * Writes the removed minimum to *out.
 * Returns BRAID_ERR_EXHAUSTED if the heap is empty.
 */
int
reconnect_heap_pop(braid_reconnect_heap_t *heap, braid_reconnect_entry_t *out)
{
	if (heap->count == 0)
		return BRAID_ERR_EXHAUSTED;

	*out = heap->entries[0];
	heap->count--;

	if (heap->count > 0) {
		heap->entries[0] = heap->entries[heap->count];
		heap_sift_down(heap, 0);
	}
	return BRAID_OK;
}

/*
 * reconnect_heap_clear — reset count to zero without freeing memory.
 * Used at pool destroy to discard pending reconnections without deallocating.
 */
void
reconnect_heap_clear(braid_reconnect_heap_t *heap)
{
	heap->count = 0;
}

/*
 * reconnect_backoff_delay — compute a full-jitter exponential backoff delay
 * for the given attempt number.
 *
 * Algorithm: delay = random(0, min(cap, base × 2^attempt))
 * The exponent is capped at 31 to prevent uint64_t left-shift overflow;
 * base is uint32_t so base << 31 uses at most 63 significant bits,
 * which fits in uint64_t. See ARCHITECTURE.md §6.2.
 *
 * Default config values are applied when the config fields are zero.
 * See ARCHITECTURE.md §16.
 */
uint64_t
reconnect_backoff_delay(braid_pool_t *pool, uint32_t attempt)
{
	uint32_t exp;
	uint64_t window;
	uint32_t base, cap;

	base = pool->config.backoff_base != 0 ? pool->config.backoff_base
					      : BRAID_DEFAULT_BACKOFF_BASE;
	cap = pool->config.backoff_cap != 0 ? pool->config.backoff_cap
					    : BRAID_DEFAULT_BACKOFF_CAP;

	/*
	 * Full jitter: delay = random(0, min(cap, base × 2^attempt)).
	 * Exponent capped at 31: base is uint32_t so base << 31 fits
	 * in uint64_t without overflow. See ARCHITECTURE.md §6.2.
	 */
	exp = (attempt < 31) ? attempt : 31;
	window = (uint64_t)base << exp;
	if (window > (uint64_t)cap)
		window = (uint64_t)cap;

	return pool_prng_uniform(pool, window);
}

/*
 * reconnect_advance — process due reconnection entries.
 * Stub — full implementation in Phase 5 Task 5.3.
 */
int
reconnect_advance(braid_pool_t *pool, uint64_t now_ms)
{
	(void)pool;
	(void)now_ms;
	return BRAID_OK;
}
