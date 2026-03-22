#include "net/udp.h"
#include "net/ipv4.h"
#include "net/net_util.h"
#include "net/netif.h"
#include "net/socket.h"
#include "lib/mem.h"
#include "lib/kprintf.h"

/* Pseudo-header for checksum computation (RFC 768) */
typedef struct {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint8_t  zero;
    uint8_t  protocol;
    uint16_t udp_length;
} __attribute__((packed)) UdpPseudoHeader;

/* Compute UDP checksum over pseudo-header + UDP header + payload.
 * All data is copied into a contiguous stack buffer for net_checksum. */
static uint16_t udp_checksum(uint32_t src_ip, uint32_t dst_ip,
                              const UdpHeader *hdr,
                              const void *payload, uint32_t payload_len) {
    uint8_t buf[12 + UDP_HEADER_SIZE + ETH_MTU];
    uint32_t total = 12 + UDP_HEADER_SIZE + payload_len;
    if (total > sizeof(buf)) return 0;

    UdpPseudoHeader *pseudo = (UdpPseudoHeader *)buf;
    pseudo->src_ip = src_ip;
    pseudo->dst_ip = dst_ip;
    pseudo->zero = 0;
    pseudo->protocol = IPV4_PROTO_UDP;
    pseudo->udp_length = hdr->length;

    memcpy(buf + 12, hdr, UDP_HEADER_SIZE);
    if (payload_len > 0)
        memcpy(buf + 12 + UDP_HEADER_SIZE, payload, payload_len);

    uint16_t cksum = net_checksum(buf, total);
    /* UDP: checksum 0 means "no checksum"; if computed 0, use 0xFFFF */
    return (cksum == 0) ? 0xFFFF : cksum;
}

void udp_rx(struct NetIf *nif, uint32_t src_ip, uint32_t dst_ip,
            const void *data, uint32_t len) {
    (void)nif;
    if (len < UDP_HEADER_SIZE) return;

    const UdpHeader *hdr = (const UdpHeader *)data;
    uint16_t udp_len = ntohs(hdr->length);
    if (udp_len < UDP_HEADER_SIZE || udp_len > len) return;

    const void *payload = (const uint8_t *)data + UDP_HEADER_SIZE;
    uint32_t payload_len = udp_len - UDP_HEADER_SIZE;

    /* Deliver to matching socket (silently drop if none) */
    socket_udp_deliver(src_ip, hdr->src_port, dst_ip, hdr->dst_port,
                       payload, payload_len);
}

int udp_send(struct NetIf *nif, uint32_t src_ip, uint32_t dst_ip,
             uint16_t src_port, uint16_t dst_port,
             const void *payload, uint32_t payload_len) {
    uint32_t udp_total = UDP_HEADER_SIZE + payload_len;
    if (udp_total > ETH_MTU - IPV4_HEADER_SIZE) return -1;

    uint8_t pkt[ETH_MTU];
    UdpHeader *hdr = (UdpHeader *)pkt;
    hdr->src_port = src_port;
    hdr->dst_port = dst_port;
    hdr->length = htons((uint16_t)udp_total);
    hdr->checksum = 0;

    if (payload_len > 0)
        memcpy(pkt + UDP_HEADER_SIZE, payload, payload_len);

    hdr->checksum = htons(udp_checksum(src_ip, dst_ip, hdr, payload, payload_len));

    return ipv4_send(nif, dst_ip, IPV4_PROTO_UDP, pkt, udp_total);
}
