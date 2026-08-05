/*
 * test_wait_queue.c — unit tests for the wait queue ring buffer
 *
 * Tests: FIFO ordering, tombstone skip on dequeue, cancel by token,
 * cancel noop on served token, timeout expiry scan, expiry stop
 * at first non-expired, shutdown drain, one-callback guarantee under
 * cancel-after-serve and timeout-after-cancel, ring wrap-around, and
 * full-ring rejection.
 *
 * See TESTING.md §3.3 for the full test catalogue.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../include/braid.h"
#include "../src/braid_internal.h"
#include "../src/braid_waitq.h"
#include "test_harness.h"

/* ── callback recorder ───────────────────────────────────────────────── */

/*
 * cb_record — records up to 8 callback invocations for inspection.
 * Each invocation appends (fd, err) to the arrays; call_count increments.
 */
#define MAX_RECORDED 8

typedef struct {
	int fd[MAX_RECORDED];
	int err[MAX_RECORDED];
	int call_count;
} cb_recorder_t;

static void
recording_cb(int fd, void *conn_ctx, int err, void *cb_ctx)
{
	cb_recorder_t *rec = cb_ctx;
	(void)conn_ctx;

	if (rec->call_count < MAX_RECORDED) {
		rec->fd[rec->call_count] = fd;
		rec->err[rec->call_count] = err;
	}
	rec->call_count++;
}

/* ── ring helpers ────────────────────────────────────────────────────── */

static braid_ring_t *
make_ring(uint32_t cap)
{
	braid_ring_t *ring;

	ring = calloc(1, sizeof(*ring));
	if (ring == NULL)
		return NULL;
	if (waitq_init(ring, cap) != BRAID_OK) {
		free(ring);
		return NULL;
	}
	return ring;
}

static void
free_ring(braid_ring_t *ring)
{
	waitq_destroy(ring);
	free(ring);
}

/*
 * waitq_init must reject cap=0 to avoid modulo-by-zero on enqueue/cancel.
 */
static void
test_init_zero_capacity_rejected(void)
{
	braid_ring_t ring;

	memset(&ring, 0, sizeof(ring));
	CHECK_ERR("init-zero: cap=0 rejected", waitq_init(&ring, 0),
		  BRAID_ERR_INVAL);
}

/* ── test cases ──────────────────────────────────────────────────────── */

/*
 * Enqueue three waiters and serve them in order.
 * Verifies FIFO: recorder A receives fd=10, B receives fd=20, C fd=30.
 */
static void
test_enqueue_dequeue_fifo(void)
{
	braid_ring_t *ring;
	cb_recorder_t recA, recB, recC;
	braid_token_t tok;

	memset(&recA, 0, sizeof(recA));
	memset(&recB, 0, sizeof(recB));
	memset(&recC, 0, sizeof(recC));

	ring = make_ring(4);
	if (ring == NULL) {
		tests_failed++;
		return;
	}

	CHECK_ERR("fifo: enqueue A",
		  waitq_enqueue(ring, recording_cb, &recA, 0, &tok), BRAID_OK);
	CHECK_ERR("fifo: enqueue B",
		  waitq_enqueue(ring, recording_cb, &recB, 0, &tok), BRAID_OK);
	CHECK_ERR("fifo: enqueue C",
		  waitq_enqueue(ring, recording_cb, &recC, 0, &tok), BRAID_OK);

	CHECK_ERR("fifo: serve A", waitq_serve_head(ring, 10, NULL), BRAID_OK);
	CHECK_ERR("fifo: serve B", waitq_serve_head(ring, 20, NULL), BRAID_OK);
	CHECK_ERR("fifo: serve C", waitq_serve_head(ring, 30, NULL), BRAID_OK);

	CHECK("fifo: A called once", recA.call_count == 1);
	CHECK("fifo: A fd=10", recA.fd[0] == 10);
	CHECK("fifo: A err=OK", recA.err[0] == BRAID_OK);
	CHECK("fifo: B called once", recB.call_count == 1);
	CHECK("fifo: B fd=20", recB.fd[0] == 20);
	CHECK("fifo: C called once", recC.call_count == 1);
	CHECK("fifo: C fd=30", recC.fd[0] == 30);
	CHECK("fifo: ring empty", ring->count == 0);

	free_ring(ring);
}

/*
 * Enqueue A and B. Cancel A (tombstone it). Serve head — B must be served,
 * A must not be called a second time.
 */
