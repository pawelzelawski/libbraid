/*
 * braid.h — libbraid public API
 *
 * Create a pool with braid_pool_create().
 * Drive the pool with braid_pool_advance() and braid_pool_notify().
 * Acquire connections with braid_pool_checkout().
 * Return connections with braid_pool_checkin().
 *
 * See ARCHITECTURE.md for the full internal design.
 */

#ifndef BRAID_H
#define BRAID_H

#include <stdint.h>

/*
 * Forward type declarations — all public types.
 * Full definitions are internal to the library.
 */

typedef struct braid_pool braid_pool_t;
typedef struct braid_config braid_config_t;
typedef uint64_t braid_token_t;
typedef struct braid_event braid_event_t;
typedef uint32_t braid_event_type_t;

/*
 * Hook function typedefs.
 * All hooks are optional — NULL is safe on every hook field.
 */

/*
 * Called after TCP connect completes; perform TLS or auth setup here.
 * conn_ctx_out is a pointer-to-pointer output parameter for connection state.
 */
typedef int (*braid_init_fn)(int fd, void **conn_ctx_out, void *hook_ctx,
			     uint64_t deadline_ms);

/* Called at checkout if idle duration exceeds idle_threshold. */
typedef int (*braid_validate_fn)(int fd, void *conn_ctx, void *hook_ctx,
				 uint64_t deadline_ms);

/*
 * Called before fd is closed; release protocol state here.
 * Must not close fd and must not call any libbraid API function.
 */
typedef void (*braid_destroy_fn)(int fd, void *conn_ctx, void *hook_ctx);

/* Called on pool lifecycle events (connection created/destroyed, etc.). */
typedef void (*braid_observe_fn)(const braid_event_t *ev, void *hook_ctx);

/* Callback for braid_pool_checkout(). fd is valid only when err == BRAID_OK. */
typedef void (*braid_checkout_cb)(int fd, void *conn_ctx, int err,
				  void *cb_ctx);

/*
 * API return codes.
 */

#define BRAID_OK 0 /* success */
#define BRAID_ERR_INVAL 1 /* invalid argument or programming error */
#define BRAID_ERR_NOMEM 2 /* allocation failure (create only) */
#define BRAID_ERR_SHUTDOWN 3 /* pool is shutting down */
#define BRAID_ERR_EXHAUSTED 4 /* max_connections reached, timeout_ms == 0 */
#define BRAID_ERR_SYSCALL 5 /* internal syscall failure */
#define BRAID_ERR_TIMEOUT 6 /* waiter deadline expired */
#define BRAID_ERR_CANCELLED 7 /* braid_pool_cancel() was called */
#define BRAID_ERR_CONNFAIL 8 /* all reconnection attempts exhausted */

/*
 * braid_pool_checkin() flags.
 */

#define BRAID_CONN_OK 0 /* return connection to IDLE state */
#define BRAID_CONN_DISCARD 1 /* discard connection, trigger destroy */

/*
 * Observability event types.
 */

#define BRAID_EV_CONN_CREATED ((braid_event_type_t)0)
#define BRAID_EV_CONN_DESTROYED ((braid_event_type_t)1)
#define BRAID_EV_RECONNECT_ATTEMPT ((braid_event_type_t)2)
#define BRAID_EV_POOL_EXHAUSTED ((braid_event_type_t)3)
#define BRAID_EV_CHECKOUT_TIMEOUT ((braid_event_type_t)4)

/*
 * Observability event struct.
 * The active union member is determined by .type.
 */

struct braid_event {
	braid_event_type_t type;
	int fd;
	union {
		struct {
			uint32_t attempt;
			int success;
		} reconnect_attempt;
	};
};

/*
 * Pool configuration struct.
 * Zero-initialise and fill only the fields you need.
 * All timeout and threshold fields default to documented values when zero.
 */

struct braid_config {
	/* Required */
	const char *host; /* target hostname or IP */
	uint16_t port; /* target port */
	int event_fd; /* caller's epoll (Linux) or kqueue fd */

	/* Pool sizing */
	uint32_t min_connections;
	uint32_t max_connections;

	/* Timeouts (milliseconds; 0 = use documented default) */
	uint32_t connect_timeout; /* default: 5000 ms */
	uint32_t init_timeout; /* default: 10000 ms */
	uint32_t validate_timeout; /* default: 2000 ms */
	uint32_t idle_threshold; /* default: 30000 ms */
	uint32_t idle_reap_timeout; /* default: 300000 ms */

	/* TCP keepalive (seconds; 0 = use documented default) */
	uint32_t keepalive_idle; /* default: 60 s */
	uint32_t keepalive_interval; /* default: 10 s */
	uint32_t keepalive_count; /* default: 3 probes */

	/* Reconnection backoff */
	uint32_t backoff_base; /* default: 100 ms */
	uint32_t backoff_cap; /* default: 30000 ms */
	uint32_t backoff_max_attempts; /* 0 = retry forever */

	/* Optional hooks — all NULL-safe */
	braid_init_fn init_fn;
	braid_validate_fn validate_fn;
	braid_destroy_fn destroy_fn;
	braid_observe_fn observe_fn;
	void *hook_context;
};

/*
 * braid_fd_tag_t — epoll/kqueue event dispatch sentinel.
 * Cast epoll_data.ptr / kevent udata to this type; check magic ==
 * BRAID_FD_MAGIC, then call braid_pool_notify() with tag->fd. See
 * ARCHITECTURE.md §8.2.
 */

#define BRAID_FD_MAGIC 0xBAADB011u

