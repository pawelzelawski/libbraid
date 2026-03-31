/*
 * braid_pool.h — pool core internal interface (internal)
 *
 * Internal pool state flags and pool_drain_deferred() declaration.
 * See ARCHITECTURE.md §9, §13.
 */

#ifndef BRAID_POOL_H
#define BRAID_POOL_H

#include "../include/braid.h"
#include "braid_internal.h"

void pool_drain_deferred(braid_pool_t *pool);

#endif /* BRAID_POOL_H */