static void
test_tombstone_skip_on_dequeue(void)
{
	braid_ring_t *ring;
	cb_recorder_t recA, recB;
	braid_token_t tokA;

	memset(&recA, 0, sizeof(recA));
	memset(&recB, 0, sizeof(recB));

	ring = make_ring(4);
	if (ring == NULL) {
		tests_failed++;
		return;
	}

	/* tokA is unused after cancel — just proves the cancel path works. */
	braid_token_t dummy;
	CHECK_ERR("skip: enqueue A",
		  waitq_enqueue(ring, recording_cb, &recA, 0, &tokA), BRAID_OK);
	CHECK_ERR("skip: enqueue B",
		  waitq_enqueue(ring, recording_cb, &recB, 0, &dummy),
		  BRAID_OK);

	/* Cancel A — A's slot is tombstoned, callback fires with CANCELLED. */
	CHECK_ERR("skip: cancel A", waitq_cancel(ring, tokA), BRAID_OK);
	CHECK("skip: A got CANCELLED", recA.err[0] == BRAID_ERR_CANCELLED);

	/* Serve head — should skip A's tombstone and deliver to B. */
	CHECK_ERR("skip: serve head", waitq_serve_head(ring, 42, NULL),
		  BRAID_OK);

	CHECK("skip: A called exactly once", recA.call_count == 1);
	CHECK("skip: B called once", recB.call_count == 1);
	CHECK("skip: B fd=42", recB.fd[0] == 42);
	CHECK("skip: B err=OK", recB.err[0] == BRAID_OK);
	CHECK("skip: ring empty after serve", ring->count == 0);

	free_ring(ring);
}

/*
 * Enqueue a waiter, cancel it. Verify callback was invoked with
 * BRAID_ERR_CANCELLED, count decremented, slot tombstoned.
 */
static void
test_cancel_by_token(void)
{
	braid_ring_t *ring;
	cb_recorder_t rec;
	braid_token_t tok;

	memset(&rec, 0, sizeof(rec));

	ring = make_ring(4);
	if (ring == NULL) {
		tests_failed++;
		return;
	}

	CHECK_ERR("cancel: enqueue",
		  waitq_enqueue(ring, recording_cb, &rec, 0, &tok), BRAID_OK);
	CHECK("cancel: count=1 before cancel", ring->count == 1);

	CHECK_ERR("cancel: cancel", waitq_cancel(ring, tok), BRAID_OK);

	CHECK("cancel: called once", rec.call_count == 1);
	CHECK("cancel: err=CANCELLED", rec.err[0] == BRAID_ERR_CANCELLED);
	CHECK("cancel: fd=-1", rec.fd[0] == -1);
	CHECK("cancel: count=0 after", ring->count == 0);

	free_ring(ring);
}

/*
 * Enqueue a waiter, serve it, then cancel with the same token.
 * The second cancel must be a no-op (slot is tombstoned from serve).
 */
static void
test_cancel_already_served_noop(void)
{
	braid_ring_t *ring;
	cb_recorder_t rec;
	braid_token_t tok;

	memset(&rec, 0, sizeof(rec));

	ring = make_ring(4);
	if (ring == NULL) {
		tests_failed++;
		return;
	}

	CHECK_ERR("cancel-noop: enqueue",
		  waitq_enqueue(ring, recording_cb, &rec, 0, &tok), BRAID_OK);
	CHECK_ERR("cancel-noop: serve", waitq_serve_head(ring, 7, NULL),
		  BRAID_OK);
	CHECK("cancel-noop: called once after serve", rec.call_count == 1);

	/* Token now refers to a tombstoned slot — cancel must be silent. */
	CHECK_ERR("cancel-noop: stale cancel", waitq_cancel(ring, tok),
		  BRAID_OK);
	CHECK("cancel-noop: still called once", rec.call_count == 1);

	free_ring(ring);
}

/*
 * Enqueue two waiters with past deadlines and one with a future deadline.
 * Call waitq_expire(now=100). Verify the two expired entries fire with
 * BRAID_ERR_TIMEOUT and the future entry is untouched.
 */
