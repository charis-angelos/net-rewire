//
//  main.c
//  Net-Rewire Daemon — SMTP-aware TCP proxy (macOS)
//
//  Listens on 0.0.0.0:2525, parses SMTP envelope to extract
//  recipient domain, resolves MX records, and tunnels each
//  connection through the Ubuntu relay server via Tailscale.
//  No PF rdr, no UTUN — Postfix connects directly to the proxy port.
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/uio.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <arpa/nameser.h>
#include <resolv.h>
#include <net/if.h>
#include <netdb.h>

/* ── Configuration ───────────────────────────────────────────────── */

#define LOCAL_PROXY_PORT  2525
#define TUNNEL_SERVER_PORT 12345

static const char *g_ubuntu_host = NULL;
static int        g_ubuntu_port = 12345;
static volatile int g_running = 1;

/* ── Logging (unbuffered) ─────────────────────────────────────── */

#define LOG_INFO(fmt, ...)  do { \
    fprintf(stdout, "[net-rewire] " fmt "\n", ##__VA_ARGS__); fflush(stdout); \
} while(0)
#define LOG_ERR(fmt, ...)   do { \
    fprintf(stderr, "[net-rewire] ERROR: " fmt "\n", ##__VA_ARGS__); fflush(stderr); \
} while(0)

/* ── Tunnel connection to Ubuntu ──────────────────────────────── */

static int connect_to_ubuntu(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { LOG_ERR("socket(): %s", strerror(errno)); return -1; }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(g_ubuntu_port);
    if (inet_pton(AF_INET, g_ubuntu_host, &sa.sin_addr) != 1) {
        LOG_ERR("Invalid host: %s", g_ubuntu_host);
        close(fd); return -1;
    }

    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        LOG_ERR("connect() to %s:%d: %s", g_ubuntu_host, g_ubuntu_port, strerror(errno));
        close(fd); return -1;
    }

    LOG_INFO("Connected to Ubuntu tunnel server %s:%d (fd=%d)",
             g_ubuntu_host, g_ubuntu_port, fd);
    return fd;
}

/* ── Frame protocol helpers (4-byte BE length + data) ─────────── */

static ssize_t read_frame(int fd, uint8_t *buf, size_t max_len) {
    uint32_t net_len = 0;
    ssize_t n = recv(fd, &net_len, 4, 0);
    if (n <= 0) return n;
    if (n < 4) {
        /* Partial read — drain remaining */
        size_t got = (size_t)n;
        while (got < 4) {
            n = recv(fd, ((uint8_t *)&net_len) + got, 4 - got, 0);
            if (n <= 0) return n;
            got += (size_t)n;
        }
    }

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

/* ── MX record resolution ─────────────────────────────────────── */

static int resolve_mx(const char *domain, char *mx_host, size_t mx_host_len) {
    unsigned char response[NS_PACKETSZ];
    int len = res_query(domain, ns_c_in, ns_t_mx, response, sizeof(response));
    if (len < 0) {
        LOG_INFO("No MX record for %s, using A-record fallback", domain);
        strlcpy(mx_host, domain, mx_host_len);
        return 0;
    }

    ns_msg handle;
    if (ns_initparse(response, len, &handle) < 0) {
        LOG_ERR("Failed to parse DNS response for %s", domain);
        strlcpy(mx_host, domain, mx_host_len);
        return 0;
    }

    uint16_t best_pref = 0xFFFF;
    char best_name[NS_MAXDNAME] = {0};
    int found = 0;

    for (int i = 0; i < ns_msg_count(handle, ns_s_an); i++) {
        ns_rr rr;
        if (ns_parserr(&handle, ns_s_an, i, &rr) < 0) continue;
        if (ns_rr_type(rr) != ns_t_mx) continue;

        const unsigned char *rdata = ns_rr_rdata(rr);
        uint16_t pref = ns_get16(rdata);

        char name[NS_MAXDNAME];
        if (ns_name_uncompress(ns_msg_base(handle), ns_msg_end(handle),
                               rdata + 2, name, sizeof(name)) < 0) continue;

        if (pref < best_pref) {
            best_pref = pref;
            strlcpy(best_name, name, sizeof(best_name));
            found = 1;
        }
    }

    if (found) {
        strlcpy(mx_host, best_name, mx_host_len);
        LOG_INFO("MX for %s → %s (pref=%u)", domain, mx_host, best_pref);
        return 1;
    } else {
        LOG_INFO("No MX records for %s, using A fallback", domain);
        strlcpy(mx_host, domain, mx_host_len);
        return 0;
    }
}

/* ── SMTP line I/O helpers ────────────────────────────────────── */

/* Read one CRLF-terminated line with a timeout (seconds).
   Returns length without the trailing \r\n, or -1 on error/timeout. */
static ssize_t read_smtp_line(int fd, char *buf, size_t bufsz, int timeout_s) {
    size_t pos = 0;
    struct timeval start;
    gettimeofday(&start, NULL);

    while (pos < bufsz - 1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);

        struct timeval now;
        gettimeofday(&now, NULL);
        long elapsed = (now.tv_sec - start.tv_sec);
        long remaining = timeout_s - elapsed;
        if (remaining <= 0) return -1;

        struct timeval tv = {remaining, 0};
        int rc = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (rc <= 0) return -1;

        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) return -1;

        buf[pos++] = c;
        if (c == '\n') {
            buf[pos] = '\0';
            /* Strip trailing \r\n */
            size_t len = pos;
            if (len >= 2 && buf[len - 2] == '\r') len -= 2;
            else if (len >= 1 && buf[len - 1] == '\n') len -= 1;
            buf[len] = '\0';
            return (ssize_t)len;
        }
    }
    return -1;
}

