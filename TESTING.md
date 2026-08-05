# Testing Strategy

## 1. Overview

Testing operates at three levels:

| Level | What | When | Tools |
|---|---|---|---|
| Unit | Individual components in isolation | During each phase, before moving on | Plain C test programs, Valgrind, ASan/UBSan |
| Integration | Full pool behaviour with real TCP sockets | After all unit tests pass | Same test binary, integration test suite |
| Performance | Latency and throughput baselines | Under release flags | Benchmark suite in `bench/` |

The rule is simple: **a phase is not done until its tests pass cleanly on
Linux with Valgrind and ASan/UBSan**. Do not accumulate untested code. Each
phase is small enough that bugs are easy to find when caught immediately.

**No external test framework.** Plain C programs with a minimal assertion
macro. The harness is described in TECH_STACK.md §6. Every test function
returns 0 on success and non-zero on failure. The test binary exits with
code 0 if all tests pass and 1 if any fail.

---

## 2. Unit Testing

### 2.1 Test Harness

See TECH_STACK.md §6.1 for the `CHECK()` macro definition and the test
binary contract. The structure is:

```
tests/
├── test_harness.h          ← CHECK macro, pass/fail counters
├── run_tests.c             ← binary entry point — calls all test suites
├── test_table.c            ← connection hash table
├── test_state_machine.c    ← conn_transition and state machine
├── test_wait_queue.c       ← wait queue ring buffer
├── test_reconnect.c        ← reconnection heap and backoff
├── test_reaper.c           ← idle reaper heap
├── test_pool.c             ← full pool lifecycle
└── test_integration.c      ← integration tests with real TCP sockets
```

`make test` compiles the library source files and all test files into one test
binary. `make release` separately builds `libbraid.a`, the artefact consumed
by embedders.

### 2.2 Per-Component Test Scope

Every component has a dedicated test file. The scope of each file is
strictly bounded — a wait queue test does not test reconnection, and a
reconnection test does not test the idle reaper. This makes failures easy
to localise.

| Test file | What it tests | What it does NOT test |
|---|---|---|
| `test_table.c` | Hash table correctness, probe chains, tombstones, compaction | State machine, pool behaviour |
| `test_state_machine.c` | Transition legality, state-entry invariants, deferred close | Hash table, reconnection |
| `test_wait_queue.c` | Ring buffer, FIFO order, cancel, timeout expiry, shutdown | Pool exhaustion logic |
| `test_reconnect.c` | Heap ordering, backoff values, overflow guard, max_attempts | Real TCP connects |
| `test_reaper.c` | Heap insert/remove, heap_index on conn record, reap threshold, floor | Pool checkout |
| `test_pool.c` | Full pool lifecycle, checkout/checkin, re-entrancy, exhaustion | Real network I/O |
| `test_integration.c` | End-to-end with real loopback TCP connections | None — cross-component |

### 2.3 Mock Clock

Timer-based behaviour — reconnection backoff deadlines, idle reap thresholds,
checkout wait timeouts — must be testable without sleeping. libbraid uses a
mock clock in test builds:

```c
/*
 * BRAID_TEST_CLOCK: when defined, braid_now_ms() reads a global
 * braid_test_clock_ms instead of calling clock_gettime(). Tests advance
 * time by writing to braid_test_clock_ms directly.
 * No test may use sleep() or usleep() for timing.
 */
#ifdef BRAID_TEST_CLOCK
extern uint64_t braid_test_clock_ms;   /* writable by tests */
#endif
```

`BRAID_TEST_CLOCK` is defined by `make test` and `make dev`. It is never
defined in release builds. All timer-dependent tests advance time via
`braid_test_clock_ms` rather than sleeping. This keeps the test suite fast
and deterministic.

