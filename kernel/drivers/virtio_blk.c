/* arc_os — virtio-blk driver (legacy virtio-PCI) on the block layer (M1-10)
 *
 * Owns: per bound device, its request queue (vring 0), a coherent page of
 * per-tag request headers and status bytes, and the map from descriptor chain
 * head to request.
 * Invariants: every chain is header, the request's segments (dma_map_page),
 * status; by_head[head] is set exactly while the device owns that chain. A
 * reset device is dead: it never sees another request.
 * Locks: none of its own. Every BlockOps call runs under the block queue lock
 * (IRQ-safe), which also serializes the vring; the interrupt handlers only
 * call blk_complete.
 * Cost: one request = 2 + segments descriptors, no allocation and no copy;
 * one doorbell per batch. */

#include "drivers/virtio_blk.h"
#include "drivers/blkdev.h"
#include "drivers/virtio.h"
#include "drivers/pci.h"
#include "drivers/irq.h"
#include "drivers/dma.h"
#include "mm/kmalloc.h"
#include "mm/page.h"
#include "lib/kprintf.h"
#include "uapi/errno.h"

#define VBLK_MAX_TRANSFER (1024 * 1024)

/* Per-tag DMA area: header the device reads, status byte it writes */
typedef struct {
    VirtioBlkReqHeader hdr;
    uint8_t status;
    uint8_t pad[15];
} VblkSlot;

_Static_assert(sizeof(VblkSlot) == 32, "virtio-blk slot is 32 bytes");

typedef struct {
    VirtioDevice vdev;
    BlockDevice disk;
    VblkSlot *slots;
    uint64_t slots_bus;
    BlkRequest **by_head;   /* vring size entries */
} VirtioBlk;

static int dma_dir(const BlkRequest *rq) {
    return rq->op == BLK_OP_READ ? DMA_FROM_DEVICE : DMA_TO_DEVICE;
}

/* Unmap the data descriptors of the chain at head and free it. The chain
 * is complete (ends with the status descriptor) or, when building it failed,
 * ends with the last data descriptor. */
static void release_chain(VirtioBlk *vb, uint16_t head, const BlkRequest *rq, int complete) {
    Virtqueue *vq = &vb->vdev.queues[0];
    Device *dev = &vb->vdev.pci->dev;
    for (uint16_t d = vq->desc[head].next; d != VRING_DESC_NONE; d = vq->desc[d].next) {
        if (complete && vq->desc[d].next == VRING_DESC_NONE) break;   /* the status byte */
        dma_unmap(dev, vq->desc[d].addr, vq->desc[d].len, dma_dir(rq));
    }
    virtq_free_chain(vq, head);
}

static uint16_t add_desc(Virtqueue *vq, uint16_t prev, uint64_t addr, uint32_t len, uint16_t flags) {
    uint16_t d = virtq_alloc_desc(vq);   /* enough are free: checked by the caller */
    vq->desc[d].addr = addr;
    vq->desc[d].len = len;
    vq->desc[d].flags = flags;
    vq->desc[d].next = VRING_DESC_NONE;
    if (prev != VRING_DESC_NONE) {
        vq->desc[prev].flags |= VRING_DESC_F_NEXT;
        vq->desc[prev].next = d;
    }
    return d;
}

static int vblk_queue_rq(BlockDevice *disk, BlkRequest *rq) {
    VirtioBlk *vb = disk->driver_data;
    Virtqueue *vq = &vb->vdev.queues[0];
    if (vq->num_free < (uint32_t)rq->total_segs + 2) return BLK_BUSY;

    VblkSlot *s = &vb->slots[rq->tag];
    uint64_t slot_bus = vb->slots_bus + (uint64_t)rq->tag * sizeof(VblkSlot);
    s->hdr.type = rq->op == BLK_OP_READ    ? VIRTIO_BLK_T_IN
                  : rq->op == BLK_OP_WRITE ? VIRTIO_BLK_T_OUT
                                           : VIRTIO_BLK_T_FLUSH;
    s->hdr.reserved = 0;
    s->hdr.sector = rq->op == BLK_OP_FLUSH ? 0 : rq->sector;
    s->status = VIRTIO_BLK_STATUS_INIT;

    uint16_t head = add_desc(vq, VRING_DESC_NONE, slot_bus, sizeof(VirtioBlkReqHeader), 0);
    uint16_t prev = head;
    uint16_t data_flags = rq->op == BLK_OP_READ ? VRING_DESC_F_WRITE : 0;
    BlkRequest *r;
    uint32_t i;
    blk_rq_for_each_seg(rq, r, i) {
        const BlkSeg *seg = &r->seg[i];
        uint64_t bus = dma_map_page(&vb->vdev.pci->dev, seg->page, seg->offset, seg->len, dma_dir(rq));
        if (bus == DMA_MAPPING_ERROR) {
            release_chain(vb, head, rq, 0);
            return -EIO;
        }
        prev = add_desc(vq, prev, bus, seg->len, data_flags);
    }
    add_desc(vq, prev, slot_bus + offsetof(VblkSlot, status), 1, VRING_DESC_F_WRITE);
    vb->by_head[head] = rq;
    virtq_add_avail(vq, head);
    return 0;
}

