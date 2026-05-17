//
//  main.c
//  Net-Rewire Daemon — UTUN-based SMTP tunnel client (macOS)
//
//  Replaces NEPacketTunnelProvider with direct UTUN interface.
//  Requires root. No Apple Network Extension entitlements needed.
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
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/uio.h>
#include <sys/kern_control.h>
#include <sys/sys_domain.h>
#include <net/if.h>
#include <net/if_utun.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "pktparse.h"

/* ── Configuration ───────────────────────────────────────────────── */

#define TUNNEL_SERVER_IP   "10.8.0.1"
#define TUNNEL_CLIENT_IP   "10.8.0.33"
#define TUNNEL_NETMASK     "255.255.255.0"
#define TUNNEL_SERVER_PORT 12345
#define TUNNEL_MTU         1400

/* Real Ubuntu server address — the daemon connects to this over TCP */
static const char *g_ubuntu_host = NULL;  /* set from CLI or config */
static int        g_ubuntu_port = 12345;

static volatile int g_running = 1;

/* ── Logging ─────────────────────────────────────────────────────── */

#define LOG_INFO(fmt, ...)  fprintf(stdout, "[net-rewire] " fmt "\n", ##__VA_ARGS__)
#define LOG_ERR(fmt, ...)   fprintf(stderr, "[net-rewire] ERROR: " fmt "\n", ##__VA_ARGS__)

/* ── UTUN interface management ───────────────────────────────────── */

static int create_utun(char *ifname_out, size_t ifname_len) {
    int fd = socket(PF_SYSTEM, SOCK_DGRAM, SYSPROTO_CONTROL);
    if (fd < 0) {
        LOG_ERR("socket(PF_SYSTEM, SOCK_DGRAM, SYSPROTO_CONTROL) failed: %s",
                strerror(errno));
        return -1;
    }

    /* Look up the UTUN kernel control */
    struct ctl_info ci;
    memset(&ci, 0, sizeof(ci));
    strncpy(ci.ctl_name, UTUN_CONTROL_NAME, sizeof(ci.ctl_name) - 1);

    if (ioctl(fd, CTLIOCGINFO, &ci) < 0) {
        LOG_ERR("CTLIOCGINFO failed for %s: %s", UTUN_CONTROL_NAME, strerror(errno));
        close(fd);
        return -1;
    }

    /* Connect — this creates the UTUN interface */
    struct sockaddr_ctl sc;
    memset(&sc, 0, sizeof(sc));
    sc.sc_len      = sizeof(sc);
    sc.sc_family   = AF_SYSTEM;
    sc.ss_sysaddr  = AF_SYS_CONTROL;
    sc.sc_id       = ci.ctl_id;
    sc.sc_unit     = 0;   /* auto-allocate unit number */

    if (connect(fd, (struct sockaddr *)&sc, sizeof(sc)) < 0) {
        LOG_ERR("connect() to UTUN control failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    /* Retrieve the assigned interface name */
    socklen_t opt_len = (socklen_t)ifname_len;
    if (getsockopt(fd, SYSPROTO_CONTROL, UTUN_OPT_IFNAME,
                   ifname_out, &opt_len) < 0) {
        /* Fallback: derive from getsockname */
        struct sockaddr_ctl peer;
        socklen_t peer_len = sizeof(peer);
        if (getsockname(fd, (struct sockaddr *)&peer, &peer_len) == 0) {
            snprintf(ifname_out, ifname_len, "utun%u", peer.sc_unit - 1);
        } else {
            strlcpy(ifname_out, "utun?", ifname_len);
        }
    }

    LOG_INFO("Created UTUN interface: %s (fd=%d)", ifname_out, fd);
    return fd;
}

static int configure_utun(const char *ifname,
                          const char *addr, const char *mask,
                          const char *dstaddr, int mtu) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        LOG_ERR("socket(AF_INET) for ioctl failed: %s", strerror(errno));
        return -1;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strlcpy(ifr.ifr_name, ifname, IFNAMSIZ);

    /* Set MTU */
    ifr.ifr_mtu = mtu;
    if (ioctl(s, SIOCSIFMTU, &ifr) < 0) {
        LOG_ERR("SIOCSIFMTU failed: %s", strerror(errno));
    }

    /* Bring interface up */
    if (ioctl(s, SIOCGIFFLAGS, &ifr) == 0) {
        ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
        if (ioctl(s, SIOCSIFFLAGS, &ifr) < 0) {
            LOG_ERR("SIOCSIFFLAGS (UP) failed: %s", strerror(errno));
        }
    }

    close(s);

    /* Configure IP address via system() — most reliable across macOS versions */
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "ifconfig %s inet %s %s netmask %s mtu %d up 2>/dev/null",
             ifname, addr, dstaddr, mask, mtu);
    if (system(cmd) != 0) {
        LOG_ERR("ifconfig failed: %s", cmd);
        return -1;
    }

    LOG_INFO("Configured %s: %s -> %s/%s mtu %d",
             ifname, addr, dstaddr, mask, mtu);
    return 0;
}

