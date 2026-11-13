#include "mm/pmm.h"
#include "mm/page.h"
#include "boot/bootinfo.h"
#include "lib/mem.h"
#include "lib/kprintf.h"
#include "lib/cmdline.h"
#include "proc/spinlock.h"

/* Physical memory manager: page descriptor array + buddy allocator (M1-02).
 * See pmm.h for the model. Invariants, all under pmm_lock:
 *   - A free block of 2^k pages has PG_BUDDY | k on its head page, refcount 0,
 *     and sits on free list k of its zone. Its other pages have flags 0.
 *   - An allocated block has order k and refcount >= 1 on its head page.
 *   - Reserved pages (PG_RESERVED) are never on a list and never freed.
 *   - Free pages are never written: all allocator state is in the descriptors,
 *     so the allocator works the same whether or not a page is mapped. */

#define PMM_LOW_RESERVED_PFN 256u            /* memory below 1 MiB stays reserved */
#define PMM_MAX_PFN          0xFFFFFFFFull   /* 32-bit PFN links: RAM up to 16 TiB */
#define PMM_DMA32_PFN        0x100000ull     /* first PFN at 4 GiB */
#define PMM_MIN_CAP          (16ull * 1024 * 1024)  /* smallest accepted arc.mem */

/* Zones. A buddy block (at most 4 MiB, size-aligned) never crosses 4 GiB, so a
 * block's zone is fixed by its PFN and merging never mixes zones. */
#define ZONE_DMA32  0
#define ZONE_NORMAL 1
#define NR_ZONES    2

typedef struct {
    struct page_list free[PAGE_MAX_ORDER + 1];  /* free blocks of each order */
    uint64_t count[PAGE_MAX_ORDER + 1];  /* free blocks of each order */
    uint32_t nonempty;                   /* bit k set when list k is not empty */
} FreeArea;

/* A usable PFN range [start, end) from the memory map. */
typedef struct {
    uint64_t start;
    uint64_t end;
} PfnRange;

struct page *pmm_page_array;
uint64_t pmm_max_pfn;
uint64_t pmm_hhdm_offset;

/* Leaf lock (work-blocks.md §7 level 10), IRQ-safe: nothing else is taken
 * while it is held, and allocation may happen with interrupts off. */
static Spinlock pmm_lock = SPINLOCK_INIT(LOCK_LEVEL_BUDDY, "buddy");

static FreeArea zones[NR_ZONES];
static uint64_t total_pages;
static uint64_t free_page_count;
static uint64_t descriptor_bytes;
static uint64_t descriptor_phys;
static uint32_t last_steps;

static PfnRange ranges[BOOTINFO_MAX_MEMMAP_ENTRIES];
static uint64_t range_count;

/* --- Free lists --- */

static inline unsigned pfn_zone(uint64_t pfn) {
    return pfn < PMM_DMA32_PFN ? ZONE_DMA32 : ZONE_NORMAL;
}

/* Put the block at `pfn` on free list `order` and mark it as a free head. */
static void list_add(uint64_t pfn, unsigned order) {
    FreeArea *z = &zones[pfn_zone(pfn)];
    struct page *pg = pfn_to_page(pfn);

    pg->flags = PG_BUDDY | order;
    pg->refcount = 0;
    page_list_push(&z->free[order], pg);
    z->nonempty |= 1u << order;
    z->count[order]++;
}

/* Take the block at `pfn` off free list `order`. Its head flags are cleared. */
static void list_del(uint64_t pfn, unsigned order) {
    FreeArea *z = &zones[pfn_zone(pfn)];
    struct page *pg = pfn_to_page(pfn);

    page_list_remove(&z->free[order], pg);
    if (page_list_empty(&z->free[order])) {
        z->nonempty &= ~(1u << order);
    }
    z->count[order]--;
    pg->flags = 0;
}

/* True if `pfn` is the head of a free block of exactly `order`. */
static inline int is_free_head(uint64_t pfn, unsigned order) {
    if (pfn >= pmm_max_pfn) return 0;
    uint32_t f = pfn_to_page(pfn)->flags;
    return (f & PG_BUDDY) && (f & PG_ORDER_MASK) == order;
}

/* Return a block to the free lists, merging with free buddies.
 * The caller cleared the head's flags. Lock held. At most one step per order. */
static void free_block(uint64_t pfn, unsigned order) {
    uint32_t steps = 1;
    while (order < PAGE_MAX_ORDER) {
        uint64_t buddy = pfn ^ (1ull << order);
        if (!is_free_head(buddy, order)) break;
        list_del(buddy, order);
        pfn &= ~(1ull << order);   /* merged block starts at the lower half */
        order++;
        steps++;
    }
    list_add(pfn, order);
    last_steps = steps;
}

