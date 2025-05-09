#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>

#include "helper.h"
#include "commands.h"
#include "dlist.h"
#include "replication.h"

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
  CMD_WAIT,
} CommandType;

// Command specification with max and min arguments
typedef struct {
  CommandType type;
  int min_args;
  int max_args;
  const char *name;
  bool should_send_to_slave;
  bool should_respond_to_master;
} CommandInfo;

// Command specification with max and min arguments
static const CommandInfo COMMANDS[] = {
    {CMD_PING, 1, 1, "PING", 0, 0},
    {CMD_ECHO, 2, 2, "ECHO", 0, 0},
    {CMD_SET, 3, 5, "SET", 1, 0},
    {CMD_GET, 2, 2, "GET", 0, 0},
    {CMD_DEL, 2, 2, "DEL", 1, 0},
    {CMD_KEYS, 2, 2, "KEYS", 0, 0},
    {CMD_CONFIG, 3, 3, "CONFIG", 0, 0},
    {CMD_INFO, 2, 2, "INFO", 0, 1},
    {CMD_REPLCONF, 3, 10, "REPLCONF", 0, 1},
    {CMD_PSYNC, 3, 3, "PSYNC", 0, 0},
    {CMD_WAIT, 3, 3, "WAIT", 0, 0},
};

// Command validation and parsing
static CommandType get_command_type(const char *cmd_str) {
  for (size_t i = 0; i < sizeof(COMMANDS) / sizeof(COMMANDS[0]); i++) {
    if (strcasecmp(cmd_str, COMMANDS[i].name) == 0)
      return COMMANDS[i].type;
  }
  return CMD_UNKNOWN;
}

static CommandInfo get_command_info(const char *cmd_str) {
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
      return COMMANDS[i].max_args == -1 ||
             (arg_count >= COMMANDS[i].min_args &&
              (arg_count <= COMMANDS[i].max_args));
  }
  return false;
}

// Command handlers
size_t handle_ping(char* write_buf, size_t buf_size) { 
  return snprintf(write_buf, buf_size, "+PONG\r\n");
}

size_t handle_echo(char* write_buf, size_t buf_size, RESPData *request) {
  return snprintf(write_buf, buf_size, "+%s\r\n", request->data.array.elements[1]->data.str);
}

size_t handle_set(char* write_buf, size_t buf_size, RESPData *request, ht_table *ht) {
  const char *key = request->data.array.elements[1]->data.str;
  void *value = request->data.array.elements[2]->data.str;

  uint64_t expiry = 0;
  if (request->data.array.count > 3 &&
      strcasecmp(request->data.array.elements[3]->data.str, "px") == 0)
    expiry =
        (uint64_t)strtol(request->data.array.elements[4]->data.str, NULL, 10);

  if (ht_set_with_relative_expiry(ht, key, value, expiry) == NULL) {
    return snprintf(write_buf, buf_size, "-ERR failed to set key\r\n");
  }

  return snprintf(write_buf, buf_size, "+OK\r\n");
}

size_t handle_get(char* write_buf, size_t buf_size, RESPData *request, ht_table *ht) {
  const char *key = request->data.array.elements[1]->data.str;
  char *value = ht_get(ht, key);

  if (value == NULL) {
    return snprintf(write_buf, buf_size, "$-1\r\n");
  } else {
    return snprintf(write_buf, buf_size, "$%ld\r\n%s\r\n", strlen(value), value);
  }
}

size_t handle_del(char* write_buf, size_t buf_size, RESPData *request, ht_table *ht) {
  ht_del(ht, request->data.array.elements[1]->data.str);
  return snprintf(write_buf, buf_size, ":1\r\n");
}

size_t handle_config(char* write_buf, size_t buf_size, RESPData *request, RedisStats *stats) {
  if (request->data.array.count < 2) {
    return snprintf(write_buf, buf_size, "-ERR CONFIG requires at least one argument\r\n");
  }

  if (strcmp(request->data.array.elements[1]->data.str, "GET") == 0) {
    if (strcmp(request->data.array.elements[2]->data.str, "dir") == 0) {
      return snprintf(write_buf, buf_size, "*2\r\n$3\r\ndir\r\n$%zu\r\n%s\r\n", 
                     strlen(stats->others.rdb_dir), stats->others.rdb_dir);
    } else if (strcmp(request->data.array.elements[2]->data.str, "dbfilename") == 0) {
      return snprintf(write_buf, buf_size, "*2\r\n$10\r\ndbfilename\r\n$%zu\r\n%s\r\n", 
                     strlen(stats->others.rdb_filename), stats->others.rdb_filename);
    } else {
      return snprintf(write_buf, buf_size, "-ERR Unknown CONFIG parameter\r\n");
    }
  } else {
    return snprintf(write_buf, buf_size, "-ERR Unknown CONFIG command\r\n");
  }
}

