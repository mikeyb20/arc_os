/* arc_os — Host-side tests for kernel/mm/pmm.c (page descriptors + buddy allocator, M1-02)
 *
 * The buddy allocator keeps all of its state in the descriptor array and never
 * writes to free pages, so a fake memory map only needs host memory behind the
 * descriptor array itself. setup() mirrors pmm_init's placement to point the
 * fake HHDM at a host buffer. GFP_ZERO is never used here (it would touch the
 * unbacked "physical" pages). */

#include "test_framework.h"
#include <stdint.h>
#include <stdlib.h>

/* Guard headers that conflict with the host environment or need stubbing */
#define ARCHOS_LIB_KPRINTF_H
#define ARCHOS_LIB_MEM_H
static int kprintf_calls;
static inline void kprintf(const char *fmt, ...) {
    (void)fmt;
    kprintf_calls++;
}
static int panics;
#define KERNEL_PANIC() \
    do {               \
        panics++;      \
    } while (0)

#define ARCHOS_PROC_SPINLOCK_H
typedef struct { volatile uint32_t locked; uint64_t saved_flags; } Spinlock;
#define SPINLOCK_INIT { .locked = 0, .saved_flags = 0 }
static inline void spinlock_acquire(Spinlock *l) {
    l->locked = 1;
}
static inline void spinlock_release(Spinlock *l) {
    l->locked = 0;
}

/* cmdline_get stub: returns `stub_arc_mem` for "arc.mem" when set */
#define ARCHOS_LIB_CMDLINE_H
static const char *stub_arc_mem;
static int cmdline_get(const char *line, const char *key, char *out, size_t outsz) {
    (void)line;
    if (stub_arc_mem == NULL || strcmp(key, "arc.mem") != 0) return 0;
    snprintf(out, outsz, "%s", stub_arc_mem);
    return 1;
}

#include "../kernel/mm/pmm.c"

#define MiB (1024ull * 1024)
#define GiB (1024ull * MiB)

static BootInfo boot;
static void *arena;

/* Initialize the PMM over `n` memory map entries with RAM cap `cap` (0 = none).
 * When `via_cmdline` is set, the cap comes from the arc.mem stub instead. */
static void setup_map(const MemoryMapEntry *map, int n, uint64_t cap, const char *arc_mem) {
    memset(&boot, 0, sizeof(boot));
    for (int i = 0; i < n; i++) boot.memory_map[i] = map[i];
    boot.memory_map_count = (uint64_t)n;

    /* Same placement pmm_init will choose */
    uint64_t eff_cap = cap;
    if (arc_mem && (!pmm_parse_size(arc_mem, &eff_cap) || eff_cap < PMM_MIN_CAP)) eff_cap = 0;
    build_ranges(&boot, eff_cap);
    uint64_t max = ranges[range_count - 1].end;
    uint64_t bytes = PAGE_ALIGN_UP(max * sizeof(struct page));
    uint64_t pfn = place_descriptor_array(bytes >> PAGE_SHIFT);

    free(arena);
    arena = aligned_alloc(PAGE_SIZE, bytes);
    boot.hhdm_offset = (uint64_t)(uintptr_t)arena - (pfn << PAGE_SHIFT);
    panics = 0;

    if (arc_mem) {
        stub_arc_mem = arc_mem;
        pmm_init(&boot);
        stub_arc_mem = NULL;
    } else {
        pmm_init_with_cap(&boot, cap);
    }
}

/* One usable range [base, base + len) */
static void setup_one(uint64_t base, uint64_t len) {
    MemoryMapEntry m = {base, len, MEMMAP_USABLE};
    setup_map(&m, 1, 0, NULL);
}

/* 64 MiB of RAM from 0, as a small machine */
static void setup_small(void) {
    setup_one(0, 64 * MiB);
}

/* q35 with -m 6G: 2 GiB below the PCI hole, the rest from 4 GiB */
static const MemoryMapEntry q35_6g[] = {
    {0x0, 0x9F000, MEMMAP_USABLE},
    {0x9F000, 0x61000, MEMMAP_RESERVED},
    {0x100000, 2 * GiB - 0x100000, MEMMAP_USABLE},
    {2 * GiB, 2 * GiB, MEMMAP_RESERVED},
    {4 * GiB, 4 * GiB, MEMMAP_USABLE},
};

