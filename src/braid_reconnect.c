/*
 * braid_reconnect.c — reconnection heap and backoff algorithm
 *
 * Min-heap keyed on next_retry_ms. Full jitter exponential backoff.
 * See ARCHITECTURE.md §6.
 */

#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../include/braid.h"
#include "braid_conn.h"
#include "braid_internal.h"
#include "braid_io.h"
#include "braid_pool.h"
#include "braid_reconnect.h"

/* Default backoff values — applied when config fields are zero. */
#define BRAID_DEFAULT_BACKOFF_BASE 100 /* milliseconds */
#define BRAID_DEFAULT_BACKOFF_CAP 30000 /* milliseconds */
#define BRAID_DEFAULT_INIT_TIMEOUT 10000 /* milliseconds */

#ifdef BRAID_TEST_CLOCK
static int (*reconnect_test_socket_create_hook)(braid_pool_t *,
						struct addrinfo *, int *,
						int *);
#endif

/* ── PRNG helpers ─────────────────────────────────────────────────────── */

/*
 * add_sat_u64 — saturating uint64_t addition.
 *
 * Used for reconnect scheduling and callback deadlines to avoid overflow.
 * See CODING_STANDARDS.md §6.
 */
static uint64_t
add_sat_u64(uint64_t a, uint64_t b)
{
	if (b > UINT64_MAX - a)
		return UINT64_MAX;
	return a + b;
}

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

	if (x == 0)
		x = 0x9e3779b97f4a7c15ULL;

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

#ifdef BRAID_TEST_CLOCK
void
reconnect_test_set_socket_create_hook(int (*hook)(braid_pool_t *,
						  struct addrinfo *, int *,
						  int *))
{
	reconnect_test_socket_create_hook = hook;
}
#endif

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
		left = (2 * i) + 1;
		right = (2 * i) + 2;
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
 * reconnect_fire_event — dispatch BRAID_EV_RECONNECT_ATTEMPT via observe_fn.
 *
 * Uses the in_callback protocol so that re-entrant checkin during observe_fn
 * sees in_callback > 0 and defers correctly. No-op if observe_fn is NULL.
 * fd is -1 on failure paths where no connection record was created.
 */
static void
reconnect_fire_event(braid_pool_t *pool, int fd, uint32_t attempt, int success)
{
	braid_event_t ev;

	if (pool->config.observe_fn == NULL)
		return;

	memset(&ev, 0, sizeof(ev));
	ev.type = BRAID_EV_RECONNECT_ATTEMPT;
	ev.fd = fd;
	ev.reconnect_attempt.attempt = attempt;
	ev.reconnect_attempt.success = success;

	pool->in_callback++;
	pool->config.observe_fn(&ev, pool->config.hook_context);
	pool->in_callback--;
	if (pool->in_callback == 0 && pool->deferred_work != 0)
		pool_drain_deferred(pool);
}

/*
 * reconnect_schedule_retry — push a new reconnect entry for attempt+1.
 *
 * The next deadline is now + backoff_delay(attempt+1). When the heap is
 * full (BRAID_ERR_EXHAUSTED), the entry is silently dropped — a full heap
 * means max_connections reconnects are already queued; the new failure
 * is not an additional concern.
 */
static void
reconnect_schedule_retry(braid_pool_t *pool, uint32_t attempt)
{
	braid_reconnect_entry_t next;
	uint64_t now_ms;
	uint64_t delay_ms;

	next.attempt = attempt + 1;
	now_ms = braid_now_ms();
	delay_ms = reconnect_backoff_delay(pool, attempt + 1);
	next.next_retry_ms = add_sat_u64(now_ms, delay_ms);
	reconnect_heap_push(&pool->reconnect,
			    next); /* EXHAUSTED: best effort */
}

/*
 * reconnect_attempt — perform one reconnection attempt for the given entry.
 *
 * Flow per ARCHITECTURE.md §6.3:
 *   1. max_attempts check: fire failure event and return (no re-insert).
 *   2. DNS: getaddrinfo(). On failure, schedule retry; no event.
 *   3. Socket creation + non-blocking connect().
 *      On failure (errno != EINPROGRESS), schedule retry; fire failure event.
 *   4. conn_alloc() for the new fd. On failure, close fd; schedule retry.
 *   5a. Immediate connect (connect()==0): CONNECTING→INITIALIZING, call
 *       init_fn if set, then INITIALIZING→IDLE. Register for reads.
 *       On init_fn failure: INITIALIZING→DEAD, schedule retry, fire failure.
 *   5b. EINPROGRESS: fd already in CONNECTING (via conn_alloc); register
 *       for writability. Connect completion handled in braid_pool_notify.
 *   6. Fire success event.
 *
 * No reconnect entry is inserted on the success path; one is inserted only
 * on failure (DNS fail, connect error, init_fn fail). See ARCHITECTURE.md §6.
 */