/* ── PF (Packet Filter) setup ────────────────────────────────────── */

static int setup_pf_route(const char *ifname) {
    /*
     * Create a temporary PF anchor that routes outbound TCP/25
     * through the UTUN interface.  We use a dedicated anchor file.
     */
    const char *anchor_path = "/etc/pf.anchors/net-rewire";
    const char *anchor_name = "net-rewire";

    /* Write the PF anchor rule */
    FILE *fp = fopen(anchor_path, "w");
    if (!fp) {
        LOG_ERR("Cannot write %s: %s (run as root)", anchor_path, strerror(errno));
        return -1;
    }
    fprintf(fp,
            "# Net-Rewire SMTP tunnel rules\n"
            "# Route outbound TCP port 25 through the UTUN interface\n"
            "pass out route-to %s proto tcp from any to any port 25\n",
            ifname);
    fclose(fp);
    LOG_INFO("Wrote PF anchor: %s", anchor_path);

    /* Ensure the anchor is loaded in pf.conf */
    const char *pf_conf = "/etc/pf.conf";
    fp = fopen(pf_conf, "r");
    int has_anchor = 0;
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, anchor_name)) { has_anchor = 1; break; }
        }
        fclose(fp);
    }

    if (!has_anchor) {
        fp = fopen(pf_conf, "a");
        if (fp) {
            fprintf(fp,
                    "\n# Net-Rewire SMTP tunnel\n"
                    "rdr-anchor \"%s\"\n"
                    "anchor \"%s\"\n"
                    "load anchor \"%s\" from \"%s\"\n",
                    anchor_name, anchor_name, anchor_name, anchor_path);
            fclose(fp);
            LOG_INFO("Added anchor %s to %s", anchor_name, pf_conf);
        }
    }

    /* Enable PF if not already running */
    if (system("pfctl -s info 2>/dev/null | grep -q 'Status: Enabled'") != 0) {
        LOG_INFO("Enabling PF...");
        system("pfctl -e 2>/dev/null");
    }

    /* Load the rules */
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "pfctl -a %s -f %s 2>&1", anchor_name, anchor_path);
    int rc = system(cmd);
    if (rc != 0) {
        LOG_ERR("pfctl load anchor failed (rc=%d): %s", rc, cmd);
        return -1;
    }

    LOG_INFO("PF rules loaded for %s", ifname);
    return 0;
}

static void remove_pf_rules(void) {
    system("pfctl -a net-rewire -F all 2>/dev/null");
    LOG_INFO("PF rules removed");
}

/* ── Tunnel client (connects to Ubuntu server) ────────────────────── */

