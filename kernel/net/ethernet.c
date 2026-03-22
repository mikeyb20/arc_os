#include "net/ethernet.h"
#include "net/net_util.h"
#include "net/netif.h"
#include "net/arp.h"
#include "net/ipv4.h"
#include "lib/mem.h"
#include "lib/kprintf.h"

void eth_rx(struct NetIf *nif, const void *frame, uint32_t len) {
    if (len < ETH_HEADER_SIZE) return;

    const EthHeader *hdr = (const EthHeader *)frame;
    const uint8_t *payload = (const uint8_t *)frame + ETH_HEADER_SIZE;
    uint32_t payload_len = len - ETH_HEADER_SIZE;
    uint16_t type = ntohs(hdr->ethertype);

    switch (type) {
    case ETH_TYPE_ARP:
        arp_rx(nif, payload, payload_len);
        break;
    case ETH_TYPE_IPV4:
        ipv4_rx(nif, payload, payload_len);
        break;
    default:
        break; /* ignore unknown */
    }
}

int eth_send(struct NetIf *nif, const uint8_t dst[ETH_ALEN],
             uint16_t ethertype, const void *payload, uint32_t payload_len) {
    if (payload_len > ETH_MTU) return -1;

    uint8_t frame[ETH_FRAME_MAX];
    EthHeader *hdr = (EthHeader *)frame;

    memcpy(hdr->dst, dst, ETH_ALEN);
    memcpy(hdr->src, nif->mac, ETH_ALEN);
    hdr->ethertype = htons(ethertype);

    if (payload_len > 0)
        memcpy(frame + ETH_HEADER_SIZE, payload, payload_len);

    return nif->send(nif, frame, ETH_HEADER_SIZE + payload_len);
}
