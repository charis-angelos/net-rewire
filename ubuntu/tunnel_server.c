//
//  tunnel_server.c
//  Net-Rewire Ubuntu Tunnel Server
//
//  Created by Claude Code
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/select.h>

#define SERVER_PORT 12345
#define TUN_DEVICE "tun0"
#define TUN_IP "10.8.0.1"
#define TUN_NETMASK "255.255.255.0"

static volatile int running = 1;

// Structure to hold client information
typedef struct {
    int socket_fd;
    struct sockaddr_in client_addr;
} client_info_t;

// Create TUN device
int create_tun_device() {
    struct ifreq ifr;
    int tun_fd;

    // Open TUN device
    if ((tun_fd = open("/dev/net/tun", O_RDWR)) < 0) {
        perror("Error opening /dev/net/tun");
        return -1;
    }

    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
    strncpy(ifr.ifr_name, TUN_DEVICE, IFNAMSIZ);

    // Configure TUN device
    if (ioctl(tun_fd, TUNSETIFF, (void *)&ifr) < 0) {
        perror("Error configuring TUN device");
        close(tun_fd);
        return -1;
    }

    printf("Created TUN device: %s\n", ifr.ifr_name);
    return tun_fd;
}

// Configure TUN device IP address
int configure_tun_device(int tun_fd) {
    char cmd[256];

    // Set IP address
    snprintf(cmd, sizeof(cmd), "ip addr add %s/%s dev %s", TUN_IP, "24", TUN_DEVICE);
    if (system(cmd) != 0) {
        fprintf(stderr, "Error setting IP address on TUN device\n");
        return -1;
    }

    // Bring interface up
    snprintf(cmd, sizeof(cmd), "ip link set %s up", TUN_DEVICE);
    if (system(cmd) != 0) {
        fprintf(stderr, "Error bringing TUN device up\n");
        return -1;
    }

    printf("Configured TUN device %s with IP %s\n", TUN_DEVICE, TUN_IP);
    return 0;
}