```c
/* Example: test that checkout timeout fires correctly */
static int
test_checkout_timeout(void)
{
        braid_pool_t    *pool;
        braid_token_t    token;
        int              callback_fired = 0;
        int              callback_err   = 0;

        /* ... pool create with max_connections = 0 to force wait ... */

        braid_test_clock_ms = 1000;
        braid_pool_checkout(pool, 500 /* ms */, on_checkout,
            &(test_ctx){&callback_fired, &callback_err}, &token);

        /* advance time past the deadline */
        braid_test_clock_ms = 1501;
        braid_pool_advance(pool, &next_ms);

        CHECK("timeout callback fires", callback_fired == 1);
        CHECK("timeout error code correct",
            callback_err == BRAID_ERR_TIMEOUT);

        /* ... cleanup ... */
        return 0;
}
```

### 2.4 Loopback TCP Test Patterns

Integration tests and pool-level unit tests that need real fds use a
loopback TCP server. The server is a forked child process that listens on
a loopback port, accepts connections, and optionally echoes or closes them
on demand.

```c
/*
 * Spawn a loopback test server on an ephemeral port.
 * Returns the port the server is listening on.
 * Server is a forked child; caller must waitpid on cleanup.
 */
static uint16_t
start_test_server(pid_t *server_pid)
{
        /* ... fork, bind to port 0, getsockname for actual port,
           signal parent, accept loop ... */
}
```

For unit tests that do not need a real server (testing checkout/checkin
mechanics, re-entrancy, error code paths), a pipe pair is used instead —
the write end is passed as a fake fd to the pool via internal test seams.
This avoids network dependency for pure logic tests.

### 2.5 epoll Instance Setup in Tests

Tests that exercise `braid_pool_advance()` and `braid_pool_notify()` must
provide a real epoll fd. The test creates its own epoll instance and
event loop for the duration of the test:

```c
static void
test_event_loop_step(int epfd, braid_pool_t *pool)
{
        struct epoll_event  events[32];
        uint32_t            next_ms;
        int                 n, i;

        braid_pool_advance(pool, &next_ms);

        n = epoll_wait(epfd, events, 32,
            next_ms == UINT32_MAX ? 0 : (int)next_ms);

        for (i = 0; i < n; i++) {
                braid_fd_tag_t *tag = events[i].data.ptr;
                if (tag->magic == BRAID_FD_MAGIC)
                        braid_pool_notify(pool, tag->fd,
                            events[i].events);
        }
}
```

Tests drive the pool by calling `test_event_loop_step()` in a loop until
the expected condition is met or a tick limit is reached. The tick limit
prevents infinite loops in failing tests.

---

## 3. Component Test Catalogue

### 3.1 Connection Table (`test_table.c`)

| Test case | What is verified |
|---|---|
| Insert and lookup by fd | fd maps to correct record |
| Probe chain — collision | Records with same `fd % table_size` all retrievable |
| Tombstone skip on lookup | Lookup traverses tombstones and finds record |
| Delete vacates slot | Deleted fd not found after delete |
| Tombstone reuse on insert | New insert takes tombstone slot; inline tag address unchanged |
| Table full returns error | Insert into full table (all slots occupied) returns error |
| fd = 0 handled correctly | fd 0 is a valid key — not confused with empty slot |
| Tag address stable after delete+insert | &conn->tag unchanged for reused slot — no epoll re-registration needed |

### 3.2 State Machine (`test_state_machine.c`)

| Test case | What is verified |
|---|---|
| All legal transitions accepted | Every transition in ARCHITECTURE.md §4.2 returns BRAID_OK |
| All illegal transitions rejected | Every unlisted transition returns error in debug build |
| IDLE entry sets last_active_ms | Timestamp written on IDLE entry |
| IDLE entry inserts into reaper heap | Reaper heap count increments |
| IDLE exit removes from reaper heap | Reaper heap count decrements |
| CONNECTING entry sets created_at_ms | Timestamp written on CONNECTING entry |
| CLOSING entry calls destroy_fn | destroy_fn invoked exactly once |
| CLOSING → DEAD closes fd | fd is closed after destroy_fn returns |
| CLOSING deferred when in_callback > 0 | destroy_fn runs on CLOSING entry; only CLOSING → DEAD is deferred |
| Deferred close fires after in_callback reaches 0 | Deferred CLOSING → DEAD transition closes and removes the connection |
| DEAD entry vacates table slot | Table lookup by fd returns not-found after DEAD |
| DEAD entry fires BRAID_EV_CONN_DESTROYED | observe_fn invoked with BRAID_EV_CONN_DESTROYED |

