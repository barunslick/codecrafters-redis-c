#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <ctype.h>

#include "commands.h"
#include "dlist.h"
#include "hashtable.h"
#include "helper.h"
#include "rdb.h"
#include "replication.h"
#include "resp.h"
#include "state.h"

// Function declarations for server operation
void run_server(RedisStats *stats);
void run_replica(RedisStats *stats);
void run_main_loop(RedisStats *stats, int epoll_fd, int server_fd,
                   ht_table *ht);
void run_replica_main_loop(RedisStats *stats, int epoll_fd, int server_fd,
                           ht_table *ht);
int setup_server_socket(RedisStats *stats);

// Function declarations for replica helper functions
void handle_new_client_connection(int server_fd, int epoll_fd);
void handle_handshake_response(RedisStats *stats, char *buf);
int process_rdb_data(RedisStats *stats, char *buf, int bytes_read);
void process_commands_in_buffer(int connection_fd, ht_table *ht, RedisStats *stats, 
                              char *buf, int bytes_read);
void handle_master_data(int connection_fd, ht_table *ht, RedisStats *stats);
void handle_client_request(int connection_fd, ht_table *ht, RedisStats *stats);

//----------------------------------------------------------------
// MAIN FUNCTION

int main(int argc, char *argv[]) {
  // Disable output buffering
  setbuf(stdout, NULL);
  setbuf(stderr, NULL);

  // Initialize Redis configuration
  RedisStats *stats = init_redis_stats();

  struct option long_options[] = {{"dir", required_argument, 0, 'd'},
                                  {"dbfilename", required_argument, 0, 'f'},
                                  {"port", required_argument, 0, 'p'},
                                  {"replicaof", required_argument, 0, 'r'},
                                  {0, 0, 0, 0}};

  int opt;
  int option_index = 0;

  // Loop to process options
  while ((opt = getopt_long(argc, argv, "d:f:", long_options, &option_index)) !=
         -1) {
    switch (opt) {
    case 'd':
      snprintf(stats->others.rdb_dir, sizeof(stats->others.rdb_dir), "%s",
               optarg);
      break;
    case 'f':
      snprintf(stats->others.rdb_filename, sizeof(stats->others.rdb_filename),
               "%s", optarg);
      break;
    case 'p': {
      uint16_t port = (uint16_t)atoi(optarg);
      if (port < 1024 || port > 65535) {
        exit_with_error("Invalid port number");
      }
      stats->server.tcp_port = port;
      break;
    }
    case 'r': {
      // Handle replicaof option
      stats->replication.role = ROLE_SLAVE;
      strncpy(stats->replication.role_str, "slave",
              sizeof(stats->replication.role_str));
      const char *host = strtok(optarg, " ");
      const char *port_str = strtok(NULL, " ");
      if (host != NULL && port_str != NULL) {
        stats->replication.master_host =
            INADDR_LOOPBACK; // HARDCODED: localhost
        stats->replication.master_port = (uint16_t)atoi(port_str);
      } else {
        exit_with_error("Invalid replicaof argument");
      }
      break;
    }
    default:
      break;
    }
  }

  if (stats->replication.role == ROLE_SLAVE) {
    run_replica(stats);
  } else {
    run_server(stats);
  }

  return 0;
}

int setup_server_socket(RedisStats *stats) {
  int server_fd = create_server_socket();
  int reuse = 1;
  bind_to_port(server_fd, INADDR_ANY, stats->server.tcp_port, reuse);
  if (set_non_blocking(server_fd, 0) < 0) {
    exit_with_error("Failed to set non-blocking mode");
  }
  if (listen(server_fd, 10) != 0) {
    exit_with_error("Listen failed");
  }
  return server_fd;
}

void run_server(RedisStats *stats) {
  ht_table *ht = ht_create();

  if (stats->others.rdb_filename[0] != '\0' &&
      stats->others.rdb_dir[0] != '\0') {
    char *rdb_path = malloc(strlen(stats->others.rdb_dir) +
                            strlen(stats->others.rdb_filename) + 2);
    sprintf(rdb_path, "%s/%s", stats->others.rdb_dir,
            stats->others.rdb_filename);
    printf("Loading RDB file from path: %s\n", rdb_path);
    load_from_rdb_file(ht, rdb_path);
    free(rdb_path);
  }

  int server_fd = setup_server_socket(stats);
  if (server_fd < 0) {
    exit_with_error("Failed to create server socket");
  }
  int epoll_fd = epoll_create(1);

  if (epoll_fd < 0) {
    exit_with_error("Failed to create epoll instance");
  }

  epoll_ctl_add(epoll_fd, server_fd, EPOLLIN | EPOLLET | EPOLLOUT);

  run_main_loop(stats, epoll_fd, server_fd, ht);
  ht_destroy(ht);
};

