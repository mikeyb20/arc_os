#ifndef ARCHOS_DRIVERS_VIRTIO_NET_H
#define ARCHOS_DRIVERS_VIRTIO_NET_H

#include <stdint.h>
#include "drivers/bus.h"

#define VIRTIO_NET_VENDOR_ID   0x1AF4
#define VIRTIO_NET_DEVICE_ID   0x1000

#define VIRTIO_NET_F_MAC       (1 << 5)

/* VirtIO-net header (legacy, 10 bytes — no MRG_RXBUF) */
typedef struct {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
} __attribute__((packed)) VirtioNetHeader;

#define VIRTIO_NET_HDR_SIZE sizeof(VirtioNetHeader)

/* Register the virtio-net PCI driver; it binds to the first virtio-net
 * device and registers it as the default network interface. Returns how many
 * it bound. */
int virtio_net_register(void);

/* Send an Ethernet frame (without virtio-net header — driver prepends it). */
int virtio_net_send(const void *data, uint32_t len);

/* Poll receive queue. Dispatches to net stack. Returns packets processed.
 * Caller holds the big kernel lock (the network stack's lock until M1-18). */
int virtio_net_poll_rx(void);

/* Times the RX worker woke up to receive. */
uint64_t virtio_net_rx_wakeups(void);

/* Receive interrupts so far (each wakes the RX worker). */
uint64_t virtio_net_rx_kicks(void);

/* The bound device and the interrupt index its RX queue uses on MSI-X (for
 * irq_set_affinity; the device self-test moves it), or -1 (no device, or
 * INTx). */
int virtio_net_rx_irq(Device **dev);

/* Get the device MAC address. */
void virtio_net_get_mac(uint8_t mac[6]);

#endif /* ARCHOS_DRIVERS_VIRTIO_NET_H */
