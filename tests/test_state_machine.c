/*
 * test_state_machine.c — unit tests for the connection state machine
 *
 * Tests: all legal transitions accepted, all illegal transitions rejected,
 * IDLE entry/exit timestamps and heap calls, CLOSING destroy_fn protocol,
 * deferred DEAD, table slot vacated on DEAD, observe_fn on DEAD.
 *
 * See TESTING.md §3.2 for the full test catalogue.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../include/braid.h"
#include "../src/braid_conn.h"
#include "../src/braid_internal.h"
#include "../src/braid_reaper.h"
#include "../src/braid_table.h"
#include "test_harness.h"

/* ── test callback state ─────────────────────────────────────────────── */

static int g_destroy_calls;
static int g_observe_calls;
static braid_event_type_t g_last_event;

static void
cb_destroy(int fd, void *ctx, void *hook)
{
	(void)fd;
	(void)ctx;
	(void)hook;
	g_destroy_calls++;
}

static void
cb_observe(const braid_event_t *ev, void *hook)
{
	(void)hook;
	g_last_event = ev->type;
	g_observe_calls++;
}

/* ── pool helpers ────────────────────────────────────────────────────── */

static braid_pool_t *
make_pool(uint32_t max)
{
	braid_pool_t *pool;

	pool = calloc(1, sizeof(*pool));
	if (pool == NULL)
		return NULL;
	pool->config.max_connections = max;
	pool->config.min_connections = 0;
	if (table_init(pool) != BRAID_OK) {
		free(pool);
		return NULL;
	}
	return pool;
}

static void
free_pool(braid_pool_t *pool)
{
	table_destroy(pool);
	free(pool);
}

/* Returns a fresh real fd (O_RDONLY on /dev/null). */
static int
make_fd(void)
{
	return open("/dev/null", O_RDONLY);
}

/* Reset all test counters before each test. */
static void
reset_counters(void)
{
	g_destroy_calls = 0;
	g_observe_calls = 0;
	g_last_event = 0;
	braid_test_reaper_insert_count = 0;
	braid_test_reaper_remove_count = 0;
}

/* ── state-advance helpers ───────────────────────────────────────────── */

/*
 * advance_to_idle — transition conn from CONNECTING to IDLE.
 * conn must have been returned by conn_alloc() (state = CONNECTING).
 */
static int
advance_to_idle(braid_pool_t *pool, braid_conn_t *conn)
{
	if (conn_transition(pool, conn, BRAID_STATE_INITIALIZING) != BRAID_OK)
		return BRAID_ERR_INVAL;
	if (conn_transition(pool, conn, BRAID_STATE_IDLE) != BRAID_OK)
		return BRAID_ERR_INVAL;
	return BRAID_OK;
}

/*
 * advance_to_active — transition conn from CONNECTING to ACTIVE.
 */
static int
advance_to_active(braid_pool_t *pool, braid_conn_t *conn)
{
	if (advance_to_idle(pool, conn) != BRAID_OK)
		return BRAID_ERR_INVAL;
	if (conn_transition(pool, conn, BRAID_STATE_ACTIVE) != BRAID_OK)
		return BRAID_ERR_INVAL;
	return BRAID_OK;
}

/* ── test cases ──────────────────────────────────────────────────────── */

/*
 * All 10 legal (from, to) pairs from ARCHITECTURE.md §4.2 return BRAID_OK.
 * Each sub-test uses a fresh pool and a fresh real fd.
 * Sub-tests ending at DEAD have their fd closed by the transition;
 * others need manual close before free_pool.
 */
