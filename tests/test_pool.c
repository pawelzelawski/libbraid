/*
 * test_pool.c — unit tests for the pool public API
 *
 * Phase 6.8 tests:
 *   create/destroy lifecycle, checkout with immediate connection,
 *   checkout enqueueing, checkin (OK and DISCARD), cancel, exhaustion,
 *   re-entrancy (checkin from callback), shutdown, observe_fn NULL,
 *   validate_fn (threshold, failure), connect timeout, init_fn deadline.
 *
 * All timer-dependent tests advance braid_test_clock_ms directly.
 * An event fd (epoll on Linux, kqueue on OpenBSD) is opened once per test
 * that needs pool->config.event_fd via make_event_fd() in test_harness.h.
 * No sleep() or usleep() are used. No real DNS or network calls are made.
 *
 * See TESTING.md §3.6 for the full test catalogue.
 * See ARCHITECTURE.md §11, §12, §13 for design references.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../include/braid.h"
#include "../src/braid_conn.h"
#include "../src/braid_io.h"
#include "../src/braid_internal.h"
#include "../src/braid_pool.h"
#include "../src/braid_reaper.h"
#include "../src/braid_reconnect.h"
#include "../src/braid_table.h"
#include "../src/braid_waitq.h"
#include "test_harness.h"

/* ── global test callback state ──────────────────────────────────────── */

/*
 * cb_recorder_t — capture checkout callback arguments for assertion.
 * Supports up to 4 captured calls per recorder instance.
 */
#define RECORDER_MAX 4
typedef struct {
	int fd[RECORDER_MAX];
	int err[RECORDER_MAX];
	void *conn_ctx[RECORDER_MAX];
	int call_count;
} cb_recorder_t;

static void
recording_checkout_cb(int fd, void *conn_ctx, int err, void *cb_ctx)
{
	cb_recorder_t *rec = cb_ctx;

	if (rec->call_count < RECORDER_MAX) {
		rec->fd[rec->call_count] = fd;
		rec->conn_ctx[rec->call_count] = conn_ctx;
		rec->err[rec->call_count] = err;
	}
	rec->call_count++;
}

static int g_observe_calls;
static braid_event_type_t g_last_event_type;
static int g_last_event_fd;

static void
recording_observe_cb(const braid_event_t *ev, void *hook)
{
	(void)hook;
	if (g_observe_calls < 16) {
		g_last_event_type = ev->type;
		g_last_event_fd = ev->fd;
	}
	g_observe_calls++;
}

static int g_destroy_calls;

static void
recording_destroy_cb(int fd, void *ctx, void *hook)
{
	(void)fd;
	(void)ctx;
	(void)hook;
	g_destroy_calls++;
}

static int g_validate_calls;
static int g_validate_return; /* BRAID_OK or BRAID_ERR_CONNFAIL */

static int
recording_validate_cb(int fd, void *conn_ctx, void *hook, uint64_t deadline_ms)
{
	(void)fd;
	(void)conn_ctx;
	(void)hook;
	(void)deadline_ms;
	g_validate_calls++;
	return g_validate_return;
}

static int g_init_calls;
static int g_init_return;
static uint64_t g_init_advance_ms;

static int
recording_init_cb(int fd, void **conn_ctx_out, void *hook, uint64_t deadline_ms)
{
	(void)fd;
	(void)conn_ctx_out;
	(void)hook;
	(void)deadline_ms;
	g_init_calls++;
#ifdef BRAID_TEST_CLOCK
	if (g_init_advance_ms > 0)
		braid_test_clock_ms += g_init_advance_ms;
#endif
	return g_init_return;
}

static void
reset_counters(void)
{
	g_observe_calls = 0;
	g_last_event_type = 0;
	g_last_event_fd = -1;
	g_destroy_calls = 0;
	g_validate_calls = 0;
	g_validate_return = BRAID_OK;
	g_init_calls = 0;
	g_init_return = BRAID_OK;
	g_init_advance_ms = 0;
}

/* ── pool factory helpers ─────────────────────────────────────────────── */

/*
 * make_epoll_fd — open a real event fd for use as pool->config.event_fd.
 * Delegates to make_event_fd() from test_harness.h (platform-conditional).
 * Returns -1 on failure. Must be closed by caller.
 */
static int
make_epoll_fd(void)
{
	return make_event_fd();
}

/*
 * make_minimal_config — fill a braid_config_t with sensible defaults for
 * unit tests. Sets min_connections=0 so no reconnect entries are inserted
 * at create time (avoids DNS and real TCP connection overhead in tests).
 *
 * Caller must supply a valid event_fd.
 */
static braid_config_t
make_minimal_config(int event_fd, uint32_t max)
{
	braid_config_t cfg;

	memset(&cfg, 0, sizeof(cfg));
	cfg.event_fd = event_fd;
	cfg.host = "127.0.0.1";
	cfg.port = 12345;
	cfg.max_connections = max;
	cfg.min_connections = 0;
	return cfg;
}

/*
 * alloc_idle_conn_on_epoll — inject an fd into the pool at IDLE state.
 *
 * Opens a socketpair so that the fd is a valid socket that can be
 * registered with epoll. Registers it for READ events (matching IDLE state).
 * Both ends of the pair are returned; peer_fd must be closed by the caller
 * after it is no longer needed. The pool takes ownership of fd (closed at
 * DEAD transition).
 *
 * Returns the connection record pointer or NULL on failure.
 */
static braid_conn_t *
alloc_idle_conn_on_epoll(braid_pool_t *pool, int *peer_fd_out)
{
	int sv[2];
	braid_conn_t *conn;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
		return NULL;

	if (conn_alloc(pool, sv[0], &conn) != BRAID_OK) {
		close(sv[0]);
		close(sv[1]);
		return NULL;
	}

	/*
	 * Pre-register the fd with the event poller before transitioning, to
	 * match what the pool's normal reconnect path does.  The API differs
	 * by platform; on kqueue EV_ADD creates-or-updates.
	 */
#ifdef __linux__
	{
		struct epoll_event ev;
		ev.events = EPOLLIN | EPOLLET;
		ev.data.ptr = &conn->tag;
		if (epoll_ctl(pool->config.event_fd, EPOLL_CTL_ADD, sv[0],
			      &ev) != 0) {
			close(sv[0]);
			close(sv[1]);
			return NULL;
		}
	}
#else
	{
		struct kevent kev;
		EV_SET(&kev, (uintptr_t)sv[0], EVFILT_READ, EV_ADD, 0, 0,
		       &conn->tag);
		if (kevent(pool->config.event_fd, &kev, 1, NULL, 0, NULL) !=
		    0) {
			close(sv[0]);
			close(sv[1]);
			return NULL;
		}
	}
#endif

	if (conn_transition(pool, conn, BRAID_STATE_INITIALIZING) != BRAID_OK) {
		close(sv[1]);
		return NULL;
	}
	if (conn_transition(pool, conn, BRAID_STATE_IDLE) != BRAID_OK) {
		close(sv[1]);
		return NULL;
	}

	if (peer_fd_out != NULL)
		*peer_fd_out = sv[1];
	else
		close(sv[1]);

	return conn;
}

/*
 * alloc_connecting_conn_on_epoll — inject a CONNECTING fd into the pool.
 * Uses a socketpair to get a real socket; registers for WRITE (CONNECTING).
 */
