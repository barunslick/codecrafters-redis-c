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

  say(master_fd, "*1\r\n$4\r\nPING\r\n");

  if (read_in(master_fd, read_buffer, sizeof(read_buffer)) < 0) {
    perror("Failed to read from master");
    close(master_fd);
    return;
  }

  // Check PONG response
  if (strncmp(read_buffer, "+PONG\r\n", 7) == 0) {
    printf("Received PONG from master\n");
  } else {
    sprintf("Unexpected response from master: &s\n", read_buffer);
  }

  // Send REPLCONF message to master with port
  snprintf(write_buffer, sizeof(write_buffer),
           "*3\r\n$8\r\nREPLCONF\r\n$14\r\nlistening-port\r\n$4\r\n%d\r\n",
           stats->server.tcp_port);

  say(master_fd, write_buffer);

  if (read_in(master_fd, read_buffer, sizeof(read_buffer)) < 0) {
    perror("Failed to read from master");
    close(master_fd);
    return;
  }

  // Check OK response
  if (strncmp(read_buffer, "+OK\r\n", 5) == 0) {
    printf("Received OK from master\n");
  } else {
    sprintf("Unexpected response from master: &s\n", read_buffer);
  }

  // Send REPLCONF message to master with capa
  snprintf(write_buffer, sizeof(write_buffer),
           "*3\r\n$8\r\nREPLCONF\r\n$4\r\ncapa\r\n$6\r\npsync2\r\n");
  say(master_fd, write_buffer);

  if (read_in(master_fd, read_buffer, sizeof(read_buffer)) < 0) {
    perror("Failed to read from master");
    close(master_fd);
    return;
  }
  // Check OK response
  if (strncmp(read_buffer, "+OK\r\n", 5) == 0) {
    printf("Received OK from master\n");
  } else {
    sprintf("Unexpected response from master: &s\n", read_buffer);
  }

  // Send PSYNC message to master
  snprintf(write_buffer, sizeof(write_buffer),
           "*3\r\n$5\r\nPSYNC\r\n$1\r\n?\r\n$2\r\n-1\r\n");
  say(master_fd, write_buffer);
  if (read_in(master_fd, read_buffer, sizeof(read_buffer)) < 0) {
    perror("Failed to read from master");
    close(master_fd);
    return;
  }

  // Check PSYNC response
  if (strncmp(read_buffer, "+OK", 11) == 0) {
    printf("Received FULLRESYNC from master\n");
  }

  // Just skip the RDB file
  if (read_in(master_fd, read_buffer, sizeof(read_buffer)) < 0) {
    perror("Failed to read RDB file");
    close(master_fd);
    return;
  }

  return;
}
