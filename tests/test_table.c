/*
 * test_table.c — unit tests for the connection hash table
 *
 * Tests: insert/lookup, probe-chain collisions, tombstone skip on
 * lookup, delete/vacate, tombstone reuse on insert, table-full error,
 * fd=0 as a valid key, and inline tag address stability.
 *
 * See TESTING.md §3.1 for the full test catalogue.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../include/braid.h"
#include "../src/braid_internal.h"
#include "../src/braid_table.h"
#include "test_harness.h"

/*
 * make_pool — allocate a minimal pool for table tests.
 * Only config.max_connections and the table fields are populated;
 * all other pool fields are safely zero from calloc.
 */
static braid_pool_t *
make_pool(uint32_t max_connections)
{
	braid_pool_t *pool;

	pool = calloc(1, sizeof(*pool));
	if (pool == NULL)
		return NULL;
	pool->config.max_connections = max_connections;
	if (table_init(pool) != BRAID_OK) {
		free(pool);
		return NULL;
	}
	return pool;
}

static void
free_pool(braid_pool_t *pool)
{
	table_destroy(pool);
	free(pool);
}

/*
 * table_init must reject max_connections=0 to prevent zero-sized table.
 */
static void
test_table_init_rejects_zero_max_connections(void)
{
	braid_pool_t pool;

	memset(&pool, 0, sizeof(pool));
	pool.config.max_connections = 0;
	CHECK_ERR("table_init rejects max_connections=0", table_init(&pool),
		  BRAID_ERR_INVAL);
}

/*
 * table_init must reject max_connections that overflow 2*max_connections.
 */
static void
test_table_init_rejects_overflow_max_connections(void)
{
	braid_pool_t pool;

	memset(&pool, 0, sizeof(pool));
	pool.config.max_connections = UINT32_MAX;
	CHECK_ERR("table_init rejects table size overflow", table_init(&pool),
		  BRAID_ERR_INVAL);
}

/*
 * insert_fd — helper: insert a zeroed record with the given fd.
 * Returns a pointer to the table slot on success, NULL on failure.
 */
static braid_conn_t *
insert_fd(braid_pool_t *pool, int fd)
{
	braid_conn_t tmp, *slot;

	memset(&tmp, 0, sizeof(tmp));
	tmp.fd = fd;
	slot = &tmp;
	if (table_insert(pool, &slot) != BRAID_OK)
		return NULL;
	return slot;
}

/* ── test cases ──────────────────────────────────────────────────────── */

/*
 * Insert fd=5 and verify lookup returns the same slot with fd=5.
 */
static void
test_insert_and_lookup(void)
{
	braid_pool_t *pool;
	braid_conn_t *slot, *found;

	pool = make_pool(4);
	if (pool == NULL) {
		tests_failed++;
		return;
	}

	slot = insert_fd(pool, 5);
	CHECK("insert fd=5 succeeds", slot != NULL);
	CHECK_ERR("lookup fd=5 returns OK", table_lookup(pool, 5, &found),
		  BRAID_OK);
	CHECK("lookup returns correct fd", found != NULL && found->fd == 5);
	CHECK("lookup returns same slot pointer", found == slot);

	free_pool(pool);
}

/*
 * Insert two fds that hash to the same slot (fd=1 and fd=9 with
 * table_size=8 both hash to slot 1).  Both must be retrievable.
 */
static void
test_probe_chain_collision(void)
{
	braid_pool_t *pool;
	braid_conn_t *slot1, *slot2, *found1, *found2;

	pool = make_pool(4); /* table_size=8; 1%8==9%8==1 */
	if (pool == NULL) {
		tests_failed++;
		return;
	}

	slot1 = insert_fd(pool, 1);
	slot2 = insert_fd(pool, 9); /* collides with fd=1 */
	CHECK("both inserts succeed", slot1 != NULL && slot2 != NULL);
	CHECK("collision lands in different slots", slot1 != slot2);

	CHECK_ERR("lookup fd=1 after collision returns OK",
		  table_lookup(pool, 1, &found1), BRAID_OK);
	CHECK_ERR("lookup fd=9 after collision returns OK",
		  table_lookup(pool, 9, &found2), BRAID_OK);
	CHECK("fd=1 found correctly", found1 != NULL && found1->fd == 1);
	CHECK("fd=9 found correctly", found2 != NULL && found2->fd == 9);

	free_pool(pool);
}

/*
 * fd=1 and fd=9 collide at slot 1.  Delete fd=1 (makes tombstone).
 * Lookup fd=9 must skip the tombstone and find fd=9 at the next slot.
 */