/* Send a line with \r\n appended */
static int send_smtp_line(int fd, const char *line) {
    size_t len = strlen(line);
    struct iovec iov[2];
    iov[0].iov_base = (void *)line;
    iov[0].iov_len  = len;
    iov[1].iov_base = "\r\n";
    iov[1].iov_len  = 2;
    ssize_t sent = writev(fd, iov, 2);
    if (sent != (ssize_t)(len + 2)) return -1;
    return 0;
}

/* Extract domain from RCPT TO:<user@domain.com> */
static int extract_rcpt_domain(const char *line, char *domain, size_t domain_size) {
    const char *p = strchr(line, '<');
    if (!p) return -1;
    p++;
    const char *at = strchr(p, '@');
    if (!at) return -1;
    const char *end = strchr(at + 1, '>');
    if (!end) return -1;
    size_t len = end - at - 1;
    if (len >= domain_size) len = domain_size - 1;
    memcpy(domain, at + 1, len);
    domain[len] = '\0';
    return 0;
}

/* ── Bidirectional relay ──────────────────────────────────────── */

static void relay_pair(int local_fd, int ubuntu_fd) {
    fd_set rfds;
    int max_fd = (local_fd > ubuntu_fd) ? local_fd : ubuntu_fd;

    fcntl(local_fd,  F_SETFL, O_NONBLOCK);
    fcntl(ubuntu_fd, F_SETFL, O_NONBLOCK);

    while (g_running) {
        FD_ZERO(&rfds);
        FD_SET(local_fd, &rfds);
        FD_SET(ubuntu_fd, &rfds);

        struct timeval tv = {30, 0}; /* 30 s idle timeout */
        int rc = select(max_fd + 1, &rfds, NULL, NULL, &tv);
        if (rc < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (rc == 0) break;

        /* Local (Postfix) → Ubuntu */
        if (FD_ISSET(local_fd, &rfds)) {
            uint8_t buf[65535];
            ssize_t n = recv(local_fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            if (write_frame(ubuntu_fd, buf, (size_t)n) < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) break;
            }
        }

        /* Ubuntu → Local (Postfix) */
        if (FD_ISSET(ubuntu_fd, &rfds)) {
            uint8_t buf[65535];
            ssize_t n = read_frame(ubuntu_fd, buf, sizeof(buf));
            if (n <= 0) break;
            ssize_t sent = send(local_fd, buf, (size_t)n, 0);
            if (sent < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) break;
            }
        }
    }
}

/* ── Per-connection handler ───────────────────────────────────── */

typedef struct {
    int local_fd;
} conn_info_t;

/*
 * Interactive SMTP proxy:
 *  1. Send 220, read EHLO  → send fake 250
 *  2. Read MAIL FROM       → send fake 250
 *  3. Read RCPT TO         → extract domain, MX lookup
 *  4. Connect Ubuntu, send CONNECT <mx>:25
 *  5. Discard real 220     → replay EHLO/MAIL/RCPT with real MX
 *  6. Enter relay mode
 */
