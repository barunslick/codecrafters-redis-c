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


#define DEFAULT_REDIS_PORT 6379
#define MAX_BUFFER_SIZE 1024

//----------------------------------------------------------------
// CONFIG
struct RedisConfig {
	char *dir;
	char *dbfilename;
} RedisConfig = {0};

//----------------------------------------------------------------


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
		// 	return parse_exit_with_error(buf);
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
// ENCODERS

char* convert_to_resp_bulk(int count, const char *strings[]) {
	// Allocate a buffer with enough space for the expected output
	// TODO: Make this resizable
	size_t buffer_size = 1024;
	char *result = malloc(buffer_size);
	if (!result)
		exit_with_error("Memory allocation failed");	
	result[0] = '\0'; // Start with an empty string

	// Add the *<count> prefix to indicate the number of elements
	snprintf(result, buffer_size, "*%d\r\n", count);

	for (int i = 0; i < count; i++) {
		const char *str = strings[i];
		char buffer[256];

		if (str == NULL) {
			snprintf(buffer, sizeof(buffer), "$-1\r\n");
		} else {
			int length = strlen(str);
			snprintf(buffer, sizeof(buffer), "$%d\r\n%s\r\n", length, str);
		}
		strcat(result, buffer);
	}

	return result;
}

//----------------------------------------------------------------


//----------------------------------------------------------------
// COMMAND HANDLER

typedef enum {
	CMD_PING,
	CMD_ECHO,
	CMD_SET,
	CMD_GET,
	CMD_DEL,
	CMD_CONFIG,
	CMD_KEYS,
	CMD_UNKNOWN,
} CommandType;

typedef struct {
	CommandType type;
	int min_args;
	int max_args;
	const char* name;
} CommandInfo;

// Command specification with max and min arguments
static const CommandInfo COMMANDS[] = {
	{CMD_PING, 1, 1, "PING"},
	{CMD_ECHO, 2, 2, "ECHO"},
	{CMD_SET, 3, 5, "SET"},
	{CMD_GET, 2, 2, "GET"},
	{CMD_DEL, 2, 2, "DEL"},
	{CMD_KEYS, 2, 2, "KEYS"},
	{CMD_CONFIG, 3, 3, "CONFIG"}
};

// Command validation and parsing
CommandType get_command_type(const char* cmd_str) {
    for (size_t i = 0; i < sizeof(COMMANDS) / sizeof(COMMANDS[0]); i++) {
        if (strcasecmp(cmd_str, COMMANDS[i].name) == 0)
            return COMMANDS[i].type;
    }
    return CMD_UNKNOWN;
}

bool validate_command_args(CommandType cmd, size_t arg_count) {
    for (size_t i = 0; i < sizeof(COMMANDS) / sizeof(COMMANDS[0]); i++) {
        if (COMMANDS[i].type == cmd)
            return COMMANDS[i].max_args == -1 || (arg_count >= COMMANDS[i].min_args && (arg_count <= COMMANDS[i].max_args));
    }
    return false;
}

// Command handlers
void handle_ping(int connection_fd) {
    say(connection_fd, "+PONG\r\n");
}

void handle_echo(int connection_fd, RESPData* request) {
    char output[1024];
    sprintf(output, "+%s\r\n", request->data.array.elements[1]->data.str);
    say(connection_fd, output);
}

void handle_set(int connection_fd, RESPData* request, ht_table* ht) {
	const char* key = request->data.array.elements[1]->data.str;
	void* value = request->data.array.elements[2]->data.str;

	uint64_t expiry = 0;
	if (request->data.array.count > 3 && strcasecmp(request->data.array.elements[3]->data.str, "px") == 0)
		expiry = (uint64_t) strtol(request->data.array.elements[4]->data.str, NULL, 10);

	if (ht_set_with_relative_expiry(ht, key, value, expiry) == NULL) {
		say(connection_fd, "-ERR failed to set key\r\n");
		return;
	}

	say(connection_fd, "+OK\r\n");

}

void handle_get(int connection_fd, RESPData* request, ht_table* ht) {
    const char* key = request->data.array.elements[1]->data.str;
    char* value = ht_get(ht, key);

    if (value == NULL) {
        say(connection_fd, "$-1\r\n");
    } else {
        char output[1024];
        sprintf(output, "$%ld\r\n%s\r\n", strlen(value), value);
        say(connection_fd, output);
    }
}