// Handle client connection
void* handle_client(void* arg) {
    client_info_t* client = (client_info_t*)arg;
    int client_fd = client->socket_fd;
    int tun_fd;
    char client_ip[INET_ADDRSTRLEN];

    inet_ntop(AF_INET, &(client->client_addr.sin_addr), client_ip, INET_ADDRSTRLEN);
    printf("Client connected: %s:%d\n", client_ip, ntohs(client->client_addr.sin_port));

    // Create TUN device for this client
    tun_fd = create_tun_device();
    if (tun_fd < 0) {
        fprintf(stderr, "Failed to create TUN device for client %s\n", client_ip);
        close(client_fd);
        free(client);
        return NULL;
    }

    // Configure TUN device
    if (configure_tun_device(tun_fd) < 0) {
        fprintf(stderr, "Failed to configure TUN device for client %s\n", client_ip);
        close(tun_fd);
        close(client_fd);
        free(client);
        return NULL;
    }

    // Set non-blocking mode for both sockets
    fcntl(client_fd, F_SETFL, O_NONBLOCK);
    fcntl(tun_fd, F_SETFL, O_NONBLOCK);

    fd_set read_fds;
    int max_fd = (client_fd > tun_fd) ? client_fd : tun_fd;

    // Main packet forwarding loop
    while (running) {
        FD_ZERO(&read_fds);
        FD_SET(client_fd, &read_fds);
        FD_SET(tun_fd, &read_fds);

        struct timeval timeout = {1, 0}; // 1 second timeout
        int activity = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);

        if (activity < 0 && errno != EINTR) {
            perror("select error");
            break;
        }

        if (activity == 0) {
            // Timeout - check if still running
            continue;
        }

        // Forward packets from client to TUN
        if (FD_ISSET(client_fd, &read_fds)) {
            uint32_t packet_length;
            ssize_t bytes_read = recv(client_fd, &packet_length, 4, 0);

            if (bytes_read <= 0) {
                if (bytes_read == 0) {
                    printf("Client %s disconnected\n", client_ip);
                } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    perror("Error reading packet length");
                }
                break;
            }

            // Convert network byte order to host byte order
            packet_length = ntohl(packet_length);

            if (packet_length > 65535 || packet_length == 0) {
                fprintf(stderr, "Invalid packet length from client: %u\n", packet_length);
                continue;
            }

            // Read packet data
            char packet_buffer[65535];
            bytes_read = recv(client_fd, packet_buffer, packet_length, 0);

            if (bytes_read <= 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    perror("Error reading packet data");
                }
                continue;
            }

            // Write packet to TUN device
            ssize_t bytes_written = write(tun_fd, packet_buffer, bytes_read);
            if (bytes_written < 0) {
                perror("Error writing to TUN device");
            } else {
                printf("Forwarded packet from client to TUN, length: %zd\n", bytes_written);
            }
        }

        // Forward packets from TUN to client
        if (FD_ISSET(tun_fd, &read_fds)) {
            char packet_buffer[65535];
            ssize_t bytes_read = read(tun_fd, packet_buffer, sizeof(packet_buffer));

            if (bytes_read > 0) {
                // Send packet length (4 bytes, network byte order)
                uint32_t packet_length = htonl((uint32_t)bytes_read);
                ssize_t bytes_sent = send(client_fd, &packet_length, 4, 0);

                if (bytes_sent != 4) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        perror("Error sending packet length");
                        break;
                    }
                } else {
                    // Send packet data
                    bytes_sent = send(client_fd, packet_buffer, bytes_read, 0);
                    if (bytes_sent < 0) {
                        if (errno != EAGAIN && errno != EWOULDBLOCK) {
                            perror("Error sending packet data");
                            break;
                        }
                    } else {
                        printf("Forwarded packet from TUN to client, length: %zd\n", bytes_sent);
                    }
                }
            } else if (bytes_read < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("Error reading from TUN device");
                break;
            }
        }
    }

    // Cleanup
    printf("Closing connection for client %s\n", client_ip);
    close(tun_fd);
    close(client_fd);
    free(client);

    return NULL;
}

// Signal handler for graceful shutdown
void signal_handler(int sig) {
    printf("\nReceived signal %d, shutting down...\n", sig);
    running = 0;
}

int main() {
    int server_fd, client_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len;

    printf("Starting Net-Rewire Tunnel Server...\n");

    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Create server socket
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("Error creating server socket");
        return 1;
    }

    // Set socket options
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("Error setting socket options");
        close(server_fd);
        return 1;
    }

    // Configure server address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(SERVER_PORT);

    // Bind server socket
    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Error binding server socket");
        close(server_fd);
        return 1;
    }

    // Listen for connections
    if (listen(server_fd, 5) < 0) {
        perror("Error listening on server socket");
        close(server_fd);
        return 1;
    }

    printf("Server listening on port %d\n", SERVER_PORT);

    // Main server loop
    while (running) {
        client_len = sizeof(client_addr);
        client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);

        if (client_fd < 0) {
            if (errno != EINTR) {
                perror("Error accepting client connection");
            }
            continue;
        }

        // Create client info structure
        client_info_t* client_info = malloc(sizeof(client_info_t));
        if (!client_info) {
            fprintf(stderr, "Error allocating client info\n");
            close(client_fd);
            continue;
        }

        client_info->socket_fd = client_fd;
        memcpy(&client_info->client_addr, &client_addr, sizeof(client_addr));

        // Create thread to handle client
        pthread_t thread_id;
        if (pthread_create(&thread_id, NULL, handle_client, client_info) != 0) {
            fprintf(stderr, "Error creating client thread\n");
            free(client_info);
            close(client_fd);
            continue;
        }

        // Detach thread (we don't need to join it)
        pthread_detach(thread_id);
    }

    // Cleanup
    printf("Shutting down server...\n");
    close(server_fd);

    return 0;
}