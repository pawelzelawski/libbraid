# Tech Stack

## 1. Language

### C (C11)

libbraid is written in C11. No C++, no scripting languages, no code generation.

**Standard**: C11 (`-std=c11`)

**Why C11 over C99**:
- `_Static_assert` for compile-time checks on struct sizes and platform
  assumptions — for example, verifying that `braid_token_t` is the expected
  width and that hash table slot counts fit in the chosen integer types
- `_Alignas` / `_Alignof` for cache-line alignment of hot pool structures
- Designated initialisers for config struct defaults and static tables
- Anonymous structs and unions in the observability event type

**Feature test macros** (defined in Makefile, not in source files):
```c
_POSIX_C_SOURCE=200809L   /* POSIX.1-2008 */
_XOPEN_SOURCE=700         /* XSI extensions */
```

Do not define `_GNU_SOURCE` — it pulls in non-portable extensions and
breaks OpenBSD builds.

---

## 2. External Dependencies

libbraid has **zero external library dependencies**. This is a hard
requirement, not a preference.

The only dependencies are the C standard library and POSIX interfaces
available on every supported platform without installation. There are no
vendored libraries, no submodules, and no package manager files.

**Runtime dependencies for embedders**:
- Linux: none beyond libc
- OpenBSD: none beyond libc

---

## 3. System Libraries

These are part of the C standard library or POSIX and require no
installation.

### 3.1 Standard C Library

```c
#include <stdint.h>     /* uint8_t, uint32_t, uint64_t, size_t         */
#include <stddef.h>     /* offsetof, NULL, size_t                       */
#include <string.h>     /* memcpy, memset                               */
#include <stdlib.h>     /* malloc, free — pool-level allocation at init */
#include <errno.h>      /* errno — syscall error inspection             */
#include <assert.h>     /* assert — debug builds only                   */
```

### 3.2 Clocks

```c
#include <time.h>       /* clock_gettime(CLOCK_MONOTONIC) */
```

`clock_gettime(CLOCK_MONOTONIC)` is used exclusively for all internal time
comparisons: reconnection backoff deadlines, idle reap thresholds, checkout
wait timeouts, and hook deadlines passed to `init_fn` and `validate_fn`.
`CLOCK_REALTIME` is never used. All internal timestamps are `uint64_t`
milliseconds.

`clock_gettime()` is called once per `braid_pool_advance()` invocation. The
result is reused for all comparisons within that call. It is not called on
the checkout, checkin, or notify paths.

### 3.3 Networking and DNS

```c
#include <sys/types.h>  /* prerequisite for socket headers              */
#include <sys/socket.h> /* socket, connect, getsockopt, setsockopt,
                           SO_KEEPALIVE, SO_ERROR                        */
#include <netinet/in.h> /* sockaddr_in, sockaddr_in6, IPPROTO_TCP       */
#include <netinet/tcp.h>/* TCP_KEEPIDLE (Linux) / TCP_KEEPALIVE (OpenBSD),
                           TCP_KEEPINTVL, TCP_KEEPCNT                    */
#include <netdb.h>      /* getaddrinfo, freeaddrinfo, struct addrinfo   */
#include <unistd.h>     /* close                                        */
#include <fcntl.h>      /* fcntl, O_NONBLOCK, O_CLOEXEC                */
```

All sockets are created with a plain `socket()` call. `O_CLOEXEC` is then
set via `fcntl(fd, F_SETFD, FD_CLOEXEC)` and `O_NONBLOCK` via
`fcntl(fd, F_SETFL, O_NONBLOCK)` immediately after creation. Using
`SOCK_NONBLOCK | SOCK_CLOEXEC` flags in the `socket()` call is Linux-specific
and does not compile on OpenBSD. `fcntl()` is the portable path and is
mandated for all socket creation in `braid_conn.c`. Non-blocking `connect()`
returns `EINPROGRESS`; completion is detected via epoll/kqueue writability
event followed by `getsockopt(SO_ERROR)`.

