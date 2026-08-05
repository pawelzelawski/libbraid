# Architecture

## 1. Library Overview

### 1.1 Design Model

libbraid is a transport resource pool, not a protocol library. It manages the
lifecycle of TCP file descriptors — creation, validation, health checking,
reconnection, and idle reaping — without any knowledge of what bytes flow over
the connections it manages. Protocol state is entirely the caller's concern.

The pool does not own a thread, a signal handler, or an event loop. The caller
drives libbraid explicitly by calling `braid_pool_advance()` from their own
event loop. libbraid registers its internal file descriptors into the caller's
epoll or kqueue instance; the caller routes events to libbraid via
`braid_pool_notify()`.

**Blocking behaviour:** `braid_pool_advance()` may block briefly on
`getaddrinfo()` during reconnection attempts (synchronous DNS resolution).
All other operations — `braid_pool_checkout()`, `braid_pool_checkin()`,
`braid_pool_cancel()`, and `braid_pool_notify()` — return immediately.
`braid_pool_destroy()` is a teardown function and may block during the
drain wait. Callers with strict latency requirements on `advance()` should
configure a local resolver with low timeouts.

The design target is one pool instance per worker. A process running N workers
creates N pools. There is no shared global state, no cross-pool coordination,
and no locking required in the common case.

### 1.2 What the Library Does Not Do

libbraid does not own a thread or event loop. It does not perform DNS
resolution outside of connection attempts. It does not implement TLS — TLS
negotiation is the caller's responsibility via `init_fn`. It does not
understand any application protocol. It does not implement flow control,
request multiplexing, or stream management. It does not support Unix domain
sockets, UDP, or any transport other than TCP. It does not implement
connection brokering across processes. It does not expose a synchronous
blocking API. It does not implement a pool reset in v1. It has no mandatory
third-party dependencies. It does not modify the calling process's signal
disposition. It does not install `atexit` handlers.

**SIGPIPE:** libbraid does not block or ignore `SIGPIPE`. When a caller
writes to a checked-out fd whose peer has closed the connection, the OS
delivers `SIGPIPE`, which terminates the process by default. The caller is
responsible for either blocking `SIGPIPE` globally (`signal(SIGPIPE, SIG_IGN)`
or `sigaction`) or using `MSG_NOSIGNAL` on every `send()` / `write()` call
over a checked-out fd. This is a documented caller requirement — most
production server processes already block `SIGPIPE` globally.

### 1.3 Design Influences and Rejected Alternatives

libbraid addresses a confirmed gap in the C ecosystem: no actively maintained,
generic, asynchronous TCP connection pool library exists that is decoupled from
a specific protocol or heavy framework. The design draws on production
experience with half-open connection failures, connection storm incidents, and
the operational behaviour of systems like Istio, libpq, and hiredis.

**Caller-owns-loop was chosen over pool-owns-thread.** A pool-owns-thread
design embeds a hidden thread inside the library, creating direct coordination
conflicts with a fiber scheduler such as libstrand and making the library
hostile to single-threaded event loop architectures. Caller-owns-loop composes
correctly with any C application that has its own event loop.

**Synchronous blocking API was rejected.** A blocking `checkout()` call that
returns an fd directly is simpler to use but incompatible with event-driven
callers. A non-blocking callback model with an explicit wait queue and
cancellation is the correct interface for the target use case.

**Pool-per-target was chosen over multi-target pools.** A single pool
instance manages one host and port. Callers needing connections to multiple
targets create multiple pools. This keeps the state machine and reconnection
engine simple and the API surface small. A multi-target pool adds index
management, per-target backoff state, and complex routing logic with no
architectural benefit at the library level.

**Fixed-size data structures were chosen over dynamic allocation.** All
internal structures — hash table, reconnection heap, idle reaper heap, wait
queue ring buffer — are allocated once at `braid_pool_create()` and sized to
`max_connections`. No allocation occurs on any subsequent operation. This
makes the pool's memory footprint deterministic and eliminates allocation
failure from the hot path.

### 1.4 API Philosophy

**Ownership is explicit.** When a caller checks out a connection, they own
the fd exclusively until checkin. libbraid makes no access to the fd while it
is checked out. The ownership boundary is a hard contract, not a convention.

**One callback per checkout, always.** Every `braid_pool_checkout()` call
results in exactly one invocation of the provided callback, regardless of
outcome — success, timeout, cancellation, or pool error. The caller never
needs to track whether a callback fired.

**Non-blocking at every operation callsite.** `braid_pool_checkout()`,
`braid_pool_checkin()`, `braid_pool_cancel()`, `braid_pool_advance()`, and
`braid_pool_notify()` all return immediately. Work that cannot complete
synchronously is deferred to `braid_pool_advance()`. `braid_pool_destroy()`
is explicitly a teardown function and may block — see §13.2.

**Hooks are optional.** Every hook is NULL-safe. A pool with no hooks
configured is fully functional — it manages fd lifecycle with no protocol
participation. There is no overhead for hooks that are not registered.

**Errors are returned, never fatal.** Internal errors are returned as error
codes. The library does not call `abort()` or `exit()` after successful
initialisation. Allocation failure at `braid_pool_create()` is the only
condition that causes the function to return NULL.

---

## 2. Internal Component Map

```
┌─────────────────────────────────────────────────────────────────┐
│  Public API                                                     │
│  braid_pool_create / braid_pool_destroy                         │
│  braid_pool_checkout / braid_pool_checkin / braid_pool_cancel   │
│  braid_pool_advance / braid_pool_notify                         │
├─────────────────────────────────────────────────────────────────┤
│  Checkout / Checkin Engine                                      │
│  Wait queue serving, validate_fn dispatch,                      │
│  re-entrancy deferred work flags                                │
├──────────────────────┬──────────────────────────────────────────┤
│  Reconnection Engine │  Idle Reaper                             │
│  Min-heap on         │  Min-heap on last_active_ms              │
│  next_retry_ms       │  Fires during braid_pool_advance()       │
│  Full jitter backoff │  Respects min_connections floor          │
├──────────────────────┴──────────────────────────────────────────┤
│  Connection State Machine                                       │
│  conn_transition() — single enforcement point                   │
│  CONNECTING / INITIALIZING / IDLE / ACTIVE / CLOSING / DEAD    │
├─────────────────────────────────────────────────────────────────┤
│  Connection Table                                               │
│  Hash table keyed on fd, open-addressed, linear probing         │
│  Fixed size: 2 × max_connections slots                          │
├─────────────────────────────────────────────────────────────────┤
│  epoll Abstraction Layer                                        │
│  Thin syscall wrapper — epoll (Linux), kqueue (BSD)             │
│  braid_fd_tag_t sentinel struct for caller event routing        │
└─────────────────────────────────────────────────────────────────┘
```

---

## 3. Connection Table

### 3.1 Structure

The connection table is an open-addressed hash table allocated once at pool
creation. It stores all connection records for the pool and is the primary
index for fd-to-record lookup.

**Size:** `2 × max_connections` slots. Load factor is at most 0.5 at full
pool capacity. Linear probing resolves collisions. At load factor ≤ 0.5,
expected probe length under uniform hashing is less than 1.5 — a constant
bounded by the configuration, not by runtime behaviour.

**Key:** The fd integer. Hash function: `fd % table_size`. fd values are
non-negative integers bounded by the process's `RLIMIT_NOFILE`. At the
pool's operating scale (at most `max_connections` fds held simultaneously),
collision frequency is low and probe chains are short.

