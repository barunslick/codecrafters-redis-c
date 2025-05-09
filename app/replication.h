#ifndef REPLICATION_H
#define REPLICATION_H

#include "state.h"

int connect_to_master(uint32_t host, uint16_t port);
void initiative_handshake(int master_fd, RedisStats *stats);
void handle_handshake_step(RedisStats *stats);
void send_rdb_file_to_slave(int connection_id, RedisStats *stats);
void read_rdb_file_from_master(int master_fd);

// Helper function to check replica acknowledgments and respond to waiting clients
uint64_t check_replica_acknowledgments(RedisStats *stats, uint64_t required_offset);
void respond_to_waiting_client(int connection_fd, uint64_t replica_ok_count);

// New functions
int process_rdb_data(RedisStats *stats, char *buf, int bytes_read);
int handle_handshake_response(RedisStats *stats, char *buf, int bytes_read);

#endif /* REPLICATION_H */