static void
test_tombstone_skip_on_lookup(void)
{
	braid_pool_t *pool;
	braid_conn_t *found;

	pool = make_pool(4);
	if (pool == NULL) {
		tests_failed++;
		return;
	}

	insert_fd(pool, 1);
	insert_fd(pool, 9);
	table_delete(pool, 1);

	CHECK_ERR("lookup fd=9 after tombstone returns OK",
		  table_lookup(pool, 9, &found), BRAID_OK);
	CHECK("tombstone skipped — fd=9 found",
	      found != NULL && found->fd == 9);

	free_pool(pool);
}

/*
 * Insert fd=7, delete it, then verify lookup returns not-found.
 */
static void
test_delete_vacates_slot(void)
{
	braid_pool_t *pool;
	braid_conn_t *found;

	pool = make_pool(4);
	if (pool == NULL) {
		tests_failed++;
		return;
	}

	insert_fd(pool, 7);
	CHECK_ERR("delete fd=7 returns OK", table_delete(pool, 7), BRAID_OK);

	found = NULL;
	CHECK("deleted fd not found",
	      table_lookup(pool, 7, &found) != BRAID_OK);
	CHECK("lookup output NULL after delete", found == NULL);

	free_pool(pool);
}

/*
 * Insert fd=1, delete it (tombstone), re-insert fd=1.
 * The second insert must reuse the tombstone slot — not a later empty
 * slot — so that the inline tag address (&conn->tag) is unchanged and
 * no epoll re-registration is required.
 */
static void
test_tombstone_reuse_on_insert(void)
{
	braid_pool_t *pool;
	braid_conn_t *slot_first, *slot_second, *found;

	pool = make_pool(4);
	if (pool == NULL) {
		tests_failed++;
		return;
	}

	slot_first = insert_fd(pool, 1);
	CHECK("first insert succeeds", slot_first != NULL);

	table_delete(pool, 1);

	slot_second = insert_fd(pool, 1);
	CHECK("re-insert after tombstone succeeds", slot_second != NULL);

	CHECK("tombstone slot reused (slot addresses equal)",
	      slot_first == slot_second);
	CHECK("inline tag address unchanged after tombstone reuse",
	      &slot_first->tag == &slot_second->tag);

	CHECK_ERR("re-inserted fd=1 is findable", table_lookup(pool, 1, &found),
		  BRAID_OK);
	CHECK("found fd correct after reuse", found != NULL && found->fd == 1);

	free_pool(pool);
}

/*
 * Fill all table_size slots (max_connections=2 → table_size=4) then
 * verify the next insert returns BRAID_ERR_EXHAUSTED.
 */
static void
test_table_full_returns_error(void)
{
	braid_pool_t *pool;
	braid_conn_t tmp, *slot;
	uint32_t i;
	int all_ok, rc;

	pool = make_pool(2); /* table_size=4 */
	if (pool == NULL) {
		tests_failed++;
		return;
	}

	all_ok = 1;
	for (i = 0; i < pool->table_size; i++) {
		memset(&tmp, 0, sizeof(tmp));
		tmp.fd = (int)i;
		slot = &tmp;
		if (table_insert(pool, &slot) != BRAID_OK) {
			all_ok = 0;
			break;
		}
	}
	CHECK("table_size inserts succeed before full", all_ok == 1);

	memset(&tmp, 0, sizeof(tmp));
	tmp.fd = (int)pool->table_size; /* hashes to slot 0: probes all */
	slot = &tmp;
	rc = table_insert(pool, &slot);
	CHECK_ERR("insert into full table returns EXHAUSTED", rc,
		  BRAID_ERR_EXHAUSTED);

	free_pool(pool);
}

/*
 * fd=0 is a valid file descriptor and must not be confused with the
 * empty-slot sentinel value (-1).
 */
static void
test_fd_zero_valid_key(void)
{
	braid_pool_t *pool;
	braid_conn_t *slot, *found;

	pool = make_pool(4);
	if (pool == NULL) {
		tests_failed++;
		return;
	}

	slot = insert_fd(pool, 0);
	CHECK("insert fd=0 succeeds", slot != NULL);
	CHECK_ERR("lookup fd=0 returns OK", table_lookup(pool, 0, &found),
		  BRAID_OK);
	CHECK("lookup fd=0 finds correct record",
	      found != NULL && found->fd == 0);

	free_pool(pool);
}

/* ── suite entry point ───────────────────────────────────────────────── */

void
run_table_tests(void)
{
	test_table_init_rejects_zero_max_connections();
	test_table_init_rejects_overflow_max_connections();
	test_insert_and_lookup();
	test_probe_chain_collision();
	test_tombstone_skip_on_lookup();
	test_delete_vacates_slot();
	test_tombstone_reuse_on_insert();
	test_table_full_returns_error();
	test_fd_zero_valid_key();
}
