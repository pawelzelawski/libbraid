/*
 * braid_internal.h — libbraid internal shared types and declarations
 *
 * This header is included by all internal translation units.
 * It is NOT part of the public API and is never installed.
 *
 * Contains: complete internal struct definitions, internal constants,
 * BRAID_DEBUG_ASSERT macro, mock clock infrastructure, and
 * _Static_assert placeholders.
 *
 * See ARCHITECTURE.md for full internal design.
 * See CODING_STANDARDS.md §3 for state machine rules.
 * See REPOSITORY_STRUCTURE.md §3 for field-level documentation.
 */

#ifndef BRAID_INTERNAL_H
#define BRAID_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "../include/braid.h"

/*
 * BRAID_DEBUG_ASSERT — assertion with diagnostic message.
 * In debug builds (BRAID_DEBUG defined): prints message and aborts.
 * In release builds: evaluates condition but takes no action on failure.
 */
#ifdef BRAID_DEBUG
#include <stdio.h>
#include <stdlib.h>
#define BRAID_DEBUG_ASSERT(cond, msg)                                          \
	do {                                                                   \
		if (!(cond)) {                                                 \
			fprintf(stderr,                                        \
				"BRAID ASSERT FAILED: %s\n"                    \
				"  condition: %s\n"                            \
				"  file: %s line: %d\n",                       \
				(msg), #cond, __FILE__, __LINE__);             \
			abort();                                               \
		}                                                              \
	} while (0)
#else
#define BRAID_DEBUG_ASSERT(cond, msg) ((void)(cond))
#endif /* BRAID_DEBUG */

/*
 * Mock clock infrastructure.
 * In test builds (BRAID_TEST_CLOCK defined), braid_now_ms() reads the
 * braid_test_clock_ms global instead of calling clock_gettime().
 * Tests advance time by writing to braid_test_clock_ms directly.
 * No test may use sleep() or usleep() for timing.
 */
#ifdef BRAID_TEST_CLOCK
extern uint64_t braid_test_clock_ms;
static inline uint64_t
braid_now_ms(void)
{
	return braid_test_clock_ms;
}
#else
static inline uint64_t
braid_now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}
#endif /* BRAID_TEST_CLOCK */

/*
 * Connection lifecycle states.
 * Written only by conn_transition() — never set directly.
 * See ARCHITECTURE.md §4.
 */
typedef enum {
	BRAID_STATE_CONNECTING = 0,
	BRAID_STATE_INITIALIZING = 1,
	BRAID_STATE_IDLE = 2,
	BRAID_STATE_ACTIVE = 3,
	BRAID_STATE_CLOSING = 4,
	BRAID_STATE_DEAD = 5,
} braid_state_t;

/*
 * Internal constants.
 */

/* Magic value for braid_fd_tag_t — used to authenticate epoll events. */
#define BRAID_FD_MAGIC 0xBAADB011u

/* Connection record flag bits. */
#define CONN_FLAG_TOMBSTONE 0x01u /* slot vacated, probe chain intact */
#define CONN_FLAG_CLOSING_DEFERRED                                             \
	0x02u /* CLOSING→DEAD deferred from callback */
#define CONN_FLAG_EVER_ACTIVE 0x04u /* connection was ACTIVE at least once */

/* Wait queue entry flag bits. */
#define WAITER_FLAG_TOMBSTONE 0x01u /* entry cancelled or served */

/* I/O event flags — platform-independent. */
#define BRAID_IO_READ 0x01u
#define BRAID_IO_WRITE 0x02u

/* Deferred work flags — set when work is deferred due to in_callback > 0. */
#define BRAID_DEFERRED_SERVE_WAITQUEUE 0x01u
#define BRAID_DEFERRED_PROCESS_DEAD 0x02u

/*
 * braid_fd_tag_t — epoll/kqueue sentinel struct.
 * Embedded inline in braid_conn_t; never separately allocated.
 * epoll_data.ptr (or kevent udata) points to &conn->tag.
 * The magic value authenticates the pointer as libbraid-owned.
 * See ARCHITECTURE.md §8.2.
 */
typedef struct braid_fd_tag {
	uint32_t magic; /* BRAID_FD_MAGIC; zeroed on DEAD to invalidate */
	int fd;
} braid_fd_tag_t;

/*
 * braid_conn_t — connection record.
 * One record per connection slot in the hash table.
 * fd == -1 with CONN_FLAG_TOMBSTONE indicates a tombstone slot.
 * fd == -1 without tombstone flag indicates an empty slot.
 * See ARCHITECTURE.md §3.2.
 */
