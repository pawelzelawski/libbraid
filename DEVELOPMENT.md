# Development Plan

## Status Overview

**Last Updated**: 2026-03-31
**Current Phase**: Phase 5 complete
**Next Task**: Phase 6 — Pool Core

### Phase Summary

| Phase | Name | Status | Tests | Notes |
|---|---|---|---|---|
| 1 | Foundation | COMPLETE | ✓ | Build system, test harness, mock clock, skeleton |
| 2 | Connection Table | COMPLETE | ✓ | Hash table, open addressing, tombstone, compaction |
| 3 | Connection State Machine | COMPLETE | ✓ | conn_transition, record lifecycle, keepalive, socket creation |
| 4 | Wait Queue | COMPLETE | ✓ | Ring buffer, cancel, timeout expiry, deferred work flags |
| 5 | Reconnection Engine and Idle Reaper | COMPLETE | ✓ | Min-heaps, backoff, DNS, reap logic |
| 6 | Pool Core | NOT STARTED | — | checkout, checkin, advance, notify, pool lifecycle |
| 7 | OpenBSD (kqueue) Port | NOT STARTED | — | kqueue translation unit, all tests on OpenBSD |
| 8 | Hardening, Benchmarks, and Release | NOT STARTED | — | Integration tests, benchmarks, README |

### Quality Milestones

| ID | Milestone | Status |
|---|---|---|
| M1 | Build system works on Linux, both architectures | DONE |
| M2 | All unit tests pass on Linux | DONE |
| M3 | All unit tests pass on OpenBSD (Phase 7) | NOT STARTED |
| M4 | Valgrind clean on Linux | DONE |
| M5 | ASan/UBSan clean on Linux | DONE |
| M6 | ASan/UBSan clean on OpenBSD | NOT STARTED |
| M7 | TSan clean on Linux | NOT STARTED |
| M8 | clang-format clean | NOT STARTED |
| M9 | clang-tidy zero warnings | NOT STARTED |
| M10 | Hash table lookup correct under collision — verified by test | DONE |
| M11 | State machine rejects all illegal transitions — verified by test | DONE |
| M12 | One callback per checkout guaranteed — verified by test | DONE |
| M13 | Backoff exponent overflow guard verified by test | DONE |
| M14 | Reaper min_connections floor verified by test | DONE |
| M15 | Re-entrancy: checkin from checkout callback safe — verified by test | NOT STARTED |
| M16 | Integration test suite passes on Linux | NOT STARTED |
| M17 | Integration test suite passes on OpenBSD | NOT STARTED |

---

## Development Principles

Before starting any phase, read and follow these documents:

- **CODING_STANDARDS.md** — KNF style, error handling, state machine
  discipline, re-entrancy rules, fd lifecycle rules, safety patterns,
  pre-commit checklist. Every function written must comply.
- **ARCHITECTURE.md** — The specification. Every implementation decision
  must match what is documented there. If there is a conflict,
  ARCHITECTURE.md wins — raise it before deviating.
- **TECH_STACK.md** — Compiler flags, build system, system library usage,
  sanitizer integration.

**Bottom-up approach**: Each phase builds on the previous. Do not start a
phase until the prior phase is complete and all its tests pass cleanly with
Valgrind and ASan/UBSan on Linux.

**Test as you go**: Write tests for each component before moving to the
next. A component without passing tests is not done.

**Mock clock discipline**: All timer-dependent code uses `braid_now_ms()`
exclusively — never `clock_gettime()` directly. In test builds
(`BRAID_TEST_CLOCK` defined), `braid_now_ms()` reads
`braid_test_clock_ms`. This is a non-negotiable requirement for testability.
No test may use `sleep()` or `usleep()` for timing.

**No allocation after `braid_pool_create()`**: Every component allocates
its internal structures during its `_init()` call, which is called from
`braid_pool_create()`. No `malloc()` call appears on any subsequent code
path. If a code path requires allocation post-creation, the design is wrong.

**Single transition point**: `conn_transition()` is the only function that
writes `conn->state`. This rule must be actively verified during code review
at each phase — `grep -n "->state\s*=" src/*.c` must return only the line
inside `conn_transition()`.

**Phase ordering rationale**: The connection table (Phase 2) has no
dependencies. The state machine (Phase 3) depends on the table — it looks
up records on DEAD. The wait queue (Phase 4) depends on neither, but is
needed by the pool core. The reconnection engine and idle reaper (Phase 5)
depend on the state machine — both drive `conn_transition()`. The pool core
(Phase 6) depends on all prior phases. Phases cannot be reordered.

---

## Phase 1 — Foundation

**Goal**: Build system works on Linux on both architectures. Repository
structure is in place. Test harness compiles and runs. Skeleton headers and
source files are in place with correct include structure. Mock clock
infrastructure is in place. `_Static_assert` placeholders are in place.
Code compiles clean with zero warnings.

**Reference documents**:
- TECH_STACK.md §4 — compiler flags, build targets
- TECH_STACK.md §5 — Makefile structure
- TECH_STACK.md §6 — test harness structure
- CODING_STANDARDS.md §1.2 — file organisation and include order
- REPOSITORY_STRUCTURE.md §1 — top-level directory layout
- REPOSITORY_STRUCTURE.md §3 — source file list

### Tasks

**1.1 — Repository skeleton** ✓ DONE
- [x] Create directory structure per REPOSITORY_STRUCTURE.md §1:
  `src/`, `include/`, `tests/`, `bench/`
- [x] Create `include/braid.h` with skeleton: include guards, `<stdint.h>`
  include, empty forward typedefs for all public types (no function
  declarations yet)
- [x] Create `src/braid_internal.h` with skeleton: include guards, internal
  forward declarations, `BRAID_DEBUG_ASSERT` macro definition,
  `BRAID_TEST_CLOCK` conditional block for mock clock
- [x] Create stub `.c` files for all modules listed in REPOSITORY_STRUCTURE.md
  §3: each file includes its own header and compiles to an empty object.
  Stub files: `braid_table.c`, `braid_conn.c`, `braid_reconnect.c`,
  `braid_reaper.c`, `braid_waitq.c`, `braid_io_epoll.c`,
  `braid_io_kqueue.c` (excluded from Linux build), `braid_pool.c`