size_t handle_keys(char* write_buf, size_t buf_size, RESPData *request, ht_table *ht) {
  const char *pattern = request->data.array.elements[1]->data.str;
  // For now, return all keys without pattern matching
  size_t count = 0;
  const char **keys = ht_get_keys(ht, &count);
  
  // Write array header
  size_t cursor = snprintf(write_buf, buf_size, "*%zu\r\n", count);
  
  // Write each key as a bulk string
  for (size_t i = 0; i < count && cursor < buf_size; i++) {
    cursor += snprintf(write_buf + cursor, buf_size - cursor, "$%zu\r\n%s\r\n", 
                      strlen(keys[i]), keys[i]);
  }
  
  free(keys);
  return cursor;
}

size_t handle_info(char* write_buf, size_t buf_size, RESPData *request, RedisStats *stats) {
  const char *info_type = request->data.array.elements[1]->data.str;

  if (strcmp(info_type, "replication") == 0) {
    // Create a temporary buffer for the info content
    char info_content[512];
    size_t info_len = 0;
    
    info_len += snprintf(info_content + info_len, sizeof(info_content) - info_len, 
                         "# Replication\r\n");
    info_len += snprintf(info_content + info_len, sizeof(info_content) - info_len, 
                         "role:%s\r\n", stats->replication.role_str);
    info_len += snprintf(info_content + info_len, sizeof(info_content) - info_len, 
                         "master_replid:%s\r\n", stats->replication.master_replid);
    info_len += snprintf(info_content + info_len, sizeof(info_content) - info_len,
                         "master_repl_offset:%lu\r\n", stats->replication.master_repl_offset);

    // Format as RESP bulk string
    return snprintf(write_buf, buf_size, "$%zu\r\n%s\r\n", info_len, info_content);
  } else {
    return snprintf(write_buf, buf_size, "-ERR Unknown INFO type\r\n");
  }
}

size_t handle_replconf(int connection_fd, char* write_buf, size_t buf_size, RESPData *request, RedisStats *stats) {
  if (strcmp(request->data.array.elements[1]->data.str, "listening-port") == 0) {
    // TODO: Handle listening-port later
    return snprintf(write_buf, buf_size, "+OK\r\n");
  } else if (strcmp(request->data.array.elements[1]->data.str, "capa") == 0) {
    // TODO: Handle capa later
    return snprintf(write_buf, buf_size, "+OK\r\n");
  } else if (strcmp(request->data.array.elements[1]->data.str, "GETACK") == 0) {
    if (strcmp(request->data.array.elements[2]->data.str, "*") == 0) {
      // Create a string array with REPLCONF ACK and the bytes read
      char bytes_read_str[20]; // Buffer to hold the string representation
      snprintf(bytes_read_str, sizeof(bytes_read_str), "%zu", stats->replication.bytes_read->bytes_read);
      
      const char *ack_array[20] = {
        "REPLCONF",
        "ACK",
        bytes_read_str
      };

      // Convert to RESP array and write to write_buffer
      size_t resp_length = convert_to_resp_array(write_buf, buf_size, 3, ack_array);
      printf("Slave offset when recieving ACK: %lu", stats->replication.bytes_read->bytes_read);
      
      if (stats->replication.bytes_read->is_reading == 0) {
        stats->replication.bytes_read->is_reading = 1;
      }
      
      return resp_length;
    } 
  } else if (strcmp(request->data.array.elements[1]->data.str, "ACK") == 0) {
    uint64_t ack_offset = strtoul(request->data.array.elements[2]->data.str, NULL, 10);

    // Update the last acknowledged offset for the replica
    Node *current_node = stats->others.connected_slaves->head;
    while (current_node != NULL) {
      ReplicaInfo *replica = (ReplicaInfo *)(current_node->data);
      if (replica->connection_fd == connection_fd) {
        replica->last_ack_offset = ack_offset;
        break;
      }
      current_node = current_node->next;
    }

    return 0;
    }
  
  return snprintf(write_buf, buf_size, "-ERR Unknown REPLCONF command\r\n");
}

