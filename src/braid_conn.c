/*
 * braid_conn.c — connection record lifecycle and state transitions
 *
 * conn_transition() is the single enforcement point for all state writes.
 * See ARCHITECTURE.md §4, §5.
 */

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "../include/braid.h"
#include "braid_internal.h"
#include "braid_conn.h"