static int connect_to_ubuntu(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_ERR("socket() for tunnel: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(g_ubuntu_port);

    if (inet_pton(AF_INET, g_ubuntu_host, &sa.sin_addr) != 1) {
        LOG_ERR("Invalid Ubuntu host: %s", g_ubuntu_host);
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        LOG_ERR("connect() to %s:%d failed: %s",
                g_ubuntu_host, g_ubuntu_port, strerror(errno));
        close(fd);
        return -1;
    }

    LOG_INFO("Connected to Ubuntu tunnel server %s:%d (fd=%d)",
             g_ubuntu_host, g_ubuntu_port, fd);
    return fd;
}

/* ── Bidirectional forwarding ────────────────────────────────────── */

static int forward_utun_to_ubuntu(int utun_fd, int ubuntu_fd) {
    /*
     * Read a raw IP packet from UTUN (with 4-byte AF prefix),
     * filter for TCP/25, encapsulate with 4-byte length prefix,
     * send to Ubuntu.
     */
    uint8_t buf[65536 + 4];  /* 4 extra for UTUN header */
    ssize_t n = read(utun_fd, buf, sizeof(buf));

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        LOG_ERR("read(utun): %s", strerror(errno));
        return -1;
    }
    if (n < 4) {
        LOG_ERR("read(utun): short read (%zd bytes)", n);
        return 0;
    }

    /* UTUN prefix: buf[0..2] reserved, buf[3] = address family */
    uint8_t *ip_packet = buf + 4;
    size_t   ip_len    = (size_t)(n - 4);

    /* Parse IP header to check if this is TCP/25 */
    struct pkt_info info;
    if (!pkt_parse(ip_packet, ip_len, &info)) {
        return 0;  /* Not parseable, drop */
    }

    if (!info.is_tcp || ntohs(info.tcp_dst) != 25) {
        return 0;  /* Not SMTP, don't tunnel */
    }

    /* Encapsulate: 4-byte BE length + raw IP packet */
    uint32_t net_len = htonl((uint32_t)ip_len);

    struct iovec iov[2];
    iov[0].iov_base = &net_len;
    iov[0].iov_len  = 4;
    iov[1].iov_base = ip_packet;
    iov[1].iov_len  = ip_len;

    ssize_t sent = writev(ubuntu_fd, iov, 2);
    if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        LOG_ERR("writev(ubuntu): %s", strerror(errno));
        return -1;
    }
    if (sent != (ssize_t)(4 + ip_len)) {
        LOG_ERR("writev(ubuntu): partial send %zd / %zu", sent, 4 + ip_len);
    }

    char src[INET_ADDRSTRLEN], dst[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &info.ip_src, src, sizeof(src));
    inet_ntop(AF_INET, &info.ip_dst, dst, sizeof(dst));
    LOG_INFO("→ FWD %s:%u → %s:%u  (%zu bytes)",
             src, ntohs(info.tcp_src), dst, ntohs(info.tcp_dst), ip_len);

    return 0;
}

static int forward_ubuntu_to_utun(int ubuntu_fd, int utun_fd) {
    /*
     * Read encapsulated IP packet from Ubuntu,
     * write it to the UTUN interface (with 4-byte family prefix).
     */
    uint32_t net_len = 0;
    ssize_t n = recv(ubuntu_fd, &net_len, 4, 0);

    if (n == 0) {
        LOG_INFO("Ubuntu server closed connection");
        return -1;
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        LOG_ERR("recv(ubuntu) length: %s", strerror(errno));
        return -1;
    }
    if (n != 4) {
        LOG_ERR("recv(ubuntu): short length read (%zd)", n);
        return -1;
    }

    uint32_t ip_len = ntohl(net_len);
    if (ip_len > 65535 || ip_len == 0) {
        LOG_ERR("Bad packet length from Ubuntu: %u", ip_len);
        return 0;
    }

    uint8_t ip_buf[65535];
    n = recv(ubuntu_fd, ip_buf, ip_len, 0);
    if (n != (ssize_t)ip_len) {
        LOG_ERR("recv(ubuntu) data: expected %u got %zd (%s)",
                ip_len, n, n < 0 ? strerror(errno) : "short");
        return (n <= 0) ? -1 : 0;
    }

    /* Determine address family from IP version nibble */
    uint8_t af_byte;
    if ((ip_buf[0] >> 4) == 6) {
        af_byte = AF_INET6;  /* 30 */
    } else {
        af_byte = AF_INET;   /* 2 */
    }

    /* Build UTUN header: 3 reserved bytes + AF */
    struct iovec iov[2];
    uint8_t utun_hdr[4] = {0, 0, 0, af_byte};
    iov[0].iov_base = utun_hdr;
    iov[0].iov_len  = 4;
    iov[1].iov_base = ip_buf;
    iov[1].iov_len  = ip_len;

    ssize_t sent = writev(utun_fd, iov, 2);
    if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        LOG_ERR("writev(utun): %s", strerror(errno));
        return -1;
    }

    LOG_INFO("← RCV %u bytes from Ubuntu → utun", ip_len);
    return 0;
}

/* ── Main event loop ─────────────────────────────────────────────── */

