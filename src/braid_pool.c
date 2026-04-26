/*
 * braid_pool.c — pool lifecycle and public API implementation
 *
 * braid_pool_create(), braid_pool_destroy(), braid_pool_checkout(),
 * braid_pool_checkin(), braid_pool_cancel(), braid_pool_advance(),
 * braid_pool_notify().
 *
 * See ARCHITECTURE.md §11, §12, §13.
 */

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef __linux__
#include <sys/random.h> /* getentropy on Linux with strict POSIX macros */
#else
/* arc4random_buf is in OpenBSD/BSD libc but its prototype is hidden when
 * _POSIX_C_SOURCE suppresses __BSD_VISIBLE.  Forward-declare with the
 * known stable ABI signature rather than relaxing feature-test macros. */
void arc4random_buf(void *, size_t);
#endif
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "../include/braid.h"
#include "braid_internal.h"
#include "braid_conn.h"
#include "braid_io.h"
#include "braid_pool.h"
#include "braid_reaper.h"
#include "braid_reconnect.h"
#include "braid_table.h"
#include "braid_waitq.h"

/* Forward declaration: defined after pool_fire_event and pool_active_count. */
static int pool_serve_waiter(braid_pool_t *pool);

static void pool_fire_conn_destroyed_event(braid_pool_t *pool, int fd);
static void pool_waitq_timeout_event_hook(void *hook_ctx);

/*
 * pool_drain_deferred — process all flagged deferred work.
 *
 * Must be called with pool->in_callback == 0.  Clears each flag before
 * dispatching its work so that nested callbacks set the flag again if
 * needed rather than relying on the current pass.
 *
 * Processing order is fixed by ARCHITECTURE.md §9.2:
 *   1. BRAID_DEFERRED_PROCESS_DEAD  — complete DEAD transitions for
 *      connections whose CLOSING→DEAD step was deferred by
 *      CONN_FLAG_CLOSING_DEFERRED while in_callback > 0.
 *   2. BRAID_DEFERRED_SERVE_WAITQUEUE — serve the head of the wait queue,
 *      which may invoke a callback that is safe here because in_callback
 *      is 0 at this point.
 */
void
pool_drain_deferred(braid_pool_t *pool)
{
	if (pool->deferred_work & BRAID_DEFERRED_PROCESS_DEAD) {
		uint32_t i;

		pool->deferred_work &= ~BRAID_DEFERRED_PROCESS_DEAD;
		for (i = 0; i < pool->table_size; i++) {
			braid_conn_t *conn = &pool->table[i];

			if (conn->fd == -1 ||
			    (conn->flags & CONN_FLAG_TOMBSTONE))
				continue;
			if (conn->flags & CONN_FLAG_CLOSING_DEFERRED)
				conn_transition(pool, conn, BRAID_STATE_DEAD);
		}
	}

	if (pool->deferred_work & BRAID_DEFERRED_SERVE_WAITQUEUE) {
		pool->deferred_work &= ~BRAID_DEFERRED_SERVE_WAITQUEUE;
		pool_serve_waiter(pool);
	}
}

/* ── pool_fire_event ────────────────────────────────────────────────── */

/*
 * pool_fire_event — fire a pool-level observable event via observe_fn.
 * Uses the in_callback protocol. Pool-level events carry no connection fd;
 * fd is set to -1.
 */
static void
pool_fire_event(braid_pool_t *pool, braid_event_type_t type)
{
	braid_event_t ev;

	if (pool->config.observe_fn == NULL)
		return;

	memset(&ev, 0, sizeof(ev));
	ev.type = type;
	ev.fd = -1;
	pool->in_callback++;
	pool->config.observe_fn(&ev, pool->config.hook_context);
	pool->in_callback--;
	if (pool->in_callback == 0 && pool->deferred_work != 0)
		pool_drain_deferred(pool);
}

static void
pool_fire_conn_destroyed_event(braid_pool_t *pool, int fd)
{
	braid_event_t ev;

	if (pool->config.observe_fn == NULL)
		return;

	memset(&ev, 0, sizeof(ev));
	ev.type = BRAID_EV_CONN_DESTROYED;
	ev.fd = fd;
	pool->in_callback++;
	pool->config.observe_fn(&ev, pool->config.hook_context);
	pool->in_callback--;
	if (pool->in_callback == 0 && pool->deferred_work != 0)
		pool_drain_deferred(pool);
}

