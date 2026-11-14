/* arc_os — virtio-blk driver (legacy virtio-PCI)
 *
 * Owns: one virtio block device (the first one found), its request queue and
 * its block-device registration.
 * Invariants: requests are synchronous: one in flight, polled to completion
 * (M1-10 makes them asynchronous with interrupt completion). The queue's
 * MSI-X vector is set up and counted now; its handler has nothing to finish
 * yet.
 * Locks: none of its own; callers are serialized by the big kernel lock
 * (syscalls) or run at boot. The interrupt handlers touch no driver state.
 * Cost: one request = two coherent DMA allocations + one copy (M1-10 drops
 * both). */

#include "drivers/virtio_blk.h"
#include "drivers/blkdev.h"
#include "drivers/virtio.h"
#include "drivers/pci.h"
#include "drivers/irq.h"
#include "drivers/dma.h"
#include "arch/hal.h"
#include "mm/pmm.h"
#include "lib/mem.h"
#include "lib/kprintf.h"
#include "uapi/errno.h"

/* Sector size in bytes */
#define SECTOR_SIZE 512

/* Polling timeout: max spins before giving up */
#define POLL_TIMEOUT 10000000

static VirtioDevice blk_vdev;
static uint64_t blk_capacity;  /* in 512-byte sectors */
static int blk_initialized;

static void virtio_blk_register_blkdev(void);

/* Queue vector (MSI-X): completions are still polled, so only count it. */
static int blk_queue_irq(void *arg) {
    (void)arg;
    return IRQ_HANDLED;
}

/* Config-change vector (MSI-X): capacity changes are not handled yet. */
static int blk_config_irq(void *arg) {
    (void)arg;
    return IRQ_HANDLED;
}

/* INTx (shared line): reading the ISR status acknowledges the device. */
static int blk_intx_irq(void *arg) {
    return virtio_isr_status(arg) != 0 ? IRQ_HANDLED : IRQ_NONE;
}

/* MSI-X entry 0 = config changes, entry 1 = request queue (with a single
 * vector the queue takes entry 0); INTx = one shared handler. */
static int blk_setup_irqs(VirtioDevice *v) {
    Device *dev = &v->pci->dev;
    int n = virtio_setup_irqs(v, 2);
    if (n < 0) return n;
    if (!v->msix) return irq_request(dev, 0, blk_intx_irq, v, IRQF_SHARED, "virtio-blk");
    uint16_t qvec = n >= 2 ? 1 : 0;
    int rc = irq_request(dev, qvec, blk_queue_irq, v, 0, "virtio-blk-queue");
    if (rc == 0 && n >= 2) rc = irq_request(dev, 0, blk_config_irq, v, 0, "virtio-blk-config");
    if (rc != 0) return rc;
    if (virtio_set_queue_vector(v, 0, qvec) != 0 ||
        virtio_set_config_vector(v, n >= 2 ? 0 : VIRTIO_MSI_NO_VECTOR) != 0) {
        kprintf("[VIRTIO-BLK] device refused the MSI-X vectors\n");
        return -EIO;
    }
    return 0;
}

static int virtio_blk_probe(PciDevice *pci, const PciDeviceId *id) {
    (void)id;
    if (blk_initialized) return -EBUSY;   /* one virtio disk for now */

    if (virtio_init_device(&blk_vdev, pci) != 0) return -ENODEV;

    /* Negotiate features — we don't need any optional features for basic I/O */
    virtio_negotiate_features(&blk_vdev, 0);

    /* Initialize request queue (queue 0) */
    if (virtio_init_queue(&blk_vdev, 0) != 0) return -ENOMEM;

    int rc = blk_setup_irqs(&blk_vdev);
    if (rc != 0) return rc;

    /* Capacity: 64 bits at the start of the device config (moved by MSI-X) */
    uint32_t cap_lo = virtio_config_read32(&blk_vdev, 0);
    uint32_t cap_hi = virtio_config_read32(&blk_vdev, 4);
    blk_capacity = ((uint64_t)cap_hi << 32) | cap_lo;

    kprintf("[VIRTIO-BLK] Capacity: %lu sectors (%lu MB), %s\n", blk_capacity,
            (blk_capacity * SECTOR_SIZE) / (1024 * 1024), blk_vdev.msix ? "MSI-X" : "INTx");

    /* Mark device ready */
    virtio_device_ready(&blk_vdev);

    blk_initialized = 1;
    pci->dev.driver_data = &blk_vdev;

    /* Register as a generic block device */
    virtio_blk_register_blkdev();
    return 0;
}

