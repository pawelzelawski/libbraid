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
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../include/braid.h"
#include "../src/braid_conn.h"
#include "../src/braid_internal.h"
#include "../src/braid_reaper.h"
#include "../src/braid_reconnect.h"
#include "../src/braid_table.h"
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

/* ── backoff test cases ──────────────────────────────────────────────── */

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

/*
 * Different pool seeds must yield different jitter sequences, preventing
 * accidental cross-pool synchronisation when many pools reconnect together.
 */
static void
test_backoff_per_pool_prng_differs(void)
{
	braid_pool_t *pool_a, *pool_b;
	uint64_t a0, a1, a2;
	uint64_t b0, b1, b2;

	pool_a = make_pool_for_backoff(0x1111111111111111ULL, UINT32_MAX,
				       UINT32_MAX);
	pool_b = make_pool_for_backoff(0x2222222222222222ULL, UINT32_MAX,
				       UINT32_MAX);
	if (pool_a == NULL || pool_b == NULL) {
		CHECK("backoff_per_pool_prng_differs: alloc", 0);
		free(pool_a);
		free(pool_b);
		return;
	}

	a0 = reconnect_backoff_delay(pool_a, 0);
	a1 = reconnect_backoff_delay(pool_a, 0);
	a2 = reconnect_backoff_delay(pool_a, 0);
	b0 = reconnect_backoff_delay(pool_b, 0);
	b1 = reconnect_backoff_delay(pool_b, 0);
	b2 = reconnect_backoff_delay(pool_b, 0);

	CHECK("backoff_per_pool_prng_differs: sequences differ",
	      a0 != b0 || a1 != b1 || a2 != b2);

	free(pool_a);
	free(pool_b);
}

/* ── reconnect_advance test infrastructure ───────────────────────────── */

static int g_reconnect_event_count;
static braid_event_t g_last_reconnect_event;

static void
cb_reconnect_observe(const braid_event_t *ev, void *hook)
{
	(void)hook;
	g_reconnect_event_count++;
	g_last_reconnect_event = *ev;
}

static void
reset_reconnect_counters(void)
{
	g_reconnect_event_count = 0;
	memset(&g_last_reconnect_event, 0, sizeof(g_last_reconnect_event));
}

/*
 * make_testpool — allocate a pool suitable for reconnect_advance tests.
 *
 * Initialises pool->config, the reconnection heap (cap = max_connections),
 * and the connection hash table. The prng is seeded to a non-zero value.
 * Caller must free with free_testpool().
 */
static braid_pool_t *
make_testpool(uint32_t max_connections, uint32_t max_attempts, const char *host,
	      uint16_t port, braid_observe_fn observe_fn)
{
	braid_pool_t *pool;
	int epfd;

	pool = calloc(1, sizeof(*pool));
	if (pool == NULL)
		return NULL;

	pool->config.max_connections = max_connections;
	pool->config.min_connections = 0;
	pool->config.backoff_max_attempts = max_attempts;
	pool->config.host = host;
	pool->config.port = port;
	epfd = epoll_create1(EPOLL_CLOEXEC);
	if (epfd < 0) {
		free(pool);
		return NULL;
	}
	pool->config.event_fd = epfd;
	pool->config.backoff_base = 100;
	pool->config.backoff_cap = 30000;
	pool->config.observe_fn = observe_fn;
	pool->prng = 0xdeadbeefcafeULL;

	if (reconnect_heap_init(&pool->reconnect, max_connections) !=
	    BRAID_OK) {
		close(epfd);
		free(pool);
		return NULL;
	}
	if (table_init(pool) != BRAID_OK) {
		reconnect_heap_destroy(&pool->reconnect);
		close(epfd);
		free(pool);
		return NULL;
	}
	if (reaper_heap_init(&pool->idle, max_connections) != BRAID_OK) {
		table_destroy(pool);
		reconnect_heap_destroy(&pool->reconnect);
		close(epfd);
		free(pool);
		return NULL;
	}
	return pool;
}

