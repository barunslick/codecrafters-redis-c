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

void initiative_handshake(int master_fd) {
    // Send handshake message to master
    char buffer[1024];

    say(master_fd, "*1\r\n$4\r\nPING\r\n");

    if (read_in(master_fd, buffer, sizeof(buffer)) < 0) {
        perror("Failed to read from master");
        close(master_fd);
        return;
    }

    return;
}