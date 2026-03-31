/*
 * braid_waitq.h — wait queue internal interface (internal)
 *
 * Fixed-size ring buffer of pending checkout requests.
 * See ARCHITECTURE.md §10.
 */

#ifndef BRAID_WAITQ_H
#define BRAID_WAITQ_H

#include <stdint.h>

#include "../include/braid.h"
#include "braid_internal.h"

int waitq_init(braid_ring_t *ring, uint32_t cap);
void waitq_destroy(braid_ring_t *ring);
int waitq_enqueue(braid_ring_t *ring, braid_checkout_cb cb, void *cb_ctx,
		  uint64_t deadline_ms, braid_token_t *token);
int waitq_serve_head(braid_ring_t *ring, int fd, void *conn_ctx);
int waitq_cancel(braid_ring_t *ring, braid_token_t token);
void waitq_expire(braid_ring_t *ring, uint64_t now_ms);
void waitq_shutdown(braid_ring_t *ring);

#endif /* BRAID_WAITQ_H */