/* Take a block of 2^order pages. Returns its PFN, or PFN_NONE. Lock held.
 * The head is left with flags = order, refcount = 1. */
static uint64_t alloc_block(unsigned order, uint32_t gfp) {
    static const unsigned normal_first[NR_ZONES] = {ZONE_NORMAL, ZONE_DMA32};
    static const unsigned dma32_only[1] = {ZONE_DMA32};
    const unsigned *try_zones = (gfp & GFP_DMA32) ? dma32_only : normal_first;
    unsigned nzones = (gfp & GFP_DMA32) ? 1 : NR_ZONES;

    for (unsigned i = 0; i < nzones; i++) {
        FreeArea *z = &zones[try_zones[i]];
        uint32_t fit = z->nonempty & ~((1u << order) - 1u);
        if (fit == 0) continue;

        unsigned have = (unsigned)__builtin_ctz(fit);   /* smallest order that fits */
        uint64_t pfn = z->free[have].head;
        list_del(pfn, have);

        uint32_t steps = 1;
        while (have > order) {        /* split: keep the lower half, free the upper */
            have--;
            list_add(pfn + (1ull << have), have);
            steps++;
        }
        last_steps = steps;

        struct page *pg = pfn_to_page(pfn);
        pg->flags = order;
        pg->refcount = 1;
        pg->mapcount = 0;
        pg->index = 0;
        pg->mapping = NULL;
        free_page_count -= 1ull << order;
        return pfn;
    }
    last_steps = 1;
    return PFN_NONE;
}

/* --- Public allocator --- */

struct page *alloc_pages(unsigned order, uint32_t flags) {
    uint64_t irqf;
    if (order > PAGE_MAX_ORDER) return NULL;

    spin_lock_irqsave(&pmm_lock, &irqf);
    uint64_t pfn = alloc_block(order, flags);
    spin_unlock_irqrestore(&pmm_lock, irqf);

    if (pfn == PFN_NONE) return NULL;
    struct page *pg = pfn_to_page(pfn);
    if (flags & GFP_ZERO) {
        memset(page_to_virt(pg), 0, (size_t)PAGE_SIZE << order);
    }
    return pg;
}

void free_pages(struct page *page, unsigned order) {
    uint64_t irqf;
    if (page == NULL) return;

    uint64_t pfn = page_to_pfn(page);
    const char *bad = NULL;

    spin_lock_irqsave(&pmm_lock, &irqf);
    if (pfn >= pmm_max_pfn || order > PAGE_MAX_ORDER) {
        bad = "outside the page array";
    } else if (page->flags & PG_RESERVED) {
        bad = "reserved page";
    } else if ((page->flags & PG_BUDDY) || page->refcount == 0) {
        bad = "page not allocated (double free?)";
    } else if (page_order(page) != order) {
        bad = "order mismatch";
    } else if (--page->refcount == 0) {
        page->flags = 0;
        free_page_count += 1ull << order;
        free_block(pfn, order);
    }
    spin_unlock_irqrestore(&pmm_lock, irqf);

    if (bad) {
        kprintf("[PMM] WARNING: bad free of PFN 0x%lx order %u: %s\n", pfn, order, bad);
    }
}

/* --- Compatibility wrappers --- */

uint64_t pmm_alloc_page(void) {
    struct page *pg = alloc_pages(0, GFP_KERNEL);
    return pg ? page_to_phys(pg) : 0;
}

void pmm_free_page(uint64_t phys_addr) {
    if (phys_addr == 0) return;
    uint64_t pfn = phys_addr >> PAGE_SHIFT;
    if (pfn >= pmm_max_pfn) {
        kprintf("[PMM] WARNING: free of 0x%lx outside the page array\n", phys_addr);
        return;
    }
    free_pages(pfn_to_page(pfn), 0);
}

/* Largest order a block at `pfn` can have and still end at or before `end`. */
static unsigned max_block_order(uint64_t pfn, uint64_t end) {
    unsigned o = PAGE_MAX_ORDER;
    while (o > 0 && ((pfn & ((1ull << o) - 1)) != 0 || pfn + (1ull << o) > end)) {
        o--;
    }
    return o;
}

