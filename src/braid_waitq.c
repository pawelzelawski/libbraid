/*
 * braid_waitq.c — wait queue ring buffer: enqueue, dequeue, cancel, expiry
 *
 * FIFO ring buffer of pending checkout requests. Cancellable via token.
 * Tombstone mechanism ensures one callback per checkout regardless of outcome.
 * See ARCHITECTURE.md §10.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../include/braid.h"
#include "braid_internal.h"
#include "braid_waitq.h"

/*
 * waitq_init — allocate cap waiter slots and reset all indices to zero.
 * Called once from braid_pool_create(). No allocation after this point.
 */
int
waitq_init(braid_ring_t *ring, uint32_t cap)
{
	ring->slots = malloc(cap * sizeof(braid_waiter_t));
	if (ring->slots == NULL)
		return BRAID_ERR_NOMEM;
	ring->head = 0;
	ring->tail = 0;
	ring->count = 0;
	ring->cap = cap;
	return BRAID_OK;
}

/*
 * waitq_destroy — free the slot array. Called from braid_pool_destroy().
 */
void
waitq_destroy(braid_ring_t *ring)
{
	free(ring->slots);
	ring->slots = NULL;
}

/*
 * waitq_enqueue — append a pending checkout request to the tail of the ring.
 * Returns BRAID_ERR_EXHAUSTED if count has reached cap (ring full).
 * The token written to *token is the ring index at write time (ring->tail
 * before advance); callers pass this to waitq_cancel() to cancel the waiter.
 */
int
waitq_enqueue(braid_ring_t *ring, braid_checkout_cb cb, void *cb_ctx,
	      uint64_t deadline_ms, braid_token_t *token)
{
	braid_waiter_t *slot;

	if (ring->count >= ring->cap)
		return BRAID_ERR_EXHAUSTED;

	slot = &ring->slots[ring->tail % ring->cap];
	slot->cb = cb;
	slot->cb_ctx = cb_ctx;
	slot->deadline_ms = deadline_ms;
	slot->token = ring->tail;
	slot->flags = 0;
	*token = ring->tail;
	ring->tail++;
	ring->count++;
	return BRAID_OK;
}

/*
 * waitq_serve_head — dequeue and invoke the oldest live waiter with fd.
 * Tombstoned head slots are skipped (their count was already decremented
 * at tombstone time). The served slot is tombstoned before the callback
 * fires to guarantee at most one invocation per checkout.
 */
int
waitq_serve_head(braid_ring_t *ring, int fd, void *conn_ctx)
{
	braid_waiter_t *slot;

	/* Drain stale tombstones from the front without touching count. */
	while (ring->count > 0) {
		slot = &ring->slots[ring->head % ring->cap];
		if (!(slot->flags & WAITER_FLAG_TOMBSTONE))
			break;
		ring->head++;
	}

	if (ring->count == 0)
		return BRAID_OK;

	slot = &ring->slots[ring->head % ring->cap];
	slot->flags |= WAITER_FLAG_TOMBSTONE;
	ring->count--;
	ring->head++;
	/* SAFETY: tombstone before callback; prevents double-invocation. */
	slot->cb(fd, conn_ctx, BRAID_OK, slot->cb_ctx);
	return BRAID_OK;
}

/*
 * waitq_cancel — tombstone the waiter identified by token and invoke its
 * callback with BRAID_ERR_CANCELLED. If the token no longer matches the
 * slot (served, expired, or wrapped around), the cancel is a no-op.
 */
int
waitq_cancel(braid_ring_t *ring, braid_token_t token)
{
	braid_waiter_t *slot;

	slot = &ring->slots[token % ring->cap];
	if (slot->token != token || (slot->flags & WAITER_FLAG_TOMBSTONE))
		return BRAID_OK;

	slot->flags |= WAITER_FLAG_TOMBSTONE;
	ring->count--;
	/* SAFETY: tombstone before callback; prevents double-invocation. */
	slot->cb(-1, NULL, BRAID_ERR_CANCELLED, slot->cb_ctx);
	return BRAID_OK;
}

/*
 * waitq_expire — scan from head and invoke BRAID_ERR_TIMEOUT for every
 * non-tombstone waiter whose deadline_ms <= now_ms (deadline_ms == 0
 * means no timeout and is never expired). Stops at the first non-tombstone
 * entry whose deadline has not yet passed.
 */
void
waitq_expire(braid_ring_t *ring, uint64_t now_ms)
{
	braid_waiter_t *slot;

	while (ring->count > 0) {
		slot = &ring->slots[ring->head % ring->cap];

		/* Drain stale tombstones from the front. */
		if (slot->flags & WAITER_FLAG_TOMBSTONE) {
			ring->head++;
			continue;
		}

		/* deadline_ms == 0 means no timeout; stop on non-expired. */
		if (slot->deadline_ms == 0 || slot->deadline_ms > now_ms)
			break;

		slot->flags |= WAITER_FLAG_TOMBSTONE;
		ring->count--;
		ring->head++;
		/* SAFETY: tombstone before callback; prevents
		 * double-invocation. */
		slot->cb(-1, NULL, BRAID_ERR_TIMEOUT, slot->cb_ctx);
	}
}

/*
 * waitq_shutdown — invoke BRAID_ERR_SHUTDOWN for every non-tombstone waiter
 * in the ring. Scans the full occupied span [head, tail). Resets count to 0
 * and advances head to tail so the ring is empty on return.
 */
void
waitq_shutdown(braid_ring_t *ring)
{
	braid_waiter_t *slot;
	uint32_t i;
	uint32_t span;

	/*
	 * span covers all occupied positions: live and already-tombstoned.
	 * Subtraction wraps correctly for uint32_t when tail < head (overflow).
	 */
	span = ring->tail - ring->head;
	for (i = 0; i < span; i++) {
		slot = &ring->slots[(ring->head + i) % ring->cap];
		if (slot->flags & WAITER_FLAG_TOMBSTONE)
			continue;
		slot->flags |= WAITER_FLAG_TOMBSTONE;
		/* SAFETY: tombstone before callback; prevents
		 * double-invocation. */
		slot->cb(-1, NULL, BRAID_ERR_SHUTDOWN, slot->cb_ctx);
	}
	ring->count = 0;
	ring->head = ring->tail;
}
