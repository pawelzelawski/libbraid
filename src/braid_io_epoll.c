/*
 * braid_io_epoll.c — Linux epoll implementation of the I/O abstraction layer
 *
 * io_watch(): epoll_ctl(EPOLL_CTL_ADD) with EPOLLET.
 * io_modify(): epoll_ctl(EPOLL_CTL_MOD).
 * io_unwatch(): epoll_ctl(EPOLL_CTL_DEL).
 *
 * epoll_data.ptr is set to &conn->tag (inline braid_fd_tag_t struct).
 * Compiled on Linux only — excluded from the OpenBSD build.
 * See ARCHITECTURE.md §8.1, §15.2.
 */

#include <stdint.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "../include/braid.h"
#include "braid_internal.h"
#include "braid_io.h"
#include "braid_table.h"

/*
 * events_to_epoll — translate BRAID_IO_* bitmask to epoll event flags.
 * Always includes EPOLLET (edge-triggered). See ARCHITECTURE.md §8.1.
 */
static uint32_t
events_to_epoll(uint32_t events)
{
	uint32_t epev = EPOLLET;

	if (events & BRAID_IO_READ)
		epev |= EPOLLIN;
	if (events & BRAID_IO_WRITE)
		epev |= EPOLLOUT;
	return epev;
}

int
io_watch(braid_pool_t *pool, int fd, uint32_t events)
{
	braid_conn_t *conn;
	struct epoll_event ev;

	if (table_lookup(pool, fd, &conn) != BRAID_OK) {
		BRAID_DEBUG_ASSERT(0, "io_watch: fd not in connection table");
		return BRAID_ERR_INVAL;
	}

	ev.events = events_to_epoll(events);
	ev.data.ptr = &conn->tag;

	if (epoll_ctl(pool->config.event_fd, EPOLL_CTL_ADD, fd, &ev) != 0)
		return BRAID_ERR_SYSCALL;

	return BRAID_OK;
}

int
io_modify(braid_pool_t *pool, int fd, uint32_t events)
{
	braid_conn_t *conn;
	struct epoll_event ev;

	if (table_lookup(pool, fd, &conn) != BRAID_OK) {
		BRAID_DEBUG_ASSERT(0, "io_modify: fd not in connection table");
		return BRAID_ERR_INVAL;
	}

	ev.events = events_to_epoll(events);
	ev.data.ptr = &conn->tag;

	if (epoll_ctl(pool->config.event_fd, EPOLL_CTL_MOD, fd, &ev) != 0)
		return BRAID_ERR_SYSCALL;

	return BRAID_OK;
}

int
io_unwatch(braid_pool_t *pool, int fd)
{
	if (epoll_ctl(pool->config.event_fd, EPOLL_CTL_DEL, fd, NULL) != 0)
		return BRAID_ERR_SYSCALL;

	return BRAID_OK;
}
