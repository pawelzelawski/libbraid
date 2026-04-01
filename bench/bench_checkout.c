/*
 * bench_checkout.c — checkout/checkin round-trip latency benchmark
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "bench_common.h"

static int
run_case(uint32_t pool_size, uint32_t iterations)
{
	pid_t server_pid = -1;
	uint16_t port = 0;
	int event_fd = -1;
	braid_pool_t *pool = NULL;
	braid_config_t cfg;
	bench_checkout_rec_t rec;
	uint64_t t0;
	uint64_t t1;
	uint32_t i;
	int err = 0;
	int rc = 0;

	if (bench_start_server(&server_pid, &port) != BRAID_OK)
		return 1;

	event_fd = bench_make_event_fd();
	if (event_fd < 0) {
		rc = 1;
		goto cleanup;
	}

	memset(&cfg, 0, sizeof(cfg));
	cfg.host = "127.0.0.1";
	cfg.port = port;
	cfg.event_fd = event_fd;
	cfg.min_connections = pool_size;
	cfg.max_connections = pool_size;
	cfg.connect_timeout = 1000;
	cfg.init_timeout = 1000;
	cfg.validate_timeout = 1000;

	pool = braid_pool_create(&cfg, &err);
	if (pool == NULL) {
		rc = 1;
		goto cleanup;
	}

	if (bench_warm_pool(pool, event_fd, pool_size, 20000) != BRAID_OK) {
		rc = 1;
		goto cleanup;
	}

	t0 = bench_now_ns();
	for (i = 0; i < iterations; i++) {
		memset(&rec, 0, sizeof(rec));
		if (braid_pool_checkout(pool, 0, bench_checkout_cb, &rec,
					NULL) != BRAID_OK) {
			rc = 1;
			goto cleanup;
		}
		if (!rec.called || rec.err != BRAID_OK || rec.fd < 0) {
			rc = 1;
			goto cleanup;
		}
		if (braid_pool_checkin(pool, rec.fd, BRAID_CONN_OK) !=
		    BRAID_OK) {
			rc = 1;
			goto cleanup;
		}
	}
	t1 = bench_now_ns();

	printf(
	    "checkout with immediate connection (pool size %u): %.1f ns/op\n",
	    pool_size, (double)(t1 - t0) / (double)iterations);

cleanup:
	if (pool != NULL)
		braid_pool_destroy(pool, 0);
	if (event_fd >= 0)
		close(event_fd);
	bench_stop_server(server_pid);
	return rc;
}

int
main(void)
{
	bench_print_hw_context("checkout_latency");

	if (run_case(10, 20000) != 0)
		return 1;
	if (run_case(100, 20000) != 0)
		return 1;
	if (run_case(500, 10000) != 0)
		return 1;
	if (run_case(1000, 5000) != 0)
		return 1;

	return 0;
}