- [x] Create corresponding `.h` files for each stub `.c`
- [x] Create top-level `Makefile` with working platform detection
  (`$(shell uname)`) and correct `PLATFORM_SRCS` selection:
  `src/braid_io_epoll.c` on Linux, `src/braid_io_kqueue.c` on OpenBSD
- [x] Create `.clang-format` per TECH_STACK.md §6.6 (KNF-based)
- [x] Create `.clang-tidy` per TECH_STACK.md §6.5
- [x] Verify `make dev` and `make release` compile all stubs with zero warnings
  on Linux x86_64

**1.2 — Test harness and mock clock** ✓ DONE
- [x] `tests/test_harness.h`: `CHECK()` and `CHECK_ERR()` macros, pass/fail counters
- [x] `tests/run_tests.c`: test binary entry point
- [x] `braid_now_ms()` in `src/braid_internal.h`: both `BRAID_TEST_CLOCK` and
  production `clock_gettime(CLOCK_MONOTONIC)` paths
- [x] `braid_test_clock_ms` defined in `run_tests.c` under `BRAID_TEST_CLOCK`
- [x] `make test` defines `-DBRAID_TEST_CLOCK` and `-DBRAID_DEBUG`
- [x] `make test` prints `0/0 tests passed`, exits 0
- [x] `make valgrind` exits clean (ERROR SUMMARY: 0)

**1.3 — `_Static_assert` placeholders** ✓ DONE
- [x] Three placeholder `_Static_assert(1 == 1, "placeholder")` entries in
  `src/braid_internal.h` for struct size and offset checks

### Phase 1 Completion Criteria

- [x] `make dev` succeeds with zero warnings on Linux x86_64
- [x] `make dev` succeeds with zero warnings on Linux ARM64 (via GitHub Actions `ubuntu-24.04-arm`)
- [x] `make test` runs and prints `0/0 tests passed` on Linux
- [x] `make valgrind` exits clean on Linux x86_64
- [x] `make valgrind` exits clean on Linux ARM64 (via GitHub Actions `ubuntu-24.04-arm`;
      local Manjaro ARM skipped — Manjaro ARM is unmaintained, ships stripped
      `ld-linux-aarch64.so.1` which Valgrind requires unstripped)
- [ ] `make lint` produces zero warnings on all C stubs
- [x] Mock clock (`braid_now_ms`) compiles correctly under both
      `BRAID_TEST_CLOCK` and production builds
- [x] Quality milestone M1 confirmed (x86_64)

---

## Phase 2 — Connection Table

**Goal**: Open-addressed hash table keyed on fd is implemented and fully
tested. Lookup, insert, delete, tombstone handling, and compaction all work
correctly. No allocation after `table_init()`.

**Reference documents**:
- ARCHITECTURE.md §3 — full hash table specification
- CODING_STANDARDS.md §2.1 — return code conventions
- CODING_STANDARDS.md §2.2 — allocation failure handling
- REPOSITORY_STRUCTURE.md §3 — `braid_table.c` description

**Prerequisite**: Phase 1 complete.

### Tasks

**2.1 — Struct definitions** ✓ DONE
- [x] Define `braid_conn_t` in `src/braid_internal.h` per ARCHITECTURE.md §3.2:
  `fd`, `state`, `conn_ctx`, `created_at_ms`, `last_active_ms`, `flags`,
  `tag` (inline). `state` is `braid_state_t` (enum, values 0–5).
- [x] Define slot flag constants: `CONN_FLAG_TOMBSTONE`, `CONN_FLAG_CLOSING_DEFERRED`
- [x] Define `braid_state_t` enum: `BRAID_STATE_CONNECTING` through
  `BRAID_STATE_DEAD`
- [x] Add `_Static_assert` checks: `sizeof(braid_conn_t)==48`,
  `offsetof(braid_fd_tag_t, magic)==0`, `sizeof(pool) > sizeof(config)`

**2.2 — Hash table implementation (`src/braid_table.c`)** ✓ DONE
- [x] Implement `table_init(pool)`: allocate `2 × max_connections` slots of
  `braid_conn_t`; initialise all `fd` fields to -1 (empty); store
  `table_size = 2 × max_connections` in pool. See ARCHITECTURE.md §3.1.
- [x] Implement `table_destroy(pool)`: free slot array
- [x] Implement `table_lookup(pool, fd, **conn)`: hash `fd % table_size`;
  linear probe; skip tombstones (`CONN_FLAG_TOMBSTONE`); stop on empty
  slot (`fd == -1`); return found record or NULL.
  See ARCHITECTURE.md §3.3.
- [x] Implement `table_insert(pool, conn)`: probe for first empty or tombstone
  slot; write record; clear tombstone flag on reused slot
- [x] Implement `table_delete(pool, fd)`: set `CONN_FLAG_TOMBSTONE`; set
  `fd = -1`. No compaction — inline `conn->tag` addresses must remain
  stable. See ARCHITECTURE.md §3.3.

**2.3 — Tests (`tests/test_table.c`)** ✓ DONE
- [x] Implement all test cases from TESTING.md §3.1:
  `test_insert_and_lookup`, `test_probe_chain_collision`,
  `test_tombstone_skip_on_lookup`, `test_delete_vacates_slot`,
  `test_tombstone_reuse_on_insert`, `test_table_full_returns_error`,
  `test_fd_zero_valid_key`
- [x] Register all tests in `run_tests.c`
- [x] All tests pass with Valgrind and ASan/UBSan clean (26/26)

### Phase 2 Completion Criteria

- [x] All `test_table.c` tests pass on Linux (26/26)
- [x] Valgrind clean; ASan/UBSan clean
- [x] `table_lookup` O(1) average verified by probe-chain test
- [x] Tombstone reuse verified — no compaction function exists in codebase
- [x] No `malloc` after `table_init()` — verified by inspection
- [x] Quality milestone M10 confirmed

---

## Phase 3 — Connection State Machine

**Goal**: `conn_transition()` is implemented as the single state-write
enforcement point. All legal transitions succeed; all illegal transitions
are rejected in debug builds. State-entry invariants are enforced. Socket
creation with keepalive configuration and `braid_fd_tag_t` allocation work
correctly. The idle reaper heap and reconnection heap APIs exist as stubs
that the state machine calls — full implementations land in Phase 5.

