#ifndef STATE_H
#define STATE_H

#include <stdbool.h>

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
    int is_reading;
    size_t bytes_read;
} BytesRead;

// Struct to store replica connection and offset information
typedef struct {
    int connection_fd;
    uint64_t last_ack_offset;
} ReplicaInfo;

typedef struct {
  int connection_fd;
  uint64_t master_offset;
  uint64_t minimum_replica_count;
  uint64_t expiry;
} WaitingClientInfo;

typedef struct {
  // Server section
  struct {
    char redis_version[16];
    char os[64];
    uint16_t tcp_port;
    uint64_t offset;
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
    int master_fd;
    uint64_t master_repl_offset;
    HandshakeState handshake_state; // Track handshake progress
    BytesRead* bytes_read; // Track bytes read during replication
  } replication;

  // Some custom stats
  struct {
    char rdb_dir[124];      // Maybe exceed
    char rdb_filename[124]; // Maybe exceed
    Llist *connected_clients;
    Llist *connected_slaves;
    Llist *waiting_clients;
    int is_replication_completed;
  } others;

} RedisStats;

RedisStats *init_redis_stats();
const char *get_role_str(RedisRole role);
ReplicaInfo* create_replica_info(int connection_fd);
WaitingClientInfo* create_waiting_client_info(int connection_fd, uint64_t master_offset, uint64_t minimum_replica_count, uint64_t relative_expiry);

#endif /* STATE_H */