### 3.3 Wait Queue (`test_wait_queue.c`)

| Test case | What is verified |
|---|---|
| Enqueue and dequeue FIFO order | First enqueued is first served |
| Tombstone skip on dequeue | Tombstoned head slots are skipped |
| Cancel by token | Callback invoked with BRAID_ERR_CANCELLED; slot tombstoned |
| Cancel already-served token is no-op | No double callback |
| Timeout expiry scan | Expired entries invoke callback with BRAID_ERR_TIMEOUT |
| Expiry stops at first non-expired entry | Non-expired entries not affected |
| Shutdown drains all entries | All pending callbacks invoked with BRAID_ERR_SHUTDOWN |
| One callback per checkout — cancel after serve | No double invocation |
| One callback per checkout — timeout after cancel | No double invocation |
| Ring wrap-around | Enqueue/dequeue across ring boundary works correctly |
| Full ring rejects enqueue | Enqueue on full ring returns error |
| Cancelled tail preserves live head | A tombstone never overwrites an older live waiter |
| BRAID_TOKEN_NONE cancellation | Immediate-checkout token cancellation is a safe no-op |

### 3.4 Reconnection Engine (`test_reconnect.c`)

| Test case | What is verified |
|---|---|
| Heap push/pop ordering | Entries popped in ascending next_retry_ms order |
| Heap peek returns minimum | Peek matches first pop |
| Backoff delay — attempt 0 | random(0, backoff_base) |
| Backoff delay — attempt 5 | random(0, min(cap, base * 32)) |
| Backoff delay — attempt 31 | Exponent capped at 31, no overflow |
| Backoff delay — attempt 64 | Same as attempt 31 — cap enforced |
| backoff_cap respected | delay never exceeds backoff_cap |
| max_attempts = 0 retries forever | No entry dropped after any number of attempts |
| max_attempts = N stops at N | Entry not re-inserted after N attempts |
| reconnect_advance fires due entries | Entries with next_retry_ms <= now_ms are popped and processed |
| reconnect_advance skips future entries | Entries with next_retry_ms > now_ms remain |
| BRAID_EV_RECONNECT_ATTEMPT fired | observe_fn invoked with correct attempt number |
| Pending retry survives asynchronous failure | CONNECTING failure schedules attempt + 1 |
| Shutdown discards pending retry entries | No reconnect work begins after destroy starts |

### 3.5 Idle Reaper (`test_reaper.c`)

| Test case | What is verified |
|---|---|
| Heap insert and peek | Inserted entry is minimum if earliest last_active_ms |
| Heap remove by conn pointer | Entry removed; heap still valid; conn->heap_index set to UINT32_MAX |
| heap_index consistency after sift-up | conn->heap_index correct on every record after insert |
| heap_index consistency after sift-down | conn->heap_index correct on every record after remove |
| reaper_advance reaps eligible connections | Connections past idle_reap_timeout transition to CLOSING |
| reaper_advance respects min_connections floor | No reap when live count would drop below min |
| reaper_advance stops when heap minimum is future | Future entries not touched |
| next_ms computed correctly | Returns exact ms until next reap event |
| Clock before last-active timestamp | Connection is not reaped by unsigned-underflow |

### 3.6 Pool Lifecycle (`test_pool.c`)