static int blocks_equal(const uint64_t *a, const uint64_t *b) {
    for (unsigned o = 0; o <= PAGE_MAX_ORDER; o++) {
        if (a[o] != b[o]) return 0;
    }
    return 1;
}

/* --- Layout and boot --- */

static int test_struct_page_32_bytes(void) {
    ASSERT_EQ(sizeof(struct page), 32);
    return 0;
}

static int test_pfn_math(void) {
    setup_small();
    struct page *pg = pfn_to_page(1000);
    ASSERT_EQ(page_to_pfn(pg), 1000);
    ASSERT_EQ(page_to_phys(pg), 1000ull * PAGE_SIZE);
    ASSERT_TRUE(virt_to_page(page_to_virt(pg)) == pg);
    ASSERT_TRUE(pfn_valid(pmm_max_pfn - 1));
    ASSERT_FALSE(pfn_valid(pmm_max_pfn));
    return 0;
}

static int test_parse_size(void) {
    uint64_t v = 0;
    ASSERT_TRUE(pmm_parse_size("4G", &v));
    ASSERT_EQ(v, 4 * GiB);
    ASSERT_TRUE(pmm_parse_size("512M", &v));
    ASSERT_EQ(v, 512 * MiB);
    ASSERT_TRUE(pmm_parse_size("65536k", &v));
    ASSERT_EQ(v, 64 * MiB);
    ASSERT_TRUE(pmm_parse_size("4096", &v));
    ASSERT_EQ(v, 4096);
    ASSERT_TRUE(pmm_parse_size("1T", &v));
    ASSERT_EQ(v, 1024 * GiB);
    ASSERT_FALSE(pmm_parse_size("", &v));
    ASSERT_FALSE(pmm_parse_size("G", &v));
    ASSERT_FALSE(pmm_parse_size("4X", &v));
    ASSERT_FALSE(pmm_parse_size("4GB", &v));
    ASSERT_FALSE(pmm_parse_size("-1", &v));
    ASSERT_FALSE(pmm_parse_size("99999999999999999999", &v));
    ASSERT_FALSE(pmm_parse_size("99999999T", &v));
    return 0;
}

static int test_init_counts(void) {
    setup_one(1 * MiB, 64 * MiB);   /* RAM 1 MiB .. 65 MiB */
    uint64_t max = (65 * MiB) >> PAGE_SHIFT;
    uint64_t array_pages = PAGE_ALIGN_UP(max * 32) >> PAGE_SHIFT;
    ASSERT_EQ(pmm_max_pfn, max);
    ASSERT_EQ(pmm_get_descriptor_bytes(), array_pages * PAGE_SIZE);
    ASSERT_EQ(pmm_get_total_pages(), ((64 * MiB) >> PAGE_SHIFT) - array_pages);
    ASSERT_EQ(pmm_get_free_pages(), pmm_get_total_pages());
    /* Array at the top of RAM, its pages reserved */
    ASSERT_EQ(descriptor_phys, (max - array_pages) << PAGE_SHIFT);
    ASSERT_TRUE(pfn_to_page(max - 1)->flags & PG_RESERVED);
    ASSERT_EQ(panics, 0);
    return 0;
}

static int test_low_memory_reserved(void) {
    setup_small();
    for (uint64_t pfn = 0; pfn < 256; pfn++) {
        ASSERT_TRUE(pfn_to_page(pfn)->flags & PG_RESERVED);
    }
    uint64_t n = 0;
    struct page *pg;
    while ((pg = alloc_pages(0, 0)) != NULL) {
        ASSERT_TRUE(page_to_pfn(pg) >= 256);
        n++;
    }
    ASSERT_EQ(n, pmm_get_total_pages());
    return 0;
}

