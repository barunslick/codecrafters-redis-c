#ifndef STATE_H
#define STATE_H

#include "dlist.h"
#include <stdint.h>

// Define enum for Redis role
typedef enum { ROLE_MASTER = 0, ROLE_SLAVE = 1 } RedisRole;

// Define enum for replica handshake state
typedef enum {
  HANDSHAKE_NOT_STARTED = 0,
  HANDSHAKE_PING_SENT = 1,
  HANDSHAKE_PORT_SENT = 2,
  HANDSHAKE_CAPA_SENT = 3,
  HANDSHAKE_PSYNC_SENT = 4,
  HANDSHAKE_COMPLETED = 5
} HandshakeState;

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
    RedisRole role;   // Using enum instead of string
    char role_str[8]; // Keep string version for INFO command output
    uint32_t master_host;
    uint16_t master_port;
    char master_replid[64];
    uint64_t master_repl_offset;
    int master_fd;
    HandshakeState handshake_state; // Track handshake progress
  } replication;

  // Some custom stats
  struct {
    char rdb_dir[124];      // Maybe exceed
    char rdb_filename[124]; // Maybe exceed
    Llist *connected_clients;
    Llist *connected_slaves;
    int is_replication_completed;
  } others;

} RedisStats;

RedisStats *init_redis_stats();
const char *get_role_str(RedisRole role);

#endif /* STATE_H */