static void *handle_local_conn(void *arg) {
    conn_info_t *ci = (conn_info_t *)arg;
    int local_fd = ci->local_fd;
    free(ci);

    LOG_INFO("New SMTP connection (fd=%d)", local_fd);

    /* ── Phase 1: 220 banner ── */
    if (send_smtp_line(local_fd, "220 mail.dtype.info ESMTP Net-Rewire") < 0) {
        LOG_ERR("Failed to send 220");
        close(local_fd);
        return NULL;
    }

    /* ── Phase 2: Read EHLO, fake 250 response ── */
    char line[2048];
    char ehlo_line[2048] = "";
    char mail_line[2048] = "";
    char rcpt_line[2048] = "";
    char rcpt_domain[256] = "";

    ssize_t n = read_smtp_line(local_fd, line, sizeof(line), 60);
    if (n <= 0) {
        LOG_ERR("No EHLO received");
        close(local_fd);
        return NULL;
    }
    LOG_INFO("Postfix → %s", line);
    strlcpy(ehlo_line, line, sizeof(ehlo_line));

    /* Fake 250-xxx multi-line response */
    send_smtp_line(local_fd, "250-mail.dtype.info");
    send_smtp_line(local_fd, "250-PIPELINING");
    send_smtp_line(local_fd, "250-SIZE 52428800");
    send_smtp_line(local_fd, "250 8BITMIME");

    /* ── Phase 3: Read MAIL FROM, fake 250 ── */
    n = read_smtp_line(local_fd, line, sizeof(line), 60);
    if (n <= 0) {
        LOG_ERR("No MAIL FROM");
        close(local_fd);
        return NULL;
    }
    LOG_INFO("Postfix → %s", line);
    strlcpy(mail_line, line, sizeof(mail_line));
    send_smtp_line(local_fd, "250 2.1.0 Ok");

    /* ── Phase 4: Read RCPT TO, extract domain ── */
    n = read_smtp_line(local_fd, line, sizeof(line), 60);
    if (n <= 0) {
        LOG_ERR("No RCPT TO");
        close(local_fd);
        return NULL;
    }
    LOG_INFO("Postfix → %s", line);
    strlcpy(rcpt_line, line, sizeof(rcpt_line));

    if (extract_rcpt_domain(line, rcpt_domain, sizeof(rcpt_domain)) < 0) {
        LOG_ERR("Failed to extract domain from RCPT TO");
        send_smtp_line(local_fd, "501 5.5.4 Bad recipient");
        close(local_fd);
        return NULL;
    }
    LOG_INFO("RCPT domain: %s", rcpt_domain);

    /* ── Phase 5: MX lookup ── */
    char mx_host[256];
    resolve_mx(rcpt_domain, mx_host, sizeof(mx_host));
    LOG_INFO("Target MX: %s:25", mx_host);

    /* ── Phase 6: Connect to Ubuntu, send CONNECT ── */
    int ubuntu_fd = connect_to_ubuntu();
    if (ubuntu_fd < 0) {
        send_smtp_line(local_fd, "421 4.3.0 Service unavailable");
        close(local_fd);
        return NULL;
    }

    char connect_msg[384];
    int msg_len = snprintf(connect_msg, sizeof(connect_msg),
                           "CONNECT %s 25\n", mx_host);
    if (write_frame(ubuntu_fd, (uint8_t *)connect_msg, (size_t)msg_len) < 0) {
        LOG_ERR("Failed to send CONNECT");
        send_smtp_line(local_fd, "421 4.3.0 Service unavailable");
        close(ubuntu_fd);
        close(local_fd);
        return NULL;
    }

    /* ── Phase 7: Discard real MX 220 banner ── */
    uint8_t discard[65535];
    ssize_t blen = read_frame(ubuntu_fd, discard, sizeof(discard));
    if (blen <= 0) {
        LOG_ERR("No 220 from MX %s", mx_host);
        send_smtp_line(local_fd, "421 4.3.0 Service unavailable");
        close(ubuntu_fd);
        close(local_fd);
        return NULL;
    }
    LOG_INFO("MX 220 banner (%zd bytes), discarding", blen);

    /* ── Phase 8: Replay EHLO to real MX, discard response ── */
    {
        char framed[2304];
        int flen = snprintf(framed, sizeof(framed), "%s\r\n", ehlo_line);
        LOG_INFO("Replay: %s", ehlo_line);
        if (write_frame(ubuntu_fd, (uint8_t *)framed, (size_t)flen) < 0) {
            close(ubuntu_fd); close(local_fd); return NULL;
        }
        uint8_t resp[65535];
        ssize_t rlen = read_frame(ubuntu_fd, resp, sizeof(resp));
        if (rlen <= 0) { close(ubuntu_fd); close(local_fd); return NULL; }
        /* Discard — Postfix already got fake 250 for EHLO */
        LOG_INFO("MX EHLO response (%zd bytes), discarding", rlen);
    }

    /* ── Phase 9: Replay MAIL FROM to real MX, discard response ── */
    {
        char framed[2304];
        int flen = snprintf(framed, sizeof(framed), "%s\r\n", mail_line);
        LOG_INFO("Replay: %s", mail_line);
        if (write_frame(ubuntu_fd, (uint8_t *)framed, (size_t)flen) < 0) {
            close(ubuntu_fd); close(local_fd); return NULL;
        }
        uint8_t resp[65535];
        ssize_t rlen = read_frame(ubuntu_fd, resp, sizeof(resp));
        if (rlen <= 0) { close(ubuntu_fd); close(local_fd); return NULL; }
        /* Discard — Postfix already got fake 250 for MAIL FROM */
        LOG_INFO("MX MAIL FROM response (%zd bytes), discarding", rlen);
    }

    /* ── Phase 10: Replay RCPT TO → forward response ── */
    {
        char framed[2304];
        int flen = snprintf(framed, sizeof(framed), "%s\r\n", rcpt_line);
        LOG_INFO("Replay: %s", rcpt_line);
        if (write_frame(ubuntu_fd, (uint8_t *)framed, (size_t)flen) < 0) {
            close(ubuntu_fd); close(local_fd); return NULL;
        }
        uint8_t resp[65535];
        ssize_t rlen = read_frame(ubuntu_fd, resp, sizeof(resp));
        if (rlen <= 0) { close(ubuntu_fd); close(local_fd); return NULL; }
        send(local_fd, resp, (size_t)rlen, 0);
    }

    LOG_INFO("SMTP replay done (MX=%s), entering relay mode", mx_host);

    /* ── Phase 11: Bidirectional relay ── */
    relay_pair(local_fd, ubuntu_fd);

    LOG_INFO("SMTP connection done (MX=%s)", mx_host);
    close(ubuntu_fd);
    close(local_fd);
    return NULL;
}

