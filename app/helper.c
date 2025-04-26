#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <unistd.h>

#include "helper.h"

#ifdef _WIN32
    #include <windows.h>
#elif __linux__
    #include <sys/utsname.h>
#elif __APPLE__
    #include <sys/sysctl.h>
#endif

// Function to get OS information
void get_os_info(char *buffer, size_t buffer_size) {
    #ifdef _WIN32
		snprintf(buffer, buffer_size, "Windows");
    #elif __linux__
        struct utsname unamedata;
        if (uname(&unamedata) == 0) {
            snprintf(buffer, buffer_size, "%s %s %s", 
                    unamedata.sysname, unamedata.release, unamedata.machine);
        } else {
			snprintf(buffer, buffer_size, "Linux (unknown version)");
        }
    #elif __APPLE__
    #else
		snprintf(buffer, buffer_size, "Unknown OS");
    #endif
    
    // Ensure null termination
    buffer[buffer_size - 1] = '\0';
}

void exit_with_error(char * msg){
	fprintf(stderr, "%s: %s\n", msg, strerror(errno));
	exit(1);
}


void error(char * msg){
	fprintf(stderr, "%s: %s\n", msg, strerror(errno));
}


// Socket Helper Functions

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