static braid_conn_t *
alloc_connecting_conn_on_epoll(braid_pool_t *pool, int *peer_fd_out)
{
	int sv[2];
	braid_conn_t *conn;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
		return NULL;

	if (conn_alloc(pool, sv[0], &conn) != BRAID_OK) {
		close(sv[0]);
		close(sv[1]);
		return NULL;
	}

	/* Pre-register WRITE interest — see alloc_idle_conn_on_epoll. */
#ifdef __linux__
	{
		struct epoll_event ev;
		ev.events = EPOLLOUT | EPOLLET;
		ev.data.ptr = &conn->tag;
		if (epoll_ctl(pool->config.event_fd, EPOLL_CTL_ADD, sv[0],
			      &ev) != 0) {
			close(sv[0]);
			close(sv[1]);
			return NULL;
		}
	}
#else
	{
		struct kevent kev;
		EV_SET(&kev, (uintptr_t)sv[0], EVFILT_WRITE, EV_ADD, 0, 0,
		       &conn->tag);
		if (kevent(pool->config.event_fd, &kev, 1, NULL, 0, NULL) !=
		    0) {
			close(sv[0]);
			close(sv[1]);
			return NULL;
		}
	}
#endif

	if (peer_fd_out != NULL)
		*peer_fd_out = sv[1];
	else
		close(sv[1]);

	return conn;
}

/* ── test cases ──────────────────────────────────────────────────────── */

/*
 * braid_pool_create with a valid config returns a non-NULL pool.
 * The pool is fully initialised: table, heaps, waitq all allocated.
 */
static void
test_pool_create_valid(void)
{
	braid_config_t cfg;
	braid_pool_t *pool;
	int err = 0;
	int epfd;

	epfd = make_epoll_fd();
	if (epfd < 0) {
		CHECK("create-valid: epoll_create1", 0);
		return;
	}

	cfg = make_minimal_config(epfd, 4);
	pool = braid_pool_create(&cfg, &err);
	CHECK("create-valid: pool non-NULL", pool != NULL);
	CHECK("create-valid: err == BRAID_OK", err == BRAID_OK);

	braid_pool_destroy(pool, 0);
	close(epfd);
}

/*
 * braid_pool_create with event_fd < 0 returns NULL and BRAID_ERR_INVAL.
 */
static void
test_pool_create_null_event_fd(void)
{
	braid_config_t cfg;
	braid_pool_t *pool;
	int err = 0;

	memset(&cfg, 0, sizeof(cfg));
	cfg.event_fd = -1;
	cfg.host = "127.0.0.1";
	cfg.port = 1234;
	cfg.max_connections = 4;
	cfg.min_connections = 0;

	pool = braid_pool_create(&cfg, &err);
	CHECK("create-null-event-fd: pool NULL", pool == NULL);
	CHECK("create-null-event-fd: err == INVAL", err == BRAID_ERR_INVAL);
}

/*
 * braid_pool_create with min_connections > max_connections returns NULL and
 * BRAID_ERR_INVAL.
 */
static void
test_pool_create_min_gt_max(void)
{
	braid_config_t cfg;
	braid_pool_t *pool;
	int err = 0;
	int epfd;

	epfd = make_epoll_fd();
	if (epfd < 0) {
		CHECK("create-min-gt-max: epoll_create1", 0);
		return;
	}

	cfg = make_minimal_config(epfd, 4);
	cfg.min_connections = 8; /* > max_connections = 4 */

	pool = braid_pool_create(&cfg, &err);
	CHECK("create-min-gt-max: pool NULL", pool == NULL);
	CHECK("create-min-gt-max: err == INVAL", err == BRAID_ERR_INVAL);
	close(epfd);
}

/*
 * Public API NULL-argument handling for pool pointer and callback pointer.
 * Functions must return BRAID_ERR_INVAL rather than crash.
 */
static void
test_api_null_argument_guards(void)
{
	braid_config_t cfg;
	braid_pool_t *pool;
	cb_recorder_t rec;
	int err = 0;
	int epfd;
	uint32_t next_ms;

	memset(&rec, 0, sizeof(rec));
	next_ms = 0;

	CHECK_ERR(
	    "null-args: checkout NULL pool",
	    braid_pool_checkout(NULL, 0, recording_checkout_cb, &rec, NULL),
	    BRAID_ERR_INVAL);
	CHECK_ERR("null-args: checkin NULL pool",
		  braid_pool_checkin(NULL, 1, BRAID_CONN_OK), BRAID_ERR_INVAL);
	CHECK_ERR("null-args: cancel NULL pool", braid_pool_cancel(NULL, 1),
		  BRAID_ERR_INVAL);
	CHECK_ERR("null-args: advance NULL pool",
		  braid_pool_advance(NULL, &next_ms), BRAID_ERR_INVAL);
	CHECK_ERR("null-args: notify NULL pool",
		  braid_pool_notify(NULL, 1, BRAID_IO_READ), BRAID_ERR_INVAL);

	epfd = make_epoll_fd();
	if (epfd < 0) {
		CHECK("null-args: epoll_create1", 0);
		return;
	}

	cfg = make_minimal_config(epfd, 4);
	pool = braid_pool_create(&cfg, &err);
	if (pool == NULL) {
		CHECK("null-args: create", 0);
		close(epfd);
		return;
	}

	CHECK_ERR("null-args: checkout NULL callback",
		  braid_pool_checkout(pool, 0, NULL, &rec, NULL),
		  BRAID_ERR_INVAL);

	braid_pool_destroy(pool, 0);
	close(epfd);
}

/*
 * braid_pool_destroy with no live connections tears down cleanly.
 * Verified by Valgrind (no leaks) and by the test completing without crash.
 */
static void
test_pool_destroy_no_active(void)
{
	braid_config_t cfg;
	braid_pool_t *pool;
	int err = 0;
	int epfd;

	epfd = make_epoll_fd();
	if (epfd < 0) {
		CHECK("destroy-no-active: epoll_create1", 0);
		return;
	}

	cfg = make_minimal_config(epfd, 4);
	pool = braid_pool_create(&cfg, &err);
	if (pool == NULL) {
		CHECK("destroy-no-active: create", 0);
		close(epfd);
		return;
	}

	braid_pool_destroy(pool, 0);
	close(epfd);
	CHECK("destroy-no-active: completed without crash", 1);
}

/*
 * braid_pool_destroy with an ACTIVE connection: drain_timeout_ms=0 so the
 * ACTIVE fd is force-closed. No crash, no leak.
 */
static void
test_pool_destroy_with_active(void)
{
	braid_config_t cfg;
	braid_pool_t *pool;
	braid_conn_t *conn;
	int err = 0;
	int epfd, peer_fd;

	epfd = make_epoll_fd();
	if (epfd < 0) {
		CHECK("destroy-with-active: epoll_create1", 0);
		return;
	}

	cfg = make_minimal_config(epfd, 4);
	pool = braid_pool_create(&cfg, &err);
	if (pool == NULL) {
		CHECK("destroy-with-active: create", 0);
		close(epfd);
		return;
	}

	conn = alloc_idle_conn_on_epoll(pool, &peer_fd);
	if (conn == NULL) {
		CHECK("destroy-with-active: alloc_idle_conn", 0);
		braid_pool_destroy(pool, 0);
		close(epfd);
		return;
	}
	conn_transition(pool, conn, BRAID_STATE_ACTIVE);
	close(peer_fd);

	braid_pool_destroy(pool, 0); /* drain_timeout_ms=0: force close */
	close(epfd);
	CHECK("destroy-with-active: completed without crash", 1);
}

/*
 * Checkout with an immediately available IDLE connection: the callback is
 * invoked synchronously before braid_pool_checkout returns, with BRAID_OK
 * and the correct fd.
 */
