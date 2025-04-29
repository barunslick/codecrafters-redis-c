#ifndef REPLICATION_H
#define REPLICATION_H

#include <stdint.h>

int connect_to_master(uint32_t host, uint16_t port);
void initiative_handshake(int master_fd, RedisStats *stats);
void send_rdb_file_to_slave(int connection_id, RedisStats *stats);

#endif /* REPLICATION_H */