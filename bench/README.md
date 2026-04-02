# bench/

Performance benchmarks for libbraid. Built separately under release flags
(`-O2`). Not part of `make test`.

```sh
make bench
```

See TECH_STACK.md §6.3 and TESTING.md §7 for context.

## What each benchmark measures

### bench_checkout — checkout/checkin round-trip latency

Warms a pool to `pool_size` live TCP loopback connections, then times a tight
loop of `braid_pool_checkout() + braid_pool_checkin(BRAID_CONN_OK)`. No new
socket is created per iteration; the same pre-warmed file descriptors are
reused throughout.

The measured path includes two kernel calls per iteration:

- `epoll_ctl(EPOLL_CTL_DEL)` — issued by `io_unwatch()` inside
  `braid_pool_checkout()` before the IDLE→ACTIVE transition
- `epoll_ctl(EPOLL_CTL_ADD)` — issued by `io_watch()` inside
  `braid_pool_checkin()` after the ACTIVE→IDLE transition

These syscalls are the direct cost of the half-open detection architecture:
IDLE connections are watched on epoll so that `recv(MSG_PEEK)` can detect
server-side closes. They cannot be removed without removing half-open
detection. The original < 500 ns design target assumed pure in-memory pool
mechanics and does not apply to this implementation path. See `TESTING.md
§7.3` for the revised target.

### bench_advance — `braid_pool_advance()` call overhead

Three scenarios, each timing a tight loop of `braid_pool_advance()` calls:

- **idle pool**: pool with `max_connections=100`, zero live connections, no
  pending reconnects, empty wait queue. Exercises the minimal advance() path:
  one `clock_gettime()` vDSO call, null checks on all internal structures.
  No kernel calls.
- **full idle pool (100 IDLE connections)**: 100 live loopback connections,
  all IDLE. Adds live slot scanning and a reaper heap peek.
- **mixed pool (50 active / 50 idle)**: common production scenario with a mix
  of checked-out and idle connections.

### bench_reconnect — reconnection engine throughput

Two sub-benchmarks:

- **heap push+pop throughput**: isolated min-heap insert+delete with
  pseudo-random keys. No syscalls; measures pure algorithmic cost.
- **reconnect_advance due-entry processing**: fires `reconnect_advance()` with
  1,000 due entries. Each entry runs a full attempt: `getaddrinfo()`,
  `socket()`, `fcntl()` ×2, `setsockopt()` ×3, `connect()` to a closed port
  (immediate `ECONNREFUSED`), then schedules a retry. The ~27–40 µs per entry
  is the cost of that syscall sequence, not the heap.

### bench_pool_scale — checkout/checkin throughput across pool sizes

Same hot path as bench_checkout, reported as throughput (ops/s) at pool sizes
10, 100, 500, and 1000. Verifies that hash table lookup and reaper heap
operations do not degrade as the pool grows.

## Baselines

Benchmarks were run on Linux x86_64 only. No dedicated ARM64 or OpenBSD
hardware was available for benchmarking. Build quality and all unit and
integration tests are verified on Linux x86_64, Linux ARM64, and OpenBSD
x86_64 via CI, but performance numbers exist only for Linux x86_64.

All runs on AC power with performance CPU governor (`cpupower frequency-set
-g performance` or equivalent). Build flags: `-O2`. Clock shown is as reported
by `/proc/cpuinfo` at the start of each benchmark binary.

### Linux x86_64 — Intel Core i7-7500U @ 2.70 GHz (Turbo 3.5 GHz), 4 cores

Date: 2026-04-02

```
bench_checkout
  checkout with immediate connection (pool size 10):     1918.9 ns/op
  checkout with immediate connection (pool size 100):    1870.9 ns/op
  checkout with immediate connection (pool size 500):    1837.4 ns/op
  checkout with immediate connection (pool size 1000):   1909.9 ns/op

bench_advance  (CPU at 3500 MHz)
  advance() idle pool:                        165.7 ns/op
  advance() full idle pool (100 conns):       229.1 ns/op
  advance() mixed pool (50 active/50 idle):   224.1 ns/op

bench_reconnect  (CPU at 3500 MHz)
  reconnect heap push+pop throughput:     149,079,842 ops/s
  reconnect_advance due-entry processing:      36,413 entries/s

bench_pool_scale  (CPU at 3500 MHz)
  pool scale (max_connections=10):    535,134 checkout+checkin ops/s
  pool scale (max_connections=100):   536,757 checkout+checkin ops/s
  pool scale (max_connections=500):   532,746 checkout+checkin ops/s
  pool scale (max_connections=1000):  513,251 checkout+checkin ops/s
```

### Linux x86_64 — AMD Ryzen 7 4800H @ 2.90 GHz (Turbo 4.2 GHz), 16 cores

Date: 2026-04-02

**Note on clock instability**: `/proc/cpuinfo` reported different frequencies
across the four benchmark binaries (4274 MHz, 2900 MHz, 3285 MHz, 1906 MHz).
The governor was scaling down between runs, likely due to thermal load or idle
periods. The pool_scale result in particular was recorded with the CPU at
1906 MHz. Results should be retaken on a thermally stable setup before being
used for direct comparison.

```
bench_checkout  (CPU at 4274 MHz)
  checkout with immediate connection (pool size 10):     3773.7 ns/op
  checkout with immediate connection (pool size 100):    3788.8 ns/op
  checkout with immediate connection (pool size 500):    3818.4 ns/op
  checkout with immediate connection (pool size 1000):   3829.4 ns/op

bench_advance  (CPU at 2900 MHz)
  advance() idle pool:                        1220.0 ns/op  ← see note
  advance() full idle pool (100 conns):       1313.7 ns/op  ← see note
  advance() mixed pool (50 active/50 idle):   1311.1 ns/op  ← see note

bench_reconnect  (CPU at 3285 MHz)
  reconnect heap push+pop throughput:     150,480,654 ops/s
  reconnect_advance due-entry processing:      40,331 entries/s

bench_pool_scale  (CPU at 1906 MHz)
  pool scale (max_connections=10):    262,434 checkout+checkin ops/s
  pool scale (max_connections=100):   262,758 checkout+checkin ops/s
  pool scale (max_connections=500):   264,102 checkout+checkin ops/s
  pool scale (max_connections=1000):  261,092 checkout+checkin ops/s
```

**advance() result not comparable to Intel**: the idle-pool advance() at
1220 ns was recorded with the CPU at 2900 MHz (base clock), while the Intel
i7-7500U result of 165 ns was recorded at 3500 MHz (turbo). The four benchmark
binaries are independent processes; on the Ryzen 4800H, the performance
governor does not hold boost frequency between binary invocations — each
binary catches the CPU at whatever frequency it happens to be at that instant.
The heap push+pop throughput (~150M ops/s) is comparable between both machines,
confirming there is no general computation penalty. The advance() numbers for
Ryzen are informational only; the Intel i7-7500U results are the reference
baseline. To obtain stable Ryzen advance() numbers, pin frequency explicitly
before running (`cpupower frequency-set -f <MHz>`) or add a warmup loop before
each binary invocation.
