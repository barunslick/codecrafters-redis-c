#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <netinet/in.h>
#include <unistd.h>

#include "helper.h"
#include "state.h"

int connect_to_master(uint32_t host, uint16_t port) {

    printf("Connecting to master at %d:%d\n", host, port);

    int sockfd = create_server_socket();

    struct sockaddr_in master_addr = {
                    .sin_family = AF_INET ,
					.sin_port = htons(port),
					.sin_addr = { htonl(host) },
					};

    if (connect(sockfd, (struct sockaddr *)&master_addr, sizeof(master_addr)) < 0) {
        error("Connection to master failed");
        close(sockfd);
        return -1;
    }

    printf("Connected to master at %d:%d\n", host, port);
    return sockfd;
}

void initiative_handshake(int master_fd, RedisStats *stats) {
    // Send handshake message to master
    char read_buffer[1024];
    char write_buffer[1024];

    say(master_fd, "*1\r\n$4\r\nPING\r\n");

    if (read_in(master_fd, read_buffer, sizeof(read_buffer)) < 0) {
        perror("Failed to read from master");
        close(master_fd);
        return;
    }

    // Check PONG response
    if (strncmp(read_buffer, "+PONG\r\n", 7) == 0) {
        printf("Received PONG from master\n");
    } else {
        sprintf("Unexpected response from master: &s\n", read_buffer);
    }

    // Send REPLCONF message to master with port
    snprintf(write_buffer, sizeof(write_buffer), "*3\r\n$8\r\nREPLCONF\r\n$14\r\nlistening-port\r\n$4\r\n%d\r\n", stats->server.tcp_port);

    say(master_fd, write_buffer);

    if (read_in(master_fd, read_buffer, sizeof(read_buffer)) < 0) {
        perror("Failed to read from master");
        close(master_fd);
        return;
    }

    // Check OK response
    if (strncmp(read_buffer, "+OK\r\n", 5) == 0) {
        printf("Received OK from master\n");
    } else {
        sprintf("Unexpected response from master: &s\n", read_buffer);
    }

    // Send REPLCONF message to master with capa
    snprintf(write_buffer, sizeof(write_buffer), "*3\r\n$8\r\nREPLCONF\r\n$4\r\ncapa\r\n$6\r\npsync2\r\n");
    say(master_fd, write_buffer);

    if (read_in(master_fd, read_buffer, sizeof(read_buffer)) < 0) {
        perror("Failed to read from master");
        close(master_fd);
        return;
    }
    // Check OK response
    if (strncmp(read_buffer, "+OK\r\n", 5) == 0) {
        printf("Received OK from master\n");
    } else {
        sprintf("Unexpected response from master: &s\n", read_buffer);
    }

    // Send PSYNC message to master
    snprintf(write_buffer, sizeof(write_buffer), "*3\r\n$5\r\nPSYNC\r\n$1\r\n?\r\n$2\r\n-1\r\n");
    say(master_fd, write_buffer);
    if (read_in(master_fd, read_buffer, sizeof(read_buffer)) < 0) {
        perror("Failed to read from master");
        close(master_fd);
        return;
    }

    // Check PSYNC response
    if (strncmp(read_buffer, "+OK", 11) == 0) {
        printf("Received FULLRESYNC from master\n");
    }

    return;
}