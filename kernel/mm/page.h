#ifndef ARCHOS_MM_PAGE_H
#define ARCHOS_MM_PAGE_H

#include <stdint.h>
#include <stddef.h>

/* Page descriptors (ADR-0016, ADR-0019).
 *
 * Every physical page from PFN 0 up to the highest usable PFN has one
 * `struct page` in a single array that pmm_init() carves straight out of the
 * memory map, before any other allocation. The array, not a bitmap, is the
 * source of truth for page state. It is never allocated from kmalloc.
 *
 * The struct is exactly 32 bytes. That is a hard limit: a new field goes into
 * a union or a side table, never into a bigger struct (32 MiB of descriptors
 * at 4 GB of RAM, 256 MiB at 32 GB). */

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif
#define PAGE_SHIFT 12

/* Largest buddy order: 2^10 pages = 4 MiB. Order 9 = 2 MiB (a huge page). */
#define PAGE_MAX_ORDER 10
#define PAGE_ORDER_2MB 9

/* "No page" in the 32-bit PFN list links. PFN 0xFFFFFFFF would be RAM at
 * 16 TiB, which the PMM never manages (see PMM_MAX_PFN in pmm.c). */
#define PFN_NONE 0xFFFFFFFFu

/* --- page->flags --- */
#define PG_ORDER_MASK 0x0000000Fu  /* bits 0-3: order of a block head (buddy or allocated) */
#define PG_RESERVED   (1u << 4)    /* not managed by the allocator (holes, firmware, kernel, descriptors) */
#define PG_BUDDY      (1u << 5)    /* head of a free block, linked on a free list */
#define PG_HEAD       (1u << 6)    /* compound page head (reserved for 2 MiB file/anon pages, M1-08/M1-12) */
#define PG_TAIL       (1u << 7)    /* compound page tail (reserved, see PG_HEAD) */
#define PG_KMALLOC    (1u << 8)    /* head of a large kmalloc block (kfree checks it) */
/* Bits 9-31 are free for later blocks (slab M1-02b, page cache M1-12).
 * free_pages() clears every flag, so owners never need to clear their bit. */

struct page {
    uint32_t flags;      /* PG_* bits and the block order */
    uint32_t refcount;   /* 0 = free or reserved; alloc_pages() sets 1 on the head */
    uint32_t mapcount;   /* page-table mappings (M1-07) */
    uint32_t index;      /* page index in its mapping (page cache, M1-12) */
    union {
        void *mapping;     /* page cache address space or anon owner (M1-07, M1-12) */
        void *slab_cache;  /* owning slab cache (M1-02b) */
    };
    union {
        struct {
            uint32_t next;  /* PFN of the next page, or PFN_NONE */
            uint32_t prev;  /* PFN of the previous page, or PFN_NONE */
        } lru;              /* buddy free lists now; LRU lists later (M1-14) */
        uint64_t slab_free; /* slab free-list head (M1-02b) */
    };
};

_Static_assert(sizeof(struct page) == 32, "struct page must be exactly 32 bytes (ADR-0019)");

/* The descriptor array and its bounds (set once by pmm_init). */
extern struct page *pmm_page_array;
extern uint64_t pmm_max_pfn;       /* array covers PFNs [0, pmm_max_pfn) */
extern uint64_t pmm_hhdm_offset;   /* virt = phys + offset */

/* True if `pfn` has a descriptor. */
static inline int pfn_valid(uint64_t pfn) {
    return pfn < pmm_max_pfn;
}

/* Descriptor for a PFN. O(1). The PFN must be valid. */
static inline struct page *pfn_to_page(uint64_t pfn) {
    return &pmm_page_array[pfn];
}

/* PFN of a descriptor. O(1). */
static inline uint64_t page_to_pfn(const struct page *pg) {
    return (uint64_t)(pg - pmm_page_array);
}

/* Physical address of a page. */
static inline uint64_t page_to_phys(const struct page *pg) {
    return page_to_pfn(pg) << PAGE_SHIFT;
}

/* Kernel virtual address of a page (through the HHDM). */
static inline void *page_to_virt(const struct page *pg) {
    return (void *)(page_to_phys(pg) + pmm_hhdm_offset);
}

/* Descriptor for an HHDM virtual address. The address must be in the HHDM
 * and its PFN valid. */
static inline struct page *virt_to_page(const void *virt) {
    return pfn_to_page(((uint64_t)(uintptr_t)virt - pmm_hhdm_offset) >> PAGE_SHIFT);
}

/* Order stored in a block head. */
static inline unsigned page_order(const struct page *pg) {
    return pg->flags & PG_ORDER_MASK;
}

#endif /* ARCHOS_MM_PAGE_H */
