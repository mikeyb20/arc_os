#ifndef ARCHOS_MM_GFP_H
#define ARCHOS_MM_GFP_H

/* Allocation flags shared by the page allocator (alloc_pages) and kmalloc.
 * Names follow Linux so later code reads the same. */
#define GFP_KERNEL 0x00  /* Normal kernel allocation */
#define GFP_ZERO   0x01  /* Zero the memory before returning it */
#define GFP_DMA32  0x02  /* Physical memory below 4 GiB (32-bit DMA devices) */

#endif /* ARCHOS_MM_GFP_H */
