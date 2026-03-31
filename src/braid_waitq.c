/*
 * braid_waitq.c — wait queue ring buffer: enqueue, dequeue, cancel, expiry
 *
 * FIFO ring buffer of pending checkout requests. Cancellable via token.
 * Tombstone mechanism ensures one callback per checkout regardless of outcome.
 * See ARCHITECTURE.md §10.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../include/braid.h"
#include "braid_internal.h"
#include "braid_waitq.h"
