#ifndef COMMANDS_H
#define COMMANDS_H

#include "hashtable.h"
#include "helper.h"
#include "resp.h"
#include "state.h"

// Redis configuration structure
typedef struct {
    char *dir;
    char *dbfilename;
    uint16_t port;
} RedisConfig;

// Command type enum
typedef enum {
    CMD_PING,
    CMD_ECHO,
    CMD_SET,
    CMD_GET,
    CMD_DEL,
    CMD_CONFIG,
    CMD_KEYS,
    CMD_INFO,
    CMD_UNKNOWN,
    CMD_REPLCONF,
    CMD_PSYNC,
} CommandType;

// Command functions
void process_command(int connection_fd, RESPData* request, ht_table* ht, RedisStats* stats);
void handle_ping(int connection_fd);
void handle_echo(int connection_fd, RESPData* request);
void handle_set(int connection_fd, RESPData* request, ht_table* ht);
void handle_get(int connection_fd, RESPData* request, ht_table* ht);
void handle_del(int connection_fd, RESPData* request, ht_table* ht);
void handle_config(int connection_fd, RESPData* request, RedisStats* stats);
void handle_keys(int connection_fd, RESPData* request, ht_table* ht);
void handle_info(int connection_fd, RESPData* request, RedisStats* stats);
void handle_replconf(int connection_fd, RESPData* request);
void handle_psync(int connection_fd, RESPData* request, RedisStats* stats);

#endif // COMMANDS_H