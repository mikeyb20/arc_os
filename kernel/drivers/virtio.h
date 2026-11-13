#ifndef ARCHOS_DRIVERS_VIRTIO_H
#define ARCHOS_DRIVERS_VIRTIO_H

#include <stdint.h>
#include "drivers/pci.h"

/* Legacy VirtIO PCI register offsets from BAR0 I/O base */
#define VIRTIO_REG_DEVICE_FEATURES  0x00  /* 32-bit, read */
#define VIRTIO_REG_GUEST_FEATURES   0x04  /* 32-bit, write */
#define VIRTIO_REG_QUEUE_ADDR       0x08  /* 32-bit, write (PFN) */
#define VIRTIO_REG_QUEUE_SIZE       0x0C  /* 16-bit, read */
#define VIRTIO_REG_QUEUE_SELECT     0x0E  /* 16-bit, write */
#define VIRTIO_REG_QUEUE_NOTIFY     0x10  /* 16-bit, write */
#define VIRTIO_REG_DEVICE_STATUS    0x12  /* 8-bit, read/write */
#define VIRTIO_REG_ISR_STATUS       0x13  /* 8-bit, read */
/* With MSI-X on, two vector registers follow and the device-specific config
 * moves from 0x14 to 0x18 (VirtioDevice.config_off) */
#define VIRTIO_REG_MSI_CONFIG_VEC   0x14  /* 16-bit: vector for config changes */
#define VIRTIO_REG_MSI_QUEUE_VEC    0x16  /* 16-bit: vector of the selected queue */
#define VIRTIO_MSI_NO_VECTOR        0xFFFF
#define VIRTIO_REG_CONFIG           0x14  /* device-specific config without MSI-X */
#define VIRTIO_REG_CONFIG_MSIX      0x18  /* ... and with MSI-X on */

/* ISR status bits (read clears; INTx only) */
#define VIRTIO_ISR_QUEUE  0x01
#define VIRTIO_ISR_CONFIG 0x02

/* Available-ring flag: the driver polls this queue, no interrupts please */
#define VRING_AVAIL_F_NO_INTERRUPT 0x01

/* VirtIO device status bits */
#define VIRTIO_STATUS_ACK           0x01
#define VIRTIO_STATUS_DRIVER        0x02
#define VIRTIO_STATUS_DRIVER_OK     0x04
#define VIRTIO_STATUS_FEATURES_OK   0x08
#define VIRTIO_STATUS_FAILED        0x80

/* Vring descriptor flags */
#define VRING_DESC_F_NEXT           0x01  /* Descriptor chains via next field */
#define VRING_DESC_F_WRITE          0x02  /* Device writes (vs reads) */

/* Maximum virtqueues per device */
#define VIRTIO_MAX_QUEUES  4

/* Invalid descriptor index sentinel */
#define VRING_DESC_NONE  0xFFFF

/* --- Vring structures (per VirtIO 1.0 spec, legacy layout) --- */

/* Descriptor: points to a data buffer. Chained via 'next' when F_NEXT is set.
 * F_WRITE marks device-writable buffers (device writes into them). */
typedef struct {
    uint64_t addr;    /* Physical address of buffer */
    uint32_t len;     /* Length of buffer */
    uint16_t flags;   /* VRING_DESC_F_* */
    uint16_t next;    /* Next descriptor in chain (if F_NEXT) */
} VringDesc;

/* Available ring: driver writes descriptor head indices here for the device
 * to consume. 'idx' is the next slot the driver will write to. */
typedef struct {
    uint16_t flags;
    uint16_t idx;     /* Next index to write */
    uint16_t ring[];  /* Descriptor indices */
} VringAvail;

/* Single element in the used ring, returned by the device after processing. */
typedef struct {
    uint32_t id;      /* Descriptor chain head index */
    uint32_t len;     /* Bytes written by device */
} VringUsedElem;

/* Used ring: device writes completed descriptor heads here for the driver
 * to collect. 'idx' is the next slot the device will write to. */
typedef struct {
    uint16_t flags;
    uint16_t idx;     /* Next index device will write */
    VringUsedElem ring[];
} VringUsed;

typedef struct {
    uint16_t size;          /* Queue size (power of 2) */
    VringDesc *desc;        /* Descriptor table (virtual) */
    VringAvail *avail;      /* Available ring (virtual) */
    VringUsed *used;        /* Used ring (virtual) */
    uint16_t free_head;     /* Head of free descriptor list */
    uint16_t num_free;      /* Number of free descriptors */
    uint16_t last_used_idx; /* Last consumed used ring index */
    uint64_t phys_addr;     /* Bus address of the vring (dma_alloc_coherent) */
    uint32_t num_pages;     /* Number of pages allocated */
} Virtqueue;

typedef struct {
    PciDevice *pci;
    uint16_t io_base;
    uint16_t config_off;    /* VIRTIO_REG_CONFIG, or VIRTIO_REG_CONFIG_MSIX with MSI-X on */
    uint8_t msix;           /* 1 when the device signals through MSI-X vectors */
    Virtqueue queues[VIRTIO_MAX_QUEUES];
    int num_queues;
} VirtioDevice;

/* Initialize a VirtIO device: reset, set ACK+DRIVER status.
 * Returns 0 on success. */
int virtio_init_device(VirtioDevice *vdev, PciDevice *pci);

/* Ask the PCI layer for up to want MSI-X vectors, else INTx (virtio-pci has
 * no plain MSI). Sets msix and config_off. Call before reading the device
 * config. Returns the number of interrupts, or -errno. */
int virtio_setup_irqs(VirtioDevice *vdev, unsigned want);

/* MSI-X: route config changes (virtio_set_config_vector) or the queue's used
 * buffers (virtio_set_queue_vector) to MSI-X entry vec, or nowhere
 * (VIRTIO_MSI_NO_VECTOR). Returns 0, or -1 if the device refused. */
int virtio_set_config_vector(VirtioDevice *vdev, uint16_t vec);
int virtio_set_queue_vector(VirtioDevice *vdev, int queue_index, uint16_t vec);

/* INTx: read (and so clear) the ISR status; VIRTIO_ISR_* bits. */
uint8_t virtio_isr_status(VirtioDevice *vdev);

/* Device-specific config at offset off (from where config_off says it is). */
uint8_t virtio_config_read8(VirtioDevice *vdev, uint16_t off);
uint32_t virtio_config_read32(VirtioDevice *vdev, uint16_t off);

/* Negotiate features: read device features, AND with supported, write back. */
void virtio_negotiate_features(VirtioDevice *vdev, uint32_t supported);

/* Initialize a virtqueue: allocate vring memory, configure with device.
 * Returns 0 on success. */
int virtio_init_queue(VirtioDevice *vdev, int queue_index);

/* Set DRIVER_OK status — device is live. */
void virtio_device_ready(VirtioDevice *vdev);

/* Allocate a descriptor from the free list. Returns index or VRING_DESC_NONE. */
uint16_t virtq_alloc_desc(Virtqueue *vq);

/* Free a descriptor chain back to the free list. */
void virtq_free_chain(Virtqueue *vq, uint16_t head);

/* Submit a descriptor chain: add head to avail ring and notify device. */
void virtio_submit(VirtioDevice *vdev, int queue_index, uint16_t head);

/* Check if device has placed entries in the used ring. */
int virtq_has_used(Virtqueue *vq);

/* Pop a used entry. Sets *len to bytes written. Returns descriptor head index. */
uint16_t virtq_pop_used(Virtqueue *vq, uint32_t *len);

#endif /* ARCHOS_DRIVERS_VIRTIO_H */
