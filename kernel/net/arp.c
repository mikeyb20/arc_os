#include "net/arp.h"
#include "net/net_util.h"
#include "net/netif.h"
#include "net/ethernet.h"
#include "lib/mem.h"
#include "lib/kprintf.h"

typedef struct {
    uint32_t ip;
    uint8_t  mac[ETH_ALEN];
    uint8_t  valid;
} ArpEntry;

static ArpEntry arp_cache[ARP_CACHE_SIZE];
static int arp_next_slot;  /* round-robin eviction */

void arp_init(void) {
    memset(arp_cache, 0, sizeof(arp_cache));
    arp_next_slot = 0;
}

void arp_insert(uint32_t ip, const uint8_t mac[ETH_ALEN]) {
    /* Update existing entry if present */
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            memcpy(arp_cache[i].mac, mac, ETH_ALEN);
            return;
        }
    }
    /* Insert into next slot (round-robin eviction) */
    ArpEntry *e = &arp_cache[arp_next_slot];
    e->ip = ip;
    memcpy(e->mac, mac, ETH_ALEN);
    e->valid = 1;
    arp_next_slot = (arp_next_slot + 1) % ARP_CACHE_SIZE;
}

const uint8_t *arp_lookup(uint32_t ip) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip)
            return arp_cache[i].mac;
    }
    return NULL;
}

void arp_rx(struct NetIf *nif, const void *data, uint32_t len) {
    if (len < ARP_PACKET_SIZE) return;

    const ArpPacket *pkt = (const ArpPacket *)data;
    if (ntohs(pkt->htype) != ARP_HTYPE_ETHER) return;
    if (ntohs(pkt->ptype) != ARP_PTYPE_IPV4) return;
    if (pkt->hlen != ETH_ALEN || pkt->plen != 4) return;

    /* Learn sender's MAC/IP (regardless of request vs reply) */
    arp_insert(pkt->spa, pkt->sha);

    /* If this is a request for our IP, send reply */
    if (ntohs(pkt->oper) == ARP_OP_REQUEST && pkt->tpa == nif->ip_addr) {
        kprintf("[ARP] Request for our IP, sending reply\n");

        ArpPacket reply;
        reply.htype = htons(ARP_HTYPE_ETHER);
        reply.ptype = htons(ARP_PTYPE_IPV4);
        reply.hlen = ETH_ALEN;
        reply.plen = 4;
        reply.oper = htons(ARP_OP_REPLY);
        memcpy(reply.sha, nif->mac, ETH_ALEN);
        reply.spa = nif->ip_addr;
        memcpy(reply.tha, pkt->sha, ETH_ALEN);
        reply.tpa = pkt->spa;

        eth_send(nif, pkt->sha, ETH_TYPE_ARP, &reply, ARP_PACKET_SIZE);
    }
}
