#ifndef ARCHOS_NET_ICMP_H
#define ARCHOS_NET_ICMP_H

#include <stdint.h>

#define ICMP_TYPE_ECHO_REPLY    0
#define ICMP_TYPE_ECHO_REQUEST  8
#define ICMP_HEADER_SIZE        8

typedef struct {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t sequence;
} __attribute__((packed)) IcmpHeader;

struct NetIf;

/* Process a received ICMP packet (called from ipv4_rx). */
void icmp_rx(struct NetIf *nif, uint32_t src_ip, const void *data, uint32_t len);

#endif /* ARCHOS_NET_ICMP_H */
