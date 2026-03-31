# Coding Standards

## 1. Code Style

### 1.1 OpenBSD KNF (Kernel Normal Form)

libbraid follows OpenBSD's Kernel Normal Form style. This is the style used
throughout the OpenBSD base system and is required for all source files in
`src/` and `include/`.

**Indentation**:
- Tabs for indentation, 8-character display width
- Spaces only for alignment within a line, never for indentation
- Never mix tabs and spaces for indentation

```c
/* Correct */
static int
conn_transition(braid_pool_t *pool, braid_conn_t *conn,
    braid_state_t new_state)
{
	if (conn->state == new_state)
		return BRAID_ERR_INVAL;
	conn->state = new_state;
	return BRAID_OK;
}

/* Wrong — spaces used for indentation */
static int
conn_transition(braid_pool_t *pool, braid_conn_t *conn,
    braid_state_t new_state)
{
    if (conn->state == new_state)   /* spaces, not tabs */
        return BRAID_ERR_INVAL;
}
```

**Braces**:
```c
/* Functions: opening brace on its own line */
static void
reconnect_heap_push(braid_reconnect_heap_t *heap,
    braid_reconnect_entry_t entry)
{
	/* body */
}

/* Control structures: opening brace on same line */
if (pool->in_callback > 0) {
	pool->deferred_work |= BRAID_DEFERRED_SERVE_WAITQUEUE;
	return;
}

/* Single-statement bodies: no braces, indented on next line */
if (conn == NULL)
	return BRAID_ERR_INVAL;

/* Loop with single statement */
for (i = 0; i < heap->count; i++)
	heap->entries[i] = heap->entries[i + 1];
```

**Line length**: Maximum 80 characters. Break long lines at logical points,
aligning continuation with the opening parenthesis or using one extra tab:

```c
/* Break at logical point, align with opening paren */
rc = braid_pool_checkout(pool, timeout_ms, on_connection,
    cb_ctx, &token);

/* Extra tab indent for continuation */
return conn_transition(pool, conn,
	BRAID_STATE_CLOSING);
```

**Naming conventions**:
```c
/* Variables and function parameters: lowercase with underscores */
uint32_t   slot;
uint64_t   now_ms;
int        fd;
braid_conn_t *conn;

/* Public API functions: braid_ prefix, lowercase with underscores */
braid_pool_t *braid_pool_create(const braid_config_t *config, int *err);
int           braid_pool_checkout(braid_pool_t *pool, uint32_t timeout_ms,
                  braid_checkout_cb cb, void *cb_ctx, braid_token_t *token);
int           braid_pool_checkin(braid_pool_t *pool, int fd, int flags);

/* Internal functions: module prefix, no braid_ prefix */
static int   conn_transition(braid_pool_t *, braid_conn_t *, braid_state_t);
static void  reconnect_heap_push(braid_reconnect_heap_t *,
                 braid_reconnect_entry_t);
static int   table_lookup(braid_pool_t *, int fd, braid_conn_t **);

/* Constants and macros: uppercase with underscores, BRAID_ prefix */
#define BRAID_CONN_OK              0
#define BRAID_CONN_DISCARD         1
#define BRAID_FD_MAGIC             0xBAADB011u
#define BRAID_DEFERRED_SERVE_WAITQUEUE  0x01
#define BRAID_DEFERRED_PROCESS_DEAD     0x02

/* Structs and typedefs: lowercase, _t suffix */
typedef struct braid_pool           braid_pool_t;
typedef struct braid_conn           braid_conn_t;
typedef struct braid_fd_tag         braid_fd_tag_t;
typedef struct braid_reconnect_entry braid_reconnect_entry_t;

/* Enums: uppercase values with BRAID_ prefix */
typedef enum {
	BRAID_STATE_CONNECTING   = 0,
	BRAID_STATE_INITIALIZING = 1,
	BRAID_STATE_IDLE         = 2,
	BRAID_STATE_ACTIVE       = 3,
	BRAID_STATE_CLOSING      = 4,
	BRAID_STATE_DEAD         = 5,
} braid_state_t;
```

