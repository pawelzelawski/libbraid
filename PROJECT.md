# libbraid

## Overview

libbraid is a C11 library for TCP connection pooling at the transport layer.
It manages file descriptor lifecycle, reconnection logic, health checking, and
idle reaping for pools of outbound TCP connections. It has no protocol
knowledge — it does not know what bytes flow over the connections it manages.

The pool does not own a thread or event loop. The caller drives libbraid
explicitly from their own event loop. libbraid registers its internal file
descriptors into the caller's epoll or kqueue instance; the caller routes
events back to libbraid. libbraid never blocks the calling thread.

The design philosophy is the same as the rest of this author's infrastructure
libraries: zero mandatory third-party dependencies, explicit ownership
contracts, first-class OpenBSD support, and code that can be audited,
understood, and trusted.

## Philosophy

### The Problem This Library Solves

Outbound TCP connections fail silently. A server process dies; the client's
socket stays writable; the operating system's keepalive defaults are two hours
on Linux. The client does not discover the connection is dead until it tries
to write and gets a reset — or until a real user request fails. Production
incidents from this failure mode are well documented. They are not exotic edge
cases; they are the normal behaviour of TCP in any environment where connections
are held longer than NAT or firewall timeout windows.

Beyond half-open detection, production connection pools must solve a second
failure mode: the connection storm. A backend recovers after an outage; all
clients detect the recovery simultaneously and reconnect at the same instant;
the backend receives a spike of connection requests and falls over again. The
solution is exponential backoff with full jitter — but only if it is
implemented correctly and consistently. A hand-rolled reconnection loop in
application code is rarely either.

Managing connection lifecycle in application code is unglamorous, repetitive,
and error-prone. Use-after-free, SIGPIPE, dirty protocol state returned to the
pool, and race conditions between the idle reaper and checkout are all
straightforward to introduce and difficult to detect. libbraid encapsulates
this logic once, correctly, with a well-specified state machine and tested
edge cases.

### Primary Goal

The primary goal is **correctness** — a connection pool that handles the full
lifecycle of a TCP connection, including all failure modes, without requiring
the caller to think about them. The caller provides a target address, optional
protocol hooks, and an event loop fd. libbraid handles everything between
`socket()` and the moment an fd is ready for use.

The secondary benefit is **reusability** — a single pool implementation that
works for any application protocol. The caller plugs in protocol-specific
behaviour via hooks (`init_fn` for TLS and authentication, `validate_fn` for
application-level liveness checks, `destroy_fn` for graceful teardown).
libbraid does not need to know whether the connections carry HTTP, Redis,
PostgreSQL, or a custom binary protocol.

### Transport Resource Pool, Not a Protocol Library

libbraid is framed precisely as a transport resource pool. Its scope begins
when it creates a socket and initiates a TCP connect. Its scope ends when it
hands a checked-out fd to the caller. What happens over that fd is entirely
the caller's concern.

This framing is deliberate. Protocol libraries manage protocol state. libbraid
manages fd lifecycle. The two concerns are orthogonal and the boundary between
them is clean. A caller can use libbraid with any protocol library — or with
no protocol library at all.

### Caller-Owns-Loop

The pool does not own a thread, an event loop, or a signal handler. The caller
integrates libbraid into their existing event loop by passing an epoll or
kqueue fd at pool creation. libbraid registers its internal fds into that
instance. The caller calls `braid_pool_advance()` once per loop iteration to
drive timer-based work, and calls `braid_pool_notify()` when an event arrives
on one of libbraid's fds.

This model composes correctly with any C application that has its own event
loop. It is a deliberate design constraint, not a limitation.

### Core Principles

1. **One callback per checkout, always.** Every `braid_pool_checkout()` call
   results in exactly one invocation of the provided callback, regardless of
   outcome — success, timeout, cancellation, or shutdown. The caller never
   needs to track whether a callback fired.

