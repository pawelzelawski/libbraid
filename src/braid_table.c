/*
 * braid_table.c — connection hash table: insert, lookup, delete
 *
 * Open-addressed hash table keyed on fd, linear probing.
 * See ARCHITECTURE.md §3.
 */

#include <stdint.h>
#include <string.h>

#include "../include/braid.h"
#include "braid_internal.h"
#include "braid_table.h"
