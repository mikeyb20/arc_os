#ifndef ARCHOS_MM_KMALLOC_H
#define ARCHOS_MM_KMALLOC_H

#include <stddef.h>
#include <stdint.h>
#include "mm/gfp.h"

/* Kernel heap (M1-02b, ADR-0016, ADR-0019). No per-object header:
 *  - Requests up to KMALLOC_SLAB_MAX come from eight slab caches, one per
 *    power-of-two size class from 16 to 2048 bytes (mm/slab.h).
 *  - Larger requests, up to KMALLOC_LARGE_MAX, go straight to alloc_pages() at
 *    the smallest order that fits (8 KiB-4 MiB blocks are powers of two).
 *  - Anything bigger fails: use alloc_pages() or, later, a mapping.
 * Every object is at least 16-byte aligned; large blocks are page aligned.
 * kfree() finds the owner through virt -> page -> cache in O(1). Slab pages
 * and large blocks share one 512 MB accounting cap (KMEM_CAP_BYTES).
 * GFP_DMA32 is ignored: DMA buffers come from alloc_pages(). */
#define KMALLOC_MIN_SIZE   16u
#define KMALLOC_SLAB_MAX   2048u                   /* largest size class */
#define KMALLOC_NR_CLASSES 8u                     /* 16, 32, ..., 2048 */
#define KMALLOC_LARGE_MAX  (4096u << 10)           /* 4 MiB, PAGE_MAX_ORDER */

/* Set up the slab allocator and the size-class caches. Needs only the PMM
 * (pmm_init); allocates nothing. */
void kmalloc_init(void);

/* Allocate `size` bytes. Flags: GFP_ZERO zeroes them. Returns NULL for size 0,
 * sizes over KMALLOC_LARGE_MAX, or when memory or the cap runs out. O(1). */
void *kmalloc(size_t size, uint32_t flags);

/* Free a kmalloc (or kmem_cache_alloc) allocation. NULL is ignored. A pointer
 * no slab or large block owns panics. O(1). */
void kfree(void *ptr);

/* Resize an allocation, keeping its contents up to the smaller size. NULL
 * acts as kmalloc, size 0 as kfree (returns NULL). Stays in place when the new
 * size falls in the same size class or block; otherwise moves. On failure
 * returns NULL and the old allocation is untouched. */
void *krealloc(void *ptr, size_t new_size);

/* Print heap totals and one line per slab cache. */
void kmalloc_dump_stats(void);

/* Heap statistics (bytes). */
typedef struct {
    size_t total_used;    /* slab objects handed out (object size each) */
    size_t total_free;    /* slab bytes not handed out (free objects, red zones, slack) */
    size_t heap_mapped;   /* bytes of pages held by slabs */
    size_t large_bytes;   /* bytes held by large (alloc_pages) allocations */
} HeapStats;

/* Collect heap statistics into 'out'. */
void kmalloc_get_stats(HeapStats *out);

#endif /* ARCHOS_MM_KMALLOC_H */
