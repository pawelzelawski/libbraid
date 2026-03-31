/*
 * braid_conn.c — connection record lifecycle and state transitions
 *
 * conn_transition() is the single enforcement point for all state writes.
 * See ARCHITECTURE.md §4, §5.
 */

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "../include/braid.h"
#include "braid_internal.h"
#include "braid_conn.h"
#include "braid_io.h"
#include "braid_reaper.h"
#include "braid_reconnect.h"
#include "braid_table.h"

/*
 * Legal state transition table.
 * Each entry is one valid (from, to) pair per ARCHITECTURE.md §4.2.
 * conn_transition() iterates this table to assert legality on every call.
 */
struct transition_rule {
	braid_state_t from;
	braid_state_t to;
};

static const struct transition_rule legal_transitions[] = {
    {BRAID_STATE_CONNECTING, BRAID_STATE_INITIALIZING},
    {BRAID_STATE_CONNECTING, BRAID_STATE_DEAD},
    {BRAID_STATE_INITIALIZING, BRAID_STATE_IDLE},
    {BRAID_STATE_INITIALIZING, BRAID_STATE_DEAD},
    {BRAID_STATE_IDLE, BRAID_STATE_ACTIVE},
    {BRAID_STATE_IDLE, BRAID_STATE_CLOSING},
    {BRAID_STATE_ACTIVE, BRAID_STATE_IDLE},
    {BRAID_STATE_ACTIVE, BRAID_STATE_CLOSING},
    {BRAID_STATE_ACTIVE, BRAID_STATE_DEAD},
    {BRAID_STATE_CLOSING, BRAID_STATE_DEAD},
};

#define LEGAL_TRANSITIONS_COUNT                                                \
	(sizeof(legal_transitions) / sizeof(legal_transitions[0]))

/*
 * conn_transition — single enforcement point for all connection state writes.
 *
 * Asserts that the (old_state → new_state) pair appears in the legal
 * transition table. In debug builds, an illegal transition prints a
 * diagnostic identifying the fd and both states, then aborts. In release
 * builds, the function returns BRAID_ERR_INVAL and leaves state unchanged.
 *
 * State-entry and state-exit invariants are enforced here and nowhere else.
 * No other function may write conn->state directly.
 * See ARCHITECTURE.md §4.3, CODING_STANDARDS.md §3.1.
 */
