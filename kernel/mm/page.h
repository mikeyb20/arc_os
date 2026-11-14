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
/* Page cache (M1-12, mm/filemap.h). A file page keeps PG_FILE until it is
 * freed, so code that unmaps it knows it is a cache page even after truncate
 * took it out of its file (mapping is NULL from then on). The others change
 * only with atomic or/and while the page holds a reference. */
#define PG_FILE       (1u << 10)   /* came from a page cache (never an anonymous user page) */
#define PG_UPTODATE   (1u << 11)   /* contents valid */
#define PG_DIRTY      (1u << 12)   /* changed since the last write-back */
#define PG_LOCKED     (1u << 13)   /* page lock (LOCK_LEVEL_PAGE): I/O, truncate, mapping */
#define PG_ERROR      (1u << 14)   /* the last read failed */
#define PG_LRU        (1u << 15)   /* on the evictable-page list (filemap.c) */
/* Bits 16-23: pieces of a page-cache read still in flight, plus one for the
 * reader while it is still adding pieces (filemap_io_begin / _end). */
#define PG_IO_SHIFT   16
#define PG_IO_ONE     (1u << PG_IO_SHIFT)
#define PG_IO_MASK    (0xFFu << PG_IO_SHIFT)
/* Reclaim (M1-14, mm/reclaim.c): which LRU list a cache page is on, and
 * whether read() found it in the cache since reclaim last looked */
#define PG_ACTIVE     (1u << 24)   /* on the active list (else the inactive one), while PG_LRU */
#define PG_REFERENCED (1u << 25)   /* used again since the last scan (read hits; PTE accessed bits are separate) */
#define PG_LOCK_META  (1u << 26)   /* page lock held at the block-device page level (filemap.c's lock-order record) */
/* Bits 27-31 are free for later blocks.
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
    } lru;              /* buddy free lists, slab lists (M1-02b), the page-cache LRU lists (M1-14) */
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
 * operation is O(1). A page is on at most one such list at a time. The tail
 * is the oldest page (the LRU lists take from it, M1-14). */
struct page_list {
    uint32_t head;   /* PFN of the first page, or PFN_NONE */
    uint32_t tail;   /* PFN of the last page, or PFN_NONE */
};

#define PAGE_LIST_INIT \
    { PFN_NONE, PFN_NONE }

/* True if the list has no pages. */
static inline int page_list_empty(const struct page_list *l) {
    return l->head == PFN_NONE;
}

/* First page of the list, or NULL. */
static inline struct page *page_list_first(const struct page_list *l) {
    return l->head == PFN_NONE ? NULL : pfn_to_page(l->head);
}

/* Last (oldest) page of the list, or NULL. */
static inline struct page *page_list_last(const struct page_list *l) {
    return l->tail == PFN_NONE ? NULL : pfn_to_page(l->tail);
}

/* Put `pg` at the front of the list. */
static inline void page_list_push(struct page_list *l, struct page *pg) {
    uint32_t pfn = (uint32_t)page_to_pfn(pg);
    pg->lru.prev = PFN_NONE;
    pg->lru.next = l->head;
    if (l->head != PFN_NONE)
        pfn_to_page(l->head)->lru.prev = pfn;
    else
        l->tail = pfn;
    l->head = pfn;
}

/* Take `pg` off the list it is on (which must be `l`). Its links are reset. */
static inline void page_list_remove(struct page_list *l, struct page *pg) {
    if (pg->lru.prev != PFN_NONE) {
        pfn_to_page(pg->lru.prev)->lru.next = pg->lru.next;
    } else {
        l->head = pg->lru.next;
    }
    if (pg->lru.next != PFN_NONE)
        pfn_to_page(pg->lru.next)->lru.prev = pg->lru.prev;
    else
        l->tail = pg->lru.prev;
    pg->lru.next = PFN_NONE;
    pg->lru.prev = PFN_NONE;
}

/* --- References (atomic). A page's memory is freed when its count reaches
 * 0 (free_pages drops one). Take a new reference only while holding one, or
 * under a lock that keeps the page from being freed (the page-cache index
 * lock for a page in a file). --- */

/* Take another reference to a page the caller can already reach safely. */
static inline void page_ref_get(struct page *pg) {
    __atomic_add_fetch(&pg->refcount, 1, __ATOMIC_RELAXED);
}

/* Current reference count (a snapshot). */
static inline uint32_t page_ref_count(const struct page *pg) {
    return __atomic_load_n(&pg->refcount, __ATOMIC_ACQUIRE);
}

/* Atomic flag bits other than the order and PG_RESERVED/BUDDY/SLAB */
static inline int page_test(const struct page *pg, uint32_t bit) {
    return (__atomic_load_n(&pg->flags, __ATOMIC_ACQUIRE) & bit) != 0;
}
static inline void page_set(struct page *pg, uint32_t bit) {
    __atomic_or_fetch(&pg->flags, bit, __ATOMIC_RELEASE);
}
static inline void page_clear(struct page *pg, uint32_t bit) {
    __atomic_and_fetch(&pg->flags, ~bit, __ATOMIC_RELEASE);
}
/* Set bit; returns 1 if it was already set. */
static inline int page_test_and_set(struct page *pg, uint32_t bit) {
    return (__atomic_fetch_or(&pg->flags, bit, __ATOMIC_ACQUIRE) & bit) != 0;
}
/* Clear bit; returns 1 if it was set. */
static inline int page_test_and_clear(struct page *pg, uint32_t bit) {
    return (__atomic_fetch_and(&pg->flags, ~bit, __ATOMIC_ACQ_REL) & bit) != 0;
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