**Slot states:** Each slot is one of three states — empty (never used),
occupied (holds a live connection record), or tombstone (held a record that
was removed). Tombstones are necessary so that linear probing chains are
not broken by deletions. Tombstones are reused on insertion.

**Allocation:** The table array is allocated as a contiguous block of
`braid_conn_t` structs at `braid_pool_create()`. No per-slot allocation.

### 3.2 Connection Record — `braid_conn_t`

Each occupied slot holds a connection record:

```c
typedef struct braid_conn {
        int              fd;             /* file descriptor; -1 = empty slot  */
        braid_state_t    state;          /* lifecycle state                    */
        void            *conn_ctx;       /* caller protocol state, opaque      */
        uint64_t         created_at_ms;  /* monotonic ms, set at CONNECTING    */
        uint64_t         last_active_ms; /* monotonic ms, updated at IDLE entry */
        uint32_t         flags;          /* internal bitfield, see §3.3        */
        uint32_t         heap_index;     /* position in idle reaper heap;
                                           UINT32_MAX when not in heap, see §7 */
        braid_fd_tag_t   tag;            /* epoll sentinel, inline — see §8.2  */
} braid_conn_t;
```

**`fd`:** Set when the connection enters CONNECTING. Never changes for the
lifetime of the record. Used as the hash table key and as the correlation
key passed to observe_fn. Set to -1 on slot vacating to mark empty.

**`state`:** Written only by `conn_transition()`. Direct writes outside
`conn_transition()` are prohibited throughout the codebase.

**`conn_ctx`:** NULL until `init_fn` writes it. All hooks after init_fn
receive it read-only. libbraid does not interpret or dereference it.

**`created_at_ms`:** Monotonic millisecond timestamp recorded when the
connection enters CONNECTING. Used for diagnostic purposes and connection
age tracking in observe_fn events. Not updated on recycling — it reflects
the age of the current fd, not the cumulative age of connection attempts.

**`last_active_ms`:** Monotonic millisecond timestamp updated on every
transition into IDLE. Used by the idle reaper heap and validate_fn threshold
comparison. Updated by `conn_transition()` as a state-entry invariant.

**`flags`:** Internal bitfield. Currently defined bits:

| Bit | Name | Meaning |
|---|---|---|
| 0 | `CONN_FLAG_TOMBSTONE` | Slot is a tombstone — was occupied, now vacated |
| 1 | `CONN_FLAG_CLOSING_DEFERRED` | CLOSING→DEAD deferred until callback returns |
| 2 | `CONN_FLAG_EVER_ACTIVE` | Connection has been ACTIVE at least once — gates BRAID_EV_CONN_CREATED |

**`heap_index`:** Position of this connection in the idle reaper heap. Set
when the connection enters IDLE; cleared to `UINT32_MAX` on IDLE exit.
`conn_transition()` maintains this field as a state-entry invariant. Using
`heap_index` on the record itself rather than a parallel index array
eliminates the fd-modulo collision risk that would arise from an external
index keyed on `fd % max_connections`. See §7.1.

**`tag`:** Inline `braid_fd_tag_t` struct embedded directly in the connection
record — not a pointer to a separately allocated struct. Since the connection
table is pre-allocated at pool creation, all tag structs are pre-allocated
with it. No allocation occurs after `braid_pool_create()`. `epoll_data.ptr`
is set to `&conn->tag` when registering the fd. See §8.2.

### 3.3 Lookup, Insert, Delete

**Lookup:** Compute `slot = fd % table_size`. Walk forward with linear
probing, skipping tombstones, until the fd matches or an empty slot is
found. Empty slot means not found. Tombstone does not terminate search.

**Insert:** Compute probe start. Walk forward, accepting the first empty
or tombstone slot. Write the record. If no slot is found (table full),
this is a programming error — the table is sized to prevent it.

**Delete:** Mark the slot as tombstone (`CONN_FLAG_TOMBSTONE`, set fd to
-1). Tombstone slots are reused on the next insert that reaches them —
no compaction is performed. This is correct and complete.

**Why no compaction:** Compaction moves live `braid_conn_t` records in
memory. Because `epoll_data.ptr` is set to `&conn->tag` (an inline field
of the record), any move would invalidate all registered event pointers in
the caller's epoll instance, requiring a full re-registration pass that
libbraid cannot perform safely. Compaction is permanently excluded.

The table is sized at `2 × max_connections`, ensuring load factor ≤ 0.5 at
full pool capacity. At that load factor, linear probe chains remain short
(expected length < 1.5 under uniform hashing) even with accumulated
tombstones, because the total number of live + tombstone slots never
exceeds `max_connections` — tombstones appear only where live records were
deleted, and new inserts reuse them before the table fills. No maintenance
beyond tombstone-on-delete is required.

---

## 4. Connection Lifecycle State Machine

### 4.1 States

```
CONNECTING   — socket created, TCP handshake in progress
INITIALIZING — TCP connected, init_fn executing
IDLE         — fully ready, available for checkout
ACTIVE       — checked out, exclusively owned by caller
CLOSING      — destroy_fn executing, graceful teardown in progress
DEAD         — terminal; fd closed, record vacated, replacement scheduled
```

DEAD is not a stable state. A connection record in DEAD is immediately
processed: destroy_fn has already been called, the fd is closed by libbraid,
the slot is vacated, and if the pool is below `min_connections` a new
reconnection entry is inserted into the reconnection heap.

### 4.2 Legal Transitions

```
CONNECTING   → INITIALIZING   TCP handshake complete
CONNECTING   → DEAD           connect() failed or connect_timeout exceeded
INITIALIZING → IDLE           init_fn returned success
INITIALIZING → DEAD           init_fn returned error or init_timeout exceeded
IDLE         → ACTIVE         checkout serving a waiter or direct checkout
IDLE         → CLOSING        idle reaper threshold exceeded, or pool destroy
ACTIVE       → IDLE           checkin with BRAID_CONN_OK
ACTIVE       → CLOSING        checkin with BRAID_CONN_DISCARD
ACTIVE       → DEAD           half-open detected mid-use (read/write error)
CLOSING      → DEAD           destroy_fn returned
```

All other transitions are illegal. `conn_transition()` asserts legality on
every call. In debug builds, an illegal transition aborts with a diagnostic
message identifying source state, target state, and the fd. In release
builds, an illegal transition returns an error code and leaves state
unchanged.

### 4.3 `conn_transition()` — Single Enforcement Point

```c
static int conn_transition(braid_pool_t *pool, braid_conn_t *conn,
        braid_state_t new_state);
```

`conn_transition()` is the only function that writes `conn->state`. It is
not part of the public API. All internal subsystems — checkout engine,
checkin engine, reconnection engine, idle reaper, pool destroy path — call
`conn_transition()` to advance state.

Responsibilities on each transition:

**On any transition:**
- Assert transition legality.
- Write `conn->state = new_state`.

**On IDLE entry:**
- Set `conn->last_active_ms` to current monotonic time.
- Insert connection into the idle reaper heap (see §7).

**On IDLE exit:**
- Remove connection from the idle reaper heap.

**On CONNECTING entry:**
- `conn->created_at_ms` is set to the current monotonic time by `conn_alloc()`
  when the CONNECTING state is established by construction. CONNECTING is always
  the initial state of a new connection; no existing connection transitions
  *into* CONNECTING via `conn_transition()`, so `conn_transition()` does not
  handle this case.