**Reference documents**:
- ARCHITECTURE.md §4 — full state machine specification
- ARCHITECTURE.md §5 — TCP keepalive configuration
- ARCHITECTURE.md §8.2 — `braid_fd_tag_t` sentinel struct
- ARCHITECTURE.md §3.2 — connection record fields
- CODING_STANDARDS.md §3 — state machine discipline
- CODING_STANDARDS.md §5 — fd lifecycle rules
- REPOSITORY_STRUCTURE.md §3 — `braid_conn.c` description

**Prerequisite**: Phase 2 complete.

### Tasks

**3.1 — `braid_fd_tag_t` definition** ✓ DONE
- [x] Define `braid_fd_tag_t` in `src/braid_internal.h`:
  `uint32_t magic` (value `BRAID_FD_MAGIC`), `int fd`.
  Define `BRAID_FD_MAGIC` constant. See ARCHITECTURE.md §8.2.

**3.2 — Legal transition table** ✓ DONE
- [x] Define the legal transition table in `src/braid_conn.c` as a static
  array of `(from, to)` pairs matching ARCHITECTURE.md §4.2 exactly.
  Used by `conn_transition()` for legality assertion.

**3.3 — `conn_transition()` implementation** ✓ DONE
- [x] Implement `conn_transition(pool, conn, new_state)` per
  ARCHITECTURE.md §4.3:
  - Assert transition legality against table (debug build: abort with
    diagnostic; release build: return `BRAID_ERR_INVAL`)
  - Write `conn->state = new_state`
  - On IDLE entry: call `braid_now_ms()`, write `conn->last_active_ms`;
    call `reaper_heap_insert()` stub
  - On IDLE exit: call `reaper_heap_remove()` stub
  - On CONNECTING entry: call `braid_now_ms()`, write `conn->created_at_ms`
  - On CLOSING entry: increment `pool->in_callback`; call `destroy_fn` if
    registered; decrement `pool->in_callback`; if `pool->in_callback == 0`
    after decrement, call `conn_transition(→ DEAD)` directly; else set
    `CONN_FLAG_CLOSING_DEFERRED`
  - On DEAD entry: if `destroy_fn` not yet called (connection came from
    ACTIVE → DEAD directly), call `destroy_fn`; call `io_unwatch()`; call
    `close(fd)`; call `table_delete()`; decrement live connection count;
    if live count < `min_connections`, call `reconnect_heap_push()` stub;
    call `observe_fn` with `BRAID_EV_CONN_DESTROYED` if registered

**3.4 — `conn_alloc()` and `conn_socket_create()`** ✓ DONE
- [x] Implement `conn_alloc(pool, fd, **conn)`: acquire free slot via
  `table_insert()`; initialise all record fields to zero/NULL; initialise
  the inline `conn->tag` struct: set `conn->tag.magic = BRAID_FD_MAGIC`
  and `conn->tag.fd = fd`. The tag is embedded in the connection record —
  no separate allocation. Pass `&conn->tag` as `epoll_data.ptr` when
  registering with epoll. See ARCHITECTURE.md §8.2.
- [x] Implement `conn_keepalive_configure(fd, config)`: set `SO_KEEPALIVE`,
  `TCP_KEEPIDLE` (Linux) / `TCP_KEEPALIVE` (OpenBSD), `TCP_KEEPINTVL`,
  `TCP_KEEPCNT` per config values with documented defaults applied when
  config field is zero. Use `#ifdef __linux__` to select the correct idle
  constant. See ARCHITECTURE.md §5.
- [x] Implement `conn_socket_create(pool, addrinfo, *fd)`: call `socket()`;
  set `O_CLOEXEC` via `fcntl(fd, F_SETFD, FD_CLOEXEC)` and `O_NONBLOCK`
  via `fcntl(fd, F_SETFL, O_NONBLOCK)` — do not use `SOCK_NONBLOCK` or
  `SOCK_CLOEXEC` in the `socket()` call (not portable to OpenBSD); assert
  both flags set in debug build; call `conn_keepalive_configure()`; call
  non-blocking `connect()`; return fd. `EINPROGRESS` is the expected result
  — not an error. See ARCHITECTURE.md §6.3.

**3.5 — Heap stubs** ✓ DONE
- [x] Add stub `reaper_heap_insert(heap, conn)` and
  `reaper_heap_remove(heap, conn)` in `src/braid_reaper.c` — no-ops that
  compile and link. Signatures take `braid_conn_t *` so that Phase 5 can
  maintain `conn->heap_index` directly. Full implementation in Phase 5.
- [x] Add stub `reconnect_heap_push(heap, entry)` in `src/braid_reconnect.c`
  — no-op that compiles and links. Full implementation in Phase 5.

**3.6 — Tests (`tests/test_state_machine.c`)** ✓ DONE
- [x] Implement all test cases from TESTING.md §3.2:
  `test_all_legal_transitions`, `test_all_illegal_transitions`,
  `test_idle_entry_sets_last_active_ms`,
  `test_idle_entry_inserts_reaper_heap` (stub verified by call count),
  `test_idle_exit_removes_reaper_heap`,
  `test_connecting_entry_sets_created_at_ms`,
  `test_closing_calls_destroy_fn`,
  `test_closing_dead_closes_fd`,
  `test_closing_deferred_when_in_callback`,
  `test_deferred_close_fires_after_callback`,
  `test_dead_vacates_table_slot`,
  `test_dead_fires_conn_destroyed_event`
- [x] Register all tests in `run_tests.c`

### Phase 3 Completion Criteria

- [x] All `test_state_machine.c` tests pass on Linux (61/61)
- [x] Valgrind clean; ASan/UBSan clean
- [x] `grep -n "state = " src/braid_conn.c` returns only the line inside
      `conn_transition()` and the bootstrap write in `conn_alloc` — verified
- [x] All legal transitions return `BRAID_OK`
- [x] All illegal transitions rejected in debug build
- [x] `braid_fd_tag_t` allocated at `conn_alloc`, freed at DEAD
- [x] Quality milestone M11 confirmed

---

## Phase 4 — Wait Queue

**Goal**: Ring buffer wait queue is fully implemented and tested. FIFO
ordering, tombstone cancel, timeout expiry, and shutdown drain all work
correctly. Deferred work flag infrastructure is in place. One callback per
checkout is guaranteed under all outcomes.

