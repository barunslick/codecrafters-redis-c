#include <stdlib.h>
#include <stdio.h>

#include "helper.h"
#include "state.h"

// Initializer function
RedisStats* init_redis_stats() {

    RedisStats* stats = malloc(sizeof(RedisStats));
    if (!stats) return NULL;
    
    get_os_info(stats->server.os, sizeof(stats->server.os));
    
    stats->server.tcp_port = DEFAULT_REDIS_PORT;
    
    // Initialize clients section
    stats->clients.connected_clients = 0;
    stats->clients.total_connections_received = 0;
    stats->clients.blocked_clients = 0;
    stats->clients.maxclients = 10000; // Default value


    // Initialize replication section
    snprintf(stats->replication.role, sizeof(stats->replication.role), "master");

    // Initialize others section
    snprintf(stats->others.rdb_dir, sizeof(stats->others.rdb_dir), "/var/lib/redis");
    snprintf(stats->others.rdb_filename, sizeof(stats->others.rdb_filename), "dump.rdb");
    
    return stats;
}