**On DEAD entry:**
- Call `destroy_fn` if not already called (CLOSING already called it).
- Call `io_unwatch(pool, conn->fd)` to remove the fd from the caller's
  epoll/kqueue instance before closing. This must happen before `close()`
  — closing an fd that is still registered in epoll leaves a stale
  registration that may match a newly allocated fd with the same number.
- Close the fd.
- Set `conn->tag.fd = -1` and `conn->tag.magic = 0` to invalidate the
  inline tag. This is a safety measure — the slot may be reused, and
  clearing the magic ensures any in-flight epoll event referencing this
  slot's tag will fail the `BRAID_FD_MAGIC` check.
- Vacate the hash table slot.
- Decrement active connection count.
- If pool is below `min_connections` AND pool is not shutting down, insert
  a reconnection entry into the reconnection heap (see §6). The shutdown
  flag suppresses this insertion — no new connections are created after
  `braid_pool_destroy()` is called.
- Fire `BRAID_EV_CONN_DESTROYED` via observe_fn.

**Note on CONN_DESTROYED without preceding CONN_CREATED:** A connection
that fails during INITIALIZING (init_fn error or deadline exceeded) or
CONNECTING (connect() failure) transitions directly to DEAD without ever
entering ACTIVE. In this case `BRAID_EV_CONN_DESTROYED` fires without a
preceding `BRAID_EV_CONN_CREATED`. Callers implementing observe_fn must
handle this case — a CONN_DESTROYED event is not guaranteed to have a
matching CONN_CREATED.

**On CLOSING entry:**
- Call `destroy_fn(fd, conn_ctx, hook_context)`.
- `conn_transition()` immediately follows with `→ DEAD` after destroy_fn
  returns, unless the CLOSING transition occurred inside a callback, in
  which case `CONN_FLAG_CLOSING_DEFERRED` is set and the `→ DEAD` transition
  is deferred until the callback returns (see §9).

**On ACTIVE entry:**
- Fire `BRAID_EV_CONN_CREATED` via observe_fn on first ACTIVE entry only
  (i.e., when the connection has not been ACTIVE before — tracked by
  `CONN_FLAG_EVER_ACTIVE`). Recycled connections (ACTIVE → IDLE → ACTIVE)
  do not re-fire this event.

**Observability note:** `BRAID_EV_CONN_CREATED` tracks fd lifecycle, not
checkout frequency. Callers needing checkout-level metrics (checkouts per
second, pool utilisation) must instrument the checkout callback rather than
observe_fn. This is intentional — observe_fn is a lifecycle hook, not a
request counter.

`conn_transition()` does not allocate memory, does not block, and does not
call back into the public API. It is re-entrant-safe with respect to the
deferred work mechanism described in §9.

### 4.4 State Diagram

```
                    ┌─────────────────┐
              ┌────▶│   CONNECTING    │────────────────────┐
              │     └────────┬────────┘                    │
              │              │ TCP handshake complete       │ connect fail /
              │              ▼                             │ connect_timeout
              │     ┌─────────────────┐                    │
              │     │  INITIALIZING   │────────────────────┤
              │     └────────┬────────┘ init_fn fail /     │
              │              │ init_fn ok  init_timeout     │
              │              ▼                             │
              │     ┌─────────────────┐                    │
              │     │      IDLE       │◀──────────────┐    │
              │     └────────┬────────┘               │    │
              │              │ checkout            BRAID_CONN_OK
              │              ▼                         │    │
              │     ┌─────────────────┐               │    │
              │     │     ACTIVE      │───────────────┘    │
              │     └────────┬────────┘                    │
              │              │ BRAID_CONN_DISCARD           │
              │              │ or half-open detected        │
              │              ▼                             │
              │     ┌─────────────────┐                    │
              │     │    CLOSING      │                    │
              │     └────────┬────────┘                    │
              │              │ destroy_fn returns           │
              │              ▼                             ▼
              │     ┌─────────────────────────────────────────┐
              └─────│              DEAD                       │
                    └─────────────────────────────────────────┘
                          │ if pool below min_connections
                          ▼
                    reconnection heap (see §6)
```

---

## 5. TCP Keepalive Configuration

libbraid enables TCP keepalive on every socket it creates. This is the first
of two half-open detection mechanisms. The second is `validate_fn` (see §5 of
the API surface — `idle_threshold` in config).

Keepalive is configured per-socket immediately after `socket()` returns,
before `connect()` is called. The following socket options are set:

| Option | Value source | Purpose |
|---|---|---|
| `SO_KEEPALIVE` | hardcoded 1 | Enable kernel keepalive probes |
| `TCP_KEEPIDLE` | `config.keepalive_idle` | Seconds idle before first probe |
| `TCP_KEEPINTVL` | `config.keepalive_interval` | Seconds between subsequent probes |
| `TCP_KEEPCNT` | `config.keepalive_count` | Failed probes before connection declared dead |

**Platform differences:** `TCP_KEEPIDLE` is Linux-specific. On OpenBSD,
the per-socket idle override uses `TCP_KEEPALIVE` (same semantics, different
constant name). `TCP_KEEPINTVL` and `TCP_KEEPCNT` exist on both platforms.
`conn_keepalive_configure()` in `braid_conn.c` uses `#ifdef` to select the
correct constant. This is the only platform conditional permitted outside
the I/O abstraction layer, because keepalive configuration is part of
socket setup, not event dispatch.

When the kernel declares a connection dead via keepalive, the next
`read()` or `write()` on that fd returns an error (`ECONNRESET` or `ETIMEDOUT`).
libbraid detects this error in `braid_pool_notify()` and transitions the
connection directly to `DEAD` (from ACTIVE) or `CLOSING → DEAD` (from IDLE).

**Default values:**

| Parameter | Default | Rationale |
|---|---|---|
| `keepalive_idle` | 60 s | Probe starts after 60 s idle — shorter than most NAT timeouts |
| `keepalive_interval` | 10 s | 10 s between probes — fast enough to detect failure within a minute |
| `keepalive_count` | 3 | Dead after 30 s of failed probes — tolerates transient packet loss |

With these defaults, a half-open connection is detected within 90 seconds
(60 s idle + 3 × 10 s probes). Callers with stricter latency requirements
reduce `keepalive_idle`. Callers on high-packet-loss paths increase
`keepalive_count`.

**`validate_fn` relationship:** keepalive catches half-open connections
during IDLE residence. `validate_fn` catches them at checkout time when
idle duration exceeds `idle_threshold`. The two mechanisms are complementary:
keepalive operates continuously in the background; `validate_fn` provides an
application-level check at the moment a connection is about to be used.

---

## 6. Reconnection Engine

### 6.1 Reconnection Heap

The reconnection engine manages connections that have reached DEAD and need
to be replaced to maintain `min_connections`, plus capacity requested by
queued checkouts. It uses a min-heap keyed on `next_retry_ms` — the absolute
monotonic time at which the next connect attempt should be made.

**Structure:**

```c
typedef struct braid_reconnect_entry {
        uint64_t  next_retry_ms;  /* absolute monotonic ms for next attempt  */
        uint32_t  attempt;        /* attempt counter, 0-indexed               */
} braid_reconnect_entry_t;
```

No fd, no destination — both are pool-global. The host and port come from
`pool->config.host` and `pool->config.port` at attempt time.

**Heap size:** Fixed at `max_connections` slots, allocated at pool creation.
At most `max_connections` connections can be dead and pending reconnection
simultaneously.

