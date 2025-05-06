#include <fcntl.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "helper.h"
#include "state.h"

int connect_to_master(uint32_t host, uint16_t port) {
  printf("Connecting to master at %d:%d\n", host, port);

  int sockfd = create_server_socket();

  struct sockaddr_in master_addr = {
      .sin_family = AF_INET,
      .sin_port = htons(port),
      .sin_addr = {htonl(host)},
  };

  if (connect(sockfd, (struct sockaddr *)&master_addr, sizeof(master_addr)) <
      0) {
    error("Connection to master failed");
    close(sockfd);
    return -1;
  }

  printf("Connected to master at %d:%d\n", host, port);
  return sockfd;
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
  printf("RDB size from master: %zu bytes\n", rdb_size);

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

void initiative_handshake(int master_fd, RedisStats *stats) {
  // Send handshake message to master
  char read_buffer[1024];
  char write_buffer[1024];

  // Step 1: Send PING to check connection
  printf("Sending PING to master...\n");
  say(master_fd, "*1\r\n$4\r\nPING\r\n");

  // Non-blocking read to get PONG response
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

  // Non-blocking read for response
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

  // Non-blocking read for response
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
  
  // Non-blocking read for FULLRESYNC response
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
      // The RDB data will be handled in the run_replica_main_loop
      break;
  }
}

void handle_handshake_response(RedisStats *stats, char *buf) {
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
  }
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
    return 1; // This is a command, not RDB data
  } 
  else if (stats->replication.handshake_state == HANDSHAKE_COMPLETED && 
          strncmp(buf, "$", 1) == 0) {
    printf("Received RDB file header, processing as RDB transfer\n");
    stats->others.is_replication_completed = 1;
  } 
  else {
    // In a real implementation, we would parse the RDB format
    // For now, we'll just consider any data as completion of RDB transfer
    stats->others.is_replication_completed = 1;
    printf("RDB file transfer marked as completed\n");
  }
  
  return 0; // This was RDB data, not a command
}