**Keepalive constant portability:** `TCP_KEEPIDLE` is Linux-specific. OpenBSD
uses `TCP_KEEPALIVE` for the per-socket idle timeout override (same semantics,
different constant name). `TCP_KEEPINTVL` and `TCP_KEEPCNT` exist on both
platforms. `conn_keepalive_configure()` in `braid_conn.c` uses
`#ifdef __linux__` to select the correct constant. A second `#ifdef __linux__`
guard in `braid_pool.c` selects between `getentropy()` (Linux) and
`arc4random_buf()` (OpenBSD/BSD) for PRNG seeding. These are the only two
platform `#ifdef` guards permitted outside the I/O abstraction translation units.

DNS resolution uses `getaddrinfo()` called synchronously in the reconnection
engine immediately before each connect attempt. Resolution is not cached —
DNS changes are picked up automatically on reconnection. `getaddrinfo()` is
a blocking call; the reconnection engine is invoked from `braid_pool_advance()`
and the blocking duration is bounded by the system resolver timeout. This is
a documented behaviour — callers with strict latency requirements on
`braid_pool_advance()` should configure a local resolver with low timeouts.

### 3.4 I/O Multiplexing — Linux (v1)

```c
#include <sys/epoll.h>  /* epoll_ctl, epoll_wait — epoll abstraction layer */
#include <unistd.h>     /* close                                           */
```

libbraid uses the caller's epoll instance, passed as `event_fd` in
`braid_config_t`. It does not create its own epoll instance.

All fds are registered with `EPOLLET` (edge-triggered). libbraid uses
`epoll_data.ptr` to carry the `braid_fd_tag_t` sentinel struct, enabling
O(1) caller-side event routing without a separate fd lookup.

### 3.5 I/O Multiplexing — OpenBSD

```c
#include <sys/event.h>  /* kqueue, kevent — epoll abstraction layer */
#include <unistd.h>     /* close                                     */
```

libbraid uses the caller's kqueue instance, passed as `event_fd` in
`braid_config_t`. The kqueue translation unit maps libbraid's internal
`BRAID_IO_READ` / `BRAID_IO_WRITE` event flags to `EVFILT_READ` /
`EVFILT_WRITE` filter additions and deletions via `kevent()`. The `udata`
field of `struct kevent` carries the `braid_fd_tag_t` sentinel pointer,
matching the role of `epoll_data.ptr` on Linux.

FreeBSD and NetBSD use the same kqueue translation unit without modification.

---

## 4. Compiler

### 4.1 Primary Compiler — Clang

**Minimum version**: Clang 11.0

Clang is the primary and required compiler. It is used for all builds,
including sanitizer builds (ASan, UBSan, TSan). clang-tidy static analysis
requires a Clang installation.

### 4.2 Secondary Compiler — GCC

**Minimum version**: GCC 10.0

GCC is supported as a secondary compiler for the C translation units. GCC
builds are validated on Linux only. TSan is not run under GCC — the TSan
build target requires Clang.

### 4.3 Compiler Flags

**Release build** (`make release`):
```makefile
CFLAGS_RELEASE = -std=c11 -O2 -Wall -Wextra -Wpedantic \
                 -Wno-unused-parameter                    \
                 -D_POSIX_C_SOURCE=200809L                \
                 -D_XOPEN_SOURCE=700
```

**Development build** (`make dev`, default):
```makefile
CFLAGS_DEV = -std=c11 -O0 -g3 -Wall -Wextra -Wpedantic  \
             -Wno-unused-parameter                         \
             -Werror                                       \
             -fsanitize=address,undefined                  \
             -fno-omit-frame-pointer                       \
             -D_POSIX_C_SOURCE=200809L                     \
             -D_XOPEN_SOURCE=700                           \
             -DBRAID_DEBUG
```

