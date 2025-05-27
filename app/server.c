#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "commands.h"
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

void handle_new_client_connection(int server_fd, int epoll_fd);
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
  int epoll_timeout;

  while (1) {
    epoll_timeout = -1;
    if (stats->others.waiting_clients->len > 0) {
      epoll_timeout = 100;
      // Process the number of slaves that have sent their offset
      WaitingClientInfo* waiting_client;
      Node *current_waiting_client = stats->others.waiting_clients->head;
      Node *next_waiting_client;

      while (current_waiting_client != NULL && stats->others.waiting_clients->len > 0) {
        waiting_client = (WaitingClientInfo *)(current_waiting_client->data);
        // Save next node before potential deletion
        next_waiting_client = current_waiting_client->next;
        
        // Check how many replicas have acknowledged the required offset
        uint64_t replica_ok_count = check_replica_acknowledgments(stats, stats->server.offset);
        
        if (waiting_client->expiry <= get_current_epoch_ms() || replica_ok_count >= waiting_client->minimum_replica_count) {
          respond_to_waiting_client(waiting_client->connection_fd, replica_ok_count);
          delete_node(stats->others.waiting_clients, current_waiting_client);
        } 
        current_waiting_client = next_waiting_client;
      }
    }

    readable = epoll_wait(epoll_fd, events, MAX_EVENTS, epoll_timeout);

    for (int i = 0; i < readable; i++) {
      // For main server connection
      if (events[i].data.fd == server_fd) {
        handle_new_client_connection(server_fd, epoll_fd);
      } else if (events[i].events & EPOLLIN) {
        connection_fd = events[i].data.fd;
        memset(buf, 0, sizeof(buf));

        // This currently makes the assumption that the buffer is large enough
        // to hold the entire message and all the message can be read in one go.
        // You would want to handle partial reads and
        // buffer the data accordingly.
        int bytes_read = read_in(connection_fd, buf, sizeof(buf));
        if (bytes_read < 0)
          continue;

        char *raw_buffer = buf;
        RESPData *parsed_buffer = parse_resp_buffer(&raw_buffer);
        // process_command(connection_fd, parsed_buffer, buf, ht, stats);
        process_commands_in_buffer(connection_fd, ht, stats, buf, bytes_read);
        free_resp_data(parsed_buffer);
        free(parsed_buffer);
      } else {
        printf("Unknown event: %d\n", events[i].events);
      }

      if (events[i].events & (EPOLLRDHUP | EPOLLHUP)) {
        // For now, just close the connection
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, events[i].data.fd, NULL);
        close(events[i].data.fd);
        printf("Connection kill timestamp: %llu\n", get_current_epoch_ms());
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
    readable = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);

    for (int i = 0; i < readable; i++) {
      if (events[i].data.fd == server_fd) {
        handle_new_client_connection(server_fd, epoll_fd);
      } 
      else if (events[i].data.fd == stats->replication.master_fd && 
               (events[i].events & EPOLLIN)) {
        handle_master_data(events[i].data.fd, ht, stats);
      } 
      else if (events[i].events & EPOLLIN) {
        handle_client_request(events[i].data.fd, ht, stats);
      }
      else {
        printf("Unknown event type: %d\n", events[i].events);
      }

      if (events[i].events & (EPOLLRDHUP | EPOLLHUP)) {
        if (events[i].data.fd == stats->replication.master_fd) {
          printf("Master connection lost\n");
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

void handle_master_data(int connection_fd, ht_table *ht, RedisStats *stats) {
  char buf[MAX_BUFFER_SIZE] = {0};

  const int bytes_read = read_in_non_blocking(connection_fd, buf, sizeof(buf));
  int remaining_buffer_size = bytes_read;
  if (bytes_read <= 0) {
    if (bytes_read < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
      perror("Error reading from master");
    }
    return;
  }
  
  int handshake_offset = 0;
  if (stats->replication.handshake_state != HANDSHAKE_COMPLETED && 
      !stats->others.is_replication_completed) {
    // handshake_offset = initiative_handshake(stats, buf, bytes_read);
    handshake_offset = handle_handshake_response(stats, buf, bytes_read);
    printf("Handshake offset: %d\n", handshake_offset);
    if (handshake_offset < 0) {
      return;
    }
  }

  char* remaining_buffer = buf + handshake_offset;
  printf("Remaining RDB buffer: %s\n", remaining_buffer);
  remaining_buffer_size = bytes_read - handshake_offset;
  int rdb_offset = 0;
  if (!stats->others.is_replication_completed) {
    rdb_offset = process_rdb_data(stats, remaining_buffer, remaining_buffer_size);
    if (rdb_offset < 0) {
      // offset >= 0 means there are commands to process after the RDB data
      // Process commands starting from the specified offset
      return;
    }
  }

  char *command_buf = remaining_buffer + rdb_offset;
  remaining_buffer_size = remaining_buffer_size - rdb_offset;
  process_commands_in_buffer(connection_fd, ht, stats, command_buf, remaining_buffer_size);
}

void handle_client_request(int connection_fd, ht_table *ht, RedisStats *stats) {
  char buf[MAX_BUFFER_SIZE] = {0};

  const int bytes_read = read_in_non_blocking(connection_fd, buf, sizeof(buf));
  if (bytes_read <= 0) {
    return;
  }

  process_commands_in_buffer(connection_fd, ht, stats, buf, bytes_read);
}
