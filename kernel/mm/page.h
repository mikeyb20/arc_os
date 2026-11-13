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
#define PG_SLAB       (1u << 9)    /* every page of a slab (mm/slab.c); slab_cache names the owner */
/* Bits 10-31 are free for later blocks (page cache M1-12).
 * free_pages() clears every flag of the head page; owners of a multi-page
 * block clear what they set on its other pages before freeing it. */

struct page {
    uint32_t flags;      /* PG_* bits and the block order */
    uint32_t refcount;   /* 0 = free or reserved; alloc_pages() sets 1 on the head */
    union {
        struct {
            uint32_t mapcount;    /* page-table mappings (M1-07) */
            uint32_t index;       /* page index in its mapping (page cache, M1-12) */
        };
        struct {                  /* slab head page (M1-02b); never mapped to user space */
            uint16_t slab_inuse;  /* objects handed out */
            uint16_t slab_fresh;  /* objects never used yet: [slab_fresh, per slab) */
            uint32_t slab_free;   /* byte offset of the first freed object, or SLAB_FREE_NONE */
        };
    };
    union {
        void *mapping;     /* page cache address space or anon owner (M1-07, M1-12) */
        void *slab_cache;  /* owning slab cache, on every page of a slab (M1-02b) */
    };
    struct {
        uint32_t next;  /* PFN of the next page, or PFN_NONE */
        uint32_t prev;  /* PFN of the previous page, or PFN_NONE */
    } lru;              /* buddy free lists, slab lists (M1-02b), LRU lists later (M1-14) */
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

/* A doubly linked list of block heads through their lru PFN links. Every
 * operation is O(1). A page is on at most one such list at a time. */
struct page_list {
    uint32_t head;   /* PFN of the first page, or PFN_NONE */
};

#define PAGE_LIST_INIT \
    { PFN_NONE }

/* True if the list has no pages. */
static inline int page_list_empty(const struct page_list *l) {
    return l->head == PFN_NONE;
}

/* First page of the list, or NULL. */
static inline struct page *page_list_first(const struct page_list *l) {
    return l->head == PFN_NONE ? NULL : pfn_to_page(l->head);
}

/* Put `pg` at the front of the list. */
static inline void page_list_push(struct page_list *l, struct page *pg) {
    uint32_t pfn = (uint32_t)page_to_pfn(pg);
    pg->lru.prev = PFN_NONE;
    pg->lru.next = l->head;
    if (l->head != PFN_NONE) pfn_to_page(l->head)->lru.prev = pfn;
    l->head = pfn;
}

/* Take `pg` off the list it is on (which must be `l`). Its links are reset. */
static inline void page_list_remove(struct page_list *l, struct page *pg) {
    if (pg->lru.prev != PFN_NONE) {
        pfn_to_page(pg->lru.prev)->lru.next = pg->lru.next;
    } else {
        l->head = pg->lru.next;
    }
    if (pg->lru.next != PFN_NONE) pfn_to_page(pg->lru.next)->lru.prev = pg->lru.prev;
    pg->lru.next = PFN_NONE;
    pg->lru.prev = PFN_NONE;
}

/* Order stored in a block head. */
static inline unsigned page_order(const struct page *pg) {
    return pg->flags & PG_ORDER_MASK;
}

/* Smallest buddy order whose block holds size bytes (size >= 1). Callers check
 * the result against PAGE_MAX_ORDER. */
static inline unsigned page_order_for(size_t size) {
    uint64_t pages_minus_1 = ((uint64_t)size - 1) >> PAGE_SHIFT;
    return pages_minus_1 == 0 ? 0 : (unsigned)(64 - __builtin_clzll(pages_minus_1));
}

#endif /* ARCHOS_MM_PAGE_H */