2. **Non-blocking at every operation callsite.** `braid_pool_checkout()`,
   `braid_pool_checkin()`, `braid_pool_cancel()`, `braid_pool_advance()`,
   and `braid_pool_notify()` all return immediately. Work that cannot
   complete synchronously is deferred to `braid_pool_advance()`.
   `braid_pool_destroy()` is explicitly a teardown function and may block
   while waiting for active connections to be returned.

3. **Fixed memory footprint.** All internal data structures are allocated once
   at `braid_pool_create()` and sized to `max_connections`. No allocation
   occurs on any subsequent operation. The pool's memory footprint is
   deterministic from the moment of creation.

4. **Explicit ownership.** When a connection is checked out, the caller owns
   the fd exclusively until checkin. libbraid makes no access to the fd while
   it is checked out. The ownership boundary is a hard contract stated
   precisely in the API.

5. **Hooks are optional.** Every hook is NULL-safe. A pool with no hooks
   configured is fully functional — it manages fd lifecycle with no protocol
   participation and no overhead for unconfigured hooks.

6. **Errors are returned, never fatal.** Internal errors are returned as error
   codes. The library does not call `abort()` or `exit()` after successful
   initialisation. All runtime failures are per-connection and handled
   internally by the reconnection engine.

## Internal Components

libbraid is organised around five internal components. They are not separately
usable layers — they form a single cohesive pool implementation — but they
have defined responsibilities and clean interfaces between them.

### Connection Table

An open-addressed hash table keyed on fd. Stores all connection records for
the pool. Allocated at creation to `2 × max_connections` slots — load factor
never exceeds 0.5, collision chains are short. All fd-to-record lookups are
O(1). No allocation after pool creation.

### Connection State Machine

Six states: CONNECTING, INITIALIZING, IDLE, ACTIVE, CLOSING, DEAD. All
transitions go through a single `conn_transition()` function — the only
point in the codebase that writes connection state. Transition legality is
asserted on every call. State-entry invariants (timestamp updates, heap
maintenance) are enforced by `conn_transition()`.

### Reconnection Engine

Manages connections that have reached DEAD and need replacement to maintain
`min_connections`. Uses a min-heap on `next_retry_ms`. Implements full jitter
exponential backoff. DNS is resolved fresh on every connect attempt. Maximum
attempts of zero means retry forever.

### Idle Reaper

Tracks IDLE connections via a min-heap on `last_active_ms`. On each
`braid_pool_advance()` call, reaps connections that have exceeded
`idle_reap_timeout`, subject to the `min_connections` floor. The heap enables
exact `next_ms` computation without scanning the full connection table.

### Wait Queue

A fixed-size ring buffer of pending checkout requests. FIFO order. Entries
carry a callback, an opaque context, and a deadline. Cancellable via an opaque
token. Tombstone mechanism ensures one callback per checkout regardless of
outcome. Expired entries culled on each `braid_pool_advance()` call.

## What libbraid Is For

libbraid is aimed at C applications that maintain outbound TCP connections:

- **Reverse proxies and load balancers** — upstream connection pools for HTTP
  and other protocols. One pool per upstream target per worker.
- **Database clients** — connection pools for PostgreSQL, MySQL, Redis, or any
  server that speaks TCP and benefits from persistent connections.
- **Message broker clients** — AMQP, MQTT, Kafka, and similar protocols where
  persistent connections carry multiplexed traffic.
- **RPC frameworks** — any C application making outbound RPC calls over TCP
  that needs to amortise connection setup cost.
- **Service mesh sidecars and telemetry agents** — programs that maintain
  persistent connections to collection or control endpoints.

libbraid is **not** aimed at:

- HTTP/2 upstream multiplexing. libbraid's one-connection-per-checkout model
  fits HTTP/1.1 naturally. HTTP/2 multiplexes many concurrent streams over one
  connection — that requires a separate stream management layer above libbraid.
- Unix domain sockets, UDP, or any transport other than TCP.
- Programs that need a synchronous blocking checkout API.
- Single-connection use cases where a pool adds no value.