**TSan build** (`make test-tsan`):
```makefile
CFLAGS_TSAN = -std=c11 -O1 -g3 -Wall -Wextra -Wpedantic  \
              -Wno-unused-parameter                         \
              -fsanitize=thread                             \
              -fno-omit-frame-pointer                       \
              -D_POSIX_C_SOURCE=200809L                     \
              -D_XOPEN_SOURCE=700                           \
              -DBRAID_DEBUG
```

`BRAID_DEBUG` enables:
- Transition legality assertions in `conn_transition()` with diagnostic
  messages identifying source state, target state, and fd
- Tombstone density warnings on the connection table
- Wait queue ring buffer bounds assertions

`-Werror` is enabled only in development builds. Release builds do not treat
warnings as errors — this prevents release breakage from compiler version
differences between platforms.

---

## 5. Build System

### 5.1 Make

libbraid uses a single POSIX-compatible `Makefile`. No CMake, no Meson, no
Autoconf. The build system must be auditable, portable, and require nothing
beyond `make` and a C11 compiler.

**Primary targets**:

```makefile
make          # equivalent to make dev
make dev      # debug build with ASan/UBSan, runs tests
make release  # optimised build, no sanitizers
make test     # run test suite (dev build)
make test-tsan# run test suite under TSan (Clang only)
make valgrind # run test suite under Valgrind (Linux only)
make lint     # clang-tidy + cppcheck
make format   # clang-format -i on all source files
make clean    # remove build artefacts
```

**Platform detection**:

```makefile
OS := $(shell uname -s)
ifeq ($(OS),Linux)
    PLATFORM_SRCS = src/braid_io_epoll.c
endif
ifeq ($(OS),OpenBSD)
    PLATFORM_SRCS = src/braid_io_kqueue.c
endif
```

The correct I/O abstraction translation unit is selected automatically. No
manual platform flag is required.

### 5.2 Build Outputs

```
build/libbraid.a        — static library (primary artefact)
build/libbraid.so       — shared library (optional target)
build/tests/run_tests   — test binary
build/bench/*           — benchmark binaries (built separately)
```

The static library is the primary artefact. Embedders link against
`libbraid.a` and include `include/braid.h`.

---

## 6. Test Infrastructure

### 6.1 Test Harness

libbraid uses a minimal hand-written test harness — no external test
framework. The harness is a single header `tests/test_harness.h` providing
`CHECK(name, expr)` and `CHECK_ERR(name, call, expected_errno)` macros.

```c
#define CHECK(name, expr) do {                                      \
    int _r = (expr) ? 0 : 1;                                        \
    if (_r == 0) { tests_passed++; }                                \
    else { fprintf(stderr, "FAIL: %s\n", (name)); tests_failed++; } \
} while (0)
```

Each test function returns 0 on success and non-zero on failure. The test
binary exits with code 0 if all tests pass and 1 if any test fails.

```sh
make test
# output:
# 47/47 tests passed
```

### 6.2 Test Files

Tests are organised by component in `tests/`:

```
tests/
├── test_harness.h
├── run_tests.c           ← test binary entry point, runs all suites
├── test_table.c          ← connection table: hash, insert, delete, probe chains
├── test_state_machine.c  ← conn_transition: legal transitions, illegal rejection
├── test_wait_queue.c     ← ring buffer: enqueue, dequeue, cancel, expiry
├── test_reconnect.c      ← reconnection heap and backoff algorithm
├── test_reaper.c         ← idle reaper heap and reap logic
├── test_pool.c           ← full pool lifecycle, checkout/checkin, exhaustion
└── test_integration.c    ← integration tests with a real loopback TCP server
```

See TESTING.md for the full test catalogue.

### 6.3 Benchmark Files

Performance benchmarks are in `bench/` and built separately under release
flags. They do not run as part of `make test`.

```
bench/
├── bench_checkout.c      ← checkout/checkin round-trip latency
├── bench_advance.c       ← braid_pool_advance() overhead at varying pool sizes
├── bench_reconnect.c     ← reconnection engine throughput
└── bench_pool_scale.c    ← pool behaviour under varying max_connections
```