**Spacing**:
```c
/* Space after keywords, not after function names */
if (condition)                    /* correct */
if(condition)                     /* wrong */
conn_transition(pool, conn, s)    /* correct */
conn_transition (pool, conn, s)   /* wrong */

/* No space inside parentheses */
if (n > 0)                        /* correct */
if ( n > 0 )                      /* wrong */

/* Space around binary operators */
next_ms = last_active_ms + idle_reap_timeout;
slot    = (uint32_t)fd % pool->table_size;

/* No space for unary operators */
tag  = &conn->tag;
val  = *ptr;
mask = ~CONN_FLAG_TOMBSTONE;
```

**Return type on its own line**:
```c
/* Correct */
static int
table_insert(braid_pool_t *pool, braid_conn_t *conn)
{
	/* ... */
}

/* Wrong */
static int table_insert(braid_pool_t *pool, braid_conn_t *conn)
{
	/* ... */
}
```

### 1.2 File Organisation

**Public header** (`include/braid.h`):

The public header exposes only what embedders need. Internal types, internal
function declarations, and implementation details are never declared here.
The header is self-contained — an embedder includes only `braid.h` and
nothing else.

```c
#ifndef BRAID_H
#define BRAID_H

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

#include <stdint.h>

/* ... public types, constants, and function declarations ... */

#endif /* BRAID_H */
```

**Internal headers** (e.g. `src/braid_table.h`):

```c
#ifndef BRAID_TABLE_H
#define BRAID_TABLE_H

/*
 * braid_table.h — connection hash table (internal)
 * See ARCHITECTURE.md §3 for hash table design and slot states.
 */

#include "../include/braid.h"
#include "braid_internal.h"

/* ... internal types and function declarations ... */

#endif /* BRAID_TABLE_H */
```

**Source files** (`.c`):
```c
/*
 * braid_table.c — connection hash table: insert, lookup, delete
 *
 * Open-addressed hash table keyed on fd, linear probing.
 * See ARCHITECTURE.md §3.
 */

#include <string.h>
#include <stdint.h>

#include "../include/braid.h"
#include "braid_internal.h"
#include "braid_table.h"
```

**Include order** (within each group, alphabetical):
```c
/* 1. System headers */
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

/* 2. OS-specific headers */
#ifdef BRAID_LINUX
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#endif
#ifdef BRAID_OPENBSD
#include <sys/event.h>
#include <sys/socket.h>
#endif

/* 3. Library headers — public first, then internal */
#include "../include/braid.h"
#include "braid_internal.h"
#include "braid_table.h"
```

---

## 2. Error Handling

### 2.1 Return Codes

All public functions return `int`. Zero (`BRAID_OK`) is success. Non-zero
is an error code from the `BRAID_ERR_*` set defined in `include/braid.h`.

```c
/* Correct — check every return value */
rc = conn_transition(pool, conn, BRAID_STATE_IDLE);
if (rc != BRAID_OK)
	goto cleanup;

/* Wrong — ignoring a return value */
conn_transition(pool, conn, BRAID_STATE_IDLE);
```

Internal static functions also return `int` with the same convention unless
they cannot fail and returning `void` is more honest. Never use sentinel
return values (−1, NULL) without a documented contract — use the `BRAID_ERR_*`
codes.

### 2.2 Allocation Failure

`braid_pool_create()` is the only function that allocates memory. If any
allocation fails, all previously allocated memory is freed and NULL is
returned. The error code is written to `*err`.

After successful creation, no public API function allocates memory. This
invariant must be preserved. If a code path requires dynamic allocation
post-creation, the design is wrong — fix the design rather than the invariant.

```c
/* Correct — check malloc immediately, free and return on failure */
pool->table = malloc(table_size * sizeof(braid_conn_t));
if (pool->table == NULL) {
	rc = BRAID_ERR_NOMEM;
	goto cleanup;
}
```

`goto cleanup` is the standard pattern for multi-resource acquisition. The
cleanup label releases all resources allocated before the failure in reverse
order.

### 2.3 Programming Errors

Conditions that represent caller misuse — passing an unrecognised fd to
`braid_pool_checkin()`, calling public functions with a NULL pool pointer,
violating documented preconditions — are handled as follows:

- In debug builds (`BRAID_DEBUG`): assert with a diagnostic message
  identifying the function, the violated invariant, and the offending value.
- In release builds: return `BRAID_ERR_INVAL` immediately. Do not attempt
  recovery or silent correction.

```c
/* Debug build: assert with diagnostic */
BRAID_DEBUG_ASSERT(pool != NULL,
    "braid_pool_checkin: pool is NULL");

/* Release build equivalent */
if (pool == NULL)
	return BRAID_ERR_INVAL;
```