**Reference documents**:
- ARCHITECTURE.md §10 — wait queue ring buffer specification
- ARCHITECTURE.md §9 — re-entrancy and deferred work
- CODING_STANDARDS.md §4 — re-entrancy and deferred work rules
- REPOSITORY_STRUCTURE.md §3 — `braid_waitq.c` description

**Prerequisite**: Phase 3 complete.

### Tasks

**4.1 — Deferred work infrastructure** ✓ DONE
- [x] Add `uint32_t in_callback` and `uint32_t deferred_work` fields to
  `braid_pool_t` in `src/braid_internal.h`
- [x] Define `BRAID_DEFERRED_SERVE_WAITQUEUE` and `BRAID_DEFERRED_PROCESS_DEAD`
  flag constants
- [x] Implement `braid_pool_drain_deferred(pool)` stub in `src/braid_pool.c`:
  processes `BRAID_DEFERRED_PROCESS_DEAD` first (no-op in this phase),
  then `BRAID_DEFERRED_SERVE_WAITQUEUE` (no-op in this phase). Full
  implementation in Phase 6. See ARCHITECTURE.md §9.2.
- [x] The `in_callback` increment/decrement/drain pattern must be in place and
  tested before wait queue logic is wired into the pool in Phase 6.

**4.2 — `braid_waiter_t` and ring buffer definition** ✓ DONE
- [x] Define `braid_waiter_t` in `src/braid_internal.h` per
  ARCHITECTURE.md §10.1: `cb`, `cb_ctx`, `deadline_ms`, `token`, `flags`
- [x] Define `WAITER_FLAG_TOMBSTONE` flag constant
- [x] Define `braid_ring_t` struct: `braid_waiter_t *slots`, `uint32_t head`,
  `uint32_t tail`, `uint32_t count`, `uint32_t capacity`

**4.3 — Wait queue implementation (`src/braid_waitq.c`)** ✓ DONE
- [x] Implement `waitq_init(ring, cap)`: allocate `cap` slots; initialise
  head, tail, count to 0
- [x] Implement `waitq_destroy(ring)`: free slot array
- [x] Implement `waitq_enqueue(ring, cb, cb_ctx, deadline_ms, *token)`:
  check capacity; write to `slots[tail % cap]`; set token to tail
  value; advance tail; increment count. Return BRAID_ERR_EXHAUSTED if full.
  See ARCHITECTURE.md §10.1.
- [x] Implement `waitq_serve_head(ring, fd, conn_ctx)`: skip tombstones by
  advancing head; invoke callback with `fd, conn_ctx, BRAID_OK, cb_ctx`;
  tombstone served slot before callback (one-callback guarantee); decrement
  count. See ARCHITECTURE.md §10.1.
- [x] Implement `waitq_cancel(ring, token)`: locate `slots[token % cap]`;
  verify token matches AND not already tombstoned; tombstone before
  callback; invoke callback with `BRAID_ERR_CANCELLED`; decrement count.
  See ARCHITECTURE.md §10.3.
- [x] Implement `waitq_expire(ring, now_ms)`: scan from head; drain
  tombstones (advance head); for each non-tombstone entry with
  `deadline_ms <= now_ms` (deadline_ms == 0 = no timeout), tombstone
  before callback, invoke `BRAID_ERR_TIMEOUT`, decrement count, advance
  head; stop at first non-expired non-tombstone entry. See §10.4.
- [x] Implement `waitq_shutdown(ring)`: scan full occupied span [head, tail);
  invoke all non-tombstone entries with `BRAID_ERR_SHUTDOWN` (tombstone
  before callback); reset count to 0; advance head to tail

**4.4 — Tests (`tests/test_wait_queue.c`)** ✓ DONE
- [x] Implement all test cases from TESTING.md §3.3:
  `test_enqueue_dequeue_fifo`, `test_tombstone_skip_on_dequeue`,
  `test_cancel_by_token`, `test_cancel_already_served_noop`,
  `test_timeout_expiry_scan`, `test_expiry_stops_at_first_nonfired`,
  `test_shutdown_drains_all`, `test_one_callback_cancel_after_serve`,
  `test_one_callback_timeout_after_cancel`,
  `test_ring_wraparound`, `test_full_ring_rejects_enqueue`
- [x] All timer-dependent tests use `braid_test_clock_ms` — no `sleep()`
- [x] Register all tests in `run_tests.c`

### Phase 4 Completion Criteria

- [x] All `test_wait_queue.c` tests pass on Linux (168/168)
- [x] Valgrind clean; ASan/UBSan clean
- [x] One-callback guarantee verified under cancel-after-serve and
      timeout-after-cancel scenarios
- [x] Ring wrap-around verified
- [x] `in_callback` and `deferred_work` fields compile and link correctly
- [x] Quality milestone M12 confirmed

---

## Phase 5 — Reconnection Engine and Idle Reaper

**Goal**: Both min-heaps are fully implemented and replace the stubs from
Phase 3. Full jitter backoff is correct including overflow guard. The idle
reaper respects the `min_connections` floor. Both components drive
`conn_transition()` correctly. `braid_pool_advance()` can compute accurate
`next_ms` from both heaps.

**Reference documents**:
- ARCHITECTURE.md §6 — reconnection engine and backoff algorithm
- ARCHITECTURE.md §7 — idle reaper heap and reap logic
- CODING_STANDARDS.md §6 — integer safety, overflow guards
- REPOSITORY_STRUCTURE.md §3 — `braid_reconnect.c`, `braid_reaper.c`

**Prerequisite**: Phase 4 complete.

### Tasks

**5.1 — Reconnection heap (`src/braid_reconnect.c`)** ✓ DONE
- [x] Define `braid_reconnect_entry_t` in `src/braid_internal.h`:
  `next_retry_ms` (uint64_t), `attempt` (uint32_t)
- [x] Define `braid_reconnect_heap_t`: `braid_reconnect_entry_t *entries`,
  `uint32_t count`, `uint32_t capacity`
- [x] Replace stub `reconnect_heap_push()` with full implementation:
  O(log n) insert with bubble-up on `next_retry_ms`
- [x] Implement `reconnect_heap_peek(heap)`: return minimum `next_retry_ms`
  or `UINT64_MAX` if empty