typedef struct braid_conn {
	int fd; /* file descriptor; -1 = empty slot  */
	braid_state_t state; /* lifecycle state; conn_transition() only */
	void *conn_ctx; /* caller protocol state, opaque */
	uint64_t created_at_ms; /* monotonic ms, set at CONNECTING entry */
	uint64_t last_active_ms; /* monotonic ms, updated at IDLE entry */
	uint32_t flags; /* CONN_FLAG_* bitfield */
	uint32_t heap_index; /* idle reaper heap position; UINT32_MAX out */
	braid_fd_tag_t tag; /* epoll sentinel — inline, no alloc needed */
} braid_conn_t;

/*
 * braid_reconnect_entry_t — one pending reconnection attempt.
 * See ARCHITECTURE.md §6.1.
 */
typedef struct braid_reconnect_entry {
	uint64_t next_retry_ms; /* absolute monotonic ms for next attempt */
	uint32_t attempt; /* zero-indexed attempt counter */
} braid_reconnect_entry_t;

/*
 * braid_reconnect_heap_t — min-heap of pending reconnections.
 * Keyed on next_retry_ms.
 * See ARCHITECTURE.md §6.1.
 */
typedef struct braid_reconnect_heap {
	braid_reconnect_entry_t *entries;
	uint32_t count;
	uint32_t cap;
} braid_reconnect_heap_t;

/*
 * braid_idle_entry_t — one entry in the idle reaper heap.
 * Stores a direct pointer to the connection record rather than an fd;
 * pool->table is never reallocated so the pointer is stable for the
 * connection's lifetime.  conn->fd is used by reaper_advance() instead
 * of a separate fd field.  See ARCHITECTURE.md §7.1.
 */
typedef struct braid_idle_entry {
	uint64_t last_active_ms;
	braid_conn_t *conn;
} braid_idle_entry_t;

/*
 * braid_idle_heap_t — min-heap of IDLE connections, keyed on last_active_ms.
 * See ARCHITECTURE.md §7.1.
 */
typedef struct braid_idle_heap {
	braid_idle_entry_t *entries;
	uint32_t count;
	uint32_t cap;
} braid_idle_heap_t;

/*
 * braid_waiter_t — one pending checkout request in the wait queue.
 * See ARCHITECTURE.md §10.
 */
typedef struct braid_waiter {
	braid_checkout_cb cb;
	void *cb_ctx;
	uint64_t deadline_ms;
	braid_token_t token;
	uint32_t flags; /* WAITER_FLAG_* bitfield */
} braid_waiter_t;

/*
 * braid_ring_t — fixed-size wait queue ring buffer.
 * See ARCHITECTURE.md §10.1.
 */
typedef struct braid_ring {
	braid_waiter_t *slots;
	uint32_t head;
	uint32_t tail;
	uint32_t count;
	uint32_t cap;
} braid_ring_t;

/*
 * braid_pool_t — the pool instance.
 * Allocated at braid_pool_create(); freed at braid_pool_destroy().
 * See ARCHITECTURE.md §13.
 */
struct braid_pool {
	braid_config_t config; /* deep copy; host is strdup'd */
	braid_conn_t *table; /* connection hash table slots */
	uint32_t table_size; /* 2 × max_connections */
	braid_reconnect_heap_t reconnect;
	braid_idle_heap_t idle;
	braid_ring_t waitq;
	uint32_t live_count; /* CONNECTING+INITIALIZING+IDLE+ACTIVE */
	uint32_t deferred_work; /* BRAID_DEFERRED_* flags */
	int in_callback; /* re-entrancy depth counter */
	int shutting_down;
	uint64_t prng; /* per-pool PRNG state */
};

/*
 * Struct size and layout assertions.
 * Fail at compile time if a field change silently alters a struct that
 * has externally-visible layout invariants.
 */

/*
 * braid_conn_t: on LP64 (x86_64, arm64) the fields pack to exactly 48
 * bytes with no padding — every field is naturally aligned.  If this
 * fires, a field was added, removed, or reordered; review the impact on
 * the hash table slot array and epoll tag pointer stability.
 */
_Static_assert(sizeof(braid_conn_t) == 48,
	       "braid_conn_t: unexpected size; check field layout");

/*
 * braid_fd_tag_t: the magic field must sit at offset 0 so that a
 * braid_fd_tag_t * recovered from epoll_data.ptr can be authenticated
 * by reading the first four bytes before any cast.
 */
_Static_assert(
    offsetof(braid_fd_tag_t, magic) == 0,
    "braid_fd_tag_t: magic must be at offset 0 for epoll authentication");

/*
 * braid_pool_t: the pool struct must be strictly larger than its
 * embedded config, confirming that all internal-state fields are present.
 */
_Static_assert(sizeof(struct braid_pool) > sizeof(braid_config_t),
	       "braid_pool_t: missing internal fields");

#endif /* BRAID_INTERNAL_H */
