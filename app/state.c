#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dlist.h"
#include "helper.h"
#include "state.h"

// Helper function to convert a RedisRole enum to string
const char *get_role_str(RedisRole role) {
  switch (role) {
  case ROLE_MASTER:
    return "master";
  case ROLE_SLAVE:
    return "slave";
  default:
    return "unknown";
  }
}

// Initializer function
RedisStats *init_redis_stats() {
  RedisStats *stats = malloc(sizeof(RedisStats));
  if (!stats)
    return NULL;

  get_os_info(stats->server.os, sizeof(stats->server.os));

  stats->server.tcp_port = DEFAULT_REDIS_PORT;

  // Initialize clients section
  stats->clients.connected_clients = 0;
  stats->clients.total_connections_received = 0;
  stats->clients.blocked_clients = 0;
  stats->clients.maxclients = 10000; // Default value

  // Initialize replication section
  stats->replication.role = ROLE_MASTER;
  strncpy(stats->replication.role_str, "master",
          sizeof(stats->replication.role_str));
  stats->replication.master_host = 0;                  // Default value
  stats->replication.master_port = DEFAULT_REDIS_PORT; // Default value
  stats->replication.handshake_state = HANDSHAKE_NOT_STARTED; // Initialize handshake state

  // Initialize others section
  snprintf(stats->others.rdb_dir, sizeof(stats->others.rdb_dir),
           "/var/lib/redis");
  snprintf(stats->others.rdb_filename, sizeof(stats->others.rdb_filename),
           "dump.rdb");

  // Set replid to 8371b4fb1155b71f4a04d3e1bc3e18c4a990aeeb
  snprintf(stats->replication.master_replid,
           sizeof(stats->replication.master_replid),
           "8371b4fb1155b71f4a04d3e1bc3e18c4a990aeeb");
  stats->replication.master_repl_offset = 0;
  stats->replication.master_fd = -1; // Default value

  stats->others.connected_clients = create_list();
  stats->others.connected_slaves = create_list(); // Skip this later for slaves
  stats->others.is_replication_completed = 0;

  return stats;
}