static void
pool_waitq_timeout_event_hook(void *hook_ctx)
{
	braid_pool_t *pool = hook_ctx;

	pool_fire_event(pool, BRAID_EV_CHECKOUT_TIMEOUT);
}

/* ── pool_serve_waiter ───────────────────────────────────────────────── */

/*
 * pool_serve_waiter — promote one IDLE connection to ACTIVE and serve the
 * wait queue head.
 *
 * Scans the connection table for any IDLE slot.  If found, transitions it
 * IDLE→ACTIVE with the full in_callback protocol and calls
 * waitq_serve_head() to fire the oldest pending checkout callback.
 *
 * Must be called with pool->in_callback == 0.
 * Returns BRAID_OK if a waiter was served, BRAID_ERR_EXHAUSTED if no
 * IDLE connection or no waiter was available.
 */
static int
pool_serve_waiter(braid_pool_t *pool)
{
	uint32_t i;

	if (pool->waitq.count == 0)
		return BRAID_ERR_EXHAUSTED;

	for (i = 0; i < pool->table_size; i++) {
		braid_conn_t *conn = &pool->table[i];

		if (conn->fd == -1 || (conn->flags & CONN_FLAG_TOMBSTONE))
			continue;
		if (conn->state != BRAID_STATE_IDLE)
			continue;

		if (io_unwatch(pool, conn->fd) != BRAID_OK) {
			conn_transition(pool, conn, BRAID_STATE_CLOSING);
			continue;
		}

		conn_transition(pool, conn, BRAID_STATE_ACTIVE);
		pool->in_callback++;
		waitq_serve_head(&pool->waitq, conn->fd, conn->conn_ctx);
		pool->in_callback--;
		if (pool->in_callback == 0 && pool->deferred_work != 0)
			pool_drain_deferred(pool);
		return BRAID_OK;
	}
	return BRAID_ERR_EXHAUSTED;
}

/* ── pool_active_count ───────────────────────────────────────────────── */

/*
 * pool_active_count — count connections currently in ACTIVE state.
 * Used by braid_pool_destroy() drain loop to wait for checkins.
 */
static uint32_t
pool_active_count(braid_pool_t *pool)
{
	uint32_t count = 0;
	uint32_t i;

	for (i = 0; i < pool->table_size; i++) {
		const braid_conn_t *c = &pool->table[i];

		if (c->fd != -1 && !(c->flags & CONN_FLAG_TOMBSTONE) &&
		    c->state == BRAID_STATE_ACTIVE)
			count++;
	}
	return count;
}

/* ── braid_pool_create ───────────────────────────────────────────────── */

/*
 * braid_pool_create — allocate and initialise a pool instance.
 *
 * Validates config, applies defaults for zero fields, allocates all internal
 * structures, deep-copies config.host via strdup(), seeds the per-pool PRNG
 * via getentropy(), and inserts min_connections reconnect entries at time 0
 * so the pool warms up on the first braid_pool_advance() call.
 *
 * Returns NULL on any failure; *err is set to a BRAID_ERR_* code.
 * Uses goto cleanup with reverse-order freeing for all failure paths.
 * See ARCHITECTURE.md §13.1, §16.
 */