- [x] Implement `reconnect_heap_pop(heap)`: O(log n) delete-min with sift-down
- [x] Implement `reconnect_heap_clear(heap)`: set count to 0
- [x] Implement `reconnect_heap_init(heap, cap)`: allocate `cap` entries
- [x] Implement `reconnect_heap_destroy(heap)`: free entries

**5.2 — Backoff algorithm** ✓ DONE
- [x] Add `uint64_t prng` field to `braid_pool_t` in `src/braid_internal.h`.
  Seed at pool creation using `getentropy()` on Linux/OpenBSD (or
  `arc4random_buf()` on OpenBSD). This is the per-pool PRNG state.
  See ARCHITECTURE.md §6.2.
- [x] Implement `reconnect_backoff_delay(pool, attempt)`:
  `exp = (attempt < 31) ? attempt : 31`;
  `window = (uint64_t)config->backoff_base << exp`;
  `if (window > config->backoff_cap) window = config->backoff_cap`;
  return `pool_prng_uniform(pool, 0, window)` using pool->prng.
  Do NOT use process-global `random()` / `srandom()` — not multi-pool safe.
  See ARCHITECTURE.md §6.2. Apply default values when config fields are
  zero per ARCHITECTURE.md §16.

**5.3 — Reconnection attempt flow** ✓ DONE
- [x] Implement `reconnect_attempt(pool, entry)` per ARCHITECTURE.md §6.3:
  1. Call `getaddrinfo()` on `pool->config.host`. On failure: insert
     reconnect entry for `attempt+1` with backoff delay; return.
  2. Call `conn_socket_create()` (socket + fcntl + keepalive + connect).
  3. If `connect()` returns 0 immediately (fast local connect): skip
     CONNECTING state. Call `conn_alloc()`, `conn_transition(→ INITIALIZING)`,
     invoke `init_fn` if present, `conn_transition(→ IDLE)` on success or
     `→ DEAD` on failure. Register fd for readability.
  4. If `connect()` returns `EINPROGRESS`: call `conn_alloc()`,
     `conn_transition(→ CONNECTING)`, `io_watch()` for writability.
  5. If `connect()` returns any other error: insert reconnect entry for
     `attempt+1` with backoff delay; fire `BRAID_EV_RECONNECT_ATTEMPT`.
  **Do not pre-insert a fallback entry at attempt start.** Reconnect
  entries are inserted only on failure paths or on CONNECTING→DEAD
  transition (see §4.3). If the attempt succeeds, no follow-up entry
  is inserted.
- [x] Implement `reconnect_advance(pool, now_ms)`: pop all entries with
  `next_retry_ms <= now_ms`; call `reconnect_attempt()` for each;
  skip and fire failure event if `max_attempts > 0` and
  `entry.attempt >= max_attempts`

**5.4 — Idle reaper heap (`src/braid_reaper.c`)** ✓ DONE
- [x] Define `braid_idle_entry_t` in `src/braid_internal.h`:
  `last_active_ms` (uint64_t), `conn` (braid_conn_t*)
- [x] Define `braid_idle_heap_t`: `braid_idle_entry_t *entries`, `uint32_t
  count`, `uint32_t capacity`. No parallel index array — heap position is
  stored in `conn->heap_index` on the connection record itself.
  See ARCHITECTURE.md §7.1 for rationale.
- [x] Replace stub `reaper_heap_insert(heap, conn)` with full implementation:
  O(log n) insert with sift-up; write assigned position to
  `conn->heap_index` at every swap; set `conn->heap_index` to final
  position on completion. See ARCHITECTURE.md §7.1.
- [x] Replace stub `reaper_heap_remove(heap, conn)` with full implementation:
  read position from `conn->heap_index`; swap with last entry; decrement
  count; attempt sift-up from position first, then sift-down — BOTH
  directions required (moved element may need sift-up if smaller than new
  parent, or sift-down if larger than a child); update `heap_index` on all
  swapped records; set `conn->heap_index = UINT32_MAX`.
  See ARCHITECTURE.md §7.1.
- [x] Implement `reaper_heap_peek(heap)`: return minimum `last_active_ms` or
  `UINT64_MAX` if empty
- [x] Implement `reaper_heap_init(heap, cap)`: allocate `cap` entries;
  initialise count to 0
- [x] Implement `reaper_heap_destroy(heap)`: free entries array

**5.5 — Reaper advance logic** ✓ DONE
- [x] Implement `reaper_advance(pool, now_ms)` per ARCHITECTURE.md §7.2:
  peek heap minimum; if `now_ms - min.last_active_ms >=
  config.idle_reap_timeout` AND live count > `min_connections`: look up
  connection by fd; call `conn_transition(→ CLOSING)`; repeat until
  heap minimum is in the future or floor would be breached.
  Apply default `idle_reap_timeout` if config field is zero.

**5.6 — Tests (`tests/test_reconnect.c`, `tests/test_reaper.c`)** ✓ DONE
- [x] Implement all test cases from TESTING.md §3.4 in `test_reconnect.c`:
  `test_heap_push_pop_ordering`, `test_heap_peek_returns_minimum`,
  `test_backoff_attempt_0`, `test_backoff_attempt_5`,
  `test_backoff_attempt_31`, `test_backoff_attempt_64`,
  `test_backoff_cap_respected`, `test_max_attempts_zero_retries_forever`,
  `test_max_attempts_N_stops`, `test_reconnect_advance_fires_due`,
  `test_reconnect_advance_skips_future`,
  `test_reconnect_attempt_event_fired`
- [x] Implement all test cases from TESTING.md §3.5 in `test_reaper.c`:
  `test_heap_insert_and_peek`, `test_heap_remove_by_conn`,
  `test_heap_index_consistency_after_sift_up`,
  `test_heap_index_consistency_after_sift_down`,
  `test_reaper_advance_reaps_eligible`,
  `test_reaper_advance_respects_floor`,
  `test_reaper_advance_stops_at_future`,
  `test_next_ms_computed_correctly`
- [x] All timer-dependent tests use `braid_test_clock_ms`
- [x] Register all tests in `run_tests.c`

### Phase 5 Completion Criteria

