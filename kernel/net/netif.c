#include "net/netif.h"
#include <stddef.h>

static NetIf *interfaces[NETIF_MAX];
static int netif_count;

int netif_register(NetIf *nif) {
    if (netif_count >= NETIF_MAX) return -1;
    interfaces[netif_count] = nif;
    return netif_count++;
}

NetIf *netif_get_default(void) {
    return (netif_count > 0) ? interfaces[0] : NULL;
}

NetIf *netif_find_by_ip(uint32_t dst_ip) {
    for (int i = 0; i < netif_count; i++) {
        if ((dst_ip & interfaces[i]->netmask) ==
            (interfaces[i]->ip_addr & interfaces[i]->netmask))
            return interfaces[i];
    }
    return NULL;
}