static void
free_testpool(braid_pool_t *pool)
{
	if (pool->config.event_fd >= 0)
		close(pool->config.event_fd);
	reaper_heap_destroy(&pool->idle);
	table_destroy(pool);
	reconnect_heap_destroy(&pool->reconnect);
	free(pool);
}

static braid_conn_t *
find_live_conn(braid_pool_t *pool)
{
	uint32_t i;

	for (i = 0; i < pool->table_size; i++) {
		if (pool->table[i].fd >= 0)
			return &pool->table[i];
	}
	return NULL;
}

static int
fake_socket_create_fail(braid_pool_t *pool, struct addrinfo *ai, int *fd_out,
			int *immediate_out)
{
	(void)pool;
	(void)ai;
	(void)fd_out;
	(void)immediate_out;
	return BRAID_ERR_SYSCALL;
}

static int
fake_socket_create_immediate(braid_pool_t *pool, struct addrinfo *ai,
			     int *fd_out, int *immediate_out)
{
	int sv[2];

	(void)pool;
	(void)ai;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
		return BRAID_ERR_SYSCALL;
	close(sv[1]);

	*fd_out = sv[0];
	*immediate_out = 1;
	return BRAID_OK;
}

static int
fake_socket_create_immediate_watch_fail(braid_pool_t *pool, struct addrinfo *ai,
					int *fd_out, int *immediate_out)
{
	int fd;

	(void)pool;
	(void)ai;

	fd = open("/dev/null", O_RDONLY);
	if (fd < 0)
		return BRAID_ERR_SYSCALL;

	*fd_out = fd;
	*immediate_out = 1;
	return BRAID_OK;
}

static int
fake_socket_create_einprogress_watch_fail(braid_pool_t *pool,
					  struct addrinfo *ai, int *fd_out,
					  int *immediate_out)
{
	int fd;

	(void)pool;
	(void)ai;

	fd = open("/dev/null", O_RDONLY);
	if (fd < 0)
		return BRAID_ERR_SYSCALL;

	*fd_out = fd;
	*immediate_out = 0;
	return BRAID_OK;
}

/* ── reconnect_advance test cases ────────────────────────────────────── */

/*
 * max_attempts = N: when entry.attempt == N the engine must not re-insert
 * the entry and must fire BRAID_EV_RECONNECT_ATTEMPT with success=0.
 * No DNS resolution or socket creation occurs on this path.
 */
static void
test_max_attempts_N_stops(void)
{
	braid_pool_t *pool;
	braid_reconnect_entry_t entry;

	reset_reconnect_counters();
	pool = make_testpool(8, 3, "127.0.0.1", 1, cb_reconnect_observe);
	if (pool == NULL) {
		CHECK("max_attempts_N_stops: alloc", 0);
		return;
	}
	entry.attempt = 3; /* at cap — must not retry */
	entry.next_retry_ms = 0;
	reconnect_heap_push(&pool->reconnect, entry);

	reconnect_advance(pool, 1000);

	CHECK("max_attempts_N_stops: heap empty (not re-inserted)",
	      pool->reconnect.count == 0);
	CHECK("max_attempts_N_stops: event fired",
	      g_reconnect_event_count == 1);
	CHECK("max_attempts_N_stops: event type",
	      g_last_reconnect_event.type == BRAID_EV_RECONNECT_ATTEMPT);
	CHECK("max_attempts_N_stops: event attempt==3",
	      g_last_reconnect_event.reconnect_attempt.attempt == 3);
	CHECK("max_attempts_N_stops: event success==0",
	      g_last_reconnect_event.reconnect_attempt.success == 0);

	free_testpool(pool);
}

