/* arc_os — Legacy virtio-PCI transport (virtqueues, status, MSI-X vectors)
 *
 * Owns: the legacy I/O-port register block of one virtio device, its vrings
 * (coherent DMA memory) and the descriptor free lists.
 * Invariants: vrings live in DMA memory from dma_alloc_coherent of the PCI
 * device; config_off matches the device's MSI-X state.
 * Locks: none; each driver serializes its own queues (virtio-blk, virtio-net).
 * Cost: descriptor alloc/free O(1) per descriptor; submit O(1). */

#include "drivers/virtio.h"
#include "drivers/dma.h"
#include "drivers/irq.h"
#include "arch/hal.h"
#include "mm/pmm.h"
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
    part1 = PAGE_ALIGN_UP(part1);
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

int virtio_init_device(VirtioDevice *vdev, PciDevice *pci) {
    vdev->pci = pci;
    vdev->io_base = pci_bar_io_base(pci->bar[0]);
    vdev->config_off = VIRTIO_REG_CONFIG;
    vdev->msix = 0;
    vdev->num_queues = 0;

    if (vdev->io_base == 0 || !(pci->bar[0] & PCI_BAR_IO)) {
        kprintf("[VIRTIO] BAR0 is not I/O space\n");
        return -1;
    }
    pci_enable_decode(pci, 0);

    /* Reset device */
    outb(vdev->io_base + VIRTIO_REG_DEVICE_STATUS, 0);

    /* Set ACKNOWLEDGE */
    outb(vdev->io_base + VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_ACK);

    /* Set DRIVER */
    outb(vdev->io_base + VIRTIO_REG_DEVICE_STATUS,
         VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);

    /* Enable PCI bus mastering for DMA */
    pci_enable_bus_master(pci);

    kprintf("[VIRTIO] Device %s  io_base=0x%x\n", pci->name, vdev->io_base);
    return 0;
}

int virtio_setup_irqs(VirtioDevice *vdev, unsigned want) {
    int n = pci_alloc_irq_vectors(vdev->pci, 1, want, PCI_IRQ_MSIX | PCI_IRQ_INTX);
    if (n < 0) return n;
    vdev->msix = vdev->pci->irq_kind == IRQ_KIND_MSIX;
    vdev->config_off = vdev->msix ? VIRTIO_REG_CONFIG_MSIX : VIRTIO_REG_CONFIG;
    return n;
}

int virtio_set_config_vector(VirtioDevice *vdev, uint16_t vec) {
    outw(vdev->io_base + VIRTIO_REG_MSI_CONFIG_VEC, vec);
    return inw(vdev->io_base + VIRTIO_REG_MSI_CONFIG_VEC) == vec ? 0 : -1;
}

int virtio_set_queue_vector(VirtioDevice *vdev, int queue_index, uint16_t vec) {
    outw(vdev->io_base + VIRTIO_REG_QUEUE_SELECT, (uint16_t)queue_index);
    outw(vdev->io_base + VIRTIO_REG_MSI_QUEUE_VEC, vec);
    return inw(vdev->io_base + VIRTIO_REG_MSI_QUEUE_VEC) == vec ? 0 : -1;
}

uint8_t virtio_isr_status(VirtioDevice *vdev) {
    return inb(vdev->io_base + VIRTIO_REG_ISR_STATUS);
}

uint8_t virtio_config_read8(VirtioDevice *vdev, uint16_t off) {
    return inb((uint16_t)(vdev->io_base + vdev->config_off + off));
}

uint32_t virtio_config_read32(VirtioDevice *vdev, uint16_t off) {
    return inl((uint16_t)(vdev->io_base + vdev->config_off + off));
}

uint32_t virtio_negotiate_features(VirtioDevice *vdev, uint32_t supported) {
    uint32_t device_features = inl(vdev->io_base + VIRTIO_REG_DEVICE_FEATURES);
    uint32_t negotiated = device_features & supported;
    outl(vdev->io_base + VIRTIO_REG_GUEST_FEATURES, negotiated);

    kprintf("[VIRTIO] Features: device=0x%x  negotiated=0x%x\n",
            device_features, negotiated);
    return negotiated;
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

    /* Coherent DMA memory for the vring (zeroed, contiguous, page-aligned);
     * the legacy queue register takes a 32-bit page number */
    uint64_t total = vring_total_size(qsz);
    uint32_t pages_needed = (uint32_t)((total + PAGE_SIZE - 1) / PAGE_SIZE);
    uint64_t phys;
    void *virt = dma_alloc_coherent(&vdev->pci->dev, total, &phys, 0);
    if (virt == NULL || phys / PAGE_SIZE > 0xFFFFFFFFULL) {
        kprintf("[VIRTIO] Failed to allocate %u pages for queue %d\n",
                pages_needed, queue_index);
        if (virt != NULL) dma_free_coherent(&vdev->pci->dev, total, virt, phys);
        return -1;
    }

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
    used_offset = PAGE_ALIGN_UP(used_offset);
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

void virtq_add_avail(Virtqueue *vq, uint16_t head) {
    uint16_t avail_idx = vq->avail->idx;
    vq->avail->ring[avail_idx % vq->size] = head;

    /* Memory barrier: device must see descriptor before avail idx update */
    virtio_mb();

    vq->avail->idx = avail_idx + 1;
}

void virtio_notify(VirtioDevice *vdev, int queue_index) {
    /* Memory barrier: device must see avail idx before notify */
    virtio_mb();
    outw(vdev->io_base + VIRTIO_REG_QUEUE_NOTIFY, (uint16_t)queue_index);
}

void virtio_submit(VirtioDevice *vdev, int queue_index, uint16_t head) {
    virtq_add_avail(&vdev->queues[queue_index], head);
    virtio_notify(vdev, queue_index);
}

void virtio_reset(VirtioDevice *vdev) {
    /* Legacy virtio-PCI: the port write resets the device before it returns */
    outb(vdev->io_base + VIRTIO_REG_DEVICE_STATUS, 0);
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
