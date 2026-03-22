#include "net/net.h"
#include "net/netif.h"
#include "net/arp.h"
#include "net/net_util.h"
#include "net/socket.h"
#include "net/loopback.h"
#include "lib/kprintf.h"

void net_init(void) {
    arp_init();
    socket_init();

    NetIf *nif = netif_get_default();
    if (!nif) {
        kprintf("[NET] No network interface available\n");
    } else {
        /* QEMU user-mode networking defaults */
        nif->ip_addr = IP4(10, 0, 2, 15);
        nif->netmask = IP4(255, 255, 255, 0);
        nif->gateway = IP4(10, 0, 2, 2);
        kprintf("[NET] IP configured: 10.0.2.15/24 gw 10.0.2.2\n");
    }

    loopback_init();
    kprintf("[NET] Network stack ready\n");
}
