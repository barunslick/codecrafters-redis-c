#include <fcntl.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "helper.h"
#include "state.h"
#include "dlist.h"

int connect_to_master(uint32_t host, uint16_t port) {
  printf("Connecting to master at %d:%d\n", host, port);

  const int fd = create_server_socket();

  struct sockaddr_in master_addr = {
      .sin_family = AF_INET,
      .sin_port = htons(port),
      .sin_addr = {htonl(host)},
  };

  if (connect(fd, (struct sockaddr *)&master_addr, sizeof(master_addr)) <
      0) {
    error("Connection to master failed");
    close(fd);
    return -1;
  }

  printf("Connected to master at %d:%d\n", host, port);
  return fd;
}

void send_rdb_file_to_slave(int connection_id, RedisStats *stats) {
  // Send the RDB file to the slave
  char empty_rdb[] = {0x52, 0x45, 0x44, 0x49, 0x53,
                      0x30, 0x30, 0x30, 0x37, 0xFF};
  size_t rdb_size = sizeof(empty_rdb);
  // Send the RESP formatted RDB file
  char resp_header[64];
  snprintf(resp_header, sizeof(resp_header), "$%zu\r\n", rdb_size);
  say(connection_id, resp_header);
  say_with_size(connection_id, empty_rdb, sizeof(empty_rdb));
}

void read_rdb_file_from_master(int master_fd) {
  // Read RDB file header ($<size>\r\n) and extract size
  char buffer[1024];
  if (read_in(master_fd, buffer, sizeof(buffer)) < 0) {
    perror("Failed to read RDB header from master");
    return;
  }

  // Parse the RDB size
  if (buffer[0] != '$') {
    printf("Unexpected RDB header format: %s\n", buffer);
    return;
  }

  // Extract size from $<size>\r\n format
  size_t rdb_size = 0;
  sscanf(buffer + 1, "%zu", &rdb_size);

  // Read and discard the RDB data in chunks using read_in
  size_t remaining = rdb_size;
  while (remaining > 0) {
    size_t to_read = (remaining < sizeof(buffer)) ? remaining : sizeof(buffer);

    // Use read_in to read chunks of the RDB file
    if (read_in(master_fd, buffer, to_read) < 0) {
      perror("Error reading RDB data");
      break;
    }

    remaining -= to_read;
  }

  printf("Skipped RDB file (%zu bytes)\n", rdb_size);
}

void handle_handshake_step(RedisStats *stats) {
  char write_buffer[1024];
  int master_fd = stats->replication.master_fd;
  
  switch(stats->replication.handshake_state) {
    case HANDSHAKE_NOT_STARTED:
      printf("Starting handshake with master: sending PING...\n");
      say(master_fd, "*1\r\n$4\r\nPING\r\n");
      stats->replication.handshake_state = HANDSHAKE_PING_SENT;
      break;
      
    case HANDSHAKE_PING_SENT:
      printf("Handshake step 2: sending REPLCONF listening-port...\n");
      snprintf(write_buffer, sizeof(write_buffer),
               "*3\r\n$8\r\nREPLCONF\r\n$14\r\nlistening-port\r\n$4\r\n%d\r\n",
               stats->server.tcp_port);
      say(master_fd, write_buffer);
      stats->replication.handshake_state = HANDSHAKE_PORT_SENT;
      break;
      
    case HANDSHAKE_PORT_SENT:
      printf("Handshake step 3: sending REPLCONF capa psync2...\n");
      snprintf(write_buffer, sizeof(write_buffer),
               "*3\r\n$8\r\nREPLCONF\r\n$4\r\ncapa\r\n$6\r\npsync2\r\n");
      say(master_fd, write_buffer);
      stats->replication.handshake_state = HANDSHAKE_CAPA_SENT;
      break;
      
    case HANDSHAKE_CAPA_SENT:
      printf("Handshake step 4: sending PSYNC...\n");
      snprintf(write_buffer, sizeof(write_buffer),
               "*3\r\n$5\r\nPSYNC\r\n$1\r\n?\r\n$2\r\n-1\r\n");
      say(master_fd, write_buffer);
      stats->replication.handshake_state = HANDSHAKE_PSYNC_SENT;
      break;
      
    case HANDSHAKE_PSYNC_SENT:
    case HANDSHAKE_COMPLETED:
      // We don't need to do anything here as we're waiting for the master's response
      break;
  }
}

