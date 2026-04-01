/*
 * bench_common.h — shared helpers for libbraid benchmarks
 */

#ifndef BRAID_BENCH_COMMON_H
#define BRAID_BENCH_COMMON_H

#include <stdint.h>
#include <sys/types.h>

#include "../include/braid.h"

typedef struct {
	int called;
	int fd;
	int err;
	void *conn_ctx;
} bench_checkout_rec_t;

void bench_checkout_cb(int fd, void *conn_ctx, int err, void *cb_ctx);
uint64_t bench_now_ns(void);
int bench_make_event_fd(void);
int bench_start_server(pid_t *pid_out, uint16_t *port_out);
void bench_stop_server(pid_t pid);
int bench_event_loop_step(braid_pool_t *pool, int event_fd, int max_wait_ms);
uint32_t bench_pool_idle_count(braid_pool_t *pool);
uint32_t bench_fd_budget(void);
int bench_warm_pool(braid_pool_t *pool, int event_fd, uint32_t want_idle,
		    int max_steps);
void bench_print_hw_context(const char *name);

#endif /* BRAID_BENCH_COMMON_H */
