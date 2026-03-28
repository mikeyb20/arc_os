#include "net/udp.h"
#include "net/ipv4.h"
#include "net/net_util.h"
#include "net/socket.h"
#include "lib/mem.h"
#include "lib/kprintf.h"

void udp_rx(struct NetIf *nif, uint32_t src_ip, uint32_t dst_ip,
            const void *data, uint32_t len) {
    (void)nif;
    if (len < UDP_HEADER_SIZE) return;

    const UdpHeader *hdr = (const UdpHeader *)data;
    uint16_t udp_len = ntohs(hdr->length);
    if (udp_len < UDP_HEADER_SIZE || udp_len > len) return;

    const uint8_t *payload = (const uint8_t *)data + UDP_HEADER_SIZE;
    uint32_t payload_len = udp_len - UDP_HEADER_SIZE;

    /* Deliver to matching socket */
    socket_udp_deliver(src_ip, hdr->src_port,
                       dst_ip, hdr->dst_port,
                       payload, payload_len);
}

int udp_send(struct NetIf *nif, uint32_t src_ip, uint16_t src_port,
             uint32_t dst_ip, uint16_t dst_port,
             const void *payload, uint32_t payload_len) {
    uint32_t total = UDP_HEADER_SIZE + payload_len;
    if (total > 1480) return -1;  /* IPv4 payload max */

    uint8_t buf[1480];
    UdpHeader *hdr = (UdpHeader *)buf;

    hdr->src_port = src_port;
    hdr->dst_port = dst_port;
    hdr->length = htons((uint16_t)total);
    hdr->checksum = 0;  /* UDP checksum optional for IPv4 */

    if (payload_len > 0) {
        memcpy(buf + UDP_HEADER_SIZE, payload, payload_len);
    }

    (void)src_ip;  /* Source IP set by ipv4_send from nif */
    return ipv4_send(nif, dst_ip, IPV4_PROTO_UDP, buf, total);
}