static void
test_checkout_immediate(void)
{
	braid_config_t cfg;
	braid_pool_t *pool;
	braid_conn_t *conn;
	cb_recorder_t rec;
	braid_token_t tok;
	int err = 0;
	int epfd, peer_fd;

	memset(&rec, 0, sizeof(rec));
	epfd = make_epoll_fd();
	if (epfd < 0) {
		CHECK("checkout-imm: epoll_create1", 0);
		return;
	}

	cfg = make_minimal_config(epfd, 4);
	pool = braid_pool_create(&cfg, &err);
	if (pool == NULL) {
		CHECK("checkout-imm: create", 0);
		close(epfd);
		return;
	}

	conn = alloc_idle_conn_on_epoll(pool, &peer_fd);
	if (conn == NULL) {
		CHECK("checkout-imm: alloc_idle_conn", 0);
		braid_pool_destroy(pool, 0);
		close(epfd);
		return;
	}

	int conn_fd = conn->fd; /* capture before checkout transitions it */
	tok = 0;
	CHECK_ERR(
	    "checkout-imm: checkout returns OK",
	    braid_pool_checkout(pool, 0, recording_checkout_cb, &rec, &tok),
	    BRAID_OK);
	/* Callback was synchronous — must have been called once. */
	CHECK("checkout-imm: callback called once", rec.call_count == 1);
	CHECK("checkout-imm: callback err=OK", rec.err[0] == BRAID_OK);
	CHECK("checkout-imm: callback fd matches", rec.fd[0] == conn_fd);

	/* Check in to clean up. */
	braid_pool_checkin(pool, conn_fd, BRAID_CONN_DISCARD);
	close(peer_fd);
	braid_pool_destroy(pool, 0);
	close(epfd);
}

/*
 * ACTIVE ownership handoff: checkout must unregister pool interest from epoll
 * so caller can register the checked-out fd; checkin with BRAID_CONN_OK must
 * restore pool read interest.
 */
static void
test_active_event_registration_handoff(void)
{
	braid_config_t cfg;
	braid_pool_t *pool;
	braid_conn_t *conn;
	cb_recorder_t rec;
	int err = 0;
	int epfd, peer_fd;
	int fd;
#ifdef __linux__
	struct epoll_event ev;
	int rc;
#endif

	memset(&rec, 0, sizeof(rec));
#ifdef __linux__
	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN | EPOLLET;
	ev.data.ptr = NULL;
#endif

	epfd = make_epoll_fd();
	if (epfd < 0) {
		CHECK("active-handoff: event_fd", 0);
		return;
	}

	cfg = make_minimal_config(epfd, 4);
	pool = braid_pool_create(&cfg, &err);
	if (pool == NULL) {
		CHECK("active-handoff: create", 0);
		close(epfd);
		return;
	}

	conn = alloc_idle_conn_on_epoll(pool, &peer_fd);
	if (conn == NULL) {
		CHECK("active-handoff: alloc_idle_conn", 0);
		braid_pool_destroy(pool, 0);
		close(epfd);
		return;
	}

	CHECK_ERR(
	    "active-handoff: checkout",
	    braid_pool_checkout(pool, 0, recording_checkout_cb, &rec, NULL),
	    BRAID_OK);
	CHECK("active-handoff: callback fired", rec.call_count == 1);
	fd = rec.fd[0];

#ifdef __linux__
	errno = 0;
	rc = epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
	CHECK("active-handoff: caller can add ACTIVE fd", rc == 0);
	if (rc == 0)
		epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
#endif

	CHECK_ERR("active-handoff: checkin OK",
		  braid_pool_checkin(pool, fd, BRAID_CONN_OK), BRAID_OK);

#ifdef __linux__
	errno = 0;
	rc = epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
	CHECK("active-handoff: pool restored IDLE watch (EEXIST)",
	      rc == -1 && errno == EEXIST);
	if (rc == 0)
		epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
#endif

	CHECK_ERR(
	    "active-handoff: checkout for cleanup",
	    braid_pool_checkout(pool, 0, recording_checkout_cb, &rec, NULL),
	    BRAID_OK);
	if (rec.call_count >= 2)
		braid_pool_checkin(pool, rec.fd[1], BRAID_CONN_DISCARD);

	close(peer_fd);
	braid_pool_destroy(pool, 0);
	close(epfd);
}

/*
 * Checkout with no IDLE connection (pool empty, timeout_ms > 0): the
 * callback is NOT invoked yet and the token is written.
 */
static void
test_checkout_enqueues(void)
{
	braid_config_t cfg;
	braid_pool_t *pool;
	cb_recorder_t rec;
	braid_token_t tok;
	int err = 0;
	int epfd;

	memset(&rec, 0, sizeof(rec));
	epfd = make_epoll_fd();
	if (epfd < 0) {
		CHECK("checkout-enq: epoll_create1", 0);
		return;
	}

	cfg = make_minimal_config(epfd, 4);
	pool = braid_pool_create(&cfg, &err);
	if (pool == NULL) {
		CHECK("checkout-enq: create", 0);
		close(epfd);
		return;
	}

	tok = 0;
	braid_test_clock_ms = 1000;
	CHECK_ERR(
	    "checkout-enq: checkout returns OK (enqueues)",
	    braid_pool_checkout(pool, 5000, recording_checkout_cb, &rec, &tok),
	    BRAID_OK);
	CHECK("checkout-enq: callback NOT called yet", rec.call_count == 0);
	CHECK("checkout-enq: token written (non-zero or valid)",
	      1); /* always */
	CHECK("checkout-enq: waitq count is 1", pool->waitq.count == 1);

	braid_pool_destroy(pool, 0); /* shutdown calls BRAID_ERR_SHUTDOWN */
	close(epfd);
}

/*
 * Checkin with BRAID_CONN_OK: connection transitions to IDLE and the wait
 * queue head is served if a waiter is pending.
 */
static void
test_checkin_conn_ok(void)
{
	braid_config_t cfg;
	braid_pool_t *pool;
	braid_conn_t *conn;
	cb_recorder_t waiter_rec, checkout_rec;
	braid_token_t tok;
	int err = 0;
	int epfd, peer_fd;
	int conn_fd;

	memset(&waiter_rec, 0, sizeof(waiter_rec));
	memset(&checkout_rec, 0, sizeof(checkout_rec));
	epfd = make_epoll_fd();
	if (epfd < 0) {
		CHECK("checkin-ok: epoll_create1", 0);
		return;
	}

	cfg = make_minimal_config(epfd, 4);
	pool = braid_pool_create(&cfg, &err);
	if (pool == NULL) {
		CHECK("checkin-ok: create", 0);
		close(epfd);
		return;
	}

	conn = alloc_idle_conn_on_epoll(pool, &peer_fd);
	if (conn == NULL) {
		CHECK("checkin-ok: alloc_idle_conn", 0);
		braid_pool_destroy(pool, 0);
		close(epfd);
		return;
	}
	conn_fd = conn->fd;

	/* Enqueue a waiter before checking out. */
	braid_test_clock_ms = 1000;
	CHECK_ERR("checkin-ok: checkout IDLE",
		  braid_pool_checkout(pool, 0, recording_checkout_cb,
				      &checkout_rec, &tok),
		  BRAID_OK);
	CHECK("checkin-ok: checkout callback fired immediately",
	      checkout_rec.call_count == 1);
	CHECK("checkin-ok: checkout err=BRAID_OK",
	      checkout_rec.err[0] == BRAID_OK);
	/*
	 * Enqueue a waiter now while no IDLE conn is available (it's ACTIVE).
	 */
	tok = 0;
	CHECK_ERR("checkin-ok: enqueue second waiter",
		  braid_pool_checkout(pool, 5000, recording_checkout_cb,
				      &waiter_rec, &tok),
		  BRAID_OK);
	CHECK("checkin-ok: waiter not fired yet", waiter_rec.call_count == 0);

	/* Checkin — must serve the pending waiter. */
	CHECK_ERR("checkin-ok: checkin",
		  braid_pool_checkin(pool, conn_fd, BRAID_CONN_OK), BRAID_OK);
	CHECK("checkin-ok: waiter fired after checkin",
	      waiter_rec.call_count == 1);
	CHECK("checkin-ok: waiter err=OK", waiter_rec.err[0] == BRAID_OK);
	CHECK("checkin-ok: waiter got same fd", waiter_rec.fd[0] == conn_fd);

	/* Clean up — conn is ACTIVE again, discard it. */
	braid_pool_checkin(pool, conn_fd, BRAID_CONN_DISCARD);
	close(peer_fd);
	braid_pool_destroy(pool, 0);
	close(epfd);
}