static void
test_timeout_expiry_scan(void)
{
	braid_ring_t *ring;
	cb_recorder_t recA, recB, recC;
	braid_token_t tok;

	memset(&recA, 0, sizeof(recA));
	memset(&recB, 0, sizeof(recB));
	memset(&recC, 0, sizeof(recC));

	ring = make_ring(4);
	if (ring == NULL) {
		tests_failed++;
		return;
	}

	/* deadline 50 — expired at now=100 */
	CHECK_ERR("expire: enqueue A (past)",
		  waitq_enqueue(ring, recording_cb, &recA, 50, &tok), BRAID_OK);
	/* deadline 100 — exactly at boundary, expired (<=) */
	CHECK_ERR("expire: enqueue B (past)",
		  waitq_enqueue(ring, recording_cb, &recB, 100, &tok),
		  BRAID_OK);
	/* deadline 200 — future */
	CHECK_ERR("expire: enqueue C (future)",
		  waitq_enqueue(ring, recording_cb, &recC, 200, &tok),
		  BRAID_OK);

	waitq_expire(ring, 100);

	CHECK("expire: A timed out", recA.call_count == 1);
	CHECK("expire: A err=TIMEOUT", recA.err[0] == BRAID_ERR_TIMEOUT);
	CHECK("expire: B timed out", recB.call_count == 1);
	CHECK("expire: B err=TIMEOUT", recB.err[0] == BRAID_ERR_TIMEOUT);
	CHECK("expire: C not touched", recC.call_count == 0);
	CHECK("expire: count=1 remaining", ring->count == 1);

	free_ring(ring);
}

/*
 * Enqueue a future waiter first, then an expired waiter behind it.
 * Call waitq_expire. The scan must stop at the first non-expired entry
 * (the future one), leaving both entries intact.
 */
static void
test_expiry_stops_at_first_nonfired(void)
{
	braid_ring_t *ring;
	cb_recorder_t recFuture, recPast;
	braid_token_t tok;

	memset(&recFuture, 0, sizeof(recFuture));
	memset(&recPast, 0, sizeof(recPast));

	ring = make_ring(4);
	if (ring == NULL) {
		tests_failed++;
		return;
	}

	/* Future entry is at head. */
	CHECK_ERR("stop: enqueue future",
		  waitq_enqueue(ring, recording_cb, &recFuture, 999, &tok),
		  BRAID_OK);
	/* Expired entry is behind it. */
	CHECK_ERR("stop: enqueue past",
		  waitq_enqueue(ring, recording_cb, &recPast, 1, &tok),
		  BRAID_OK);

	waitq_expire(ring, 100);

	CHECK("stop: future not called", recFuture.call_count == 0);
	CHECK("stop: past not called", recPast.call_count == 0);
	CHECK("stop: count=2 unchanged", ring->count == 2);

	free_ring(ring);
}

/*
 * Enqueue three waiters. Call waitq_shutdown. All three must receive
 * BRAID_ERR_SHUTDOWN callbacks; count must be 0 and ring must be empty.
 */
static void
test_shutdown_drains_all(void)
{
	braid_ring_t *ring;
	cb_recorder_t recA, recB, recC;
	braid_token_t tok;

	memset(&recA, 0, sizeof(recA));
	memset(&recB, 0, sizeof(recB));
	memset(&recC, 0, sizeof(recC));

	ring = make_ring(4);
	if (ring == NULL) {
		tests_failed++;
		return;
	}

	CHECK_ERR("shutdown: enqueue A",
		  waitq_enqueue(ring, recording_cb, &recA, 0, &tok), BRAID_OK);
	CHECK_ERR("shutdown: enqueue B",
		  waitq_enqueue(ring, recording_cb, &recB, 0, &tok), BRAID_OK);
	CHECK_ERR("shutdown: enqueue C",
		  waitq_enqueue(ring, recording_cb, &recC, 0, &tok), BRAID_OK);

	waitq_shutdown(ring);

	CHECK("shutdown: A called", recA.call_count == 1);
	CHECK("shutdown: A err=SHUTDOWN", recA.err[0] == BRAID_ERR_SHUTDOWN);
	CHECK("shutdown: B called", recB.call_count == 1);
	CHECK("shutdown: B err=SHUTDOWN", recB.err[0] == BRAID_ERR_SHUTDOWN);
	CHECK("shutdown: C called", recC.call_count == 1);
	CHECK("shutdown: C err=SHUTDOWN", recC.err[0] == BRAID_ERR_SHUTDOWN);
	CHECK("shutdown: count=0", ring->count == 0);
	CHECK("shutdown: head==tail", ring->head == ring->tail);

	free_ring(ring);
}

/*
 * One-callback guarantee — cancel after serve.
 * Serve a waiter so it fires BRAID_OK, then cancel with the same token.
 * Total call count must remain 1.
 */
