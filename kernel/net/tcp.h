#ifndef ARCHOS_NET_TCP_H
#define ARCHOS_NET_TCP_H

#include <stdint.h>

#define IPV4_PROTO_TCP  6
#define TCP_HEADER_SIZE 20  /* Minimum (no options) */

/* TCP flags */
#define TCP_FIN  0x01
#define TCP_SYN  0x02
#define TCP_RST  0x04
#define TCP_PSH  0x08
#define TCP_ACK  0x10
#define TCP_URG  0x20

typedef struct {
    uint16_t src_port;    /* Network byte order */
    uint16_t dst_port;    /* Network byte order */
    uint32_t seq;         /* Network byte order */
    uint32_t ack;         /* Network byte order */
    uint8_t  data_off;    /* Upper 4 bits = header length in 32-bit words */
    uint8_t  flags;       /* TCP flags */
    uint16_t window;      /* Network byte order */
    uint16_t checksum;    /* Network byte order */
    uint16_t urgent;      /* Network byte order */
} __attribute__((packed)) TcpHeader;

struct NetIf;

/* Process a received TCP segment (called from ipv4_rx). */
void tcp_rx(struct NetIf *nif, uint32_t src_ip, uint32_t dst_ip,
            const void *data, uint32_t len);

/* Send a TCP segment with given flags and optional payload. */
int tcp_send(struct NetIf *nif, uint32_t src_ip, uint16_t src_port,
             uint32_t dst_ip, uint16_t dst_port,
             uint32_t seq, uint32_t ack_num, uint8_t flags,
             const void *payload, uint32_t payload_len);

#endif /* ARCHOS_NET_TCP_H */
