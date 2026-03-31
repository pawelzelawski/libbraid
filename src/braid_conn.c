/*
 * braid_conn.c — connection record lifecycle and state transitions
 *
 * conn_transition() is the single enforcement point for all state writes.
 * See ARCHITECTURE.md §4, §5.
 */

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "../include/braid.h"
#include "braid_internal.h"
#include "braid_conn.h"
#include "braid_io.h"
#include "braid_pool.h"
#include "braid_reaper.h"
#include "braid_reconnect.h"
#include "braid_table.h"

/*
 * callback_leave — common callback-exit protocol.
 * Decrements in_callback and drains deferred work on outermost callback exit.
 * See CODING_STANDARDS.md §4.1.
 */
static void
callback_leave(braid_pool_t *pool)
{
	pool->in_callback--;
	if (pool->in_callback == 0 && pool->deferred_work != 0)
		pool_drain_deferred(pool);
}

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
				callback_leave(pool);
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
			callback_leave(pool);
		}
		if (pool->in_callback == 0) {
			return conn_transition(pool, conn, BRAID_STATE_DEAD);
		}

		conn->flags |= CONN_FLAG_CLOSING_DEFERRED;
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
			callback_leave(pool);
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
			callback_leave(pool);
		}
		break;
	}
	} /* switch */

	return BRAID_OK;
}

/*
 * conn_alloc — allocate a connection record and bootstrap CONNECTING state.
 *
 * Acquires a free hash table slot via table_insert(), zeroes the record,
 * then initialises all fields required at CONNECTING entry:
 *   - fd and tag embedded in the slot (no separate allocation for tag)
 *   - created_at_ms set here because CONNECTING is the initial state,
 *     established by construction rather than by conn_transition()
 *   - heap_index set to UINT32_MAX (not in the idle reaper heap)
 *
 * Increments pool->live_count. Caller must call conn_transition(→ other)
 * or conn_transition(→ DEAD) to complete the lifecycle.
 * See ARCHITECTURE.md §8.2, §3.2.
 */
int
conn_alloc(braid_pool_t *pool, int fd, braid_conn_t **conn)
{
	braid_conn_t tmp;
	int rc;

	/*
	 * table_insert() reads (*conn)->fd to find the right probe chain,
	 * so we must initialise a local record with the fd set before calling
	 * it.  table_insert() copies the record into the table slot and sets
	 * *conn to point to that slot.
	 */
	memset(&tmp, 0, sizeof(tmp));
	tmp.fd = fd;
	*conn = &tmp;

	rc = table_insert(pool, conn);
	if (rc != BRAID_OK) {
		*conn = NULL;
		return rc;
	}

	/* *conn now points to the in-table slot.  Fix up non-zero fields. */
	(*conn)->state = BRAID_STATE_CONNECTING;
	(*conn)->heap_index = UINT32_MAX;
	(*conn)->created_at_ms = braid_now_ms();
	(*conn)->tag.magic = BRAID_FD_MAGIC;
	(*conn)->tag.fd = fd;

	pool->live_count++;

	return BRAID_OK;
}

/* Default TCP keepalive values — applied when config fields are zero. */
#define BRAID_KEEPALIVE_IDLE_DEFAULT 60 /* seconds */
#define BRAID_KEEPALIVE_INTVL_DEFAULT 10 /* seconds */
#define BRAID_KEEPALIVE_CNT_DEFAULT 3 /* probes  */

/*
 * conn_keepalive_configure — apply TCP keepalive socket options.
 *
 * Enables SO_KEEPALIVE and configures the per-socket idle, interval,
 * and probe-count values. Uses config fields when non-zero; otherwise
 * applies the documented defaults above.
 *
 * TCP_KEEPIDLE is Linux-specific; OpenBSD uses TCP_KEEPALIVE for the
 * same semantics. The #ifdef is the only platform conditional permitted
 * outside the I/O abstraction layer. See ARCHITECTURE.md §5.
 */
int
conn_keepalive_configure(int fd, const braid_config_t *config)
{
	int one = 1;
	int idle = config->keepalive_idle ? (int)config->keepalive_idle
					  : BRAID_KEEPALIVE_IDLE_DEFAULT;
	int intvl = config->keepalive_interval ? (int)config->keepalive_interval
					       : BRAID_KEEPALIVE_INTVL_DEFAULT;
	int cnt = config->keepalive_count ? (int)config->keepalive_count
					  : BRAID_KEEPALIVE_CNT_DEFAULT;

	if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one)) == -1)
		return BRAID_ERR_SYSCALL;

#ifdef __linux__
	if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle)) ==
	    -1)
		return BRAID_ERR_SYSCALL;
#else
	if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &idle, sizeof(idle)) ==
	    -1)
		return BRAID_ERR_SYSCALL;
#endif

	if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl)) ==
	    -1)
		return BRAID_ERR_SYSCALL;
	if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt)) == -1)
		return BRAID_ERR_SYSCALL;

	return BRAID_OK;
}

/*
 * conn_socket_create — create and configure a non-blocking TCP socket.
 *
 * Creates the socket, sets O_CLOEXEC and O_NONBLOCK via fcntl() (not via
 * SOCK_CLOEXEC / SOCK_NONBLOCK — those flags are not portable to OpenBSD),
 * applies TCP keepalive, and calls non-blocking connect(). EINPROGRESS is
 * the normal result and is not treated as a failure — the caller registers
 * the fd for writability and waits for connect completion via epoll/kqueue.
 *
 * Returns BRAID_OK and writes the fd to *fd_out on success (including
 * EINPROGRESS). Sets *immediate_out to 1 if connect() returned 0 (fast
 * local connect), 0 if EINPROGRESS. Returns BRAID_ERR_SYSCALL on failure.
 * See ARCHITECTURE.md §6.3, §5.
 */
int
conn_socket_create(braid_pool_t *pool, struct addrinfo *ai, int *fd_out,
		   int *immediate_out)
{
	int fd;
	int flags;
	int connect_rc;

	fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
	if (fd == -1)
		return BRAID_ERR_SYSCALL;

	/* O_CLOEXEC: close-on-exec, prevents fd leak across exec(). */
	flags = fcntl(fd, F_GETFD);
	if (flags == -1 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == -1)
		goto fail;

	/* O_NONBLOCK: required for non-blocking connect(). */
	flags = fcntl(fd, F_GETFL);
	if (flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
		goto fail;

#ifdef BRAID_DEBUG
	{
		int f = fcntl(fd, F_GETFD);
		BRAID_DEBUG_ASSERT(
		    f != -1 && (f & FD_CLOEXEC),
		    "conn_socket_create: FD_CLOEXEC not set after fcntl");
		f = fcntl(fd, F_GETFL);
		BRAID_DEBUG_ASSERT(
		    f != -1 && (f & O_NONBLOCK),
		    "conn_socket_create: O_NONBLOCK not set after fcntl");
	}
#endif

	if (conn_keepalive_configure(fd, &pool->config) != BRAID_OK)
		goto fail;

	/*
	 * Non-blocking connect(). A return value of 0 indicates immediate
	 * connection (common on loopback). EINPROGRESS is the expected outcome
	 * for remote hosts — the caller watches for writability and calls
	 * getsockopt(SO_ERROR). Any other errno is a hard failure.
	 */
	connect_rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
	if (connect_rc == -1 && errno != EINPROGRESS)
		goto fail;

	*immediate_out = (connect_rc == 0);
	*fd_out = fd;
	return BRAID_OK;

fail:
	close(fd);
	return BRAID_ERR_SYSCALL;
}
