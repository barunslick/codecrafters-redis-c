#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>


const int DEFAULT_REDIS_PORT = 6379;

void error(char * msg){
	fprintf(stderr, "%s: %s\n", msg, strerror(errno));
	exit(1);
}

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
		error("SO_REUSEADDR failed");

	if (bind(socket, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) != 0)
		error("Bind failed");
}

void say(int socket, char * msg) {
	if (send(socket, msg, strlen(msg), 0) == -1)
		error("Send failed");
}

int read_in(int socket, char *buf, int len) {
	char *s = buf;         // Pointer to the current position in the buffer
	int remaining = len;   // Remaining space in the buffer
	int bytes_read;

	// Read data from the socket in a loop
	while ((bytes_read = recv(socket, s, remaining, 0)) > 0) {
		if (s[bytes_read - 1] == '\n') {
		    // Null-terminate the string, replacing '\n' with '\0'
		    s[bytes_read - 1] = '\0';
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
		return -1;
	} else if (bytes_read == 0) {
		// Connection closed, return an empty string
		buf[0] = '\0';
		return 0;
	}

	// Null-terminate if no newline is found
	buf[len - remaining] = '\0';
	return len - remaining;
}

int main() {
	// Disable output buffering
	setbuf(stdout, NULL);
	setbuf(stderr, NULL);
	
	// You can use print statements as follows for debugging, they'll be visible when running tests.
	printf("Logs from your program will appear here!\n");

	// Uncomment this block to pass the first stage
	//
	int server_fd, client_addr_len;
	struct sockaddr_in client_addr;

	server_fd = create_server_socket();

	// Since the tester restarts your program quite often, setting SO_REUSEADDR
	// ensures that we don't run into 'Address already in use' errors
	int reuse = 1;
	bind_to_port(server_fd, DEFAULT_REDIS_PORT, reuse);

	int connection_backlog = 5;
	if (listen(server_fd, connection_backlog) != 0) {
		printf("Listen failed: %s \n", strerror(errno));
		return 1;
	}

	printf("Waiting for a client to connect...\n");
	client_addr_len = sizeof(client_addr);

	int connection_fd = accept(server_fd, (struct sockaddr *) &client_addr, &client_addr_len);
	while (1) {
		char buf[1024];
		int len = read_in(connection_fd, buf, sizeof(buf));
		if (len < 0) {
			break;
		} else if (len == 0) {
			printf("Client disconnected\n");
			break;
		}
		say(connection_fd, "+PONG\r\n");
	}
	
	close(server_fd);

	return 0;
}
