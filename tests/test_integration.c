/*
 * test_integration.c — end-to-end integration tests with real TCP sockets
 *
 * Phase 8.1 initial slice:
 *   - full connect -> checkout -> checkin -> reuse cycle
 *   - warm pool reaches min_connections
 *
 * Tests use a real loopback TCP server in a forked child process and a real
 * event fd (epoll on Linux, kqueue on BSD). The pool is driven by repeatedly
 * calling braid_pool_advance() and routing readiness events into
 * braid_pool_notify().
 */

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __linux__
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/event.h>
#endif

#include "../include/braid.h"
#include "../src/braid_internal.h"
#include "../src/braid_reconnect.h"
#include "test_harness.h"

typedef struct {
	int calls;
	int fd;
	int err;
	void *conn_ctx;
} checkout_rec_t;

typedef struct {
	braid_event_type_t types[128];
	int fds[128];
	int count;
} event_rec_t;

typedef struct {
	int calls;
} validate_ctx_t;

typedef struct {
	int calls;
} init_ctx_t;

typedef struct {
	int calls;
	int null_ctx_calls;
	int write_ok_calls;
} destroy_ctx_t;

typedef struct {
	int seq;
	int timeout_event_seq;
	int timeout_cb_seq;
	int timeout_cb_err;
	int timeout_cb_calls;
} timeout_order_t;

static uint32_t pool_idle_count(braid_pool_t *pool);
static void stop_test_server(pid_t pid);

static void
checkout_cb(int fd, void *conn_ctx, int err, void *cb_ctx)
{
	checkout_rec_t *rec = cb_ctx;

	rec->calls++;
	rec->fd = fd;
	rec->err = err;
	rec->conn_ctx = conn_ctx;
}

static void
observe_cb(const braid_event_t *ev, void *cb_ctx)
{
	event_rec_t *rec = cb_ctx;

	if (rec->count < (int)(sizeof(rec->types) / sizeof(rec->types[0]))) {
		rec->types[rec->count] = ev->type;
		rec->fds[rec->count] = ev->fd;
	}
	rec->count++;
}

static void
observe_timeout_order_cb(const braid_event_t *ev, void *cb_ctx)
{
	timeout_order_t *ord = cb_ctx;

	ord->seq++;
	if (ev->type == BRAID_EV_CHECKOUT_TIMEOUT &&
	    ord->timeout_event_seq == 0)
		ord->timeout_event_seq = ord->seq;
}

static void
checkout_timeout_order_cb(int fd, void *conn_ctx, int err, void *cb_ctx)
{
	timeout_order_t *ord = cb_ctx;

	(void)fd;
	(void)conn_ctx;
	ord->seq++;
	ord->timeout_cb_calls++;
	ord->timeout_cb_seq = ord->seq;
	ord->timeout_cb_err = err;
}

static int
reconnect_force_socket_create_fail(braid_pool_t *pool, struct addrinfo *ai,
				   int *fd_out, int *immediate_out)
{
	(void)pool;
	(void)ai;
	(void)fd_out;
	(void)immediate_out;
	return BRAID_ERR_SYSCALL;
}

static int
wait_fd_ready(int fd, int want_write, int timeout_ms)
{
	fd_set fds;
	struct timeval tv;
	int rc;

	for (;;) {
		FD_ZERO(&fds);
		FD_SET(fd, &fds);
		tv.tv_sec = timeout_ms / 1000;
		tv.tv_usec = (timeout_ms % 1000) * 1000;

		rc = select(fd + 1, want_write ? NULL : &fds,
			    want_write ? &fds : NULL, NULL, &tv);
		if (rc > 0)
			return BRAID_OK;
		if (rc == 0)
			return BRAID_ERR_TIMEOUT;
		if (errno == EINTR)
			continue;
		return BRAID_ERR_SYSCALL;
	}
}

static int
send_exact_retry(int fd, const void *buf, size_t n, int max_spins)
{
	const uint8_t *p = buf;
	size_t off = 0;
	int spins = 0;
	int flags = 0;

#ifdef MSG_NOSIGNAL
	flags = MSG_NOSIGNAL;
#endif

	while (off < n && spins < max_spins) {
		ssize_t w = send(fd, p + off, n - off, flags);

		if (w > 0) {
			off += (size_t)w;
			continue;
		}
		if (w == 0)
			break;
		if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
			if (spins++ >= max_spins)
				return BRAID_ERR_TIMEOUT;
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				int ready = wait_fd_ready(fd, 1, 1);

				if (ready == BRAID_ERR_SYSCALL)
					return BRAID_ERR_SYSCALL;
			}
			continue;
		}
		return BRAID_ERR_SYSCALL;
	}

	return (off == n) ? BRAID_OK : BRAID_ERR_TIMEOUT;
}

static int
recv_exact_retry(int fd, void *buf, size_t n, int max_spins)
{
	uint8_t *p = buf;
	size_t off = 0;
	int spins = 0;

	while (off < n && spins < max_spins) {
		ssize_t r = recv(fd, p + off, n - off, 0);

		if (r > 0) {
			off += (size_t)r;
			continue;
		}
		if (r == 0)
			break;
		if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
			if (spins++ >= max_spins)
				return BRAID_ERR_TIMEOUT;
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				int ready = wait_fd_ready(fd, 0, 1);

				if (ready == BRAID_ERR_SYSCALL)
					return BRAID_ERR_SYSCALL;
			}
			continue;
		}
		return BRAID_ERR_SYSCALL;
	}

	return (off == n) ? BRAID_OK : BRAID_ERR_TIMEOUT;
}

static int
validate_pingpong_cb(int fd, void *conn_ctx, void *hook_ctx,
		     uint64_t deadline_ms)
{
	validate_ctx_t *ctx = hook_ctx;
	char pong[4];

	(void)conn_ctx;
	(void)deadline_ms;
	ctx->calls++;

	if (send_exact_retry(fd, "PING", 4, 2000) != BRAID_OK)
		return BRAID_ERR_CONNFAIL;
	if (recv_exact_retry(fd, pong, 4, 2000) != BRAID_OK)
		return BRAID_ERR_CONNFAIL;
	if (memcmp(pong, "PONG", 4) != 0)
		return BRAID_ERR_CONNFAIL;

	return BRAID_OK;
}

static int
validate_timeout_cb(int fd, void *conn_ctx, void *hook_ctx,
		    uint64_t deadline_ms)
{
	validate_ctx_t *ctx = hook_ctx;

	(void)fd;
	(void)conn_ctx;
	ctx->calls++;

#ifdef BRAID_TEST_CLOCK
	braid_test_clock_ms = deadline_ms + 1;
#endif

	return BRAID_OK;
}

static int
init_handshake_cb(int fd, void **conn_ctx_out, void *hook_ctx,
		  uint64_t deadline_ms)
{
	init_ctx_t *ctx = hook_ctx;
	char ok[4];

	(void)deadline_ms;
	ctx->calls++;

	if (send_exact_retry(fd, "HELO", 4, 2000) != BRAID_OK)
		return BRAID_ERR_CONNFAIL;
	if (recv_exact_retry(fd, ok, 4, 2000) != BRAID_OK)
		return BRAID_ERR_CONNFAIL;
	if (memcmp(ok, "OKAY", 4) != 0)
		return BRAID_ERR_CONNFAIL;

	*conn_ctx_out = hook_ctx;
	return BRAID_OK;
}