static int test_holes(void) {
    /* Usable 1..3 MiB, hole 3..5 MiB, usable from an unaligned base to 40 MiB */
    const MemoryMapEntry map[] = {
        {1 * MiB, 2 * MiB, MEMMAP_USABLE},
        {3 * MiB, 2 * MiB, MEMMAP_RESERVED},
        {5 * MiB + 0x800, 35 * MiB - 0x800, MEMMAP_USABLE},
    };
    setup_map(map, 3, 0, NULL);
    uint64_t hole_lo = (3 * MiB) >> PAGE_SHIFT;
    uint64_t hole_hi = ((5 * MiB) >> PAGE_SHIFT) + 1;   /* partial page at 5 MiB is not usable */
    for (uint64_t pfn = hole_lo; pfn < hole_hi; pfn++) {
        ASSERT_TRUE(pfn_to_page(pfn)->flags & PG_RESERVED);
    }

    /* Freeing a hole page is refused */
    uint64_t before = pmm_get_free_pages();
    free_pages(pfn_to_page(hole_lo), 0);
    ASSERT_EQ(pmm_get_free_pages(), before);

    /* Nothing allocated from the hole, in any order */
    uint64_t got = 0;
    for (int order = PAGE_MAX_ORDER; order >= 0; order--) {
        struct page *pg;
        while ((pg = alloc_pages((unsigned)order, 0)) != NULL) {
            uint64_t s = page_to_pfn(pg), e = s + (1ull << order);
            ASSERT_TRUE(e <= hole_lo || s >= hole_hi);
            ASSERT_EQ(s & ((1ull << order) - 1), 0);   /* size-aligned */
            got += 1ull << order;
        }
    }
    ASSERT_EQ(got, pmm_get_total_pages());
    ASSERT_EQ(pmm_get_free_pages(), 0);
    return 0;
}

/* --- Buddy behaviour --- */

static int test_split_and_coalesce(void) {
    setup_small();
    uint64_t initial[PAGE_MAX_ORDER + 1], now[PAGE_MAX_ORDER + 1];
    pmm_get_free_blocks(initial);

    struct page *a = alloc_pages(0, 0);
    struct page *b = alloc_pages(3, 0);
    struct page *c = alloc_pages(PAGE_ORDER_2MB, 0);
    ASSERT_TRUE(a && b && c);
    ASSERT_EQ(page_order(b), 3);
    ASSERT_EQ(b->refcount, 1);
    ASSERT_EQ(page_to_pfn(c) & 511, 0);
    pmm_get_free_blocks(now);
    ASSERT_FALSE(blocks_equal(initial, now));

    free_pages(b, 3);
    free_pages(a, 0);
    free_pages(c, PAGE_ORDER_2MB);
    pmm_get_free_blocks(now);
    ASSERT_TRUE(blocks_equal(initial, now));   /* everything merged back */
    ASSERT_EQ(pmm_get_free_pages(), pmm_get_total_pages());
    return 0;
}

static int test_order9_after_fragmentation(void) {
    setup_small();
    uint64_t initial[PAGE_MAX_ORDER + 1], now[PAGE_MAX_ORDER + 1];
    pmm_get_free_blocks(initial);
    uint64_t total = pmm_get_total_pages();

    /* Take every page one at a time, then free every other one: half the RAM
     * is free, but no two free pages are buddies, so no 2 MiB block exists */
    struct page **all = malloc(total * sizeof(*all));
    uint64_t n = 0;
    struct page *pg;
    while ((pg = alloc_pages(0, 0)) != NULL) all[n++] = pg;
    ASSERT_EQ(n, total);
    for (uint64_t i = 0; i < n; i++) {
        if (page_to_pfn(all[i]) & 1) {
            free_pages(all[i], 0);
            all[i] = NULL;
        }
    }
    ASSERT_TRUE(pmm_get_free_pages() >= total / 2 - 1);
    ASSERT_TRUE(alloc_pages(1, 0) == NULL);
    ASSERT_TRUE(alloc_pages(PAGE_ORDER_2MB, 0) == NULL);

    /* Free the even pages of one 2 MiB-aligned range (2..4 MiB: usable, below
     * the descriptor array at the top): they merge with their free odd buddies
     * all the way up, and an order-9 block appears there */
    const uint64_t base = 512;
    for (uint64_t i = 0; i < n; i++) {
        if (all[i] && page_to_pfn(all[i]) >= base && page_to_pfn(all[i]) < base + 512) {
            free_pages(all[i], 0);
            all[i] = NULL;
        }
    }
    struct page *huge = alloc_pages(PAGE_ORDER_2MB, 0);
    ASSERT_TRUE(huge != NULL);
    ASSERT_EQ(page_to_pfn(huge), base);
    free_pages(huge, PAGE_ORDER_2MB);

    for (uint64_t i = 0; i < n; i++)
        if (all[i]) free_pages(all[i], 0);
    free(all);
    pmm_get_free_blocks(now);
    ASSERT_TRUE(blocks_equal(initial, now));
    return 0;
}