uint64_t pmm_alloc_contiguous(size_t count) {
    uint64_t irqf;
    if (count == 0) return 0;
    unsigned order = 0;
    while ((1ull << order) < count) order++;
    if (order > PAGE_MAX_ORDER) return 0;

    spin_lock_irqsave(&pmm_lock, &irqf);
    uint64_t pfn = alloc_block(order, GFP_KERNEL);
    if (pfn == PFN_NONE) {
        spin_unlock_irqrestore(&pmm_lock, irqf);
        return 0;
    }
    pfn_to_page(pfn)->flags = 0;   /* the head is an order-0 page too */
    /* Each of the `count` pages becomes its own order-0 page (refcount 1), so
     * callers can free them one at a time. The unused tail goes back to the
     * free lists in the largest aligned pieces. */
    for (uint64_t i = 1; i < count; i++) {
        struct page *pg = pfn_to_page(pfn + i);
        pg->flags = 0;
        pg->refcount = 1;
        pg->mapcount = 0;
        pg->index = 0;
        pg->mapping = NULL;
    }
    uint64_t end = pfn + (1ull << order);
    for (uint64_t p = pfn + count; p < end;) {
        unsigned o = max_block_order(p, end);
        pfn_to_page(p)->flags = 0;
        free_block(p, o);
        free_page_count += 1ull << o;
        p += 1ull << o;
    }
    spin_unlock_irqrestore(&pmm_lock, irqf);
    return pfn << PAGE_SHIFT;
}

/* --- Statistics --- */

uint64_t pmm_get_total_pages(void) {
    return total_pages;
}

uint64_t pmm_get_free_pages(void) {
    return free_page_count;
}

uint64_t pmm_get_descriptor_bytes(void) {
    return descriptor_bytes;
}

void pmm_get_free_blocks(uint64_t *out) {
    uint64_t irqf;
    spin_lock_irqsave(&pmm_lock, &irqf);
    for (unsigned o = 0; o <= PAGE_MAX_ORDER; o++) {
        out[o] = zones[ZONE_DMA32].count[o] + zones[ZONE_NORMAL].count[o];
    }
    spin_unlock_irqrestore(&pmm_lock, irqf);
}

uint32_t pmm_last_steps(void) {
    return last_steps;
}

void pmm_dump_stats(void) {
    uint64_t blocks[PAGE_MAX_ORDER + 1];
    pmm_get_free_blocks(blocks);
    kprintf("[PMM] Free blocks per order:");
    for (unsigned o = 0; o <= PAGE_MAX_ORDER; o++) {
        kprintf(" %u:%lu", o, blocks[o]);
    }
    kprintf("\n[PMM] Free: %lu pages (%lu MiB) of %lu\n", free_page_count, (free_page_count * PAGE_SIZE) >> 20,
            total_pages);
}

/* --- Boot --- */

int pmm_parse_size(const char *s, uint64_t *out) {
    if (s == NULL || *s < '0' || *s > '9') return 0;
    uint64_t v = 0;
    while (*s >= '0' && *s <= '9') {
        uint64_t d = (uint64_t)(*s - '0');
        if (v > (UINT64_MAX - d) / 10) return 0;
        v = v * 10 + d;
        s++;
    }
    unsigned shift = 0;
    switch (*s) {
    case '\0':
        break;
    case 'K':
    case 'k':
        shift = 10;
        s++;
        break;
    case 'M':
    case 'm':
        shift = 20;
        s++;
        break;
    case 'G':
    case 'g':
        shift = 30;
        s++;
        break;
    case 'T':
    case 't':
        shift = 40;
        s++;
        break;
    default:
        return 0;
    }
    if (*s != '\0') return 0;
    if (shift && v > (UINT64_MAX >> shift)) return 0;
    *out = v << shift;
    return 1;
}

/* Collect usable RAM as PFN ranges sorted by address, clipped to
 * [1 MiB, 16 TiB), then cut to `cap_bytes` of RAM (0 = no cap). */
static void build_ranges(const BootInfo *info, uint64_t cap_bytes) {
    range_count = 0;
    for (uint64_t i = 0; i < info->memory_map_count; i++) {
        const MemoryMapEntry *e = &info->memory_map[i];
        if (e->type != MEMMAP_USABLE) continue;
        uint64_t start = PAGE_ALIGN_UP(e->base) >> PAGE_SHIFT;
        uint64_t end = (e->base + e->length) >> PAGE_SHIFT;
        if (start < PMM_LOW_RESERVED_PFN) start = PMM_LOW_RESERVED_PFN;
        if (end > PMM_MAX_PFN) end = PMM_MAX_PFN;
        if (start >= end) continue;

        /* Insertion sort: boot only, at most BOOTINFO_MAX_MEMMAP_ENTRIES */
        uint64_t j = range_count++;
        while (j > 0 && ranges[j - 1].start > start) {
            ranges[j] = ranges[j - 1];
            j--;
        }
        ranges[j].start = start;
        ranges[j].end = end;
    }

    if (cap_bytes == 0) return;
    uint64_t left = cap_bytes >> PAGE_SHIFT;
    for (uint64_t i = 0; i < range_count; i++) {
        uint64_t len = ranges[i].end - ranges[i].start;
        if (len >= left) {
            ranges[i].end = ranges[i].start + left;
            range_count = (left > 0) ? i + 1 : i;
            return;
        }
        left -= len;
    }
}