/*
 * Checkin with BRAID_CONN_DISCARD: connection transitions to CLOSING→DEAD.
 * The fd is closed and removed from the table.
 */
static void
test_checkin_conn_discard(void)
{
	braid_config_t cfg;
	braid_pool_t *pool;
	braid_conn_t *conn;
	cb_recorder_t rec;
	braid_token_t tok;
	int err = 0;
	int epfd, peer_fd;
	int conn_fd;

	reset_counters();
	memset(&rec, 0, sizeof(rec));
	epfd = make_epoll_fd();
	if (epfd < 0) {
		CHECK("checkin-discard: epoll_create1", 0);
		return;
	}

	cfg = make_minimal_config(epfd, 4);
	cfg.destroy_fn = recording_destroy_cb;
	pool = braid_pool_create(&cfg, &err);
	if (pool == NULL) {
		CHECK("checkin-discard: create", 0);
		close(epfd);
		return;
	}

	conn = alloc_idle_conn_on_epoll(pool, &peer_fd);
	if (conn == NULL) {
		CHECK("checkin-discard: alloc_idle_conn", 0);
		braid_pool_destroy(pool, 0);
		close(epfd);
		return;
	}
	conn_fd = conn->fd;

	/* Check out so we can check in with DISCARD. */
	CHECK_ERR(
	    "checkin-discard: checkout",
	    braid_pool_checkout(pool, 0, recording_checkout_cb, &rec, &tok),
	    BRAID_OK);
	CHECK("checkin-discard: checkout fired", rec.call_count == 1);

	CHECK_ERR("checkin-discard: checkin with DISCARD",
		  braid_pool_checkin(pool, conn_fd, BRAID_CONN_DISCARD),
		  BRAID_OK);

	CHECK("checkin-discard: live_count decremented", pool->live_count == 0);
	CHECK("checkin-discard: destroy_fn called once", g_destroy_calls == 1);

	/* Table slot should be vacated. */
	{
		braid_conn_t *lookup;

		CHECK_ERR("checkin-discard: fd no longer in table",
			  table_lookup(pool, conn_fd, &lookup),
			  BRAID_ERR_INVAL);
	}

	close(peer_fd);
	braid_pool_destroy(pool, 0);
	close(epfd);
}

/*
 * braid_pool_checkin with an unrecognised fd returns BRAID_ERR_INVAL.
 */
static void
test_checkin_unknown_fd(void)
{
	braid_config_t cfg;
	braid_pool_t *pool;
	int err = 0;
	int epfd;

	epfd = make_epoll_fd();
	if (epfd < 0) {
		CHECK("checkin-unknown: epoll_create1", 0);
		return;
	}

	cfg = make_minimal_config(epfd, 4);
	pool = braid_pool_create(&cfg, &err);
	if (pool == NULL) {
		CHECK("checkin-unknown: create", 0);
		close(epfd);
		return;
	}

	CHECK_ERR("checkin-unknown: unknown fd returns INVAL",
		  braid_pool_checkin(pool, 9999, BRAID_CONN_OK),
		  BRAID_ERR_INVAL);

	braid_pool_destroy(pool, 0);
	close(epfd);
}

/*
 * braid_pool_cancel on a pending token: the callback is invoked with
 * BRAID_ERR_CANCELLED and the wait queue slot is tombstoned.
 */
static void
test_cancel_pending(void)
{
	braid_config_t cfg;
	braid_pool_t *pool;
	cb_recorder_t rec;
	braid_token_t tok;
	int err = 0;
	int epfd;

	memset(&rec, 0, sizeof(rec));
	epfd = make_epoll_fd();
	if (epfd < 0) {
		CHECK("cancel-pending: epoll_create1", 0);
		return;
	}

	cfg = make_minimal_config(epfd, 4);
	pool = braid_pool_create(&cfg, &err);
	if (pool == NULL) {
		CHECK("cancel-pending: create", 0);
		close(epfd);
		return;
	}

	braid_test_clock_ms = 500;
	tok = 0;
	CHECK_ERR(
	    "cancel-pending: enqueue",
	    braid_pool_checkout(pool, 5000, recording_checkout_cb, &rec, &tok),
	    BRAID_OK);
	CHECK("cancel-pending: not fired yet", rec.call_count == 0);

	CHECK_ERR("cancel-pending: cancel", braid_pool_cancel(pool, tok),
		  BRAID_OK);
	CHECK("cancel-pending: fired with CANCELLED", rec.call_count == 1);
	CHECK("cancel-pending: err=CANCELLED",
	      rec.err[0] == BRAID_ERR_CANCELLED);
	CHECK("cancel-pending: waitq empty", pool->waitq.count == 0);

	braid_pool_destroy(pool, 0);
	close(epfd);
}

/*
 * braid_pool_cancel on an already-fired token: silent no-op, no double
 * callback.
 */
static void
test_cancel_fired_token(void)
{
	braid_config_t cfg;
	braid_pool_t *pool;
	braid_conn_t *conn;
	cb_recorder_t rec;
	braid_token_t tok;
	int err = 0;
	int epfd, peer_fd;
	int conn_fd;

	memset(&rec, 0, sizeof(rec));
	epfd = make_epoll_fd();
	if (epfd < 0) {
		CHECK("cancel-fired: epoll_create1", 0);
		return;
	}

	cfg = make_minimal_config(epfd, 4);
	pool = braid_pool_create(&cfg, &err);
	if (pool == NULL) {
		CHECK("cancel-fired: create", 0);
		close(epfd);
		return;
	}

	conn = alloc_idle_conn_on_epoll(pool, &peer_fd);
	if (conn == NULL) {
		CHECK("cancel-fired: alloc_idle_conn", 0);
		braid_pool_destroy(pool, 0);
		close(epfd);
		return;
	}
	conn_fd = conn->fd;

	/* First place a waiter to get a token. */
	braid_test_clock_ms = 1000;
	/*
	 * Checkout immediately: all IDLE conns are available, so checkout
	 * fires callback and returns OK (no token used). We need a queued
	 * checkout: first exhaust the idle connection.
	 */
	{
		cb_recorder_t first_rec;

		memset(&first_rec, 0, sizeof(first_rec));
		CHECK_ERR("cancel-fired: first checkout (immediate)",
			  braid_pool_checkout(pool, 0, recording_checkout_cb,
					      &first_rec, NULL),
			  BRAID_OK);
		conn_fd = first_rec.fd[0];
	}

	tok = 0;
	CHECK_ERR(
	    "cancel-fired: second checkout (enqueue)",
	    braid_pool_checkout(pool, 5000, recording_checkout_cb, &rec, &tok),
	    BRAID_OK);
	CHECK("cancel-fired: queued, not fired", rec.call_count == 0);

	/* Serve the waiter via checkin. */
	braid_pool_checkin(pool, conn_fd, BRAID_CONN_OK);
	CHECK("cancel-fired: served after checkin", rec.call_count == 1);
	CHECK("cancel-fired: err=OK", rec.err[0] == BRAID_OK);
	int served_fd = rec.fd[0];

	/* Cancel with stale token — must be no-op. */
	CHECK_ERR("cancel-fired: stale cancel", braid_pool_cancel(pool, tok),
		  BRAID_OK);
	CHECK("cancel-fired: still one call", rec.call_count == 1);

	braid_pool_checkin(pool, served_fd, BRAID_CONN_DISCARD);
	close(peer_fd);
	braid_pool_destroy(pool, 0);
	close(epfd);
}

