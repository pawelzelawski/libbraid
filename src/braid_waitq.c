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
	if (cap == 0)
		return BRAID_ERR_INVAL;

	ring->slots = calloc(cap, sizeof(braid_waiter_t));
	if (ring->slots == NULL)
		return BRAID_ERR_NOMEM;
	ring->head = 0;
	ring->tail = 0;
	ring->count = 0;
	ring->cap = cap;
	/* Keep token % cap aligned with the first physical slot (tail == 0). */
	ring->next_token = (braid_token_t)cap;
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
 * waitq_discard_head_tombstones -- reclaim tombstoned physical slots that
 * have reached the ring head. A tombstone behind a live waiter cannot be
 * reused without overwriting a live entry or breaking FIFO order.
 */
static void
waitq_discard_head_tombstones(braid_ring_t *ring)
{
	while (ring->head != ring->tail) {
		const braid_waiter_t *slot =
		    &ring->slots[ring->head % ring->cap];

		if (!(slot->flags & WAITER_FLAG_TOMBSTONE))
			break;
		ring->head++;
	}
}

/*
 * waitq_enqueue — append a pending checkout request to the tail of the ring.
 * Returns BRAID_ERR_EXHAUSTED if the physical ring span has reached cap.
 * The token written to *token is a monotonic opaque identifier; callers pass
 * it to waitq_cancel() to cancel the waiter.
 */
int
waitq_enqueue(braid_ring_t *ring, braid_checkout_cb cb, void *cb_ctx,
	      uint64_t deadline_ms, braid_token_t *token)
{
	braid_waiter_t *slot;

	waitq_discard_head_tombstones(ring);
	if (ring->tail - ring->head >= ring->cap)
		return BRAID_ERR_EXHAUSTED;

	slot = &ring->slots[ring->tail % ring->cap];
	slot->cb = cb;
	slot->cb_ctx = cb_ctx;
	slot->deadline_ms = deadline_ms;
	slot->token = ring->next_token;
	slot->flags = 0;
	*token = ring->next_token;
	ring->next_token++;
	if (ring->next_token == BRAID_TOKEN_NONE)
		ring->next_token++;
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
	waitq_discard_head_tombstones(ring);

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

	if (token == BRAID_TOKEN_NONE)
		return BRAID_OK;

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
 * waitq_expire_with_hook — scan from head and invoke BRAID_ERR_TIMEOUT for
 * every non-tombstone waiter whose deadline_ms <= now_ms (deadline_ms == 0
 * means no timeout and is never expired). Stops at the first non-tombstone
 * entry whose deadline has not yet passed.
 *
 * If hook is non-NULL, it is called immediately before each timeout callback.
 */
void
waitq_expire_with_hook(braid_ring_t *ring, uint64_t now_ms,
		       waitq_expire_hook_fn hook, void *hook_ctx)
{
	while (ring->count > 0) {
		braid_waiter_t *slot = &ring->slots[ring->head % ring->cap];

		/* Drain stale tombstones from the front. */
		if (slot->flags & WAITER_FLAG_TOMBSTONE) {
			waitq_discard_head_tombstones(ring);
			continue;
		}

		/* deadline_ms == 0 means no timeout; stop on non-expired. */
		if (slot->deadline_ms == 0 || slot->deadline_ms > now_ms)
			break;

		slot->flags |= WAITER_FLAG_TOMBSTONE;
		ring->count--;
		ring->head++;
		if (hook != NULL)
			hook(hook_ctx);
		/* SAFETY: tombstone before callback; prevents
		 * double-invocation. */
		slot->cb(-1, NULL, BRAID_ERR_TIMEOUT, slot->cb_ctx);
	}
}

/*
 * waitq_expire — convenience wrapper around waitq_expire_with_hook with no
 * hook. Use when timeout expiry requires no side effects beyond the callback.
 */
void
waitq_expire(braid_ring_t *ring, uint64_t now_ms)
{
	waitq_expire_with_hook(ring, now_ms, NULL, NULL);
}

/*
 * waitq_fail_all — invoke err for every waiter present at entry. Advances the
 * head before each callback so callback re-entrancy may safely enqueue new
 * waiters beyond the captured tail without being consumed by this drain.
 */
void
waitq_fail_all(braid_ring_t *ring, int err)
{
	uint32_t end = ring->tail;

	while (ring->head != end) {
		braid_waiter_t *slot = &ring->slots[ring->head % ring->cap];

		ring->head++;
		if (slot->flags & WAITER_FLAG_TOMBSTONE)
			continue;
		slot->flags |= WAITER_FLAG_TOMBSTONE;
		ring->count--;
		/* SAFETY: tombstone before callback; prevents
		 * double-invocation. */
		slot->cb(-1, NULL, err, slot->cb_ctx);
	}
}

/* Invoke BRAID_ERR_SHUTDOWN for every waiter present at shutdown entry. */
void
waitq_shutdown(braid_ring_t *ring)
{
	waitq_fail_all(ring, BRAID_ERR_SHUTDOWN);
}