static void vblk_kick(BlockDevice *disk) {
    VirtioBlk *vb = disk->driver_data;
    virtio_notify(&vb->vdev, 0);
}

static void vblk_poll(BlockDevice *disk) {
    VirtioBlk *vb = disk->driver_data;
    Virtqueue *vq = &vb->vdev.queues[0];
    while (virtq_has_used(vq)) {
        uint16_t head = virtq_pop_used(vq, NULL);
        BlkRequest *rq = head < vq->size ? vb->by_head[head] : NULL;
        if (rq == NULL) continue;   /* not ours (a device bug): ignore it */
        vb->by_head[head] = NULL;
        uint8_t st = vb->slots[rq->tag].status;
        release_chain(vb, head, rq, 1);
        blk_rq_done(rq, st == VIRTIO_BLK_S_OK ? 0 : st == VIRTIO_BLK_S_UNSUPP ? -EOPNOTSUPP : -EIO);
    }
}

/* Timed out: stop the device, then drop every chain it had (the block layer
 * fails the requests). */
static void vblk_reset(BlockDevice *disk) {
    VirtioBlk *vb = disk->driver_data;
    Virtqueue *vq = &vb->vdev.queues[0];
    virtio_reset(&vb->vdev);
    for (uint16_t h = 0; h < vq->size; h++) {
        if (vb->by_head[h] == NULL) continue;
        release_chain(vb, h, vb->by_head[h], 1);
        vb->by_head[h] = NULL;
    }
}

static const BlockOps vblk_ops = {
    .queue_rq = vblk_queue_rq,
    .kick = vblk_kick,
    .poll = vblk_poll,
    .reset = vblk_reset,
};

/* Queue vector (MSI-X): requests finished */
static int vblk_queue_irq(void *arg) {
    VirtioBlk *vb = arg;
    if (vb->disk.queue != NULL) blk_complete(&vb->disk);
    return IRQ_HANDLED;
}

/* Config-change vector (MSI-X): capacity changes are not handled yet. */
static int vblk_config_irq(void *arg) {
    (void)arg;
    return IRQ_HANDLED;
}

/* INTx (shared line): reading the ISR status acknowledges the device. */
static int vblk_intx_irq(void *arg) {
    VirtioBlk *vb = arg;
    uint8_t isr = virtio_isr_status(&vb->vdev);
    if ((isr & VIRTIO_ISR_QUEUE) && vb->disk.queue != NULL) blk_complete(&vb->disk);
    return isr != 0 ? IRQ_HANDLED : IRQ_NONE;
}

/* MSI-X entry 0 = config changes, entry 1 = request queue (with a single
 * vector the queue takes entry 0); INTx = one shared handler. */
static int vblk_setup_irqs(VirtioBlk *vb) {
    VirtioDevice *v = &vb->vdev;
    Device *dev = &v->pci->dev;
    int n = virtio_setup_irqs(v, 2);
    if (n < 0) return n;
    if (!v->msix) return irq_request(dev, 0, vblk_intx_irq, vb, IRQF_SHARED, "virtio-blk");
    uint16_t qvec = n >= 2 ? 1 : 0;
    int rc = irq_request(dev, qvec, vblk_queue_irq, vb, 0, "virtio-blk-queue");
    if (rc == 0 && n >= 2) rc = irq_request(dev, 0, vblk_config_irq, vb, 0, "virtio-blk-config");
    if (rc != 0) return rc;
    if (virtio_set_queue_vector(v, 0, qvec) != 0 ||
        virtio_set_config_vector(v, n >= 2 ? 0 : VIRTIO_MSI_NO_VECTOR) != 0) {
        kprintf("[VIRTIO-BLK] device refused the MSI-X vectors\n");
        return -EIO;
    }
    return 0;
}

/* Disk properties from the device config (after vblk_setup_irqs: MSI-X
 * moves the config). */
