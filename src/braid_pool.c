/*
 * braid_pool.c — pool lifecycle and public API implementation
 *
 * braid_pool_create(), braid_pool_destroy(), braid_pool_checkout(),
 * braid_pool_checkin(), braid_pool_cancel(), braid_pool_advance(),
 * braid_pool_notify().
 *
 * See ARCHITECTURE.md §11, §12, §13.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../include/braid.h"
#include "braid_internal.h"
#include "braid_conn.h"
#include "braid_io.h"
#include "braid_pool.h"
#include "braid_reaper.h"
#include "braid_reconnect.h"
#include "braid_table.h"
#include "braid_waitq.h"