typedef struct braid_fd_tag {
	uint32_t magic; /* BRAID_FD_MAGIC; zeroed when connection is closed */
	int fd;
} braid_fd_tag_t;

/*
 * braid_pool_create — allocate and initialise a connection pool.
 *
 * config    — pool configuration; all fields are copied; config->host is
 *             deep-copied via strdup() and may be freed by the caller after
 *             this call returns.
 * err       — if non-NULL, receives BRAID_OK on success or a BRAID_ERR_*
 *             code on failure.
 *
 * Returns a pool handle on success, NULL on failure. The caller owns the
 * returned pool and must eventually pass it to braid_pool_destroy().
 *
 * See ARCHITECTURE.md §13.1.
 */
braid_pool_t *braid_pool_create(const braid_config_t *config, int *err);

/*
 * braid_pool_destroy — orderly teardown of all pool resources.
 *
 * pool              — pool to destroy. NULL is a no-op.
 * drain_timeout_ms  — milliseconds to wait for ACTIVE connections to be
 *                     checked in before force-closing them. Pass 0 to skip
 *                     the drain wait.
 *
 * Cancels all pending waiters, closes all internal fds, and frees all
 * pool memory. After this call, pool is invalid and must not be used.
 * May block for up to drain_timeout_ms if ACTIVE connections are outstanding.
 *
 * See ARCHITECTURE.md §13.2.
 */
void braid_pool_destroy(braid_pool_t *pool, uint32_t drain_timeout_ms);

/*
 * braid_pool_checkout — acquire a connection from the pool.
 *
 * pool        — pool to check out from.
 * timeout_ms  — if no IDLE connection is available, wait up to this many
 *               milliseconds. Pass 0 to return immediately without waiting.
 * cb          — callback invoked exactly once with the result. When
 *               err == BRAID_OK, fd is valid and exclusively owned by the
 *               caller until braid_pool_checkin() is called. When err != 0,
 *               fd is -1 and conn_ctx is NULL.
 * cb_ctx      — opaque pointer passed through to cb unchanged.
 * token       — if non-NULL and a waiter is enqueued (timeout_ms > 0 and no
 *               IDLE connection was immediately available), receives a token
 *               that can be passed to braid_pool_cancel() to cancel the wait.
 *
 * Returns BRAID_OK if a connection was immediately available (cb already
 * fired) or a waiter was enqueued. Returns BRAID_ERR_EXHAUSTED (without
 * invoking cb) if no connection is available and timeout_ms == 0.
 *
 * See ARCHITECTURE.md §11.
 */
int braid_pool_checkout(braid_pool_t *pool, uint32_t timeout_ms,
			braid_checkout_cb cb, void *cb_ctx,
			braid_token_t *token);

/*
 * braid_pool_checkin — return or discard an ACTIVE connection.
 *
 * pool   — owning pool.
 * fd     — file descriptor received via a checkout callback.
 * flags  — BRAID_CONN_OK to return the connection to IDLE state; or
 *           BRAID_CONN_DISCARD to close and replace the connection.
 *
 * Ownership of fd reverts to libbraid on return. After this call the caller
 * must not access fd. Returns BRAID_ERR_INVAL if fd is not a recognised
 * ACTIVE connection in pool.
 *
 * See ARCHITECTURE.md §9.
 */
int braid_pool_checkin(braid_pool_t *pool, int fd, int flags);

/*
 * braid_pool_cancel — cancel a pending checkout waiter by token.
 *
 * pool   — owning pool.
 * token  — token received via the token output of braid_pool_checkout().
 *
 * If the waiter has not yet been served or timed out, cancels it and invokes
 * its callback with BRAID_ERR_CANCELLED. If the token is stale (already
 * served, expired, or wrapped around), this is a silent no-op — it is always
 * safe to call cancel regardless of the waiter's current state.
 *
 * See ARCHITECTURE.md §9.2.
 */
int braid_pool_cancel(braid_pool_t *pool, braid_token_t token);

/*
 * braid_pool_advance — drive all timer-based pool work.
 *
 * pool     — pool to advance.
 * next_ms  — if non-NULL, receives the number of milliseconds until the
 *            next scheduled event (suitable for use as an epoll_wait /
 *            kevent timeout). UINT32_MAX means no event is pending.
 *
 * Must be called once per event-loop iteration, before epoll_wait() /
 * kevent(). Processes due reconnection attempts, enforces connect_timeout
 * on CONNECTING sockets, reaps idle connections, and expires wait queue
 * timeouts. May invoke init_fn, destroy_fn, checkout callbacks, and
 * observe_fn during this call.
 *
 * braid_pool_advance() calls clock_gettime() exactly once per call.
 * May block briefly on getaddrinfo() during reconnection attempts.
 *
 * See ARCHITECTURE.md §11.
 */
int braid_pool_advance(braid_pool_t *pool, uint32_t *next_ms);

/*
 * braid_pool_notify — dispatch an epoll/kqueue event for a libbraid fd.
 *
 * pool    — owning pool.
 * fd      — file descriptor reported by epoll_wait() / kevent().
 * events  — event flags from the epoll_event or kevent structure; the value
 *            is accepted but not used — dispatch is state-based.
 *
 * Must be called by the event loop for every event whose epoll_data.ptr
 * (or kevent udata) has BRAID_FD_MAGIC set in the braid_fd_tag_t struct.
 * Handles CONNECTING completion, INITIALIZING, and IDLE half-open detection.
 * Always returns BRAID_OK, including for unrecognised fds (timing artefact).
 *
 * See ARCHITECTURE.md §12.
 */
int braid_pool_notify(braid_pool_t *pool, int fd, uint32_t events);

#endif /* BRAID_H */