/* ADR-0017: the hooks exist; power management of the disk comes later. */
static int virtio_blk_suspend(Device *dev, int how) {
    (void)how;
    kprintf("[VIRTIO-BLK] %s: suspend not supported yet\n", dev->name);
    return -EOPNOTSUPP;
}

static int virtio_blk_resume(Device *dev, int how) {
    (void)how;
    kprintf("[VIRTIO-BLK] %s: resume not supported yet\n", dev->name);
    return -EOPNOTSUPP;
}

static const PciDeviceId virtio_blk_ids[] = {
    PCI_DEVICE(VIRTIO_BLK_VENDOR_ID, VIRTIO_BLK_DEVICE_ID),
    {0},
};

static PciDriver virtio_blk_driver = {
    .driver = {.name = "virtio-blk", .suspend = virtio_blk_suspend, .resume = virtio_blk_resume},
    .ids = virtio_blk_ids,
    .probe = virtio_blk_probe,
};

int virtio_blk_register(void) {
    return pci_register_driver(&virtio_blk_driver);
}

/* Build a 3-descriptor chain for a VirtIO-blk request.
 * data_flags controls the data descriptor direction (WRITE|NEXT for reads, NEXT for writes).
 * Returns the head descriptor index, or VRING_DESC_NONE on failure. */
static uint16_t blk_build_chain(Virtqueue *vq, uint64_t req_phys,
                                 uint64_t data_phys, uint32_t data_bytes,
                                 uint64_t status_phys, uint16_t data_flags) {
    uint16_t desc_hdr  = virtq_alloc_desc(vq);
    uint16_t desc_data = virtq_alloc_desc(vq);
    uint16_t desc_stat = virtq_alloc_desc(vq);
    if (desc_hdr == VRING_DESC_NONE || desc_data == VRING_DESC_NONE || desc_stat == VRING_DESC_NONE) {
        uint16_t descs[] = { desc_hdr, desc_data, desc_stat };
        for (int i = 0; i < 3; i++) {
            if (descs[i] != VRING_DESC_NONE) virtq_free_chain(vq, descs[i]);
        }
        return VRING_DESC_NONE;
    }

    /* Descriptor 0: request header (device-readable) */
    vq->desc[desc_hdr].addr  = req_phys;
    vq->desc[desc_hdr].len   = sizeof(VirtioBlkReqHeader);
    vq->desc[desc_hdr].flags = VRING_DESC_F_NEXT;
    vq->desc[desc_hdr].next  = desc_data;

    /* Descriptor 1: data buffer */
    vq->desc[desc_data].addr  = data_phys;
    vq->desc[desc_data].len   = data_bytes;
    vq->desc[desc_data].flags = data_flags;
    vq->desc[desc_data].next  = desc_stat;

    /* Descriptor 2: status byte (device-writable) */
    vq->desc[desc_stat].addr  = status_phys;
    vq->desc[desc_stat].len   = 1;
    vq->desc[desc_stat].flags = VRING_DESC_F_WRITE;
    vq->desc[desc_stat].next  = VRING_DESC_NONE;

    return desc_hdr;
}