braid_pool_t *
braid_pool_create(const braid_config_t *config, int *err)
{
	braid_pool_t *pool;
	int rc;
	uint32_t i;

	/* Validate required config fields. */
	if (config == NULL || config->event_fd < 0 ||
	    config->max_connections == 0 ||
	    config->min_connections > config->max_connections) {
		if (err != NULL)
			*err = BRAID_ERR_INVAL;
		return NULL;
	}

	pool = calloc(1, sizeof(*pool));
	if (pool == NULL) {
		if (err != NULL)
			*err = BRAID_ERR_NOMEM;
		return NULL;
	}

	/* Copy config; host is deep-copied below. */
	pool->config = *config;
	pool->config.host = NULL;

	/* Apply default values for all zero fields per ARCHITECTURE.md §16. */
	if (pool->config.connect_timeout == 0)
		pool->config.connect_timeout = 5000;
	if (pool->config.init_timeout == 0)
		pool->config.init_timeout = 10000;
	if (pool->config.validate_timeout == 0)
		pool->config.validate_timeout = 2000;
	if (pool->config.idle_threshold == 0)
		pool->config.idle_threshold = 30000;
	if (pool->config.idle_reap_timeout == 0)
		pool->config.idle_reap_timeout = 300000;
	if (pool->config.keepalive_idle == 0)
		pool->config.keepalive_idle = 60;
	if (pool->config.keepalive_interval == 0)
		pool->config.keepalive_interval = 10;
	if (pool->config.keepalive_count == 0)
		pool->config.keepalive_count = 3;
	if (pool->config.backoff_base == 0)
		pool->config.backoff_base = 100;
	if (pool->config.backoff_cap == 0)
		pool->config.backoff_cap = 30000;
	/* backoff_max_attempts: 0 is a valid sentinel meaning "retry forever".
	 */

	if (config->host != NULL) {
		pool->config.host = strdup(config->host);
		if (pool->config.host == NULL) {
			rc = BRAID_ERR_NOMEM;
			goto cleanup;
		}
	}

	rc = table_init(pool);
	if (rc != BRAID_OK)
		goto cleanup;

	rc = reconnect_heap_init(&pool->reconnect, config->max_connections);
	if (rc != BRAID_OK)
		goto cleanup;

	rc = reaper_heap_init(&pool->idle, config->max_connections);
	if (rc != BRAID_OK)
		goto cleanup;

	rc = waitq_init(&pool->waitq, config->max_connections);
	if (rc != BRAID_OK)
		goto cleanup;

	/*
	 * Seed per-pool PRNG.  Non-zero is required for the xorshift64
	 * algorithm in pool_prng_next().  If the entropy call returns zero
	 * bits (astronomically unlikely), fall back to a non-zero constant.
	 * See ARCHITECTURE.md §6.2.
	 *
	 * getentropy() is used on Linux (declared in <sys/random.h>).
	 * arc4random_buf() is used on OpenBSD/BSD (always visible in libc,
	 * not gated by feature-test macros).
	 */
#ifdef __linux__
	getentropy(&pool->prng, sizeof(pool->prng));
#else
	arc4random_buf(&pool->prng, sizeof(pool->prng));
#endif
	if (pool->prng == 0)
		pool->prng = 0x9e3779b97f4a7c15ULL;

	/*
	 * Insert min_connections entries into the reconnection heap at
	 * next_retry_ms = 0 so that the first braid_pool_advance() call
	 * fires all initial connect attempts without blocking create().
	 * See ARCHITECTURE.md §13.1 step 8.
	 */
	for (i = 0; i < config->min_connections; i++) {
		braid_reconnect_entry_t entry;

		memset(&entry, 0, sizeof(entry));
		rc = reconnect_heap_push(&pool->reconnect, entry);
		if (rc != BRAID_OK)
			goto cleanup;
	}

	if (err != NULL)
		*err = BRAID_OK;
	return pool;

cleanup:
	if (err != NULL)
		*err = rc;
	/* Free in reverse allocation order — safe to call on zero structs. */
	waitq_destroy(&pool->waitq);
	reaper_heap_destroy(&pool->idle);
	reconnect_heap_destroy(&pool->reconnect);
	table_destroy(pool);
	free((void *)pool->config.host);
	free(pool);
	return NULL;
}

/* ── braid_pool_destroy ──────────────────────────────────────────────── */

/*
 * braid_pool_destroy — orderly teardown of all pool resources.
 *
 * Steps (see ARCHITECTURE.md §13.2):
 *   1. Mark shutting_down to suppress new reconnections.
 *   2. Cancel all pending wait queue entries (BRAID_ERR_SHUTDOWN callbacks).
 *   3. Drain ACTIVE connections (if drain_timeout_ms > 0).
 *   4. CONNECTING → manual close, no destroy_fn, fire observe_fn.
 *   5. INITIALIZING and deferred CLOSING → conn_transition(→ DEAD).
 *   6. IDLE → conn_transition(→ CLOSING) which cascades to DEAD.
 *   7. Clear reconnection heap.
 *   8. Force-close any remaining ACTIVE fds (drain expired or drain=0).
 *   9. Free all structures.
 */
