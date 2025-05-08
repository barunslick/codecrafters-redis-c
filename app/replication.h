#ifndef REPLICATION_H
#define REPLICATION_H

#include "state.h"

int connect_to_master(uint32_t host, uint16_t port);
void initiative_handshake(int master_fd, RedisStats *stats);
void handle_handshake_step(RedisStats *stats);
void send_rdb_file_to_slave(int connection_id, RedisStats *stats);
void read_rdb_file_from_master(int master_fd);

// New functions
int process_rdb_data(RedisStats *stats, char *buf, int bytes_read);
int handle_handshake_response(RedisStats *stats, char *buf, int bytes_read);

#endif /* REPLICATION_H */
