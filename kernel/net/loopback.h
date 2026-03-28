#ifndef ARCHOS_NET_LOOPBACK_H
#define ARCHOS_NET_LOOPBACK_H

/* Initialize the loopback interface (127.0.0.1).
 * Registers as a NetIf; packets sent to it are looped back to the
 * network stack's RX path. Returns 0 on success. */
int loopback_init(void);

#endif /* ARCHOS_NET_LOOPBACK_H */
