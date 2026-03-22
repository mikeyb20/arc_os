#include "net/ipv4.h"
#include "net/net_util.h"
#include "net/netif.h"
#include "net/ethernet.h"
#include "net/arp.h"
#include "net/icmp.h"
#include "net/udp.h"
#include "lib/mem.h"
#include "lib/kprintf.h"

static uint16_t ip_id_counter = 1;

void ipv4_rx(struct NetIf *nif, const void *data, uint32_t len) {
    if (len < IPV4_HEADER_SIZE) return;

    const Ipv4Header *hdr = (const Ipv4Header *)data;

    /* Check version */
    uint8_t version = hdr->ver_ihl >> 4;
    uint8_t ihl = hdr->ver_ihl & 0x0F;
    if (version != 4 || ihl < 5) return;

    uint32_t hdr_len = (uint32_t)ihl * 4;
    if (len < hdr_len) return;

    /* Verify header checksum */
    if (net_checksum(data, hdr_len) != 0) return;

    /* Check destination (us or broadcast) */
    uint32_t bcast = nif->ip_addr | ~nif->netmask;
    if (hdr->dst_ip != nif->ip_addr && hdr->dst_ip != 0xFFFFFFFF && hdr->dst_ip != bcast)
        return;

    const uint8_t *payload = (const uint8_t *)data + hdr_len;
    uint32_t payload_len = ntohs(hdr->total_len) - hdr_len;
    if (hdr_len + payload_len > len) return;

    switch (hdr->protocol) {
    case IPV4_PROTO_ICMP:
        icmp_rx(nif, hdr->src_ip, payload, payload_len);
        break;
    case IPV4_PROTO_UDP:
        udp_rx(nif, hdr->src_ip, hdr->dst_ip, payload, payload_len);
        break;
    default:
        break;
    }
}

int ipv4_send(struct NetIf *nif, uint32_t dst_ip, uint8_t protocol,
              const void *payload, uint32_t payload_len) {
    uint32_t total_len = IPV4_HEADER_SIZE + payload_len;
    if (total_len > ETH_MTU) return -1;

    uint8_t pkt[ETH_MTU];
    Ipv4Header *hdr = (Ipv4Header *)pkt;

    hdr->ver_ihl = 0x45;  /* IPv4, IHL=5 */
    hdr->tos = 0;
    hdr->total_len = htons((uint16_t)total_len);
    hdr->id = htons(ip_id_counter++);
    hdr->flags_frag = 0;
    hdr->ttl = IPV4_DEFAULT_TTL;
    hdr->protocol = protocol;
    hdr->checksum = 0;
    hdr->src_ip = nif->ip_addr;
    hdr->dst_ip = dst_ip;

    /* Compute header checksum */
    hdr->checksum = htons(net_checksum(hdr, IPV4_HEADER_SIZE));

    /* Append payload */
    if (payload_len > 0)
        memcpy(pkt + IPV4_HEADER_SIZE, payload, payload_len);

    /* Determine next-hop: on-link or gateway */
    uint32_t next_hop = dst_ip;
    if ((dst_ip & nif->netmask) != (nif->ip_addr & nif->netmask))
        next_hop = nif->gateway;

    /* ARP lookup for next-hop MAC */
    const uint8_t *dst_mac = arp_lookup(next_hop);
    if (!dst_mac) {
        /* No ARP entry — drop (QEMU ARPs us first, so this is rare) */
        kprintf("[IPv4] No ARP entry for next-hop, dropping\n");
        return -1;
    }

    return eth_send(nif, dst_mac, ETH_TYPE_IPV4, pkt, total_len);
}
