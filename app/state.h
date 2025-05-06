#ifndef STATE_H
#define STATE_H

#include <stdint.h>
#include "dlist.h"

// Define enum for Redis role
typedef enum {
    ROLE_MASTER = 0,
    ROLE_SLAVE = 1
} RedisRole;

typedef struct {
    // Server section
    struct {
        char redis_version[16];  
        char os[64];
        uint16_t tcp_port;
    } server;

    // Clients section
    struct {
        uint64_t connected_clients;
        uint64_t total_connections_received;
        uint64_t blocked_clients;
        uint64_t maxclients;
    } clients;

    struct {
        RedisRole role;  // Using enum instead of string
        char role_str[8]; // Keep string version for INFO command output
        uint32_t master_host;
        uint16_t master_port;
        char master_replid[64];
        uint64_t master_repl_offset;
        int master_fd;
    } replication;


    // Some custom stats
    struct {
        char rdb_dir[124]; // Maybe exceed
        char rdb_filename[124]; // Maybe exceed
        Llist* connected_clients;
        Llist* connected_slaves;
    } others;
    
} RedisStats;

RedisStats* init_redis_stats();
const char* get_role_str(RedisRole role);

#endif /* STATE_H */