int
conn_transition(braid_pool_t *pool, braid_conn_t *conn, braid_state_t new_state)
{
	braid_state_t old_state = conn->state;
	size_t i;
	int legal = 0;

	for (i = 0; i < LEGAL_TRANSITIONS_COUNT; i++) {
		if (legal_transitions[i].from == old_state &&
		    legal_transitions[i].to == new_state) {
			legal = 1;
			break;
		}
	}

	if (!legal) {
#ifdef BRAID_DEBUG
		fprintf(
		    stderr,
		    "conn_transition: illegal transition %d -> %d on fd %d\n",
		    (int)old_state, (int)new_state, conn->fd);
		abort();
#endif
		return BRAID_ERR_INVAL;
	}

	/*
	 * IDLE exit: remove from the reaper heap before writing new state so
	 * the heap sees a consistent IDLE record.
	 */
	if (old_state == BRAID_STATE_IDLE)
		reaper_heap_remove(&pool->idle, conn);

	/* Single state write — the only site in the codebase. */
	conn->state = new_state;

	switch (new_state) {
	case BRAID_STATE_CONNECTING:
		conn->created_at_ms = braid_now_ms();
		break;

	case BRAID_STATE_INITIALIZING:
		/* No entry invariants beyond the state write. */
		break;

	case BRAID_STATE_IDLE:
		conn->last_active_ms = braid_now_ms();
		reaper_heap_insert(&pool->idle, conn);
		break;

	case BRAID_STATE_ACTIVE:
		/*
		 * Fire BRAID_EV_CONN_CREATED on first ACTIVE entry only.
		 * Recycled connections (ACTIVE -> IDLE -> ACTIVE) do not
		 * re-fire. See ARCHITECTURE.md §4.3.
		 */
		if (!(conn->flags & CONN_FLAG_EVER_ACTIVE)) {
			conn->flags |= CONN_FLAG_EVER_ACTIVE;
			if (pool->config.observe_fn != NULL) {
				braid_event_t ev;
				memset(&ev, 0, sizeof(ev));
				ev.type = BRAID_EV_CONN_CREATED;
				ev.fd = conn->fd;
				pool->in_callback++;
				pool->config.observe_fn(
				    &ev, pool->config.hook_context);
				pool->in_callback--;
			}
		}
		break;

	case BRAID_STATE_CLOSING:
		/*
		 * Call destroy_fn with the in_callback protocol so that any
		 * re-entrant checkin sees in_callback > 0 and defers.
		 * After destroy_fn returns, immediately recurse to DEAD
		 * unless we are already inside a callback (deferred close).
		 * See ARCHITECTURE.md §4.3, CODING_STANDARDS.md §4.1.
		 */
		if (pool->config.destroy_fn != NULL) {
			pool->in_callback++;
			pool->config.destroy_fn(conn->fd, conn->conn_ctx,
						pool->config.hook_context);
			pool->in_callback--;
		}
		if (pool->in_callback == 0) {
			return conn_transition(pool, conn, BRAID_STATE_DEAD);
		} else {
			conn->flags |= CONN_FLAG_CLOSING_DEFERRED;
		}
		break;

	case BRAID_STATE_DEAD: {
		int fd = conn->fd;

		/*
		 * Call destroy_fn if CLOSING did not already call it.
		 * SAFETY: old_state == CLOSING means destroy_fn was invoked
		 * from the CLOSING entry handler above; all other paths to
		 * DEAD (ACTIVE, CONNECTING, INITIALIZING) have not yet called
		 * it. ARCHITECTURE.md §4.3.
		 */
		if (old_state != BRAID_STATE_CLOSING &&
		    pool->config.destroy_fn != NULL) {
			pool->in_callback++;
			pool->config.destroy_fn(fd, conn->conn_ctx,
						pool->config.hook_context);
			pool->in_callback--;
		}

		/*
		 * Unregister from epoll/kqueue before closing the fd.
		 * Closing without unwatching leaves a stale registration
		 * that may match a newly allocated fd with the same number.
		 * See ARCHITECTURE.md §4.3, CODING_STANDARDS.md §5.
		 */
		io_unwatch(pool, fd);
		close(fd);

		/*
		 * Invalidate the inline tag so that any in-flight epoll event
		 * referencing this slot fails the BRAID_FD_MAGIC check.
		 * See ARCHITECTURE.md §8.2.
		 */
		conn->tag.fd = -1;
		conn->tag.magic = 0;

		table_delete(pool, fd);

		BRAID_DEBUG_ASSERT(
		    pool->live_count > 0,
		    "conn_transition: live_count underflow on DEAD entry");
		pool->live_count--;

		/*
		 * If the pool has fallen below min_connections and is not
		 * shutting down, schedule a reconnection attempt.
		 * The shutdown flag suppresses new connections after destroy.
		 * See ARCHITECTURE.md §4.3, §6.
		 */
		if (pool->live_count < pool->config.min_connections &&
		    !pool->shutting_down) {
			braid_reconnect_entry_t entry;
			memset(&entry, 0, sizeof(entry));
			reconnect_heap_push(&pool->reconnect, entry);
		}

		if (pool->config.observe_fn != NULL) {
			braid_event_t ev;
			memset(&ev, 0, sizeof(ev));
			ev.type = BRAID_EV_CONN_DESTROYED;
			ev.fd = fd;
			pool->in_callback++;
			pool->config.observe_fn(&ev, pool->config.hook_context);
			pool->in_callback--;
		}
		break;
	}
	} /* switch */

	return BRAID_OK;
}
