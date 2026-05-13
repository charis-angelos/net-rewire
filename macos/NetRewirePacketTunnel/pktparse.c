//
//  pktparse.c
//  NetRewirePacketTunnel
//
//  Created by Claude Code
//

#include "pktparse.h"
#include <string.h>
#include <arpa/inet.h>

int pkt_parse(const uint8_t *buf, size_t len, struct pkt_info *info) {
    memset(info, 0, sizeof(*info));

    // Check minimum length for IPv4 header (20 bytes)
    if (len < 20) {
        return 0;
    }

    // Parse IP header manually (no dependency on netinet/ip.h)
    uint8_t version = (buf[0] >> 4) & 0x0F;
    if (version != 4) {
        return 0;  // Not IPv4
    }

    uint8_t ihl = (buf[0] & 0x0F) * 4;
    if (ihl < 20 || len < ihl) {
        return 0;
    }

    info->is_ipv4 = 1;
    info->ip_header_len = ihl;

    // Source IP (bytes 12-15) and Dest IP (bytes 16-19), network byte order
    memcpy(&info->ip_src, &buf[12], 4);
    memcpy(&info->ip_dst, &buf[16], 4);

    uint8_t protocol = buf[9];
    if (protocol != 6) {  // 6 = TCP
        return 1;  // Valid IP packet but not TCP
    }

    // TCP header starts at offset ihl, minimum 20 bytes
    if (len < (size_t)ihl + 20) {
        return 1;  // Valid IP packet but TCP header incomplete
    }

    uint8_t tcp_header_len = (buf[ihl + 12] >> 4) * 4;

    info->is_tcp = 1;
    info->tcp_header_len = tcp_header_len;

    // Source port and dest port in NETWORK byte order (like struct tcphdr fields)
    uint16_t src_port, dst_port;
    memcpy(&src_port, &buf[ihl], 2);
    memcpy(&dst_port, &buf[ihl + 2], 2);
    info->tcp_src = src_port;
    info->tcp_dst = dst_port;

    return 1;
}