| Test case | What is verified |
|---|---|
| Pool create with valid config | Returns non-NULL pool |
| Pool create with NULL event_fd | Returns NULL, err = BRAID_ERR_INVAL |
| Pool create with min > max | Returns NULL, err = BRAID_ERR_INVAL |
| Pool create with NULL host | Returns NULL, err = BRAID_ERR_INVAL |
| Pool create allocates all internal structures | Create cleanup paths are inspected; allocation-failure injection is not implemented |
| Pool destroy with no active connections | Clean teardown, no leaks |
| Pool destroy with active connection | Active connection closed after timeout |
| Checkout with immediately available connection | Callback invoked before checkout returns |
| Checkout enqueues when no connection available | Token written, callback not yet invoked |
| Queued demand grows capacity | Reconnect entries are added up to max_connections |
| Reconnect arrival serves waiter | Oldest queued checkout receives a new IDLE connection |
| Finite reconnect exhaustion | Pending checkouts receive BRAID_ERR_CONNFAIL |
| Checkin with BRAID_CONN_OK | Connection transitions to IDLE, wait queue served |
| Checkin with BRAID_CONN_DISCARD | Connection transitions to CLOSING → DEAD |
| Checkin unrecognised fd | Returns BRAID_ERR_INVAL |
| Cancel pending checkout | Callback invoked with BRAID_ERR_CANCELLED |
| Cancel already-fired token | No-op, no double callback |
| Exhaustion with timeout_ms = 0 | Returns BRAID_ERR_EXHAUSTED, callback not invoked |
| Checkin from within checkout callback | Re-entrancy: deferred work fires correctly after return |
| Discard from checkout callback | Deferred CLOSING → DEAD work completes after callback return |
| Timeout callback checkin | Re-entrant checkin cannot bypass timeout delivery |
| Shutdown cancels all pending waiters | All pending callbacks invoked with BRAID_ERR_SHUTDOWN |
| observe_fn NULL — no crash | Pool functions correctly with no observability hook |
| validate_fn called when idle_threshold exceeded | validate_fn invoked at checkout when threshold passed |
| validate_fn failure discards connection | Connection transitions to CLOSING → DEAD |
| init_fn deadline exceeded | Connection transitions to DEAD |
| Absent event registration unwatch | io_unwatch() succeeds on epoll and kqueue |

---

## 4. Integration Test Catalogue (`test_integration.c`)

Integration tests use a real loopback TCP server. The pool is fully
configured with a real epoll fd and driven by `test_event_loop_step()`.

| Test case | What is verified |
|---|---|
| Full connect → checkout → checkin → reuse | End-to-end lifecycle; fd reused on second checkout |
| Warm pool reaches min_connections | Pool creates min_connections connections without any checkout calls |
| Half-open: server closes connection while IDLE | Pool detects via MSG_PEEK; transitions to DEAD; reconnects |
| Half-open: write error while ACTIVE | Pool transitions ACTIVE → DEAD on caller checkin with BRAID_CONN_DISCARD |
| Reconnection after server restart | Server killed and restarted; pool reconnects with backoff; waiter receives connection |
| Backoff prevents connection storm | Multiple pools reconnecting simultaneously: no simultaneous connect burst |
| validate_fn PING/PONG over real socket | validate_fn sends probe, receives response; connection reused |
| validate_fn timeout exceeded | validate_fn hangs (server does not respond); connection discarded |
| init_fn TLS-style handshake simulation | init_fn sends and receives handshake bytes; conn_ctx written |
| destroy_fn graceful teardown | destroy_fn sends FIN; server receives clean close |
| destroy_fn with unknown protocol state | destroy_fn called on BRAID_CONN_DISCARD; must not crash |
| observe_fn event sequence | CONN_CREATED, CONN_DESTROYED, RECONNECT_ATTEMPT in correct order |
| BRAID_EV_POOL_EXHAUSTED fires | Pool at max_connections; additional checkout fires event |
| BRAID_EV_CHECKOUT_TIMEOUT fires | Waiter times out; event fires before callback |
| Pool with max_connections = 1, concurrent checkouts | Second checkout queued and served after checkin |
| Pool destroy during pending reconnection | Reconnection heap cleared; no connect attempt after destroy |

