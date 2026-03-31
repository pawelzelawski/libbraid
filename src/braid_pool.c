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
