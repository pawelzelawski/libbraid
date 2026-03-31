/*
 * braid_conn.h — connection record internal interface (internal)
 *
 * Connection record lifecycle, state transitions, socket creation.
 * See ARCHITECTURE.md §4, §5.
 */

#ifndef BRAID_CONN_H
#define BRAID_CONN_H

#include <netdb.h>

#include "../include/braid.h"
#include "braid_internal.h"

int conn_transition(braid_pool_t *pool, braid_conn_t *conn,
		    braid_state_t new_state);
int conn_alloc(braid_pool_t *pool, int fd, braid_conn_t **conn);
int conn_socket_create(braid_pool_t *pool, struct addrinfo *ai, int *fd);
int conn_keepalive_configure(int fd, const braid_config_t *config);

#endif /* BRAID_CONN_H */
