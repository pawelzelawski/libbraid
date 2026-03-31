/*
 * braid_io.h — platform-independent I/O abstraction interface (internal)
 *
 * Thin wrapper over epoll (Linux) or kqueue (OpenBSD/FreeBSD/NetBSD).
 * Implemented by braid_io_epoll.c or braid_io_kqueue.c — selected by Makefile.
 * See ARCHITECTURE.md §8.1.
 */

#ifndef BRAID_IO_H
#define BRAID_IO_H

#include "../include/braid.h"
#include "braid_internal.h"

int io_watch(braid_pool_t *pool, int fd, uint32_t events);
int io_modify(braid_pool_t *pool, int fd, uint32_t events);
int io_unwatch(braid_pool_t *pool, int fd);

#endif /* BRAID_IO_H */