/* ── Signal handler ────────────────────────────────────────────── */

static void sig_handler(int sig) {
    LOG_INFO("Signal %d, shutting down...", sig);
    g_running = 0;
}

/* ── Usage ─────────────────────────────────────────────────────── */

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s <ubuntu-host> [ubuntu-port]\n"
            "\n"
            "  ubuntu-host   IP / hostname of the Ubuntu tunnel server\n"
            "  ubuntu-port   TCP port on Ubuntu (default: 12345)\n"
            "\n"
            "SMTP-aware proxy. Listens on 0.0.0.0:2525, parses RCPT TO\n"
            "to resolve MX records, then tunnels through the Ubuntu relay.\n"
            "Requires root privileges.\n",
            prog);
}

/* ── Entry point ────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    if (argc < 2) { usage(argv[0]); return 1; }

    g_ubuntu_host = argv[1];
    if (argc >= 3) {
        g_ubuntu_port = atoi(argv[2]);
        if (g_ubuntu_port <= 0 || g_ubuntu_port > 65535) {
            LOG_ERR("Invalid port: %s", argv[2]); return 1;
        }
    }

    if (geteuid() != 0) {
        LOG_ERR("Root required. Use sudo."); return 1;
    }

    LOG_INFO("Net-Rewire Daemon (SMTP-aware proxy mode) starting...");
    LOG_INFO("Ubuntu server: %s:%d", g_ubuntu_host, g_ubuntu_port);

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    /* ── Listen on 0.0.0.0:2525 ── */

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        LOG_ERR("socket(): %s", strerror(errno)); return 1;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in la;
    memset(&la, 0, sizeof(la));
    la.sin_family = AF_INET;
    la.sin_addr.s_addr = INADDR_ANY;
    la.sin_port = htons(LOCAL_PROXY_PORT);

    if (bind(listen_fd, (struct sockaddr *)&la, sizeof(la)) < 0) {
        LOG_ERR("bind(): %s", strerror(errno)); return 1;
    }
    if (listen(listen_fd, 16) < 0) {
        LOG_ERR("listen(): %s", strerror(errno)); return 1;
    }

    LOG_INFO("Listening on 0.0.0.0:%d", LOCAL_PROXY_PORT);

    /* ── Accept loop ── */

    while (g_running) {
        struct sockaddr_in ca;
        socklen_t ca_len = sizeof(ca);
        int local_fd = accept(listen_fd, (struct sockaddr *)&ca, &ca_len);
        if (local_fd < 0) {
            if (errno == EINTR) continue;
            LOG_ERR("accept(): %s", strerror(errno)); break;
        }

        conn_info_t *ci = malloc(sizeof(conn_info_t));
        ci->local_fd = local_fd;

        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_local_conn, ci) != 0) {
            LOG_ERR("pthread_create failed");
            close(local_fd);
            free(ci);
            continue;
        }
        pthread_detach(tid);
    }

    LOG_INFO("Shutting down...");
    close(listen_fd);
    LOG_INFO("Net-Rewire Daemon stopped.");
    return 0;
}