void
braid_pool_destroy(braid_pool_t *pool, uint32_t drain_timeout_ms)
{
	uint32_t i;

	if (pool == NULL)
		return;

	/* Step 1: suppress new reconnections and checkout calls. */
	pool->shutting_down = 1;

	/* Step 2: cancel all pending waiters. */
	waitq_shutdown(&pool->waitq);

	/*
	 * Step 3: wait for ACTIVE connections to be checked in.
	 * Polls by sleeping 10 ms at a time up to drain_timeout_ms.
	 * In production the caller's event loop drives checkins during
	 * this window.  See ARCHITECTURE.md §13.2.
	 */
	if (drain_timeout_ms > 0) {
		uint64_t deadline_ms;
		struct timespec ts;
		uint32_t next_ms;

		deadline_ms = braid_now_ms() + drain_timeout_ms;
		ts.tv_sec = 0;
		ts.tv_nsec = 10L * 1000 * 1000; /* 10 ms */
		while (pool_active_count(pool) > 0) {
			if (braid_now_ms() >= deadline_ms)
				break;
			braid_pool_advance(pool, &next_ms);
			nanosleep(&ts, NULL);
		}
	}

	/*
	 * Step 4: close all CONNECTING sockets without calling destroy_fn.
	 * No protocol state has been established for CONNECTING connections.
	 * Handle manually rather than via conn_transition() to avoid the
	 * destroy_fn call that conn_transition(→ DEAD) would otherwise make
	 * for old_state != CLOSING.  See ARCHITECTURE.md §13.2 step 4.
	 */
	for (i = 0; i < pool->table_size; i++) {
		braid_conn_t *conn = &pool->table[i];
		int fd;

		if (conn->fd == -1 || (conn->flags & CONN_FLAG_TOMBSTONE))
			continue;
		if (conn->state != BRAID_STATE_CONNECTING)
			continue;

		fd = conn->fd;
		io_unwatch(pool, fd);
		close(fd);
		conn->tag.fd = -1;
		conn->tag.magic = 0;
		table_delete(pool, fd);
		BRAID_DEBUG_ASSERT(
		    pool->live_count > 0,
		    "braid_pool_destroy: live_count underflow (CONNECTING)");
		pool->live_count--;

		pool_fire_conn_destroyed_event(pool, fd);
	}

	/*
	 * Step 5: INITIALIZING → DEAD (destroy_fn called for partial
	 * protocol state); deferred CLOSING → DEAD (in_callback is 0 here).
	 * conn_transition handles io_unwatch, close, table_delete, live_count.
	 */
	for (i = 0; i < pool->table_size; i++) {
		braid_conn_t *conn = &pool->table[i];

		if (conn->fd == -1 || (conn->flags & CONN_FLAG_TOMBSTONE))
			continue;
		if (conn->state == BRAID_STATE_INITIALIZING ||
		    conn->state == BRAID_STATE_CLOSING)
			conn_transition(pool, conn, BRAID_STATE_DEAD);
	}

	/*
	 * Step 6: IDLE → CLOSING → DEAD (destroy_fn called for each).
	 * conn_transition(→ CLOSING) immediately chains to DEAD because
	 * in_callback is 0 at this point.
	 */
	for (i = 0; i < pool->table_size; i++) {
		braid_conn_t *conn = &pool->table[i];

		if (conn->fd == -1 || (conn->flags & CONN_FLAG_TOMBSTONE))
			continue;
		if (conn->state == BRAID_STATE_IDLE)
			conn_transition(pool, conn, BRAID_STATE_CLOSING);
	}

	/* Step 7: discard all pending reconnection entries. */
	reconnect_heap_clear(&pool->reconnect);

	/*
	 * Step 8: force-close any remaining ACTIVE fds.
	 * These are connections that were not checked in before the drain
	 * timeout.  destroy_fn is not called.  See ARCHITECTURE.md §13.2.
	 */
	for (i = 0; i < pool->table_size; i++) {
		braid_conn_t *conn = &pool->table[i];

		if (conn->fd == -1 || (conn->flags & CONN_FLAG_TOMBSTONE))
			continue;
		/* Only ACTIVE connections remain — all others were cleared. */
		io_unwatch(pool, conn->fd);
		close(conn->fd);
	}

	/* Step 9: free all allocated structures. */
	waitq_destroy(&pool->waitq);
	reaper_heap_destroy(&pool->idle);
	reconnect_heap_destroy(&pool->reconnect);
	table_destroy(pool);
	free((void *)pool->config.host);
	free(pool);
}

