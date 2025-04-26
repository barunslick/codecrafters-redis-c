#include <stdint.h>

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
        char role[8];
        char master_host[64];
        uint16_t master_port;
    } replication;


    // Some custom stats
    struct {
        char rdb_dir[124]; // Maybe exceed
        char rdb_filename[124]; // Maybe exceed
    } others;
    
} RedisStats;

RedisStats* init_redis_stats();