**Demand growth:** `min_connections` is a warm baseline, not a fixed pool
size. A checkout with `timeout_ms > 0` that must queue raises the capacity
target to `ACTIVE + queued waiters`, capped at `max_connections`; live and
already-scheduled connections count toward that target. A timeout of zero
never schedules a connection. The idle reaper eventually returns unused
surplus capacity to the minimum floor.

**Operations:**
- `reconnect_heap_push(heap, entry)` — O(log n) insert after bubble-up
- `reconnect_heap_peek(heap)` — O(1) read minimum `next_retry_ms`
- `reconnect_heap_pop(heap)` — O(log n) delete minimum after sift-down
- `reconnect_heap_clear(heap)` — O(1) reset count to zero, used at destroy

### 6.2 Full Jitter Backoff Algorithm

The reconnection engine uses full jitter exponential backoff. This algorithm
was chosen over equal jitter and decorrelated jitter because it provides the
strongest desynchronisation across multiple reconnecting clients — the primary
defence against connection storms when a backend recovers.

**Algorithm:**

```
sleep = random_uniform(0, min(cap, base × 2^attempt))
```

Where:
- `base` = `config.backoff_base` (milliseconds)
- `cap` = `config.backoff_cap` (milliseconds)
- `attempt` = zero-indexed attempt counter from `braid_reconnect_entry_t`
- `random_uniform(lo, hi)` — uniform random integer in [lo, hi]

**Default values:**

| Parameter | Default | Rationale |
|---|---|---|
| `backoff_base` | 100 ms | First retry fires quickly — transient failures should resolve fast |
| `backoff_cap` | 30 000 ms | Cap at 30 s — avoids indefinite silence after extended outage |
| `backoff_max_attempts` | 0 | 0 = retry forever; suits long-lived server processes |

**Random number source:** Each pool maintains its own PRNG state seeded
at `braid_pool_create()` time (e.g. from `getentropy()` or `arc4random()`).
Using a process-global `random()` / `srandom()` is incorrect — it introduces
shared mutable state between pool instances and is not multi-pool safe. The
per-pool PRNG state is a field in `braid_pool_t`. The implementation caps the
exponent at 31 before computing `base × 2^attempt`.
The result is then clamped to `cap`, so the behaviour is correct for all
attempt values.

**DNS at each attempt:** The host string from `config.host` is resolved
fresh on every connect attempt. DNS resolution is synchronous via
`getaddrinfo()` called immediately before `socket()` + `connect()`. This
ensures that DNS-based failover and load balancing take effect on reconnection
without requiring the pool to be destroyed and recreated.

### 6.3 Reconnection Flow

On each `braid_pool_advance()` call:

1. Peek at the reconnection heap minimum.
2. If `now_ms < entry.next_retry_ms`, compute `next_ms` contribution and skip.
3. Pop the entry.
4. If `config.backoff_max_attempts > 0` and `entry.attempt >= max_attempts`,
   do not reconnect — pool will run below `min_connections`. Fire
   `BRAID_EV_RECONNECT_ATTEMPT` with failure and no re-insertion.
5. Resolve DNS via `getaddrinfo()` on `config.host`. If resolution fails,
   treat as a failed attempt: insert a new reconnect entry for `attempt+1`
   using the backoff algorithm and stop.
6. Create socket, apply keepalive settings (§5), set `O_NONBLOCK` and
   `O_CLOEXEC` via `fcntl()`, call non-blocking `connect()`.
7. **If `connect()` returns 0 immediately** (fast local connect): skip the
   writable-event wait path. Call `conn_transition(→ INITIALIZING)` directly,
   invoke `init_fn` if registered (or skip to IDLE if NULL), then
   `conn_transition(→ IDLE)` on success or `→ DEAD` on failure. Register
   fd for readability (not writability) in epoll.
8. **If `connect()` returns `EINPROGRESS`**: allocate a connection record,
   `conn_transition(→ CONNECTING)`, register fd in epoll for writability.
   Connect completion will be signalled via a writable event delivered to
   `braid_pool_notify()`.
9. **If `connect()` returns any other error**: treat as failed attempt.
   Insert a new reconnect entry for `attempt+1` with backoff delay. Fire
   `BRAID_EV_RECONNECT_ATTEMPT` with failure.
10. When a connection reaches IDLE, immediately serve the oldest queued
    waiter before leaving reconnect processing.
11. Fire `BRAID_EV_RECONNECT_ATTEMPT` with attempt number (in-progress cases).

**Reconnect entry bookkeeping:** A new reconnect heap entry for `attempt+1`
is inserted **only on failure** — either a connect() error, a DNS failure,
or a subsequent CONNECTING/INITIALIZING/registration failure. The pending
connection retains its originating attempt number until it is fully registered
as IDLE, so asynchronous failure and connect timeout preserve the backoff
sequence rather than restarting at attempt zero. It is not pre-inserted at
attempt start. If the attempt succeeds (CONNECTING→INITIALIZING→IDLE), no
follow-up entry is needed and none is inserted.

**Terminal failure:** When a finite `backoff_max_attempts` limit is reached,
and there is no live connection or pending reconnect that can serve queued
checkouts, all queued callbacks receive `BRAID_ERR_CONNFAIL`. Existing ACTIVE
connections remain usable and may still serve waiters when checked in.

**`connect_timeout` enforcement:** CONNECTING connections that have exceeded
`created_at_ms + connect_timeout` are aborted by `braid_pool_advance()` (see
§11, step 2). The timeout is enforced by scanning CONNECTING connections
during each advance call.

---

## 7. Idle Reaper

### 7.1 Idle Reaper Heap

The idle reaper uses a min-heap keyed on `last_active_ms` containing only
connections in the IDLE state. This allows `braid_pool_advance()` to compute
the exact time until the next idle reap event without scanning the full
connection table.

**Structure:** The heap stores `(last_active_ms, conn*)` pairs. A direct
pointer to the connection record is used rather than `fd` — the connection
table is allocated once at pool creation and never reallocated, so the
pointer is stable for the connection's lifetime. This avoids a hash table
lookup on every sift-up and sift-down operation during heap maintenance.

**Heap size:** Fixed at `max_connections` slots, allocated at pool creation.

**Heap maintenance:** `conn_transition()` maintains the heap as a state-entry
invariant:
- On IDLE entry: insert `(conn->last_active_ms, conn)` into the heap;
  write the assigned heap position into `conn->heap_index`.
- On IDLE exit (to ACTIVE, CLOSING, or DEAD): remove the entry from the heap
  using `conn->heap_index` for O(1) position lookup; set
  `conn->heap_index = UINT32_MAX`.

**Position tracking via `conn->heap_index`:** Removal from the middle of
the heap is O(log n). The entry's position is found in O(1) via
`conn->heap_index` — a field on the connection record itself, updated on
every sift-up and sift-down operation.

**Heap removal algorithm:** Swap the target entry with the last entry in
the heap. Decrement count. Then attempt both sift-up AND sift-down on the
moved element — sift-down alone is insufficient because the moved element
may be smaller than its new parent, requiring sift-up to restore heap order.
Update `heap_index` on both the moved record and any records swapped during
the sift operations. After removal, set `conn->heap_index = UINT32_MAX` on
the removed record.

### 7.2 Reap Logic

On each `braid_pool_advance()` call:

1. Peek at the idle reaper heap minimum `(last_active_ms, conn*)`.
2. If `now_ms - last_active_ms < config.idle_reap_timeout`, no reap needed.
   Compute `next_ms` contribution as `last_active_ms + idle_reap_timeout - now_ms`.
3. Otherwise, check whether reaping this connection would drop the pool below
   `min_connections`. If so, skip this entry and peek the next. Continue until
   a reapable connection is found or the heap is exhausted.
