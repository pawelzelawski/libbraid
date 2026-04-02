# Repository Structure

## 1. Top-Level Layout

```
libbraid/
├── Makefile                    # Build orchestration: dev, release, test, bench, lint, install
├── .clang-format               # Code formatting rules (KNF-based)
├── .clang-tidy                 # Static analysis configuration
├── README.md                   # Embedder-facing introduction and quickstart
├── PROJECT.md                  # Overview, goals, scope, design philosophy
├── ARCHITECTURE.md             # Full internal architecture — data structures,
│                               #   state machine, reconnection engine, idle reaper,
│                               #   epoll abstraction, re-entrancy handling
├── TECH_STACK.md               # Build system, compiler flags, system libraries, tools
├── CODING_STANDARDS.md         # C style, error handling, safety patterns, checklist
├── REPOSITORY_STRUCTURE.md     # This file
├── DEVELOPMENT.md              # Phased build plan, milestones, task breakdown
├── TESTING.md                  # Test strategy, test catalogue, CI approach
├── include/                    # Public header
├── src/                        # Library source files
├── tests/                      # Unit and integration tests
└── bench/                      # Performance benchmarks
```

The build output is `libbraid.a`. No binary is installed. `make install`
installs exactly two files: `libbraid.a` into `$(LIBDIR)` and
`include/braid.h` into `$(INCLUDEDIR)`.

---

## 2. include/ — Public Header

```
include/
│
└── braid.h             # The only header an embedder includes.
                        # Self-contained: pulls in only <stdint.h>.
                        # Declares all public types, constants, and functions:
                        #   braid_pool_t, braid_config_t, braid_token_t,
                        #   braid_event_t, braid_event_type_t,
                        #   braid_checkout_cb and all hook typedefs,
                        #   BRAID_OK, BRAID_ERR_*, BRAID_CONN_OK,
                        #   BRAID_CONN_DISCARD,
                        #   braid_pool_create, braid_pool_destroy,
                        #   braid_pool_checkout, braid_pool_checkin,
                        #   braid_pool_cancel, braid_pool_advance,
                        #   braid_pool_notify.
                        # Does NOT expose any internal types, struct layouts,
                        #   or implementation details.
                        # See ARCHITECTURE.md for full API documentation.
                        # See README.md for a quickstart integration example.
```

`braid.h` is the entire public surface of the library. An embedder adds
`-I/usr/local/include` (or wherever installed) and writes
`#include <braid.h>`. Nothing else is needed.

---

## 3. src/ — Library Source Files

Library source is organised by component. Each component is one `.c` file
with a corresponding internal `.h` file. The epoll abstraction layer has one
translation unit per platform, selected by the Makefile. Internal headers are
never installed and never included by callers.