static void vblk_read_config(VirtioBlk *vb, uint32_t features) {
    VirtioDevice *v = &vb->vdev;
    BlockDevice *d = &vb->disk;
    uint16_t qsz = v->queues[0].size;
    d->nr_sectors = ((uint64_t)virtio_config_read32(v, VIRTIO_BLK_CFG_CAPACITY + 4) << 32) |
                    virtio_config_read32(v, VIRTIO_BLK_CFG_CAPACITY);
    uint32_t lbs = (features & VIRTIO_BLK_F_BLK_SIZE) ? virtio_config_read32(v, VIRTIO_BLK_CFG_BLK_SIZE) : 512;
    d->logical_block = lbs == 4096 ? 4096 : 512;
    uint8_t exp = (features & VIRTIO_BLK_F_TOPOLOGY) ? virtio_config_read8(v, VIRTIO_BLK_CFG_PHYS_EXP) : 0;
    d->physical_block = exp <= 3 ? d->logical_block << exp : d->logical_block;
    /* Without SEG_MAX the device takes one data segment per request */
    uint32_t segs = (features & VIRTIO_BLK_F_SEG_MAX) ? virtio_config_read32(v, VIRTIO_BLK_CFG_SEG_MAX) : 1;
    if (segs == 0) segs = 1;
    if (segs > (uint32_t)qsz - 2) segs = qsz - 2;
    d->max_segments = segs;
    d->max_transfer = VBLK_MAX_TRANSFER;
    d->flags = ((features & VIRTIO_BLK_F_RO) ? BLK_RDONLY : 0) | ((features & VIRTIO_BLK_F_FLUSH) ? BLK_CACHE : 0);
    d->ops = &vblk_ops;
    d->driver_data = vb;
}

static int vblk_probe(PciDevice *pci, const PciDeviceId *id) {
    (void)id;
    Device *dev = &pci->dev;
    VirtioBlk *vb = kmalloc(sizeof(VirtioBlk), GFP_ZERO);
    if (vb == NULL) return -ENOMEM;
    if (virtio_init_device(&vb->vdev, pci) != 0) {
        kfree(vb);
        return -ENODEV;
    }

    uint32_t want =
        VIRTIO_BLK_F_SEG_MAX | VIRTIO_BLK_F_RO | VIRTIO_BLK_F_BLK_SIZE | VIRTIO_BLK_F_FLUSH | VIRTIO_BLK_F_TOPOLOGY;
    uint32_t features = virtio_negotiate_features(&vb->vdev, want);
    uint16_t qsz = 0;
    uint32_t depth = 0;
    if (virtio_init_queue(&vb->vdev, 0) == 0) {   /* (a vring is not freed on failure: boot only) */
        qsz = vb->vdev.queues[0].size;
        depth = qsz < BLK_MAX_DEPTH ? qsz : BLK_MAX_DEPTH;
        vb->by_head = kmalloc(qsz * sizeof(BlkRequest *), GFP_ZERO);
        vb->slots = dma_alloc_coherent(dev, depth * sizeof(VblkSlot), &vb->slots_bus, 0);
    }
    if (vb->by_head == NULL || vb->slots == NULL || qsz < 3) {
        virtio_reset(&vb->vdev);
        if (vb->slots != NULL) dma_free_coherent(dev, depth * sizeof(VblkSlot), vb->slots, vb->slots_bus);
        kfree(vb->by_head);
        kfree(vb);
        return -ENOMEM;
    }

    int rc = vblk_setup_irqs(vb);
    if (rc != 0) return rc;
    vblk_read_config(vb, features);
    kprintf("[VIRTIO-BLK] %s: %s, features 0x%x\n", pci->name, vb->vdev.msix ? "MSI-X" : "INTx", features);
    virtio_device_ready(&vb->vdev);
    dev->driver_data = vb;
    return blk_disk_register(&vb->disk, "vd", depth);
}

/* ADR-0017: the hooks exist; power management of the disk comes later. */
static int vblk_suspend(Device *dev, int how) {
    (void)how;
    kprintf("[VIRTIO-BLK] %s: suspend not supported yet\n", dev->name);
    return -EOPNOTSUPP;
}

static int vblk_resume(Device *dev, int how) {
    (void)how;
    kprintf("[VIRTIO-BLK] %s: resume not supported yet\n", dev->name);
    return -EOPNOTSUPP;
}

static const PciDeviceId virtio_blk_ids[] = {
    PCI_DEVICE(VIRTIO_BLK_VENDOR_ID, VIRTIO_BLK_DEVICE_ID),
    {0},
};

static PciDriver virtio_blk_driver = {
    .driver = {.name = "virtio-blk", .suspend = vblk_suspend, .resume = vblk_resume},
    .ids = virtio_blk_ids,
    .probe = vblk_probe,
};

int virtio_blk_register(void) {
    return pci_register_driver(&virtio_blk_driver);
}