---

## 3. State Machine Discipline

### 3.1 Single Transition Point

`conn_transition()` is the only function in the codebase that writes
`conn->state`. Direct writes to `conn->state` outside of `conn_transition()`
are prohibited. This rule must be enforced in code review.

```c
/* Correct */
rc = conn_transition(pool, conn, BRAID_STATE_IDLE);

/* Wrong — direct write, bypasses invariants and observability */
conn->state = BRAID_STATE_IDLE;
```

### 3.2 Transition Legality

Every call to `conn_transition()` is a statement that the transition is
legal. In debug builds, `conn_transition()` asserts the transition against
the legal transition table and aborts with a diagnostic on violation. Do not
call `conn_transition()` for speculative transitions — verify preconditions
before calling.

```c
/*
 * SAFETY: transition is legal only if connection is in IDLE state.
 * Verified by caller before this call. conn_transition() will assert in
 * debug builds if this precondition is violated.
 * See ARCHITECTURE.md §4.2.
 */
BRAID_DEBUG_ASSERT(conn->state == BRAID_STATE_IDLE,
    "waitq_serve_head: connection not in IDLE state");
rc = conn_transition(pool, conn, BRAID_STATE_ACTIVE);
```

---

## 4. Re-entrancy and Deferred Work

### 4.1 in_callback Protocol

The `pool->in_callback` counter must be incremented before every caller
callback invocation and decremented after. This rule applies to all callback
types: `braid_checkout_cb`, `init_fn`, `validate_fn`, `destroy_fn`, and
`observe_fn`.

```c
pool->in_callback++;
pool->config.init_fn(conn->fd, &conn->conn_ctx,
    deadline_ms, pool->config.hook_context);
pool->in_callback--;
if (pool->in_callback == 0 && pool->deferred_work != 0)
	pool_drain_deferred(pool);
```

Never invoke a callback without the counter protocol. A callback that calls
back into `braid_pool_checkin()` must find `in_callback > 0` and defer
correctly. Missing the increment breaks this guarantee.

**`destroy_fn` re-entrancy restriction:** `destroy_fn` is invoked with
`pool->in_callback > 0`. Unlike other hooks, `destroy_fn` must not call
any libbraid API function — including `braid_pool_checkin()` on a different
connection. Calling `braid_pool_checkin()` from within `destroy_fn` would
trigger deferred work processing while `in_callback > 0` via a different
code path than the normal checkout callback, potentially corrupting the
deferred work state. The `destroy_fn` restriction is absolute: no libbraid
calls from within `destroy_fn`. This must be stated clearly in the
`destroy_fn` docblock in `include/braid.h`.

### 4.2 Deferred Work Flags

Set deferred work flags rather than performing work directly whenever
`pool->in_callback > 0`. Do not perform DEAD processing or wait queue
serving inline during a callback invocation.

```c
/* Correct — defer when inside a callback */
if (pool->in_callback > 0) {
	pool->deferred_work |= BRAID_DEFERRED_PROCESS_DEAD;
	return;
}
/* ... perform dead processing directly ... */

/* Wrong — processing dead connections while inside a callback */
process_dead_connections(pool);  /* may re-enter checkout path */
```

---

## 5. fd Lifecycle Rules

### 5.1 Ownership Contract

An fd is exclusively owned by the caller from the moment the checkout
callback fires until `braid_pool_checkin()` is called. libbraid must never
access, close, or register an fd that is currently checked out. This
invariant must be respected at all new callsites.

```c
/*
 * SAFETY: do not access conn->fd while conn->state == BRAID_STATE_ACTIVE.
 * The fd is exclusively owned by the caller until checkin.
 * See ARCHITECTURE.md §1.4.
 */
BRAID_DEBUG_ASSERT(conn->state != BRAID_STATE_ACTIVE,
    "io_unwatch: attempted to modify epoll registration for active fd");
```

### 5.2 fd Closing

libbraid always closes the fd at the DEAD transition, after `destroy_fn`
returns. No other code path may close a libbraid-managed fd.

