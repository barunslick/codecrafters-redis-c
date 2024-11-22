#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include "hashtable.h"


#define DEFAULT_REDIS_PORT 6379
#define MAX_BUFFER_SIZE 1024

//----------------------------------------------------------------
// HELPER FUNCTIONS 
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
		error("Read failed");
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
// PARSER

typedef enum {
	RESP_INVALID,
	RESP_SIMPLE_STRING,
	RESP_ERROR,
	RESP_INTEGER,
	RESP_BULK_STRING,
	RESP_ARRAY,
	RESP_NULL
} RESPType;

typedef struct RESPData {  // 4(+4 padding) + 8 = 16 bytes
	RESPType type; // 4 bytes
	union {
		char *str; // 8 bytes
		char *error; // 8 bytes
		long long integer; // Redis integers are 64-bit(8 bytes) signed integers
		// struct RESPData *array; I don't understand array at this point
		struct {
			struct RESPData **elements;
			size_t count;
		} array;
	} data;
} RESPData;

// Forward Declaration
RESPData* parse_resp_buffer(char** buf);
RESPData* parse_bulk_string(char** buf);
RESPData* parse_array(char** buf);
void free_resp_data(RESPData* data);


void free_resp_data(RESPData *data) {
	switch (data->type) {
		case RESP_SIMPLE_STRING:
			free(data->data.str);
			break;
		case RESP_ERROR:
			free(data->data.error);
			break;
		default:
			break;
	}
}

RESPData* parse_bulk_string(char **buf) {
	// Example: $3\r\nfoo\r\n
	// "foo"
	char *start = *buf;
	char *end = strchr(start, '\r');
	RESPData *data = malloc(sizeof(RESPData));
	data->type = RESP_BULK_STRING;

	long length = strtol(start + 1, NULL, 10);

	if (length < 0) {
		data->type = RESP_NULL;
		data->data.str = NULL;
		*buf = end + 2;
		return data;
	}

	*buf = end + 2; // Skip the "$3\r\n" part
	data->data.str = strndup(*buf, length); // Copy happens here.
	if (data->data.str == NULL) {
		printf("Error copying bulk string\n");
		free(data);
		return NULL;
	}

	*buf += length + 2; // Skip the "foo\r\n" part
	
	return data;
}

RESPData* parse_resp_buffer(char **buf) {
	switch (*buf[0]) {
		// case '+':
		// 	return parse_simple_string(buf);
		// case '-':
		// 	return parse_error(buf);
		// case ':':
		// 	return parse_integer(buf);
		case '$':
			return parse_bulk_string(buf);
		case '*':
			return parse_array(buf);
		default:
			return NULL;
	}
}

RESPData* parse_array(char **buf) {
	// Example: *3\r\n$3\r\nfoo\r\n$3\r\nbar\r\n$5\r\nHello\r\n
	// ["foo", "bar", "Hello"]
	char *start = *buf;
	char *end = strchr(start, '\r');
	RESPData *data = malloc(sizeof(RESPData));
	data->type = RESP_ARRAY;

	long count = strtol(start + 1, NULL, 10);

	if (count < 0) {
		data->type = RESP_NULL;
		data->data.array.elements = NULL;
		data->data.array.count = 0;
		*buf = end + 2;
		return data;
	}
	// TODO: Need more handling of cases such as -1, Needing to send actual null

	*buf = end + 2; // Skip the "*3\r\n" part
	
	data->data.array.count = count;
	data->data.array.elements = malloc(count * sizeof(RESPData*));
	
	for (int i = 0; i < count; i++) {
		data->data.array.elements[i] = parse_resp_buffer(buf);

		if (data->data.array.elements[i] == NULL) {
			printf("Error parsing array element %d\n", i);
			// Free the previously allocated elements
			for (int j = 0; j < i; j++) {
				free_resp_data(data->data.array.elements[j]);
			}
			free(data->data.array.elements);
			free(data);
			return NULL;
		}
	}

	return data;
}

//----------------------------------------------------------------


//----------------------------------------------------------------
// MAIN FUNCTION

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
	
	while(read_in(connection_fd, buf, sizeof(buf))){
		char *parse_buf = buf;
		// say(connection_fd, "+PONG\r\n");
		RESPData *request= parse_resp_buffer(&parse_buf);
		if (request == NULL) {
			// Handle invalid request
			printf("Error parsing request\n");
			continue;
		}
		else if (request->type == RESP_ARRAY && request->data.array.count == 0) {
			// Handle empty array
			printf("Empty array\n");
			continue;
		}
		else if (request->type == RESP_ARRAY && strcmp(request->data.array.elements[0]->data.str, "PING") == 0) {
			say(connection_fd, "+PONG\r\n");
		}
		else if (request->type == RESP_ARRAY && strcmp(request->data.array.elements[0]->data.str, "ECHO") == 0) {
			char output[1024];
			sprintf(output, "+%s\r\n", request->data.array.elements[1]->data.str);
			say(connection_fd, output);
		}
		else if (request->type == RESP_ARRAY && strcmp(request->data.array.elements[0]->data.str, "SET") == 0) {
			ht_set(ht, request->data.array.elements[1]->data.str, request->data.array.elements[2]->data.str);
			say(connection_fd, "+OK\r\n");
		}
		else if (request->type == RESP_ARRAY && strcmp(request->data.array.elements[0]->data.str, "GET") == 0) {
			char *value = ht_get(ht, request->data.array.elements[1]->data.str);
			if (value == NULL) {
				say(connection_fd, "$-1\r\n");
			} else {
				char output[1024];
				sprintf(output, "$%ld\r\n%s\r\n", strlen(value), value);
				say(connection_fd, output);
			}
		}
		else if (request->type == RESP_ARRAY && strcmp(request->data.array.elements[0]->data.str, "DEL") == 0) {
			ht_del(ht, request->data.array.elements[1]->data.str);
			say(connection_fd, ":1\r\n");
		}
		else {
			// Handle unknown command
			printf("Unknown command\n");
		}

		free_resp_data(request);
	}
	
	if (pid) {
		close(server_fd);
	}

	return 0;
}