---

## 5. Sanitizer Testing

### 5.1 AddressSanitizer + UndefinedBehaviorSanitizer

Run on every commit on Linux and before every OpenBSD test pass.

```sh
make dev && make test
```

libbraid has no context switching or custom stack management — no ASan
integration hooks are required. ASan runs without modification. All tests
must pass ASan/UBSan clean.

Critical cases ASan is expected to catch if regressions occur:
- Use-after-free on a connection record after DEAD transition
- Use-after-free on a wait queue slot after tombstone
- Out-of-bounds access on hash table probe (table_size computation error)
- Access to `conn_ctx` after `destroy_fn` has freed it

### 5.2 Valgrind

Run on Linux before every commit. Catches leak and uninitialised-read classes
that ASan may not.

```sh
make valgrind
```

Expected clean paths:
- `braid_pool_create()` allocates all memory; `braid_pool_destroy()` frees
  all of it. No leaks under any test case including forced teardown.
- `strdup(config.host)` in create — freed in destroy.

### 5.3 ThreadSanitizer

Run before release. Validates no accidental shared state across pool
instances in multi-pool integration tests.

```sh
make test-tsan
```

No TSan fiber hooks are required. The primary value is confirming that
pools used from separate threads (one pool per thread, as designed) do not
share mutable global state.

---

## 6. Platform Testing

| Platform | Arch | I/O | Status |
|---|---|---|---|
| Linux | x86_64 | epoll | Done — primary development platform |
| Linux | ARM64 | epoll | Done — tested via CI |
| OpenBSD | x86_64 | kqueue | Done — all tests pass |
| OpenBSD | ARM64 | kqueue | Done — all tests pass |

**All four platform/arch combinations are part of the v1 scope and are tested.**
FreeBSD and NetBSD share the kqueue translation unit — spot-check on FreeBSD
before release is recommended but not gated.

All tests must pass on all platforms before any phase is declared complete.
Platform-specific setup and assertions are permitted only where epoll and
kqueue APIs differ. Every platform must exercise the same library-level
scenario and semantic expectation.

---

## 7. Performance Benchmarks

### 7.1 When to Run

Benchmarks are not part of `make test`. Run them manually under release
flags when a phase is complete and again before release.

```sh
make bench
```

### 7.2 Output Format

Each benchmark reports hardware context alongside results:

```
libbraid benchmark — checkout_latency
Platform : Linux x86_64
CPU      : Intel Core i9-13900K @ 5.8 GHz
Cores    : 24 (8P + 16E)
Build    : release (-O2)
Date     : 2026-03-30

checkout with immediate connection (pool size 10):   340 ns/op
checkout with immediate connection (pool size 100):  360 ns/op
checkout with immediate connection (pool size 1000): 390 ns/op
```

Results without hardware context must not be recorded as baselines.

### 7.3 Benchmark Targets (Indicative)

Design targets derived from the architecture. Not pass/fail gates. Actual
results on specific hardware will differ.

| Benchmark | Design target |
|---|---|
| Checkout with immediate connection | < 4 µs on modern x86_64 (includes two `epoll_ctl` syscalls per round-trip) |
| Checkin with wait queue serve | < 500 ns |
| `braid_pool_advance()` — idle pool | < 200 ns |
| `braid_pool_advance()` — 100 IDLE connections | < 1 µs |
| Full connect → IDLE (loopback) | < 500 µs |

The checkout target was revised from the original < 500 ns design estimate after
measuring on real hardware. The < 500 ns estimate assumed pure in-memory pool
mechanics; the actual path includes `io_unwatch()` (checkout) and `io_watch()`
(checkin), each issuing an `epoll_ctl` syscall. These syscalls are a direct
architectural cost of IDLE connection half-open detection and cannot be
eliminated without removing that feature. See `bench/README.md` for recorded
baselines and a full breakdown of the hot path.

