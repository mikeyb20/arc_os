#ifndef ARCHOS_NET_UDP_H
#define ARCHOS_NET_UDP_H

#include <stdint.h>

#define UDP_HEADER_SIZE 8

typedef struct {
    uint16_t src_port;   /* network byte order */
    uint16_t dst_port;   /* network byte order */
    uint16_t length;     /* header + payload, network byte order */
    uint16_t checksum;   /* UDP pseudo-header checksum */
} __attribute__((packed)) UdpHeader;

struct NetIf;

/* Process a received UDP packet (called from ipv4_rx when protocol==17). */
void udp_rx(struct NetIf *nif, uint32_t src_ip, uint32_t dst_ip,
            const void *data, uint32_t len);

/* Send a UDP datagram. All ports in network byte order. Returns 0 or -1. */
int udp_send(struct NetIf *nif, uint32_t src_ip, uint32_t dst_ip,
             uint16_t src_port, uint16_t dst_port,
             const void *payload, uint32_t payload_len);

#endif /* ARCHOS_NET_UDP_H */
