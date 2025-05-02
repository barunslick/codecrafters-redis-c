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

#include "hashtable.h"
#include "helper.h"
#include "rdb.h"
#include "resp.h"
#include "commands.h"
#include "replication.h"


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

	int server_fd, client_addr_len;
	struct sockaddr_in client_addr;

	server_fd = create_server_socket();

	// Since the tester restarts your program quite often, setting SO_REUSEADDR
	// ensures that we don't run into 'Address already in use' errors
	int reuse = 1;
	bind_to_port(server_fd, INADDR_ANY, stats->server.tcp_port, reuse);

	int connection_backlog = 5;
	if (listen(server_fd, connection_backlog) != 0) {
		printf("Listen failed: %s \n", strerror(errno));
		return 1;
	}

	printf("Waiting for a client to connect...\n");
	client_addr_len = sizeof(client_addr);

	int pid;
	int connection_fd;

	// Probably move the replication code to a separate process
	if (strcmp(stats->replication.role, "slave") == 0) {
		int master_fd = connect_to_master(stats->replication.master_host, stats->replication.master_port);
		if (master_fd < 0) {
			exit_with_error("Failed to connect to master");
		}
		printf("Initiating handshake with master...\n");
		initiative_handshake(master_fd, stats);
		close(master_fd);
	}

	while (1) { // Main program loop
		connection_fd = accept(server_fd, (struct sockaddr *) &client_addr, &client_addr_len);
		int pid = fork();
		if (!pid) {
			close(server_fd); // Close the server_fd in the child process
			break;
		}
		// Close the connection_fd in the parent process
		close(connection_fd);
	}

	char buf[MAX_BUFFER_SIZE];
	ht_table *ht = ht_create(); // :( This isn't shared memory, so connections will now see each other's data

	// Load the RDB file into the hash table
	if (strcmp(stats->replication.role, "slave") != 0 && stats->others.rdb_filename[0] != '\0' && stats->others.rdb_dir[0] != '\0') {
		char *rdb_path = malloc(strlen(stats->others.rdb_dir) + strlen(stats->others.rdb_filename) + 2);
		sprintf(rdb_path, "%s/%s", stats->others.rdb_dir, stats->others.rdb_filename);
		printf("Loading RDB file from path: %s\n", rdb_path);
		load_from_rdb_file(ht, rdb_path);
		free(rdb_path);
	}
	
	while(read_in(connection_fd, buf, sizeof(buf))) {
		char *parse_buf = buf;
		RESPData *request = parse_resp_buffer(&parse_buf);
		process_command(connection_fd, request, ht, stats);
		free_resp_data(request);
		free(request);
	}

	ht_destroy(ht);

	if (pid) {
		close(server_fd);
	}

	return 0;
}