/* ── braid_pool_checkout ─────────────────────────────────────────────── */

/*
 * braid_pool_checkout — acquire a connection from the pool.
 *
 * Scans the connection table for a valid IDLE connection.  If one is found
 * and has not exceeded idle_threshold (or passes validate_fn), it is
 * transitioned to ACTIVE and the callback cb is invoked immediately.
 *
 * If no IDLE connection is available and timeout_ms == 0, fires
 * BRAID_EV_POOL_EXHAUSTED and returns BRAID_ERR_EXHAUSTED without invoking
 * the callback.  With timeout_ms > 0, the waiter is enqueued; token is
 * written to *token.  BRAID_EV_POOL_EXHAUSTED fires if the pool is at
 * max_connections capacity.
 *
 * See ARCHITECTURE.md §13, DEVELOPMENT.md §6.4.
 */
int
braid_pool_checkout(braid_pool_t *pool, uint32_t timeout_ms,
		    braid_checkout_cb cb, void *cb_ctx, braid_token_t *token)
{
	uint64_t now_ms;
	uint32_t i;

	if (pool == NULL || cb == NULL)
		return BRAID_ERR_INVAL;

	if (pool->shutting_down)
		return BRAID_ERR_SHUTDOWN;

	now_ms = braid_now_ms();

	/* Scan for a valid IDLE connection. */
	for (i = 0; i < pool->table_size; i++) {
		braid_conn_t *conn = &pool->table[i];

		if (conn->fd == -1 || (conn->flags & CONN_FLAG_TOMBSTONE))
			continue;
		if (conn->state != BRAID_STATE_IDLE)
			continue;

		/*
		 * Call validate_fn only when the idle_threshold has been
		 * exceeded.  See ARCHITECTURE.md §4.3.
		 */
		if (pool->config.validate_fn != NULL &&
		    now_ms >=
			conn->last_active_ms + pool->config.idle_threshold) {
			uint64_t deadline_ms;
			int vrc;

			deadline_ms = now_ms + pool->config.validate_timeout;
			pool->in_callback++;
			vrc = pool->config.validate_fn(
			    conn->fd, conn->conn_ctx, pool->config.hook_context,
			    deadline_ms);
			pool->in_callback--;
			if (pool->in_callback == 0 && pool->deferred_work != 0)
				pool_drain_deferred(pool);

			/*
			 * Enforce validate_timeout even on success: if
			 * validate_fn returned after deadline, treat as
			 * failure.  See ARCHITECTURE.md §17.
			 */
			if (vrc != BRAID_OK || braid_now_ms() > deadline_ms) {
				conn_transition(pool, conn,
						BRAID_STATE_CLOSING);
				continue;
			}
		}

		/* Found a valid IDLE connection — serve immediately. */
		if (io_unwatch(pool, conn->fd) != BRAID_OK) {
			conn_transition(pool, conn, BRAID_STATE_CLOSING);
			continue;
		}

		conn_transition(pool, conn, BRAID_STATE_ACTIVE);
		pool->in_callback++;
		cb(conn->fd, conn->conn_ctx, BRAID_OK, cb_ctx);
		pool->in_callback--;
		if (pool->in_callback == 0 && pool->deferred_work != 0)
			pool_drain_deferred(pool);
		return BRAID_OK;
	}

	/* No IDLE connection available. */
	if (timeout_ms == 0) {
		pool_fire_event(pool, BRAID_EV_POOL_EXHAUSTED);
		return BRAID_ERR_EXHAUSTED;
	}

	/* Enqueue a waiter for when a connection becomes available. */
	{
		braid_token_t tok;
		uint64_t deadline_ms;
		int rc;

		deadline_ms = now_ms + (uint64_t)timeout_ms;
		rc = waitq_enqueue(&pool->waitq, cb, cb_ctx, deadline_ms, &tok);
		if (rc != BRAID_OK)
			return rc;

		if (token != NULL)
			*token = tok;

		/*
		 * Fire BRAID_EV_POOL_EXHAUSTED when every connection slot is
		 * occupied (live_count >= max_connections).  See DEVELOPMENT.md
		 * §6.4.
		 */
		if (pool->live_count >= pool->config.max_connections)
			pool_fire_event(pool, BRAID_EV_POOL_EXHAUSTED);
	}

	return BRAID_OK;
}