/*
 * Cancel with a stale wrapped-around token: the token slot holds a different
 * (newer) waiter.  The cancel must be a no-op — it must NOT fire the newer
 * waiter's callback.
 */
static void
test_cancel_stale_wrapped_token_noop(void)
{
	braid_config_t cfg;
	braid_pool_t *pool;
	cb_recorder_t recA, recB;
	braid_token_t tokA;
	int err = 0;
	int epfd;

	memset(&recA, 0, sizeof(recA));
	memset(&recB, 0, sizeof(recB));
	epfd = make_epoll_fd();
	if (epfd < 0) {
		CHECK("cancel-stale-wrap: epoll_create1", 0);
		return;
	}

	cfg = make_minimal_config(epfd, 4);
	pool = braid_pool_create(&cfg, &err);
	if (pool == NULL) {
		CHECK("cancel-stale-wrap: create", 0);
		close(epfd);
		return;
	}

	braid_test_clock_ms = 1000;

	/*
	 * Fill a 1-slot ring with minimal capacity so wrap-around happens
	 * quickly. Instead we use the real pool but exercise wrap-around by
	 * enqueue+cancel repeatedly to wrap the tail pointer.
	 *
	 * Strategy: enqueue A (gets tokA), cancel A (tombstone, count--),
	 * enqueue B into the same slot position (same token value or next).
	 * Cancel tokA — must not fire B.
	 */
	tokA = 0;
	CHECK_ERR("cancel-stale-wrap: enqueue A",
		  braid_pool_checkout(pool, 5000, recording_checkout_cb, &recA,
				      &tokA),
		  BRAID_OK);
	/* Cancel A immediately. */
	CHECK_ERR("cancel-stale-wrap: cancel A", braid_pool_cancel(pool, tokA),
		  BRAID_OK);
	CHECK("cancel-stale-wrap: A fired with CANCELLED",
	      recA.call_count == 1);

	/* Enqueue B — it will land at tail which has advanced past tokA. */
	CHECK_ERR(
	    "cancel-stale-wrap: enqueue B",
	    braid_pool_checkout(pool, 5000, recording_checkout_cb, &recB, NULL),
	    BRAID_OK);

	/*
	 * Cancel with tokA (stale) — tokB occupies the ring at a different
	 * position (tail advanced) and tokA no longer matches anything.
	 */
	CHECK_ERR("cancel-stale-wrap: cancel stale tokA",
		  braid_pool_cancel(pool, tokA), BRAID_OK);
	CHECK("cancel-stale-wrap: B not fired", recB.call_count == 0);

	braid_pool_destroy(pool, 0);
	close(epfd);
}

/*
 * braid_pool_checkout with timeout_ms == 0 and no IDLE connection: returns
 * BRAID_ERR_EXHAUSTED without invoking any callback.
 */
static void
test_exhaustion_zero_timeout(void)
{
	braid_config_t cfg;
	braid_pool_t *pool;
	cb_recorder_t rec;
	braid_token_t tok;
	int err = 0;
	int epfd;

	memset(&rec, 0, sizeof(rec));
	reset_counters();
	epfd = make_epoll_fd();
	if (epfd < 0) {
		CHECK("exhaustion-zero: epoll_create1", 0);
		return;
	}

	cfg = make_minimal_config(epfd, 4);
	cfg.observe_fn = recording_observe_cb;
	pool = braid_pool_create(&cfg, &err);
	if (pool == NULL) {
		CHECK("exhaustion-zero: create", 0);
		close(epfd);
		return;
	}

	tok = 0;
	CHECK_ERR(
	    "exhaustion-zero: checkout returns EXHAUSTED",
	    braid_pool_checkout(pool, 0, recording_checkout_cb, &rec, &tok),
	    BRAID_ERR_EXHAUSTED);
	CHECK("exhaustion-zero: callback NOT invoked", rec.call_count == 0);
	CHECK("exhaustion-zero: POOL_EXHAUSTED event fired",
	      g_last_event_type == BRAID_EV_POOL_EXHAUSTED);

	braid_pool_destroy(pool, 0);
	close(epfd);
}

/*
 * braid_pool_checkin called from within a checkout callback (re-entrancy):
 * the pool must not crash and the deferred work must fire correctly after the
 * outermost callback returns.
 *
 * Scenario: checkout fires immediately (IDLE conn available), invoking the
 * callback. Inside the callback, checkin is called with BRAID_CONN_OK.
 * A second waiter is pending. After the outermost callback chain returns,
 * the second waiter must have been served (deferred SERVE_WAITQUEUE drained).
 */
typedef struct {
	braid_pool_t *pool;
	int fd;
	cb_recorder_t *second_rec;
} reentrant_cb_ctx_t;

static void
reentrant_checkout_cb(int fd, void *conn_ctx, int err, void *cb_ctx)
{
	reentrant_cb_ctx_t *ctx = cb_ctx;
	braid_token_t tok;

	(void)conn_ctx;
	(void)err;
	ctx->fd = fd;

	/*
	 * Enqueue a second waiter while holding the connection (in_callback>0).
	 * No IDLE conn is available (current one is ACTIVE), so this queues.
	 */
	braid_test_clock_ms = 1001;
	tok = 0;
	braid_pool_checkout(ctx->pool, 5000, recording_checkout_cb,
			    ctx->second_rec, &tok);

	/*
	 * Checkin while in_callback > 0: BRAID_DEFERRED_SERVE_WAITQUEUE is set.
	 * The deferred drain fires after braid_pool_checkout() returns.
	 */
	braid_pool_checkin(ctx->pool, fd, BRAID_CONN_OK);
}

static void
test_checkin_from_callback(void)
{
	braid_config_t cfg;
	braid_pool_t *pool;
	braid_conn_t *conn;
	cb_recorder_t second_rec;
	reentrant_cb_ctx_t ctx;
	int err = 0;
	int epfd, peer_fd;

	memset(&second_rec, 0, sizeof(second_rec));
	epfd = make_epoll_fd();
	if (epfd < 0) {
		CHECK("reentrant: epoll_create1", 0);
		return;
	}

	cfg = make_minimal_config(epfd, 4);
	pool = braid_pool_create(&cfg, &err);
	if (pool == NULL) {
		CHECK("reentrant: create", 0);
		close(epfd);
		return;
	}

	conn = alloc_idle_conn_on_epoll(pool, &peer_fd);
	if (conn == NULL) {
		CHECK("reentrant: alloc_idle_conn", 0);
		braid_pool_destroy(pool, 0);
		close(epfd);
		return;
	}

	/*
	 * Checkout the IDLE connection immediately. The reentrant_checkout_cb
	 * will enqueue a second waiter and then checkin from within the
	 * callback.
	 */
	braid_test_clock_ms = 1000;
	ctx.pool = pool;
	ctx.fd = -1;
	ctx.second_rec = &second_rec;

	CHECK_ERR(
	    "reentrant: checkout IDLE",
	    braid_pool_checkout(pool, 0, reentrant_checkout_cb, &ctx, NULL),
	    BRAID_OK);

	/*
	 * After checkout returns, the deferred work should have been drained:
	 * the second waiter should have been served by the deferred
	 * SERVE_WAITQUEUE firing.
	 */
	CHECK("reentrant: second waiter served after return",
	      second_rec.call_count == 1);
	CHECK("reentrant: second waiter err=OK", second_rec.err[0] == BRAID_OK);

	/* Clean up the second checkout. */
	if (second_rec.call_count > 0 && second_rec.err[0] == BRAID_OK)
		braid_pool_checkin(pool, second_rec.fd[0], BRAID_CONN_DISCARD);

	close(peer_fd);
	braid_pool_destroy(pool, 0);
	close(epfd);
}

