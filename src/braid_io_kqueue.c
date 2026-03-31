/*
 * braid_io_kqueue.c — kqueue implementation of the I/O abstraction layer
 *
 * io_watch(): kevent(EVFILT_READ or EVFILT_WRITE, EV_ADD).
 * io_modify(): delete old filter, add new filter.
 * io_unwatch(): kevent(EV_DELETE) for all active filters on fd.
 *
 * udata is set to &conn->tag (inline braid_fd_tag_t struct).
 * Compiled on OpenBSD, FreeBSD, and NetBSD only — excluded from Linux build.
 * See ARCHITECTURE.md §8.1, §15.3.
 */

#include <stdint.h>
#include <sys/event.h>
#include <unistd.h>

#include "../include/braid.h"
#include "braid_internal.h"
#include "braid_io.h"