**`destroy_fn` must not close the fd.** This is a hard rule, not a style
preference. If `destroy_fn` closes the fd, the OS may recycle that fd number
before libbraid's subsequent `close()` executes. libbraid will then close
an unrelated fd that happens to have been allocated the same number — a
silent, hard-to-reproduce production bug. `destroy_fn` must release protocol
state (free `conn_ctx`) and perform graceful protocol teardown writes only.
It must leave the fd open for libbraid to close. This rule must be stated in
the `destroy_fn` typedef documentation in `include/braid.h`.

### 5.3 O_NONBLOCK

Every socket libbraid creates is set non-blocking with `O_NONBLOCK` before
`connect()` is called. This is verified in debug builds at socket creation.
libbraid never modifies fd flags on fds it does not own.

### 5.4 O_CLOEXEC

Every socket libbraid creates is created with `O_CLOEXEC`. Sockets do not
leak into child processes after `exec()`.

---

## 6. Integer Safety

Use fixed-width types for all size calculations, timer arithmetic, and
protocol values. Check for overflow before arithmetic on values that could
be large.

```c
/* Use sized types */
uint64_t  now_ms;
uint64_t  next_retry_ms;
uint32_t  slot;
uint32_t  attempt;

/* Overflow guard on backoff computation — see ARCHITECTURE.md §6.2 */
uint32_t  exp = (attempt < 31) ? attempt : 31;
uint64_t  window = (uint64_t)config->backoff_base << exp;
if (window > config->backoff_cap)
	window = config->backoff_cap;

/* Overflow guard on timeout deadline computation */
if (timeout_ms > UINT64_MAX - now_ms)
	deadline_ms = UINT64_MAX;   /* saturate */
else
	deadline_ms = now_ms + timeout_ms;
```

---

## 7. Documentation

### 7.1 Function Comments

Every non-trivial public function requires a block comment describing what
it does, its parameters, return values, and any ownership or lifetime
constraints. Internal static helpers require a brief comment unless the
function name is entirely self-explanatory.

```c
/*
 * braid_pool_checkout — request a connection from the pool.
 *
 * Non-blocking. Returns immediately in all cases.
 *
 * If a connection is immediately available, cb is invoked before this
 * function returns. Otherwise the request is enqueued with the given
 * timeout. When a connection becomes available or the timeout expires,
 * cb is invoked from braid_pool_advance(). One cb invocation is
 * guaranteed for every checkout call, regardless of outcome.
 *
 * pool:       pool to check out from
 * timeout_ms: maximum wait time in milliseconds; 0 = fail immediately
 * cb:         callback invoked with the result (fd, conn_ctx, err, cb_ctx)
 * cb_ctx:     opaque context passed through to cb unchanged
 * token:      receives an opaque cancellation token if the request is
 *             queued; undefined if connection was immediately available
 *
 * Returns BRAID_OK if the request was enqueued or immediately served.
 * Returns BRAID_ERR_EXHAUSTED if timeout_ms == 0 and no connection is
 *         available (cb is not invoked in this case).
 * Returns BRAID_ERR_SHUTDOWN if the pool is being destroyed.
 * Returns BRAID_ERR_INVAL if pool or cb is NULL.
 */
int
braid_pool_checkout(braid_pool_t *pool, uint32_t timeout_ms,
    braid_checkout_cb cb, void *cb_ctx, braid_token_t *token);
```

### 7.2 Architecture Cross-Reference Comments

When implementing pool, table, reconnection, reaper, or wait queue logic,
reference the relevant section of ARCHITECTURE.md.

```c
/*
 * Open-addressed hash table lookup by fd, linear probing.
 * Tombstones do not terminate search — walk continues to empty slot.
 * See ARCHITECTURE.md §3.3.
 */

/*
 * Full jitter backoff: sleep = random(0, min(cap, base * 2^attempt)).
 * Exponent capped at 31 to prevent uint64_t overflow.
 * See ARCHITECTURE.md §6.2.
 */

/*
 * Idle reaper: peek heap minimum, check against idle_reap_timeout.
 * Respects min_connections floor — never reaps below it.
 * See ARCHITECTURE.md §7.2.
 */

/*
 * braid_fd_tag_t magic check — distinguishes libbraid fds from caller fds
 * in the shared epoll instance. See ARCHITECTURE.md §8.2.
 */
```

### 7.3 SAFETY Comments

Mark safety-critical invariants explicitly so they are never accidentally
removed during refactoring. The comment format is `/* SAFETY: ... */` on
its own line before the guarded code.