static void
test_all_legal_transitions(void)
{
	braid_pool_t *pool;
	braid_conn_t *conn;
	int fd, rc;

	/* 1. CONNECTING → INITIALIZING */
	pool = make_pool(4);
	fd = make_fd();
	conn_alloc(pool, fd, &conn);
	rc = conn_transition(pool, conn, BRAID_STATE_INITIALIZING);
	CHECK_ERR("CONNECTING->INITIALIZING", rc, BRAID_OK);
	close(fd);
	free_pool(pool);

	/* 2. CONNECTING → DEAD */
	pool = make_pool(4);
	fd = make_fd();
	conn_alloc(pool, fd, &conn);
	rc = conn_transition(pool, conn, BRAID_STATE_DEAD);
	CHECK_ERR("CONNECTING->DEAD", rc, BRAID_OK);
	free_pool(pool);

	/* 3. INITIALIZING → IDLE */
	pool = make_pool(4);
	fd = make_fd();
	conn_alloc(pool, fd, &conn);
	conn_transition(pool, conn, BRAID_STATE_INITIALIZING);
	rc = conn_transition(pool, conn, BRAID_STATE_IDLE);
	CHECK_ERR("INITIALIZING->IDLE", rc, BRAID_OK);
	close(fd);
	free_pool(pool);

	/* 4. INITIALIZING → DEAD */
	pool = make_pool(4);
	fd = make_fd();
	conn_alloc(pool, fd, &conn);
	conn_transition(pool, conn, BRAID_STATE_INITIALIZING);
	rc = conn_transition(pool, conn, BRAID_STATE_DEAD);
	CHECK_ERR("INITIALIZING->DEAD", rc, BRAID_OK);
	free_pool(pool);

	/* 5. IDLE → ACTIVE */
	pool = make_pool(4);
	fd = make_fd();
	conn_alloc(pool, fd, &conn);
	advance_to_idle(pool, conn);
	rc = conn_transition(pool, conn, BRAID_STATE_ACTIVE);
	CHECK_ERR("IDLE->ACTIVE", rc, BRAID_OK);
	close(fd);
	free_pool(pool);

	/* 6. IDLE → CLOSING (auto-DEAD fires; fd closed by DEAD) */
	pool = make_pool(4);
	fd = make_fd();
	conn_alloc(pool, fd, &conn);
	advance_to_idle(pool, conn);
	rc = conn_transition(pool, conn, BRAID_STATE_CLOSING);
	CHECK_ERR("IDLE->CLOSING", rc, BRAID_OK);
	free_pool(pool);

	/* 7. ACTIVE → IDLE */
	pool = make_pool(4);
	fd = make_fd();
	conn_alloc(pool, fd, &conn);
	advance_to_active(pool, conn);
	rc = conn_transition(pool, conn, BRAID_STATE_IDLE);
	CHECK_ERR("ACTIVE->IDLE", rc, BRAID_OK);
	close(fd);
	free_pool(pool);

	/* 8. ACTIVE → CLOSING (auto-DEAD fires; fd closed by DEAD) */
	pool = make_pool(4);
	fd = make_fd();
	conn_alloc(pool, fd, &conn);
	advance_to_active(pool, conn);
	rc = conn_transition(pool, conn, BRAID_STATE_CLOSING);
	CHECK_ERR("ACTIVE->CLOSING", rc, BRAID_OK);
	free_pool(pool);

	/* 9. ACTIVE → DEAD */
	pool = make_pool(4);
	fd = make_fd();
	conn_alloc(pool, fd, &conn);
	advance_to_active(pool, conn);
	rc = conn_transition(pool, conn, BRAID_STATE_DEAD);
	CHECK_ERR("ACTIVE->DEAD", rc, BRAID_OK);
	free_pool(pool);

	/*
	 * 10. CLOSING → DEAD (direct, via deferred scenario)
	 * Set in_callback = 1 so CLOSING defers → state stays CLOSING;
	 * then trigger the explicit CLOSING→DEAD transition.
	 */
	pool = make_pool(4);
	fd = make_fd();
	conn_alloc(pool, fd, &conn);
	advance_to_active(pool, conn);
	pool->in_callback = 1;
	conn_transition(pool, conn, BRAID_STATE_CLOSING); /* deferred */
	pool->in_callback = 0;
	rc = conn_transition(pool, conn, BRAID_STATE_DEAD);
	CHECK_ERR("CLOSING->DEAD", rc, BRAID_OK);
	free_pool(pool);
}

