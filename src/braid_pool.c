/*
 * braid_pool.c — pool lifecycle and public API implementation
 *
 * braid_pool_create(), braid_pool_destroy(), braid_pool_checkout(),
 * braid_pool_checkin(), braid_pool_cancel(), braid_pool_advance(),
 * braid_pool_notify().
 *
 * See ARCHITECTURE.md §11, §12, §13.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
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

/*
 * pool_drain_deferred — process all flagged deferred work.
 *
 * Must be called with pool->in_callback == 0.  Clears each flag before
 * dispatching its work so that nested callbacks set the flag again if
 * needed rather than relying on the current pass.
 *
 * Processing order is fixed by ARCHITECTURE.md §9.2:
 *   1. BRAID_DEFERRED_PROCESS_DEAD  — close fds, vacate slots, insert
 *      reconnection entries for any connections whose DEAD transition was
 *      deferred by CONN_FLAG_CLOSING_DEFERRED. (Full implementation in
 *      Phase 6 — stub in Phase 4.)
 *   2. BRAID_DEFERRED_SERVE_WAITQUEUE — serve the head of the wait queue,
 *      which may invoke a callback that is safe here because in_callback
 *      is 0 at this point. (Full implementation in Phase 6 — stub in
 *      Phase 4.)
 */
void
pool_drain_deferred(braid_pool_t *pool)
{
	if (pool->deferred_work & BRAID_DEFERRED_PROCESS_DEAD) {
		pool->deferred_work &= ~BRAID_DEFERRED_PROCESS_DEAD;
		/* Phase 6: iterate table, call conn_transition(→ DEAD)
		 * for every conn with CONN_FLAG_CLOSING_DEFERRED set. */
	}

	if (pool->deferred_work & BRAID_DEFERRED_SERVE_WAITQUEUE) {
		pool->deferred_work &= ~BRAID_DEFERRED_SERVE_WAITQUEUE;
		/* Phase 6: call waitq_serve_head() if queue non-empty. */
	}
}

/* ── pool_fire_event ────────────────────────────────────────────────── */

/*
 * pool_fire_event — fire a pool-level observable event via observe_fn.
 * Does NOT use the in_callback protocol — observe_fn is expected to be
 * a passive logging/metrics hook that does not call back into libbraid.
 * Pool-level events carry no connection fd; fd is set to -1.
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
	pool->config.observe_fn(&ev, pool->config.hook_context);
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
		braid_conn_t *c = &pool->table[i];

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
	 * Seed per-pool PRNG via getentropy().  Non-zero is required for
	 * the xorshift64 algorithm in pool_prng_next().  If getentropy()
	 * returns zero bits (astronomically unlikely), fall back to a
	 * non-zero constant.  See ARCHITECTURE.md §6.2.
	 */
	getentropy(&pool->prng, sizeof(pool->prng));
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

		deadline_ms = braid_now_ms() + drain_timeout_ms;
		ts.tv_sec = 0;
		ts.tv_nsec = 10 * 1000 * 1000; /* 10 ms */
		while (pool_active_count(pool) > 0) {
			if (braid_now_ms() >= deadline_ms)
				break;
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

		if (pool->config.observe_fn != NULL) {
			braid_event_t ev;

			memset(&ev, 0, sizeof(ev));
			ev.type = BRAID_EV_CONN_DESTROYED;
			ev.fd = fd;
			pool->config.observe_fn(&ev, pool->config.hook_context);
		}
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

	pool->in_callback++;
	rc = waitq_cancel(&pool->waitq, token);
	pool->in_callback--;

	if (pool->in_callback == 0 && pool->deferred_work != 0)
		pool_drain_deferred(pool);

	return rc;
}