void initiative_handshake(int master_fd, RedisStats *stats) {
  // Send handshake message to master
  char read_buffer[1024];
  char write_buffer[1024];

  // Step 1: Send PING to check connection
  printf("Sending PING to master...\n");
  say(master_fd, "*1\r\n$4\r\nPING\r\n");

  memset(read_buffer, 0, sizeof(read_buffer));
  int bytes_read = read_in_non_blocking(master_fd, read_buffer, sizeof(read_buffer));
  
  if (bytes_read <= 0) {
    printf("No immediate response from master for PING, continuing...\n");
  } else if (strncmp(read_buffer, "+PONG\r\n", 7) == 0) {
    printf("Received PONG from master\n");
  } else {
    printf("Unexpected response from master: %s\n", read_buffer);
  }

  // Step 2: Send REPLCONF listening-port
  printf("Sending REPLCONF listening-port...\n");
  snprintf(write_buffer, sizeof(write_buffer),
           "*3\r\n$8\r\nREPLCONF\r\n$14\r\nlistening-port\r\n$4\r\n%d\r\n",
           stats->server.tcp_port);
  say(master_fd, write_buffer);

  memset(read_buffer, 0, sizeof(read_buffer));
  bytes_read = read_in_non_blocking(master_fd, read_buffer, sizeof(read_buffer));
  
  if (bytes_read <= 0) {
    printf("No immediate response from master for REPLCONF listening-port, continuing...\n");
  } else if (strncmp(read_buffer, "+OK\r\n", 5) == 0) {
    printf("Received OK from master for listening-port\n");
  } else {
    printf("Unexpected response from master: %s\n", read_buffer);
  }

  // Step 3: Send REPLCONF capa psync2
  printf("Sending REPLCONF capa psync2...\n");
  snprintf(write_buffer, sizeof(write_buffer),
           "*3\r\n$8\r\nREPLCONF\r\n$4\r\ncapa\r\n$6\r\npsync2\r\n");
  say(master_fd, write_buffer);

  memset(read_buffer, 0, sizeof(read_buffer));
  bytes_read = read_in_non_blocking(master_fd, read_buffer, sizeof(read_buffer));
  
  if (bytes_read <= 0) {
    printf("No immediate response from master for REPLCONF capa, continuing...\n");
  } else if (strncmp(read_buffer, "+OK\r\n", 5) == 0) {
    printf("Received OK from master for capa\n");
  } else {
    printf("Unexpected response from master: %s\n", read_buffer);
  }

  // Step 4: Send PSYNC
  printf("Sending PSYNC command...\n");
  snprintf(write_buffer, sizeof(write_buffer),
           "*3\r\n$5\r\nPSYNC\r\n$1\r\n?\r\n$2\r\n-1\r\n");
  say(master_fd, write_buffer);
  
  memset(read_buffer, 0, sizeof(read_buffer));
  bytes_read = read_in_non_blocking(master_fd, read_buffer, sizeof(read_buffer));
  
  if (bytes_read <= 0) {
    printf("No immediate response from master for PSYNC, continuing...\n");
  } else if (strncmp(read_buffer, "+FULLRESYNC", 11) == 0) {
    printf("Received FULLRESYNC from master: %s\n", read_buffer);
  } else {
    printf("Unexpected response from master: %s\n", read_buffer);
  }

  // Don't attempt to read the RDB file here
  // It will be handled by the event loop in run_replica_main_loop
  printf("Handshake initiated. Waiting for RDB file from master...\n");
  
  return;
}