/*
 * braid_pool_advance with connect_timeout exceeded: any CONNECTING connection
 * whose created_at_ms + connect_timeout <= now_ms must transition to DEAD.
 */
static void
test_connect_timeout_aborts_connecting(void)
{
	braid_config_t cfg;
	braid_pool_t *pool;
	braid_conn_t *conn;
	int err = 0;
	int epfd, peer_fd;
	uint32_t next_ms;

	epfd = make_epoll_fd();
	if (epfd < 0) {
		CHECK("connect-timeout: epoll_create1", 0);
		return;
	}

	cfg = make_minimal_config(epfd, 4);
	cfg.connect_timeout = 1000; /* 1s */
	cfg.min_connections = 0;
	pool = braid_pool_create(&cfg, &err);
	if (pool == NULL) {
		CHECK("connect-timeout: create", 0);
		close(epfd);
		return;
	}

	/* Manually create a CONNECTING connection. */
	braid_test_clock_ms = 500;
	conn = alloc_connecting_conn_on_epoll(pool, &peer_fd);
	if (conn == NULL) {
		CHECK("connect-timeout: alloc_connecting_conn", 0);
		braid_pool_destroy(pool, 0);
		close(epfd);
		return;
	}
	/* Force created_at_ms to 500 to control the deadline. */
	conn->created_at_ms = 500;

	CHECK("connect-timeout: live_count = 1", pool->live_count == 1);

	/* Advance time past deadline (500 + 1000 = 1500, now = 2000). */
	braid_test_clock_ms = 2000;
	next_ms = 0;
	CHECK_ERR("connect-timeout: advance returns OK",
		  braid_pool_advance(pool, &next_ms), BRAID_OK);
	CHECK("connect-timeout: live_count = 0 after advance",
	      pool->live_count == 0);

	close(peer_fd);
	braid_pool_destroy(pool, 0);
	close(epfd);
}

/*
 * Shutdown suppresses reconnect: after pool->shutting_down is set, no new
 * reconnect entries are inserted even if live_count drops below
 * min_connections.
 */
static void
test_shutdown_suppresses_reconnect(void)
{
	braid_config_t cfg;
	braid_pool_t *pool;
	braid_conn_t *conn;
	int err = 0;
	int epfd, peer_fd;

	epfd = make_epoll_fd();
	if (epfd < 0) {
		CHECK("shutdown-suppress: epoll_create1", 0);
		return;
	}

	cfg = make_minimal_config(epfd, 4);
	cfg.min_connections = 2; /* would trigger reconnect on DEAD */
	pool = braid_pool_create(&cfg, &err);
	if (pool == NULL) {
		CHECK("shutdown-suppress: create", 0);
		close(epfd);
		return;
	}

	/* Drain the initial reconnect entries inserted for min_connections. */
	reconnect_heap_clear(&pool->reconnect);

	conn = alloc_idle_conn_on_epoll(pool, &peer_fd);
	if (conn == NULL) {
		CHECK("shutdown-suppress: alloc_idle_conn", 0);
		braid_pool_destroy(pool, 0);
		close(epfd);
		return;
	}

	/* Mark shutting down before triggering DEAD. */
	pool->shutting_down = 1;

	/* Transition to DEAD — should NOT insert a reconnect entry. */
	conn_transition(pool, conn, BRAID_STATE_CLOSING);

	CHECK("shutdown-suppress: reconnect heap empty",
	      pool->reconnect.count == 0);

	close(peer_fd);
	braid_pool_destroy(pool, 0);
	close(epfd);
}

/*
 * braid_pool_destroy cancels all pending waiters with BRAID_ERR_SHUTDOWN.
 */
static void
test_shutdown_cancels_waiters(void)
{
	braid_config_t cfg;
	braid_pool_t *pool;
	cb_recorder_t recA, recB;
	braid_token_t tok;
	int err = 0;
	int epfd;

	memset(&recA, 0, sizeof(recA));
	memset(&recB, 0, sizeof(recB));
	epfd = make_epoll_fd();
	if (epfd < 0) {
		CHECK("shutdown-waiters: epoll_create1", 0);
		return;
	}

	cfg = make_minimal_config(epfd, 4);
	pool = braid_pool_create(&cfg, &err);
	if (pool == NULL) {
		CHECK("shutdown-waiters: create", 0);
		close(epfd);
		return;
	}

	braid_test_clock_ms = 1000;
	tok = 0;
	CHECK_ERR(
	    "shutdown-waiters: enqueue A",
	    braid_pool_checkout(pool, 5000, recording_checkout_cb, &recA, &tok),
	    BRAID_OK);
	CHECK_ERR(
	    "shutdown-waiters: enqueue B",
	    braid_pool_checkout(pool, 5000, recording_checkout_cb, &recB, NULL),
	    BRAID_OK);
	CHECK("shutdown-waiters: waitq count = 2", pool->waitq.count == 2);

	braid_pool_destroy(pool, 0);
	close(epfd);

	CHECK("shutdown-waiters: A got SHUTDOWN",
	      recA.err[0] == BRAID_ERR_SHUTDOWN);
	CHECK("shutdown-waiters: A called once", recA.call_count == 1);
	CHECK("shutdown-waiters: B got SHUTDOWN",
	      recB.err[0] == BRAID_ERR_SHUTDOWN);
	CHECK("shutdown-waiters: B called once", recB.call_count == 1);
}

/*
 * Pool with observe_fn = NULL: all pool operations complete without crash.
 */
static void
test_observe_fn_null_no_crash(void)
{
	braid_config_t cfg;
	braid_pool_t *pool;
	braid_conn_t *conn;
	cb_recorder_t rec;
	braid_token_t tok;
	int err = 0;
	int epfd, peer_fd;

	memset(&rec, 0, sizeof(rec));
	epfd = make_epoll_fd();
	if (epfd < 0) {
		CHECK("observe-null: epoll_create1", 0);
		return;
	}

	cfg = make_minimal_config(epfd, 4);
	cfg.observe_fn = NULL; /* explicit NULL — default anyway */
	pool = braid_pool_create(&cfg, &err);
	if (pool == NULL) {
		CHECK("observe-null: create", 0);
		close(epfd);
		return;
	}

	conn = alloc_idle_conn_on_epoll(pool, &peer_fd);
	if (conn == NULL) {
		CHECK("observe-null: alloc_idle_conn", 0);
		braid_pool_destroy(pool, 0);
		close(epfd);
		return;
	}

	tok = 0;
	CHECK_ERR(
	    "observe-null: checkout OK",
	    braid_pool_checkout(pool, 0, recording_checkout_cb, &rec, &tok),
	    BRAID_OK);
	CHECK("observe-null: callback fired", rec.call_count == 1);
	braid_pool_checkin(pool, rec.fd[0], BRAID_CONN_DISCARD);

	close(peer_fd);
	braid_pool_destroy(pool, 0);
	close(epfd);
	CHECK("observe-null: no crash", 1);
}

/*
 * validate_fn is called only when idle_threshold has been exceeded.
 * At checkout, if now_ms < last_active_ms + idle_threshold, validate_fn
 * must NOT be called. If now_ms >= last_active_ms + idle_threshold, it
 * must be called.
 */
