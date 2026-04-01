/*
 * bench_reconnect.c — reconnection engine and heap throughput benchmark
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../src/braid_reconnect.h"
#include "bench_common.h"

static int
benchmark_heap_push_pop(void)
{
	braid_reconnect_heap_t heap;
	braid_reconnect_entry_t entry;
	braid_reconnect_entry_t out;
	uint64_t t0;
	uint64_t t1;
	uint32_t i;
	uint32_t ops = 400000;

	if (reconnect_heap_init(&heap, 4096) != BRAID_OK)
		return 1;

	t0 = bench_now_ns();
	for (i = 0; i < ops; i++) {
		entry.next_retry_ms =
		    (uint64_t)((i * 1103515245u) & 0x7fffffff);
		entry.attempt = i & 31u;
		if (reconnect_heap_push(&heap, entry) != BRAID_OK) {
			reconnect_heap_destroy(&heap);
			return 1;
		}
		if (reconnect_heap_pop(&heap, &out) != BRAID_OK) {
			reconnect_heap_destroy(&heap);
			return 1;
		}
	}
	t1 = bench_now_ns();

	printf("reconnect heap push+pop throughput: %.0f ops/s\n",
	       (double)ops / ((double)(t1 - t0) / 1000000000.0));

	reconnect_heap_destroy(&heap);
	return 0;
}

static int
benchmark_reconnect_advance_due_entries(void)
{
	int event_fd;
	braid_pool_t *pool;
	braid_config_t cfg;
	braid_reconnect_entry_t entry;
	uint64_t t0;
	uint64_t t1;
	uint32_t i;
	uint32_t due = 1000;
	int err = 0;

	event_fd = bench_make_event_fd();
	if (event_fd < 0)
		return 1;

	memset(&cfg, 0, sizeof(cfg));
	cfg.host = "127.0.0.1";
	cfg.port = 1;
	cfg.event_fd = event_fd;
	cfg.min_connections = 0;
	cfg.max_connections = due;
	cfg.backoff_base = 1;
	cfg.backoff_cap = 1;

	pool = braid_pool_create(&cfg, &err);
	if (pool == NULL) {
		close(event_fd);
		return 1;
	}

	memset(&entry, 0, sizeof(entry));
	for (i = 0; i < due; i++) {
		entry.next_retry_ms = 0;
		entry.attempt = 0;
		if (reconnect_heap_push(&pool->reconnect, entry) != BRAID_OK) {
			braid_pool_destroy(pool, 0);
			close(event_fd);
			return 1;
		}
	}

	t0 = bench_now_ns();
	reconnect_advance(pool, braid_now_ms());
	t1 = bench_now_ns();

	printf("reconnect_advance due-entry processing: %.0f entries/s\n",
	       (double)due / ((double)(t1 - t0) / 1000000000.0));

	braid_pool_destroy(pool, 0);
	close(event_fd);
	return 0;
}

int
main(void)
{
	bench_print_hw_context("reconnect_throughput");

	if (benchmark_heap_push_pop() != 0)
		return 1;
	if (benchmark_reconnect_advance_due_entries() != 0)
		return 1;

	return 0;
}