- [x] All `test_reconnect.c` tests pass on Linux
- [x] All `test_reaper.c` tests pass on Linux
- [x] Valgrind clean; ASan/UBSan clean
- [x] Backoff exponent overflow guard verified (`attempt >= 31` clamped)
- [x] `backoff_cap` respected at all attempt values
- [x] Reaper `heap_index` consistent on conn record after every insert,
      remove, sift-up, and sift-down operation
- [x] Reaper `min_connections` floor verified
- [x] Quality milestones M13 and M14 confirmed

---

## Phase 6 — Pool Core

**Goal**: The full public API is implemented and tested. `braid_pool_create()`
allocates and wires all components. `braid_pool_checkout()`,
`braid_pool_checkin()`, `braid_pool_cancel()`, `braid_pool_advance()`, and
`braid_pool_notify()` all work correctly. Re-entrancy is handled via the
deferred work mechanism. `braid_pool_destroy()` tears down cleanly under all
conditions. The epoll abstraction layer is fully implemented. All pool-level
tests pass.

**Reference documents**:
- ARCHITECTURE.md §8 — epoll abstraction layer and fd tagging
- ARCHITECTURE.md §9 — re-entrancy and deferred work
- ARCHITECTURE.md §11 — `braid_pool_advance()` execution order
- ARCHITECTURE.md §12 — `braid_pool_notify()` dispatch
- ARCHITECTURE.md §13 — pool lifecycle
- ARCHITECTURE.md §16 — default values (applied when config fields are zero)
- ARCHITECTURE.md §18 — error codes
- CODING_STANDARDS.md §4 — re-entrancy rules
- REPOSITORY_STRUCTURE.md §3 — `braid_pool.c`, `braid_io_epoll.c`

**Prerequisite**: Phase 5 complete.

### Tasks

**6.1 — `braid_pool_t` struct definition** ✓ DONE
- Define `braid_pool_t` in `src/braid_internal.h`: config copy, connection
  table, reconnection heap, idle reaper heap, wait queue ring, live
  connection count, shutdown flag, `in_callback`, `deferred_work`

**6.2 — epoll abstraction layer (`src/braid_io_epoll.c`)** ✓ DONE
- [x] Implement `io_watch(pool, fd, events)`: `epoll_ctl(EPOLL_CTL_ADD)` with
  `EPOLLET`; `epoll_data.ptr = conn->tag`. See ARCHITECTURE.md §8.1.
- [x] Implement `io_modify(pool, fd, events)`: `epoll_ctl(EPOLL_CTL_MOD)`
- [x] Implement `io_unwatch(pool, fd)`: `epoll_ctl(EPOLL_CTL_DEL)`
- [x] Translate `BRAID_IO_READ` → `EPOLLIN`, `BRAID_IO_WRITE` → `EPOLLOUT`

**6.3 — `braid_pool_create()` and `braid_pool_destroy()`** ✓ DONE
- [x] Implement `braid_pool_create(config, *err)` per ARCHITECTURE.md §13.1:
  validate config (`event_fd >= 0`, `min_connections <= max_connections`,
  `max_connections > 0`); allocate `braid_pool_t`; call `table_init()`,
  `reconnect_heap_init()`, `reaper_heap_init()`, `waitq_init()`; deep-copy
  `config.host` via `strdup()`; insert `min_connections` reconnect entries
  with `next_retry_ms = 0`. Use `goto cleanup` for all failure paths.
  Apply default values for all zero config fields per ARCHITECTURE.md §16.
- [x] Implement `braid_pool_destroy(pool, drain_timeout_ms)` per
  ARCHITECTURE.md §13.2: mark shutdown; `waitq_shutdown()`; if
  `drain_timeout_ms > 0`, poll with short sleeps waiting for ACTIVE
  connections; transition all CONNECTING sockets to DEAD via `io_unwatch()`
  + `close()` (no destroy_fn); transition all INITIALIZING connections to
  DEAD via destroy_fn + close(); reap all IDLE connections via
  `conn_transition(→ CLOSING)`; clear reconnection heap; `io_unwatch()`
  any remaining registered fds; free all structures including
  `strdup`'d host.

**6.4 — `braid_pool_checkout()` and `braid_pool_checkin()`** ✓ DONE
- [x] Implement `braid_pool_checkout(pool, timeout_ms, cb, cb_ctx, *token)`:
  - Reject if pool shutting down: return `BRAID_ERR_SHUTDOWN`
  - Find any IDLE connection in table; if found, check
    `last_active_ms` against `idle_threshold`: if exceeded, call
    `validate_fn` (with `in_callback` protocol); if validate fails,
    call `conn_transition(→ CLOSING)` and continue search
  - If IDLE connection found and valid: call `conn_transition(→ ACTIVE)`;
    increment `in_callback`; invoke callback with `fd, conn_ctx,
    BRAID_OK`; decrement `in_callback`; drain deferred work
  - If no IDLE connection and `timeout_ms == 0`: fire
    `BRAID_EV_POOL_EXHAUSTED`; return `BRAID_ERR_EXHAUSTED` (no callback)
  - Otherwise: compute `deadline_ms = braid_now_ms() + timeout_ms`;
    call `waitq_enqueue()`; write token; fire `BRAID_EV_POOL_EXHAUSTED`
    if pool at `max_connections`
- [x] Implement `braid_pool_checkin(pool, fd, flags)`:
  - `table_lookup(fd)` — return `BRAID_ERR_INVAL` if not found
  - Assert `BRAID_STATE_ACTIVE` in debug build
  - If `flags == BRAID_CONN_OK`: call `conn_transition(→ IDLE)`; if
    `in_callback == 0`, call `waitq_serve_head()` if queue non-empty;
    else set `BRAID_DEFERRED_SERVE_WAITQUEUE`
  - If `flags == BRAID_CONN_DISCARD`: call `conn_transition(→ CLOSING)`

**6.5 — `braid_pool_cancel()`** ✓ DONE
- [x] Implement `braid_pool_cancel(pool, token)`: call `waitq_cancel(ring,
  token)` with `in_callback` protocol around callback invocation