static void
destroy_graceful_cb(int fd, void *conn_ctx, void *hook_ctx)
{
	destroy_ctx_t *ctx = hook_ctx;

	(void)conn_ctx;
	ctx->calls++;
	if (send_exact_retry(fd, "BYE!", 4, 2000) == BRAID_OK)
		ctx->write_ok_calls++;
}

static void
destroy_unknown_ctx_cb(int fd, void *conn_ctx, void *hook_ctx)
{
	destroy_ctx_t *ctx = hook_ctx;

	(void)fd;
	ctx->calls++;
	if (conn_ctx == NULL)
		ctx->null_ctx_calls++;
}

static int
read_exact(int fd, void *buf, size_t n)
{
	uint8_t *p = buf;
	size_t off = 0;

	while (off < n) {
		ssize_t r = read(fd, p + off, n - off);

		if (r == 0)
			return BRAID_ERR_SYSCALL;
		if (r < 0) {
			if (errno == EINTR)
				continue;
			return BRAID_ERR_SYSCALL;
		}
		off += (size_t)r;
	}

	return BRAID_OK;
}

static int
start_test_server_bind(pid_t *pid_out, uint16_t bind_port, uint16_t *port_out)
{
	int pipefd[2];
	pid_t pid;

	if (pipe(pipefd) != 0)
		return BRAID_ERR_SYSCALL;

	pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return BRAID_ERR_SYSCALL;
	}

	if (pid == 0) {
		int one = 1;
		int srvfd;
		int i;
		struct sockaddr_in addr;
		struct sockaddr_in bound;
		socklen_t blen = sizeof(bound);
		uint16_t port;
		int clients[64];
		int nclients = 0;

		close(pipefd[0]);

		srvfd = socket(AF_INET, SOCK_STREAM, 0);
		if (srvfd < 0)
			_exit(10);

		if (setsockopt(srvfd, SOL_SOCKET, SO_REUSEADDR, &one,
			       sizeof(one)) != 0)
			_exit(11);

		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		addr.sin_port = htons(bind_port);

		if (bind(srvfd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
			_exit(12);
		if (getsockname(srvfd, (struct sockaddr *)&bound, &blen) != 0)
			_exit(13);
		if (listen(srvfd, 32) != 0)
			_exit(14);

		port = ntohs(bound.sin_port);
		if (write(pipefd[1], &port, sizeof(port)) != sizeof(port))
			_exit(15);
		close(pipefd[1]);
		signal(SIGPIPE, SIG_IGN);

		for (;;) {
			fd_set rfds;
			int maxfd = srvfd;
			int rc;

			FD_ZERO(&rfds);
			FD_SET(srvfd, &rfds);
			for (i = 0; i < nclients; i++) {
				FD_SET(clients[i], &rfds);
				if (clients[i] > maxfd)
					maxfd = clients[i];
			}

			rc = select(maxfd + 1, &rfds, NULL, NULL, NULL);
			if (rc < 0) {
				if (errno == EINTR)
					continue;
				_exit(16);
			}

			if (FD_ISSET(srvfd, &rfds)) {
				int cfd = accept(srvfd, NULL, NULL);

				if (cfd >= 0) {
					if (nclients <
					    (int)(sizeof(clients) /
						  sizeof(clients[0])))
						clients[nclients++] = cfd;
					else
						close(cfd);
				}
			}

			for (i = 0; i < nclients; i++) {
				int cfd = clients[i];
				char buf[16];
				ssize_t r;

				if (!FD_ISSET(cfd, &rfds))
					continue;

				r = read(cfd, buf, sizeof(buf));
				if (r <= 0) {
					close(cfd);
					clients[i] = clients[nclients - 1];
					nclients--;
					i--;
					continue;
				}

				if (r >= 4 && memcmp(buf, "PING", 4) == 0)
					send(cfd, "PONG", 4, 0);
				else if (r >= 4 && memcmp(buf, "HELO", 4) == 0)
					send(cfd, "OKAY", 4, 0);
			}
		}
	}

	close(pipefd[1]);
	if (read_exact(pipefd[0], port_out, sizeof(*port_out)) != BRAID_OK) {
		close(pipefd[0]);
		kill(pid, SIGTERM);
		waitpid(pid, NULL, 0);
		return BRAID_ERR_SYSCALL;
	}
	close(pipefd[0]);

	*pid_out = pid;
	return BRAID_OK;
}

static int
start_test_server(pid_t *pid_out, uint16_t *port_out)
{
	return start_test_server_bind(pid_out, 0, port_out);
}

static int
start_test_server_on_port(pid_t *pid_out, uint16_t port)
{
	uint16_t actual = 0;
	int rc;

	rc = start_test_server_bind(pid_out, port, &actual);
	if (rc != BRAID_OK)
		return rc;
	if (actual != port) {
		stop_test_server(*pid_out);
		*pid_out = -1;
		return BRAID_ERR_SYSCALL;
	}

	return BRAID_OK;
}

static void
stop_test_server(pid_t pid)
{
	if (pid <= 0)
		return;
	kill(pid, SIGTERM);
	waitpid(pid, NULL, 0);
}

static int
event_loop_step(braid_pool_t *pool, int event_fd)
{
	uint32_t next_ms = 0;
	int rc;

	rc = braid_pool_advance(pool, &next_ms);
	if (rc != BRAID_OK)
		return rc;

#ifdef BRAID_TEST_CLOCK
	braid_test_clock_ms += 10;
#endif

#ifdef __linux__
	{
		struct epoll_event events[32];
		int timeout_ms;
		int n;
		int i;

		timeout_ms = (next_ms == UINT32_MAX) ? 10 : (int)next_ms;
		if (timeout_ms > 50)
			timeout_ms = 50;
		if (timeout_ms < 0)
			timeout_ms = 0;

		n = epoll_wait(event_fd, events, 32, timeout_ms);
		if (n < 0) {
			if (errno == EINTR)
				return BRAID_OK;
			return BRAID_ERR_SYSCALL;
		}

		for (i = 0; i < n; i++) {
			braid_fd_tag_t *tag = events[i].data.ptr;

			if (tag == NULL || tag->magic != BRAID_FD_MAGIC)
				continue;
			braid_pool_notify(pool, tag->fd,
					  BRAID_IO_READ | BRAID_IO_WRITE);
		}
	}
#else
	{
		struct kevent events[32];
		struct timespec ts;
		int timeout_ms;
		int n;
		int i;

		timeout_ms = (next_ms == UINT32_MAX) ? 10 : (int)next_ms;
		if (timeout_ms > 50)
			timeout_ms = 50;
		if (timeout_ms < 0)
			timeout_ms = 0;

		ts.tv_sec = timeout_ms / 1000;
		ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;

		n = kevent(event_fd, NULL, 0, events, 32, &ts);
		if (n < 0) {
			if (errno == EINTR)
				return BRAID_OK;
			return BRAID_ERR_SYSCALL;
		}

		for (i = 0; i < n; i++) {
			braid_fd_tag_t *tag = (braid_fd_tag_t *)events[i].udata;

			if (tag == NULL || tag->magic != BRAID_FD_MAGIC)
				continue;
			braid_pool_notify(pool, tag->fd,
					  BRAID_IO_READ | BRAID_IO_WRITE);
		}
	}
#endif

	return BRAID_OK;
}

static int
run_until_idle_at_least(braid_pool_t *pool, int event_fd, uint32_t want,
			int max_steps)
{
	int i;

	for (i = 0; i < max_steps; i++) {
		if (pool_idle_count(pool) >= want)
			return BRAID_OK;
		if (event_loop_step(pool, event_fd) != BRAID_OK)
			return BRAID_ERR_SYSCALL;
	}

	return BRAID_ERR_TIMEOUT;
}

static int
event_count_type(event_rec_t *rec, braid_event_type_t type)
{
	int i;
	int n = 0;

	for (i = 0; i < rec->count; i++)
		if (rec->types[i] == type)
			n++;

	return n;
}

static int
event_first_index_after(event_rec_t *rec, braid_event_type_t type, int after)
{
	int i;

	for (i = after + 1; i < rec->count; i++)
		if (rec->types[i] == type)
			return i;

	return -1;
}

static uint32_t
pool_idle_count(braid_pool_t *pool)
{
	uint32_t i;
	uint32_t count = 0;

	for (i = 0; i < pool->table_size; i++) {
		braid_conn_t *conn = &pool->table[i];

		if (conn->fd == -1 || (conn->flags & CONN_FLAG_TOMBSTONE))
			continue;
		if (conn->state == BRAID_STATE_IDLE)
			count++;
	}

	return count;
}

/*
 * Full connect -> checkout -> checkin -> reuse.
 * Verifies end-to-end lifecycle and fd reuse on the second checkout.
 */
static void
test_full_connect_checkout_checkin_reuse(void)
{
	pid_t server_pid = -1;
	uint16_t port = 0;
	int event_fd = -1;
	braid_pool_t *pool = NULL;
	braid_config_t cfg;
	checkout_rec_t first;
	checkout_rec_t second;
	int err = 0;
	int i;

	memset(&first, 0, sizeof(first));
	memset(&second, 0, sizeof(second));

	if (start_test_server(&server_pid, &port) != BRAID_OK) {
		CHECK("integration-cycle: start server", 0);
		return;
	}

	event_fd = make_event_fd();
	if (event_fd < 0) {
		CHECK("integration-cycle: event fd", 0);
		stop_test_server(server_pid);
		return;
	}

	memset(&cfg, 0, sizeof(cfg));
	cfg.host = "127.0.0.1";
	cfg.port = port;
	cfg.event_fd = event_fd;
	cfg.min_connections = 1;
	cfg.max_connections = 2;
	cfg.connect_timeout = 1000;
	cfg.init_timeout = 1000;
	cfg.validate_timeout = 1000;

	pool = braid_pool_create(&cfg, &err);
	CHECK("integration-cycle: create pool", pool != NULL);
	if (pool == NULL)
		goto cleanup;

#ifdef BRAID_TEST_CLOCK
	braid_test_clock_ms = 0;
#endif

	for (i = 0; i < 500; i++) {
		if (pool_idle_count(pool) >= 1)
			break;
		if (event_loop_step(pool, event_fd) != BRAID_OK)
			break;
	}
	CHECK("integration-cycle: pool warmed to one idle connection",
	      pool_idle_count(pool) >= 1);

	CHECK_ERR("integration-cycle: first checkout request",
		  braid_pool_checkout(pool, 0, checkout_cb, &first, NULL),
		  BRAID_OK);
	CHECK("integration-cycle: first checkout callback fired",
	      first.calls == 1);
	CHECK("integration-cycle: first checkout success",
	      first.err == BRAID_OK);
	CHECK("integration-cycle: first fd valid", first.fd >= 0);

	CHECK_ERR("integration-cycle: first checkin",
		  braid_pool_checkin(pool, first.fd, BRAID_CONN_OK), BRAID_OK);

	CHECK_ERR("integration-cycle: second checkout request",
		  braid_pool_checkout(pool, 0, checkout_cb, &second, NULL),
		  BRAID_OK);
	CHECK("integration-cycle: second checkout callback fired",
	      second.calls == 1);
	CHECK("integration-cycle: second checkout success",
	      second.err == BRAID_OK);
	CHECK("integration-cycle: fd reused after checkin",
	      second.fd == first.fd);

	CHECK_ERR("integration-cycle: second checkin",
		  braid_pool_checkin(pool, second.fd, BRAID_CONN_OK), BRAID_OK);

cleanup:
	if (pool != NULL)
		braid_pool_destroy(pool, 0);
	if (event_fd >= 0)
		close(event_fd);
	stop_test_server(server_pid);
}

/*
 * Pool exhaustion event is emitted when max_connections is reached and a
 * zero-timeout checkout request cannot be served.
 */
static void
test_pool_exhausted_event_fires(void)
{
	pid_t server_pid = -1;
	uint16_t port = 0;
	int event_fd = -1;
	braid_pool_t *pool = NULL;
	braid_config_t cfg;
	checkout_rec_t active;
	event_rec_t events;
	int err = 0;

	memset(&active, 0, sizeof(active));
	memset(&events, 0, sizeof(events));

	if (start_test_server(&server_pid, &port) != BRAID_OK) {
		CHECK("integration-exhausted: start server", 0);
		return;
	}

	event_fd = make_event_fd();
	if (event_fd < 0) {
		CHECK("integration-exhausted: event fd", 0);
		stop_test_server(server_pid);
		return;
	}

	memset(&cfg, 0, sizeof(cfg));
	cfg.host = "127.0.0.1";
	cfg.port = port;
	cfg.event_fd = event_fd;
	cfg.min_connections = 1;
	cfg.max_connections = 1;
	cfg.observe_fn = observe_cb;
	cfg.hook_context = &events;

	pool = braid_pool_create(&cfg, &err);
	CHECK("integration-exhausted: create pool", pool != NULL);
	if (pool == NULL)
		goto cleanup;

#ifdef BRAID_TEST_CLOCK
	braid_test_clock_ms = 0;
#endif

	CHECK_ERR("integration-exhausted: warm pool",
		  run_until_idle_at_least(pool, event_fd, 1, 500), BRAID_OK);
	events.count = 0;

	CHECK_ERR("integration-exhausted: first checkout",
		  braid_pool_checkout(pool, 0, checkout_cb, &active, NULL),
		  BRAID_OK);
	CHECK("integration-exhausted: first callback fired", active.calls == 1);

	CHECK_ERR("integration-exhausted: zero-timeout checkout exhausted",
		  braid_pool_checkout(pool, 0, checkout_cb, &active, NULL),
		  BRAID_ERR_EXHAUSTED);
	CHECK("integration-exhausted: POOL_EXHAUSTED event fired",
	      event_count_type(&events, BRAID_EV_POOL_EXHAUSTED) >= 1);

	CHECK_ERR("integration-exhausted: checkin active",
		  braid_pool_checkin(pool, active.fd, BRAID_CONN_OK), BRAID_OK);

cleanup:
#ifdef BRAID_TEST_CLOCK
	reconnect_test_set_socket_create_hook(NULL);
#endif
	if (pool != NULL)
		braid_pool_destroy(pool, 0);
	if (event_fd >= 0)
		close(event_fd);
	stop_test_server(server_pid);
}

/*
 * With max_connections=1, a second checkout waits and is served when the
 * first active connection is checked in.
 */
static void
test_single_connection_concurrent_checkouts(void)
{
	pid_t server_pid = -1;
	uint16_t port = 0;
	int event_fd = -1;
	braid_pool_t *pool = NULL;
	braid_config_t cfg;
	checkout_rec_t c1;
	checkout_rec_t c2;
	int err = 0;
	int i;

	memset(&c1, 0, sizeof(c1));
	memset(&c2, 0, sizeof(c2));

	if (start_test_server(&server_pid, &port) != BRAID_OK) {
		CHECK("integration-concurrent1: start server", 0);
		return;
	}

	event_fd = make_event_fd();
	if (event_fd < 0) {
		CHECK("integration-concurrent1: event fd", 0);
		stop_test_server(server_pid);
		return;
	}

	memset(&cfg, 0, sizeof(cfg));
	cfg.host = "127.0.0.1";
	cfg.port = port;
	cfg.event_fd = event_fd;
	cfg.min_connections = 1;
	cfg.max_connections = 1;

	pool = braid_pool_create(&cfg, &err);
	CHECK("integration-concurrent1: create pool", pool != NULL);
	if (pool == NULL)
		goto cleanup;

#ifdef BRAID_TEST_CLOCK
	braid_test_clock_ms = 0;
#endif

	CHECK_ERR("integration-concurrent1: warm pool",
		  run_until_idle_at_least(pool, event_fd, 1, 500), BRAID_OK);

	CHECK_ERR("integration-concurrent1: checkout first",
		  braid_pool_checkout(pool, 1000, checkout_cb, &c1, NULL),
		  BRAID_OK);
	CHECK("integration-concurrent1: first callback fired", c1.calls == 1);
	CHECK("integration-concurrent1: first checkout success",
	      c1.err == BRAID_OK);

	CHECK_ERR("integration-concurrent1: checkout second enqueued",
		  braid_pool_checkout(pool, 1000, checkout_cb, &c2, NULL),
		  BRAID_OK);
	CHECK("integration-concurrent1: second callback not yet fired",
	      c2.calls == 0);

	CHECK_ERR("integration-concurrent1: checkin first",
		  braid_pool_checkin(pool, c1.fd, BRAID_CONN_OK), BRAID_OK);

	for (i = 0; i < 200 && c2.calls == 0; i++)
		event_loop_step(pool, event_fd);

	CHECK("integration-concurrent1: second callback fired", c2.calls == 1);
	CHECK("integration-concurrent1: second checkout success",
	      c2.err == BRAID_OK);
	CHECK("integration-concurrent1: same fd served", c2.fd == c1.fd);

	CHECK_ERR("integration-concurrent1: checkin second",
		  braid_pool_checkin(pool, c2.fd, BRAID_CONN_OK), BRAID_OK);

cleanup:
	if (pool != NULL)
		braid_pool_destroy(pool, 0);
	if (event_fd >= 0)
		close(event_fd);
	stop_test_server(server_pid);
}

/*
 * Observe callback receives CONN_CREATED, then CONN_DESTROYED on discard,
 * then RECONNECT_ATTEMPT after replacement scheduling.
 */
static void
test_observe_event_sequence(void)
{
	pid_t server_pid = -1;
	uint16_t port = 0;
	int event_fd = -1;
	braid_pool_t *pool = NULL;
	braid_config_t cfg;
	checkout_rec_t c;
	event_rec_t events;
	int err = 0;
	int i;
	int idx_created;
	int idx_destroyed;
	int idx_reconnect;

	memset(&c, 0, sizeof(c));
	memset(&events, 0, sizeof(events));

	if (start_test_server(&server_pid, &port) != BRAID_OK) {
		CHECK("integration-observe: start server", 0);
		return;
	}

	event_fd = make_event_fd();
	if (event_fd < 0) {
		CHECK("integration-observe: event fd", 0);
		stop_test_server(server_pid);
		return;
	}

	memset(&cfg, 0, sizeof(cfg));
	cfg.host = "127.0.0.1";
	cfg.port = port;
	cfg.event_fd = event_fd;
	cfg.min_connections = 1;
	cfg.max_connections = 1;
	cfg.observe_fn = observe_cb;
	cfg.hook_context = &events;

	pool = braid_pool_create(&cfg, &err);
	CHECK("integration-observe: create pool", pool != NULL);
	if (pool == NULL)
		goto cleanup;

#ifdef BRAID_TEST_CLOCK
	braid_test_clock_ms = 0;
#endif

	CHECK_ERR("integration-observe: warm pool",
		  run_until_idle_at_least(pool, event_fd, 1, 500), BRAID_OK);
	events.count = 0;

	CHECK_ERR("integration-observe: checkout",
		  braid_pool_checkout(pool, 0, checkout_cb, &c, NULL),
		  BRAID_OK);
	CHECK_ERR("integration-observe: discard checkin",
		  braid_pool_checkin(pool, c.fd, BRAID_CONN_DISCARD), BRAID_OK);

	for (i = 0; i < 400; i++) {
		if (event_count_type(&events, BRAID_EV_RECONNECT_ATTEMPT) > 0)
			break;
		event_loop_step(pool, event_fd);
	}

	idx_created =
	    event_first_index_after(&events, BRAID_EV_CONN_CREATED, -1);
	idx_destroyed = event_first_index_after(
	    &events, BRAID_EV_CONN_DESTROYED, idx_created);
	idx_reconnect = event_first_index_after(
	    &events, BRAID_EV_RECONNECT_ATTEMPT, idx_destroyed);

	CHECK("integration-observe: CONN_CREATED observed", idx_created >= 0);
	CHECK("integration-observe: CONN_DESTROYED after CREATED",
	      idx_destroyed > idx_created);
	CHECK("integration-observe: RECONNECT_ATTEMPT after DESTROYED",
	      idx_reconnect > idx_destroyed);

cleanup:
	if (pool != NULL)
		braid_pool_destroy(pool, 0);
	if (event_fd >= 0)
		close(event_fd);
	stop_test_server(server_pid);
}

/*
 * When peer closes while connection is IDLE, pool detects EOF via MSG_PEEK,
 * transitions the connection to DEAD, and schedules reconnect work.
 */
static void
test_half_open_idle_peer_close_detected(void)
{
	pid_t server_pid = -1;
	uint16_t port = 0;
	int event_fd = -1;
	braid_pool_t *pool = NULL;
	braid_config_t cfg;
	int err = 0;
	int i;
	int saw_drop = 0;
	uint32_t initial_live;

	if (start_test_server(&server_pid, &port) != BRAID_OK) {
		CHECK("integration-halfopen-idle: start server", 0);
		return;
	}

	event_fd = make_event_fd();
	if (event_fd < 0) {
		CHECK("integration-halfopen-idle: event fd", 0);
		stop_test_server(server_pid);
		return;
	}

	memset(&cfg, 0, sizeof(cfg));
	cfg.host = "127.0.0.1";
	cfg.port = port;
	cfg.event_fd = event_fd;
	cfg.min_connections = 1;
	cfg.max_connections = 1;

	pool = braid_pool_create(&cfg, &err);
	CHECK("integration-halfopen-idle: create pool", pool != NULL);
	if (pool == NULL)
		goto cleanup;

#ifdef BRAID_TEST_CLOCK
	braid_test_clock_ms = 0;
#endif

	CHECK_ERR("integration-halfopen-idle: warm pool",
		  run_until_idle_at_least(pool, event_fd, 1, 500), BRAID_OK);
	initial_live = pool->live_count;

	stop_test_server(server_pid);
	server_pid = -1;

	for (i = 0; i < 400; i++) {
		event_loop_step(pool, event_fd);
		if (pool->live_count < initial_live)
			saw_drop = 1;
		if (saw_drop && pool_idle_count(pool) == 0)
			break;
	}

	CHECK("integration-halfopen-idle: observed connection teardown",
	      saw_drop == 1);
	CHECK("integration-halfopen-idle: no healthy idle connection remains",
	      pool_idle_count(pool) == 0);

cleanup:
	if (pool != NULL)
		braid_pool_destroy(pool, 0);
	if (event_fd >= 0)
		close(event_fd);
	stop_test_server(server_pid);
}

/*
 * Half-open while ACTIVE is reported by caller with BRAID_CONN_DISCARD.
 * Pool must destroy the active connection and replenish to min_connections.
 */
static void
test_half_open_active_discard_replaced(void)
{
	pid_t server_pid = -1;
	uint16_t port = 0;
	int event_fd = -1;
	braid_pool_t *pool = NULL;
	braid_config_t cfg;
	checkout_rec_t c;
	event_rec_t events;
	int err = 0;

	memset(&c, 0, sizeof(c));
	memset(&events, 0, sizeof(events));

	if (start_test_server(&server_pid, &port) != BRAID_OK) {
		CHECK("integration-halfopen-active: start server", 0);
		return;
	}

	event_fd = make_event_fd();
	if (event_fd < 0) {
		CHECK("integration-halfopen-active: event fd", 0);
		stop_test_server(server_pid);
		return;
	}

	memset(&cfg, 0, sizeof(cfg));
	cfg.host = "127.0.0.1";
	cfg.port = port;
	cfg.event_fd = event_fd;
	cfg.min_connections = 1;
	cfg.max_connections = 1;
	cfg.observe_fn = observe_cb;
	cfg.hook_context = &events;

	pool = braid_pool_create(&cfg, &err);
	CHECK("integration-halfopen-active: create pool", pool != NULL);
	if (pool == NULL)
		goto cleanup;

#ifdef BRAID_TEST_CLOCK
	braid_test_clock_ms = 0;
#endif

	CHECK_ERR("integration-halfopen-active: warm pool",
		  run_until_idle_at_least(pool, event_fd, 1, 500), BRAID_OK);

	events.count = 0;
	CHECK_ERR("integration-halfopen-active: checkout",
		  braid_pool_checkout(pool, 0, checkout_cb, &c, NULL),
		  BRAID_OK);
	CHECK("integration-halfopen-active: checkout succeeded",
	      c.calls == 1 && c.err == BRAID_OK);

	CHECK_ERR("integration-halfopen-active: discard checkin",
		  braid_pool_checkin(pool, c.fd, BRAID_CONN_DISCARD), BRAID_OK);
	CHECK_ERR("integration-halfopen-active: replacement reaches idle",
		  run_until_idle_at_least(pool, event_fd, 1, 700), BRAID_OK);
	CHECK("integration-halfopen-active: destroy event observed",
	      event_count_type(&events, BRAID_EV_CONN_DESTROYED) >= 1);

cleanup:
	if (pool != NULL)
		braid_pool_destroy(pool, 0);
	if (event_fd >= 0)
		close(event_fd);
	stop_test_server(server_pid);
}

/*
 * Reconnection after server restart: after a forced discard while the server
 * is down, restarting the server on the same port should let the pool recover.
 */
static void
test_reconnect_after_server_restart(void)
{
	pid_t server_pid = -1;
	uint16_t port = 0;
	int event_fd = -1;
	braid_pool_t *pool = NULL;
	braid_config_t cfg;
	checkout_rec_t c1;
	checkout_rec_t c2;
	int err = 0;
	int i;

	memset(&c1, 0, sizeof(c1));
	memset(&c2, 0, sizeof(c2));

	if (start_test_server(&server_pid, &port) != BRAID_OK) {
		CHECK("integration-restart: start server", 0);
		return;
	}

	event_fd = make_event_fd();
	if (event_fd < 0) {
		CHECK("integration-restart: event fd", 0);
		stop_test_server(server_pid);
		return;
	}

	memset(&cfg, 0, sizeof(cfg));
	cfg.host = "127.0.0.1";
	cfg.port = port;
	cfg.event_fd = event_fd;
	cfg.min_connections = 1;
	cfg.max_connections = 1;

	pool = braid_pool_create(&cfg, &err);
	CHECK("integration-restart: create pool", pool != NULL);
	if (pool == NULL)
		goto cleanup;

#ifdef BRAID_TEST_CLOCK
	braid_test_clock_ms = 0;
#endif

	CHECK_ERR("integration-restart: warm pool",
		  run_until_idle_at_least(pool, event_fd, 1, 500), BRAID_OK);
	CHECK_ERR("integration-restart: checkout before stop",
		  braid_pool_checkout(pool, 0, checkout_cb, &c1, NULL),
		  BRAID_OK);
	CHECK("integration-restart: first checkout succeeded",
	      c1.calls == 1 && c1.err == BRAID_OK);

	stop_test_server(server_pid);
	server_pid = -1;

	CHECK_ERR("integration-restart: discard while server down",
		  braid_pool_checkin(pool, c1.fd, BRAID_CONN_DISCARD),
		  BRAID_OK);

	for (i = 0; i < 200; i++)
		event_loop_step(pool, event_fd);
	CHECK("integration-restart: no idle while server down",
	      pool_idle_count(pool) == 0);

	CHECK_ERR("integration-restart: restart same port",
		  start_test_server_on_port(&server_pid, port), BRAID_OK);
	CHECK_ERR("integration-restart: pool recovers after restart",
		  run_until_idle_at_least(pool, event_fd, 1, 1000), BRAID_OK);

	CHECK_ERR("integration-restart: checkout after restart",
		  braid_pool_checkout(pool, 0, checkout_cb, &c2, NULL),
		  BRAID_OK);
	CHECK("integration-restart: second checkout succeeded",
	      c2.calls == 1 && c2.err == BRAID_OK);
	CHECK_ERR("integration-restart: checkin after restart",
		  braid_pool_checkin(pool, c2.fd, BRAID_CONN_OK), BRAID_OK);

cleanup:
	if (pool != NULL)
		braid_pool_destroy(pool, 0);
	if (event_fd >= 0)
		close(event_fd);
	stop_test_server(server_pid);
}

/*
 * validate_fn PING/PONG over real socket.
 * Connection is checked only after idle_threshold is exceeded.
 */
static void
test_validate_fn_ping_pong_over_socket(void)
{
	pid_t server_pid = -1;
	uint16_t port = 0;
	int event_fd = -1;
	braid_pool_t *pool = NULL;
	braid_config_t cfg;
	checkout_rec_t c;
	validate_ctx_t vctx;
	int err = 0;

	memset(&c, 0, sizeof(c));
	memset(&vctx, 0, sizeof(vctx));

	if (start_test_server(&server_pid, &port) != BRAID_OK) {
		CHECK("integration-validate-ping: start server", 0);
		return;
	}

	event_fd = make_event_fd();
	if (event_fd < 0) {
		CHECK("integration-validate-ping: event fd", 0);
		stop_test_server(server_pid);
		return;
	}

	memset(&cfg, 0, sizeof(cfg));
	cfg.host = "127.0.0.1";
	cfg.port = port;
	cfg.event_fd = event_fd;
	cfg.min_connections = 1;
	cfg.max_connections = 1;
	cfg.idle_threshold = 1;
	cfg.validate_timeout = 1000;
	cfg.validate_fn = validate_pingpong_cb;
	cfg.hook_context = &vctx;

	pool = braid_pool_create(&cfg, &err);
	CHECK("integration-validate-ping: create pool", pool != NULL);
	if (pool == NULL)
		goto cleanup;

#ifdef BRAID_TEST_CLOCK
	braid_test_clock_ms = 0;
#endif

	CHECK_ERR("integration-validate-ping: warm pool",
		  run_until_idle_at_least(pool, event_fd, 1, 500), BRAID_OK);
#ifdef BRAID_TEST_CLOCK
	braid_test_clock_ms += 100;
#endif

	CHECK_ERR("integration-validate-ping: checkout",
		  braid_pool_checkout(pool, 0, checkout_cb, &c, NULL),
		  BRAID_OK);
	CHECK("integration-validate-ping: checkout succeeded",
	      c.calls == 1 && c.err == BRAID_OK);
	CHECK("integration-validate-ping: validate called", vctx.calls >= 1);

	CHECK_ERR("integration-validate-ping: checkin",
		  braid_pool_checkin(pool, c.fd, BRAID_CONN_OK), BRAID_OK);

cleanup:
	if (pool != NULL)
		braid_pool_destroy(pool, 0);
	if (event_fd >= 0)
		close(event_fd);
	stop_test_server(server_pid);
}

/*
 * validate_fn timeout exceeded discards the connection.
 * The timed-out checkout request fails fast with zero-timeout mode.
 */
static void
test_validate_fn_timeout_exceeded(void)
{
	pid_t server_pid = -1;
	uint16_t port = 0;
	int event_fd = -1;
	braid_pool_t *pool = NULL;
	braid_config_t cfg;
	validate_ctx_t vctx;
	int err = 0;

	memset(&vctx, 0, sizeof(vctx));

	if (start_test_server(&server_pid, &port) != BRAID_OK) {
		CHECK("integration-validate-timeout: start server", 0);
		return;
	}

	event_fd = make_event_fd();
	if (event_fd < 0) {
		CHECK("integration-validate-timeout: event fd", 0);
		stop_test_server(server_pid);
		return;
	}

	memset(&cfg, 0, sizeof(cfg));
	cfg.host = "127.0.0.1";
	cfg.port = port;
	cfg.event_fd = event_fd;
	cfg.min_connections = 1;
	cfg.max_connections = 1;
	cfg.idle_threshold = 1;
	cfg.validate_timeout = 50;
	cfg.validate_fn = validate_timeout_cb;
	cfg.hook_context = &vctx;

	pool = braid_pool_create(&cfg, &err);
	CHECK("integration-validate-timeout: create pool", pool != NULL);
	if (pool == NULL)
		goto cleanup;

#ifdef BRAID_TEST_CLOCK
	braid_test_clock_ms = 0;
#endif

	CHECK_ERR("integration-validate-timeout: warm pool",
		  run_until_idle_at_least(pool, event_fd, 1, 500), BRAID_OK);
#ifdef BRAID_TEST_CLOCK
	braid_test_clock_ms += 100;
#endif

	CHECK_ERR("integration-validate-timeout: checkout fails exhausted",
		  braid_pool_checkout(pool, 0, checkout_cb, NULL, NULL),
		  BRAID_ERR_EXHAUSTED);
	CHECK("integration-validate-timeout: validate called", vctx.calls >= 1);
	CHECK_ERR("integration-validate-timeout: replacement recovers",
		  run_until_idle_at_least(pool, event_fd, 1, 800), BRAID_OK);

cleanup:
	if (pool != NULL)
		braid_pool_destroy(pool, 0);
	if (event_fd >= 0)
		close(event_fd);
	stop_test_server(server_pid);
}

/*
 * init_fn handshake simulation over real socket.
 * init_fn exchanges HELO/OKAY and attaches conn_ctx.
 */
static void
test_init_fn_handshake_simulation(void)
{
	pid_t server_pid = -1;
	uint16_t port = 0;
	int event_fd = -1;
	braid_pool_t *pool = NULL;
	braid_config_t cfg;
	checkout_rec_t c;
	init_ctx_t ictx;
	int err = 0;

	memset(&c, 0, sizeof(c));
	memset(&ictx, 0, sizeof(ictx));

	if (start_test_server(&server_pid, &port) != BRAID_OK) {
		CHECK("integration-init: start server", 0);
		return;
	}

	event_fd = make_event_fd();
	if (event_fd < 0) {
		CHECK("integration-init: event fd", 0);
		stop_test_server(server_pid);
		return;
	}

	memset(&cfg, 0, sizeof(cfg));
	cfg.host = "127.0.0.1";
	cfg.port = port;
	cfg.event_fd = event_fd;
	cfg.min_connections = 1;
	cfg.max_connections = 1;
	cfg.init_timeout = 1000;
	cfg.init_fn = init_handshake_cb;
	cfg.hook_context = &ictx;

	pool = braid_pool_create(&cfg, &err);
	CHECK("integration-init: create pool", pool != NULL);
	if (pool == NULL)
		goto cleanup;

#ifdef BRAID_TEST_CLOCK
	braid_test_clock_ms = 0;
#endif

	CHECK_ERR("integration-init: warm pool",
		  run_until_idle_at_least(pool, event_fd, 1, 500), BRAID_OK);
	CHECK("integration-init: init called", ictx.calls >= 1);

	CHECK_ERR("integration-init: checkout",
		  braid_pool_checkout(pool, 0, checkout_cb, &c, NULL),
		  BRAID_OK);
	CHECK("integration-init: conn_ctx attached",
	      c.calls == 1 && c.err == BRAID_OK && c.conn_ctx == &ictx);
	CHECK_ERR("integration-init: checkin",
		  braid_pool_checkin(pool, c.fd, BRAID_CONN_OK), BRAID_OK);

cleanup:
	if (pool != NULL)
		braid_pool_destroy(pool, 0);
	if (event_fd >= 0)
		close(event_fd);
	stop_test_server(server_pid);
}

/*
 * destroy_fn graceful teardown path.
 * destroy_fn attempts a protocol goodbye write before fd close.
 */
static void
test_destroy_fn_graceful_teardown(void)
{
	pid_t server_pid = -1;
	uint16_t port = 0;
	int event_fd = -1;
	braid_pool_t *pool = NULL;
	braid_config_t cfg;
	checkout_rec_t c;
	destroy_ctx_t dctx;
	int err = 0;

	memset(&c, 0, sizeof(c));
	memset(&dctx, 0, sizeof(dctx));

	if (start_test_server(&server_pid, &port) != BRAID_OK) {
		CHECK("integration-destroy-graceful: start server", 0);
		return;
	}

	event_fd = make_event_fd();
	if (event_fd < 0) {
		CHECK("integration-destroy-graceful: event fd", 0);
		stop_test_server(server_pid);
		return;
	}

	memset(&cfg, 0, sizeof(cfg));
	cfg.host = "127.0.0.1";
	cfg.port = port;
	cfg.event_fd = event_fd;
	cfg.min_connections = 1;
	cfg.max_connections = 1;
	cfg.destroy_fn = destroy_graceful_cb;
	cfg.hook_context = &dctx;

	pool = braid_pool_create(&cfg, &err);
	CHECK("integration-destroy-graceful: create pool", pool != NULL);
	if (pool == NULL)
		goto cleanup;

#ifdef BRAID_TEST_CLOCK
	braid_test_clock_ms = 0;
#endif

	CHECK_ERR("integration-destroy-graceful: warm pool",
		  run_until_idle_at_least(pool, event_fd, 1, 500), BRAID_OK);
	CHECK_ERR("integration-destroy-graceful: checkout",
		  braid_pool_checkout(pool, 0, checkout_cb, &c, NULL),
		  BRAID_OK);
	CHECK_ERR("integration-destroy-graceful: discard",
		  braid_pool_checkin(pool, c.fd, BRAID_CONN_DISCARD), BRAID_OK);

	CHECK("integration-destroy-graceful: destroy called once",
	      dctx.calls == 1);
	CHECK("integration-destroy-graceful: goodbye write succeeded",
	      dctx.write_ok_calls == 1);

cleanup:
	if (pool != NULL)
		braid_pool_destroy(pool, 0);
	if (event_fd >= 0)
		close(event_fd);
	stop_test_server(server_pid);
}

/*
 * destroy_fn handles unknown protocol state.
 * Connection has no init_fn so conn_ctx is NULL when destroy_fn runs.
 */
static void
test_destroy_fn_unknown_protocol_state(void)
{
	pid_t server_pid = -1;
	uint16_t port = 0;
	int event_fd = -1;
	braid_pool_t *pool = NULL;
	braid_config_t cfg;
	checkout_rec_t c;
	destroy_ctx_t dctx;
	int err = 0;

	memset(&c, 0, sizeof(c));
	memset(&dctx, 0, sizeof(dctx));

	if (start_test_server(&server_pid, &port) != BRAID_OK) {
		CHECK("integration-destroy-unknown: start server", 0);
		return;
	}

	event_fd = make_event_fd();
	if (event_fd < 0) {
		CHECK("integration-destroy-unknown: event fd", 0);
		stop_test_server(server_pid);
		return;
	}

	memset(&cfg, 0, sizeof(cfg));
	cfg.host = "127.0.0.1";
	cfg.port = port;
	cfg.event_fd = event_fd;
	cfg.min_connections = 1;
	cfg.max_connections = 1;
	cfg.destroy_fn = destroy_unknown_ctx_cb;
	cfg.hook_context = &dctx;

	pool = braid_pool_create(&cfg, &err);
	CHECK("integration-destroy-unknown: create pool", pool != NULL);
	if (pool == NULL)
		goto cleanup;

#ifdef BRAID_TEST_CLOCK
	braid_test_clock_ms = 0;
#endif

	CHECK_ERR("integration-destroy-unknown: warm pool",
		  run_until_idle_at_least(pool, event_fd, 1, 500), BRAID_OK);
	CHECK_ERR("integration-destroy-unknown: checkout",
		  braid_pool_checkout(pool, 0, checkout_cb, &c, NULL),
		  BRAID_OK);
	CHECK_ERR("integration-destroy-unknown: discard",
		  braid_pool_checkin(pool, c.fd, BRAID_CONN_DISCARD), BRAID_OK);

	CHECK("integration-destroy-unknown: destroy called once",
	      dctx.calls == 1);
	CHECK("integration-destroy-unknown: NULL conn_ctx handled",
	      dctx.null_ctx_calls == 1);

cleanup:
	if (pool != NULL)
		braid_pool_destroy(pool, 0);
	if (event_fd >= 0)
		close(event_fd);
	stop_test_server(server_pid);
}

/*
 * Destroy while reconnect entries are pending must return cleanly and
 * clear internal reconnect state without attempting post-destroy connects.
 */
static void
test_destroy_during_pending_reconnect(void)
{
	int event_fd = -1;
	braid_pool_t *pool = NULL;
	braid_config_t cfg;
	braid_reconnect_entry_t entry;
	int err = 0;

	event_fd = make_event_fd();
	if (event_fd < 0) {
		CHECK("integration-destroy-reconnect: event fd", 0);
		return;
	}

	memset(&cfg, 0, sizeof(cfg));
	cfg.host = "127.0.0.1";
	cfg.port = 65535;
	cfg.event_fd = event_fd;
	cfg.min_connections = 0;
	cfg.max_connections = 1;

	pool = braid_pool_create(&cfg, &err);
	CHECK("integration-destroy-reconnect: create pool", pool != NULL);
	if (pool == NULL) {
		close(event_fd);
		return;
	}

#ifdef BRAID_TEST_CLOCK
	braid_test_clock_ms = 0;
#endif

	memset(&entry, 0, sizeof(entry));
	entry.next_retry_ms = UINT64_MAX;
	entry.attempt = 7;
	CHECK_ERR("integration-destroy-reconnect: push pending reconnect",
		  reconnect_heap_push(&pool->reconnect, entry), BRAID_OK);

	CHECK("integration-destroy-reconnect: reconnect queue non-empty",
	      pool->reconnect.count > 0);

	braid_pool_destroy(pool, 0);
	pool = NULL;
	close(event_fd);
}

/*
 * Backoff storm prevention: multiple failing pools should not schedule their
 * next reconnect attempt at exactly the same timestamp.
 */
static void
test_backoff_prevents_connection_storm(void)
{
	enum { STORM_POOLS = 5 };
	int event_fd[STORM_POOLS];
	braid_pool_t *pools[STORM_POOLS];
	uint64_t retry_ms[STORM_POOLS];
	int i;
	int err = 0;
	int all_equal = 1;

	memset(event_fd, -1, sizeof(event_fd));
	memset(pools, 0, sizeof(pools));
	memset(retry_ms, 0, sizeof(retry_ms));

#ifdef BRAID_TEST_CLOCK
	braid_test_clock_ms = 0;
	reconnect_test_set_socket_create_hook(
	    reconnect_force_socket_create_fail);
#endif

	for (i = 0; i < STORM_POOLS; i++) {
		braid_config_t cfg;
		braid_reconnect_entry_t top;

		event_fd[i] = make_event_fd();
		if (event_fd[i] < 0) {
			CHECK("integration-storm: event fd", 0);
			goto cleanup;
		}

		memset(&cfg, 0, sizeof(cfg));
		cfg.host = "127.0.0.1";
		cfg.port = 54321;
		cfg.event_fd = event_fd[i];
		cfg.min_connections = 1;
		cfg.max_connections = 1;
		cfg.backoff_base = 20000;
		cfg.backoff_cap = 60000;

		pools[i] = braid_pool_create(&cfg, &err);
		CHECK("integration-storm: create pool", pools[i] != NULL);
		if (pools[i] == NULL)
			goto cleanup;

		CHECK_ERR("integration-storm: advance",
			  braid_pool_advance(pools[i], NULL), BRAID_OK);
		CHECK("integration-storm: reconnect queued",
		      pools[i]->reconnect.count > 0);
		CHECK_ERR("integration-storm: reconnect peek",
			  reconnect_heap_peek(&pools[i]->reconnect, &top),
			  BRAID_OK);
		retry_ms[i] = top.next_retry_ms;
	}

	for (i = 1; i < STORM_POOLS; i++) {
		if (retry_ms[i] != retry_ms[0]) {
			all_equal = 0;
			break;
		}
	}

	CHECK("integration-storm: reconnect deadlines desynchronised",
	      all_equal == 0);

cleanup:
#ifdef BRAID_TEST_CLOCK
	reconnect_test_set_socket_create_hook(NULL);
#endif
	for (i = 0; i < STORM_POOLS; i++) {
		if (pools[i] != NULL)
			braid_pool_destroy(pools[i], 0);
		if (event_fd[i] >= 0)
			close(event_fd[i]);
	}
}

/*
 * Checkout timeout observability: BRAID_EV_CHECKOUT_TIMEOUT must fire before
 * the timed-out checkout callback is invoked.
 */
static void
test_checkout_timeout_event_before_callback(void)
{
	pid_t server_pid = -1;
	uint16_t port = 0;
	int event_fd = -1;
	braid_pool_t *pool = NULL;
	braid_config_t cfg;
	checkout_rec_t active;
	timeout_order_t ord;
	int err = 0;
	int i;

	memset(&active, 0, sizeof(active));
	memset(&ord, 0, sizeof(ord));

	if (start_test_server(&server_pid, &port) != BRAID_OK) {
		CHECK("integration-timeout-event: start server", 0);
		return;
	}

	event_fd = make_event_fd();
	if (event_fd < 0) {
		CHECK("integration-timeout-event: event fd", 0);
		stop_test_server(server_pid);
		return;
	}

	memset(&cfg, 0, sizeof(cfg));
	cfg.host = "127.0.0.1";
	cfg.port = port;
	cfg.event_fd = event_fd;
	cfg.min_connections = 1;
	cfg.max_connections = 1;
	cfg.observe_fn = observe_timeout_order_cb;
	cfg.hook_context = &ord;

	pool = braid_pool_create(&cfg, &err);
	CHECK("integration-timeout-event: create pool", pool != NULL);
	if (pool == NULL)
		goto cleanup;

#ifdef BRAID_TEST_CLOCK
	braid_test_clock_ms = 0;
#endif

	CHECK_ERR("integration-timeout-event: warm pool",
		  run_until_idle_at_least(pool, event_fd, 1, 500), BRAID_OK);

	CHECK_ERR("integration-timeout-event: checkout active",
		  braid_pool_checkout(pool, 0, checkout_cb, &active, NULL),
		  BRAID_OK);
	CHECK("integration-timeout-event: active checkout success",
	      active.calls == 1 && active.err == BRAID_OK);

	memset(&ord, 0, sizeof(ord));
	CHECK_ERR("integration-timeout-event: enqueue timed waiter",
		  braid_pool_checkout(pool, 30, checkout_timeout_order_cb, &ord,
				      NULL),
		  BRAID_OK);

	for (i = 0; i < 300 && ord.timeout_cb_calls == 0; i++)
		event_loop_step(pool, event_fd);

	CHECK("integration-timeout-event: timeout callback fired",
	      ord.timeout_cb_calls == 1);
	CHECK("integration-timeout-event: timeout callback error",
	      ord.timeout_cb_err == BRAID_ERR_TIMEOUT);
	CHECK("integration-timeout-event: timeout event fired",
	      ord.timeout_event_seq > 0);
	CHECK("integration-timeout-event: event before callback",
	      ord.timeout_event_seq < ord.timeout_cb_seq);

	CHECK_ERR("integration-timeout-event: checkin active",
		  braid_pool_checkin(pool, active.fd, BRAID_CONN_OK), BRAID_OK);

cleanup:
	if (pool != NULL)
		braid_pool_destroy(pool, 0);
	if (event_fd >= 0)
		close(event_fd);
	stop_test_server(server_pid);
}
/*
 * Warm pool reaches min_connections without any checkout calls.
 * Then verifies two immediate checkouts succeed from the warmed pool.
 */
static void
test_warm_pool_reaches_min_connections(void)
{
	pid_t server_pid = -1;
	uint16_t port = 0;
	int event_fd = -1;
	braid_pool_t *pool = NULL;
	braid_config_t cfg;
	checkout_rec_t c1;
	checkout_rec_t c2;
	int err = 0;
	int i;

	memset(&c1, 0, sizeof(c1));
	memset(&c2, 0, sizeof(c2));

	if (start_test_server(&server_pid, &port) != BRAID_OK) {
		CHECK("integration-warm: start server", 0);
		return;
	}

	event_fd = make_event_fd();
	if (event_fd < 0) {
		CHECK("integration-warm: event fd", 0);
		stop_test_server(server_pid);
		return;
	}

	memset(&cfg, 0, sizeof(cfg));
	cfg.host = "127.0.0.1";
	cfg.port = port;
	cfg.event_fd = event_fd;
	cfg.min_connections = 2;
	cfg.max_connections = 2;
	cfg.connect_timeout = 1000;
	cfg.init_timeout = 1000;
	cfg.validate_timeout = 1000;

	pool = braid_pool_create(&cfg, &err);
	CHECK("integration-warm: create pool", pool != NULL);
	if (pool == NULL)
		goto cleanup;

#ifdef BRAID_TEST_CLOCK
	braid_test_clock_ms = 0;
#endif

	for (i = 0; i < 500; i++) {
		if (pool_idle_count(pool) >= 2)
			break;
		if (event_loop_step(pool, event_fd) != BRAID_OK)
			break;
	}

	CHECK("integration-warm: pool has two idle connections",
	      pool_idle_count(pool) >= 2);

	CHECK_ERR("integration-warm: checkout #1",
		  braid_pool_checkout(pool, 0, checkout_cb, &c1, NULL),
		  BRAID_OK);
	CHECK_ERR("integration-warm: checkout #2",
		  braid_pool_checkout(pool, 0, checkout_cb, &c2, NULL),
		  BRAID_OK);
	CHECK("integration-warm: callback #1 fired", c1.calls == 1);
	CHECK("integration-warm: callback #2 fired", c2.calls == 1);
	CHECK("integration-warm: checkout #1 success", c1.err == BRAID_OK);
	CHECK("integration-warm: checkout #2 success", c2.err == BRAID_OK);
	CHECK("integration-warm: distinct active fds", c1.fd != c2.fd);

	CHECK_ERR("integration-warm: checkin #1",
		  braid_pool_checkin(pool, c1.fd, BRAID_CONN_OK), BRAID_OK);
	CHECK_ERR("integration-warm: checkin #2",
		  braid_pool_checkin(pool, c2.fd, BRAID_CONN_OK), BRAID_OK);

cleanup:
	if (pool != NULL)
		braid_pool_destroy(pool, 0);
	if (event_fd >= 0)
		close(event_fd);
	stop_test_server(server_pid);
}

/*
 * Integration suite entry point.
 */
void
run_integration_tests(void)
{
	test_full_connect_checkout_checkin_reuse();
	test_warm_pool_reaches_min_connections();
	test_pool_exhausted_event_fires();
	test_single_connection_concurrent_checkouts();
	test_observe_event_sequence();
	test_half_open_idle_peer_close_detected();
	test_half_open_active_discard_replaced();
	test_reconnect_after_server_restart();
	test_validate_fn_ping_pong_over_socket();
	test_validate_fn_timeout_exceeded();
	test_init_fn_handshake_simulation();
	test_destroy_fn_graceful_teardown();
	test_destroy_fn_unknown_protocol_state();
	test_destroy_during_pending_reconnect();
	test_backoff_prevents_connection_storm();
	test_checkout_timeout_event_before_callback();
}
