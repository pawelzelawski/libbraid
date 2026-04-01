/*
 * bench_common.c — shared helpers for libbraid benchmarks
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
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

#include "../src/braid_internal.h"
#include "bench_common.h"

void
bench_checkout_cb(int fd, void *conn_ctx, int err, void *cb_ctx)
{
	bench_checkout_rec_t *rec = cb_ctx;

	rec->called = 1;
	rec->fd = fd;
	rec->err = err;
	rec->conn_ctx = conn_ctx;
}

uint64_t
bench_now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

#ifndef __linux__
static int
bench_read_first_line(const char *cmd, char *buf, size_t buflen)
{
	FILE *f;

	if (buflen == 0)
		return BRAID_ERR_INVAL;

	buf[0] = '\0';
	f = popen(cmd, "r");
	if (f == NULL)
		return BRAID_ERR_SYSCALL;

	if (fgets(buf, (int)buflen, f) == NULL) {
		(void)pclose(f);
		return BRAID_ERR_SYSCALL;
	}

	(void)pclose(f);
	buf[strcspn(buf, "\r\n")] = '\0';
	return (buf[0] == '\0') ? BRAID_ERR_SYSCALL : BRAID_OK;
}
#endif

int
bench_make_event_fd(void)
{
#ifdef __linux__
	return epoll_create1(EPOLL_CLOEXEC);
#else
	return kqueue();
#endif
}

static int
bench_read_exact(int fd, void *buf, size_t n)
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

int
bench_start_server(pid_t *pid_out, uint16_t *port_out)
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
		int clients[4096];
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
		if (listen(srvfd, 512) != 0)
			_exit(14);

		port = ntohs(bound.sin_port);
		if (write(pipefd[1], &port, sizeof(port)) !=
		    (ssize_t)sizeof(port))
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
	if (bench_read_exact(pipefd[0], port_out, sizeof(*port_out)) !=
	    BRAID_OK) {
		close(pipefd[0]);
		kill(pid, SIGTERM);
		waitpid(pid, NULL, 0);
		return BRAID_ERR_SYSCALL;
	}
	close(pipefd[0]);

	*pid_out = pid;
	return BRAID_OK;
}

void
bench_stop_server(pid_t pid)
{
	if (pid <= 0)
		return;
	kill(pid, SIGTERM);
	waitpid(pid, NULL, 0);
}

int
bench_event_loop_step(braid_pool_t *pool, int event_fd, int max_wait_ms)
{
	uint32_t next_ms = 0;
	int rc;
	int timeout_ms;

	rc = braid_pool_advance(pool, &next_ms);
	if (rc != BRAID_OK)
		return rc;

	timeout_ms = (next_ms == UINT32_MAX) ? max_wait_ms : (int)next_ms;
	if (timeout_ms < 0)
		timeout_ms = 0;
	if (timeout_ms > max_wait_ms)
		timeout_ms = max_wait_ms;

#ifdef __linux__
	{
		struct epoll_event events[64];
		int i;
		int n;

		n = epoll_wait(event_fd, events, 64, timeout_ms);
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
		struct kevent events[64];
		struct timespec ts;
		int i;
		int n;

		ts.tv_sec = timeout_ms / 1000;
		ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
		n = kevent(event_fd, NULL, 0, events, 64, &ts);
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

uint32_t
bench_pool_idle_count(braid_pool_t *pool)
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

uint32_t
bench_fd_budget(void)
{
	struct rlimit lim;
	const rlim_t reserve = (rlim_t)64;

	if (getrlimit(RLIMIT_NOFILE, &lim) != 0)
		return UINT32_MAX;

	if (lim.rlim_cur == RLIM_INFINITY)
		return UINT32_MAX;

	if (lim.rlim_cur <= reserve)
		return 0;

	if (lim.rlim_cur - reserve > (rlim_t)UINT32_MAX)
		return UINT32_MAX;

	return (uint32_t)(lim.rlim_cur - reserve);
}

int
bench_warm_pool(braid_pool_t *pool, int event_fd, uint32_t want_idle,
		int max_steps)
{
	int i;

	for (i = 0; i < max_steps; i++) {
		if (bench_pool_idle_count(pool) >= want_idle)
			return BRAID_OK;
		if (bench_event_loop_step(pool, event_fd, 20) != BRAID_OK)
			return BRAID_ERR_SYSCALL;
	}

	return BRAID_ERR_TIMEOUT;
}

void
bench_print_hw_context(const char *name)
{
	struct utsname u;
	long cores;
	char model[256] = "unknown";
	char mhz[64] = "unknown";

	uname(&u);
	cores = sysconf(_SC_NPROCESSORS_ONLN);

#ifdef __linux__
	{
		FILE *f = fopen("/proc/cpuinfo", "r");
		char line[512];

		if (f != NULL) {
			while (fgets(line, sizeof(line), f) != NULL) {
				if (strncmp(line, "model name", 10) == 0) {
					char *p = strchr(line, ':');

					if (p != NULL) {
						p += 1;
						while (*p == ' ' || *p == '\t')
							p++;
						snprintf(model, sizeof(model),
							 "%s", p);
						model[strcspn(model, "\r\n")] =
						    '\0';
					}
				}
				if (strncmp(line, "cpu MHz", 7) == 0) {
					char *p = strchr(line, ':');

					if (p != NULL) {
						p += 1;
						while (*p == ' ' || *p == '\t')
							p++;
						snprintf(mhz, sizeof(mhz),
							 "%s MHz", p);
						mhz[strcspn(mhz, "\r\n")] =
						    '\0';
					}
				}
				if (strcmp(model, "unknown") != 0 &&
				    strcmp(mhz, "unknown") != 0)
					break;
			}
			fclose(f);
		}
	}
#else
	{
		char speed[64];

		(void)bench_read_first_line("sysctl -n hw.model 2>/dev/null",
					    model, sizeof(model));

		if (bench_read_first_line("sysctl -n hw.cpuspeed 2>/dev/null",
					  speed, sizeof(speed)) == BRAID_OK) {
			snprintf(mhz, sizeof(mhz), "%s MHz", speed);
		} else if (bench_read_first_line(
			       "sysctl -n hw.cpufrequency 2>/dev/null", speed,
			       sizeof(speed)) == BRAID_OK) {
			char *end = NULL;
			unsigned long long hz = strtoull(speed, &end, 10);

			if (end != speed && hz > 0ULL)
				snprintf(mhz, sizeof(mhz), "%.2f GHz",
					 (double)hz / 1000000000.0);
			else
				snprintf(mhz, sizeof(mhz), "%s", speed);
		}
	}
#endif

	printf("libbraid benchmark - %s\n", name);
	printf("Platform : %s %s\n", u.sysname, u.machine);
	printf("CPU      : %s\n", model);
	printf("Cores    : %ld\n", cores);
	printf("Clock    : %s\n\n", mhz);
}