static void event_loop(int utun_fd, int ubuntu_fd) {
    fd_set rfds;
    int max_fd = (utun_fd > ubuntu_fd) ? utun_fd : ubuntu_fd;

    LOG_INFO("Entering event loop (utun=%d, ubuntu=%d, max=%d)",
             utun_fd, ubuntu_fd, max_fd);

    while (g_running) {
        FD_ZERO(&rfds);
        FD_SET(utun_fd, &rfds);
        FD_SET(ubuntu_fd, &rfds);

        struct timeval tv = {1, 0};  /* 1s timeout */
        int rc = select(max_fd + 1, &rfds, NULL, NULL, &tv);

        if (rc < 0) {
            if (errno == EINTR) continue;
            LOG_ERR("select(): %s", strerror(errno));
            break;
        }

        if (rc == 0) continue;  /* timeout — check g_running */

        /* UTUN → Ubuntu: outbound packets */
        if (FD_ISSET(utun_fd, &rfds)) {
            if (forward_utun_to_ubuntu(utun_fd, ubuntu_fd) < 0) break;
        }

        /* Ubuntu → UTUN: returning packets */
        if (FD_ISSET(ubuntu_fd, &rfds)) {
            if (forward_ubuntu_to_utun(ubuntu_fd, utun_fd) < 0) break;
        }

        /* If both are still set, process both */
        if (FD_ISSET(utun_fd, &rfds)) {
            if (forward_utun_to_ubuntu(utun_fd, ubuntu_fd) < 0) break;
        }
    }

    LOG_INFO("Event loop exited");
}

/* ── Signal handler ──────────────────────────────────────────────── */

static void sig_handler(int sig) {
    LOG_INFO("Received signal %d, shutting down...", sig);
    g_running = 0;
}

/* ── Usage ───────────────────────────────────────────────────────── */

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s <ubuntu-host> [ubuntu-port]\n"
            "\n"
            "  ubuntu-host   IP address of the Ubuntu tunnel server\n"
            "  ubuntu-port   TCP port (default: 12345)\n"
            "\n"
            "Creates a UTUN interface and routes outbound TCP/25\n"
            "traffic through the Ubuntu tunnel server.\n"
            "Requires root privileges.\n",
            prog);
}

/* ── Entry point ─────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    g_ubuntu_host = argv[1];
    if (argc >= 3) {
        g_ubuntu_port = atoi(argv[2]);
        if (g_ubuntu_port <= 0 || g_ubuntu_port > 65535) {
            LOG_ERR("Invalid port: %s", argv[2]);
            return 1;
        }
    }

    if (geteuid() != 0) {
        LOG_ERR("This daemon requires root privileges. Use sudo.");
        return 1;
    }

    LOG_INFO("Net-Rewire Daemon starting...");
    LOG_INFO("Ubuntu server: %s:%d", g_ubuntu_host, g_ubuntu_port);

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    /* 1. Create UTUN interface */
    char ifname[IFNAMSIZ] = {0};
    int utun_fd = create_utun(ifname, sizeof(ifname));
    if (utun_fd < 0) return 1;

    /* 2. Configure UTUN */
    if (configure_utun(ifname,
                       TUNNEL_CLIENT_IP, TUNNEL_NETMASK,
                       TUNNEL_SERVER_IP, TUNNEL_MTU) < 0) {
        close(utun_fd);
        return 1;
    }

    /* 3. Set up PF rules to route TCP/25 through UTUN */
    if (setup_pf_route(ifname) < 0) {
        LOG_ERR("PF setup failed — port-25 traffic will NOT be redirected");
        close(utun_fd);
        return 1;
    }

    /* 4. Connect to Ubuntu tunnel server */
    int ubuntu_fd = connect_to_ubuntu();
    if (ubuntu_fd < 0) {
        remove_pf_rules();
        close(utun_fd);
        return 1;
    }

    /* 5. Set both fds non-blocking */
    fcntl(utun_fd,   F_SETFL, O_NONBLOCK);
    fcntl(ubuntu_fd, F_SETFL, O_NONBLOCK);

    /* 6. Main event loop */
    event_loop(utun_fd, ubuntu_fd);

    /* 7. Cleanup */
    LOG_INFO("Shutting down...");
    remove_pf_rules();
    close(ubuntu_fd);
    close(utun_fd);

    LOG_INFO("Net-Rewire Daemon stopped.");
    return 0;
}
