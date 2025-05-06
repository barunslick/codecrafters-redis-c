#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
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
    snprintf(buffer, buffer_size, "%s %s %s", unamedata.sysname,
             unamedata.release, unamedata.machine);
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

void exit_with_error(char *msg) {
  fprintf(stderr, "%s: %s\n", msg, strerror(errno));
  exit(1);
}

void error(char *msg) { fprintf(stderr, "%s: %s\n", msg, strerror(errno)); }

// Socket Helper Functions

int create_server_socket() {
  int server_fd = socket(PF_INET, SOCK_STREAM, 0);
  if (server_fd == -1)
    error("Socket creation failed");

  return server_fd;
}

void bind_to_port(int socket, uint32_t host, int port, int reuse) {
  struct sockaddr_in serv_addr = {
      .sin_family = AF_INET,
      .sin_port = htons(port),
      .sin_addr = {htonl(host)},
  };

  if (setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0)
    exit_with_error("SO_REUSEADDR failed");

  if (bind(socket, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) != 0)
    exit_with_error("Bind failed");
}

int set_non_blocking(int fd, int block) {
  if (fd < 0) {
    return -1; // Invalid file descriptor
  }

  int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1) {
    return -1; // Failed to get flags
  }
  flags = (block) ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
  return (fcntl(fd, F_SETFL, flags) == 0);
}

void epoll_ctl_add(int epoll_fd, int fd, uint32_t events) {
  struct epoll_event ev;
  ev.events = events;
  ev.data.fd = fd;

  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) == -1) {
    exit_with_error("Epoll control failed");
  }
}

void say(int socket, char *msg) {
  if (send(socket, msg, strlen(msg), 0) == -1)
    exit_with_error("Send failed");
}

void say_with_size(int socket, void *msg, size_t size) {
  if (send(socket, msg, size, 0) == -1)
    exit_with_error("Send failed");
}

int read_in(int socket, char *buf, int len) {
  char *s = buf;       // Pointer to the current position in the buffer
  int remaining = len; // Remaining space in the buffer
  int bytes_read;

  // Read data from the socket in a loop
  while ((bytes_read = recv(socket, s, remaining, 0)) > 0) {
    // Move the pointer forward and update the remaining space
    s += bytes_read;
    remaining -= bytes_read;

    // Check if we have a complete message (ending with \r\n)
    if (s - buf >= 2 && *(s - 1) == '\n' && *(s - 2) == '\r') {
      // Add null terminator after \r\n for string operations
      if (remaining > 0) {
        *s = '\0';
      }
      return len - remaining;
    }

    // Prevent buffer overflow
    if (remaining <= 0)
      break;
  }

  if (bytes_read < 0) {
    // Check if this is a non-blocking socket with no data available
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      // Non-blocking socket with no data available yet
      // Return what we've read so far (which may be nothing)
      if (remaining > 0) {
        *s = '\0';
      } else if (len > 0) {
        buf[len - 1] = '\0';
      }
      return len - remaining;
    } else {
      // Real error occurred during recv
      exit_with_error("Read failed");
    }
  } else if (bytes_read == 0) {
    // Connection closed, return an empty string
    buf[0] = '\0';
    return 0;
  }

  // Ensure null termination at the end of the received data
  if (remaining > 0) {
    *s = '\0';
  } else if (len > 0) {
    buf[len - 1] = '\0';
  }

  return len - remaining;
}

int read_in_non_blocking(int socket, char *buf, int len) {
  char *s = buf; // Pointer to the current position in the buffer
  int bytes_read;

  // For non-blocking I/O, make a single attempt to read
  bytes_read = recv(socket, s, len, 0);

  if (bytes_read > 0) {
    // Successfully read some data
    // Ensure null termination if there's space
    if (bytes_read < len) {
      buf[bytes_read] = '\0';
    } else if (len > 0) {
      buf[len - 1] = '\0';
    }
    return bytes_read;
  } else if (bytes_read == 0) {
    // Connection closed by the other end
    buf[0] = '\0';
    return 0;
  } else {
    // bytes_read < 0
    // Check if this is a non-blocking socket with no data available
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      // No data available right now, but connection is still open
      buf[0] = '\0';
      return 0;
    } else {
      // Real error occurred
      exit_with_error("Read failed");
    }
  }

  return 0;
}

uint32_t resolve_host(const char *hostname) {
  if (hostname == NULL) {
    return INADDR_ANY; // Return INADDR_ANY if hostname is NULL
  }

  if (strcmp(hostname, "localhost") == 0) {
    return INADDR_LOOPBACK; // Return loopback address for localhost
  }

  return inet_addr(hostname); // Convert hostname to IP address
}

ssize_t read_file_to_buffer(int fd, char *buffer, size_t buffer_size) {
  // Currently, this function has no indication of whether the file exceeds the
  // buffer size. Fix it later.
  ssize_t bytes_read = 0;
  ssize_t total_bytes_read = 0;

  if (fd < 0 || buffer == NULL || buffer_size == 0) {
    return -1; // Invalid fd, buffer or size
  }

  while ((bytes_read = read(fd, buffer + total_bytes_read,
                            buffer_size - total_bytes_read)) > 0) {
    total_bytes_read += bytes_read;
    if (total_bytes_read >= buffer_size) {
      break; // Buffer is full
    }
  }

  if (total_bytes_read < 0) {
    return -1;
  }

  // Ensure null termination
  if (total_bytes_read < buffer_size) {
    buffer[total_bytes_read] = '\0';
  } else {
    buffer[buffer_size - 1] = '\0'; // Will overwrite the last byte.
  }

  return total_bytes_read;
}