```c
/*
 * SAFETY: conn->state must not be written directly. All transitions go
 * through conn_transition(). Direct writes bypass legality assertions and
 * observability hooks. See ARCHITECTURE.md §4.3.
 */

/*
 * SAFETY: do not serve the wait queue while in_callback > 0. Serving
 * the queue invokes a checkout callback which may call braid_pool_checkin()
 * re-entrantly. Defer via BRAID_DEFERRED_SERVE_WAITQUEUE.
 * See ARCHITECTURE.md §9.2.
 */

/*
 * SAFETY: io_unwatch() must be called before close(). Closing an fd
 * registered in epoll without removing it first leaves a stale registration
 * that may match a newly allocated fd with the same number.
 * See ARCHITECTURE.md §8.1.
 */

/*
 * SAFETY: destroy_fn must be called before close(fd). The caller may hold
 * references to conn_ctx that are invalidated by protocol teardown. libbraid
 * always calls destroy_fn first, then closes fd, never the other way around.
 * See ARCHITECTURE.md §4.3.
 */
```

### 7.4 TODO and FIXME

```c
/* TODO: batch kevent() calls in braid_io_kqueue.c when v2 OpenBSD port lands */
/* NOTE: backoff_max_attempts == 0 means retry forever — do not treat as
 *       "no limit" in the sense of zero attempts. See ARCHITECTURE.md §6.2 */
/* NOTE: BRAID_OK == 0; all error codes are positive.
 *       Do not compare rc < 0 to mean error — use rc != BRAID_OK */
/* NOTE: heap removal requires sift-up AND sift-down after swap-with-last.
 *       Sift-down alone is insufficient — moved element may need sift-up.
 *       See ARCHITECTURE.md §7.1 */
```

---

## 8. Pre-Commit Checklist

Before every commit:

- [ ] Compiles without warnings on Linux (`-Wall -Wextra -Wpedantic`)
- [ ] Compiles without warnings on OpenBSD (`-Wall -Wextra -Wpedantic`)
- [ ] All tests pass (`make test`)
- [ ] Valgrind clean on Linux (`make valgrind`)
- [ ] ASan/UBSan clean on both platforms (`make dev && make test`)
- [ ] clang-format clean (`make format`)
- [ ] No `malloc`/`free` outside `braid_pool_create()` / `braid_pool_destroy()`
      — verify with: `grep -n "malloc\|free" src/*.c`
- [ ] Every `malloc` call checks for NULL return and uses `goto cleanup`
- [ ] No direct write to `conn->state` outside `conn_transition()` — verify
      with: `grep -n "->state\s*=" src/*.c`
- [ ] `pool->in_callback` incremented before and decremented after every
      callback invocation
- [ ] `pool_drain_deferred()` called after every in_callback decrement that
      reaches zero with `deferred_work != 0`
- [ ] `destroy_fn` docblock states it must not call any libbraid API function
      and must not close the fd
- [ ] `io_unwatch()` called before `close()` on every fd that was registered
- [ ] `destroy_fn` called before `close(fd)` on every discarded connection
- [ ] `O_NONBLOCK` and `O_CLOEXEC` set via `fcntl()` on every socket created
      by libbraid — never via `SOCK_NONBLOCK`/`SOCK_CLOEXEC` in socket() call
- [ ] `SAFETY:` comment present on every new safety-critical invariant
- [ ] Overflow guard present on all backoff exponent and deadline arithmetic
- [ ] `BRAID_DEBUG_ASSERT` present for all programming-error preconditions
- [ ] `goto cleanup` pattern used for all multi-resource acquisition in init
      paths
- [ ] `waitq_cancel()` verifies `slot.token == token` before tombstoning
- [ ] Heap removal performs sift-up AND sift-down after swap-with-last
- [ ] Shutdown flag checked before inserting reconnect entries in DEAD path
- [ ] Reconnect entries inserted only on failure, not pre-inserted at attempt
      start (see ARCHITECTURE.md §6.3)
- [ ] New public functions have complete doc comment blocks
- [ ] New pool/table/reconnect/reaper logic has ARCHITECTURE.md
      cross-reference comment
- [ ] No `FIXME` added without a comment explaining what is deferred and why

---

**Document Version**: 1.0
**Last Updated**: 2026-03-30
**See Also**: PROJECT.md, ARCHITECTURE.md, TECH_STACK.md, DEVELOPMENT.md
