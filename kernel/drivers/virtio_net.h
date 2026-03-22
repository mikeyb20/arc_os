#ifndef ARCHOS_DRIVERS_VIRTIO_NET_H
#define ARCHOS_DRIVERS_VIRTIO_NET_H

#include <stdint.h>

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

/* Initialize VirtIO-net device. Returns 0 on success. */
int virtio_net_init(void);

/* Send an Ethernet frame (without virtio-net header — driver prepends it). */
int virtio_net_send(const void *data, uint32_t len);

/* Poll receive queue. Dispatches to net stack. Returns packets processed. */
int virtio_net_poll_rx(void);

/* Get the device MAC address. */
void virtio_net_get_mac(uint8_t mac[6]);

#endif /* ARCHOS_DRIVERS_VIRTIO_NET_H */