4. Call `conn_transition(conn, CLOSING)` which calls `destroy_fn` and then
   `→ DEAD`.
5. Repeat from step 1 until no more connections are eligible.

**min_connections floor:** The reaper never reduces the live connection count
below `min_connections`. If all IDLE connections are needed to satisfy the
floor, none are reaped regardless of their idle duration. This is a best-effort
guarantee — if the pool is in a degraded state with fewer than `min_connections`
live connections, the reaper does not reap any idle connections at all.

---

## 8. epoll Abstraction Layer

### 8.1 Design

The epoll abstraction layer is a thin internal interface that translates
libbraid's fd management operations into platform-specific syscalls. It is
not part of the public API. Its purpose is to confine all platform-specific
code to one translation unit; the epoll and kqueue implementations share this
interface without modifying any other component.

The interface is intentionally mechanical — it mirrors syscall semantics
rather than imposing a higher-level model. libbraid's internal code expresses
what it wants to do; the abstraction layer translates that intent into the
correct syscall sequence for the platform.

**Interface:**

```c
int io_watch(braid_pool_t *pool, int fd, uint32_t events);
int io_modify(braid_pool_t *pool, int fd, uint32_t events);
int io_unwatch(braid_pool_t *pool, int fd);
```

`events` is a bitmask using libbraid-internal constants:

```c
#define BRAID_IO_READ   0x01
#define BRAID_IO_WRITE  0x02
```

On Linux, these map to `EPOLLIN` and `EPOLLOUT`. On OpenBSD (kqueue), they
map to `EVFILT_READ` and `EVFILT_WRITE` filter additions and deletions via
`kevent()`. The kqueue translation unit handles the per-event filter
registration model internally — callers of the abstraction layer think only
in terms of fd + event mask.

**`io_watch` on Linux:** Calls `epoll_ctl(EPOLL_CTL_ADD)` with `EPOLLET`
(edge-triggered). Sets `epoll_data.ptr` to the connection's `braid_fd_tag_t`
struct (see §8.2). Edge-triggered mode is used to avoid repeated events on
connections that have data available but have not been read.

**`io_modify` on Linux:** Calls `epoll_ctl(EPOLL_CTL_MOD)`. Used when a
connection transitions from watching for writability (connect in progress) to
readability (keepalive error detection while IDLE).

**`io_unwatch` on Linux:** Calls `epoll_ctl(EPOLL_CTL_DEL)`. Called on every
DEAD transition before the fd is closed. An `ENOENT` result is successful: the
fd is already absent from the epoll instance, matching kqueue's tolerated
missing-filter `EV_DELETE` semantics.

The `event_fd` used by the abstraction layer is `pool->config.event_fd` — the
caller's epoll or kqueue instance, passed at pool creation.

### 8.2 fd Tagging — `braid_fd_tag_t`

When the caller calls `epoll_wait()`, events arrive for both their own fds
and libbraid's fds. The caller must distinguish them to know whether to call
`braid_pool_notify()`.

libbraid embeds one `braid_fd_tag_t` struct directly inside each `braid_conn_t`
record as an inline member. Since the connection table is allocated once at
`braid_pool_create()`, all tag structs are pre-allocated with it. No separate
allocation or deallocation occurs. `epoll_data.ptr` is set to `&conn->tag`
when registering the fd with `epoll_ctl`.

```c
typedef struct braid_fd_tag {
        uint32_t  magic;   /* BRAID_FD_MAGIC — identifies libbraid fds */
        int       fd;      /* the fd this tag belongs to                */
} braid_fd_tag_t;
```

`BRAID_FD_MAGIC` is a fixed 32-bit constant defined in the internal header.

The caller inspects the tag at event dispatch:

```c
struct epoll_event *ev = &events[i];
braid_fd_tag_t *tag = (braid_fd_tag_t *)ev->data.ptr;

if (tag->magic == BRAID_FD_MAGIC) {
        braid_pool_notify(pool, tag->fd, ev->events);
} else {
        /* caller's own event handling */
}
```

**Stale event note:** When a connection transitions to DEAD, `io_unwatch()`
is called before `close(fd)`. This prevents new epoll events for that fd.
However, events already returned by a previous `epoll_wait()` call and not
yet processed by the caller could reference a slot that has since been reused
for a new connection. In that case `tag->fd` will reflect the new connection's
fd, not the old one. This fd-reuse race is inherent in all Unix async I/O code.
The standard event loop pattern — call `braid_pool_advance()` and then
`epoll_wait()` once per iteration, processing all events before calling
`advance()` again — minimises the window for this race to near-zero in
practice, because `advance()` cannot reuse a slot between `epoll_wait()` and
the caller processing its result set in the same iteration.

---

## 9. Re-entrancy and Deferred Work

### 9.1 The Re-entrancy Scenario

`braid_pool_checkin()` is safe to call from within a checkout callback. The
scenario is:

1. `braid_pool_advance()` serves the wait queue head.
2. The checkout callback fires with a valid fd.
3. The caller, inside the callback, calls `braid_pool_checkin()` immediately.
4. checkin must not process the `CLOSING → DEAD` transition or serve the next
   wait queue entry while the original callback is still on the call stack.

Without deferral, wait queue serving could re-enter the same code path,
causing use-after-free on the wait queue ring buffer's current-head pointer
and double-invocation of observe_fn events.

### 9.2 Deferred Work Flags

The pool carries a `uint32_t deferred_work` bitfield:

```c
#define BRAID_DEFERRED_SERVE_WAITQUEUE   0x01
#define BRAID_DEFERRED_PROCESS_DEAD      0x02
```

The pool also carries a `uint32_t in_callback` counter, incremented before
any caller callback invocation and decremented after.

In any code path that modifies pool state and would normally trigger wait
queue serving or DEAD processing:

```c
if (pool->in_callback > 0) {
        pool->deferred_work |= BRAID_DEFERRED_SERVE_WAITQUEUE;
        return;
}
```

After every callback invocation:

```c
pool->in_callback--;
if (pool->in_callback == 0 && pool->deferred_work != 0) {
        pool_drain_deferred(pool);
}
```

`pool_drain_deferred()` processes the flagged operations in a defined
order: `BRAID_DEFERRED_PROCESS_DEAD` first (close fds, vacate slots, insert
reconnection entries), then `BRAID_DEFERRED_SERVE_WAITQUEUE` (serve the next
wait queue entry, which may invoke another callback, which is safe because
`in_callback` is 0 at this point and any nested checkin re-enters the deferred
path correctly).

The deferred work flags are idempotent — each flag represents a class of work,
not a specific operation. Running the drain pass once after the outermost
callback returns is sufficient regardless of how many times checkin was called
during the callback.

---

## 10. Wait Queue

### 10.1 Ring Buffer

The wait queue is a fixed-size ring buffer allocated at pool creation to
`max_connections` slots. This is the correct bound: there is no value in
queuing more waiters than the maximum number of connections that could
eventually serve them.

```c
typedef struct braid_waiter {
        braid_checkout_cb  cb;         /* callback to invoke when served       */
        void              *cb_ctx;     /* opaque context passed through to cb  */
        uint64_t           deadline_ms;/* absolute monotonic expiry time        */
        braid_token_t      token;      /* opaque, monotonically issued token    */
        uint32_t           flags;      /* WAITER_FLAG_TOMBSTONE for cancel      */
} braid_waiter_t;
```

