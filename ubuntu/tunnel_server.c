//
//  tunnel_server.c
//  Net-Rewire Ubuntu TCP Proxy Server
//
//  Accepts connections from macOS daemon, connects to SMTP servers,
//  and relays TCP data bidirectionally.
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/uio.h>

#define SERVER_PORT 12345

static volatile int running = 1;

typedef struct {
    int client_fd;
    char client_ip[INET_ADDRSTRLEN];
} client_info_t;

/* ── Resolve and connect to destination ────────────────────────── */

static int connect_to_dest(const char *host, uint16_t port) {
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;   /* Force IPv4 for SPF alignment */
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    int ret = getaddrinfo(host, port_str, &hints, &res);
    if (ret != 0) {
        fprintf(stderr, "getaddrinfo(%s): %s\n", host, gai_strerror(ret));
        return -1;
    }

    int fd = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

/* ── Read framed message (4-byte BE length + data) ──────────────── */

static ssize_t read_frame(int fd, uint8_t *buf, size_t max_len) {
    uint32_t net_len = 0;
    ssize_t n = recv(fd, &net_len, 4, MSG_WAITALL);
    if (n <= 0) return n;
    if (n != 4) return -1;

    uint32_t len = ntohl(net_len);
    if (len == 0 || len > max_len) return -1;

    size_t total = 0;
    while (total < len) {
        n = recv(fd, buf + total, len - total, 0);
        if (n <= 0) return n;
        total += (size_t)n;
    }
    return (ssize_t)total;
}

/* ── Write framed message ───────────────────────────────────────── */

static ssize_t write_frame(int fd, const uint8_t *data, size_t len) {
    uint32_t net_len = htonl((uint32_t)len);
    struct iovec iov[2];
    iov[0].iov_base = &net_len;
    iov[0].iov_len  = 4;
    iov[1].iov_base = (void *)data;
    iov[1].iov_len  = len;

    ssize_t sent = writev(fd, iov, 2);
    if (sent < 0) return -1;
    if (sent != (ssize_t)(4 + len)) return -1;
    return (ssize_t)len;
}

/* ── Bidirectional relay ────────────────────────────────────────── */

static void relay(int client_fd, int dest_fd) {
    fd_set rfds;
    int max_fd = (client_fd > dest_fd) ? client_fd : dest_fd;

    while (running) {
        FD_ZERO(&rfds);
        FD_SET(client_fd, &rfds);
        FD_SET(dest_fd, &rfds);

        struct timeval tv = {30, 0};  /* 30s idle timeout */
        int rc = select(max_fd + 1, &rfds, NULL, NULL, &tv);
        if (rc <= 0) break;

        /* Client → Destination */
        if (FD_ISSET(client_fd, &rfds)) {
            uint8_t buf[65535];
            ssize_t n = read_frame(client_fd, buf, sizeof(buf));
            if (n <= 0) break;

            ssize_t sent = send(dest_fd, buf, (size_t)n, 0);
            if (sent < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) break;
            }
        }

        /* Destination → Client */
        if (FD_ISSET(dest_fd, &rfds)) {
            uint8_t buf[65535];
            ssize_t n = recv(dest_fd, buf, sizeof(buf), 0);
            if (n <= 0) break;

            if (write_frame(client_fd, buf, (size_t)n) < 0) break;
        }
    }
}

/* ── Client handler ─────────────────────────────────────────────── */

static void *handle_client(void *arg) {
    client_info_t *ci = (client_info_t *)arg;
    int client_fd = ci->client_fd;

    printf("Client connected: %s\n", ci->client_ip);

    /* Read CONNECT message: "CONNECT <host> <port>\n" */
    uint8_t msg_buf[256];
    ssize_t n = read_frame(client_fd, msg_buf, sizeof(msg_buf) - 1);
    if (n <= 0) {
        fprintf(stderr, "Failed to read CONNECT from %s\n", ci->client_ip);
        close(client_fd);
        free(ci);
        return NULL;
    }
    msg_buf[n] = '\0';

    char dest_host[128];
    unsigned int dest_port = 25;
    if (sscanf((char *)msg_buf, "CONNECT %127s %u", dest_host, &dest_port) < 1) {
        fprintf(stderr, "Invalid CONNECT message: %s\n", msg_buf);
        close(client_fd);
        free(ci);
        return NULL;
    }

    printf("%s → CONNECT %s:%u\n", ci->client_ip, dest_host, dest_port);

    /* Connect to destination */
    int dest_fd = connect_to_dest(dest_host, (uint16_t)dest_port);
    if (dest_fd < 0) {
        fprintf(stderr, "Failed to connect to %s:%u for %s\n",
                dest_host, dest_port, ci->client_ip);
        close(client_fd);
        free(ci);
        return NULL;
    }

    printf("%s ↔ %s:%u established\n", ci->client_ip, dest_host, dest_port);

    /* Relay bidirectionally */
    relay(client_fd, dest_fd);

    printf("%s disconnected (%s:%u)\n", ci->client_ip, dest_host, dest_port);
    close(dest_fd);
    close(client_fd);
    free(ci);
    return NULL;
}

/* ── Signal handler ─────────────────────────────────────────────── */

static void sig_handler(int sig) {
    printf("\nSignal %d, shutting down...\n", sig);
    running = 0;
}

/* ── Entry point ─────────────────────────────────────────────────── */

int main(void) {
    int server_fd;
    struct sockaddr_in server_addr;

    setlinebuf(stdout);
    setlinebuf(stderr);
    printf("Starting Net-Rewire TCP Proxy Server...\n");

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(SERVER_PORT);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind"); close(server_fd); return 1;
    }
    if (listen(server_fd, 16) < 0) {
        perror("listen"); close(server_fd); return 1;
    }

    printf("Server listening on port %d\n", SERVER_PORT);

    while (running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept"); continue;
        }

        client_info_t *ci = malloc(sizeof(client_info_t));
        ci->client_fd = client_fd;
        inet_ntop(AF_INET, &client_addr.sin_addr, ci->client_ip, sizeof(ci->client_ip));

        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_client, ci) != 0) {
            perror("pthread_create");
            close(client_fd);
            free(ci);
            continue;
        }
        pthread_detach(tid);
    }

    printf("Shutting down...\n");
    close(server_fd);
    return 0;
}
