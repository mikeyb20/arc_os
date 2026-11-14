#ifndef ARCHOS_MM_GFP_H
#define ARCHOS_MM_GFP_H

/* Allocation flags shared by the page allocator (alloc_pages) and kmalloc.
 * Names follow Linux so later code reads the same. */
#define GFP_KERNEL 0x00  /* Normal kernel allocation */
#define GFP_ZERO   0x01  /* Zero the memory before returning it */
#define GFP_DMA32  0x02  /* Physical memory below 4 GiB (32-bit DMA devices) */
/* May sleep to reclaim page-cache pages when memory runs low (M1-14): below
 * the min watermark the caller reclaims directly before it gets its page.
 * Only for callers that hold no spinlock and could sleep anyway (user page
 * faults, page-cache pages). Linux's GFP_KERNEL implies this; here kernel
 * allocations never reclaim, so they keep working under any lock and may use
 * the reserve below min. */
#define GFP_RECLAIM 0x04

#endif /* ARCHOS_MM_GFP_H */