void handle_del(int connection_fd, RESPData* request, ht_table* ht) {
    ht_del(ht, request->data.array.elements[1]->data.str);
    say(connection_fd, ":1\r\n");
}

// Main command processor
void process_command(int connection_fd, RESPData* request, ht_table* ht) {
    if (request == NULL || request->type != RESP_ARRAY || request->data.array.count == 0) {
        say(connection_fd, "-ERR Invalid request\r\n");
        return;
    }

    const char* cmd_str = request->data.array.elements[0]->data.str;
    CommandType cmd = get_command_type(cmd_str);
    
    if (!validate_command_args(cmd, request->data.array.count)) {
        say(connection_fd, "-ERR wrong number of arguments\r\n");
        return;
    }

    switch (cmd) {
        case CMD_PING:
			handle_ping(connection_fd);
			break;
        case CMD_ECHO:
			handle_echo(connection_fd, request);
			break;
        case CMD_SET:
			handle_set(connection_fd, request, ht);
			break;
        case CMD_GET:
			handle_get(connection_fd, request, ht);
			break;
        case CMD_DEL:
			handle_del(connection_fd, request, ht);
			break;
		case CMD_CONFIG:
			if (request->data.array.count < 2) {
				printf("Error: CONFIG GET requires at least one argument\n");
				break;
			} else {
				if (strcmp(request->data.array.elements[1]->data.str, "GET") == 0 && strcmp(request->data.array.elements[2]->data.str, "dir") == 0) {
					const char *values[] = {"dir", RedisConfig.dir};
					char* dir = convert_to_resp_bulk(2, values);
					say(connection_fd, dir);
				} else if (strcmp(request->data.array.elements[1]->data.str, "GET") == 0 && strcmp(request->data.array.elements[2]->data.str, "dbfilename") == 0) {
					const char *values[] = {"dbfilename", RedisConfig.dbfilename};
					char* dbfilename = convert_to_resp_bulk(2, values);
					say(connection_fd, dbfilename);
				} else {
					printf("Error: CONFIG GET requires at least one argument\n");
					break;
				}
			}
			break;
		case CMD_KEYS: {
			const char *pattern = request->data.array.elements[1]->data.str;
			// For now convert all the keys and values to RESP and send it.
			size_t count = 0;
			const char** keys = ht_get_keys(ht, &count);
			char *keys_resp = convert_to_resp_bulk(count, keys);
			say(connection_fd, keys_resp);
			break;
		}
        default:
			say(connection_fd, "-ERR unknown command\r\n");
    }
}

//----------------------------------------------------------------
// MAIN FUNCTION

int main(int argc, char *argv[]) {
	// Disable output buffering
	setbuf(stdout, NULL);
	setbuf(stderr, NULL);
	
	// You can use print statements as follows for debugging, they'll be visible when running tests.
	printf("Logs from your program will appear here!\n");


	// Store the arguments provided by user
	char* dir = NULL;
	char* dbfilename = NULL;

	struct option long_options[] = {
		{"dir", required_argument, 0, 'd'},
		{"dbfilename", required_argument, 0, 'f'},
		{0, 0, 0, 0}
	};

	int opt;
	int option_index = 0;

	// Loop to process options
	while ((opt = getopt_long(argc, argv, "df:", long_options, &option_index)) != -1) {
		switch (opt) {
			case 'd':
				RedisConfig.dir = optarg;
				break;
			case 'f':
				RedisConfig.dbfilename = optarg;
				break;
			default:
				break;
	    }
	}

	printf("Directory:%s\n", RedisConfig.dir);
	printf("File name:%s\n", RedisConfig.dbfilename);

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

	// Load the RDB file into the hash table
	if (RedisConfig.dbfilename != NULL) {
		char *rdb_path = malloc(strlen(RedisConfig.dir) + strlen(RedisConfig.dbfilename) + 2);
		sprintf(rdb_path, "%s/%s", RedisConfig.dir, RedisConfig.dbfilename);
		printf("Loading RDB file from path: %s\n", rdb_path);
		load_from_rdb_file(ht, rdb_path);
		free(rdb_path);
	}

	
	while(read_in(connection_fd, buf, sizeof(buf))){
		char *parse_buf = buf;
		// say(connection_fd, "+PONG\r\n");
		RESPData *request= parse_resp_buffer(&parse_buf);
		process_command(connection_fd, request, ht);
		free_resp_data(request);
	}

	ht_destroy(ht);
	
	if (pid) {
		close(server_fd);
	}

	return 0;
}
