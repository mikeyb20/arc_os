#ifndef ARCHOS_MM_KMALLOC_H
#define ARCHOS_MM_KMALLOC_H

#include <stddef.h>
#include <stdint.h>

/* Allocation flags */
#define GFP_KERNEL  0x00  /* Normal kernel allocation */
#define GFP_ZERO    0x01  /* Zero the allocation */

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
} HeapStats;

/* Collect heap statistics into 'out'. */
void kmalloc_get_stats(HeapStats *out);

#endif /* ARCHOS_MM_KMALLOC_H */
