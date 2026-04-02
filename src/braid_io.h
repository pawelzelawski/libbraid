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

#ifdef BRAID_TEST_CLOCK
#ifndef __linux__
/* kqueue test helpers — only declared on non-Linux platforms where
 * braid_io_kqueue.c is compiled. On Linux, braid_io_epoll.c is used and
 * these functions do not exist. */
void io_kqueue_test_force_error_on_call(int call_no);
void io_kqueue_test_reset_apply_calls(void);
int io_kqueue_test_get_apply_calls(void);
#endif /* __linux__ */
#endif /* BRAID_TEST_CLOCK */

#endif /* BRAID_IO_H */
