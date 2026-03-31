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

/*
 * Stubs — full implementation in Phase 6.
 */

int
io_watch(braid_pool_t *pool, int fd, uint32_t events)
{
	(void)pool;
	(void)fd;
	(void)events;
	return BRAID_OK;
}

int
io_modify(braid_pool_t *pool, int fd, uint32_t events)
{
	(void)pool;
	(void)fd;
	(void)events;
	return BRAID_OK;
}

int
io_unwatch(braid_pool_t *pool, int fd)
{
	(void)pool;
	(void)fd;
	return BRAID_OK;
}