```
src/
│
│   — Internal shared definitions —
│
├── braid_internal.h    # Internal shared types, forward declarations,
│                       #   and compile-time assertions.
│                       # Complete definitions of all internal structs:
│                       #   braid_pool_t, braid_conn_t, braid_fd_tag_t,
│                       #   braid_reconnect_entry_t, braid_reconnect_heap_t,
│                       #   braid_idle_heap_t, braid_waiter_t, braid_ring_t.
│                       # braid_conn_t includes heap_index (uint32_t) — heap
│                       #   position for O(1) idle reaper removal; UINT32_MAX
│                       #   when connection is not in the heap.
│                       # braid_conn_t includes inline braid_fd_tag_t tag —
│                       #   embedded directly, not a pointer; pre-allocated
│                       #   as part of the connection table; no malloc needed.
│                       # braid_pool_t includes uint64_t prng — per-pool PRNG
│                       #   state seeded at braid_pool_create(); used by the
│                       #   reconnection backoff jitter. Not process-global.
│                       # braid_state_t enum definition.
│                       # Internal constants: BRAID_FD_MAGIC,
│                       #   CONN_FLAG_TOMBSTONE, CONN_FLAG_CLOSING_DEFERRED,
│                       #   CONN_FLAG_EVER_ACTIVE,
│                       #   WAITER_FLAG_TOMBSTONE, BRAID_IO_READ, BRAID_IO_WRITE,
│                       #   BRAID_DEFERRED_SERVE_WAITQUEUE,
│                       #   BRAID_DEFERRED_PROCESS_DEAD.
│                       # BRAID_DEBUG_ASSERT macro definition.
│                       # _Static_assert checks on key struct sizes.
│                       # NOT included by embedders — internal only.
│                       # See CODING_STANDARDS.md §3 for state machine rules.
│
│   — Connection Table —
│
├── braid_table.c       # Open-addressed hash table keyed on fd.
│                       # table_init(): allocate 2 × max_connections slots.
│                       # table_destroy(): free slot array.
│                       # table_lookup(pool, fd, **conn): linear probe from
│                       #   fd % table_size; skip tombstones; O(1) average.
│                       # table_insert(pool, conn): find first empty or
│                       #   tombstone slot; write record.
│                       # table_delete(pool, fd): set CONN_FLAG_TOMBSTONE,
│                       #   clear fd to -1. No compaction — inline tag
│                       #   addresses must remain stable. See ARCHITECTURE.md §3.3.
│                       # See ARCHITECTURE.md §3.
│
├── braid_table.h       # Connection table internal interface.
│                       # table_init(), table_destroy(), table_lookup(),
│                       #   table_insert(), table_delete() declarations.
│
│   — Connection State Machine —
│
├── braid_conn.c        # Connection record lifecycle and state transitions.
│                       # conn_transition(pool, conn, new_state):
│                       #   — asserts transition legality in debug builds;
│                       #   — writes conn->state;
│                       #   — on IDLE entry: records last_active_ms,
│                       #       inserts into idle reaper heap;
│                       #   — on IDLE exit: removes from idle reaper heap;
│                       #   — on CONNECTING entry: records created_at_ms;
│                       #   — on CLOSING entry: calls destroy_fn, then
│                       #       transitions to DEAD (or sets
│                       #       CONN_FLAG_CLOSING_DEFERRED if in_callback > 0);
│                       #   — on DEAD entry: io_unwatch(), calls destroy_fn if
│                       #       not already called, clears conn->tag.magic to
│                       #       invalidate inline tag, closes fd, vacates table
│                       #       slot, decrements live count, inserts reconnect
│                       #       entry if below min_connections, fires
│                       #       BRAID_EV_CONN_DESTROYED.
│                       # conn_alloc(pool, fd, **conn): acquire free slot,
│                       #   initialise record, set inline conn->tag.magic and
│                       #   conn->tag.fd — no separate allocation needed.
│                       # conn_keepalive_configure(fd, config): set
│                       #   SO_KEEPALIVE, TCP_KEEPIDLE/TCP_KEEPALIVE,
│                       #   TCP_KEEPINTVL, TCP_KEEPCNT per pool config.
│                       # conn_socket_create(pool, addrinfo, **fd): create
│                       #   socket with O_NONBLOCK and O_CLOEXEC, configure
│                       #   keepalive, initiate non-blocking connect().
│                       # See ARCHITECTURE.md §4, §5.
│
├── braid_conn.h        # Connection record internal interface.
│                       # conn_transition(), conn_alloc(), conn_socket_create(),
│                       #   conn_keepalive_configure() declarations.
│                       # Legal transition table (array of braid_state_t pairs).
│
│   — Reconnection Engine —
│
├── braid_reconnect.c   # Reconnection heap and backoff algorithm.
│                       # reconnect_heap_init(heap, cap): allocate cap slots.
│                       # reconnect_heap_destroy(heap): free slot array.
│                       # reconnect_heap_push(heap, entry): O(log n) insert
│                       #   with sift-up on next_retry_ms.
│                       # reconnect_heap_peek(heap): O(1) minimum.
│                       # reconnect_heap_pop(heap): O(log n) delete-min
│                       #   with sift-down.
│                       # reconnect_heap_clear(heap): O(1) reset count to 0.
│                       # reconnect_backoff_delay(pool, attempt): compute
│                       #   full jitter sleep = random(0, min(cap, base * 2^n))
│                       #   using pool->prng (per-pool PRNG state). Exponent
│                       #   capped at 31. See ARCHITECTURE.md §6.2.
│                       # reconnect_attempt(pool, entry): resolve DNS via
│                       #   getaddrinfo(), call conn_socket_create(), register
│                       #   fd in epoll, conn_transition(CONNECTING). Inserts
│                       #   reconnect entry for attempt+1 only on failure (not
│                       #   pre-inserted at attempt start). Handles connect()==0
│                       #   fast-success path. See ARCHITECTURE.md §6.3.
│                       # reconnect_advance(pool, now_ms): pop all entries
│                       #   with next_retry_ms <= now_ms, call
│                       #   reconnect_attempt() for each.
│                       # See ARCHITECTURE.md §6.
│
├── braid_reconnect.h   # Reconnection engine internal interface.
│                       # reconnect_heap_init(), reconnect_heap_destroy(),
│                       #   reconnect_heap_push(), reconnect_heap_peek(),
│                       #   reconnect_heap_pop(), reconnect_heap_clear(),
│                       #   reconnect_advance() declarations.
│
│   — Idle Reaper —
│
├── braid_reaper.c      # Idle reaper heap and reap logic.
│                       # reaper_heap_init(heap, cap): allocate cap slots.
│                       #   No parallel index array — heap position is stored
│                       #   directly in conn->heap_index on the connection
│                       #   record, eliminating fd-modulo collision risk.
│                       # reaper_heap_destroy(heap): free slot array.
│                       # reaper_heap_insert(heap, conn): O(log n) insert with
│                       #   sift-up; writes assigned position to conn->heap_index
│                       #   at every swap.
│                       # reaper_heap_remove(heap, conn): O(log n) remove by
│                       #   position from conn->heap_index; swap with last entry;
│                       #   attempt sift-up then sift-down on moved element —
│                       #   both directions required for correctness; updates
│                       #   heap_index on all swapped records; sets
│                       #   conn->heap_index = UINT32_MAX on removed record.
│                       # reaper_heap_peek(heap): O(1) minimum last_active_ms.
│                       # reaper_advance(pool, now_ms): pop all entries with
│                       #   last_active_ms + idle_reap_timeout <= now_ms,
│                       #   subject to min_connections floor; call
│                       #   conn_transition(CLOSING) for each eligible entry.
│                       # Called from conn_transition() on IDLE entry and exit
│                       #   to maintain heap membership. See ARCHITECTURE.md §7.
│
├── braid_reaper.h      # Idle reaper internal interface.
│                       # reaper_heap_init(), reaper_heap_destroy(),
│                       #   reaper_heap_insert(heap, conn),
│                       #   reaper_heap_remove(heap, conn),
│                       #   reaper_heap_peek(), reaper_advance() declarations.
│
│   — Wait Queue —
│
├── braid_waitq.c       # Wait queue ring buffer: enqueue, dequeue, cancel,
│                       #   expiry scan.
│                       # waitq_init(ring, cap): allocate cap braid_waiter_t
│                       #   slots; initialise head, tail, count.
│                       # waitq_destroy(ring): free slot array.
│                       # waitq_enqueue(ring, cb, cb_ctx, deadline_ms,
│                       #   *token): write to tail slot; advance tail;
│                       #   write ring index to *token. O(1).
│                       # waitq_serve_head(ring, fd, conn_ctx): dequeue from
│                       #   head, skipping tombstones; invoke callback with
│                       #   fd, conn_ctx, BRAID_OK, cb_ctx; tombstone served
│                       #   slot. O(1) amortised.
│                       # waitq_cancel(ring, token): locate slot at token %
│                       #   cap; verify slot.token == token before acting
│                       #   (stale wrapped tokens must not cancel new waiters);
│                       #   invoke callback with BRAID_ERR_CANCELLED; tombstone.
│                       # waitq_expire(ring, now_ms): scan from head,
│                       #   invoking callbacks with BRAID_ERR_TIMEOUT for
│                       #   expired non-tombstone entries; stops at first
│                       #   non-expired non-tombstone entry.
│                       # waitq_shutdown(ring): invoke all non-tombstone
│                       #   entries with BRAID_ERR_SHUTDOWN; tombstone all.
│                       # See ARCHITECTURE.md §10.
│
├── braid_waitq.h       # Wait queue internal interface.
│                       # waitq_init(), waitq_destroy(), waitq_enqueue(),
│                       #   waitq_serve_head(), waitq_cancel(),
│                       #   waitq_expire(), waitq_shutdown() declarations.
│
│   — I/O Abstraction Layer —
│
├── braid_io.h          # Platform-independent I/O abstraction interface.
│                       # io_watch(pool, fd, events): register fd in the
│                       #   caller's epoll/kqueue instance.
│                       # io_modify(pool, fd, events): modify watched events.
│                       # io_unwatch(pool, fd): remove fd from epoll/kqueue.
│                       # BRAID_IO_READ and BRAID_IO_WRITE flag constants.
│                       # Implemented by braid_io_epoll.c or braid_io_kqueue.c.
│                       # See ARCHITECTURE.md §8.1.
│
├── braid_io_epoll.c    # Linux epoll implementation of braid_io.h.
│                       # io_watch(): epoll_ctl(EPOLL_CTL_ADD) with EPOLLET;
│                       #   sets epoll_data.ptr to &conn->tag (inline struct).
│                       # io_modify(): epoll_ctl(EPOLL_CTL_MOD).
│                       # io_unwatch(): epoll_ctl(EPOLL_CTL_DEL).
│                       # Compiled on Linux only. See ARCHITECTURE.md §8.1,
│                       #   §15.2.
│
├── braid_io_kqueue.c   # OpenBSD/FreeBSD/NetBSD kqueue implementation of
│                       #   braid_io.h. Not compiled in v1.
│                       # io_watch(): kevent(EVFILT_READ or EVFILT_WRITE,
│                       #   EV_ADD); sets udata to conn->tag.
│                       # io_modify(): delete old filter, add new filter.
│                       # io_unwatch(): kevent(EV_DELETE) for all active
│                       #   filters on fd.
│                       # Compiled on OpenBSD, FreeBSD, NetBSD only.
│                       #   See ARCHITECTURE.md §8.1, §15.3.
│
│   — Pool Core —
│
├── braid_pool.c        # Pool lifecycle and public API implementation.
│                       # braid_pool_create(): allocate and initialise all
│                       #   internal structures in order; validate config;
│                       #   deep-copy config.host via strdup(); seed per-pool
│                       #   PRNG (pool->prng); insert min_connections reconnect
│                       #   entries with next_retry_ms = 0 for immediate warm-up.
│                       # braid_pool_destroy(): mark shutdown (suppresses
│                       #   reconnect insertion); cancel wait queue; drain
│                       #   active connections; reap CONNECTING, INITIALIZING,
│                       #   IDLE connections; clear reconnect heap; io_unwatch
│                       #   all fds; free all structures.
│                       # braid_pool_checkout(): check for available IDLE
│                       #   connection; if found, run validate_fn if idle
│                       #   threshold exceeded, invoke callback immediately;
│                       #   else enqueue in wait queue. Non-blocking.
│                       # braid_pool_checkin(): table_lookup by fd; validate
│                       #   ACTIVE state; if BRAID_CONN_OK, conn_transition
│                       #   to IDLE, serve wait queue head if present; if
│                       #   BRAID_CONN_DISCARD, conn_transition to CLOSING.
│                       #   Respects in_callback deferred work protocol.
│                       # braid_pool_cancel(): delegate to waitq_cancel()
│                       #   with in_callback protocol.
│                       # braid_pool_advance(): capture now_ms; drive
│                       #   reconnect_advance(), enforce connect_timeout on
│                       #   CONNECTING sockets, reaper_advance(),
│                       #   waitq_expire(); drain deferred work; compute
│                       #   next_ms as minimum of all next-event times.
│                       #   See ARCHITECTURE.md §11.
│                       # braid_pool_notify(): table_lookup by fd; dispatch
│                       #   by connection state: CONNECTING writability,
│                       #   IDLE readability (1-byte MSG_PEEK half-open
│                       #   detection), ACTIVE (ignored), CLOSING/DEAD (ignored).
│                       #   See ARCHITECTURE.md §12.
│                       # pool_drain_deferred(): process deferred_work flags
│                       #   in order: PROCESS_DEAD then SERVE_WAITQUEUE.
│                       #   Called after every in_callback decrement to zero.
│                       #   Internal function — no braid_ prefix.
│                       #   See ARCHITECTURE.md §9.
│
└── braid_pool.h        # Pool core internal interface.
                        # pool_drain_deferred() declaration.
                        # Internal pool state flags (shutdown, etc).
```

