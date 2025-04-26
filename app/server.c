#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
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


//----------------------------------------------------------------
// HELPER FUNCTIONS 

int create_server_socket() {
	int server_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (server_fd == -1)
		error("Socket creation failed");

	return server_fd;
}

void bind_to_port(int socket, int port, int reuse) {
	struct sockaddr_in serv_addr = { .sin_family = AF_INET ,
					 .sin_port = htons(port),
					 .sin_addr = { htonl(INADDR_ANY) },
					};


	if (setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0)
		exit_with_error("SO_REUSEADDR failed");

	if (bind(socket, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) != 0)
		exit_with_error("Bind failed");
}

void say(int socket, char * msg) {
	if (send(socket, msg, strlen(msg), 0) == -1)
		exit_with_error("Send failed");
}

int read_in(int socket, char *buf, int len) {
	char *s = buf;         // Pointer to the current position in the buffer
	int remaining = len;   // Remaining space in the buffer
	int bytes_read;

	// Read data from the socket in a loop
	while ((bytes_read = recv(socket, buf, remaining, 0)) > 0) {
		if (s[bytes_read - 1] == '\n' && s[bytes_read - 2] == '\r'){
			// Null-terminate the string, replacing '\n' with '\0'
			buf[bytes_read - 1] = '\0';	
			return len - remaining + bytes_read;
		}
		// Move the pointer forward and update the remaining space
		s += bytes_read;
		remaining -= bytes_read;

		// Prevent buffer overflow
		if (remaining <= 0) break;
	}

	if (bytes_read < 0) {
		// Error occurred during recv
		exit_with_error("Read failed");
	} else if (bytes_read == 0) {
		// Connection closed, return an empty string
		buf[0] = '\0';
		return 0;
	}

	buf[len - remaining] = '\0';
	return len - remaining;
}
//----------------------------------------------------------------

//----------------------------------------------------------------
// MAIN FUNCTION

int main(int argc, char *argv[]) {
	// Disable output buffering
	setbuf(stdout, NULL);
	setbuf(stderr, NULL);
	
	// You can use print statements as follows for debugging, they'll be visible when running tests.
	printf("Logs from your program will appear here!\n");

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
			case 'r':
				// Handle replicaof option
				// For now, just print the value
				snprintf(stats->replication.role, sizeof(stats->replication.role), "slave");
				char *host = strtok(optarg, " ");
				char *port_str = strtok(NULL, " ");
				if (host != NULL && port_str != NULL) {
					strncpy(stats->replication.master_host, host, sizeof(stats->replication.master_host));
					stats->replication.master_port = (uint16_t)atoi(port_str);
				} else {
					exit_with_error("Invalid replicaof argument");
				}

				break;
			default:
				break;
	    }
	}

	// Uncomment this block to pass the first stage
	//
	int server_fd, client_addr_len;
	struct sockaddr_in client_addr;

	server_fd = create_server_socket();

	// Since the tester restarts your program quite often, setting SO_REUSEADDR
	// ensures that we don't run into 'Address already in use' errors
	int reuse = 1;
	bind_to_port(server_fd, stats->server.tcp_port, reuse);

	int connection_backlog = 5;
	if (listen(server_fd, connection_backlog) != 0) {
		printf("Listen failed: %s \n", strerror(errno));
		return 1;
	}

	printf("Waiting for a client to connect...\n");
	client_addr_len = sizeof(client_addr);

	int pid;
	int connection_fd;

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

	ht_table *ht = ht_create();

	// Load the RDB file into the hash table
	if (stats->others.rdb_filename[0] != '\0' && stats->others.rdb_dir[0] != '\0') {
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
