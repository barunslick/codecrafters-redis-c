#ifndef HELPER_H
#define HELPER_H

#define DEFAULT_REDIS_PORT 6379
#define MAX_BUFFER_SIZE 1024

void exit_with_error(char * msg);
void error(char * msg);
void get_os_info(char *buffer, size_t buffer_size);

#endif // HELPER_H