---

## 4. tests/ — Test Files

```
tests/
│
├── test_harness.h          # Minimal test harness: CHECK() and CHECK_ERR()
│                           #   macros; pass/fail counters; exit code.
│
├── run_tests.c             # Test binary entry point. Calls all test suite
│                           #   functions in order; prints summary; exits 0
│                           #   on all pass, 1 on any failure.
│
├── test_table.c            # Connection table tests.
│                           # Hash function distribution, insert and lookup,
│                           #   linear probe chain correctness, tombstone
│                           #   handling, delete and re-insert, compaction
│                           #   trigger, table-full behaviour.
│
├── test_state_machine.c    # State machine tests.
│                           # All legal transitions accepted, all illegal
│                           #   transitions rejected, conn_transition()
│                           #   state-entry invariants (last_active_ms,
│                           #   heap membership), CLOSING deferred flag
│                           #   behaviour.
│
├── test_wait_queue.c       # Wait queue tests.
│                           # Enqueue and dequeue FIFO ordering, tombstone
│                           #   skip on dequeue, cancel by token,
│                           #   timeout expiry scan, shutdown drain,
│                           #   one-callback-per-checkout guarantee,
│                           #   ring wrap-around correctness.
│
├── test_reconnect.c        # Reconnection engine tests.
│                           # Heap insert/pop/peek ordering, backoff delay
│                           #   values across attempt range, overflow guard
│                           #   at attempt >= 31, max_attempts enforcement,
│                           #   reconnect_advance() firing order.
│
├── test_reaper.c           # Idle reaper tests.
│                           # Heap insert, remove by fd, peek minimum,
│                           #   reap_advance() threshold enforcement,
│                           #   min_connections floor respected,
│                           #   index array consistency across sift operations.
│
├── test_pool.c             # Full pool lifecycle tests.
│                           # Pool create and destroy, warm-up to
│                           #   min_connections, checkout with immediate
│                           #   connection, checkout with wait queue,
│                           #   checkin with BRAID_CONN_OK and
│                           #   BRAID_CONN_DISCARD, cancel pending checkout,
│                           #   exhaustion with timeout_ms = 0,
│                           #   checkin from within checkout callback
│                           #   (re-entrancy), shutdown cancels waiters.
│
└── test_integration.c      # Integration tests with real TCP sockets.
                            # Loopback TCP server (forked child process).
                            # Full connect → checkout → checkin → reuse cycle.
                            # Half-open detection: server closes connection,
                            #   pool detects via keepalive or notify error.
                            # Reconnection: server restarts, pool reconnects
                            #   with backoff, waiter receives connection.
                            # validate_fn called when idle_threshold exceeded.
                            # destroy_fn called on BRAID_CONN_DISCARD.
                            # observe_fn events fired in correct sequence.
```

