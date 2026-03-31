/*
 * braid_table.h — connection hash table (internal)
 *
 * Open-addressed hash table keyed on fd, linear probing.
 * Fixed size: 2 × max_connections slots.
 * See ARCHITECTURE.md §3.
 */

#ifndef BRAID_TABLE_H
#define BRAID_TABLE_H

#include "../include/braid.h"
#include "braid_internal.h"

int table_init(braid_pool_t *pool);
void table_destroy(braid_pool_t *pool);
int table_lookup(braid_pool_t *pool, int fd, braid_conn_t **conn);
int table_insert(braid_pool_t *pool, braid_conn_t **conn);
int table_delete(braid_pool_t *pool, int fd);

#endif /* BRAID_TABLE_H */
