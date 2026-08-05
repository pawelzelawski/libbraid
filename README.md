# libbraid

A C11 TCP connection pool for event-driven servers.

libbraid manages a pool of persistent TCP connections to a single upstream
host. It handles reconnection with full-jitter exponential backoff, idle
connection reaping, per-connection lifecycle hooks (TLS/auth setup,
validation, teardown), and a wait queue for callers that arrive when the
pool is fully busy. The library has zero external dependencies and plugs
directly into the caller's existing epoll (Linux) or kqueue (OpenBSD) loop.

---

## Requirements

- C11 compiler (clang ≥ 7 or gcc ≥ 8)
- POSIX.1-2008 libc
- Linux 2.6.27+ or OpenBSD 6.8+

No other libraries are required.

---

## Build and install

```sh
make
make test          # runs the full test suite (requires clang)
make install       # installs to /usr/local by default
```

To install to a custom prefix:

```sh
make install PREFIX=/opt/myapp
```

This installs `include/braid.h` and `lib/libbraid.a`.

---

## Integration example

The pool is driven by the caller's event loop. Call `braid_pool_advance()`
once per iteration before blocking, then call `braid_pool_notify()` for each
event whose tag matches `BRAID_FD_MAGIC`.

```c
#include <stdint.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <unistd.h>
#include "braid.h"

#define MAX_EVENTS 64

static void on_checkout(int fd, void *conn_ctx, int err, void *cb_ctx)
{
    (void)conn_ctx;
    braid_pool_t *pool = cb_ctx;

    if (err != BRAID_OK) {
        fprintf(stderr, "checkout failed: %d\n", err);
        return;
    }

    /* Use fd for one request, then return it to the pool. */
    braid_pool_checkin(pool, fd, BRAID_CONN_OK);
}

int main(void)
{
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0)
        return 1;

    braid_config_t cfg = {0};
    cfg.host            = "127.0.0.1";
    cfg.port            = 6379;
    cfg.event_fd        = epfd;
    cfg.min_connections = 2;
    cfg.max_connections = 16;
    /* All timeout and keepalive fields are optional; zero uses the defaults. */

    int err;
    braid_pool_t *pool = braid_pool_create(&cfg, &err);
    if (!pool) {
        fprintf(stderr, "braid_pool_create: %d\n", err);
        return 1;
    }

    struct epoll_event events[MAX_EVENTS];

    for (;;) {
        uint32_t next_ms;
        braid_pool_advance(pool, &next_ms);
        int timeout = (next_ms == UINT32_MAX) ? -1 : (int)next_ms;

        int n = epoll_wait(epfd, events, MAX_EVENTS, timeout);
        for (int i = 0; i < n; i++) {
            braid_fd_tag_t *tag = events[i].data.ptr;
            if (tag == NULL || tag->magic != BRAID_FD_MAGIC)
                continue; /* not a libbraid fd — handle it yourself */
            braid_pool_notify(pool, tag->fd, events[i].events);
        }

        /* Acquire a connection; cb fires immediately if one is idle. */
        braid_pool_checkout(pool, 500, on_checkout, pool, NULL);
    }

    braid_pool_destroy(pool, 1000);
    close(epfd);
    return 0;
}
```

On OpenBSD, replace `epoll_create1`, `epoll_wait`, and the `epoll_event`
struct with the equivalent `kqueue` / `kevent` calls; the libbraid API is
identical on both platforms.

---

## Capacity and checkout behavior

`min_connections` is the pool's warm baseline; `max_connections` is its hard
limit. When a checkout with a non-zero `timeout_ms` cannot be served
immediately, libbraid queues it and creates capacity lazily up to
`max_connections`. Connections created for a burst are later eligible for
normal idle reaping back toward the warm baseline.

A checkout with `timeout_ms == 0` never creates capacity: it returns
`BRAID_ERR_EXHAUSTED` when no IDLE connection is immediately available. A
queued checkout may receive `BRAID_ERR_CONNFAIL` if a finite reconnection
budget is exhausted and no live or scheduled connection can serve it.

---

## Known limitations

- **Synchronous DNS resolution.** `braid_pool_advance()` calls
  `getaddrinfo()` during reconnection attempts. If the upstream hostname
  requires a non-trivial DNS lookup this will block the event loop briefly.
  Use a numeric IP or a `/etc/hosts` entry to avoid this.

- **Cooperative event loop.** libbraid does not create threads. The caller
  must call `braid_pool_advance()` and `braid_pool_notify()` from a single
  thread. All callbacks fire synchronously on that thread.

- **One connection per checkout.** Each checkout grants exclusive ownership
  of one TCP connection. This model suits request/response protocols
  (Redis, Memcached, database wire protocols) but is not suited for HTTP/2
  upstream multiplexing where multiple inflight requests share one fd.

---

## Further reading

See [ARCHITECTURE.md](ARCHITECTURE.md) for the full internal design: hash
table layout, connection state machine, reconnection backoff algorithm,
idle reaper, wait queue, and epoll/kqueue abstraction layer.

---

## License

[ISC](LICENSE)