int process_rdb_data(RedisStats *stats, char *buf, int bytes_read) {
  printf("Processing RDB data: First 16 bytes: ");
  for (int j = 0; j < (bytes_read > 16 ? 16 : bytes_read); j++) {
    printf("%02x ", (unsigned char)buf[j]);
  }
  printf("\n");
  
  // Check if this looks like a RESP command rather than RDB data
  if (buf[0] == '*') {
    printf("Received first command from master, marking replication as completed\n");
    stats->others.is_replication_completed = 1;
    return 0; // Return 0 as offset since we start from the beginning
  } 
  
  // Handle RDB data starting with '$' (RESP bulk string format)
  else if (stats->replication.handshake_state == HANDSHAKE_COMPLETED && 
          strncmp(buf, "$", 1) == 0) {
    char *end_ptr = strchr(buf, '\r');
    if (!end_ptr) {
      printf("Incomplete RDB header, waiting for more data\n");
      return -1; 
    }
    
    // Extract the RDB size
    size_t rdb_size = 0;
    sscanf(buf + 1, "%zu", &rdb_size);
    printf("RDB file size: %zu bytes\n", rdb_size);
    
    // Skip the header ($<size>\r\n)
    size_t header_len = (end_ptr - buf) + 2; // +2 for \r\n
    
    if (bytes_read >= header_len + rdb_size) { 
      printf("Complete RDB file received\n");
      
      if (bytes_read > header_len + rdb_size) {
        char *additional_data = buf + header_len + rdb_size;
        int additional_bytes = bytes_read - (header_len + rdb_size);
        
        if (additional_data[0] == '*') {
          printf("Additional RESP commands received after RDB transfer\n");
          stats->others.is_replication_completed = 1;
          
          return header_len + rdb_size;
        }
        else {
          printf("Warning: Unexpected additional data format after RDB transfer\n");
          stats->others.is_replication_completed = 1;
          return header_len + rdb_size;
        }
      }
      
      stats->others.is_replication_completed = 1;
      printf("RDB transfer completed\n");
      return -1;
    } else {
      printf("Partial RDB file received (%lu of %zu bytes), waiting for more data\n",
             bytes_read - header_len, rdb_size);
      
      size_t expected_bytes = header_len + rdb_size;
      if (bytes_read > expected_bytes) {
        printf("Warning: Received more data than expected during partial RDB transfer\n");
        printf("Expected: %zu bytes, Received: %d bytes\n", expected_bytes, bytes_read);
      }
      
      return -1;
    }
  } 
  else {
    if (bytes_read >= 9 && 
        strncmp(buf, "REDIS", 5) == 0) {
      printf("Found RDB file with REDIS signature\n");
      stats->others.is_replication_completed = 1;
    } else {
      printf("Unknown data format in replication stream, marking transfer as completed\n");
      printf("Data starts with: ");
      for (int i = 0; i < (bytes_read > 16 ? 16 : bytes_read); i++) {
        printf("%02x ", (unsigned char)buf[i]);
      }
      printf("\n");
      stats->others.is_replication_completed = 1;
    }
    
    return -1;
  }
}

int handle_handshake_response(RedisStats *stats, char *buf, int bytes_read) {
  if (stats->replication.handshake_state == HANDSHAKE_PING_SENT && 
      strncmp(buf, "+PONG\r\n", 7) == 0) {
    printf("Received PONG from master, proceeding with handshake\n");
    handle_handshake_step(stats); // Send REPLCONF listening-port
  }
  else if (stats->replication.handshake_state == HANDSHAKE_PORT_SENT && 
           strncmp(buf, "+OK\r\n", 5) == 0) {
    printf("Received OK for listening-port, proceeding with handshake\n");
    handle_handshake_step(stats); // Send REPLCONF capa
    
  }
  else if (stats->replication.handshake_state == HANDSHAKE_CAPA_SENT && 
           strncmp(buf, "+OK\r\n", 5) == 0) {
    printf("Received OK for capa, proceeding with handshake\n");
    handle_handshake_step(stats); // Send PSYNC
  }
  else if (stats->replication.handshake_state == HANDSHAKE_PSYNC_SENT && 
           strncmp(buf, "+FULLRESYNC", 11) == 0) {
    printf("Received FULLRESYNC from master, handshake completed\n");
    stats->replication.handshake_state = HANDSHAKE_COMPLETED;
    
    // Check for additional data after FULLRESYNC response
    char *fullresync_end = strstr(buf, "\r\n");
    if (fullresync_end) {
      int fullresync_len = (fullresync_end - buf) + 2;
      
      if (bytes_read > fullresync_len) {
        printf("Additional data received with FULLRESYNC (%d bytes)\n", 
               bytes_read - fullresync_len);

        return fullresync_len;
      }
    }
  }
  return -1;
}

// Helper function to check how many replicas have acknowledged a specific offset
uint64_t check_replica_acknowledgments(RedisStats *stats, uint64_t required_offset) {
  uint64_t replica_ok_count = 0;
  Node* current_replica = stats->others.connected_slaves->head;
  
  while (current_replica != NULL) {
    ReplicaInfo *replica = (ReplicaInfo *)(current_replica->data);
    if (replica->last_ack_offset >= required_offset) {
      replica_ok_count++;
    }
    current_replica = current_replica->next;
  }
  
  return replica_ok_count;
}

// Helper function to respond to waiting client with replication status
void respond_to_waiting_client(int connection_fd, uint64_t replica_ok_count) {
  char response[64] = {0};
  snprintf(response, sizeof(response), ":%d\r\n", (int)replica_ok_count);
  say(connection_fd, response);
}
