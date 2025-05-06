#ifndef HELPER_H
#define HELPER_H

#include <stdint.h>

#define DEFAULT_REDIS_PORT 6379
#define MAX_BUFFER_SIZE 1024

void exit_with_error(char *msg);
void error(char *msg);
void get_os_info(char *buffer, size_t buffer_size);

// Server socket helper functions
int create_server_socket();
void bind_to_port(int socket, uint32_t host, int port, int reuse);
void say(int socket, char *msg);
void say_with_size(int socket, void *msg, size_t size);
int read_in(int socket, char *buf, int len);
int read_in_non_blocking(int socket, char *buf, int len);
uint32_t resolve_host(const char *hostname);
ssize_t read_file_to_buffer(int fd, char *buffer, size_t buffer_size);
int set_non_blocking(int fd, int block);
void epoll_ctl_add(int epoll_fd, int fd, uint32_t events);

#endif // HELPER_H