/* ── braid_pool_checkin ──────────────────────────────────────────────── */

/*
 * braid_pool_checkin — return or discard an ACTIVE connection.
 *
 * BRAID_CONN_OK: transition ACTIVE→IDLE and serve any pending wait queue
 *   head, subject to the in_callback deferred work protocol.
 * BRAID_CONN_DISCARD: transition ACTIVE→CLOSING→DEAD.
 *
 * Returns BRAID_ERR_INVAL if fd is not a recognised ACTIVE connection.
 * See ARCHITECTURE.md §9, DEVELOPMENT.md §6.4.
 */
int
braid_pool_checkin(braid_pool_t *pool, int fd, int flags)
{
	braid_conn_t *conn;
	int rc;

	if (pool == NULL)
		return BRAID_ERR_INVAL;

	rc = table_lookup(pool, fd, &conn);
	if (rc != BRAID_OK)
		return BRAID_ERR_INVAL;

	BRAID_DEBUG_ASSERT(
	    conn->state == BRAID_STATE_ACTIVE,
	    "braid_pool_checkin: connection not in ACTIVE state");
	if (conn->state != BRAID_STATE_ACTIVE)
		return BRAID_ERR_INVAL;

	if (flags == BRAID_CONN_OK) {
		conn_transition(pool, conn, BRAID_STATE_IDLE);
		rc = io_watch(pool, conn->fd, BRAID_IO_READ);
		if (rc != BRAID_OK) {
			conn_transition(pool, conn, BRAID_STATE_CLOSING);
			return BRAID_ERR_SYSCALL;
		}
		/*
		 * Serve wait queue unless we are inside a callback.
		 * Re-entrancy guard: if in_callback > 0, a checkin from
		 * within a checkout callback defers the serve so that the
		 * outer callback's call stack is not re-entered.
		 * See ARCHITECTURE.md §9.2.
		 */
		if (pool->in_callback == 0)
			pool_serve_waiter(pool);
		else
			pool->deferred_work |= BRAID_DEFERRED_SERVE_WAITQUEUE;
	} else if (flags == BRAID_CONN_DISCARD) {
		conn_transition(pool, conn, BRAID_STATE_CLOSING);
	} else {
		BRAID_DEBUG_ASSERT(0,
				   "braid_pool_checkin: unknown flags value");
		return BRAID_ERR_INVAL;
	}

	return BRAID_OK;
}
/* ── braid_pool_cancel ───────────────────────────────────────────────── */

/*
 * braid_pool_cancel — cancel a pending checkout by token.
 *
 * Wraps waitq_cancel() with the in_callback protocol so that any deferred
 * work triggered by the cancellation callback is drained before we return.
 * If the token is stale (already served, expired, or wrapped around) the
 * cancel is a silent no-op. See ARCHITECTURE.md §9.2, DEVELOPMENT.md §6.5.
 */
int
braid_pool_cancel(braid_pool_t *pool, braid_token_t token)
{
	int rc;

	if (pool == NULL)
		return BRAID_ERR_INVAL;

	pool->in_callback++;
	rc = waitq_cancel(&pool->waitq, token);
	pool->in_callback--;

	if (pool->in_callback == 0 && pool->deferred_work != 0)
		pool_drain_deferred(pool);

	return rc;
}

/* ── braid_pool_advance ─────────────────────────────────────────────────────
 */

