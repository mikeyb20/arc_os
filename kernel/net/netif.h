#ifndef ARCHOS_NET_NETIF_H
#define ARCHOS_NET_NETIF_H

#include <stdint.h>
#include "net/ethernet.h"

#define NETIF_MAX 4

typedef struct NetIf {
    uint8_t  mac[ETH_ALEN];
    uint32_t ip_addr;     /* network byte order */
    uint32_t netmask;     /* network byte order */
    uint32_t gateway;     /* network byte order */
    int (*send)(struct NetIf *nif, const void *frame, uint32_t len);
    void *private_data;
} NetIf;

/* Register a network interface. Returns slot index or -1. */
int netif_register(NetIf *nif);

/* Get the default (first) network interface. Returns NULL if none. */
NetIf *netif_get_default(void);

#endif /* ARCHOS_NET_NETIF_H */