**Ring state:** `head` and `tail` indices (uint32_t), both modulo
`max_connections`. `tail` is the next write position; `head` is the next
read position. `count` tracks live (non-tombstone) entries. The physical span
from `head` to `tail` also includes tombstones and cannot exceed capacity;
only tombstones that reach `head` are reusable without breaking FIFO order.

**Enqueue (tail):** O(1). Write to `ring[tail % capacity]`, advance tail.

**Dequeue (head):** O(1) amortised. Read from `ring[head % capacity]`,
advance head. Skip tombstones — advance head without invoking callback.
Tombstone skipping is O(k) where k is consecutive tombstones at head, bounded
by the ring size.

### 10.2 Tokens

`braid_token_t` is uint64_t. Tokens are opaque, monotonically issued values;
they must not be interpreted. Their sole valid use is as an argument to
`braid_pool_cancel()`. `BRAID_TOKEN_NONE` denotes that no waiter was queued
and is always safe to cancel as a no-op.

When a connection is immediately available at checkout time (no enqueue
occurred), `braid_pool_checkout()` writes `BRAID_TOKEN_NONE` when the caller
provided a token pointer. Calling `braid_pool_cancel()` with that token is a
documented no-op.

### 10.3 Cancellation via Tombstone

`braid_pool_cancel()` locates the slot at `ring[token % capacity]` and
verifies that the stored token value matches the provided token before
acting. This check is mandatory — without it, a stale token whose ring slot
has been reused by a new waiter would cancel the wrong request. If
`slot.token != token`, the cancel is a no-op (the original waiter has already
been served, timed out, or the pool was shut down). If the tokens match, set
`WAITER_FLAG_TOMBSTONE`, invoke the callback with `BRAID_ERR_CANCELLED`, and
decrement count.

One callback per checkout is guaranteed by the tombstone mechanism: once a
slot is tombstoned, the dequeue path skips it without invoking the callback
again. Once the callback fires (on serve, timeout, or cancel), the slot is
tombstoned to prevent double-invocation.

### 10.4 Timeout Expiry

On each `braid_pool_advance()` call, the wait queue is scanned from head
forward. Any waiter with `deadline_ms <= now_ms` that is not already a
tombstone has its callback invoked with `BRAID_ERR_TIMEOUT` and is tombstoned.

The scan stops at the first non-expired, non-tombstone entry. This is correct
for a FIFO queue under the assumption that later entries have later deadlines.
If callers enqueue with non-monotone deadlines (valid — each checkout
specifies its own `timeout_ms`), a short-timeout waiter behind a long-timeout
waiter in the ring may be delayed indefinitely. This is an accepted trade-off
for the connection pool use case, where checkout `timeout_ms` values are
typically uniform and small. Callers with mixed timeout requirements should be
aware of this limitation. A full ring scan on every advance call would avoid
the issue but adds O(n) cost per advance iteration.

---

## 11. `braid_pool_advance()` Execution Order

`braid_pool_advance()` is the driver for all timer-based and deferred work.
The caller invokes it once per event loop iteration, before `epoll_wait()`.

Execution order on each call:

1. **Capture current time.** Call `clock_gettime(CLOCK_MONOTONIC)` once.
   All subsequent comparisons in this advance call use this snapshot.

2. **Process reconnection heap.** Pop entries with `next_retry_ms <= now_ms`.
   Attempt to create new connections (§6.3). Stop when heap minimum is in
   the future.

2a. **Enforce connect_timeout on CONNECTING connections.** Scan all
    connections in CONNECTING state. For each where
    `now_ms > conn->created_at_ms + config.connect_timeout`: call
    `io_unwatch()`, `close(fd)`, `conn_transition(→ DEAD)`. The DEAD
    transition inserts a reconnect entry (if not in shutdown). This scan
    is O(max_connections) in the worst case but CONNECTING connections are
    few and short-lived; the scan terminates quickly in practice.

3. **Process idle reaper heap.** Pop entries with
   `last_active_ms + idle_reap_timeout <= now_ms`, subject to
   `min_connections` floor (§7.2). Stop when heap minimum is in the future.

4. **Expire wait queue entries.** Scan wait queue from head, invoke callbacks
   with `BRAID_ERR_TIMEOUT` for expired waiters (§10.4).

5. **Drain deferred work.** Process `deferred_work` flags if `in_callback == 0`
   (§9.2). This covers any DEAD connections or wait queue serving deferred
   from a previous callback invocation.

6. **Compute `next_ms`.** Take the minimum of:
   - Time until reconnection heap minimum fires
   - Time until soonest CONNECTING connection hits connect_timeout
   - Time until idle reaper heap minimum fires
   - Time until the soonest non-tombstone wait queue entry expires
   - `UINT32_MAX` if none of the above have a scheduled event

   Write to `*next_ms`. The caller passes this value as the timeout to
   `epoll_wait()`.

`braid_pool_advance()` does not call `epoll_wait()` itself. It does not
block. It may invoke caller callbacks (checkout callbacks for wait queue
entries served by new IDLE connections arising from reconnection). All
callback invocations follow the deferred work protocol (§9).

---

## 12. `braid_pool_notify()` Execution

`braid_pool_notify(pool, fd, events)` is called by the caller when
`epoll_wait()` returns an event on a fd belonging to libbraid. The caller
identifies libbraid fds via the `braid_fd_tag_t` magic check (§8.2).

Dispatch by connection state:

**CONNECTING:** Event is writable — connect completed or failed.
Call `getsockopt(SO_ERROR)`. If error: `conn_transition(→ DEAD)`. If no
error: `conn_transition(→ INITIALIZING)`. If `init_fn` is registered, call
it with the `in_callback` protocol and deadline; on success
`conn_transition(→ IDLE)`; on failure `conn_transition(→ DEAD)`. If
`init_fn` is NULL, transition directly `INITIALIZING → IDLE` — no hook
invocation, no deadline tracking. `io_modify()` to switch event registration
from writability to readability regardless of whether `init_fn` is present.

**IDLE:** Event is readable — the peer has sent data, closed the connection,
or the connection has errored. Kernel TCP keepalive ACKs do NOT make the
socket readable; a readable IDLE socket always means one of: unexpected
protocol data, EOF, or error. In all three cases the connection is no longer
in a clean state for the pool.

Call `recv(fd, &probe_byte, 1, MSG_PEEK)`:
- Returns -1 with `EAGAIN`/`EWOULDBLOCK`: spurious wakeup, no action.
- Returns 0: peer closed connection (EOF). Half-open detected.
  `conn_transition(→ CLOSING → DEAD)`.
- Returns 1: unexpected data from peer sitting in the receive buffer.
  This indicates a protocol error or connection reuse bug in the caller.
  Treat as unclean — `conn_transition(→ CLOSING → DEAD)`.
- Returns -1 with any other error: connection error. Half-open detected.
  `conn_transition(→ CLOSING → DEAD)`.

In all non-EAGAIN cases, `conn_transition(→ CLOSING → DEAD)` is called.

**Note on zero-length MSG_PEEK:** `recv(fd, NULL, 0, MSG_PEEK)` is not a
valid liveness test — it returns 0 unconditionally on most implementations
regardless of connection state. Always use a 1-byte peek buffer.

**ACTIVE:** Caller owns the fd. libbraid does not register IDLE connections
for readability events while ACTIVE — the registration is removed on IDLE→ACTIVE
transition and restored on ACTIVE→IDLE. If a spurious event arrives for an
ACTIVE fd (timing edge case), it is silently ignored.