/* HIGH test machine class: 32 GiB, the top 2 GiB remapped above 4 GiB. The
 * array covers PFN 0 to 34 GiB at 32 B each (ADR-0016's -m 32G check; CI
 * runners have 16 GB, so QEMU can't boot a 32G guest there). */
static int test_sizing_32g(void) {
    const MemoryMapEntry map[] = {
        {0x100000, 2 * GiB - 0x100000, MEMMAP_USABLE},
        {2 * GiB, 2 * GiB, MEMMAP_RESERVED},
        {4 * GiB, 30 * GiB, MEMMAP_USABLE},
    };
    setup_map(map, 3, 0, NULL);
    ASSERT_EQ(pmm_max_pfn, (34 * GiB) >> PAGE_SHIFT);
    ASSERT_EQ(pmm_get_descriptor_bytes(), 34 * GiB / PAGE_SIZE * 32);   /* 272 MiB */
    ASSERT_TRUE(descriptor_phys >= 4 * GiB);          /* top-down: DMA32 left alone */
    uint64_t usable = (32 * GiB - 0x100000) >> PAGE_SHIFT;
    ASSERT_EQ(pmm_get_total_pages(), usable - pmm_get_descriptor_bytes() / PAGE_SIZE);
    ASSERT_EQ(panics, 0);
    return 0;
}

static int test_exhaustion(void) {
    setup_small();
    uint64_t initial[PAGE_MAX_ORDER + 1], now[PAGE_MAX_ORDER + 1];
    pmm_get_free_blocks(initial);
    uint64_t total = pmm_get_total_pages();

    struct page **all = malloc(total * sizeof(*all));
    uint64_t n = 0;
    struct page *pg;
    while ((pg = alloc_pages(0, 0)) != NULL) all[n++] = pg;
    ASSERT_EQ(n, total);
    ASSERT_EQ(pmm_get_free_pages(), 0);
    ASSERT_TRUE(alloc_pages(0, 0) == NULL);
    ASSERT_TRUE(alloc_pages(PAGE_MAX_ORDER, 0) == NULL);
    ASSERT_EQ(pmm_alloc_page(), 0);

    /* Free in reverse order: merging must still rebuild every block */
    while (n > 0) free_pages(all[--n], 0);
    free(all);
    pmm_get_free_blocks(now);
    ASSERT_TRUE(blocks_equal(initial, now));
    ASSERT_EQ(pmm_get_free_pages(), total);
    return 0;
}

static int test_order_limits(void) {
    setup_small();
    ASSERT_TRUE(alloc_pages(PAGE_MAX_ORDER + 1, 0) == NULL);
    struct page *pg = alloc_pages(PAGE_MAX_ORDER, 0);
    ASSERT_TRUE(pg != NULL);
    ASSERT_EQ(page_to_pfn(pg) & 1023, 0);
    free_pages(pg, PAGE_MAX_ORDER);
    return 0;
}

static int test_steps_bounded(void) {
    /* ADR-0019: alloc and free take at most one step per order */
    setup_small();
    static struct page *held[512];
    static unsigned held_order[512];
    uint32_t worst = 0;
    unsigned seed = 12345;
    for (int i = 0; i < 512; i++) {
        seed = seed * 1103515245u + 12345u;
        unsigned order = (seed >> 16) % (PAGE_MAX_ORDER + 1);
        held[i] = alloc_pages(order, 0);
        held_order[i] = order;
        if (pmm_last_steps() > worst) worst = pmm_last_steps();
    }
    for (int i = 0; i < 512; i++) {
        free_pages(held[i], held_order[i]);   /* NULL (failed alloc) is ignored */
        if (pmm_last_steps() > worst) worst = pmm_last_steps();
    }
    ASSERT_TRUE(worst >= 2);                       /* some splitting and merging happened */
    ASSERT_TRUE(worst <= PAGE_MAX_ORDER + 1);
    return 0;
}

/* --- Zones and arc.mem --- */