static void
test_one_callback_cancel_after_serve(void)
{
	braid_ring_t *ring;
	cb_recorder_t rec;
	braid_token_t tok;

	memset(&rec, 0, sizeof(rec));

	ring = make_ring(4);
	if (ring == NULL) {
		tests_failed++;
		return;
	}

	CHECK_ERR("1cb-cs: enqueue",
		  waitq_enqueue(ring, recording_cb, &rec, 0, &tok), BRAID_OK);
	CHECK_ERR("1cb-cs: serve", waitq_serve_head(ring, 5, NULL), BRAID_OK);
	CHECK_ERR("1cb-cs: cancel", waitq_cancel(ring, tok), BRAID_OK);

	CHECK("1cb-cs: exactly one call", rec.call_count == 1);
	CHECK("1cb-cs: first call OK", rec.err[0] == BRAID_OK);

	free_ring(ring);
}

/*
 * One-callback guarantee — timeout after cancel.
 * Cancel a waiter (fires CANCELLED), then call expire with a past now_ms.
 * Total call count must remain 1.
 */
static void
test_one_callback_timeout_after_cancel(void)
{
	braid_ring_t *ring;
	cb_recorder_t rec;
	braid_token_t tok;

	memset(&rec, 0, sizeof(rec));

	ring = make_ring(4);
	if (ring == NULL) {
		tests_failed++;
		return;
	}

	CHECK_ERR("1cb-tc: enqueue",
		  waitq_enqueue(ring, recording_cb, &rec, 50, &tok), BRAID_OK);
	CHECK_ERR("1cb-tc: cancel", waitq_cancel(ring, tok), BRAID_OK);
	CHECK("1cb-tc: CANCELLED after cancel",
	      rec.err[0] == BRAID_ERR_CANCELLED);

	/* now_ms=1000 — would expire the slot if it weren't already tombstoned.
	 */
	waitq_expire(ring, 1000);

	CHECK("1cb-tc: exactly one call", rec.call_count == 1);

	free_ring(ring);
}

/*
 * Ring wrap-around: use cap=4. Enqueue 4 entries, serve all 4 (head wraps).
 * Enqueue 4 more (tail wraps). Serve all 4 again. Verify correct delivery.
 */
static void
test_ring_wraparound(void)
{
	braid_ring_t *ring;
	cb_recorder_t recs[8];
	braid_token_t tok;
	int i;

	for (i = 0; i < 8; i++)
		memset(&recs[i], 0, sizeof(recs[i]));

	ring = make_ring(4);
	if (ring == NULL) {
		tests_failed++;
		return;
	}

	/* First batch: fill ring and drain completely. */
	for (i = 0; i < 4; i++)
		CHECK_ERR("wrap: enqueue first batch",
			  waitq_enqueue(ring, recording_cb, &recs[i], 0, &tok),
			  BRAID_OK);
	for (i = 0; i < 4; i++)
		CHECK_ERR("wrap: serve first batch",
			  waitq_serve_head(ring, i + 10, NULL), BRAID_OK);

	CHECK("wrap: ring empty after first drain", ring->count == 0);

	/* Second batch: tail and head must wrap around modulo cap=4. */
	for (i = 0; i < 4; i++)
		CHECK_ERR(
		    "wrap: enqueue second batch",
		    waitq_enqueue(ring, recording_cb, &recs[4 + i], 0, &tok),
		    BRAID_OK);

	CHECK("wrap: count=4 before second drain", ring->count == 4);

	for (i = 0; i < 4; i++)
		CHECK_ERR("wrap: serve second batch",
			  waitq_serve_head(ring, i + 20, NULL), BRAID_OK);

	/* Verify each recorder in the second batch got exactly one call. */
	for (i = 0; i < 4; i++) {
		CHECK("wrap: second-batch entry served once",
		      recs[4 + i].call_count == 1);
		CHECK("wrap: second-batch entry err=OK",
		      recs[4 + i].err[0] == BRAID_OK);
		CHECK("wrap: second-batch fd correct",
		      recs[4 + i].fd[0] == i + 20);
	}

	CHECK("wrap: ring empty after second drain", ring->count == 0);

	free_ring(ring);
}

/*
 * Full ring rejects enqueue.
 * Fill a cap=2 ring. The third enqueue must return BRAID_ERR_EXHAUSTED
 * without invoking any callback or modifying the ring.
 */