See TESTING.md for the full test catalogue and rationale.

---

## 5. bench/ — Performance Benchmarks

Built separately under release flags. Not run as part of `make test`.

```
bench/
│
├── bench_checkout.c        # Checkout/checkin round-trip latency.
│                           # Measures: time from checkout call to callback
│                           #   invocation with an immediately available
│                           #   connection. Baseline pool operation cost.
│
├── bench_advance.c         # braid_pool_advance() overhead.
│                           # Measures: advance() call duration at varying
│                           #   pool sizes (empty queue, full idle pool,
│                           #   mixed states). Establishes event loop tax.
│
├── bench_reconnect.c       # Reconnection engine throughput.
│                           # Measures: reconnect entries processed per
│                           #   second; heap push/pop throughput at
│                           #   max_connections scale.
│
└── bench_pool_scale.c      # Pool behaviour under varying max_connections.
                            # Measures: checkout/checkin throughput at
                            #   10, 100, 500, 1000 connections.
                            # Validates O(1) lookup assumption holds
                            #   empirically across the range.
```

Benchmark output includes hardware context (CPU model, core count, clock
speed). Numbers are not comparable across machines without this context.

---

## 6. Top-Level Files

### Makefile

Single top-level Makefile. No per-directory Makefiles. Platform detected
at build time.

