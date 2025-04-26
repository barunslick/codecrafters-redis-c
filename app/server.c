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

#define DEFAULT_REDIS_PORT 6379
#define MAX_BUFFER_SIZE 1024

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
	RedisConfig config = {
    	.port = DEFAULT_REDIS_PORT,
    	.dir = NULL,
    	.dbfilename = NULL
	};

	struct option long_options[] = {
		{"dir", required_argument, 0, 'd'},
		{"dbfilename", required_argument, 0, 'f'},
		{"port", required_argument, 0, 'p'},
		{0, 0, 0, 0}
	};

	int opt;
	int option_index = 0;

	// Loop to process options
	while ((opt = getopt_long(argc, argv, "d:f:", long_options, &option_index)) != -1) {
		switch (opt) {
			case 'd':
				config.dir = optarg;
				break;
			case 'f':
				config.dbfilename = optarg;
				break;
			case 'p':
				config.port = (uint16_t)atoi(optarg);
				if (config.port <= 0 || config.port > 65535) {
					exit_with_error("Invalid port number");
				}
				break;
			default:
				break;
	    }
	}

	printf("Directory: %s\n", config.dir);
	printf("File name: %s\n", config.dbfilename);

	// Uncomment this block to pass the first stage
	//
	int server_fd, client_addr_len;
	struct sockaddr_in client_addr;

	server_fd = create_server_socket();

	// Since the tester restarts your program quite often, setting SO_REUSEADDR
	// ensures that we don't run into 'Address already in use' errors
	int reuse = 1;
	bind_to_port(server_fd, config.port, reuse);

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
	if (config.dbfilename != NULL && config.dir != NULL) {
		char *rdb_path = malloc(strlen(config.dir) + strlen(config.dbfilename) + 2);
		sprintf(rdb_path, "%s/%s", config.dir, config.dbfilename);
		printf("Loading RDB file from path: %s\n", rdb_path);
		load_from_rdb_file(ht, rdb_path);
		free(rdb_path);
	}
	
	while(read_in(connection_fd, buf, sizeof(buf))) {
		char *parse_buf = buf;
		RESPData *request = parse_resp_buffer(&parse_buf);
		process_command(connection_fd, request, ht, &config);
		free_resp_data(request);
		free(request);
	}

	ht_destroy(ht);
	
	if (pid) {
		close(server_fd);
	}

	return 0;
}
