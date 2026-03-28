#include "net/loopback.h"
#include "net/netif.h"
#include <stddef.h>
#include "net/ethernet.h"
#include "net/ipv4.h"
#include "net/net_util.h"
#include "lib/kprintf.h"

static NetIf lo_nif;

/* Loopback send: skip Ethernet framing, feed directly to ipv4_rx.
 * The 'frame' is an Ethernet frame (header + IP packet). */
static int loopback_send(NetIf *nif, const void *frame, uint32_t len) {
    if (len < ETH_HEADER_SIZE) return -1;

    /* Skip Ethernet header, deliver IP payload directly */
    const uint8_t *payload = (const uint8_t *)frame + ETH_HEADER_SIZE;
    uint32_t payload_len = len - ETH_HEADER_SIZE;

    ipv4_rx(nif, payload, payload_len);
    return 0;
}

int loopback_init(void) {
    /* Loopback has no real MAC — use all zeros */
    for (int i = 0; i < ETH_ALEN; i++) lo_nif.mac[i] = 0;

    lo_nif.ip_addr = IP4(127, 0, 0, 1);
    lo_nif.netmask = IP4(255, 0, 0, 0);
    lo_nif.gateway = 0;
    lo_nif.send = loopback_send;
    lo_nif.private_data = NULL;

    int slot = netif_register(&lo_nif);
    if (slot < 0) {
        kprintf("[LOOPBACK] Failed to register interface\n");
        return -1;
    }

    kprintf("[LOOPBACK] lo0 registered (127.0.0.1)\n");
    return 0;
}
