#ifndef ARCHOS_NET_IPV4_H
#define ARCHOS_NET_IPV4_H

#include <stdint.h>

#define IPV4_HEADER_SIZE  20
#define IPV4_PROTO_ICMP   1
#define IPV4_PROTO_TCP    6
#define IPV4_PROTO_UDP   17
#define IPV4_DEFAULT_TTL  64

typedef struct {
    uint8_t  ver_ihl;
    uint8_t  tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint32_t src_ip;
    uint32_t dst_ip;
} __attribute__((packed)) Ipv4Header;

struct NetIf;

/* Process a received IPv4 packet (called from eth_rx). */
void ipv4_rx(struct NetIf *nif, const void *data, uint32_t len);

/* Send an IPv4 packet. Resolves next-hop MAC via ARP, sends via Ethernet. */
int ipv4_send(struct NetIf *nif, uint32_t dst_ip, uint8_t protocol,
              const void *payload, uint32_t payload_len);

#endif /* ARCHOS_NET_IPV4_H */
