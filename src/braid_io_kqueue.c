/*
 * braid_io_kqueue.c — kqueue implementation of the I/O abstraction layer
 *
 * io_watch(): kevent(EVFILT_READ or EVFILT_WRITE, EV_ADD).
 * io_modify(): delete old filter(s), add new filter(s).
 * io_unwatch(): EV_DELETE for all active filters on fd.
 *
 * udata is set to &conn->tag (inline braid_fd_tag_t struct).
 * Compiled on OpenBSD, FreeBSD, and NetBSD only — excluded from Linux build.
 * See ARCHITECTURE.md §8.1, §15.3.
 */

#include <errno.h>
#include <stdint.h>
#include <sys/event.h>
#include <unistd.h>

#include "../include/braid.h"
#include "braid_internal.h"
#include "braid_io.h"
#include "braid_table.h"

#ifdef BRAID_TEST_CLOCK
static int kq_apply_calls;
static int kq_apply_force_error_call;

void
io_kqueue_test_force_error_on_call(int call_no)
{
	kq_apply_force_error_call = call_no;
}

void
io_kqueue_test_reset_apply_calls(void)
{
	kq_apply_calls = 0;
}

int
io_kqueue_test_get_apply_calls(void)
{
	return kq_apply_calls;
}
#endif

/*
 * kq_apply — submit a changelist of kevents to the caller's kqueue fd.
 * Returns BRAID_OK on success, BRAID_ERR_SYSCALL on hard failure.
 * ENOENT on EV_DELETE is tolerated — the filter was not registered.
 */
static int
kq_apply(int kqfd, struct kevent *changes, int nchanges)
{
	int ret;

#ifdef BRAID_TEST_CLOCK
	kq_apply_calls++;
	if (kq_apply_force_error_call > 0 &&
	    kq_apply_calls == kq_apply_force_error_call)
		return BRAID_ERR_SYSCALL;
#endif

	ret = kevent(kqfd, changes, nchanges, NULL, 0, NULL);
	if (ret == -1) {
		if (errno == ENOENT)
			return BRAID_OK;
		return BRAID_ERR_SYSCALL;
	}
	return BRAID_OK;
}

int
io_watch(braid_pool_t *pool, int fd, uint32_t events)
{
	braid_conn_t *conn;
	struct kevent changes[2];
	int n = 0;

	if (table_lookup(pool, fd, &conn) != BRAID_OK) {
		BRAID_DEBUG_ASSERT(0, "io_watch: fd not in connection table");
		return BRAID_ERR_INVAL;
	}

	if (events & BRAID_IO_READ)
		EV_SET(&changes[n++], (uintptr_t)fd, EVFILT_READ, EV_ADD, 0, 0,
		       &conn->tag);
	if (events & BRAID_IO_WRITE)
		EV_SET(&changes[n++], (uintptr_t)fd, EVFILT_WRITE, EV_ADD, 0, 0,
		       &conn->tag);

	if (n == 0)
		return BRAID_OK;

	return kq_apply(pool->config.event_fd, changes, n);
}

int
io_modify(braid_pool_t *pool, int fd, uint32_t events)
{
	braid_conn_t *conn;
	struct kevent kev;
	struct kevent adds[2];
	int rc;
	int n = 0;

	if (table_lookup(pool, fd, &conn) != BRAID_OK) {
		BRAID_DEBUG_ASSERT(0, "io_modify: fd not in connection table");
		return BRAID_ERR_INVAL;
	}

	/*
	 * kqueue has no EPOLL_CTL_MOD equivalent.  Delete each filter
	 * individually: a missing-filter ENOENT on the first delete would
	 * abort the changelist before the second delete is submitted when
	 * nevents == 0 (kqueue stops at the first error it cannot store).
	 * kq_apply tolerates ENOENT.
	 * SAFETY: EV_DELETE does not dereference udata; pass NULL.
	 */
	EV_SET(&kev, (uintptr_t)fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
	rc = kq_apply(pool->config.event_fd, &kev, 1);
	if (rc != BRAID_OK)
		return rc;

	EV_SET(&kev, (uintptr_t)fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
	rc = kq_apply(pool->config.event_fd, &kev, 1);
	if (rc != BRAID_OK)
		return rc;

	if (events & BRAID_IO_READ)
		EV_SET(&adds[n++], (uintptr_t)fd, EVFILT_READ, EV_ADD, 0, 0,
		       &conn->tag);
	if (events & BRAID_IO_WRITE)
		EV_SET(&adds[n++], (uintptr_t)fd, EVFILT_WRITE, EV_ADD, 0, 0,
		       &conn->tag);

	if (n == 0)
		return BRAID_OK;

	return kq_apply(pool->config.event_fd, adds, n);
}

int
io_unwatch(const braid_pool_t *pool, int fd)
{
	struct kevent kev;
	int rc;

	/*
	 * Delete each filter individually: see io_modify comment above for
	 * why a batch EV_DELETE can silently skip filters.
	 */
	EV_SET(&kev, (uintptr_t)fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
	rc = kq_apply(pool->config.event_fd, &kev, 1);
	if (rc != BRAID_OK)
		return rc;

	EV_SET(&kev, (uintptr_t)fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
	return kq_apply(pool->config.event_fd, &kev, 1);
}
