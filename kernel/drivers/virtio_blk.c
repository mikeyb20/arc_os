#include "drivers/virtio_blk.h"
#include "drivers/virtio.h"
#include "drivers/pci.h"
#include "arch/x86_64/io.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "lib/mem.h"
#include "lib/kprintf.h"

/* Sector size in bytes */
#define SECTOR_SIZE 512

/* Polling timeout: max spins before giving up */
#define POLL_TIMEOUT 10000000

static VirtioDevice blk_vdev;
static uint64_t blk_capacity;  /* in 512-byte sectors */
static int blk_initialized;

/* Free 'count' contiguous physical pages starting at phys. */
static void free_contiguous_pages(uint64_t phys, uint32_t count) {
    for (uint32_t i = 0; i < count; i++)
        pmm_free_page(phys + i * PAGE_SIZE);
}

int virtio_blk_init(void) {
    const PciDevice *pci = pci_find_device(VIRTIO_BLK_VENDOR_ID, VIRTIO_BLK_DEVICE_ID);
    if (!pci) {
        kprintf("[VIRTIO-BLK] No VirtIO block device found\n");
        return -1;
    }

    kprintf("[VIRTIO-BLK] Found device at %x:%x.%x\n",
            pci->addr.bus, pci->addr.device, pci->addr.function);

    if (virtio_init_device(&blk_vdev, pci) != 0) {
        return -1;
    }

    /* Negotiate features — we don't need any optional features for basic I/O */
    virtio_negotiate_features(&blk_vdev, 0);

    /* Initialize request queue (queue 0) */
    if (virtio_init_queue(&blk_vdev, 0) != 0) {
        return -1;
    }

    /* Read capacity from device-specific config (offset 0x14 from BAR0).
     * Legacy VirtIO-blk config: 64-bit capacity at offset 0. */
    uint32_t cap_lo = inl(blk_vdev.io_base + VIRTIO_REG_CONFIG + 0);
    uint32_t cap_hi = inl(blk_vdev.io_base + VIRTIO_REG_CONFIG + 4);
    blk_capacity = ((uint64_t)cap_hi << 32) | cap_lo;

    kprintf("[VIRTIO-BLK] Capacity: %lu sectors (%lu MB)\n",
            blk_capacity, (blk_capacity * SECTOR_SIZE) / (1024 * 1024));

    /* Mark device ready */
    virtio_device_ready(&blk_vdev);

    blk_initialized = 1;
    kprintf("[VIRTIO-BLK] Initialized successfully\n");
    return 0;
}

int virtio_blk_read(uint64_t sector, uint32_t count, void *buf) {
    if (!blk_initialized) return -1;
    if (count == 0) return 0;
    if (sector + count > blk_capacity) return -1;

    Virtqueue *vq = &blk_vdev.queues[0];
    uint64_t hhdm = vmm_get_hhdm_offset();
    uint32_t data_bytes = count * SECTOR_SIZE;

    /* Allocate a physically contiguous page for the request header + status.
     * Layout: [VirtioBlkReqHeader | padding... | status_byte]
     * Data buffer gets its own contiguous allocation. */
    uint64_t req_phys = pmm_alloc_page();
    if (req_phys == 0) {
        kprintf("[VIRTIO-BLK] Failed to alloc request page\n");
        return -1;
    }

    uint32_t data_pages = (data_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t data_phys = pmm_alloc_contiguous(data_pages);
    if (data_phys == 0) {
        kprintf("[VIRTIO-BLK] Failed to alloc %u data pages\n", data_pages);
        pmm_free_page(req_phys);
        return -1;
    }

    /* Set up request header */
    VirtioBlkReqHeader *hdr = (VirtioBlkReqHeader *)(req_phys + hhdm);
    hdr->type = VIRTIO_BLK_T_IN;
    hdr->reserved = 0;
    hdr->sector = sector;

    /* Status byte at end of request page */
    uint8_t *status_ptr = (uint8_t *)(req_phys + hhdm + sizeof(VirtioBlkReqHeader));
    *status_ptr = 0xFF;  /* Sentinel — device will overwrite */
    uint64_t status_phys = req_phys + sizeof(VirtioBlkReqHeader);

    /* Build 3-descriptor chain: header → data → status */
    uint16_t d0 = virtq_alloc_desc(vq);
    uint16_t d1 = virtq_alloc_desc(vq);
    uint16_t d2 = virtq_alloc_desc(vq);
    if (d0 == VRING_DESC_NONE || d1 == VRING_DESC_NONE || d2 == VRING_DESC_NONE) {
        kprintf("[VIRTIO-BLK] No free descriptors\n");
        if (d0 != VRING_DESC_NONE) virtq_free_chain(vq, d0);
        if (d1 != VRING_DESC_NONE) virtq_free_chain(vq, d1);
        if (d2 != VRING_DESC_NONE) virtq_free_chain(vq, d2);
        pmm_free_page(req_phys);
        free_contiguous_pages(data_phys, data_pages);
        return -1;
    }

    /* Descriptor 0: request header (device-readable) */
    vq->desc[d0].addr  = req_phys;
    vq->desc[d0].len   = sizeof(VirtioBlkReqHeader);
    vq->desc[d0].flags = VRING_DESC_F_NEXT;
    vq->desc[d0].next  = d1;

    /* Descriptor 1: data buffer (device-writable for reads) */
    vq->desc[d1].addr  = data_phys;
    vq->desc[d1].len   = data_bytes;
    vq->desc[d1].flags = VRING_DESC_F_WRITE | VRING_DESC_F_NEXT;
    vq->desc[d1].next  = d2;

    /* Descriptor 2: status byte (device-writable) */
    vq->desc[d2].addr  = status_phys;
    vq->desc[d2].len   = 1;
    vq->desc[d2].flags = VRING_DESC_F_WRITE;
    vq->desc[d2].next  = VRING_DESC_NONE;

    /* Submit and poll */
    virtio_submit(&blk_vdev, 0, d0);

    /* Spin-poll for completion */
    int timeout = POLL_TIMEOUT;
    while (!virtq_has_used(vq) && --timeout > 0) {
        __asm__ volatile ("pause");
    }

    if (timeout == 0) {
        kprintf("[VIRTIO-BLK] Read timeout (sector %lu, count %u)\n", sector, count);
        virtq_free_chain(vq, d0);
        pmm_free_page(req_phys);
        free_contiguous_pages(data_phys, data_pages);
        return -1;
    }

    /* Pop used entry */
    uint32_t used_len;
    virtq_pop_used(vq, &used_len);
    virtq_free_chain(vq, d0);

    /* Check status */
    int ret = -1;
    if (*status_ptr == VIRTIO_BLK_S_OK) {
        /* Copy data to caller's buffer */
        memcpy(buf, (void *)(data_phys + hhdm), data_bytes);
        ret = 0;
    } else {
        kprintf("[VIRTIO-BLK] Read failed, status=%d\n", *status_ptr);
    }

    /* Free buffers */
    pmm_free_page(req_phys);
    free_contiguous_pages(data_phys, data_pages);

    return ret;
}

int virtio_blk_write(uint64_t sector, uint32_t count, const void *buf) {
    (void)sector; (void)count; (void)buf;
    kprintf("[VIRTIO-BLK] Write not implemented\n");
    return -1;
}

uint64_t virtio_blk_capacity(void) {
    return blk_capacity;
}