/*
 * A representative subset of the 20 illegal (from, to) pairs causes
 * abort() in debug builds.  Each is verified by forking a child process
 * and checking that the child terminates with SIGABRT.
 * Covers all six source states.
 */
static void
test_all_illegal_transitions(void)
{
	static const struct {
		braid_state_t from;
		braid_state_t to;
		const char *name;
	} cases[] = {
	    {BRAID_STATE_CONNECTING, BRAID_STATE_IDLE, "CONNECTING->IDLE"},
	    {BRAID_STATE_CONNECTING, BRAID_STATE_ACTIVE, "CONNECTING->ACTIVE"},
	    {BRAID_STATE_INITIALIZING, BRAID_STATE_CONNECTING,
	     "INITIALIZING->CONNECTING"},
	    {BRAID_STATE_IDLE, BRAID_STATE_DEAD, "IDLE->DEAD"},
	    {BRAID_STATE_IDLE, BRAID_STATE_INITIALIZING, "IDLE->INITIALIZING"},
	    {BRAID_STATE_ACTIVE, BRAID_STATE_CONNECTING, "ACTIVE->CONNECTING"},
	    {BRAID_STATE_CLOSING, BRAID_STATE_IDLE, "CLOSING->IDLE"},
	    {BRAID_STATE_DEAD, BRAID_STATE_CONNECTING, "DEAD->CONNECTING"},
	};
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		pid_t pid;
		int status;

		pid = fork();
		if (pid == 0) {
			/* Child: suppress diagnostic, then attempt the illegal
			 * transition.  abort() from BRAID_DEBUG_ASSERT is
			 * expected; the parent verifies SIGABRT. */
			int devnull = open("/dev/null", O_WRONLY);
			if (devnull >= 0) {
				dup2(devnull, STDERR_FILENO);
				close(devnull);
			}

			braid_pool_t pool;
			braid_conn_t tmp, *conn;
			memset(&pool, 0, sizeof(pool));
			pool.config.max_connections = 4;
			table_init(&pool);
			memset(&tmp, 0, sizeof(tmp));
			tmp.fd = 42;
			conn = &tmp;
			table_insert(&pool, &conn);

			/*
			 * SAFETY: direct state write here is a test-only
			 * precondition setup in an ephemeral child process
			 * that will immediately abort.
			 */
			conn->state = cases[i].from;
			conn_transition(&pool, conn, cases[i].to);
			_exit(1); /* not reached on abort */
		}

		waitpid(pid, &status, 0);
		CHECK(cases[i].name,
		      WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
	}
}

/*
 * On IDLE entry, conn->last_active_ms must be written with the current
 * monotonic time from braid_now_ms().
 */
static void
test_idle_entry_sets_last_active_ms(void)
{
	braid_pool_t *pool;
	braid_conn_t *conn;
	int fd;

	pool = make_pool(4);
	fd = make_fd();
	conn_alloc(pool, fd, &conn);

	braid_test_clock_ms = 12345;
	conn_transition(pool, conn, BRAID_STATE_INITIALIZING);
	braid_test_clock_ms = 67890;
	conn_transition(pool, conn, BRAID_STATE_IDLE);

	CHECK("last_active_ms set on IDLE entry",
	      conn->last_active_ms == 67890);

	close(fd);
	free_pool(pool);
}

/*
 * On IDLE entry, reaper_heap_insert() is called exactly once.
 * Verified via the test-visible stub counter.
 */
static void
test_idle_entry_inserts_reaper_heap(void)
{
	braid_pool_t *pool;
	braid_conn_t *conn;
	int fd;

	pool = make_pool(4);
	fd = make_fd();
	reset_counters();

	conn_alloc(pool, fd, &conn);
	conn_transition(pool, conn, BRAID_STATE_INITIALIZING);
	conn_transition(pool, conn, BRAID_STATE_IDLE);

	CHECK("reaper_heap_insert called once on IDLE entry",
	      braid_test_reaper_insert_count == 1);

	close(fd);
	free_pool(pool);
}