ssize_t handle_wait(int connection_fd, char* write_buf, size_t buf_size, RESPData *request, RedisStats *stats) {
  if (stats->replication.role == ROLE_SLAVE) {
    return snprintf(write_buf, buf_size, "-ERR WAIT not supported in slave mode\r\n");
  }

  if (stats->others.connected_slaves->len == 0) {
    return snprintf(write_buf, buf_size, ":0\r\n");
  }

  uint64_t num_slaves = atol(request->data.array.elements[1]->data.str);
  uint64_t expiry = atol(request->data.array.elements[2]->data.str) + get_current_epoch_ms();

  if (stats->replication.role == ROLE_MASTER) {
    Node *current_node = stats->others.connected_slaves->head;
    // Check if enough replicas have already acknowledged the offset
    uint64_t replica_ok_count = check_replica_acknowledgments(stats, stats->server.offset);
    
    if (replica_ok_count >= num_slaves) {
      respond_to_waiting_client(connection_fd, replica_ok_count);
      return 0;
    }

    while (current_node != NULL) {
      ReplicaInfo *replica = (ReplicaInfo *)(current_node->data);
      say(replica->connection_fd, "*3\r\n$8\r\nREPLCONF\r\n$6\r\nGETACK\r\n$1\r\n*\r\n");
      current_node = current_node->next;
    }

    // Add the client as waiting
    WaitingClientInfo *waiting_client = create_waiting_client_info(connection_fd, stats->replication.master_repl_offset, num_slaves, expiry);
    if (waiting_client == NULL) {
      exit_with_error("Failed to allocate memory for WaitingClientInfo");
    }
    if (add_to_list_tail(stats->others.waiting_clients, waiting_client) == NULL) {
      free(waiting_client);
      exit_with_error("Failed to add waiting client to list");
    }

    return 0;
  }
}

void handle_psync(int connection_fd, RESPData *request, RedisStats *stats) {
  if (stats->replication.role == ROLE_SLAVE) {
    say(connection_fd, "-ERR PSYNC not supported in slave mode\r\n");
    return;
  }

  // Create a buffer to hold the response
  char buffer[1024];

  sprintf(buffer, "FULLRESYNC %s 0\r\n", stats->replication.master_replid);

  // Send the response to the client
  char *resp = convert_to_resp_string(buffer);
  say(connection_fd, resp);
  free(resp);

  send_rdb_file_to_slave(connection_fd, stats);

  // Create a ReplicaInfo struct to store the connection fd and last acknowledged offset
  ReplicaInfo* replica = create_replica_info(connection_fd);
  if (replica == NULL) {
    exit_with_error("Failed to allocate memory for ReplicaInfo");
  }
  
  if (add_to_list_tail(stats->others.connected_slaves, replica) == NULL) {
    free(replica);
    exit_with_error("Internal Error");
  }

  return;
}


// ----------------- Command processing functions ----------------------------
// ---------------------------------------------------------------------
void process_commands_in_buffer(int connection_fd, ht_table *ht, RedisStats *stats, 
                              char *buf, int bytes_read) {
  char *current_pos = buf;
  char *end_pos = buf + bytes_read;
  
  while (current_pos < end_pos) {
    // Check if we have enough data to determine command type
    if (current_pos >= end_pos - 1) {
      printf("Reached end of buffer\n");
      break;
    }

    // Check command type marker ($ or *)
    char cmd_type = *current_pos;
    if (cmd_type != '$' && cmd_type != '*') {
      break;
    }

    // Try to find the end of this command
    char *command_end = NULL;
    if (cmd_type == '$') {
      // For bulk string, first find length
      char *length_end = strstr(current_pos, "\r\n");
      if (!length_end) {
        printf("Incomplete bulk string command\n");
        break;
      }
      
      long length = strtol(current_pos + 1, NULL, 10);
      if (length < 0) {
        // Skip null bulk string
        current_pos = length_end + 2;
        continue;
      }
      
      if (length_end + 2 + length + 2 > end_pos) {
        printf("Incomplete bulk string data\n");
        break;
      }
      
      command_end = length_end + 2 + length + 2; // Skip length, \r\n, data, and final \r\n
    } else {
      // For arrays, we need to parse the entire structure
      char *raw_buffer = current_pos;
      RESPData *parsed_buffer = parse_resp_buffer(&raw_buffer);
      
      if (parsed_buffer != NULL) {
        if (stats->replication.role == ROLE_SLAVE && stats->replication.bytes_read->is_reading == 1) {
          stats->replication.bytes_read->bytes_read += (raw_buffer - current_pos);
        }

        process_command(connection_fd, parsed_buffer, current_pos, ht, stats);
        free_resp_data(parsed_buffer);
        free(parsed_buffer);
        
        command_end = raw_buffer;
      } else {
        // Failed to parse array, try to find next command
        printf("Failed to parse array command\n");
        break;
      }
    }

    // Move to the next command
    if (command_end) {
      current_pos = command_end;
    } else {
      break;
    }
  }
}