/* Pick where the descriptor array goes: the top of the highest usable range
 * that holds it (top-down, to leave memory below 4 GiB for DMA32 users).
 * Returns its first PFN and shrinks that range, or returns 0 if none fits. */
static uint64_t place_descriptor_array(uint64_t pages) {
    for (uint64_t i = range_count; i > 0; i--) {
        PfnRange *r = &ranges[i - 1];
        if (r->end - r->start >= pages) {
            r->end -= pages;
            return r->end;
        }
    }
    return 0;
}

/* Hand [start, end) to the allocator in the largest aligned blocks. */
static void free_range(uint64_t start, uint64_t end) {
    for (uint64_t pfn = start; pfn < end; pfn++) {
        pfn_to_page(pfn)->flags = 0;
    }
    for (uint64_t pfn = start; pfn < end;) {
        unsigned o = max_block_order(pfn, end);
        free_block(pfn, o);
        pfn += 1ull << o;
    }
    total_pages += end - start;
}

/* Build the array and free lists from the memory map. `cap_bytes` = arc.mem. */
static void pmm_init_with_cap(const BootInfo *info, uint64_t cap_bytes) {
    pmm_hhdm_offset = info->hhdm_offset;
    total_pages = 0;
    free_page_count = 0;
    for (unsigned z = 0; z < NR_ZONES; z++) {
        for (unsigned o = 0; o <= PAGE_MAX_ORDER; o++) {
            zones[z].free[o].head = PFN_NONE;
            zones[z].count[o] = 0;
        }
        zones[z].nonempty = 0;
    }

    build_ranges(info, cap_bytes);
    if (range_count == 0) {
        kprintf("[PMM] FATAL: no usable memory above 1 MiB\n");
        KERNEL_PANIC();
        return;
    }
    pmm_max_pfn = ranges[range_count - 1].end;

    /* Nothing may allocate before the array exists, so it is placed straight
     * from the memory map. */
    descriptor_bytes = PAGE_ALIGN_UP(pmm_max_pfn * sizeof(struct page));
    uint64_t array_pages = descriptor_bytes >> PAGE_SHIFT;
    uint64_t array_pfn = place_descriptor_array(array_pages);
    if (array_pfn == 0) {
        kprintf("[PMM] FATAL: no usable range for %lu bytes of page descriptors\n", descriptor_bytes);
        KERNEL_PANIC();
        return;
    }
    descriptor_phys = array_pfn << PAGE_SHIFT;
    pmm_page_array = (struct page *)(descriptor_phys + pmm_hhdm_offset);

    /* Every page starts reserved; usable ranges are then freed. */
    const struct page reserved = {
        .flags = PG_RESERVED,
        .refcount = 0,
        .mapcount = 0,
        .index = 0,
        .mapping = NULL,
        .lru = {PFN_NONE, PFN_NONE},
    };
    for (uint64_t pfn = 0; pfn < pmm_max_pfn; pfn++) {
        pmm_page_array[pfn] = reserved;
    }
    for (uint64_t i = 0; i < range_count; i++) {
        free_range(ranges[i].start, ranges[i].end);
    }
    free_page_count = total_pages;

    uint64_t usable_bytes = (total_pages + array_pages) * PAGE_SIZE;
    uint64_t bp = usable_bytes ? descriptor_bytes * 10000 / usable_bytes : 0;
    /* kprintf has no field widths: pad the hundredths by hand */
    kprintf("[PMM] Page descriptors: %lu x %lu B = %lu KiB at phys 0x%lx "
            "(PFN 0..0x%lx, %lu.%s%lu%% of %lu MiB usable)\n",
            pmm_max_pfn, (uint64_t)sizeof(struct page), descriptor_bytes >> 10, descriptor_phys, pmm_max_pfn - 1,
            bp / 100, (bp % 100 < 10) ? "0" : "", bp % 100, usable_bytes >> 20);
    if (cap_bytes) {
        kprintf("[PMM] arc.mem: using %lu MiB of RAM\n", usable_bytes >> 20);
    }
}

void pmm_init(const BootInfo *info) {
    uint64_t cap = 0;
    char val[32];
    if (cmdline_get(info->cmdline, "arc.mem", val, sizeof(val))) {
        if (!pmm_parse_size(val, &cap) || cap < PMM_MIN_CAP) {
            kprintf("[PMM] WARNING: ignoring arc.mem='%s' (need a size >= 16M)\n", val);
            cap = 0;
        }
    }
    pmm_init_with_cap(info, cap);
    pmm_dump_stats();
}
