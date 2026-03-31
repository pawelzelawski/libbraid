/*
 * braid_table.c — connection hash table: insert, lookup, delete
 *
 * Open-addressed hash table keyed on fd, linear probing.
 * Table size is 2 × max_connections; load factor never exceeds 0.5.
 * See ARCHITECTURE.md §3.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../include/braid.h"
#include "braid_internal.h"
#include "braid_table.h"

/*
 * table_init — allocate 2 × max_connections slots and mark all empty.
 * Called once from braid_pool_create(). No allocation after this point.
 */
int
table_init(braid_pool_t *pool)
{
	uint32_t i;

	pool->table_size = pool->config.max_connections * 2;
	pool->table = calloc(pool->table_size, sizeof(braid_conn_t));
	if (pool->table == NULL)
		return BRAID_ERR_NOMEM;

	for (i = 0; i < pool->table_size; i++)
		pool->table[i].fd = -1;

	return BRAID_OK;
}

/*
 * table_destroy — free the slot array. Called from braid_pool_destroy().
 */
void
table_destroy(braid_pool_t *pool)
{
	free(pool->table);
	pool->table = NULL;
}

/*
 * table_lookup — find the record for fd using linear probing.
 * Tombstones are skipped; an empty slot terminates the search.
 * Sets *conn to the record on success, NULL on failure.
 * Returns BRAID_OK if found, BRAID_ERR_INVAL if not found.
 */
int
table_lookup(braid_pool_t *pool, int fd, braid_conn_t **conn)
{
	braid_conn_t *s;
	uint32_t i, slot;

	*conn = NULL;

	for (i = 0; i < pool->table_size; i++) {
		slot = ((uint32_t)fd + i) % pool->table_size;
		s = &pool->table[slot];

		if (s->flags & CONN_FLAG_TOMBSTONE)
			continue; /* tombstone: chain continues */
		if (s->fd == -1)
			break; /* empty: end of chain */
		if (s->fd == fd) {
			*conn = s;
			return BRAID_OK;
		}
	}

	return BRAID_ERR_INVAL;
}

/*
 * table_insert — find the first tombstone or empty slot on (*conn)->fd's
 * probe chain; copy the record into that slot; clear any tombstone flag;
 * update *conn to point to the in-table copy.
 *
 * Prefers a tombstone slot over an empty one further in the chain to
 * keep probe chains compact. Returns BRAID_ERR_EXHAUSTED if no free
 * slot exists — a programming error under normal use, as the table is
 * sized at 2 × max_connections.
 */
int
table_insert(braid_pool_t *pool, braid_conn_t **conn)
{
	braid_conn_t *s, *tombstone;
	uint32_t i, slot;
	int fd;

	fd = (*conn)->fd;
	tombstone = NULL;

	for (i = 0; i < pool->table_size; i++) {
		slot = ((uint32_t)fd + i) % pool->table_size;
		s = &pool->table[slot];

		if (s->flags & CONN_FLAG_TOMBSTONE) {
			if (tombstone == NULL)
				tombstone = s;
			continue;
		}
		if (s->fd == -1) {
			/* Empty slot: use tombstone if one was found earlier */
			if (tombstone != NULL)
				s = tombstone;
			*s = **conn;
			s->flags &= ~(uint32_t)CONN_FLAG_TOMBSTONE;
			*conn = s;
			return BRAID_OK;
		}
	}

	/* No empty slot; reuse first tombstone if available */
	if (tombstone != NULL) {
		*tombstone = **conn;
		tombstone->flags &= ~(uint32_t)CONN_FLAG_TOMBSTONE;
		*conn = tombstone;
		return BRAID_OK;
	}

	return BRAID_ERR_EXHAUSTED;
}

/*
 * table_delete — mark the slot for fd as a tombstone: set
 * CONN_FLAG_TOMBSTONE and fd = -1. Probe chains for other records on
 * the same chain are preserved. No compaction — inline tag addresses
 * must remain stable for epoll_data.ptr correctness. See §3.3.
 */
int
table_delete(braid_pool_t *pool, int fd)
{
	braid_conn_t *conn;
	int rc;

	rc = table_lookup(pool, fd, &conn);
	if (rc != BRAID_OK)
		return rc;

	conn->flags |= CONN_FLAG_TOMBSTONE;
	conn->fd = -1;
	return BRAID_OK;
}