The `advance()` targets assume Intel x86_64 (Skylake+). AMD Ryzen results
captured in the current baseline set are not directly comparable — the Ryzen
4800H performance governor does not hold boost frequency between independent
benchmark binary invocations, so each binary catches a different CPU frequency.
The Ryzen advance() number reflects the base clock (2900 MHz), not turbo. See
`bench/README.md` for details.

---

## 8. Test Coverage Tracking

Track coverage manually. Update after each phase. A cell is marked done
only when the test passes cleanly with no Valgrind or ASan errors.

### Unit Test Coverage

| Module | Test file | Written | Valgrind clean | ASan clean | OpenBSD |
|---|---|---|---|---|---|
| Connection table | test_table.c | ✓ | ✓ | ✓ | ✓ |
| State machine | test_state_machine.c | ✓ | ✓ | ✓ | ✓ |
| Wait queue | test_wait_queue.c | ✓ | ✓ | ✓ | ✓ |
| Reconnection engine | test_reconnect.c | ✓ | ✓ | ✓ | ✓ |
| Idle reaper | test_reaper.c | ✓ | ✓ | ✓ | ✓ |
| Pool lifecycle | test_pool.c | ✓ | ✓ | ✓ | ✓ |
| Integration tests | test_integration.c | ✓ | ✓ | ✓ | ✓ |

### Key Correctness Test Cases

| Test case | File | Reference |
|---|---|---|
| Tombstone does not terminate probe chain | test_table.c | ARCHITECTURE.md §3.3 |
| Tag address stable after tombstone+reuse — no compaction | test_table.c | ARCHITECTURE.md §3.3 |
| All legal transitions accepted | test_state_machine.c | ARCHITECTURE.md §4.2 |
| All illegal transitions rejected | test_state_machine.c | ARCHITECTURE.md §4.2 |
| IDLE entry updates last_active_ms and reaper heap | test_state_machine.c | ARCHITECTURE.md §4.3 |
| CLOSING deferred when in_callback > 0 | test_state_machine.c | ARCHITECTURE.md §9.2 |
| Deferred close fires after callback returns | test_state_machine.c | ARCHITECTURE.md §9.2 |
| One callback per checkout — all outcomes | test_wait_queue.c | ARCHITECTURE.md §10.3 |
| Stale wrapped token cancel is no-op | test_wait_queue.c | ARCHITECTURE.md §10.3 |
| Ring wrap-around correct | test_wait_queue.c | ARCHITECTURE.md §10.1 |
| Expiry stops at first non-expired entry | test_wait_queue.c | ARCHITECTURE.md §10.4 |
| Backoff exponent capped at 31 | test_reconnect.c | ARCHITECTURE.md §6.2 |
| backoff_cap respected at all attempt values | test_reconnect.c | ARCHITECTURE.md §6.2 |
| max_attempts = 0 retries forever | test_reconnect.c | ARCHITECTURE.md §6.2 |
| Per-pool PRNG — reconnect entries differ across pools | test_reconnect.c | ARCHITECTURE.md §6.2 |
| Reconnect entry inserted only on failure not on attempt start | test_reconnect.c | ARCHITECTURE.md §6.3 |
| connect()=0 fast path reaches IDLE without epoll writable event | test_reconnect.c | ARCHITECTURE.md §6.3 |
| Reaper respects min_connections floor | test_reaper.c | ARCHITECTURE.md §7.2 |
| Reaper heap_index consistent on conn after insert, remove, sift | test_reaper.c | ARCHITECTURE.md §7.1 |
| Reaper heap remove by conn pointer — UINT32_MAX set after remove | test_reaper.c | ARCHITECTURE.md §7.1 |
| Heap removal sift-up fires when moved element smaller than parent | test_reaper.c | ARCHITECTURE.md §7.1 |
| Checkout with immediate connection — no enqueue | test_pool.c | ARCHITECTURE.md §11 |
| connect_timeout aborts CONNECTING socket in advance() | test_pool.c | ARCHITECTURE.md §11 |
| Shutdown suppresses reconnect entry insertion | test_pool.c | ARCHITECTURE.md §13.2 |
| Checkin from within checkout callback | test_pool.c | ARCHITECTURE.md §9 |
| validate_fn called only above idle_threshold | test_pool.c | ARCHITECTURE.md §4.3 |
| braid_fd_tag_t magic distinguishes pool fds | test_pool.c | ARCHITECTURE.md §8.2 |
| Half-open detected via 1-byte MSG_PEEK on IDLE fd | test_integration.c | ARCHITECTURE.md §12 |
| Unexpected peer data on IDLE fd closes connection | test_integration.c | ARCHITECTURE.md §12 |
| Reconnection with backoff after server restart | test_integration.c | ARCHITECTURE.md §6.3 |
| Full event sequence fires in correct order | test_integration.c | ARCHITECTURE.md §6.3 |
| Pool destroy cleans up all fds | test_integration.c | ARCHITECTURE.md §13.2 |