static void
test_full_ring_rejects_enqueue(void)
{
	braid_ring_t *ring;
	cb_recorder_t recA, recB, recExtra;
	braid_token_t tok;

	memset(&recA, 0, sizeof(recA));
	memset(&recB, 0, sizeof(recB));
	memset(&recExtra, 0, sizeof(recExtra));

	ring = make_ring(2);
	if (ring == NULL) {
		tests_failed++;
		return;
	}

	CHECK_ERR("full: enqueue A",
		  waitq_enqueue(ring, recording_cb, &recA, 0, &tok), BRAID_OK);
	CHECK_ERR("full: enqueue B",
		  waitq_enqueue(ring, recording_cb, &recB, 0, &tok), BRAID_OK);
	CHECK("full: count=2", ring->count == 2);

	CHECK_ERR("full: enqueue extra rejected",
		  waitq_enqueue(ring, recording_cb, &recExtra, 0, &tok),
		  BRAID_ERR_EXHAUSTED);

	CHECK("full: count still 2 after rejection", ring->count == 2);
	CHECK("full: extra callback not invoked", recExtra.call_count == 0);

	free_ring(ring);
}

/*
 * Cancelling a later entry leaves a physical tombstone behind the live head.
 * An enqueue must not wrap and overwrite that live head; after the head is
 * served, both its slot and the following tombstone are reclaimable.
 */
static void
test_cancelled_tail_preserves_live_head(void)
{
	braid_ring_t *ring;
	cb_recorder_t recA, recB, recC;
	braid_token_t tokA, tokB, tokC;

	memset(&recA, 0, sizeof(recA));
	memset(&recB, 0, sizeof(recB));
	memset(&recC, 0, sizeof(recC));
	ring = make_ring(2);
	if (ring == NULL) {
		tests_failed++;
		return;
	}

	CHECK_ERR("cancel-tail: enqueue A",
		  waitq_enqueue(ring, recording_cb, &recA, 0, &tokA), BRAID_OK);
	CHECK_ERR("cancel-tail: enqueue B",
		  waitq_enqueue(ring, recording_cb, &recB, 0, &tokB), BRAID_OK);
	CHECK_ERR("cancel-tail: cancel B", waitq_cancel(ring, tokB), BRAID_OK);
	CHECK_ERR("cancel-tail: enqueue C rejected",
		  waitq_enqueue(ring, recording_cb, &recC, 0, &tokC),
		  BRAID_ERR_EXHAUSTED);
	CHECK_ERR("cancel-tail: serve A", waitq_serve_head(ring, 42, NULL),
		  BRAID_OK);
	CHECK("cancel-tail: A served once", recA.call_count == 1);
	CHECK("cancel-tail: A receives fd", recA.fd[0] == 42);
	CHECK("cancel-tail: B cancelled once", recB.call_count == 1);
	CHECK("cancel-tail: C not called", recC.call_count == 0);
	CHECK_ERR("cancel-tail: enqueue C after reclaim",
		  waitq_enqueue(ring, recording_cb, &recC, 0, &tokC), BRAID_OK);
	CHECK_ERR("cancel-tail: serve C", waitq_serve_head(ring, 43, NULL),
		  BRAID_OK);
	CHECK("cancel-tail: C served once", recC.call_count == 1);
	CHECK("cancel-tail: C receives fd", recC.fd[0] == 43);

	free_ring(ring);
}

/* BRAID_TOKEN_NONE never identifies an uninitialised or live waiter. */
static void
test_none_token_is_noop(void)
{
	braid_ring_t *ring;
	cb_recorder_t rec;

	memset(&rec, 0, sizeof(rec));
	ring = make_ring(2);
	if (ring == NULL) {
		tests_failed++;
		return;
	}

	CHECK_ERR("none-token: cancel is no-op",
		  waitq_cancel(ring, BRAID_TOKEN_NONE), BRAID_OK);
	CHECK("none-token: callback not invoked", rec.call_count == 0);
	CHECK("none-token: ring unchanged", ring->count == 0);

	free_ring(ring);
}

/* ── suite entry point ───────────────────────────────────────────────── */

void
run_wait_queue_tests(void)
{
	test_init_zero_capacity_rejected();
	test_enqueue_dequeue_fifo();
	test_tombstone_skip_on_dequeue();
	test_cancel_by_token();
	test_cancel_already_served_noop();
	test_timeout_expiry_scan();
	test_expiry_stops_at_first_nonfired();
	test_shutdown_drains_all();
	test_one_callback_cancel_after_serve();
	test_one_callback_timeout_after_cancel();
	test_ring_wraparound();
	test_full_ring_rejects_enqueue();
	test_cancelled_tail_preserves_live_head();
	test_none_token_is_noop();
}
