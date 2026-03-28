#include "net/tcp.h"
#include "net/ipv4.h"
#include "net/net_util.h"
#include "net/netif.h"
#include "net/socket.h"
#include "lib/mem.h"
#include "lib/kprintf.h"

/* TCP pseudo-header for checksum computation */
typedef struct {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint8_t  zero;
    uint8_t  protocol;
    uint16_t tcp_len;
} __attribute__((packed)) TcpPseudoHeader;

/* Compute TCP checksum over pseudo-header + segment */
static uint16_t tcp_checksum(uint32_t src_ip, uint32_t dst_ip,
                              const void *segment, uint32_t seg_len) {
    TcpPseudoHeader pseudo;
    pseudo.src_ip = src_ip;
    pseudo.dst_ip = dst_ip;
    pseudo.zero = 0;
    pseudo.protocol = IPV4_PROTO_TCP;
    pseudo.tcp_len = htons((uint16_t)seg_len);

    /* Sum pseudo-header */
    uint32_t sum = 0;
    const uint16_t *p = (const uint16_t *)&pseudo;
    for (uint32_t i = 0; i < sizeof(pseudo) / 2; i++) {
        sum += p[i];
    }

    /* Sum TCP segment */
    const uint16_t *seg = (const uint16_t *)segment;
    uint32_t words = seg_len / 2;
    for (uint32_t i = 0; i < words; i++) {
        sum += seg[i];
    }
    if (seg_len & 1) {
        uint8_t last = ((const uint8_t *)segment)[seg_len - 1];
        sum += (uint16_t)last;
    }

    /* Fold carries */
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return (uint16_t)~sum;
}

void tcp_rx(struct NetIf *nif, uint32_t src_ip, uint32_t dst_ip,
            const void *data, uint32_t len) {
    (void)nif;
    if (len < TCP_HEADER_SIZE) return;

    const TcpHeader *hdr = (const TcpHeader *)data;
    uint8_t data_off = (hdr->data_off >> 4) * 4;
    if (data_off < TCP_HEADER_SIZE || data_off > len) return;

    /* Verify checksum */
    uint16_t cksum = tcp_checksum(src_ip, dst_ip, data, len);
    if (cksum != 0) return;

    /* Deliver to socket layer for state machine processing */
    socket_tcp_deliver(src_ip, hdr->src_port,
                       dst_ip, hdr->dst_port,
                       data, len);
}

int tcp_send(struct NetIf *nif, uint32_t src_ip, uint16_t src_port,
             uint32_t dst_ip, uint16_t dst_port,
             uint32_t seq, uint32_t ack_num, uint8_t flags,
             const void *payload, uint32_t payload_len) {
    uint32_t total = TCP_HEADER_SIZE + payload_len;
    if (total > 1480) return -1;

    uint8_t buf[1480];
    TcpHeader *hdr = (TcpHeader *)buf;

    hdr->src_port = src_port;
    hdr->dst_port = dst_port;
    hdr->seq = htonl(seq);
    hdr->ack = htonl(ack_num);
    hdr->data_off = (TCP_HEADER_SIZE / 4) << 4;
    hdr->flags = flags;
    hdr->window = htons(SOCK_RXBUF_SIZE);
    hdr->checksum = 0;
    hdr->urgent = 0;

    if (payload_len > 0) {
        memcpy(buf + TCP_HEADER_SIZE, payload, payload_len);
    }

    /* Compute checksum over pseudo-header + segment */
    hdr->checksum = tcp_checksum(src_ip, dst_ip, buf, total);

    (void)src_ip;
    return ipv4_send(nif, dst_ip, IPV4_PROTO_TCP, buf, total);
}