Benchmark output includes hardware context (CPU model, core count, clock
speed). Numbers are not comparable across machines without this context.

---

## 7. Development Tools

### 7.1 Valgrind (Linux only)

**Purpose**: Memory error detection — leaks, use-after-free, uninitialised
reads.

**Installation**: `apt install valgrind`

**Usage**:
```sh
make valgrind
# equivalent to:
valgrind --leak-check=full          \
         --show-leak-kinds=all      \
         --track-origins=yes        \
         --error-exitcode=1         \
         ./build/tests/run_tests
```

libbraid has no fiber stacks — no Valgrind stack registration is required.
All memory is heap-allocated at `braid_pool_create()` and freed at
`braid_pool_destroy()`. All tests must pass Valgrind clean. Run on Linux
before every commit. Valgrind is not available on OpenBSD — use the ASan
build there.

### 7.2 AddressSanitizer + UndefinedBehaviorSanitizer

**Purpose**: Runtime memory and undefined behaviour detection.

**Compiler flags**: `-fsanitize=address,undefined` (included in `make dev`)

Available on both Linux and OpenBSD with Clang.

libbraid has no context switches and no custom stack management — no ASan
fiber hook integration is required. ASan operates without modification.

All tests must pass ASan/UBSan clean on both Linux and OpenBSD.

### 7.3 ThreadSanitizer

**Purpose**: Data race detection.

TSan and ASan are mutually exclusive — TSan runs as a separate target.

```sh
make test-tsan
```

libbraid's single-pool-per-thread design means TSan is primarily useful for
validating that no global or shared state is accidentally accessed across
pool instances in multi-pool integration tests. No TSan fiber integration
is required.

TSan is not a per-commit gate. Run before release.

**TSan availability**: Clang only. The `make test-tsan` target fails
gracefully when built with GCC.

### 7.4 clang-tidy

**Purpose**: Static analysis.

**Installation**: `apt install clang-tidy` (Linux) — included with Clang on
OpenBSD.

```sh
make lint
# or directly:
clang-tidy src/*.c -- $(CFLAGS_DEV) -I include/
```

### 7.5 cppcheck

**Purpose**: Additional static analysis, complementary to clang-tidy.

**Installation**: `apt install cppcheck` / `pkg_add cppcheck`

```sh
cppcheck --enable=all --error-exitcode=1 \
         --suppress=missingIncludeSystem \
         src/
```

### 7.6 clang-format

**Purpose**: Consistent code formatting.

**Configuration**: `.clang-format` in repository root. KNF-based style,
consistent with the author's other C libraries.

```sh
make format
# equivalent to:
clang-format -i src/*.c src/*.h include/braid.h
```

CI rejects commits that are not clang-format clean.

---

## 8. Dependency Summary

| Dependency | Version | Type | Purpose | Platforms |
|---|---|---|---|---|
| Clang | ≥ 11.0 | Build tool | Compilation, sanitizers | Linux, OpenBSD |
| GCC | ≥ 10.0 | Build tool | Secondary compiler (C only, no TSan) | Linux |
| libc | system | System lib | Standard C, POSIX networking, DNS | Linux, OpenBSD |
| Valgrind | latest | Dev tool | Memory checking | Linux only |
| clang-tidy | ≥ 11.0 | Dev tool | Static analysis | Linux, OpenBSD |
| cppcheck | latest | Dev tool | Static analysis | Linux, OpenBSD |

**Runtime dependencies for embedders**:
- Linux: libc only
- OpenBSD: libc only

**Development-only dependencies** (not required to embed the library):
- Valgrind (Linux only)
- clang-tidy, cppcheck

---

**Document Version**: 1.0
**Last Updated**: 2026-03-30
**See Also**: PROJECT.md, ARCHITECTURE.md, CODING_STANDARDS.md, DEVELOPMENT.md
