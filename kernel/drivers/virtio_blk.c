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

/* Build a 3-descriptor chain for a VirtIO-blk read request.
 * Returns the head descriptor index, or VRING_DESC_NONE on failure. */
static uint16_t blk_build_read_chain(Virtqueue *vq, uint64_t req_phys,
                                      uint64_t data_phys, uint32_t data_bytes,
                                      uint64_t status_phys) {
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

    /* Descriptor 1: data buffer (device-writable for reads) */
    vq->desc[desc_data].addr  = data_phys;
    vq->desc[desc_data].len   = data_bytes;
    vq->desc[desc_data].flags = VRING_DESC_F_WRITE | VRING_DESC_F_NEXT;
    vq->desc[desc_data].next  = desc_stat;

    /* Descriptor 2: status byte (device-writable) */
    vq->desc[desc_stat].addr  = status_phys;
    vq->desc[desc_stat].len   = 1;
    vq->desc[desc_stat].flags = VRING_DESC_F_WRITE;
    vq->desc[desc_stat].next  = VRING_DESC_NONE;

    return desc_hdr;
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
    uint16_t desc_head = blk_build_read_chain(vq, req_phys, data_phys, data_bytes, status_phys);
    if (desc_head == VRING_DESC_NONE) {
        kprintf("[VIRTIO-BLK] No free descriptors\n");
        pmm_free_page(req_phys);
        free_contiguous_pages(data_phys, data_pages);
        return -1;
    }

    /* Submit and poll */
    virtio_submit(&blk_vdev, 0, desc_head);

    /* Spin-poll for completion */
    int timeout = POLL_TIMEOUT;
    while (!virtq_has_used(vq) && --timeout > 0) {
        __asm__ volatile ("pause");
    }

    if (timeout == 0) {
        kprintf("[VIRTIO-BLK] Read timeout (sector %lu, count %u)\n", sector, count);
        virtq_free_chain(vq, desc_head);
        pmm_free_page(req_phys);
        free_contiguous_pages(data_phys, data_pages);
        return -1;
    }

    /* Pop used entry */
    uint32_t used_len;
    virtq_pop_used(vq, &used_len);
    virtq_free_chain(vq, desc_head);

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

/* Build a 3-descriptor chain for a VirtIO-blk write request.
 * Returns the head descriptor index, or VRING_DESC_NONE on failure. */
static uint16_t blk_build_write_chain(Virtqueue *vq, uint64_t req_phys,
                                       uint64_t data_phys, uint32_t data_bytes,
                                       uint64_t status_phys) {
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

    /* Descriptor 1: data buffer (device-readable for writes) */
    vq->desc[desc_data].addr  = data_phys;
    vq->desc[desc_data].len   = data_bytes;
    vq->desc[desc_data].flags = VRING_DESC_F_NEXT;
    vq->desc[desc_data].next  = desc_stat;

    /* Descriptor 2: status byte (device-writable) */
    vq->desc[desc_stat].addr  = status_phys;
    vq->desc[desc_stat].len   = 1;
    vq->desc[desc_stat].flags = VRING_DESC_F_WRITE;
    vq->desc[desc_stat].next  = VRING_DESC_NONE;

    return desc_hdr;
}

int virtio_blk_write(uint64_t sector, uint32_t count, const void *buf) {
    if (!blk_initialized) return -1;
    if (count == 0) return 0;
    if (sector + count > blk_capacity) return -1;

    Virtqueue *vq = &blk_vdev.queues[0];
    uint64_t hhdm = vmm_get_hhdm_offset();
    uint32_t data_bytes = count * SECTOR_SIZE;

    /* Allocate request page (header + status) and data pages */
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
    hdr->type = VIRTIO_BLK_T_OUT;
    hdr->reserved = 0;
    hdr->sector = sector;

    /* Copy caller data into DMA pages BEFORE submit */
    memcpy((void *)(data_phys + hhdm), buf, data_bytes);

    /* Status byte at end of request page */
    uint8_t *status_ptr = (uint8_t *)(req_phys + hhdm + sizeof(VirtioBlkReqHeader));
    *status_ptr = 0xFF;
    uint64_t status_phys = req_phys + sizeof(VirtioBlkReqHeader);

    /* Build 3-descriptor chain: header → data → status */
    uint16_t desc_head = blk_build_write_chain(vq, req_phys, data_phys, data_bytes, status_phys);
    if (desc_head == VRING_DESC_NONE) {
        kprintf("[VIRTIO-BLK] No free descriptors\n");
        pmm_free_page(req_phys);
        free_contiguous_pages(data_phys, data_pages);
        return -1;
    }

    /* Submit and poll */
    virtio_submit(&blk_vdev, 0, desc_head);

    int timeout = POLL_TIMEOUT;
    while (!virtq_has_used(vq) && --timeout > 0) {
        __asm__ volatile ("pause");
    }

    if (timeout == 0) {
        kprintf("[VIRTIO-BLK] Write timeout (sector %lu, count %u)\n", sector, count);
        virtq_free_chain(vq, desc_head);
        pmm_free_page(req_phys);
        free_contiguous_pages(data_phys, data_pages);
        return -1;
    }

    /* Pop used entry */
    uint32_t used_len;
    virtq_pop_used(vq, &used_len);
    virtq_free_chain(vq, desc_head);

    /* Check status */
    int ret = -1;
    if (*status_ptr == VIRTIO_BLK_S_OK) {
        ret = 0;
    } else {
        kprintf("[VIRTIO-BLK] Write failed, status=%d\n", *status_ptr);
    }

    /* Free buffers */
    pmm_free_page(req_phys);
    free_contiguous_pages(data_phys, data_pages);

    return ret;
}

uint64_t virtio_blk_capacity(void) {
    return blk_capacity;
}
