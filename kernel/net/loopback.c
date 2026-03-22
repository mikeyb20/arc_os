#include "net/loopback.h"
#include "net/netif.h"
#include "net/ethernet.h"
#include "net/net_util.h"
#include "lib/mem.h"
#include "lib/kprintf.h"

static NetIf lo_nif;

static int lo_send(struct NetIf *nif, const void *frame, uint32_t len) {
    /* Loopback: feed frame directly back into the receive path */
    eth_rx(nif, frame, len);
    return 0;
}

void loopback_init(void) {
    memset(&lo_nif, 0, sizeof(lo_nif));
    lo_nif.ip_addr = IP4(127, 0, 0, 1);
    lo_nif.netmask = IP4(255, 0, 0, 0);
    lo_nif.gateway = 0;
    lo_nif.send = lo_send;
    netif_register(&lo_nif);
    kprintf("[NET] lo0 registered (127.0.0.1)\n");
}