/*
 * braid_pool_advance — drive all timer-based pool work.
 *
 * Called once per event-loop iteration, before epoll_wait().  Execution
 * order follows ARCHITECTURE.md §11:
 *   1. Capture now_ms.
 *   2. reconnect_advance() — pop and attempt due reconnections.
 *   2a. Enforce connect_timeout on all CONNECTING connections.
 *   3. reaper_advance() — close idle connections above idle_reap_timeout.
 *   4. waitq_expire_with_hook() — fire BRAID_ERR_TIMEOUT for expired waiters.
 *   5. pool_drain_deferred() if not inside a callback.
 *   6. Write time-until-next-event (ms) to *next_ms; UINT32_MAX if idle.
 *
 * All callbacks fired here follow the in_callback deferred work protocol.
 * See ARCHITECTURE.md §11, DEVELOPMENT.md §6.6.
 */
int
braid_pool_advance(braid_pool_t *pool, uint32_t *next_ms)
{
	uint64_t now_ms;
	uint64_t earliest_ms;
	uint32_t i;

	if (pool == NULL)
		return BRAID_ERR_INVAL;

	now_ms = braid_now_ms();
	earliest_ms = UINT64_MAX;

	/* Step 2: process reconnection heap. */
	reconnect_advance(pool, now_ms);
	{
		braid_reconnect_entry_t entry;

		if (reconnect_heap_peek(&pool->reconnect, &entry) == BRAID_OK)
			if (entry.next_retry_ms < earliest_ms)
				earliest_ms = entry.next_retry_ms;
	}

	/*
	 * Step 2a: abort CONNECTING sockets that have exceeded
	 * connect_timeout.  conn_transition(DEAD) handles io_unwatch,
	 * close, live_count decrement, and reconnect entry insertion.
	 * Track the soonest future deadline for next_ms.
	 */
	for (i = 0; i < pool->table_size; i++) {
		braid_conn_t *conn = &pool->table[i];
		uint64_t deadline_ms;

		if (conn->fd == -1 || (conn->flags & CONN_FLAG_TOMBSTONE))
			continue;
		if (conn->state != BRAID_STATE_CONNECTING)
			continue;

		deadline_ms = conn->created_at_ms +
			      (uint64_t)pool->config.connect_timeout;
		if (now_ms > deadline_ms) {
			conn_transition(pool, conn, BRAID_STATE_DEAD);
		} else {
			if (deadline_ms < earliest_ms)
				earliest_ms = deadline_ms;
		}
	}

	/* Step 3: process idle reaper heap. */
	reaper_advance(pool, now_ms);
	{
		braid_idle_entry_t entry;
		uint64_t timeout_ms;

		timeout_ms = pool->config.idle_reap_timeout != 0
				 ? (uint64_t)pool->config.idle_reap_timeout
				 : 300000;

		if (reaper_heap_peek(&pool->idle, &entry) == BRAID_OK) {
			uint64_t fire_ms = entry.last_active_ms + timeout_ms;

			if (fire_ms < earliest_ms)
				earliest_ms = fire_ms;
		}
	}

	/* Step 4: expire wait queue entries and fire timeout events. */
	waitq_expire_with_hook(&pool->waitq, now_ms,
			       pool_waitq_timeout_event_hook, pool);
	{
		/*
		 * Walk the occupied ring span to find the soonest
		 * non-tombstone entry with a deadline.  The ring is FIFO
		 * so the first live entry with a deadline is the soonest.
		 */
		uint32_t span = pool->waitq.tail - pool->waitq.head;
		uint32_t j;

		for (j = 0; j < span; j++) {
			const braid_waiter_t *slot =
			    &pool->waitq.slots[(pool->waitq.head + j) %
					       pool->waitq.cap];

			if (slot->flags & WAITER_FLAG_TOMBSTONE)
				continue;
			if (slot->deadline_ms == 0)
				continue; /* no timeout */
			if (slot->deadline_ms < earliest_ms)
				earliest_ms = slot->deadline_ms;
			break; /* FIFO: first live entry is soonest */
		}
	}

	/* Step 5: drain any work deferred from callbacks above. */
	if (pool->in_callback == 0 && pool->deferred_work != 0)
		pool_drain_deferred(pool);

	/* Step 6: compute relative next_ms for epoll_wait timeout. */
	if (next_ms != NULL) {
		if (earliest_ms == UINT64_MAX) {
			*next_ms = UINT32_MAX;
		} else if (earliest_ms <= now_ms) {
			*next_ms = 0;
		} else {
			uint64_t delta = earliest_ms - now_ms;

			*next_ms = delta > (uint64_t)UINT32_MAX
				       ? UINT32_MAX
				       : (uint32_t)delta;
		}
	}

	return BRAID_OK;
}

