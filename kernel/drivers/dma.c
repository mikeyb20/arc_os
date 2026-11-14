/* arc_os — DMA helper (M1-09, ADR-0006)
 *
 * Owns: turning kernel memory into device bus addresses (identity on x86
 * without an IOMMU) and allocating coherent DMA buffers from the buddy
 * allocator.
 * Invariants: every address handed to a device fits under its dma_mask; a
 * coherent buffer is one buddy block, so it is contiguous and size-aligned.
 * Locks: none of its own (the buddy allocator locks itself).
 * Cost: coherent alloc = one alloc_pages; map_single = one page-table walk
 * per page of the buffer; map_page = O(1). */

#include "drivers/dma.h"
#include "mm/pmm.h"
#include "mm/page.h"
#include "mm/vmm.h"
#include "mm/gfp.h"
#include "lib/kprintf.h"
#include "uapi/errno.h"

#define DMA_32BIT_LIMIT 0xFFFFFFFFULL

int dma_set_mask(Device *dev, uint64_t mask) {
    if (mask < PAGE_SIZE - 1) return -EINVAL;
    dev->dma_mask = mask;
    return 0;
}

/* Buddy order for size bytes, or -1 for 0 bytes or more than PAGE_MAX_ORDER. */
static int order_for(size_t size) {
    if (size == 0) return -1;
    unsigned order = page_order_for(size);
    return order <= PAGE_MAX_ORDER ? (int)order : -1;
}

void *dma_alloc_coherent(Device *dev, size_t size, uint64_t *bus_addr, uint32_t flags) {
    int order = order_for(size);
    if (order < 0) return NULL;
    uint32_t gfp = GFP_ZERO;
    if ((flags & DMA_BELOW_4G) || dev->dma_mask <= DMA_32BIT_LIMIT) gfp |= GFP_DMA32;
    struct page *pg = alloc_pages((unsigned)order, gfp);
    if (pg == NULL) return NULL;
    uint64_t phys = page_to_phys(pg);
    if (!dma_fits(phys, (size_t)PAGE_SIZE << order, dev->dma_mask)) {
        kprintf("[DMA] %s: buffer at 0x%lx is above the device's mask 0x%lx\n", dev->name, phys, dev->dma_mask);
        free_pages(pg, (unsigned)order);
        return NULL;
    }
    *bus_addr = phys;
    return page_to_virt(pg);
}

void dma_free_coherent(Device *dev, size_t size, void *cpu_addr, uint64_t bus_addr) {
    (void)dev;
    (void)bus_addr;
    int order = order_for(size);
    if (cpu_addr == NULL || order < 0) return;
    free_pages(virt_to_page(cpu_addr), (unsigned)order);
}

/* Physical address of a kernel virtual address (HHDM or kernel image), or 0:
 * one walk of the kernel page tables. */
static uint64_t kernel_phys(uint64_t virt) {
    return vmm_get_phys(virt);
}

uint64_t dma_map_single(Device *dev, const void *cpu_addr, size_t size, int dir) {
    (void)dir;
    uint64_t virt = (uint64_t)(uintptr_t)cpu_addr;
    uint64_t phys = kernel_phys(virt);
    if (phys == 0 || size == 0) return DMA_MAPPING_ERROR;
    /* Contiguous? Every later page must follow the first (kernel-image buffers) */
    for (uint64_t p = PAGE_ALIGN_DOWN(virt) + PAGE_SIZE; p < virt + size; p += PAGE_SIZE) {
        if (kernel_phys(p) != PAGE_ALIGN_DOWN(phys) + (p - PAGE_ALIGN_DOWN(virt))) return DMA_MAPPING_ERROR;
    }
    if (!dma_fits(phys, size, dev->dma_mask)) return DMA_MAPPING_ERROR;
    return phys;
}

uint64_t dma_map_page(Device *dev, struct page *pg, size_t offset, size_t size, int dir) {
    (void)dir;
    uint64_t phys = page_to_phys(pg) + offset;
    return dma_fits(phys, size, dev->dma_mask) ? phys : DMA_MAPPING_ERROR;
}

void dma_unmap(Device *dev, uint64_t bus_addr, size_t size, int dir) {
    /* Identity mapping, coherent caches: nothing to undo until an IOMMU */
    (void)dev;
    (void)bus_addr;
    (void)size;
    (void)dir;
}