```makefile
# Key targets
make              # same as make dev
make dev          # debug build with ASan/UBSan and BRAID_DEBUG=1
make release      # optimised build
make test         # build and run tests/run_tests (dev flags)
make test-tsan    # build and run tests with ThreadSanitizer (Clang only)
make valgrind     # run tests under Valgrind (Linux only)
make bench        # build and run all benchmarks (release flags)
make lint         # clang-tidy + cppcheck on src/
make format       # clang-format on src/*.c src/*.h include/braid.h
make clean        # remove build artefacts
make install      # install libbraid.a and include/braid.h to PREFIX
```

Platform detected via `$(shell uname)`. The correct I/O abstraction
translation unit (`braid_io_epoll.c` or `braid_io_kqueue.c`) is selected
automatically. No manual platform flag is required.

`make dev` defines `-DBRAID_DEBUG=1` which enables:
- Transition legality assertions in `conn_transition()` with diagnostic
  messages identifying source state, target state, and fd
- Tombstone density warnings on the connection table
- Wait queue ring buffer bounds assertions
- O_NONBLOCK and O_CLOEXEC verification at socket creation

### .clang-format

KNF-based formatting rules:

```yaml
BasedOnStyle: LLVM
IndentWidth: 8
UseTab: ForIndentation
BreakBeforeBraces: Linux     # functions: own line; control: same line
ColumnLimit: 80
AllowShortFunctionsOnASingleLine: None
AllowShortIfStatementsOnASingleLine: Never
```

