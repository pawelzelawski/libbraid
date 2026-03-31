/*
 * braid_reaper.c — idle reaper heap and reap logic
 *
 * Min-heap keyed on last_active_ms. Reaps connections exceeding
 * idle_reap_timeout, subject to the min_connections floor.
 * See ARCHITECTURE.md §7.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../include/braid.h"
#include "braid_internal.h"
#include "braid_reaper.h"