## What libbraid Explicitly Does Not Do

- Does not own a thread, signal handler, or event loop
- Does not perform TLS — TLS negotiation is the caller's responsibility via `init_fn`
- Does not understand any application protocol
- Does not implement flow control, request multiplexing, or stream management
- Does not support Unix domain sockets or UDP
- Does not implement cross-process connection brokering
- Does not expose a synchronous blocking API
- Does not implement pool reset in v1
- Does not have mandatory third-party dependencies
- Does not modify the calling process's signal disposition or block `SIGPIPE` —
  callers writing to checked-out fds must handle `SIGPIPE` themselves (via
  `signal(SIGPIPE, SIG_IGN)` or `MSG_NOSIGNAL`)
- Does not install `atexit` handlers

## Current Status

**Architecture and documentation complete.** Implementation is complete
through Phase 5.

| Phase | Name | Status |
|---|---|---|
| — | Architecture | COMPLETE |
| — | Documentation | COMPLETE |
| 1 | Foundation | COMPLETE |
| 2 | Connection Table | COMPLETE |
| 3 | Connection State Machine | COMPLETE |
| 4 | Wait Queue | COMPLETE |
| 5 | Reconnection Engine and Idle Reaper | COMPLETE |
| 6 | Pool Core | NOT STARTED |
| 7 | OpenBSD (kqueue) port | NOT STARTED |
| 8 | Hardening, benchmarks, and release | NOT STARTED |

## Relationship to Existing Libraries

### Protocol-Specific Pool Implementations

libpq, hiredis, and similar protocol libraries embed connection pools that are
tightly coupled to their protocol. They are not reusable across protocols and
expose no general-purpose pool interface. libbraid is protocol-agnostic — it
manages fd lifecycle and delegates all protocol behaviour to caller-provided
hooks.

### Event Loop Libraries — libuv, libevent, libev

These libraries multiplex I/O events and timers but provide no connection pool
abstraction. They manage fd readiness; they do not manage fd lifecycle,
reconnection, or health checking. libbraid uses the caller's event loop fd
directly and does not require or conflict with any of these libraries.

### Abandoned Pool Projects

Several C connection pool projects exist on GitHub
(ipfans/c-connection-pool, lloydzhou/c-connection-pool, searx/connpool and
others). All are synchronous-only, lack health checking, and have not been
maintained since 2017–2018. None target an asynchronous caller-owns-loop
architecture. libbraid fills the gap they left.

### HTTP/2 Upstream Multiplexer (Future)

libbraid's one-connection-per-checkout model does not fit HTTP/2 upstream
pooling, where many concurrent streams share one connection. A future separate
library will address this by sitting above libbraid (for fd lifecycle) and
above a protocol library (for HTTP/2 framing), exposing a per-stream checkout
API. This library depends on libbraid being production-ready and is not in
scope for the current work.

## License

ISC License. Simple, permissive, compatible with OpenBSD philosophy.

## Document Index

| Document | Audience | Purpose |
|---|---|---|
| PROJECT.md | Both | Overview, goals, scope, design philosophy (this file) |
| ARCHITECTURE.md | Implementer | Full internal architecture — data structures, state machine, reconnection engine, idle reaper, epoll abstraction, re-entrancy handling |
| TECH_STACK.md | Implementer | Build system, compiler flags, sanitizer integration, tooling |
| CODING_STANDARDS.md | Implementer | C11 style, naming, error handling, documentation requirements |
| REPOSITORY_STRUCTURE.md | Implementer | Directory layout, file-by-file descriptions, component-to-file mapping |
| DEVELOPMENT.md | Implementer | Phased build plan, milestones, per-phase tasks, test requirements |
| TESTING.md | Implementer primary, reviewer secondary | Test strategy, unit and integration test catalogue, sanitizer testing, platform testing, CI approach |

---

**Document Version**: 1.0
**Last Updated**: 2026-03-31
**Status**: Implementation complete through Phase 4
