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
