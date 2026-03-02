#include "drivers/virtio.h"
#include "arch/x86_64/io.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "lib/mem.h"
#include "lib/kprintf.h"

/* Vring size calculations per VirtIO spec (legacy layout).
 * align is typically 4096 for legacy PCI transport. */
static uint64_t vring_desc_size(uint16_t qsz) {
    return (uint64_t)qsz * sizeof(VringDesc);
}

static uint64_t vring_avail_size(uint16_t qsz) {
    return sizeof(uint16_t) * 2 + sizeof(uint16_t) * qsz + sizeof(uint16_t);
}

static uint64_t vring_used_size(uint16_t qsz) {
    return sizeof(uint16_t) * 2 + sizeof(VringUsedElem) * qsz + sizeof(uint16_t);
}

/* Total vring size with legacy alignment (descriptors + avail aligned to 4096,
 * then used ring). */
static uint64_t vring_total_size(uint16_t qsz) {
    uint64_t part1 = vring_desc_size(qsz) + vring_avail_size(qsz);
    /* Align part1 up to page boundary */
    part1 = (part1 + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t part2 = vring_used_size(qsz);
    return part1 + part2;
}

/* Memory barrier helpers */
static inline void virtio_mb(void) {
    __asm__ volatile ("mfence" ::: "memory");
}

static inline void virtio_rmb(void) {
    __asm__ volatile ("lfence" ::: "memory");
}

int virtio_init_device(VirtioDevice *vdev, const PciDevice *pci) {
    vdev->pci = pci;
    vdev->io_base = pci_bar_io_base(pci->bar[0]);
    vdev->irq = pci->irq_line;
    vdev->num_queues = 0;

    if (vdev->io_base == 0) {
        kprintf("[VIRTIO] BAR0 is not I/O space\n");
        return -1;
    }

    /* Reset device */
    outb(vdev->io_base + VIRTIO_REG_DEVICE_STATUS, 0);

    /* Set ACKNOWLEDGE */
    outb(vdev->io_base + VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_ACK);

    /* Set DRIVER */
    outb(vdev->io_base + VIRTIO_REG_DEVICE_STATUS,
         VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);

    /* Enable PCI bus mastering for DMA */
    pci_enable_bus_master(pci);

    kprintf("[VIRTIO] Device at %x:%x.%x  io_base=0x%x  irq=%d\n",
            pci->addr.bus, pci->addr.device, pci->addr.function,
            vdev->io_base, vdev->irq);

    return 0;
}

void virtio_negotiate_features(VirtioDevice *vdev, uint32_t supported) {
    uint32_t device_features = inl(vdev->io_base + VIRTIO_REG_DEVICE_FEATURES);
    uint32_t negotiated = device_features & supported;
    outl(vdev->io_base + VIRTIO_REG_GUEST_FEATURES, negotiated);

    kprintf("[VIRTIO] Features: device=0x%x  negotiated=0x%x\n",
            device_features, negotiated);
}

int virtio_init_queue(VirtioDevice *vdev, int queue_index) {
    if (queue_index >= VIRTIO_MAX_QUEUES) return -1;

    /* Select the queue */
    outw(vdev->io_base + VIRTIO_REG_QUEUE_SELECT, (uint16_t)queue_index);

    /* Read queue size (0 = queue not available) */
    uint16_t qsz = inw(vdev->io_base + VIRTIO_REG_QUEUE_SIZE);
    if (qsz == 0) {
        kprintf("[VIRTIO] Queue %d not available\n", queue_index);
        return -1;
    }

    /* Allocate physically contiguous memory for the vring */
    uint64_t total = vring_total_size(qsz);
    uint32_t pages_needed = (uint32_t)((total + PAGE_SIZE - 1) / PAGE_SIZE);
    uint64_t phys = pmm_alloc_contiguous(pages_needed);
    if (phys == 0) {
        kprintf("[VIRTIO] Failed to allocate %u pages for queue %d\n",
                pages_needed, queue_index);
        return -1;
    }

    /* Zero the vring memory */
    uint64_t hhdm = vmm_get_hhdm_offset();
    void *virt = (void *)(phys + hhdm);
    memset(virt, 0, pages_needed * PAGE_SIZE);

    /* Set up virtqueue pointers */
    Virtqueue *vq = &vdev->queues[queue_index];
    vq->size = qsz;
    vq->phys_addr = phys;
    vq->num_pages = pages_needed;

    vq->desc  = (VringDesc *)virt;
    /* Avail ring follows descriptors */
    vq->avail = (VringAvail *)((uint8_t *)virt + vring_desc_size(qsz));
    /* Used ring is at page-aligned offset after desc+avail */
    uint64_t used_offset = vring_desc_size(qsz) + vring_avail_size(qsz);
    used_offset = (used_offset + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    vq->used  = (VringUsed *)((uint8_t *)virt + used_offset);

    /* Initialize free descriptor list: chain all descriptors via next */
    for (uint16_t i = 0; i < qsz; i++) {
        vq->desc[i].next = (i + 1 < qsz) ? (i + 1) : VRING_DESC_NONE;
    }
    vq->free_head = 0;
    vq->num_free = qsz;
    vq->last_used_idx = 0;

    /* Tell device the physical page frame number of the vring */
    outl(vdev->io_base + VIRTIO_REG_QUEUE_ADDR, (uint32_t)(phys / PAGE_SIZE));

    if (queue_index >= vdev->num_queues) {
        vdev->num_queues = queue_index + 1;
    }

    kprintf("[VIRTIO] Queue %d: size=%u  phys=0x%lx  pages=%u\n",
            queue_index, qsz, phys, pages_needed);

    return 0;
}

void virtio_device_ready(VirtioDevice *vdev) {
    outb(vdev->io_base + VIRTIO_REG_DEVICE_STATUS,
         VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);
}

uint16_t virtq_alloc_desc(Virtqueue *vq) {
    if (vq->num_free == 0) return VRING_DESC_NONE;

    uint16_t idx = vq->free_head;
    vq->free_head = vq->desc[idx].next;
    vq->num_free--;
    vq->desc[idx].next = VRING_DESC_NONE;
    return idx;
}

void virtq_free_chain(Virtqueue *vq, uint16_t head) {
    uint16_t idx = head;
    for (;;) {
        uint16_t next = vq->desc[idx].next;
        /* Push this descriptor back to free list */
        vq->desc[idx].addr = 0;
        vq->desc[idx].len = 0;
        vq->desc[idx].flags = 0;
        vq->desc[idx].next = vq->free_head;
        vq->free_head = idx;
        vq->num_free++;

        if (next == VRING_DESC_NONE) break;
        idx = next;
    }
}

void virtio_submit(VirtioDevice *vdev, int queue_index, uint16_t head) {
    Virtqueue *vq = &vdev->queues[queue_index];

    /* Add to available ring */
    uint16_t avail_idx = vq->avail->idx;
    vq->avail->ring[avail_idx % vq->size] = head;

    /* Memory barrier: device must see descriptor before avail idx update */
    virtio_mb();

    vq->avail->idx = avail_idx + 1;

    /* Memory barrier: device must see avail idx before notify */
    virtio_mb();

    /* Notify device */
    outw(vdev->io_base + VIRTIO_REG_QUEUE_NOTIFY, (uint16_t)queue_index);
}

int virtq_has_used(Virtqueue *vq) {
    virtio_rmb();
    return vq->last_used_idx != vq->used->idx;
}

uint16_t virtq_pop_used(Virtqueue *vq, uint32_t *len) {
    virtio_rmb();
    uint16_t idx = vq->last_used_idx % vq->size;
    VringUsedElem *e = &vq->used->ring[idx];
    if (len) *len = e->len;
    uint16_t desc_head = (uint16_t)e->id;
    vq->last_used_idx++;
    return desc_head;
}