/* ── braid_pool_notify ──────────────────────────────────────────────────── */

/*
 * braid_pool_notify — dispatch an epoll/kqueue event for a libbraid fd.
 *
 * Called by the event loop when epoll_wait() returns an event for a fd
 * tagged with BRAID_FD_MAGIC.  Dispatches by connection state:
 *
 *   CONNECTING + writable: connect() complete or failed.  getsockopt()
 *     checks SO_ERROR.  On error: DEAD.  On success: INITIALIZING,
 *     run init_fn if present (in_callback protocol, deadline from
 *     init_timeout), then IDLE.  io_modify(READ) on success path.
 *
 *   IDLE + readable: probe for half-open via recv(MSG_PEEK).
 *     EAGAIN/EWOULDBLOCK: spurious wakeup, no action.
 *     Any other result: CLOSING (chains to DEAD via conn_transition).
 *
 *   ACTIVE, INITIALIZING, CLOSING, DEAD: silently ignored.
 *
 * Always returns BRAID_OK, including for unrecognised fds (timing artefact).
 * See ARCHITECTURE.md §12, DEVELOPMENT.md §6.7.
 */
int
braid_pool_notify(braid_pool_t *pool, int fd, uint32_t events)
{
	braid_conn_t *conn;

	if (pool == NULL)
		return BRAID_ERR_INVAL;

	(void)events; /* dispatch by state, not event bits */

	if (table_lookup(pool, fd, &conn) != BRAID_OK)
		return BRAID_OK;

	switch (conn->state) {
	case BRAID_STATE_CONNECTING: {
		int so_error = 0;
		socklen_t errlen = sizeof(so_error);

		if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &errlen) !=
			0 ||
		    so_error != 0) {
			conn_transition(pool, conn, BRAID_STATE_DEAD);
			break;
		}

		conn_transition(pool, conn, BRAID_STATE_INITIALIZING);

		if (pool->config.init_fn != NULL) {
			uint64_t timeout_ms;
			uint64_t deadline_ms;
			int init_rc;

			timeout_ms = pool->config.init_timeout != 0
					 ? (uint64_t)pool->config.init_timeout
					 : 10000;
			deadline_ms = braid_now_ms() + timeout_ms;
			pool->in_callback++;
			init_rc = pool->config.init_fn(
			    fd, &conn->conn_ctx, pool->config.hook_context,
			    deadline_ms);
			pool->in_callback--;
			if (pool->in_callback == 0 && pool->deferred_work != 0)
				pool_drain_deferred(pool);

			if (init_rc != BRAID_OK) {
				conn_transition(pool, conn, BRAID_STATE_DEAD);
				break;
			}
			if (braid_now_ms() > deadline_ms) {
				conn_transition(pool, conn, BRAID_STATE_DEAD);
				break;
			}
		}

		conn_transition(pool, conn, BRAID_STATE_IDLE);
		if (io_modify(pool, fd, BRAID_IO_READ) != BRAID_OK)
			conn_transition(pool, conn, BRAID_STATE_CLOSING);
		break;
	}

	case BRAID_STATE_IDLE: {
		char probe;
		ssize_t n;

		n = recv(fd, &probe, 1, MSG_PEEK);
		if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
			break; /* spurious wakeup */

		/* EOF (0), data (1), or error (-1 with other errno):
		 * connection is no longer clean.  CLOSING chains to DEAD
		 * via conn_transition when in_callback == 0. */
		conn_transition(pool, conn, BRAID_STATE_CLOSING);
		break;
	}

	case BRAID_STATE_ACTIVE:
	case BRAID_STATE_INITIALIZING:
	case BRAID_STATE_CLOSING:
	case BRAID_STATE_DEAD:
		break;
	}

	return BRAID_OK;
}
