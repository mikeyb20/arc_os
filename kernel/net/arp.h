#ifndef ARCHOS_NET_ARP_H
#define ARCHOS_NET_ARP_H

#include <stdint.h>
#include "net/ethernet.h"

#define ARP_HTYPE_ETHER  1
#define ARP_PTYPE_IPV4   0x0800
#define ARP_OP_REQUEST   1
#define ARP_OP_REPLY     2
#define ARP_PACKET_SIZE  28
#define ARP_CACHE_SIZE   16

typedef struct {
    uint16_t htype;
    uint16_t ptype;
    uint8_t  hlen;
    uint8_t  plen;
    uint16_t oper;
    uint8_t  sha[ETH_ALEN];
    uint32_t spa;
    uint8_t  tha[ETH_ALEN];
    uint32_t tpa;
} __attribute__((packed)) ArpPacket;

struct NetIf;

/* Initialize ARP subsystem. */
void arp_init(void);

/* Process incoming ARP (called from eth_rx). */
void arp_rx(struct NetIf *nif, const void *data, uint32_t len);

/* Look up MAC for an IP. Returns pointer to 6-byte MAC, or NULL. */
const uint8_t *arp_lookup(uint32_t ip);

/* Insert a static ARP entry. */
void arp_insert(uint32_t ip, const uint8_t mac[ETH_ALEN]);

#endif /* ARCHOS_NET_ARP_H */
