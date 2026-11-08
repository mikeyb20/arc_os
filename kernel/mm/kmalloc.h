#ifndef ARCHOS_MM_KMALLOC_H
#define ARCHOS_MM_KMALLOC_H

#include <stddef.h>
#include <stdint.h>
#include "mm/gfp.h"

/* Kernel heap (ADR-0016: capped at 512 MB, counting both paths below).
 *  - Requests under KMALLOC_LARGE_MIN, or over KMALLOC_LARGE_MAX, come from the
 *    first-fit heap at HEAP_START (virtually contiguous, built page by page).
 *  - Requests from KMALLOC_LARGE_MIN to KMALLOC_LARGE_MAX go straight to
 *    alloc_pages() and live in the HHDM, rounded up to a power-of-two block.
 * GFP_DMA32 is ignored: DMA buffers come from alloc_pages(). The slab allocator
 * (M1-02b) replaces the small-object path. */
#define KMALLOC_LARGE_MIN (2u * 4096u)            /* 8 KiB */
#define KMALLOC_LARGE_MAX (4096u << 10)           /* 4 MiB, PAGE_MAX_ORDER */
#define KMALLOC_CAP_BYTES (512ull * 1024 * 1024)  /* ADR-0016 */

/* Initialize the kernel heap allocator. Must be called after vmm_init(). */
void kmalloc_init(void);

/* Allocate 'size' bytes from the kernel heap. Returns NULL on failure. */
void *kmalloc(size_t size, uint32_t flags);

/* Free a previous kmalloc allocation. */
void kfree(void *ptr);

/* Reallocate: grow or shrink an allocation. Returns NULL on failure. */
void *krealloc(void *ptr, size_t new_size);

/* Print heap statistics. */
void kmalloc_dump_stats(void);

/* Heap statistics structure */
typedef struct {
    size_t total_used;
    size_t total_free;
    size_t total_blocks;
    size_t free_blocks;
    size_t largest_free;
    size_t heap_mapped;
    size_t large_bytes;   /* bytes held by large (alloc_pages) allocations */
} HeapStats;

/* Collect heap statistics into 'out'. */
void kmalloc_get_stats(HeapStats *out);

/* Most heap blocks a single kmalloc has walked since boot (first-fit cost,
 * reported as a budget; M1-02b's slab replaces the walk). */
size_t kmalloc_max_walk(void);

#endif /* ARCHOS_MM_KMALLOC_H */
