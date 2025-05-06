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


// Command functions
void process_command(int connection_fd, RESPData* parsed_request, char* raw_buffer, ht_table* ht, RedisStats* stats);
void process_commands_in_buffer(int connection_fd, ht_table *ht, RedisStats *stats, 
                              char *buf, int bytes_read);
size_t handle_ping(char* write_buf, size_t buf_size);
size_t handle_echo(char* write_buf, size_t buf_size, RESPData* request);
size_t handle_set(char* write_buf, size_t buf_size, RESPData* request, ht_table* ht);
size_t handle_get(char* write_buf, size_t buf_size, RESPData* request, ht_table* ht);
size_t handle_del(char* write_buf, size_t buf_size, RESPData* request, ht_table* ht);
size_t handle_config(char* write_buf, size_t buf_size, RESPData* request, RedisStats* stats);
size_t handle_keys(char* write_buf, size_t buf_size, RESPData* request, ht_table* ht);
size_t handle_info(char* write_buf, size_t buf_size, RESPData* request, RedisStats* stats);
size_t handle_replconf(char* write_buf, size_t buf_size, RESPData* request, RedisStats* stats);
void handle_psync(int connection_fd, RESPData *request, RedisStats *stats);

#endif // COMMANDS_H