void run_replica(RedisStats *stats) {
  ht_table *ht = ht_create();
  int server_fd = setup_server_socket(stats);
  if (server_fd < 0) {
    exit_with_error("Failed to create server socket");
  }

  int epoll_fd = epoll_create(1);

  if (epoll_fd < 0) {
    exit_with_error("Failed to create epoll instance");
  }

  epoll_ctl_add(epoll_fd, server_fd, EPOLLIN | EPOLLET | EPOLLOUT);

  int master_fd = connect_to_master(stats->replication.master_host,
                                    stats->replication.master_port);
  if (master_fd < 0) {
    exit_with_error("Failed to connect to master");
  }
  
  // Set non-blocking mode from the start
  set_non_blocking(master_fd, 1);
  stats->replication.master_fd = master_fd;
  
  // Add master to epoll for event-driven handling before starting handshake
  epoll_ctl_add(epoll_fd, stats->replication.master_fd,
                EPOLLIN | EPOLLET | EPOLLRDHUP | EPOLLHUP);

  printf("Connected to master. Handshake will be handled in event loop...\n");
  
  // Initiate the first step of the handshake
  handle_handshake_step(stats);

  run_replica_main_loop(stats, epoll_fd, server_fd, ht);
  ht_destroy(ht);
  close(master_fd);
};

void run_main_loop(RedisStats *stats, int epoll_fd, int server_fd,
                   ht_table *ht) {
  int readable = 0;
  char buf[MAX_BUFFER_SIZE];
  const int MAX_EVENTS = 10;
  struct epoll_event events[MAX_EVENTS];
  int connection_fd;
  struct sockaddr_in client_addr;
  int client_addr_len = sizeof(client_addr);

  while (1) {
    readable = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);

    for (int i = 0; i < readable; i++) {
      // For main server connection
      if (events[i].data.fd == server_fd) {
        connection_fd = accept(server_fd, (struct sockaddr *)&client_addr,
                               &client_addr_len);
        if (connection_fd < 0) {
          exit_with_error("Failed to accept connection");
        }
        set_non_blocking(connection_fd, 1);
        // int* value = malloc(sizeof(int));
        // *value = connection_fd;
        // add_to_list_head(stats->others.connected_clients, value);
        epoll_ctl_add(epoll_fd, connection_fd,
                      EPOLLIN | EPOLLET | EPOLLRDHUP | EPOLLHUP);
      } else if (events[i].events & EPOLLIN) {
        connection_fd = events[i].data.fd;
        memset(buf, 0, sizeof(buf));

        // This currently makes the assumption that the buffer is large enough
        // to hold the entire message and all the message can be read in one go.
        // In a real-world scenario, you would want to handle partial reads and
        // buffer the data accordingly.
        int bytes_Read = read_in(connection_fd, buf, sizeof(buf));
        if (bytes_Read < 0)
          continue;

        char *raw_buffer = buf;
        RESPData *parsed_buffer = parse_resp_buffer(&raw_buffer);
        process_command(connection_fd, parsed_buffer, buf, ht, stats);
        free_resp_data(parsed_buffer);
        free(parsed_buffer);
      } else {
        printf("Unknown event: %d\n", events[i].events);
      }

      if (events[i].events & (EPOLLRDHUP | EPOLLHUP)) {
        // For now, just close the connection
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, events[i].data.fd, NULL);
        close(events[i].data.fd);
        continue;
      }
    }
  }
}

void run_replica_main_loop(RedisStats *stats, int epoll_fd, int server_fd,
                           ht_table *ht) {
  int readable = 0;
  const int MAX_EVENTS = 32;
  struct epoll_event events[MAX_EVENTS];

  while (1) {
    // Wait for events on any of the registered file descriptors
    readable = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);

    for (int i = 0; i < readable; i++) {
      // Handle different types of events based on the file descriptor
      if (events[i].data.fd == server_fd) {
        // New client connection
        handle_new_client_connection(server_fd, epoll_fd);
      } 
      else if (events[i].data.fd == stats->replication.master_fd && 
               (events[i].events & EPOLLIN)) {
        // Data received from master
        handle_master_data(events[i].data.fd, ht, stats);
      } 
      else if (events[i].events & EPOLLIN) {
        // Data received from a client
        handle_client_request(events[i].data.fd, ht, stats);
      }
      else {
        printf("Unknown event type: %d\n", events[i].events);
      }

      // Handle disconnections for any file descriptor
      if (events[i].events & (EPOLLRDHUP | EPOLLHUP)) {
        if (events[i].data.fd == stats->replication.master_fd) {
          printf("Master connection lost\n");
          // In a real implementation, we would attempt to reconnect
        }
        
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, events[i].data.fd, NULL);
        close(events[i].data.fd);
      }
    }
  }
}