static int
reconnect_attempt(braid_pool_t *pool, braid_reconnect_entry_t entry)
{
	struct addrinfo hints, *res;
	char port_str[6]; /* max "65535\0" */
	braid_conn_t *conn;
	int fd, rc, immediate;

	/* Step 1: max_attempts check — stops retrying when limit reached. */
	if (pool->config.backoff_max_attempts > 0 &&
	    entry.attempt >= pool->config.backoff_max_attempts) {
		reconnect_fire_event(pool, -1, entry.attempt, 0);
		return BRAID_OK;
	}

	/* Step 2: DNS resolution — fresh on every attempt for DNS failover. */
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	snprintf(port_str, sizeof(port_str), "%u", (unsigned)pool->config.port);
	if (getaddrinfo(pool->config.host, port_str, &hints, &res) != 0) {
		reconnect_schedule_retry(pool, entry.attempt);
		return BRAID_OK;
	}

	/* Step 3: create socket, apply keepalive, non-blocking connect. */
#ifdef BRAID_TEST_CLOCK
	if (reconnect_test_socket_create_hook != NULL)
		rc = reconnect_test_socket_create_hook(pool, res, &fd,
						       &immediate);
	else
#endif
		rc = conn_socket_create(pool, res, &fd, &immediate);
	freeaddrinfo(res);
	if (rc != BRAID_OK) {
		reconnect_schedule_retry(pool, entry.attempt);
		reconnect_fire_event(pool, -1, entry.attempt, 0);
		return BRAID_OK;
	}

	/* Step 4: allocate connection record (bootstraps CONNECTING state). */
	rc = conn_alloc(pool, fd, &conn);
	if (rc != BRAID_OK) {
		close(fd);
		reconnect_schedule_retry(pool, entry.attempt);
		return BRAID_OK;
	}

	if (immediate) {
		/*
		 * Step 5a: connect() returned 0 — no writable-event wait.
		 * Transition directly to INITIALIZING and run init_fn inline.
		 */
		conn_transition(pool, conn, BRAID_STATE_INITIALIZING);

		if (pool->config.init_fn != NULL) {
			uint32_t timeout;
			uint64_t now_ms;
			uint64_t deadline;
			int init_rc;

			timeout = pool->config.init_timeout != 0
				      ? pool->config.init_timeout
				      : BRAID_DEFAULT_INIT_TIMEOUT;
			now_ms = braid_now_ms();
			deadline = add_sat_u64(now_ms, (uint64_t)timeout);
			pool->in_callback++;
			init_rc = pool->config.init_fn(
			    fd, &conn->conn_ctx, pool->config.hook_context,
			    deadline);
			pool->in_callback--;
			if (pool->in_callback == 0 && pool->deferred_work != 0)
				pool_drain_deferred(pool);

			if (init_rc != BRAID_OK) {
				conn_transition(pool, conn, BRAID_STATE_DEAD);
				reconnect_schedule_retry(pool, entry.attempt);
				reconnect_fire_event(pool, -1, entry.attempt,
						     0);
				return BRAID_OK;
			}
		}

		conn_transition(pool, conn, BRAID_STATE_IDLE);
		io_watch(pool, fd, BRAID_IO_READ);
	} else {
		/*
		 * Step 5b: EINPROGRESS — conn already in CONNECTING state
		 * (set by conn_alloc). Register for writability; connect
		 * completion is signalled via braid_pool_notify() (Phase 6).
		 */
		io_watch(pool, fd, BRAID_IO_WRITE);
	}

	/* Step 6: fire success event (attempt in progress or completed). */
	reconnect_fire_event(pool, fd, entry.attempt, 1);
	return BRAID_OK;
}

/*
 * reconnect_advance — process all due reconnection heap entries.
 *
 * Pops entries with next_retry_ms <= now_ms and calls reconnect_attempt()
 * for each. Stops when the heap minimum is in the future or empty.
 *
 * The loop is bounded by the entry count at call entry (limit). Without
 * this bound, a failed attempt that re-inserts with delay=0 would cause
 * the same entry to be processed again within the same advance call.
 * Entries re-inserted this call are deferred to the next invocation.
 * See ARCHITECTURE.md §6.3.
 */
int
reconnect_advance(braid_pool_t *pool, uint64_t now_ms)
{
	braid_reconnect_entry_t entry;
	uint32_t limit;

	limit = pool->reconnect.count;

	while (limit-- > 0) {
		if (reconnect_heap_peek(&pool->reconnect, &entry) != BRAID_OK)
			break;
		if (entry.next_retry_ms > now_ms)
			break;
		reconnect_heap_pop(&pool->reconnect, &entry);
		reconnect_attempt(pool, entry);
	}
	return BRAID_OK;
}