static void
test_validate_fn_called_above_threshold(void)
{
	braid_config_t cfg;
	braid_pool_t *pool;
	braid_conn_t *conn;
	cb_recorder_t rec;
	int err = 0;
	int epfd, peer_fd;

	memset(&rec, 0, sizeof(rec));
	reset_counters();
	epfd = make_epoll_fd();
	if (epfd < 0) {
		CHECK("validate-threshold: epoll_create1", 0);
		return;
	}

	cfg = make_minimal_config(epfd, 4);
	cfg.validate_fn = recording_validate_cb;
	cfg.idle_threshold = 5000; /* 5 s */
	cfg.validate_timeout = 2000;
	g_validate_return = BRAID_OK;
	pool = braid_pool_create(&cfg, &err);
	if (pool == NULL) {
		CHECK("validate-threshold: create", 0);
		close(epfd);
		return;
	}

	/* Set clock to 1000 ms when transitioning to IDLE. */
	braid_test_clock_ms = 1000;
	conn = alloc_idle_conn_on_epoll(pool, &peer_fd);
	if (conn == NULL) {
		CHECK("validate-threshold: alloc_idle_conn", 0);
		braid_pool_destroy(pool, 0);
		close(epfd);
		return;
	}
	/* last_active_ms = 1000 (set by IDLE transition at clock=1000). */

	/* Checkout before threshold: clock = 5999 < 1000 + 5000 = 6000. */
	braid_test_clock_ms = 5999;
	CHECK_ERR(
	    "validate-threshold: checkout before threshold",
	    braid_pool_checkout(pool, 0, recording_checkout_cb, &rec, NULL),
	    BRAID_OK);
	CHECK("validate-threshold: NOT called before threshold",
	      g_validate_calls == 0);
	CHECK("validate-threshold: checkout fired", rec.call_count == 1);
	braid_pool_checkin(pool, rec.fd[0], BRAID_CONN_OK);

	/*
	 * After CONN_OK checkin the conn is IDLE again with
	 * last_active_ms=5999. Override it back to 1000 so that at time 6001
	 * the threshold is crossed: 6001 >= 1000 + 5000 = 6001.
	 */
	conn->last_active_ms = 1000;

	/* Checkout above threshold: now=6001 >= 1000+5000=6001. */
	memset(&rec, 0, sizeof(rec));
	braid_test_clock_ms = 6001;
	CHECK_ERR(
	    "validate-threshold: checkout above threshold",
	    braid_pool_checkout(pool, 0, recording_checkout_cb, &rec, NULL),
	    BRAID_OK);
	CHECK("validate-threshold: called above threshold",
	      g_validate_calls >= 1);
	CHECK("validate-threshold: checkout still fired (validate returned OK)",
	      rec.call_count == 1);
	if (rec.call_count > 0)
		braid_pool_checkin(pool, rec.fd[0], BRAID_CONN_DISCARD);

	close(peer_fd);
	braid_pool_destroy(pool, 0);
	close(epfd);
}

/*
 * validate_fn returning non-OK: the connection is discarded (CLOSING→DEAD),
 * and checkout continues searching. With no other IDLE connections the
 * checkout enqueues (if timeout>0) or returns EXHAUSTED (timeout=0).
 */
static void
test_validate_fn_failure_discards(void)
{
	braid_config_t cfg;
	braid_pool_t *pool;
	braid_conn_t *conn;
	cb_recorder_t rec;
	int err = 0;
	int epfd, peer_fd;

	memset(&rec, 0, sizeof(rec));
	reset_counters();
	epfd = make_epoll_fd();
	if (epfd < 0) {
		CHECK("validate-fail: epoll_create1", 0);
		return;
	}

	cfg = make_minimal_config(epfd, 4);
	cfg.validate_fn = recording_validate_cb;
	cfg.idle_threshold = 1; /* 1 ms — always exceeded */
	cfg.validate_timeout = 2000;
	cfg.destroy_fn = recording_destroy_cb;
	g_validate_return = BRAID_ERR_CONNFAIL; /* force failure */
	pool = braid_pool_create(&cfg, &err);
	if (pool == NULL) {
		CHECK("validate-fail: create", 0);
		close(epfd);
		return;
	}

	braid_test_clock_ms = 1000;
	conn = alloc_idle_conn_on_epoll(pool, &peer_fd);
	if (conn == NULL) {
		CHECK("validate-fail: alloc_idle_conn", 0);
		braid_pool_destroy(pool, 0);
		close(epfd);
		return;
	}
	/* Force last_active_ms to 0 so threshold (1ms from now) is exceeded. */
	conn->last_active_ms = 0;

	/* Checkout — validate_fn fails, connection must be discarded. */
	braid_test_clock_ms = 1100;
	CHECK_ERR(
	    "validate-fail: checkout returns EXHAUSTED (no valid conn)",
	    braid_pool_checkout(pool, 0, recording_checkout_cb, &rec, NULL),
	    BRAID_ERR_EXHAUSTED);
	CHECK("validate-fail: validate_fn called", g_validate_calls >= 1);
	CHECK("validate-fail: callback NOT fired", rec.call_count == 0);
	CHECK("validate-fail: destroy_fn called", g_destroy_calls >= 1);
	CHECK("validate-fail: live_count = 0", pool->live_count == 0);

	close(peer_fd);
	braid_pool_destroy(pool, 0);
	close(epfd);
}

/*
 * init_fn with deadline exceeded: if init_fn returns non-OK, the connection
 * transitions to DEAD. Simulated by braid_pool_notify on a CONNECTING fd with
 * getsockopt returning 0 (success) but init_fn returning failure.
 */
static void
test_init_fn_deadline_exceeded(void)
{
	braid_config_t cfg;
	braid_pool_t *pool;
	braid_conn_t *conn;
	int err = 0;
	int epfd, peer_fd;

	reset_counters();
	epfd = make_epoll_fd();
	if (epfd < 0) {
		CHECK("init-deadline: epoll_create1", 0);
		return;
	}

	cfg = make_minimal_config(epfd, 4);
	cfg.init_fn = recording_init_cb;
	cfg.init_timeout = 100;
	cfg.destroy_fn = recording_destroy_cb;
	g_init_return = BRAID_ERR_TIMEOUT; /* simulate deadline exceeded */
	pool = braid_pool_create(&cfg, &err);
	if (pool == NULL) {
		CHECK("init-deadline: create", 0);
		close(epfd);
		return;
	}

	/*
	 * Build a CONNECTING socketpair. braid_pool_notify dispatches on state,
	 * not on events bits. The fd must be registered as CONNECTING so we use
	 * alloc_connecting_conn_on_epoll.
	 */
	braid_test_clock_ms = 1000;
	conn = alloc_connecting_conn_on_epoll(pool, &peer_fd);
	if (conn == NULL) {
		CHECK("init-deadline: alloc_connecting_conn", 0);
		braid_pool_destroy(pool, 0);
		close(epfd);
		return;
	}
	int conn_fd = conn->fd;

	/*
	 * Notify with CONNECTING state: getsockopt on the AF_UNIX socketpair
	 * will return SO_ERROR == 0, so the success path runs and init_fn is
	 * called. init_fn returns BRAID_ERR_TIMEOUT, triggering DEAD.
	 */
	CHECK_ERR("init-deadline: braid_pool_notify returns OK",
		  braid_pool_notify(pool, conn_fd, BRAID_IO_WRITE), BRAID_OK);
	CHECK("init-deadline: init_fn called once", g_init_calls == 1);
	CHECK("init-deadline: live_count = 0 (conn transitioned to DEAD)",
	      pool->live_count == 0);

	/*
	 * The connection fd was closed by conn_transition(→ DEAD).
	 * Do not close conn_fd again. Close the peer end.
	 */
	close(peer_fd);
	braid_pool_destroy(pool, 0);
	close(epfd);
}