// Helper functions for replica main loop
void handle_new_client_connection(int server_fd, int epoll_fd) {
  struct sockaddr_in client_addr;
  int client_addr_len = sizeof(client_addr);
  
  int connection_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_addr_len);
  if (connection_fd < 0) {
    exit_with_error("Failed to accept connection");
  }
  
  set_non_blocking(connection_fd, 1);
  epoll_ctl_add(epoll_fd, connection_fd, EPOLLIN | EPOLLET | EPOLLRDHUP | EPOLLHUP);
  printf("Accepted new client connection\n");
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

void process_commands_in_buffer(int connection_fd, ht_table *ht, RedisStats *stats, 
                              char *buf, int bytes_read) {
  char *current_pos = buf;
  char *end_pos = buf + bytes_read;
  
  printf("Processing commands from buffer (%d bytes)\n", bytes_read);
  
  while (current_pos < end_pos) {
    // Check if we have a complete RESP command
    char *command_end = strstr(current_pos, "\r\n");
    if (!command_end) {
      printf("Incomplete command in buffer, waiting for more data\n");
      break;
    }
    
    // Parse and process the command
    char *raw_buffer = current_pos;
    RESPData *parsed_buffer = parse_resp_buffer(&raw_buffer);
    
    if (parsed_buffer != NULL) {
      // Print command preview for debugging
      printf("Processing command: ");
      if (parsed_buffer->type == RESP_ARRAY && parsed_buffer->data.array.count > 0 && 
          parsed_buffer->data.array.elements[0]->type == RESP_BULK_STRING) {
        printf("%s", parsed_buffer->data.array.elements[0]->data.str);
        if (parsed_buffer->data.array.count > 1) {
          printf(" (with %zu arguments)\n", parsed_buffer->data.array.count - 1);
        } else {
          printf(" (no arguments)\n");
        }
      } else {
        printf("Non-standard command format\n");
      }
      
      // Process the command
      process_command(connection_fd, parsed_buffer, current_pos, ht, stats);
      free_resp_data(parsed_buffer);
      free(parsed_buffer);
      
      // Move current_pos past this command to the next one
      current_pos = raw_buffer;
    } else {
      printf("Failed to parse command\n");
      // Move past this invalid command to avoid getting stuck
      current_pos = command_end + 2;
    }
  }
}

void handle_master_data(int connection_fd, ht_table *ht, RedisStats *stats) {
  char buf[MAX_BUFFER_SIZE];
  memset(buf, 0, sizeof(buf));
  
  int bytes_read = read_in_non_blocking(connection_fd, buf, sizeof(buf));
  if (bytes_read <= 0) {
    // No data or error
    if (bytes_read < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
      perror("Error reading from master");
    }
    return;
  }
  
  printf("Read %d bytes from master: %.20s...\n", bytes_read, buf);

  // Handle the handshake steps
  if (stats->replication.handshake_state != HANDSHAKE_COMPLETED && 
      !stats->others.is_replication_completed) {
    handle_handshake_response(stats, buf);
    return;
  }
  
  // Check if this is RDB data or regular command
  if (!stats->others.is_replication_completed) {
    int is_command = process_rdb_data(stats, buf, bytes_read);
    if (is_command) {
      // This is actually a command, process it
      process_commands_in_buffer(connection_fd, ht, stats, buf, bytes_read);
    }
    return;
  }
  
  // Regular command from master after replication is complete
  process_commands_in_buffer(connection_fd, ht, stats, buf, bytes_read);
}

void handle_client_request(int connection_fd, ht_table *ht, RedisStats *stats) {
  char buf[MAX_BUFFER_SIZE];
  memset(buf, 0, sizeof(buf));

  int bytes_read = read_in_non_blocking(connection_fd, buf, sizeof(buf));
  if (bytes_read <= 0) {
    return;
  }

  char *raw_buffer = buf;
  RESPData *parsed_buffer = parse_resp_buffer(&raw_buffer);

  if (parsed_buffer != NULL) {
    process_command(connection_fd, parsed_buffer, buf, ht, stats);
    free_resp_data(parsed_buffer);
    free(parsed_buffer);
  } else {
    printf("Failed to parse buffer from client\n");
  }
}