**6.6 — `braid_pool_advance()`**
- Implement `braid_pool_advance(pool, *next_ms)` per ARCHITECTURE.md §11:
  1. `now_ms = braid_now_ms()`
  2. `reconnect_advance(pool, now_ms)`; track heap minimum for `next_ms`
  2a. Scan CONNECTING connections: for each where
      `now_ms > conn->created_at_ms + config.connect_timeout`, call
      `io_unwatch()`, close fd, `conn_transition(→ DEAD)`. Track soonest
      non-expired CONNECTING deadline for `next_ms`.
  3. `reaper_advance(pool, now_ms)`; track heap minimum for `next_ms`
  4. `waitq_expire(ring, now_ms)`; track soonest expiry for `next_ms`
  5. `pool_drain_deferred(pool)` if `in_callback == 0`
  6. Write minimum of all next-event times to `*next_ms`;
     `UINT32_MAX` if no events scheduled. Return `BRAID_OK`.
- Implement `pool_drain_deferred(pool)` fully: process
  `BRAID_DEFERRED_PROCESS_DEAD` (re-run DEAD transition for deferred
  connections), then `BRAID_DEFERRED_SERVE_WAITQUEUE` (call
  `waitq_serve_head()`). Internal function — no `braid_` prefix.
  See ARCHITECTURE.md §9.2.

**6.7 — `braid_pool_notify()`**
- Implement `braid_pool_notify(pool, fd, events)` per
  ARCHITECTURE.md §12:
  - `table_lookup(fd)` — silently return if not found (timing artefact)
  - CONNECTING + writable: `getsockopt(SO_ERROR)`; if error:
    `conn_transition(→ DEAD)`; else: `conn_transition(→ INITIALIZING)`;
    call `init_fn` with `in_callback` protocol and deadline; on success
    `conn_transition(→ IDLE)`; on failure `conn_transition(→ DEAD)`;
    `io_modify()` from write to read
  - IDLE + readable: call `recv(fd, &probe_byte, 1, MSG_PEEK)`.
    If returns -1 with EAGAIN: spurious wakeup, no action.
    If returns 0, 1, or -1 with any other error: half-open or unclean
    connection, `conn_transition(→ CLOSING)`. See ARCHITECTURE.md §12.
  - ACTIVE: silently ignore
  - CLOSING, DEAD: silently ignore

**6.8 — Tests (`tests/test_pool.c`)**
- Implement all test cases from TESTING.md §3.6:
  `test_pool_create_valid`, `test_pool_create_null_event_fd`,
  `test_pool_create_min_gt_max`, `test_pool_destroy_no_active`,
  `test_pool_destroy_with_active`, `test_checkout_immediate`,
  `test_checkout_enqueues`, `test_checkin_conn_ok`,
  `test_checkin_conn_discard`, `test_checkin_unknown_fd`,
  `test_cancel_pending`, `test_cancel_fired_token`,
  `test_cancel_stale_wrapped_token_noop`,
  `test_connect_timeout_aborts_connecting`,
  `test_shutdown_suppresses_reconnect`,
  `test_exhaustion_zero_timeout`, `test_checkin_from_callback`,
  `test_shutdown_cancels_waiters`, `test_observe_fn_null_no_crash`,
  `test_validate_fn_called_above_threshold`,
  `test_validate_fn_failure_discards`, `test_init_fn_deadline_exceeded`
- All timer-dependent tests use `braid_test_clock_ms` and a test
  epoll fd
- Register all tests in `run_tests.c`

### Phase 6 Completion Criteria

- [ ] All `test_pool.c` tests pass on Linux
- [ ] Valgrind clean; ASan/UBSan clean
- [ ] `braid_pool_create()` / `braid_pool_destroy()` cycle: no leaks
      (Valgrind confirms)
- [ ] Re-entrancy test passes: checkin from within checkout callback
      completes without crash or double callback
- [ ] Stale-token cancel verified: wrapped token does not cancel newer waiter
- [ ] connect_timeout verified: CONNECTING socket aborted after deadline
- [ ] Shutdown suppresses reconnect: no reconnect entries inserted after
      pool marked shutting down
- [ ] `make lint` zero warnings
- [ ] All default config values applied correctly when fields are zero
- [ ] Quality milestone M15 confirmed

---

## Phase 7 — OpenBSD (kqueue) Port

**Goal**: The kqueue translation unit is implemented. All tests pass on
OpenBSD x86_64 and ARM64. The codebase has no Linux-specific assumptions
outside `src/braid_io_epoll.c`.

**Reference documents**:
- ARCHITECTURE.md §8.1 — epoll abstraction interface
- ARCHITECTURE.md §15.3 — kqueue implementation notes
- TECH_STACK.md §3.5 — kqueue system headers

**Prerequisite**: Phase 6 complete. All tests passing on Linux.

### Tasks

**7.1 — kqueue translation unit (`src/braid_io_kqueue.c`)**
- Implement `io_watch(pool, fd, events)`: `kevent(EVFILT_READ, EV_ADD)`
  or `kevent(EVFILT_WRITE, EV_ADD)` per `events` bitmask; set
  `udata = &conn->tag`. See ARCHITECTURE.md §15.3.
- Implement `io_modify(pool, fd, events)`: delete old filter(s) via
  `EV_DELETE`; add new filter(s)
- Implement `io_unwatch(pool, fd)`: `EV_DELETE` for all active filters
  on fd
- The caller's kqueue fd is passed via `config.event_fd` — the same
  field used for the epoll fd on Linux. The field name is platform-neutral
  from Phase 1 onward.

**7.2 — Platform test pass**
- Run the full test suite on OpenBSD x86_64 and ARM64:
  `make test`, `make valgrind` (unavailable — use ASan instead),
  `make dev` (ASan/UBSan)
- Fix any failures — common sources: byte order assumptions, `getaddrinfo`
  flag differences, `MSG_PEEK` behaviour differences
- Run integration tests (`test_integration.c`) on OpenBSD

### Phase 7 Completion Criteria

- [ ] All unit tests pass on OpenBSD x86_64 and ARM64
- [ ] ASan/UBSan clean on OpenBSD
- [ ] No `#ifdef` for platform differences outside `src/braid_io_epoll.c`
      and `src/braid_io_kqueue.c`
- [ ] Quality milestones M3 and M6 confirmed
- [ ] Quality milestone M17 confirmed

---

## Phase 8 — Hardening, Benchmarks, and Release