/* Internal transfer helper for both reads and writes. */
static int blk_transfer(uint64_t sector, uint32_t count, void *buf, int is_write) {
    if (!blk_initialized) return -1;
    if (count == 0) return 0;
    if (sector + count > blk_capacity) return -1;

    const char *op = is_write ? "Write" : "Read";
    Virtqueue *vq = &blk_vdev.queues[0];
    Device *dev = &blk_vdev.pci->dev;
    uint32_t data_bytes = count * SECTOR_SIZE;

    /* Request page (header + status) and data buffer: coherent DMA memory */
    uint64_t req_phys, data_phys;
    uint8_t *req = dma_alloc_coherent(dev, PAGE_SIZE, &req_phys, 0);
    if (req == NULL) {
        kprintf("[VIRTIO-BLK] Failed to alloc request page\n");
        return -1;
    }
    uint8_t *data = dma_alloc_coherent(dev, data_bytes, &data_phys, 0);
    if (data == NULL) {
        kprintf("[VIRTIO-BLK] Failed to alloc %u data bytes\n", data_bytes);
        dma_free_coherent(dev, PAGE_SIZE, req, req_phys);
        return -1;
    }

    /* Set up request header */
    VirtioBlkReqHeader *hdr = (VirtioBlkReqHeader *)req;
    hdr->type = is_write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
    hdr->reserved = 0;
    hdr->sector = sector;

    /* For writes, copy caller data into DMA pages before submit */
    if (is_write) {
        memcpy(data, buf, data_bytes);
    }

    /* Status byte at end of request page */
    uint8_t *status_ptr = req + sizeof(VirtioBlkReqHeader);
    *status_ptr = VIRTIO_BLK_STATUS_INIT;
    uint64_t status_phys = req_phys + sizeof(VirtioBlkReqHeader);

    /* Build 3-descriptor chain: header -> data -> status */
    uint16_t data_flags = is_write ? VRING_DESC_F_NEXT
                                   : (VRING_DESC_F_WRITE | VRING_DESC_F_NEXT);
    uint16_t desc_head = blk_build_chain(vq, req_phys, data_phys, data_bytes,
                                          status_phys, data_flags);
    if (desc_head == VRING_DESC_NONE) {
        kprintf("[VIRTIO-BLK] No free descriptors\n");
        dma_free_coherent(dev, data_bytes, data, data_phys);
        dma_free_coherent(dev, PAGE_SIZE, req, req_phys);
        return -1;
    }

    /* Submit and spin-poll for completion */
    virtio_submit(&blk_vdev, 0, desc_head);

    int timeout = POLL_TIMEOUT;
    while (!virtq_has_used(vq) && --timeout > 0) {
        __asm__ volatile ("pause");
    }

    if (timeout == 0) {
        kprintf("[VIRTIO-BLK] %s timeout (sector %lu, count %u)\n", op, sector, count);
        virtq_free_chain(vq, desc_head);
        dma_free_coherent(dev, data_bytes, data, data_phys);
        dma_free_coherent(dev, PAGE_SIZE, req, req_phys);
        return -1;
    }

    /* Pop used entry */
    uint32_t used_len;
    virtq_pop_used(vq, &used_len);
    virtq_free_chain(vq, desc_head);

    /* Check status */
    int ret = -1;
    if (*status_ptr == VIRTIO_BLK_S_OK) {
        /* For reads, copy data to caller's buffer */
        if (!is_write) {
            memcpy(buf, data, data_bytes);
        }
        ret = 0;
    } else {
        kprintf("[VIRTIO-BLK] %s failed, status=%d\n", op, *status_ptr);
    }

    /* Free buffers */
    dma_free_coherent(dev, data_bytes, data, data_phys);
    dma_free_coherent(dev, PAGE_SIZE, req, req_phys);

    return ret;
}

int virtio_blk_read(uint64_t sector, uint32_t count, void *buf) {
    return blk_transfer(sector, count, buf, 0);
}

int virtio_blk_write(uint64_t sector, uint32_t count, const void *buf) {
    return blk_transfer(sector, count, (void *)buf, 1);
}

uint64_t virtio_blk_capacity(void) {
    return blk_capacity;
}

/* --- Block device registration --- */

static int virtio_blk_dev_read(BlockDevice *dev, uint64_t sector, uint32_t count, void *buf) {
    (void)dev;
    return virtio_blk_read(sector, count, buf);
}

static int virtio_blk_dev_write(BlockDevice *dev, uint64_t sector, uint32_t count, const void *buf) {
    (void)dev;
    return virtio_blk_write(sector, count, buf);
}

static uint64_t virtio_blk_dev_capacity(BlockDevice *dev) {
    (void)dev;
    return virtio_blk_capacity();
}

static BlockDevice virtio_blk_blockdev;

static void virtio_blk_register_blkdev(void) {
    virtio_blk_blockdev.read = virtio_blk_dev_read;
    virtio_blk_blockdev.write = virtio_blk_dev_write;
    virtio_blk_blockdev.capacity = virtio_blk_dev_capacity;
    blkdev_register(&virtio_blk_blockdev);
}
