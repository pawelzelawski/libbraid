/*
 * braid_reaper.h — idle reaper internal interface (internal)
 *
 * Min-heap of IDLE connections keyed on last_active_ms.
 * See ARCHITECTURE.md §7.
 */

#ifndef BRAID_REAPER_H
#define BRAID_REAPER_H

#include <stdint.h>

#include "../include/braid.h"
#include "braid_internal.h"

int reaper_heap_init(braid_idle_heap_t *heap, uint32_t cap);
void reaper_heap_destroy(braid_idle_heap_t *heap);
int reaper_heap_insert(braid_idle_heap_t *heap, braid_conn_t *conn);
int reaper_heap_remove(braid_idle_heap_t *heap, braid_conn_t *conn);
int reaper_heap_peek(braid_idle_heap_t *heap, braid_idle_entry_t *out);
int reaper_advance(braid_pool_t *pool, uint64_t now_ms);

/*
 * Test-visible call counters — only present in test builds.
 * Reset these before each test that verifies reaper heap interaction.
 */
#ifdef BRAID_TEST_CLOCK
extern int braid_test_reaper_insert_count;
extern int braid_test_reaper_remove_count;
#endif

#endif /* BRAID_REAPER_H */