/*
 * max_attempts = 0 means retry forever: even at attempt=50 the engine must
 * not invoke the max_attempts check and must re-insert the entry on failure.
 *
 * The test uses an RFC 2606 .invalid hostname which is guaranteed to never
 * resolve. getaddrinfo() returns EAI_NONAME immediately on any compliant
 * resolver (systemd-resolved returns it without a network round-trip).
 * The DNS-failure path schedules a retry without opening a socket, so there
 * is no fd leak regardless of the system's network state.
 */
static void
test_max_attempts_zero_retries_forever(void)
{
	braid_pool_t *pool;
	braid_reconnect_entry_t entry;

	pool = make_testpool(8, 0, "braid-test-unresolvable.invalid", 1, NULL);
	if (pool == NULL) {
		CHECK("max_attempts_zero: alloc", 0);
		return;
	}
	entry.attempt = 50; /* well above any reasonable limit; must not stop */
	entry.next_retry_ms = 0;
	reconnect_heap_push(&pool->reconnect, entry);

	reconnect_advance(pool, 1000);

	/* DNS failure path re-inserts entry; max_attempts=0 never stops it. */
	CHECK("max_attempts_zero: entry re-inserted (retry forever)",
	      pool->reconnect.count > 0);

	free_testpool(pool);
}

/*
 * Entries with next_retry_ms <= now_ms must be popped and processed.
 * Using max_attempts=1 / attempt=1 triggers the no-insert code path so
 * the heap is left empty — proving the entry was consumed.
 */
static void
test_reconnect_advance_fires_due(void)
{
	braid_pool_t *pool;
	braid_reconnect_entry_t entry;

	reset_reconnect_counters();
	pool = make_testpool(8, 1, "127.0.0.1", 1, cb_reconnect_observe);
	if (pool == NULL) {
		CHECK("advance_fires_due: alloc", 0);
		return;
	}
	entry.attempt = 1; /* at max_attempts → max_attempts check fires */
	entry.next_retry_ms = 500;
	reconnect_heap_push(&pool->reconnect, entry);

	CHECK("advance_fires_due: count == 1 before",
	      pool->reconnect.count == 1);
	reconnect_advance(pool, 1000); /* 1000 >= 500 → entry is due */
	CHECK("advance_fires_due: count == 0 after (entry consumed)",
	      pool->reconnect.count == 0);

	free_testpool(pool);
}

/*
 * Entries with next_retry_ms > now_ms must not be processed.
 * After advance the count must remain 1.
 */
static void
test_reconnect_advance_skips_future(void)
{
	braid_pool_t *pool;
	braid_reconnect_entry_t entry;

	pool = make_testpool(8, 1, "127.0.0.1", 1, NULL);
	if (pool == NULL) {
		CHECK("advance_skips_future: alloc", 0);
		return;
	}
	entry.attempt = 0;
	entry.next_retry_ms = 999999; /* far in the future */
	reconnect_heap_push(&pool->reconnect, entry);

	reconnect_advance(pool, 1000); /* 1000 < 999999 → skip */

	CHECK("advance_skips_future: count == 1 (entry not consumed)",
	      pool->reconnect.count == 1);

	free_testpool(pool);
}

/*
 * observe_fn must be invoked with BRAID_EV_RECONNECT_ATTEMPT carrying the
 * correct attempt number. The max_attempts path is used to keep the test
 * deterministic without a real network call.
 */
static void
test_reconnect_attempt_event_fired(void)
{
	braid_pool_t *pool;
	braid_reconnect_entry_t entry;

	reset_reconnect_counters();
	pool = make_testpool(8, 5, "127.0.0.1", 1, cb_reconnect_observe);
	if (pool == NULL) {
		CHECK("attempt_event_fired: alloc", 0);
		return;
	}
	entry.attempt = 5; /* at max_attempts=5 → max_attempts check fires */
	entry.next_retry_ms = 0;
	reconnect_heap_push(&pool->reconnect, entry);

	reconnect_advance(pool, 1000);

	CHECK("attempt_event_fired: observe_fn called",
	      g_reconnect_event_count == 1);
	CHECK("attempt_event_fired: event type == RECONNECT_ATTEMPT",
	      g_last_reconnect_event.type == BRAID_EV_RECONNECT_ATTEMPT);
	CHECK("attempt_event_fired: attempt number == 5",
	      g_last_reconnect_event.reconnect_attempt.attempt == 5);
	CHECK("attempt_event_fired: success == 0",
	      g_last_reconnect_event.reconnect_attempt.success == 0);

	free_testpool(pool);
}

