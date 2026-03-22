#ifndef ARCHOS_NET_ETHERNET_H
#define ARCHOS_NET_ETHERNET_H

#include <stdint.h>

#define ETH_ALEN        6
#define ETH_HEADER_SIZE 14
#define ETH_MTU         1500
#define ETH_FRAME_MAX   (ETH_HEADER_SIZE + ETH_MTU)

#define ETH_TYPE_ARP    0x0806
#define ETH_TYPE_IPV4   0x0800

typedef struct {
    uint8_t  dst[ETH_ALEN];
    uint8_t  src[ETH_ALEN];
    uint16_t ethertype;       /* network byte order */
} __attribute__((packed)) EthHeader;

/* Forward declare NetIf */
struct NetIf;

/* Process a received Ethernet frame (dispatches to ARP/IPv4). */
void eth_rx(struct NetIf *nif, const void *frame, uint32_t len);

/* Send an Ethernet frame. Builds header, appends payload, calls nif->send. */
int eth_send(struct NetIf *nif, const uint8_t dst[ETH_ALEN],
             uint16_t ethertype, const void *payload, uint32_t payload_len);

#endif /* ARCHOS_NET_ETHERNET_H */
