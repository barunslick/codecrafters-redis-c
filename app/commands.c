#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/socket.h>

#include "commands.h"
#include "replication.h"
#include "dlist.h"

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

// Command specification with max and min arguments
typedef struct {
    CommandType type;
    int min_args;
    int max_args;
    const char* name;
    bool should_send_to_slave;
} CommandInfo;

// Command specification with max and min arguments
static const CommandInfo COMMANDS[] = {
    {CMD_PING, 1, 1, "PING", 0},
    {CMD_ECHO, 2, 2, "ECHO", 0},
    {CMD_SET, 3, 5, "SET", 1},
    {CMD_GET, 2, 2, "GET", 0},
    {CMD_DEL, 2, 2, "DEL", 1},
    {CMD_KEYS, 2, 2, "KEYS", 0},
    {CMD_CONFIG, 3, 3, "CONFIG", 0},
    {CMD_INFO, 2, 2, "INFO", 0},
    {CMD_REPLCONF, 3, 10, "REPLCONF", 0},
    {CMD_PSYNC, 3, 3, "PSYNC", 0},
};

// Command validation and parsing
static CommandType get_command_type(const char* cmd_str) {
    for (size_t i = 0; i < sizeof(COMMANDS) / sizeof(COMMANDS[0]); i++) {
        if (strcasecmp(cmd_str, COMMANDS[i].name) == 0)
            return COMMANDS[i].type;
    }
    return CMD_UNKNOWN;
}

static CommandInfo get_command_info(const char* cmd_str) {
    for (size_t i = 0; i < sizeof(COMMANDS) / sizeof(COMMANDS[0]); i++) {
        if (strcasecmp(cmd_str, COMMANDS[i].name) == 0)
            return COMMANDS[i];
    }
    CommandInfo unknown_cmd = {CMD_UNKNOWN, -1, -1, "UNKNOWN", 0};
    return unknown_cmd;
}

static bool validate_command_args(CommandType cmd, size_t arg_count) {
    for (size_t i = 0; i < sizeof(COMMANDS) / sizeof(COMMANDS[0]); i++) {
        if (COMMANDS[i].type == cmd)
            return COMMANDS[i].max_args == -1 || (arg_count >= COMMANDS[i].min_args && (arg_count <= COMMANDS[i].max_args));
    }
    return false;
}

// Command handlers
void handle_ping(int connection_fd) {
    say(connection_fd, "+PONG\r\n");
}

void handle_echo(int connection_fd, RESPData* request) {
    char output[1024];
    sprintf(output, "+%s\r\n", request->data.array.elements[1]->data.str);
    say(connection_fd, output);
}

void handle_set(int connection_fd, RESPData* request, ht_table* ht) {
    const char* key = request->data.array.elements[1]->data.str;
    void* value = request->data.array.elements[2]->data.str;

    uint64_t expiry = 0;
    if (request->data.array.count > 3 && strcasecmp(request->data.array.elements[3]->data.str, "px") == 0)
        expiry = (uint64_t) strtol(request->data.array.elements[4]->data.str, NULL, 10);

    if (ht_set_with_relative_expiry(ht, key, value, expiry) == NULL) {
        say(connection_fd, "-ERR failed to set key\r\n");
        return;
    }

    say(connection_fd, "+OK\r\n");
}

void handle_get(int connection_fd, RESPData* request, ht_table* ht) {
    const char* key = request->data.array.elements[1]->data.str;
    char* value = ht_get(ht, key);

    if (value == NULL) {
        say(connection_fd, "$-1\r\n");
    } else {
        char output[1024];
        sprintf(output, "$%ld\r\n%s\r\n", strlen(value), value);
        say(connection_fd, output);
    }
}

void handle_del(int connection_fd, RESPData* request, ht_table* ht) {
    ht_del(ht, request->data.array.elements[1]->data.str);
    say(connection_fd, ":1\r\n");
}

void handle_config(int connection_fd, RESPData* request, RedisStats* stats) {
    if (request->data.array.count < 2) {
        say(connection_fd, "-ERR CONFIG requires at least one argument\r\n");
        return;
    }
    
    if (strcmp(request->data.array.elements[1]->data.str, "GET") == 0) {
        if (strcmp(request->data.array.elements[2]->data.str, "dir") == 0) {
            const char *values[] = {"dir", stats->others.rdb_dir};
            char* resp = convert_to_resp_array(2, values);
            say(connection_fd, resp);
            free(resp);
        } else if (strcmp(request->data.array.elements[2]->data.str, "dbfilename") == 0) {
            const char *values[] = {"dbfilename", stats->others.rdb_filename};
            char* resp = convert_to_resp_array(2, values);
            say(connection_fd, resp);
            free(resp);
        } else {
            say(connection_fd, "-ERR Unknown CONFIG parameter\r\n");
        }
    } else {
        say(connection_fd, "-ERR Unknown CONFIG command\r\n");
    }
}

void handle_keys(int connection_fd, RESPData* request, ht_table* ht) {
    const char *pattern = request->data.array.elements[1]->data.str;
    // For now, return all keys without pattern matching
    size_t count = 0;
    const char** keys = ht_get_keys(ht, &count);
    char *keys_resp = convert_to_resp_array(count, keys);
    say(connection_fd, keys_resp);
    free(keys_resp);
    free(keys);
}