/*
 * Retry entries are inserted only on failure paths, not at attempt start.
 * Force a connect failure via test hook and verify exactly one attempt+1
 * entry is present after reconnect_advance() consumes the due entry.
 */
static void
test_reconnect_entry_inserted_only_on_failure(void)
{
	braid_pool_t *pool;
	braid_reconnect_entry_t entry;

	reconnect_test_set_socket_create_hook(fake_socket_create_fail);
	pool = make_testpool(8, 0, "127.0.0.1", 8080, NULL);
	if (pool == NULL) {
		CHECK("reconnect_entry_failure_only: alloc", 0);
		reconnect_test_set_socket_create_hook(NULL);
		return;
	}

	entry.attempt = 7;
	entry.next_retry_ms = 0;
	reconnect_heap_push(&pool->reconnect, entry);

	reconnect_advance(pool, 1000);

	CHECK("reconnect_entry_failure_only: one retry enqueued",
	      pool->reconnect.count == 1);
	CHECK_ERR("reconnect_entry_failure_only: peek queued retry",
		  reconnect_heap_peek(&pool->reconnect, &entry), BRAID_OK);
	CHECK("reconnect_entry_failure_only: retry attempt incremented",
	      entry.attempt == 8);

	free_testpool(pool);
	reconnect_test_set_socket_create_hook(NULL);
}

/*
 * Fast-path connect (connect()==0) must reach IDLE directly without waiting
 * on a writable event and must not pre-schedule a reconnect retry.
 * The socket-create hook forces an immediate-connect path deterministically.
 */
static void
test_connect_zero_fast_path_reaches_idle_without_writable_event(void)
{
	braid_pool_t *pool;
	braid_reconnect_entry_t entry;
	braid_conn_t *conn;

	reconnect_test_set_socket_create_hook(fake_socket_create_immediate);
	pool = make_testpool(8, 0, "127.0.0.1", 8080, NULL);
	if (pool == NULL) {
		CHECK("connect_zero_fast_path: alloc", 0);
		reconnect_test_set_socket_create_hook(NULL);
		return;
	}

	entry.attempt = 0;
	entry.next_retry_ms = 0;
	reconnect_heap_push(&pool->reconnect, entry);

	reconnect_advance(pool, 1000);

	CHECK("connect_zero_fast_path: no retry pre-inserted",
	      pool->reconnect.count == 0);
	conn = find_live_conn(pool);
	CHECK("connect_zero_fast_path: connection created", conn != NULL);
	if (conn != NULL)
		CHECK("connect_zero_fast_path: state reached IDLE",
		      conn->state == BRAID_STATE_IDLE);

	if (conn != NULL && conn->state == BRAID_STATE_IDLE)
		conn_transition(pool, conn, BRAID_STATE_CLOSING);

	free_testpool(pool);
	reconnect_test_set_socket_create_hook(NULL);
}

/*
 * If io_watch(READ) fails after immediate-connect initialisation, the
 * connection must be discarded and exactly one retry attempt+1 enqueued.
 */
