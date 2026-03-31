/*
 * braid_reconnect.c — reconnection heap and backoff algorithm
 *
 * Min-heap keyed on next_retry_ms. Full jitter exponential backoff.
 * See ARCHITECTURE.md §6.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../include/braid.h"
#include "braid_internal.h"
#include "braid_reconnect.h"
