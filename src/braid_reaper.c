/*
 * braid_reaper.c — idle reaper heap and reap logic
 *
 * Min-heap keyed on last_active_ms. Reaps connections exceeding
 * idle_reap_timeout, subject to the min_connections floor.
 * See ARCHITECTURE.md §7.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../include/braid.h"
#include "braid_internal.h"
#include "braid_reaper.h"

/*
 * Stubs — full implementation in Phase 5.
 * Signatures take braid_conn_t * so Phase 5 can maintain conn->heap_index.
 */

int
reaper_heap_insert(braid_idle_heap_t *heap, braid_conn_t *conn)
{
	(void)heap;
	(void)conn;
	return BRAID_OK;
}

int
reaper_heap_remove(braid_idle_heap_t *heap, braid_conn_t *conn)
{
	(void)heap;
	(void)conn;
	return BRAID_OK;
}