static int test_dma32_zone(void) {
    setup_map(q35_6g, 5, 0, NULL);
    /* Descriptor array lands above 4 GiB (top-down) */
    ASSERT_TRUE(descriptor_phys >= 4 * GiB);
    ASSERT_EQ(pmm_max_pfn, (8 * GiB) >> PAGE_SHIFT);

    struct page *normal = alloc_pages(0, 0);
    struct page *low = alloc_pages(0, GFP_DMA32);
    struct page *low_big = alloc_pages(PAGE_MAX_ORDER, GFP_DMA32);
    ASSERT_TRUE(normal && low && low_big);
    ASSERT_TRUE(page_to_phys(normal) >= 4 * GiB);        /* high memory used first */
    ASSERT_TRUE(page_to_phys(low) < 4 * GiB);
    ASSERT_TRUE(page_to_phys(low_big) + (4 * MiB) <= 4 * GiB);
    free_pages(normal, 0);
    free_pages(low, 0);
    free_pages(low_big, PAGE_MAX_ORDER);
    ASSERT_EQ(pmm_get_free_pages(), pmm_get_total_pages());
    return 0;
}

static int test_dma32_exhausted(void) {
    /* Only RAM above 4 GiB (plus the reserved low MiB): DMA32 requests fail */
    const MemoryMapEntry map[] = {
        {0x0, 0x9F000, MEMMAP_USABLE},
        {4 * GiB, 64 * MiB, MEMMAP_USABLE},
    };
    setup_map(map, 2, 0, NULL);
    ASSERT_TRUE(alloc_pages(0, GFP_DMA32) == NULL);
    ASSERT_TRUE(alloc_pages(0, 0) != NULL);
    return 0;
}

static int test_arc_mem_cap(void) {
    setup_map(q35_6g, 5, 3 * GiB, NULL);
    uint64_t array_pages = pmm_get_descriptor_bytes() >> PAGE_SHIFT;
    /* Exactly 3 GiB of RAM above 1 MiB is used: 2047 MiB low + 1025 MiB high */
    ASSERT_EQ(pmm_get_total_pages() + array_pages, (3 * GiB) >> PAGE_SHIFT);
    ASSERT_EQ(pmm_max_pfn, (4 * GiB + 1025 * MiB) >> PAGE_SHIFT);
    /* Nothing above the cap is handed out */
    struct page *pg;
    while ((pg = alloc_pages(PAGE_MAX_ORDER, 0)) != NULL) {
        ASSERT_TRUE(page_to_pfn(pg) + 1024 <= pmm_max_pfn);
    }
    return 0;
}

static int test_arc_mem_cap_below_hole(void) {
    /* A cap smaller than the low range drops the high range entirely */
    setup_map(q35_6g, 5, 1 * GiB, NULL);
    ASSERT_EQ(pmm_max_pfn, (1 * GiB + 1 * MiB) >> PAGE_SHIFT);
    ASSERT_TRUE(alloc_pages(0, 0) != NULL);
    return 0;
}

static int test_arc_mem_cmdline(void) {
    setup_map(q35_6g, 5, 0, "3G");
    uint64_t array_pages = pmm_get_descriptor_bytes() >> PAGE_SHIFT;
    ASSERT_EQ(pmm_get_total_pages() + array_pages, (3 * GiB) >> PAGE_SHIFT);

    /* Malformed or tiny values are ignored: all 6 GiB used */
    setup_map(q35_6g, 5, 0, "lots");
    ASSERT_EQ(pmm_max_pfn, (8 * GiB) >> PAGE_SHIFT);
    setup_map(q35_6g, 5, 0, "4M");
    ASSERT_EQ(pmm_max_pfn, (8 * GiB) >> PAGE_SHIFT);
    return 0;
}

/* --- Bad frees and reference counts --- */

static int test_double_free_ignored(void) {
    setup_small();
    struct page *pg = alloc_pages(0, 0);
    uint64_t before = pmm_get_free_pages();
    free_pages(pg, 0);
    ASSERT_EQ(pmm_get_free_pages(), before + 1);
    kprintf_calls = 0;
    free_pages(pg, 0);
    ASSERT_EQ(pmm_get_free_pages(), before + 1);
    ASSERT_EQ(kprintf_calls, 1);   /* logged once */
    return 0;
}

