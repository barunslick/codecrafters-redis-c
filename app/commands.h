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
void handle_psync(int connection_fd, RESPData *request, RedisStats *stats);

#endif // COMMANDS_H