void handle_info(int connection_fd, RESPData* request, RedisStats* stats) {
    const char *info_type = request->data.array.elements[1]->data.str;

    if (strcmp(info_type, "replication") == 0) {
        // Add # Replication heading
        size_t buffer_size = 1024;
        size_t cursor = 0;
        char* buffer = malloc(buffer_size);
        if (buffer == NULL) {
            exit_with_error("Memory allocation failed");
        }

        cursor += snprintf(buffer + cursor, buffer_size - cursor, "# Replication\r\n");
        cursor += snprintf(buffer + cursor, buffer_size - cursor, "role:%s\r\n", stats->replication.role_str);
        cursor += snprintf(buffer + cursor, buffer_size - cursor, "master_replid:%s\r\n", stats->replication.master_replid);
        cursor += snprintf(buffer + cursor, buffer_size - cursor, "master_repl_offset:%lu\r\n", stats->replication.master_repl_offset);

        char* resp = convert_to_resp_string(buffer);
        say(connection_fd, resp);
        free(buffer);
        free(resp);
    } else {
        say(connection_fd, "-ERR Unknown INFO type\r\n");
    }
}


void handle_replconf(int connection_fd, RESPData* request) {
    if (strcmp(request->data.array.elements[1]->data.str, "listening-port") == 0) {
        // TODO: Handle listening-port later
        say(connection_fd, "+OK\r\n");
    } else if (strcmp(request->data.array.elements[1]->data.str, "capa") == 0) {
        // TODO: Handle capa later
        say(connection_fd, "+OK\r\n");
    } else {
        say(connection_fd, "-ERR Unknown REPLCONF command\r\n");
    }

    return;
}

void handle_psync(int connection_fd, RESPData* request, RedisStats* stats) {
    // Check if it is slave
    if (stats->replication.role == ROLE_SLAVE) {
        say(connection_fd, "-ERR PSYNC not supported in slave mode\r\n");
        return;
    }

    // Create a buffer to hold the response
    char buffer[1024];

    sprintf(buffer, "FULLRESYNC %s 0\r\n", stats->replication.master_replid);

    // Send the response to the client
    char* resp = convert_to_resp_string(buffer);
    say(connection_fd, resp);
    free(resp);

    send_rdb_file_to_slave(connection_fd, stats);

    // Set the slave as ready to read. Probably need to remove from here later.
	int* value = malloc(sizeof(int));
	*value = connection_fd;
    if (add_to_list_tail(stats->others.connected_slaves, value) == NULL) {
        exit_with_error("Internal Error");
    }

    return;
}

// ----------------- Main command processor ----------------------------
// ---------------------------------------------------------------------
void process_command(int connection_fd, RESPData* parsed_request, char* raw_buffer, ht_table* ht, RedisStats* stats) {
    if (parsed_request == NULL || parsed_request->type != RESP_ARRAY || parsed_request->data.array.count == 0) {
        say(connection_fd, "-ERR Invalid request\r\n");
        return;
    }

    const char* cmd_str = parsed_request->data.array.elements[0]->data.str;
    CommandInfo cmd = get_command_info(cmd_str);
    CommandType cmd_type = cmd.type;

    if (cmd_type == CMD_UNKNOWN) {
        say(connection_fd, "-ERR unknown command\r\n");
        return;
    }
    
    if (!validate_command_args(cmd_type, parsed_request->data.array.count)) {
        say(connection_fd, "-ERR wrong number of arguments\r\n");
        return;
    }

    switch (cmd_type) {
        case CMD_PING:
            handle_ping(connection_fd);
            break;
        case CMD_ECHO:
            handle_echo(connection_fd, parsed_request);
            break;
        case CMD_SET:
            // Check if the command should be sent to the slave
            handle_set(connection_fd, parsed_request, ht);
            break;
        case CMD_GET:
            handle_get(connection_fd, parsed_request, ht);
            break;
        case CMD_DEL:
            handle_del(connection_fd, parsed_request, ht);
            break;
        case CMD_CONFIG:
            handle_config(connection_fd, parsed_request, stats);
            break;
        case CMD_KEYS:
            handle_keys(connection_fd, parsed_request, ht);
            break;
        case CMD_INFO:
            handle_info(connection_fd, parsed_request, stats);
            break;
        case CMD_REPLCONF:
            handle_replconf(connection_fd, parsed_request);
            break;
        case CMD_PSYNC:
            handle_psync(connection_fd, parsed_request, stats);
            break;
        default:
            say(connection_fd, "-ERR unknown command\r\n");
    }

    if (cmd.should_send_to_slave && stats->replication.role == ROLE_MASTER) {
        int slave_connection_fd;
        Node* current_node;
    
        current_node = stats->others.connected_slaves->head;
        int node_index = 0;
    
        while(current_node != NULL) {
            slave_connection_fd = *(int*)(current_node->data);
            say(slave_connection_fd, raw_buffer);
            current_node = current_node->next;
        }
    }
}