**Goal**: Full integration test suite passes on Linux and OpenBSD. All
benchmarks produce baseline numbers. All quality milestones confirmed.
README.md complete. Library is ready for a v0.1.0 release tag.

**Prerequisite**: Phase 7 complete. All unit tests passing on all platforms.

### Tasks

**8.1 — Integration test suite**
- Implement all integration tests in `tests/test_integration.c` per
  TESTING.md §4:
  full connect/checkout/checkin/reuse cycle, warm pool, half-open
  detection via MSG_PEEK, write error on ACTIVE connection, reconnection
  after server restart, backoff desynchronisation, validate_fn with real
  socket, validate_fn timeout, init_fn handshake simulation, destroy_fn
  graceful teardown, destroy_fn unknown state, observe_fn event sequence,
  BRAID_EV_POOL_EXHAUSTED, BRAID_EV_CHECKOUT_TIMEOUT, single-connection
  concurrent checkouts, pool destroy during reconnection
- All integration tests pass on Linux — quality milestone M16
- All integration tests pass on OpenBSD — quality milestone M17

**8.2 — Benchmark suite**
- Implement all benchmarks in `bench/` per REPOSITORY_STRUCTURE.md §5:
  `bench_checkout.c`, `bench_advance.c`, `bench_reconnect.c`,
  `bench_pool_scale.c`
- Run on Linux x86_64 and ARM64; record baselines with full hardware
  context per TESTING.md §7.2

**8.3 — Final quality pass**
- `make lint` → zero clang-tidy and cppcheck warnings on all platforms
- `make test` → all tests pass on Linux and OpenBSD
- `make valgrind` → clean on Linux
- Full ASan/UBSan run on both platforms
- Full TSan run on Linux
- All quality milestone status cells updated to DONE

**8.4 — README.md**
- Written for an embedder coming to the project cold
- Contents: one-paragraph description, requirements (libc only),
  how to build (`make && make install`), minimal integration example
  (pool create, advance loop, checkout/checkin pattern), known
  limitations (synchronous DNS in advance, cooperative caller-owns-loop,
  one-connection-per-checkout not suited for HTTP/2 upstream),
  link to ARCHITECTURE.md for full design

### Phase 8 Completion Criteria

- [ ] Integration test suite passes on Linux — M16 confirmed
- [ ] Integration test suite passes on OpenBSD — M17 confirmed
- [ ] Benchmark baselines recorded on Linux x86_64 and ARM64,
      OpenBSD x86_64
- [ ] `make lint` zero warnings — M8, M9 confirmed
- [ ] All tests pass on all platforms — M2, M3 confirmed
- [ ] Valgrind clean — M4 confirmed
- [ ] ASan/UBSan clean on both platforms — M5, M6 confirmed
- [ ] TSan clean on Linux — M7 confirmed
- [ ] All quality milestone status cells updated to DONE
- [ ] README.md complete and integration example compiles and runs
- [ ] v0.1.0 release tag applied

---

## Cross-Reference Index

| Topic | Primary reference | Secondary reference |
|---|---|---|
| Hash table design and slot states | ARCHITECTURE.md §3 | REPOSITORY_STRUCTURE.md §3 (braid_table.c) |
| Connection record fields | ARCHITECTURE.md §3.2 | CODING_STANDARDS.md §3 |
| Hash table lookup, insert, delete | ARCHITECTURE.md §3.3 | REPOSITORY_STRUCTURE.md §3 (braid_table.c) |
| State machine legal transitions | ARCHITECTURE.md §4.2 | CODING_STANDARDS.md §3.2 |
| conn_transition() responsibilities | ARCHITECTURE.md §4.3 | CODING_STANDARDS.md §3.1 |
| TCP keepalive configuration | ARCHITECTURE.md §5 | TECH_STACK.md §3.3 |
| Reconnection engine and backoff | ARCHITECTURE.md §6 | REPOSITORY_STRUCTURE.md §3 (braid_reconnect.c) |
| Full jitter backoff algorithm | ARCHITECTURE.md §6.2 | CODING_STANDARDS.md §6 |
| Idle reaper heap and floor | ARCHITECTURE.md §7 | REPOSITORY_STRUCTURE.md §3 (braid_reaper.c) |
| Idle reaper heap_index on conn record | ARCHITECTURE.md §7.1 | REPOSITORY_STRUCTURE.md §3 (braid_reaper.c) |
| epoll abstraction interface | ARCHITECTURE.md §8.1 | REPOSITORY_STRUCTURE.md §3 (braid_io.h) |
| braid_fd_tag_t sentinel struct | ARCHITECTURE.md §8.2 | CODING_STANDARDS.md §5 |
| Re-entrancy and deferred work | ARCHITECTURE.md §9 | CODING_STANDARDS.md §4 |
| Wait queue ring buffer | ARCHITECTURE.md §10 | REPOSITORY_STRUCTURE.md §3 (braid_waitq.c) |
| braid_pool_advance() execution order | ARCHITECTURE.md §11 | REPOSITORY_STRUCTURE.md §3 (braid_pool.c) |
| braid_pool_notify() dispatch | ARCHITECTURE.md §12 | REPOSITORY_STRUCTURE.md §3 (braid_pool.c) |
| Pool create and destroy | ARCHITECTURE.md §13 | REPOSITORY_STRUCTURE.md §3 (braid_pool.c) |
| Monotonic time and mock clock | ARCHITECTURE.md §14 | TESTING.md §2.3 |
| kqueue implementation | ARCHITECTURE.md §15.3 | REPOSITORY_STRUCTURE.md §3 (braid_io_kqueue.c) |
| Default config values | ARCHITECTURE.md §16 | — |
| Error codes | ARCHITECTURE.md §18 | — |
| SAFETY comment convention | CODING_STANDARDS.md §7.3 | — |
| Pre-commit checklist | CODING_STANDARDS.md §8 | — |
| Mock clock (BRAID_TEST_CLOCK) | TESTING.md §2.3 | — |
| Integration test catalogue | TESTING.md §4 | REPOSITORY_STRUCTURE.md §4 |

---

**Document Version**: 1.0
**Last Updated**: 2026-03-30
**See Also**: PROJECT.md, ARCHITECTURE.md, TECH_STACK.md, CODING_STANDARDS.md,
REPOSITORY_STRUCTURE.md, TESTING.md