### Release Verification Gates

| Milestone | Confirmed by |
|---|---|
| Hash table lookup correct under collision | test_table.c probe chain tests |
| State machine rejects all illegal transitions | test_state_machine.c illegal transition tests |
| One callback per checkout guaranteed | test_wait_queue.c one-callback tests |
| Backoff never overflows | test_reconnect.c overflow guard tests |
| Reaper floor respected | test_reaper.c min_connections floor tests |
| Re-entrancy: checkin from callback safe | test_pool.c re-entrancy test |
| Integration suite passes on Linux | test_integration.c all tests |
| Integration suite passes on OpenBSD | test_integration.c all tests |

---

## 9. Cross-Reference

| Topic | Reference |
|---|---|
| Test harness structure and CHECK macro | TECH_STACK.md §6 |
| Mock clock (BRAID_TEST_CLOCK) | TECH_STACK.md §4.3, this document §2.3 |
| Per-phase test tasks | DEVELOPMENT.md (each phase) |
| Hash table design and slot states | ARCHITECTURE.md §3 |
| Connection record fields | ARCHITECTURE.md §3.2 |
| State machine legal transitions | ARCHITECTURE.md §4.2 |
| conn_transition() responsibilities | ARCHITECTURE.md §4.3 |
| TCP keepalive defaults | ARCHITECTURE.md §5 |
| Reconnection engine and backoff | ARCHITECTURE.md §6 |
| Idle reaper heap and floor | ARCHITECTURE.md §7 |
| epoll abstraction and fd tagging | ARCHITECTURE.md §8 |
| Re-entrancy and deferred work | ARCHITECTURE.md §9 |
| Wait queue ring buffer | ARCHITECTURE.md §10 |
| braid_pool_advance() execution order | ARCHITECTURE.md §11 |
| braid_pool_notify() dispatch | ARCHITECTURE.md §12 |
| Pool create and destroy | ARCHITECTURE.md §13 |
| Error codes | ARCHITECTURE.md §18 |
| SAFETY comment convention | CODING_STANDARDS.md §7.3 |
| Source file purposes | REPOSITORY_STRUCTURE.md §3 |
| Integration test descriptions | REPOSITORY_STRUCTURE.md §4 |
| Benchmark descriptions | REPOSITORY_STRUCTURE.md §5 |

---

**Document Version**: 1.0
**Last Updated**: 2026-03-30
**See Also**: PROJECT.md, ARCHITECTURE.md, TECH_STACK.md, CODING_STANDARDS.md,
DEVELOPMENT.md, REPOSITORY_STRUCTURE.md