static int test_bad_order_and_tail_free(void) {
    setup_small();
    struct page *pg = alloc_pages(2, 0);
    uint64_t before = pmm_get_free_pages();
    free_pages(pg, 1);                    /* wrong order */
    free_pages(pfn_to_page(page_to_pfn(pg) + 1), 0);  /* tail page */
    ASSERT_EQ(pmm_get_free_pages(), before);
    free_pages(pg, 2);
    ASSERT_EQ(pmm_get_free_pages(), before + 4);
    return 0;
}

static int test_refcount_holds_page(void) {
    setup_small();
    struct page *pg = alloc_pages(0, 0);
    pg->refcount++;                       /* a second owner (COW, M1-08) */
    uint64_t before = pmm_get_free_pages();
    free_pages(pg, 0);
    ASSERT_EQ(pmm_get_free_pages(), before);
    ASSERT_EQ(pg->refcount, 1);
    free_pages(pg, 0);
    ASSERT_EQ(pmm_get_free_pages(), before + 1);
    return 0;
}

/* --- Compatibility wrappers --- */

static int test_compat_single_pages(void) {
    setup_small();
    uint64_t before = pmm_get_free_pages();
    uint64_t a = pmm_alloc_page();
    uint64_t b = pmm_alloc_page();
    ASSERT_TRUE(a != 0 && b != 0 && a != b);
    ASSERT_EQ(a % PAGE_SIZE, 0);
    ASSERT_EQ(pmm_get_free_pages(), before - 2);
    pmm_free_page(a);
    pmm_free_page(b);
    pmm_free_page(0);                     /* ignored */
    ASSERT_EQ(pmm_get_free_pages(), before);
    return 0;
}

static int test_compat_contiguous(void) {
    setup_small();
    uint64_t initial[PAGE_MAX_ORDER + 1], now[PAGE_MAX_ORDER + 1];
    pmm_get_free_blocks(initial);
    uint64_t before = pmm_get_free_pages();

    uint64_t base = pmm_alloc_contiguous(5);
    ASSERT_TRUE(base != 0);
    ASSERT_EQ(pmm_get_free_pages(), before - 5);   /* the 3-page tail went back */
    for (int i = 0; i < 5; i++) {
        struct page *pg = pfn_to_page((base >> PAGE_SHIFT) + (uint64_t)i);
        ASSERT_EQ(pg->refcount, 1);
        ASSERT_EQ(page_order(pg), 0);
    }
    /* Freed one page at a time, as virtio-blk does */
    for (int i = 0; i < 5; i++) pmm_free_page(base + (uint64_t)i * PAGE_SIZE);
    ASSERT_EQ(pmm_get_free_pages(), before);
    pmm_get_free_blocks(now);
    ASSERT_TRUE(blocks_equal(initial, now));

    ASSERT_EQ(pmm_alloc_contiguous(0), 0);
    ASSERT_EQ(pmm_alloc_contiguous(1025), 0);      /* above 4 MiB */
    ASSERT_TRUE(pmm_alloc_contiguous(1024) != 0);
    return 0;
}

/* --- Test suite export --- */

TestCase pmm_tests[] = {
    {"struct_page_32_bytes", test_struct_page_32_bytes},
    {"pfn_math", test_pfn_math},
    {"parse_size", test_parse_size},
    {"init_counts", test_init_counts},
    {"low_memory_reserved", test_low_memory_reserved},
    {"holes", test_holes},
    {"split_and_coalesce", test_split_and_coalesce},
    {"order9_after_fragmentation", test_order9_after_fragmentation},
    {"sizing_32g", test_sizing_32g},
    {"exhaustion", test_exhaustion},
    {"order_limits", test_order_limits},
    {"steps_bounded", test_steps_bounded},
    {"dma32_zone", test_dma32_zone},
    {"dma32_exhausted", test_dma32_exhausted},
    {"arc_mem_cap", test_arc_mem_cap},
    {"arc_mem_cap_below_hole", test_arc_mem_cap_below_hole},
    {"arc_mem_cmdline", test_arc_mem_cmdline},
    {"double_free_ignored", test_double_free_ignored},
    {"bad_order_and_tail_free", test_bad_order_and_tail_free},
    {"refcount_holds_page", test_refcount_holds_page},
    {"compat_single_pages", test_compat_single_pages},
    {"compat_contiguous", test_compat_contiguous},
};

int pmm_test_count = sizeof(pmm_tests) / sizeof(pmm_tests[0]);