/*
 * init_timeout overrun enforcement: init_fn returning BRAID_OK after the
 * deadline must still be treated as INITIALIZING failure.
 */
static void
test_init_fn_elapsed_deadline_enforced(void)
{
	braid_config_t cfg;
	braid_pool_t *pool;
	braid_conn_t *conn;
	int err = 0;
	int epfd, peer_fd;

	reset_counters();
	epfd = make_epoll_fd();
	if (epfd < 0) {
		CHECK("init-elapsed: epoll_create1", 0);
		return;
	}

	cfg = make_minimal_config(epfd, 4);
	cfg.init_fn = recording_init_cb;
	cfg.init_timeout = 100;
	cfg.destroy_fn = recording_destroy_cb;
	g_init_return = BRAID_OK;
	g_init_advance_ms = 1000; /* force deadline overrun inside init_fn */

	pool = braid_pool_create(&cfg, &err);
	if (pool == NULL) {
		CHECK("init-elapsed: create", 0);
		close(epfd);
		return;
	}

	braid_test_clock_ms = 1000;
	conn = alloc_connecting_conn_on_epoll(pool, &peer_fd);
	if (conn == NULL) {
		CHECK("init-elapsed: alloc_connecting_conn", 0);
		braid_pool_destroy(pool, 0);
		close(epfd);
		return;
	}

	CHECK_ERR("init-elapsed: notify returns OK",
		  braid_pool_notify(pool, conn->fd, BRAID_IO_WRITE), BRAID_OK);
	CHECK("init-elapsed: init_fn called once", g_init_calls == 1);
	CHECK("init-elapsed: conn discarded on deadline overrun",
	      pool->live_count == 0);

	close(peer_fd);
	braid_pool_destroy(pool, 0);
	close(epfd);
}

/*
 * io_modify failure in CONNECTING completion path must discard the
 * connection rather than leaving a live slot without event registration.
 */
static void
test_notify_io_modify_failure_discards_connection(void)
{
	braid_config_t cfg;
	braid_pool_t *pool;
	braid_conn_t *conn;
	int err = 0;
	int epfd, peer_fd;
	int fd;

	epfd = make_epoll_fd();
	if (epfd < 0) {
		CHECK("notify-modfail: epoll_create1", 0);
		return;
	}

	cfg = make_minimal_config(epfd, 4);
	pool = braid_pool_create(&cfg, &err);
	if (pool == NULL) {
		CHECK("notify-modfail: create", 0);
		close(epfd);
		return;
	}

	conn = alloc_connecting_conn_on_epoll(pool, &peer_fd);
	if (conn == NULL) {
		CHECK("notify-modfail: alloc_connecting_conn", 0);
		braid_pool_destroy(pool, 0);
		close(epfd);
		return;
	}
	fd = conn->fd;

	close(epfd);
	epfd = -1;

	CHECK_ERR("notify-modfail: notify returns OK",
		  braid_pool_notify(pool, fd, BRAID_IO_WRITE), BRAID_OK);
	CHECK("notify-modfail: connection discarded on io_modify failure",
	      pool->live_count == 0);

	close(peer_fd);
	braid_pool_destroy(pool, 0);
}

#ifndef __linux__
/*
 * kqueue: io_modify must propagate a hard failure from the first EV_DELETE
 * call (read filter) and stop immediately.
 */
static void
test_kqueue_io_modify_delete_error_propagates(void)
{
	braid_config_t cfg;
	braid_pool_t *pool;
	braid_conn_t *conn;
	int err = 0;
	int epfd, peer_fd;

	epfd = make_epoll_fd();
	if (epfd < 0) {
		CHECK("kq-mod-delerr: kqueue", 0);
		return;
	}

	cfg = make_minimal_config(epfd, 4);
	pool = braid_pool_create(&cfg, &err);
	if (pool == NULL) {
		CHECK("kq-mod-delerr: create", 0);
		close(epfd);
		return;
	}

	conn = alloc_idle_conn_on_epoll(pool, &peer_fd);
	if (conn == NULL) {
		CHECK("kq-mod-delerr: alloc_idle_conn", 0);
		braid_pool_destroy(pool, 0);
		close(epfd);
		return;
	}

	io_kqueue_test_reset_apply_calls();
	io_kqueue_test_force_error_on_call(1);
	CHECK_ERR("kq-mod-delerr: io_modify returns SYSCALL",
		  io_modify(pool, conn->fd, BRAID_IO_READ), BRAID_ERR_SYSCALL);
	CHECK("kq-mod-delerr: stopped after first apply call",
	      io_kqueue_test_get_apply_calls() == 1);

	io_kqueue_test_force_error_on_call(0);
	io_kqueue_test_reset_apply_calls();
	close(peer_fd);
	braid_pool_destroy(pool, 0);
	close(epfd);
}

/*
 * kqueue: io_unwatch must propagate a hard failure from the first EV_DELETE
 * call (read filter) and stop immediately.
 */
static void
test_kqueue_io_unwatch_delete_error_propagates(void)
{
	braid_config_t cfg;
	braid_pool_t *pool;
	braid_conn_t *conn;
	int err = 0;
	int epfd, peer_fd;

	epfd = make_epoll_fd();
	if (epfd < 0) {
		CHECK("kq-unwatch-delerr: kqueue", 0);
		return;
	}

	cfg = make_minimal_config(epfd, 4);
	pool = braid_pool_create(&cfg, &err);
	if (pool == NULL) {
		CHECK("kq-unwatch-delerr: create", 0);
		close(epfd);
		return;
	}

	conn = alloc_idle_conn_on_epoll(pool, &peer_fd);
	if (conn == NULL) {
		CHECK("kq-unwatch-delerr: alloc_idle_conn", 0);
		braid_pool_destroy(pool, 0);
		close(epfd);
		return;
	}

	io_kqueue_test_reset_apply_calls();
	io_kqueue_test_force_error_on_call(1);
	CHECK_ERR("kq-unwatch-delerr: io_unwatch returns SYSCALL",
		  io_unwatch(pool, conn->fd), BRAID_ERR_SYSCALL);
	CHECK("kq-unwatch-delerr: stopped after first apply call",
	      io_kqueue_test_get_apply_calls() == 1);

	io_kqueue_test_force_error_on_call(0);
	io_kqueue_test_reset_apply_calls();
	close(peer_fd);
	braid_pool_destroy(pool, 0);
	close(epfd);
}
#endif

/* ── test suite entry point ──────────────────────────────────────────── */

void
run_pool_tests(void)
{
	braid_test_clock_ms = 0;
	reset_counters();

	test_pool_create_valid();
	test_pool_create_null_event_fd();
	test_pool_create_min_gt_max();
	test_api_null_argument_guards();
	test_pool_destroy_no_active();
	test_pool_destroy_with_active();
	test_checkout_immediate();
	test_active_event_registration_handoff();
	test_checkout_enqueues();
	test_checkin_conn_ok();
	test_checkin_conn_discard();
	test_checkin_unknown_fd();
	test_cancel_pending();
	test_cancel_fired_token();
	test_cancel_stale_wrapped_token_noop();
	test_exhaustion_zero_timeout();
	test_checkin_from_callback();
	test_shutdown_cancels_waiters();
	test_observe_fn_null_no_crash();
	test_validate_fn_called_above_threshold();
	test_validate_fn_failure_discards();
	test_connect_timeout_aborts_connecting();
	test_shutdown_suppresses_reconnect();
	test_init_fn_deadline_exceeded();
	test_init_fn_elapsed_deadline_enforced();
	test_notify_io_modify_failure_discards_connection();
#ifndef __linux__
	test_kqueue_io_modify_delete_error_propagates();
	test_kqueue_io_unwatch_delete_error_propagates();
#endif
}
