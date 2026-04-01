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
#include "test_harness.h"

typedef struct {
	int calls;
	int fd;
	int err;
	void *conn_ctx;
} checkout_rec_t;

static void
checkout_cb(int fd, void *conn_ctx, int err, void *cb_ctx)
{
	checkout_rec_t *rec = cb_ctx;

	rec->calls++;
	rec->fd = fd;
	rec->err = err;
	rec->conn_ctx = conn_ctx;
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
start_test_server(pid_t *pid_out, uint16_t *port_out)
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
		addr.sin_port = htons(0);

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

		for (;;) {
			int cfd = accept(srvfd, NULL, NULL);

			if (cfd < 0) {
				if (errno == EINTR)
					continue;
				_exit(16);
			}

			if (nclients <
			    (int)(sizeof(clients) / sizeof(clients[0])))
				clients[nclients++] = cfd;
			else
				close(cfd);
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

void
run_integration_tests(void)
{
	test_full_connect_checkout_checkin_reuse();
	test_warm_pool_reaches_min_connections();
}
