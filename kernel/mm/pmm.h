#ifndef ARCHOS_MM_PMM_H
#define ARCHOS_MM_PMM_H

#include <stdint.h>
#include <stddef.h>
#include "boot/bootinfo.h"
#include "mm/gfp.h"
#include "mm/page.h"

#define PAGE_ALIGN_UP(x)    (((x) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1ULL))
#define PAGE_ALIGN_DOWN(x)  ((x) & ~(PAGE_SIZE - 1ULL))

/* Physical memory manager: page descriptor array + buddy allocator
 * (ADR-0016, ADR-0019, block M1-02).
 *
 * Free memory sits in power-of-two blocks of 2^order pages, order 0 (4 KiB) to
 * PAGE_MAX_ORDER (4 MiB), on per-order free lists in two zones: below 4 GiB
 * (DMA32) and above. Allocation splits a larger block; free merges a block
 * with its "buddy" (the neighbour it was split from) as long as both are free.
 * Both are at most one step per order.
 *
 * Locking: one IRQ-safe spinlock, a leaf lock (work-blocks.md §7, level 10).
 * Nothing else is taken while it is held. The memory-pressure hook (M1-14)
 * runs after it is released, in the allocating context. */

/* Initialize from the BootInfo memory map. Reads `arc.mem=<size>` from the
 * kernel command line (e.g. arc.mem=4G): RAM beyond that amount is ignored, so
 * a bigger machine can run at the 4 GB floor. Must run before any allocation. */
void pmm_init(const BootInfo *info);

/* Allocate 2^order physically contiguous pages, aligned to their size.
 * Flags: GFP_ZERO zeroes the block; GFP_DMA32 takes memory below 4 GiB only.
 * Without GFP_DMA32 memory above 4 GiB is used first, then below.
 * GFP_RECLAIM: the caller may sleep and holds no spinlock, so low memory is
 * reclaimed before it fails (M1-14, see the pressure hook below).
 * Returns the head descriptor (refcount 1), or NULL. At most one step per
 * order in the allocator itself. */
struct page *alloc_pages(unsigned order, uint32_t flags);

/* alloc_pages without the memory-pressure hook below: the buddy allocation
 * alone (reclaim's own retries use it). Same flags and result. */
struct page *pmm_alloc_block(unsigned order, uint32_t flags);

/* --- Memory pressure (M1-14, mm/reclaim.c) --- */

/* Free-page watermarks: below low the reclaim thread is woken, below min a
 * GFP_RECLAIM allocation reclaims directly, and reclaim stops at high. */
typedef struct PmmWatermarks {
    uint64_t min;
    uint64_t low;
    uint64_t high;
} PmmWatermarks;

/* Called by alloc_pages when the allocation failed (got NULL) or left fewer
 * free pages than the low watermark; returns the block to hand out (got, or
 * one found after reclaiming). It runs in the allocating context, so it
 * decides itself what that context allows (GFP_RECLAIM, spinlocks held). */
typedef struct page *(*pmm_pressure_fn)(struct page *got, unsigned order, uint32_t flags);

/* Set by reclaim_init once (boot). Until then alloc_pages never calls out. */
void pmm_set_watermarks(const PmmWatermarks *w);
void pmm_get_watermarks(PmmWatermarks *out);
void pmm_set_pressure_hook(pmm_pressure_fn fn);

/* Fewest free pages left after any allocation since boot (/proc/vmstat). */
uint64_t pmm_free_lowest(void);

/* Drop the caller's reference to a block from alloc_pages(page, order). When
 * the reference count reaches 0 the block returns to the free lists and merges
 * with free buddies. `order` must match the allocation. Bad frees (reserved
 * page, already free, wrong order) are logged and ignored. */
void free_pages(struct page *page, unsigned order);

/* --- Compatibility wrappers (pre-M1-02 interface, physical addresses) --- */

/* Allocate one page. Returns its physical address, or 0. */
uint64_t pmm_alloc_page(void);

/* Free one page by physical address. 0 is ignored. */
void pmm_free_page(uint64_t phys_addr);

/* Allocate `count` contiguous pages. Each page is then an independent order-0
 * page, freed one at a time with pmm_free_page(). Returns the base, or 0. */
uint64_t pmm_alloc_contiguous(size_t count);

/* Pages handed to the allocator at boot (usable RAM after arc.mem, minus the
 * descriptor array and memory below 1 MiB). */
uint64_t pmm_get_total_pages(void);

/* Pages currently free. */
uint64_t pmm_get_free_pages(void);

/* Bytes used by the descriptor array. */
uint64_t pmm_get_descriptor_bytes(void);

/* Number of free blocks of each order, summed over both zones.
 * `out` has PAGE_MAX_ORDER + 1 entries. */
void pmm_get_free_blocks(uint64_t *out);

/* Print the descriptor array size and free blocks per order. */
void pmm_dump_stats(void);

/* Parse a size such as "4G", "512M", "65536K" or "4294967296" (bytes).
 * Returns 1 and sets *out on success, 0 on a malformed string. */
int pmm_parse_size(const char *s, uint64_t *out);

/* Loop steps (orders visited) by the last alloc_pages/free_pages call.
 * Host tests check it never passes PAGE_MAX_ORDER + 1 (ADR-0019). */
uint32_t pmm_last_steps(void);

#endif /* ARCHOS_MM_PMM_H */