### .clang-tidy

Static analysis checks:

```yaml
Checks: >
  clang-analyzer-*,
  cert-*,
  bugprone-*,
  performance-*,
  portability-*,
  -cert-err33-c,
  -bugprone-easily-swappable-parameters
```

---

## 7. Component-to-File Mapping

| Component | Description | Primary files |
|---|---|---|
| Connection table | Hash table keyed on fd | `src/braid_table.c` |
| State machine | conn_transition, record lifecycle | `src/braid_conn.c` |
| Reconnection engine | Min-heap, backoff, DNS, connect | `src/braid_reconnect.c` |
| Idle reaper | Min-heap on last_active_ms | `src/braid_reaper.c` |
| Wait queue | Ring buffer, cancel, expiry | `src/braid_waitq.c` |
| I/O abstraction — Linux | epoll translation unit | `src/braid_io_epoll.c` |
| I/O abstraction — kqueue | kqueue translation unit (v2) | `src/braid_io_kqueue.c` |
| Pool core | Public API, advance, notify | `src/braid_pool.c` |
| Internal definitions | Shared types, assertions | `src/braid_internal.h` |
| Public API | Embedder header | `include/braid.h` |

---

## 8. Naming Conventions Across Files

Function names are prefixed by their module. This makes `grep` and code
navigation unambiguous across the codebase.

| Module | File | Internal prefix | Example |
|---|---|---|---|
| Connection table | `src/braid_table.c` | `table_` | `table_lookup()` |
| State machine | `src/braid_conn.c` | `conn_` | `conn_transition()` |
| Reconnection | `src/braid_reconnect.c` | `reconnect_` | `reconnect_heap_push()` |
| Idle reaper | `src/braid_reaper.c` | `reaper_` | `reaper_advance()` |
| Wait queue | `src/braid_waitq.c` | `waitq_` | `waitq_enqueue()` |
| I/O abstraction | `src/braid_io_epoll.c` | `io_` | `io_watch()` |
| Pool core | `src/braid_pool.c` | `pool_` | `pool_drain_deferred()` |

Public API functions (declared in `include/braid.h`) use the `braid_`
prefix throughout. No internal function name begins with `braid_` — that
prefix is reserved exclusively for the public API.

| Public namespace | Functions |
|---|---|
| `braid_pool_` | `braid_pool_create`, `braid_pool_destroy`, `braid_pool_checkout`, `braid_pool_checkin`, `braid_pool_cancel`, `braid_pool_advance`, `braid_pool_notify` |

---

**Document Version**: 1.0
**Last Updated**: 2026-03-30
**See Also**: PROJECT.md, ARCHITECTURE.md, TECH_STACK.md, CODING_STANDARDS.md,
DEVELOPMENT.md, TESTING.md
