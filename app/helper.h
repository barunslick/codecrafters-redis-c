#ifndef HELPER_H
#define HELPER_H

#define DEFAULT_REDIS_PORT 6379
#define MAX_BUFFER_SIZE 1024

void exit_with_error(char * msg);
void error(char * msg);
void get_os_info(char *buffer, size_t buffer_size);

// Server socket helper functions
int create_server_socket();
void bind_to_port(int socket, int port, int reuse);
void say(int socket, char * msg);
int read_in(int socket, char *buf, int len);

#endif // HELPER_H