/*
 * On IDLE exit, reaper_heap_remove() is called exactly once.
 * We transition to ACTIVE (which exits IDLE) and verify the counter.
 */
static void
test_idle_exit_removes_reaper_heap(void)
{
	braid_pool_t *pool;
	braid_conn_t *conn;
	int fd;

	pool = make_pool(4);
	fd = make_fd();
	reset_counters();

	conn_alloc(pool, fd, &conn);
	advance_to_active(pool, conn); /* IDLE entry then IDLE exit */

	CHECK("reaper_heap_remove called once on IDLE exit",
	      braid_test_reaper_remove_count == 1);

	close(fd);
	free_pool(pool);
}

/*
 * On CONNECTING entry (via conn_alloc), conn->created_at_ms must be set
 * to the current monotonic time.  CONNECTING is the initial state
 * established by conn_alloc() — the invariant is set there.
 */
static void
test_connecting_entry_sets_created_at_ms(void)
{
	braid_pool_t *pool;
	braid_conn_t *conn;
	int fd;

	pool = make_pool(4);
	braid_test_clock_ms = 999;
	fd = make_fd();
	conn_alloc(pool, fd, &conn);

	CHECK("created_at_ms set on CONNECTING entry",
	      conn->created_at_ms == 999);

	close(fd);
	free_pool(pool);
}

/*
 * On CLOSING entry, destroy_fn is invoked exactly once via the
 * in_callback protocol, before the automatic transition to DEAD.
 */
static void
test_closing_calls_destroy_fn(void)
{
	braid_pool_t *pool;
	braid_conn_t *conn;
	int fd;

	pool = make_pool(4);
	pool->config.destroy_fn = cb_destroy;
	g_destroy_calls = 0;
	fd = make_fd();

	conn_alloc(pool, fd, &conn);
	advance_to_active(pool, conn);
	conn_transition(pool, conn,
			BRAID_STATE_CLOSING); /* auto-DEAD; fd closed */

	CHECK("destroy_fn called exactly once", g_destroy_calls == 1);

	free_pool(pool);
}

/*
 * After CLOSING → DEAD, the fd must be closed.
 * Verified by checking fcntl() returns EBADF on the old fd value.
 */
static void
test_closing_dead_closes_fd(void)
{
	braid_pool_t *pool;
	braid_conn_t *conn;
	int fd;

	pool = make_pool(4);
	fd = make_fd();

	conn_alloc(pool, fd, &conn);
	advance_to_active(pool, conn);
	conn_transition(pool, conn,
			BRAID_STATE_CLOSING); /* auto-DEAD closes fd */

	CHECK("fd closed after CLOSING->DEAD",
	      fcntl(fd, F_GETFD) == -1 && errno == EBADF);

	free_pool(pool);
}

/*
 * When CLOSING is called while pool->in_callback > 0 (the connection is
 * being closed from within a callback), the DEAD transition must be deferred:
 * CONN_FLAG_CLOSING_DEFERRED is set, state stays CLOSING, and the fd is
 * not yet closed.  destroy_fn IS called in CLOSING (with in_callback
 * protocol) regardless of deferral — only the DEAD step is deferred.
 * See ARCHITECTURE.md §4.3, CODING_STANDARDS.md §4.1.
 */
