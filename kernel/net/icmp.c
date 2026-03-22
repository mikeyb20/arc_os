#include "net/icmp.h"
#include "net/net_util.h"
#include "net/ipv4.h"
#include "net/netif.h"
#include "lib/mem.h"
#include "lib/kprintf.h"

void icmp_rx(struct NetIf *nif, uint32_t src_ip, const void *data, uint32_t len) {
    if (len < ICMP_HEADER_SIZE) return;

    const IcmpHeader *hdr = (const IcmpHeader *)data;

    /* Verify ICMP checksum (covers header + data) */
    if (net_checksum(data, len) != 0) return;

    if (hdr->type == ICMP_TYPE_ECHO_REQUEST && hdr->code == 0) {
        kprintf("[ICMP] Echo request from %d.%d.%d.%d (id=%u seq=%u)\n",
                src_ip & 0xFF, (src_ip >> 8) & 0xFF,
                (src_ip >> 16) & 0xFF, (src_ip >> 24) & 0xFF,
                ntohs(hdr->id), ntohs(hdr->sequence));

        /* Build echo reply: same id, sequence, and payload data */
        uint8_t reply[ETH_MTU];
        IcmpHeader *rhdr = (IcmpHeader *)reply;
        rhdr->type = ICMP_TYPE_ECHO_REPLY;
        rhdr->code = 0;
        rhdr->checksum = 0;
        rhdr->id = hdr->id;
        rhdr->sequence = hdr->sequence;

        /* Copy payload (everything after ICMP header) */
        uint32_t payload_len = len - ICMP_HEADER_SIZE;
        if (payload_len > 0)
            memcpy(reply + ICMP_HEADER_SIZE, (const uint8_t *)data + ICMP_HEADER_SIZE, payload_len);

        /* Compute ICMP checksum over entire reply */
        rhdr->checksum = htons(net_checksum(reply, ICMP_HEADER_SIZE + payload_len));

        ipv4_send(nif, src_ip, IPV4_PROTO_ICMP, reply, ICMP_HEADER_SIZE + payload_len);
        kprintf("[ICMP] Echo reply sent\n");
    }
}