static void
test_immediate_watch_failure_discards_and_schedules_retry(void)
{
	braid_pool_t *pool;
	braid_reconnect_entry_t entry;

	reset_reconnect_counters();
	reconnect_test_set_socket_create_hook(
	    fake_socket_create_immediate_watch_fail);
	pool = make_testpool(8, 0, "127.0.0.1", 8080, cb_reconnect_observe);
	if (pool == NULL) {
		CHECK("immediate_watch_failure: alloc", 0);
		reconnect_test_set_socket_create_hook(NULL);
		return;
	}

	entry.attempt = 2;
	entry.next_retry_ms = 0;
	reconnect_heap_push(&pool->reconnect, entry);

	reconnect_advance(pool, 1000);

	CHECK("immediate_watch_failure: retry enqueued",
	      pool->reconnect.count == 1);
	CHECK_ERR("immediate_watch_failure: peek retry",
		  reconnect_heap_peek(&pool->reconnect, &entry), BRAID_OK);
	CHECK("immediate_watch_failure: retry attempt incremented",
	      entry.attempt == 3);
	CHECK("immediate_watch_failure: connection discarded",
	      find_live_conn(pool) == NULL);
	CHECK("immediate_watch_failure: events fired",
	      g_reconnect_event_count >= 1);
	CHECK("immediate_watch_failure: last event type reconnect attempt",
	      g_last_reconnect_event.type == BRAID_EV_RECONNECT_ATTEMPT);
	CHECK("immediate_watch_failure: event success==0",
	      g_last_reconnect_event.reconnect_attempt.success == 0);
	CHECK("immediate_watch_failure: event attempt matches",
	      g_last_reconnect_event.reconnect_attempt.attempt == 2);

	free_testpool(pool);
	reconnect_test_set_socket_create_hook(NULL);
}

/*
 * If io_watch(WRITE) fails for EINPROGRESS connect, reconnect_advance must
 * discard the connection and enqueue one retry with attempt incremented.
 */
static void
test_einprogress_watch_failure_discards_and_schedules_retry(void)
{
	braid_pool_t *pool;
	braid_reconnect_entry_t entry;

	reset_reconnect_counters();
	reconnect_test_set_socket_create_hook(
	    fake_socket_create_einprogress_watch_fail);
	pool = make_testpool(8, 0, "127.0.0.1", 8080, cb_reconnect_observe);
	if (pool == NULL) {
		CHECK("einprogress_watch_failure: alloc", 0);
		reconnect_test_set_socket_create_hook(NULL);
		return;
	}

	entry.attempt = 4;
	entry.next_retry_ms = 0;
	reconnect_heap_push(&pool->reconnect, entry);

	reconnect_advance(pool, 1000);

	CHECK("einprogress_watch_failure: retry enqueued",
	      pool->reconnect.count == 1);
	CHECK_ERR("einprogress_watch_failure: peek retry",
		  reconnect_heap_peek(&pool->reconnect, &entry), BRAID_OK);
	CHECK("einprogress_watch_failure: retry attempt incremented",
	      entry.attempt == 5);
	CHECK("einprogress_watch_failure: connection discarded",
	      find_live_conn(pool) == NULL);
	CHECK("einprogress_watch_failure: events fired",
	      g_reconnect_event_count >= 1);
	CHECK("einprogress_watch_failure: last event type reconnect attempt",
	      g_last_reconnect_event.type == BRAID_EV_RECONNECT_ATTEMPT);
	CHECK("einprogress_watch_failure: event success==0",
	      g_last_reconnect_event.reconnect_attempt.success == 0);
	CHECK("einprogress_watch_failure: event attempt matches",
	      g_last_reconnect_event.reconnect_attempt.attempt == 4);

	free_testpool(pool);
	reconnect_test_set_socket_create_hook(NULL);
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
	test_backoff_per_pool_prng_differs();
	test_max_attempts_N_stops();
	test_max_attempts_zero_retries_forever();
	test_reconnect_advance_fires_due();
	test_reconnect_advance_skips_future();
	test_reconnect_attempt_event_fired();
	test_reconnect_entry_inserted_only_on_failure();
	test_connect_zero_fast_path_reaches_idle_without_writable_event();
	test_immediate_watch_failure_discards_and_schedules_retry();
	test_einprogress_watch_failure_discards_and_schedules_retry();
}
