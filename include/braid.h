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
 * Connection destroyed reason codes — carried in BRAID_EV_CONN_DESTROYED.
 */

#define BRAID_REASON_DISCARD 0
#define BRAID_REASON_HALFOPEN 1
#define BRAID_REASON_VALIDATE_FAILED 2
#define BRAID_REASON_INIT_FAILED 3
#define BRAID_REASON_CONNECT_FAILED 4
#define BRAID_REASON_IDLE_REAPED 5
#define BRAID_REASON_SHUTDOWN 6

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
			uint32_t reason; /* BRAID_REASON_* */
		} conn_destroyed;
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
 * Public API function declarations.
 * (Implementations land in Phase 6 — braid_pool.c.)
 */

braid_pool_t *braid_pool_create(const braid_config_t *config, int *err);
void braid_pool_destroy(braid_pool_t *pool, uint32_t drain_timeout_ms);

int braid_pool_checkout(braid_pool_t *pool, uint32_t timeout_ms,
			braid_checkout_cb cb, void *cb_ctx,
			braid_token_t *token);
int braid_pool_checkin(braid_pool_t *pool, int fd, int flags);
int braid_pool_cancel(braid_pool_t *pool, braid_token_t token);

int braid_pool_advance(braid_pool_t *pool, uint32_t *next_ms);
int braid_pool_notify(braid_pool_t *pool, int fd, uint32_t events);

#endif /* BRAID_H */
