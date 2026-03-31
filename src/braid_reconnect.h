/*
 * braid_reconnect.h — reconnection engine internal interface (internal)
 *
 * Min-heap of pending reconnections and full jitter backoff.
 * See ARCHITECTURE.md §6.
 */

#ifndef BRAID_RECONNECT_H
#define BRAID_RECONNECT_H

#include <stdint.h>

#include "../include/braid.h"
#include "braid_internal.h"

int reconnect_heap_init(braid_reconnect_heap_t *heap, uint32_t cap);
void reconnect_heap_destroy(braid_reconnect_heap_t *heap);
int reconnect_heap_push(braid_reconnect_heap_t *heap,
			braid_reconnect_entry_t entry);
int reconnect_heap_peek(braid_reconnect_heap_t *heap,
			braid_reconnect_entry_t *out);
int reconnect_heap_pop(braid_reconnect_heap_t *heap,
		       braid_reconnect_entry_t *out);
void reconnect_heap_clear(braid_reconnect_heap_t *heap);
uint64_t reconnect_backoff_delay(braid_pool_t *pool, uint32_t attempt);
int reconnect_advance(braid_pool_t *pool, uint64_t now_ms);

#ifdef BRAID_TEST_CLOCK
struct addrinfo;
void reconnect_test_set_socket_create_hook(int (*hook)(braid_pool_t *,
						       struct addrinfo *, int *,
						       int *));
#endif

#endif /* BRAID_RECONNECT_H */
