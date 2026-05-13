//
//  pktparse.h
//  NetRewirePacketTunnel
//
//  Created by Claude Code
//

#ifndef PKTPARSE_H
#define PKTPARSE_H

#include <stdint.h>
#include <arpa/inet.h>
#include <string.h>

struct pkt_info {
    int is_ipv4;
    int is_tcp;
    uint32_t ip_src;
    uint32_t ip_dst;
    uint16_t tcp_src;
    uint16_t tcp_dst;
    int ip_header_len;
    int tcp_header_len;
};

/**
 * Parse IP packet and extract key information
 * @param buf Raw packet bytes
 * @param len Packet length
 * @param info Output structure with parsed information
 * @return 1 if successfully parsed, 0 otherwise
 */
int pkt_parse(const uint8_t *buf, size_t len, struct pkt_info *info);

#endif