**CLOSING, DEAD:** Event on a closing or dead fd is a timing artefact of
the epoll edge-triggered model. Silently ignored.

---

## 13. Pool Lifecycle

### 13.1 `braid_pool_create()`

Allocates and initialises all internal structures in a single pass:

1. Validate config: `event_fd >= 0`, `min_connections <= max_connections`,
   `max_connections > 0`.
2. Allocate `braid_pool_t`.
3. Allocate connection table: `2 × max_connections` slots of `braid_conn_t`.
4. Allocate reconnection heap: `max_connections` slots.
5. Allocate idle reaper heap: `max_connections` slots.
6. Allocate wait queue ring: `max_connections` slots of `braid_waiter_t`.
7. Copy config into pool (deep copy `config.host` via `strdup()`).
8. Insert `min_connections` entries into the reconnection heap with
   `next_retry_ms = 0` (fire immediately on first advance call). This starts
   the warm pool population without blocking `braid_pool_create()`.

If any allocation fails, all previously allocated memory is freed and NULL
is returned with the error code written to `*err`.

### 13.2 `braid_pool_destroy()`

`braid_pool_destroy(pool, drain_timeout_ms)` initiates orderly shutdown.

**Shutdown pattern — caller responsibility:** `braid_pool_destroy()` is a
blocking teardown function. It must be called only after the caller has
ensured that no ACTIVE connections remain, or with `drain_timeout_ms = 0`
for emergency teardown. Calling `braid_pool_destroy()` from within the
event loop thread while ACTIVE connections exist with `drain_timeout_ms > 0`
will deadlock — the event loop cannot run while destroy is blocking, so
in-flight requests can never call `braid_pool_checkin()`. The correct
graceful shutdown sequence is:

1. Stop accepting new work at the application level.
2. Allow in-flight requests to complete and call `braid_pool_checkin()`.
3. Once all connections are IDLE (verified by the caller), call
   `braid_pool_destroy()` from the event loop thread.

For emergency teardown (process exit, fatal error) where connections cannot
be cleanly drained, pass `drain_timeout_ms = 0` (forced immediate teardown).

**Shutdown suppresses reconnection:** The moment the pool is marked as
shutting down (step 1 of the shutdown sequence), the `DEAD` entry path in
`conn_transition()` no longer inserts reconnection entries into the heap.
Reconnection entries that are already in the heap are discarded at step 7.
No new connections are created after shutdown begins.

**`drain_timeout_ms` semantics:**
- `> 0`: wait up to `drain_timeout_ms` milliseconds for any remaining ACTIVE
  connections to be checked in, polling via `braid_pool_advance()` with short
  sleeps. If the deadline expires with ACTIVE connections still outstanding,
  proceed to forced teardown.
- `= 0`: skip the drain wait entirely — proceed directly to forced teardown.
  This is intentionally different from `timeout_ms = 0` in
  `braid_pool_checkout()` (which means fail-immediately). The parameter name
  `drain_timeout_ms` makes this distinction explicit.

**Shutdown steps:**

1. Mark pool as shutting down, discard all pending reconnection entries, and
   reject new checkout calls with `BRAID_ERR_SHUTDOWN`. No reconnect attempt
   may begin after this point, including while draining ACTIVE connections.
2. Cancel all pending wait queue entries — invoke callbacks with
   `BRAID_ERR_SHUTDOWN`, tombstone slots.
3. If `drain_timeout_ms > 0`: wait up to `drain_timeout_ms` for all ACTIVE
   connections to be checked in. Poll by calling `braid_pool_advance()` in
   a loop with short sleeps.
4. Transition all CONNECTING sockets to DEAD: call `io_unwatch()` and
   `close(fd)` on each. `destroy_fn` is not called for CONNECTING sockets
   (no protocol state has been established). `BRAID_EV_CONN_DESTROYED` fires
   via observe_fn.
5. Transition all INITIALIZING connections to DEAD: call `destroy_fn` (may
   have partial protocol state), then `close(fd)`.
6. Transition all IDLE connections to `CLOSING → DEAD` — calls `destroy_fn`
   for each.
7. Unregister all remaining fds from epoll/kqueue via `io_unwatch()`.
8. Free all allocated structures: connection table, heaps, ring buffer,
   `strdup`'d host string, pool struct.

**Forced teardown (drain timeout expired or `drain_timeout_ms = 0` with
ACTIVE connections remaining):**
Close remaining ACTIVE fds directly via `close()`. `destroy_fn` is not
called for force-closed connections — the caller retains responsibility for
those fds and their protocol state. Any writes to these fds by a caller
thread that has not yet been notified of pool destruction may raise `SIGPIPE`.
This is documented as a caller responsibility — callers must coordinate their
own shutdown before calling destroy with active connections.

---

## 14. Monotonic Time

libbraid uses `CLOCK_MONOTONIC` throughout for all time comparisons and
deadline calculations. `CLOCK_REALTIME` is never used internally.

All time values in the public API (`timeout_ms` parameters, `deadline_ms`
in hook signatures) are either relative durations (converted to absolute
monotonic time internally on receipt) or absolute monotonic milliseconds
(hook deadlines). The distinction is documented per-parameter in the API
reference.

`braid_pool_advance()` captures a monotonic-time snapshot at entry and uses it
for its reconnect-heap, connect-timeout, idle-reaper, and wait-queue-expiry
scans. Operations that occur during the call may take additional readings to
timestamp a lifecycle transition, schedule a retry after a failure, or enforce
a hook deadline. Checkout, checkin, and notify likewise read monotonic time
only when their own timing work requires it.

All internal absolute deadlines use saturating addition. A timeout added near
`UINT64_MAX` therefore becomes `UINT64_MAX` rather than wrapping into the
past. The idle reaper also treats a clock value earlier than `last_active_ms`
as not yet eligible, avoiding unsigned-underflow reaps.

---

## 15. Platform Portability

### 15.1 Abstraction Boundary

All platform-specific code is confined to the epoll abstraction layer
(§8.1). The abstraction layer is one translation unit per platform:

```
src/braid_io_epoll.c   — Linux (v1)
src/braid_io_kqueue.c  — OpenBSD, FreeBSD, NetBSD
```

The build system selects the correct translation unit. No `#ifdef` for
platform differences appears outside these files.

### 15.2 Linux — epoll (v1)

`epoll_create1(EPOLL_CLOEXEC)` is used (not `epoll_create()`). All fds are
registered with `EPOLLET` (edge-triggered). The epoll fd is provided by the
caller via `config.event_fd` — libbraid does not create its own epoll
instance.

### 15.3 OpenBSD, FreeBSD, NetBSD — kqueue

kqueue uses per-event filter registration (`EVFILT_READ`, `EVFILT_WRITE`)
rather than epoll's per-fd bitmask. The kqueue translation unit maps
`io_watch(fd, BRAID_IO_WRITE)` to a single `kevent(EVFILT_WRITE, EV_ADD)`
call and `io_modify(fd, BRAID_IO_READ)` to a `kevent(EVFILT_WRITE, EV_DELETE)`
followed by `kevent(EVFILT_READ, EV_ADD)`. This model difference is entirely
internal to `src/braid_io_kqueue.c`.

The `udata` field of `struct kevent` is set to the `braid_fd_tag_t*` pointer,
matching the role of `epoll_data.ptr` on Linux. Caller event routing code
(§8.2) is platform-independent — it inspects the tag magic value regardless
of whether the pointer arrived via epoll or kqueue.

FreeBSD and NetBSD share the kqueue translation unit without modification.
kqueue is the common interface across all three platforms.