// ----------------- Main command processor ----------------------------
// ---------------------------------------------------------------------
void process_command(int connection_fd, RESPData *parsed_request,
                     char *raw_buffer, ht_table *ht, RedisStats *stats) {
  if (parsed_request == NULL || parsed_request->type != RESP_ARRAY ||
      parsed_request->data.array.count == 0) {
    say(connection_fd, "-ERR Invalid request\r\n");
    return;
  }

  const char *cmd_str = parsed_request->data.array.elements[0]->data.str;
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

  // Use a single pre-allocated buffer for all handlers
  char write_buf[4096];  // Increased size to handle larger responses
  size_t response_len = 0;
  // Call the appropriate command handler and get the response in the buffer
  switch (cmd_type) {
  case CMD_PING:
    response_len = handle_ping(write_buf, sizeof(write_buf));
    break;
  case CMD_ECHO:
    response_len = handle_echo(write_buf, sizeof(write_buf), parsed_request);
    break;
  case CMD_SET:
    response_len = handle_set(write_buf, sizeof(write_buf), parsed_request, ht);
    break;
  case CMD_GET:
    response_len = handle_get(write_buf, sizeof(write_buf), parsed_request, ht);
    break;
  case CMD_DEL:
    response_len = handle_del(write_buf, sizeof(write_buf), parsed_request, ht);
    break;
  case CMD_CONFIG:
    response_len = handle_config(write_buf, sizeof(write_buf), parsed_request, stats);
    break;
  case CMD_KEYS:
    response_len = handle_keys(write_buf, sizeof(write_buf), parsed_request, ht);
    break;
  case CMD_INFO:
    response_len = handle_info(write_buf, sizeof(write_buf), parsed_request, stats);
    break;
  case CMD_REPLCONF:
    response_len = handle_replconf(connection_fd, write_buf, sizeof(write_buf), parsed_request, stats);
    break;
  case CMD_PSYNC:
    handle_psync(connection_fd, parsed_request, stats);
    break;
  case CMD_WAIT:
    response_len = handle_wait(connection_fd, write_buf, sizeof(write_buf), parsed_request, stats);
    break;
  default:
    snprintf(write_buf, sizeof(write_buf), "-ERR unknown command\r\n");
    response_len = strlen(write_buf);
  }

  if (response_len > 0) {
    if (stats->replication.role == ROLE_SLAVE) {
      // If the command is not a replication command,
      if (connection_fd != stats->replication.master_fd) {
        say(connection_fd, write_buf);
      } else if (connection_fd == stats->replication.master_fd && cmd.should_respond_to_master) {
        say(connection_fd, write_buf);
      }
    } else {
      // If the command is not a replication command, send the response to the client
      say(connection_fd, write_buf);
    }
  }

  // Propagate commands to slaves if needed
  if (cmd.should_send_to_slave && stats->replication.role == ROLE_MASTER) {
    // Update the server offset
    stats->server.offset += strlen(raw_buffer);
    Node *current_node = stats->others.connected_slaves->head;

    while (current_node != NULL) {
      ReplicaInfo *replica = (ReplicaInfo *)(current_node->data);
      say(replica->connection_fd, raw_buffer);
      
      // Update the master's replication offset after sending command to replica
      stats->replication.master_repl_offset += strlen(raw_buffer);
      
      current_node = current_node->next;
    }
  }
}
