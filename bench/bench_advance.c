/*
 * bench_advance.c — braid_pool_advance() overhead benchmark
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "bench_common.h"

static double
measure_advance_ns_per_call(braid_pool_t *pool, uint32_t iterations)
{
	uint64_t t0;
	uint64_t t1;
	uint32_t next_ms;
	uint32_t i;

	t0 = bench_now_ns();
	for (i = 0; i < iterations; i++)
		braid_pool_advance(pool, &next_ms);
	t1 = bench_now_ns();

	return (double)(t1 - t0) / (double)iterations;
}

static int
scenario_idle_pool(void)
{
	int event_fd;
	braid_pool_t *pool;
	braid_config_t cfg;
	int err = 0;
	double ns_per_call;

	event_fd = bench_make_event_fd();
	if (event_fd < 0)
		return 1;

	memset(&cfg, 0, sizeof(cfg));
	cfg.host = "127.0.0.1";
	cfg.port = 1;
	cfg.event_fd = event_fd;
	cfg.min_connections = 0;
	cfg.max_connections = 100;

	pool = braid_pool_create(&cfg, &err);
	if (pool == NULL) {
		close(event_fd);
		return 1;
	}

	ns_per_call = measure_advance_ns_per_call(pool, 200000);
	printf("advance() idle pool: %.1f ns/op\n", ns_per_call);

	braid_pool_destroy(pool, 0);
	close(event_fd);
	return 0;
}

static int
scenario_full_idle_pool(void)
{
	pid_t server_pid = -1;
	uint16_t port = 0;
	int event_fd = -1;
	braid_pool_t *pool = NULL;
	braid_config_t cfg;
	int err = 0;
	double ns_per_call;
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
	cfg.min_connections = 100;
	cfg.max_connections = 100;

	pool = braid_pool_create(&cfg, &err);
	if (pool == NULL) {
		rc = 1;
		goto cleanup;
	}

	if (bench_warm_pool(pool, event_fd, 100, 30000) != BRAID_OK) {
		rc = 1;
		goto cleanup;
	}

	ns_per_call = measure_advance_ns_per_call(pool, 100000);
	printf("advance() full idle pool (100 conns): %.1f ns/op\n",
	       ns_per_call);

cleanup:
	if (pool != NULL)
		braid_pool_destroy(pool, 0);
	if (event_fd >= 0)
		close(event_fd);
	bench_stop_server(server_pid);
	return rc;
}

static int
scenario_mixed_pool(void)
{
	pid_t server_pid = -1;
	uint16_t port = 0;
	int event_fd = -1;
	braid_pool_t *pool = NULL;
	braid_config_t cfg;
	bench_checkout_rec_t rec;
	int active_fds[50];
	int err = 0;
	double ns_per_call;
	int i;
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
	cfg.min_connections = 100;
	cfg.max_connections = 100;

	pool = braid_pool_create(&cfg, &err);
	if (pool == NULL) {
		rc = 1;
		goto cleanup;
	}

	if (bench_warm_pool(pool, event_fd, 100, 30000) != BRAID_OK) {
		rc = 1;
		goto cleanup;
	}

	for (i = 0; i < 50; i++) {
		memset(&rec, 0, sizeof(rec));
		if (braid_pool_checkout(pool, 0, bench_checkout_cb, &rec,
					NULL) != BRAID_OK ||
		    !rec.called || rec.err != BRAID_OK) {
			rc = 1;
			goto cleanup;
		}
		active_fds[i] = rec.fd;
	}

	ns_per_call = measure_advance_ns_per_call(pool, 100000);
	printf("advance() mixed pool (50 active/50 idle): %.1f ns/op\n",
	       ns_per_call);

	for (i = 0; i < 50; i++)
		braid_pool_checkin(pool, active_fds[i], BRAID_CONN_OK);

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
	bench_print_hw_context("advance_overhead");

	if (scenario_idle_pool() != 0)
		return 1;
	if (scenario_full_idle_pool() != 0)
		return 1;
	if (scenario_mixed_pool() != 0)
		return 1;

	return 0;
}