---

## 16. Default Values Summary

| Config field | Default | Unit |
|---|---|---|
| `connect_timeout` | 5 000 | ms |
| `init_timeout` | 10 000 | ms |
| `validate_timeout` | 2 000 | ms |
| `idle_threshold` | 30 000 | ms |
| `idle_reap_timeout` | 300 000 | ms |
| `keepalive_idle` | 60 | s |
| `keepalive_interval` | 10 | s |
| `keepalive_count` | 3 | probes |
| `backoff_base` | 100 | ms |
| `backoff_cap` | 30 000 | ms |
| `backoff_max_attempts` | 0 | 0 = forever |

All timeout and threshold fields in `braid_config_t` default to the values
above when left at zero in a zero-initialised config struct. A zero value is
never interpreted as "no timeout" except for `backoff_max_attempts`, where
zero explicitly means retry forever. This exception is documented in the API
reference for that field.

---

## 17. Thread Safety

The pool is designed for single-threaded use per instance. All public API
functions are unsafe to call concurrently on the same pool from multiple
threads without external synchronisation.

The intended deployment model is one pool per worker. A process running N
workers on N threads creates N pool instances. Each worker calls its own
pool's API exclusively. There is no cross-pool shared state, no global state,
and no locking inside libbraid.

Ownership semantics provide the concurrency primitive for the checked-out
fd. Once checked out, the fd is exclusively owned by the caller until
checkin. The caller may pass the fd to another thread, hand it off to a
libstrand fiber, or use it however they wish — libbraid makes no access to
the fd while it is checked out and does not need to be notified about
internal caller threading.

**Hook invocation threading:** All hooks (`init_fn`, `validate_fn`,
`destroy_fn`, `observe_fn`) are invoked from the thread calling
`braid_pool_advance()` or `braid_pool_notify()`. Hooks must not call back
into libbraid API functions other than `braid_pool_checkin()` (which is safe
per §9). Hooks must not block.

**`hook_context` lifetime:** The `config.hook_context` pointer is passed
through to every hook invocation for the entire lifetime of the pool,
including during forced teardown in `braid_pool_destroy()`. The caller must
ensure that `hook_context` remains valid until `braid_pool_destroy()` returns.
Freeing `hook_context` before destroy completes will cause a use-after-free
if any hook is invoked during teardown.

**`braid_pool_notify` return value:** Returns `int`. Returns `BRAID_OK`
on all normal paths, including when the fd is not recognised as a
libbraid-managed fd (a normal timing artefact — the caller silently
ignores the return value in this case). `BRAID_ERR_INVAL` is not returned
for unrecognised fds — silently returning `BRAID_OK` is the correct
behaviour, as `braid_pool_notify()` is on a hot dispatch path and the
caller has already done the magic-number check before calling it.

**Hook fd-ownership rules:** Hooks must not close the fd passed to them,
set it blocking (`O_NONBLOCK` must remain set), unregister it from the
caller's event loop, or otherwise interfere with pool fd ownership. If
`destroy_fn` closes the fd, the subsequent `close()` by libbraid will close
a different fd if the OS has already recycled the number — this is a
hard bug, not a nuisance. `destroy_fn` must release protocol state (free
`conn_ctx`) and perform graceful protocol teardown writes only. It must
leave the fd open for libbraid to close.

**`validate_fn` timeout enforcement:** `validate_fn` receives an absolute
monotonic deadline in milliseconds (`deadline_ms` parameter) computed as
`braid_now_ms() + config.validate_timeout` at the point of invocation.
`validate_fn` must respect this deadline. libbraid checks elapsed time when
`validate_fn` returns: if `braid_now_ms() > deadline_ms`, the connection is
treated as a validation failure regardless of the return value and transitions
to CLOSING → DEAD. Enforcement is contractual, not preemptive — a hanging
`validate_fn` stalls the event loop for the duration. The `validate_timeout`
default is 2000 ms (see §16).

**`destroy_fn` re-entrancy restriction:** `destroy_fn` is called with
`pool->in_callback > 0`. Calling `braid_pool_checkin()` on a *different*
connection from within `destroy_fn` is unsupported — it will trigger deferred
work processing while `in_callback > 0` and may corrupt the pool's deferred
work state. `destroy_fn` must not call any libbraid API function. This
restriction is stricter than the general hook rule and is stated separately
because `destroy_fn` is the only hook that is called at a point where the
pool is actively in teardown of another connection.

---

## 18. Error Codes

### 18.1 API Return Codes

Returned from public API functions. Zero is always success.

| Code | Value | Meaning |
|---|---|---|
| `BRAID_OK` | 0 | Success |
| `BRAID_ERR_INVAL` | 1 | Invalid argument or programming error — bad fd at checkin, invalid config at create |
| `BRAID_ERR_NOMEM` | 2 | Allocation failure — only possible at `braid_pool_create()` |
| `BRAID_ERR_SHUTDOWN` | 3 | Pool is shutting down — checkout rejected |
| `BRAID_ERR_EXHAUSTED` | 4 | `max_connections` reached and `timeout_ms == 0` — immediate fail, no queuing |
| `BRAID_ERR_SYSCALL` | 5 | Internal syscall failure — `epoll_ctl`, `getsockopt`, etc. Per-connection, not pool-fatal |

`BRAID_ERR_SYSCALL` is per-connection. When an internal syscall fails on a
connection (e.g. `epoll_ctl` fails adding a new fd), that connection is
treated as a connect failure and handed to the reconnection engine.
`braid_pool_advance()` has no pool-fatal return path — all runtime failures
are per-connection and handled internally.

### 18.2 Checkout Callback Error Codes

Passed as the `err` parameter to `braid_checkout_cb`. The fd parameter is
valid only when `err == BRAID_OK`.

| Code | Meaning |
|---|---|
| `BRAID_OK` | Connection ready — fd is valid |
| `BRAID_ERR_TIMEOUT` | Waiter deadline expired before a connection became available |
| `BRAID_ERR_CANCELLED` | Caller called `braid_pool_cancel()` |
| `BRAID_ERR_SHUTDOWN` | Pool destroyed while waiter was queued |
| `BRAID_ERR_CONNFAIL` | All reconnection attempts exhausted — pool permanently below min |

### 18.3 Connection Destroyed Reason Codes

Not yet implemented. The `BRAID_EV_CONN_DESTROYED` event does not carry a
reason field in the current release. Reason code population is deferred to v2.

---

## 19. Open Items

Items deferred from the architecture phase. Each must be resolved before the
relevant implementation phase begins.

| Item | Blocks | Notes |
|---|---|---|
| Pool reset API | v2 | Intentionally deferred; requires full connection drain semantics |

**Confirmed out-of-scope decisions (permanently excluded):**

| Decision | Rationale |
|---|---|
| Internal thread / pool-owns-loop | Incompatible with libstrand and caller event loop architectures |
| Multi-target pool | Adds routing complexity with no benefit at library level; caller creates one pool per target |
| Synchronous blocking checkout | Incompatible with event-driven callers |
| io_uring | Linux-only; would permanently exclude OpenBSD |
| HTTP/2 stream multiplexing | Separate future library; libbraid's one-connection-per-checkout model is intentional |

---

**Document Version**: 1.5  
**Last Updated**: 2026-03-30  
**Source**: Derived from libbraid design sessions, March 2026  
**See Also**: PROJECT.md, TECH_STACK.md, CODING_STANDARDS.md, DEVELOPMENT.md, TESTING.md, REPOSITORY_STRUCTURE.md