static void
test_closing_deferred_when_in_callback(void)
{
	braid_pool_t *pool;
	braid_conn_t *conn;
	int fd;

	pool = make_pool(4);
	pool->config.destroy_fn = cb_destroy;
	g_destroy_calls = 0;
	fd = make_fd();

	pool->in_callback = 1; /* simulate inside a checkout callback */
	conn_alloc(pool, fd, &conn);
	advance_to_active(pool, conn);
	conn_transition(pool, conn, BRAID_STATE_CLOSING);

	CHECK("state is CLOSING (not DEAD) while deferred",
	      conn->state == BRAID_STATE_CLOSING);
	CHECK("CONN_FLAG_CLOSING_DEFERRED set",
	      conn->flags & CONN_FLAG_CLOSING_DEFERRED);
	CHECK("destroy_fn called in CLOSING (regardless of deferral)",
	      g_destroy_calls == 1);
	CHECK("fd still open while deferred", fcntl(fd, F_GETFD) != -1);

	/* Clean up: simulate callback returning */
	pool->in_callback = 0;
	conn_transition(pool, conn, BRAID_STATE_DEAD); /* fd closed */
	free_pool(pool);
}

/*
 * After the deferred DEAD transition fires (simulating pool_drain_deferred),
 * the fd must be closed and the table slot must be vacated.
 */
static void
test_deferred_close_fires_after_callback(void)
{
	braid_pool_t *pool;
	braid_conn_t *conn, *found;
	int fd;

	pool = make_pool(4);
	fd = make_fd();

	pool->in_callback = 1;
	conn_alloc(pool, fd, &conn);
	advance_to_active(pool, conn);
	conn_transition(pool, conn, BRAID_STATE_CLOSING); /* deferred */

	CHECK("fd open before drain", fcntl(fd, F_GETFD) != -1);

	/* Simulate drain_deferred: in_callback reaches 0, DEAD fires. */
	pool->in_callback = 0;
	conn_transition(pool, conn, BRAID_STATE_DEAD);

	CHECK("fd closed after drain",
	      fcntl(fd, F_GETFD) == -1 && errno == EBADF);
	CHECK("table slot vacated after drain",
	      table_lookup(pool, fd, &found) == BRAID_ERR_INVAL);

	free_pool(pool);
}

/*
 * On DEAD entry, table_delete() must be called so that a subsequent
 * table_lookup() for that fd returns not-found.
 */
static void
test_dead_vacates_table_slot(void)
{
	braid_pool_t *pool;
	braid_conn_t *conn, *found;
	int fd;

	pool = make_pool(4);
	fd = make_fd();

	conn_alloc(pool, fd, &conn);
	CHECK("slot present before DEAD",
	      table_lookup(pool, fd, &found) == BRAID_OK);

	conn_transition(pool, conn, BRAID_STATE_DEAD); /* fd closed */

	CHECK("slot vacated after DEAD",
	      table_lookup(pool, fd, &found) == BRAID_ERR_INVAL);

	free_pool(pool);
}

/*
 * On DEAD entry, observe_fn must be called with BRAID_EV_CONN_DESTROYED.
 */
static void
test_dead_fires_conn_destroyed_event(void)
{
	braid_pool_t *pool;
	braid_conn_t *conn;
	int fd;

	pool = make_pool(4);
	pool->config.observe_fn = cb_observe;
	g_observe_calls = 0;
	fd = make_fd();

	conn_alloc(pool, fd, &conn);
	conn_transition(pool, conn, BRAID_STATE_DEAD); /* fd closed */

	CHECK("observe_fn called on DEAD", g_observe_calls == 1);
	CHECK("event type is BRAID_EV_CONN_DESTROYED",
	      g_last_event == BRAID_EV_CONN_DESTROYED);

	free_pool(pool);
}

/* ── suite entry point ───────────────────────────────────────────────── */

void
run_state_machine_tests(void)
{
	test_all_legal_transitions();
	test_all_illegal_transitions();
	test_idle_entry_sets_last_active_ms();
	test_idle_entry_inserts_reaper_heap();
	test_idle_exit_removes_reaper_heap();
	test_connecting_entry_sets_created_at_ms();
	test_closing_calls_destroy_fn();
	test_closing_dead_closes_fd();
	test_closing_deferred_when_in_callback();
	test_deferred_close_fires_after_callback();
	test_dead_vacates_table_slot();
	test_dead_fires_conn_destroyed_event();
}
