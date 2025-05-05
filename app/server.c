#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdbool.h>
#include <getopt.h>
#include <sys/epoll.h>

#include "hashtable.h"
#include "helper.h"
#include "rdb.h"
#include "resp.h"
#include "commands.h"
#include "replication.h"


void run_server(RedisStats* stats);
void run_replica(RedisStats* stats);
void run_main_loop(RedisStats* stats, int server_fd, ht_table *ht);

//----------------------------------------------------------------
// MAIN FUNCTION

int main(int argc, char *argv[]) {
	// Disable output buffering
	setbuf(stdout, NULL);
	setbuf(stderr, NULL);
	
	// Initialize Redis configuration
	RedisStats *stats = init_redis_stats();

	struct option long_options[] = {
		{"dir", required_argument, 0, 'd'},
		{"dbfilename", required_argument, 0, 'f'},
		{"port", required_argument, 0, 'p'},
		{"replicaof", required_argument, 0, 'r'},
		{0, 0, 0, 0}
	};

	int opt;
	int option_index = 0;

	// Loop to process options
	while ((opt = getopt_long(argc, argv, "d:f:", long_options, &option_index)) != -1) {
		switch (opt) {
			case 'd':
				snprintf(stats->others.rdb_dir, sizeof(stats->others.rdb_dir), "%s", optarg);
				break;
			case 'f':
				snprintf(stats->others.rdb_filename, sizeof(stats->others.rdb_filename), "%s", optarg);
				break;
			case 'p': {
				uint16_t port =  (uint16_t)atoi(optarg);
				if (port < 1024 || port > 65535) {
					exit_with_error("Invalid port number");
				}
				stats->server.tcp_port = port;
				break;
			}
			case 'r': {
				// Handle replicaof option
				// For now, just print the value
				snprintf(stats->replication.role, sizeof(stats->replication.role), "slave");
				const char *host = strtok(optarg, " ");
				const char *port_str = strtok(NULL, " ");
				if (host != NULL && port_str != NULL) {
					stats->replication.master_host = INADDR_LOOPBACK; // HARDCODED: localhost
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

	if (strcmp(stats->replication.role, "slave") == 0) {
		run_replica(stats);
	} else {
		run_server(stats);
	}

	return 0;
}

int setup_server_socket(RedisStats* stats) {
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

void run_server(RedisStats* stats) {
	ht_table *ht = ht_create();

	if (stats->others.rdb_filename[0] != '\0' && stats->others.rdb_dir[0] != '\0') {
		char *rdb_path = malloc(strlen(stats->others.rdb_dir) + strlen(stats->others.rdb_filename) + 2);
		sprintf(rdb_path, "%s/%s", stats->others.rdb_dir, stats->others.rdb_filename);
		printf("Loading RDB file from path: %s\n", rdb_path);
		load_from_rdb_file(ht, rdb_path);
		free(rdb_path);
	}

	int server_fd = setup_server_socket(stats);
	if (server_fd < 0) {
		exit_with_error("Failed to create server socket");
	}

	run_main_loop(stats, server_fd, ht);
	ht_destroy(ht);
};

void run_replica(RedisStats* stats) {
	int master_fd = connect_to_master(stats->replication.master_host, stats->replication.master_port);
	if (master_fd < 0) {
		exit_with_error("Failed to connect to master");
	}
	printf("Initiating handshake with master...\n");
	initiative_handshake(master_fd, stats);
	close(master_fd);

	ht_table *ht = ht_create();
	int server_fd = setup_server_socket(stats);
	if (server_fd < 0) {
		exit_with_error("Failed to create server socket");
	}

	run_main_loop(stats, server_fd, ht);
	ht_destroy(ht);
};

void run_main_loop(RedisStats* stats, int server_fd, ht_table *ht) {
	int epoll_fd = epoll_create(1);

	if (epoll_fd < 0) {
		exit_with_error("Failed to create epoll instance");
	}

	epoll_ctl_add(epoll_fd, server_fd, EPOLLIN | EPOLLET | EPOLLOUT);

	int readable = 0;
	char buf[MAX_BUFFER_SIZE];
	const int MAX_EVENTS = 10;
	struct epoll_event events[MAX_EVENTS];
	int connection_fd;
	struct sockaddr_in client_addr;
	int client_addr_len = sizeof(client_addr);

	while (1)
	{
		readable = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);

		for (int i = 0; i < readable; i++){
			// For main server connection
			if (events[i].data.fd == server_fd) {
				connection_fd = accept(server_fd, (struct sockaddr *) &client_addr, &client_addr_len);
				if (connection_fd < 0){
					exit_with_error("Failed to accept connection");
				}
				set_non_blocking(connection_fd, 1);
				epoll_ctl_add(epoll_fd, connection_fd, EPOLLIN | EPOLLET | EPOLLRDHUP | EPOLLHUP);
			} else if (events[i].events & EPOLLIN) {
				connection_fd = events[i].data.fd;
				memset(buf, 0, sizeof(buf));

				// This currently makes the assumption that the buffer is large enough to hold the entire message
				// and all the message can be read in one go.
				// In a real-world scenario, you would want to handle partial reads and buffer the data accordingly.
				int bytes_Read = read_in(connection_fd, buf, sizeof(buf)); 
				if (bytes_Read < 0)
					continue;

				char *raw_buffer = buf;
				RESPData *parsed_buffer = parse_resp_buffer(&raw_buffer);
				process_command(connection_fd, parsed_buffer, raw_buffer, ht, stats);
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