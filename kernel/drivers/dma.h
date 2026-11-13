#ifndef ARCHOS_DRIVERS_DMA_H
#define ARCHOS_DRIVERS_DMA_H

/* DMA memory for drivers (M1-09, ADR-0006).
 *
 * Every driver gets bus addresses for device DMA from here, never from the
 * page allocator or the HHDM directly. On x86 without an IOMMU a bus address
 * is the physical address and memory is cache-coherent, so these are identity
 * mappings plus checks against the device's DMA mask; an IOMMU (later) slots
 * in behind the same calls. There are no bounce buffers: memory the device
 * cannot reach fails with DMA_MAPPING_ERROR.
 *
 * Coherent buffers: physically contiguous, zeroed, CPU and device see each
 * other's writes at once (rings, descriptors). Streaming mappings: an existing
 * kernel buffer handed to the device for one transfer, unmapped after. */

#include <stdint.h>
#include <stddef.h>
#include "drivers/bus.h"

struct page;

#define DMA_BIT_MASK(n) ((n) >= 64 ? ~0ULL : ((1ULL << (n)) - 1))

/* dma_alloc_coherent flags */
#define DMA_BELOW_4G 0x1   /* bus address below 4 GiB even if the mask allows more */

/* Transfer directions (streaming mappings) */
#define DMA_TO_DEVICE     1
#define DMA_FROM_DEVICE   2
#define DMA_BIDIRECTIONAL 3

#define DMA_MAPPING_ERROR (~0ULL)

/* Set the highest bus address dev can reach (e.g. DMA_BIT_MASK(32)). Returns
 * 0, or -EINVAL for a mask below 4 KiB. */
int dma_set_mask(Device *dev, uint64_t mask);

/* size bytes (rounded up to a power-of-two number of pages, at most 4 MiB),
 * zeroed, contiguous, aligned to their size. Sets *bus_addr. Returns the
 * kernel address, or NULL. Thread context. */
void *dma_alloc_coherent(Device *dev, size_t size, uint64_t *bus_addr, uint32_t flags);

/* Free a dma_alloc_coherent buffer (same size). */
void dma_free_coherent(Device *dev, size_t size, void *cpu_addr, uint64_t bus_addr);

/* Bus address of size bytes at kernel address cpu_addr (HHDM or kernel
 * image; must be physically contiguous), or DMA_MAPPING_ERROR. */
uint64_t dma_map_single(Device *dev, const void *cpu_addr, size_t size, int dir);

/* Bus address of size bytes at offset in page block pg, or DMA_MAPPING_ERROR. */
uint64_t dma_map_page(Device *dev, struct page *pg, size_t offset, size_t size, int dir);

/* End a streaming mapping (dma_map_single or dma_map_page). */
void dma_unmap(Device *dev, uint64_t bus_addr, size_t size, int dir);

/* Pure helper (host-tested): 1 if [bus, bus + size) fits under mask. */
static inline int dma_fits(uint64_t bus, size_t size, uint64_t mask) {
    return size != 0 && bus + size - 1 >= bus && bus + size - 1 <= mask;
}

#endif /* ARCHOS_DRIVERS_DMA_H */
