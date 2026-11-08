/* arc_os — Host-side tests for kernel/mm/kmalloc.c */

#include "test_framework.h"
#include <stdint.h>

/* Guard kernel headers that conflict with host or need stubbing */
#define ARCHOS_MM_PMM_H
#define ARCHOS_MM_PAGE_H
#define ARCHOS_MM_VMM_H
#define ARCHOS_LIB_KPRINTF_H
#define ARCHOS_MM_KMALLOC_H
#define ARCHOS_LIB_MEM_H        /* Use libc memset/memcpy */
#define ARCHOS_PROC_SPINLOCK_H  /* Real spinlock uses cli/popf; stub below */

#define PAGE_SIZE 4096
#define GFP_KERNEL  0x00
#define GFP_ZERO    0x01
#define GFP_DMA32         0x02
#define KMALLOC_LARGE_MIN (2u * 4096u)
#define KMALLOC_LARGE_MAX (4096u << 10)
#define KMALLOC_CAP_BYTES (512ull * 1024 * 1024)

typedef struct {
    size_t total_used;
    size_t total_free;
    size_t total_blocks;
    size_t free_blocks;
    size_t largest_free;
    size_t heap_mapped;
    size_t large_bytes;
} HeapStats;
#define VMM_FLAG_WRITABLE 1
#define VMM_FLAG_NOEXEC   4

/* Stub spinlock that records nesting, so tests can check that every public
 * call takes the heap lock exactly once and releases it (krealloc must not
 * re-enter kmalloc/kfree through the locked wrappers). */
typedef struct {
    volatile uint32_t locked;
    uint64_t saved_flags;
} Spinlock;
#define SPINLOCK_INIT { .locked = 0, .saved_flags = 0 }
static int lock_depth;
static int lock_max_depth;
static int lock_acquires;
static inline void spinlock_acquire(Spinlock *lock) {
    lock->locked = 1;
    lock_acquires++;
    if (++lock_depth > lock_max_depth) lock_max_depth = lock_depth;
}
static inline void spinlock_release(Spinlock *lock) {
    lock->locked = 0;
    lock_depth--;
}

/* Stub kprintf */
static inline void kprintf(const char *fmt, ...) { (void)fmt; }
static int panics;
#define KERNEL_PANIC() \
    do {               \
        panics++;      \
    } while (0)

/* Static arena replaces the kernel heap region */
#define ARENA_SIZE (512 * 1024)
static uint8_t arena[ARENA_SIZE] __attribute__((aligned(4096)));

/* Stub PMM: returns incrementing fake physical addresses (static to avoid linker clash) */
static uint64_t fake_phys_next;

static uint64_t pmm_alloc_page(void) {
    uint64_t addr = fake_phys_next;
    fake_phys_next += 4096;
    return addr;
}

/* Stub page allocator for the large path: a few fake "pages", each backed by
 * its own host buffer. The descriptor array is the slot table. */
#define PAGE_SHIFT     12
#define PAGE_MAX_ORDER 10
#define PG_ORDER_MASK  0xFu
#define PG_KMALLOC     (1u << 8)
struct page {
    uint32_t flags;
    uint32_t refcount;
    void *mem;
};
#define STUB_SLOTS 16
static struct page stub_pages[STUB_SLOTS];
static int stub_allocs, stub_frees, stub_fail_next;
static uint32_t stub_last_flags;
static inline unsigned page_order(const struct page *pg) {
    return pg->flags & PG_ORDER_MASK;
}
static inline uint64_t page_to_pfn(const struct page *pg) {
    return (uint64_t)(pg - stub_pages);
}
static inline int pfn_valid(uint64_t pfn) {
    return pfn < STUB_SLOTS;
}
static inline void *page_to_virt(const struct page *pg) {
    return pg->mem;
}
static struct page *virt_to_page(const void *v) {
    for (int i = 0; i < STUB_SLOTS; i++)
        if (stub_pages[i].mem == v) return &stub_pages[i];
    return &stub_pages[STUB_SLOTS];   /* invalid: pfn_valid() fails */
}
static struct page *alloc_pages(unsigned order, uint32_t flags) {
    stub_last_flags = flags;
    if (stub_fail_next) {
        stub_fail_next = 0;
        return NULL;
    }
    for (int i = 0; i < STUB_SLOTS; i++) {
        if (stub_pages[i].refcount == 0) {
            size_t sz = (size_t)4096 << order;
            stub_pages[i].mem = aligned_alloc(4096, sz);
            memset(stub_pages[i].mem, (flags & GFP_ZERO) ? 0 : 0xEE, sz);
            stub_pages[i].flags = order;
            stub_pages[i].refcount = 1;
            stub_allocs++;
            return &stub_pages[i];
        }
    }
    return NULL;
}
static void free_pages(struct page *pg, unsigned order) {
    if (page_order(pg) != order || pg->refcount != 1) {
        stub_frees = -1000;
        return;
    }
    pg->refcount = 0;
    pg->flags = 0;
    free(pg->mem);
    pg->mem = NULL;
    stub_frees++;
}

/* Stub VMM: no-op (static to avoid linker clash) */
static void vmm_map_page(uint64_t virt, uint64_t phys, uint32_t flags) {
    (void)virt; (void)phys; (void)flags;
}

/* Include kmalloc.c — HEAP_START/HEAP_MAX will be defined as kernel addresses,
 * but we bypass kmalloc_init and manually set up the heap in the arena. */
#include "../kernel/mm/kmalloc.c"

/* Reset the heap state before each test — manually init into our arena,
 * bypassing kmalloc_init (which uses HEAP_START that points at kernel VA). */
static void setup_heap(void) {
    memset(arena, 0, ARENA_SIZE);
    fake_phys_next = 0x100000;
    large_bytes = 0;
    stub_allocs = stub_frees = stub_fail_next = 0;

    /* Manually set up a 16 KB initial heap in the arena */
    uint64_t arena_base = (uint64_t)(uintptr_t)arena;
    heap_current_end = arena_base + 4 * PAGE_SIZE;

    heap_start_block = (BlockHeader *)arena;
    heap_start_block->magic = BLOCK_MAGIC;
    heap_start_block->size  = (4 * PAGE_SIZE) - HEADER_SIZE;
    heap_start_block->free  = 1;
    heap_start_block->next  = NULL;
    heap_start_block->prev  = NULL;
}

/* --- Tests --- */

static int test_basic_alloc_free(void) {
    setup_heap();
    void *p = kmalloc(64, GFP_KERNEL);
    ASSERT_TRUE(p != NULL);
    kfree(p);
    return 0;
}

static int test_kmalloc_zero_returns_null(void) {
    setup_heap();
    void *p = kmalloc(0, GFP_KERNEL);
    ASSERT_TRUE(p == NULL);
    return 0;
}

static int test_gfp_zero_zeroes_memory(void) {
    setup_heap();
    uint8_t *p = (uint8_t *)kmalloc(128, GFP_ZERO);
    ASSERT_TRUE(p != NULL);
    for (int i = 0; i < 128; i++) {
        ASSERT_EQ(p[i], 0);
    }
    kfree(p);
    return 0;
}

static int test_split_block(void) {
    setup_heap();
    void *a = kmalloc(32, GFP_KERNEL);
    void *b = kmalloc(32, GFP_KERNEL);
    ASSERT_TRUE(a != NULL);
    ASSERT_TRUE(b != NULL);
    ASSERT_TRUE(a != b);
    kfree(a);
    kfree(b);
    return 0;
}

static int test_coalesce(void) {
    setup_heap();
    void *a = kmalloc(64, GFP_KERNEL);
    void *b = kmalloc(64, GFP_KERNEL);
    ASSERT_TRUE(a != NULL);
    ASSERT_TRUE(b != NULL);

    /* Free both — they should coalesce */
    kfree(a);
    kfree(b);

    /* Now allocate combined size — should succeed from the coalesced block */
    void *c = kmalloc(128 + 64, GFP_KERNEL);
    ASSERT_TRUE(c != NULL);
    kfree(c);
    return 0;
}

static int test_multiple_alloc_free_cycles(void) {
    setup_heap();
    void *ptrs[32];
    for (int i = 0; i < 32; i++) {
        ptrs[i] = kmalloc(64, GFP_KERNEL);
        ASSERT_TRUE(ptrs[i] != NULL);
    }
    for (int i = 0; i < 32; i++) {
        kfree(ptrs[i]);
    }
    return 0;
}

static int test_alignment(void) {
    setup_heap();
    for (size_t sz = 1; sz <= 256; sz++) {
        void *p = kmalloc(sz, GFP_KERNEL);
        ASSERT_TRUE(p != NULL);
        ASSERT_EQ((uintptr_t)p % 16, 0);
        kfree(p);
    }
    return 0;
}

static int test_krealloc_null_acts_as_kmalloc(void) {
    setup_heap();
    void *p = krealloc(NULL, 128);
    ASSERT_TRUE(p != NULL);
    kfree(p);
    return 0;
}

static int test_krealloc_zero_frees(void) {
    setup_heap();
    void *p = kmalloc(64, GFP_KERNEL);
    ASSERT_TRUE(p != NULL);
    void *q = krealloc(p, 0);
    ASSERT_TRUE(q == NULL);
    return 0;
}

static int test_krealloc_grow_preserves_data(void) {
    setup_heap();
    uint8_t *p = (uint8_t *)kmalloc(32, GFP_KERNEL);
    ASSERT_TRUE(p != NULL);
    for (int i = 0; i < 32; i++) p[i] = (uint8_t)i;

    uint8_t *q = (uint8_t *)krealloc(p, 256);
    ASSERT_TRUE(q != NULL);
    for (int i = 0; i < 32; i++) {
        ASSERT_EQ(q[i], (uint8_t)i);
    }
    kfree(q);
    return 0;
}

static int test_krealloc_shrink_preserves_data(void) {
    setup_heap();
    uint8_t *p = (uint8_t *)kmalloc(256, GFP_KERNEL);
    ASSERT_TRUE(p != NULL);
    for (int i = 0; i < 64; i++) p[i] = (uint8_t)(i + 100);

    uint8_t *q = (uint8_t *)krealloc(p, 64);
    ASSERT_TRUE(q != NULL);
    for (int i = 0; i < 64; i++) {
        ASSERT_EQ(q[i], (uint8_t)(i + 100));
    }
    kfree(q);
    return 0;
}

static int test_double_free_no_crash(void) {
    setup_heap();
    void *p = kmalloc(64, GFP_KERNEL);
    ASSERT_TRUE(p != NULL);
    kfree(p);
    /* Double free — should print warning (silenced) but not crash */
    kfree(p);
    return 0;
}

static int test_canary_survives_normal_use(void) {
    setup_heap();
    void *p = kmalloc(64, GFP_KERNEL);
    ASSERT_TRUE(p != NULL);

    /* Check that the block header magic is intact */
    uint64_t *header = (uint64_t *)((uint8_t *)p - HEADER_SIZE);
    ASSERT_EQ(*header, BLOCK_MAGIC);

    kfree(p);
    return 0;
}

static int test_split_block_no_underflow(void) {
    setup_heap();

    /* Fill heap so the last block is NOT free, forcing heap_grow path */
    size_t fill_size = (4 * PAGE_SIZE) - HEADER_SIZE - 64;
    void *fill = kmalloc(fill_size, GFP_KERNEL);
    ASSERT_TRUE(fill != NULL);

    /* Allocate a page-aligned size — this triggers the grow path where
     * the old code had: need = size + HEADER_SIZE (one header short).
     * split_block would then underflow: remaining = block->size - size - HEADER_SIZE
     * when block->size == size (grown exactly size + HEADER_SIZE bytes).
     * With the fix, need = size + 2*HEADER_SIZE, giving split_block room. */
    void *p = kmalloc(PAGE_SIZE, GFP_KERNEL);
    ASSERT_TRUE(p != NULL);

    /* Verify we can still allocate after — heap is not corrupted */
    void *q = kmalloc(64, GFP_KERNEL);
    ASSERT_TRUE(q != NULL);

    kfree(q);
    kfree(p);
    kfree(fill);
    return 0;
}

static int test_lock_taken_once_per_call(void) {
    setup_heap();
    lock_depth = 0; lock_max_depth = 0; lock_acquires = 0;

    void *p = kmalloc(64, GFP_KERNEL);
    ASSERT_EQ(lock_acquires, 1);
    ASSERT_EQ(lock_depth, 0);

    /* Grow past the neighbouring block so krealloc takes the copy path,
     * which allocates and frees internally. */
    void *blocker = kmalloc(64, GFP_KERNEL);
    lock_acquires = 0;
    void *q = krealloc(p, 4096);
    ASSERT_TRUE(q != NULL);
    ASSERT_EQ(lock_acquires, 1);
    ASSERT_EQ(lock_depth, 0);

    /* Heap growth path (beyond the initial 16 KB) also holds the lock once. */
    lock_acquires = 0;
    void *big = kmalloc(32 * 1024, GFP_KERNEL);
    ASSERT_TRUE(big != NULL);
    ASSERT_EQ(lock_acquires, 1);

    kfree(big);
    kfree(q);
    kfree(blocker);

    HeapStats st;
    kmalloc_get_stats(&st);
    ASSERT_EQ(lock_depth, 0);
    ASSERT_EQ(lock_max_depth, 1);
    return 0;
}

/* kmalloc_max_walk() records the longest first-fit walk (budget metric) */
static int test_max_walk_counts_blocks(void) {
    setup_heap();
    heap_walk_max = 0;
    void *a = kmalloc(64, GFP_KERNEL);          /* walks 1 block */
    ASSERT_EQ(kmalloc_max_walk(), 1);
    void *b = kmalloc(64, GFP_KERNEL);          /* a (used), then the rest: 2 */
    void *c = kmalloc(64, GFP_KERNEL);          /* a, b, rest: 3 */
    ASSERT_EQ(kmalloc_max_walk(), 3);
    kfree(a);
    void *d = kmalloc(32, GFP_KERNEL);          /* reuses a's block: 1, max stays 3 */
    ASSERT_EQ(kmalloc_max_walk(), 3);
    kfree(b); kfree(c); kfree(d);
    return 0;
}

/* --- Large allocations (M1-02): straight from alloc_pages --- */

static int test_large_goes_to_pages(void) {
    setup_heap();
    uint8_t *p = kmalloc(KMALLOC_LARGE_MIN, GFP_ZERO);
    ASSERT_TRUE(p != NULL);
    ASSERT_FALSE(in_heap(p));
    ASSERT_EQ(stub_allocs, 1);
    ASSERT_EQ(virt_to_page(p)->flags, PG_KMALLOC | 1);   /* 8 KiB = order 1 */
    ASSERT_EQ(stub_last_flags, GFP_ZERO);
    for (int i = 0; i < (int)KMALLOC_LARGE_MIN; i++) ASSERT_EQ(p[i], 0);
    HeapStats st;
    kmalloc_get_stats(&st);
    ASSERT_EQ(st.large_bytes, 8192);
    kfree(p);
    ASSERT_EQ(stub_frees, 1);
    kmalloc_get_stats(&st);
    ASSERT_EQ(st.large_bytes, 0);
    return 0;
}

static int test_large_rounds_to_power_of_two(void) {
    setup_heap();
    void *p = kmalloc(12 * 1024, GFP_KERNEL);   /* 3 pages -> order 2 */
    ASSERT_EQ(page_order(virt_to_page(p)), 2);
    void *q = kmalloc(KMALLOC_LARGE_MAX, GFP_KERNEL);
    ASSERT_EQ(page_order(virt_to_page(q)), 10);
    kfree(p);
    kfree(q);
    ASSERT_EQ(stub_frees, 2);
    return 0;
}

static int test_small_and_huge_stay_in_heap(void) {
    setup_heap();
    void *p = kmalloc(KMALLOC_LARGE_MIN - 1, GFP_KERNEL);
    ASSERT_TRUE(p != NULL);
    ASSERT_TRUE(in_heap(p));
    ASSERT_EQ(stub_allocs, 0);
    kfree(p);
    return 0;
}

/* kfree of a page-allocator block kmalloc didn't hand out is caught, not freed */
static int test_kfree_foreign_page_panics(void) {
    setup_heap();
    panics = 0;
    struct page *pg = alloc_pages(1, GFP_KERNEL);   /* no PG_KMALLOC */
    kfree(page_to_virt(pg));
    ASSERT_EQ(panics, 1);
    ASSERT_EQ(stub_frees, 0);
    HeapStats st;
    kmalloc_get_stats(&st);
    ASSERT_EQ(st.large_bytes, 0);                   /* accounting untouched */
    free_pages(pg, 1);
    return 0;
}

static int test_large_failure_releases_budget(void) {
    setup_heap();
    stub_fail_next = 1;
    ASSERT_TRUE(kmalloc(64 * 1024, GFP_KERNEL) == NULL);
    HeapStats st;
    kmalloc_get_stats(&st);
    ASSERT_EQ(st.large_bytes, 0);
    return 0;
}

static int test_large_cap(void) {
    setup_heap();
    large_bytes = KMALLOC_CAP_BYTES - 4096;          /* budget nearly used */
    ASSERT_TRUE(kmalloc(64 * 1024, GFP_KERNEL) == NULL);
    ASSERT_EQ(stub_allocs, 0);
    large_bytes = 0;
    return 0;
}

static int test_krealloc_heap_to_large_and_back(void) {
    setup_heap();
    uint8_t *p = kmalloc(100, GFP_KERNEL);
    for (int i = 0; i < 100; i++) p[i] = (uint8_t)i;
    uint8_t *q = krealloc(p, 20000);                 /* heap -> large */
    ASSERT_TRUE(q != NULL);
    ASSERT_EQ(stub_allocs, 1);
    for (int i = 0; i < 100; i++) ASSERT_EQ(q[i], (uint8_t)i);
    uint8_t *r = krealloc(q, 30000);                 /* fits the 32 KiB block */
    ASSERT_TRUE(r == q);
    uint8_t *s2 = krealloc(r, 50);                   /* large -> heap */
    ASSERT_TRUE(s2 != NULL);
    ASSERT_EQ(stub_frees, 1);
    for (int i = 0; i < 50; i++) ASSERT_EQ(s2[i], (uint8_t)i);
    ASSERT_TRUE(krealloc(kmalloc(16384, 0), 0) == NULL);   /* large, size 0 frees */
    ASSERT_EQ(stub_frees, 2);
    kfree(s2);
    return 0;
}

/* --- Test suite export --- */

TestCase kmalloc_tests[] = {
    {"basic_alloc_free", test_basic_alloc_free},
    {"kmalloc_zero_returns_null", test_kmalloc_zero_returns_null},
    {"gfp_zero_zeroes_memory", test_gfp_zero_zeroes_memory},
    {"split_block", test_split_block},
    {"coalesce", test_coalesce},
    {"multiple_alloc_free_cycles", test_multiple_alloc_free_cycles},
    {"alignment", test_alignment},
    {"krealloc_null_as_kmalloc", test_krealloc_null_acts_as_kmalloc},
    {"krealloc_zero_frees", test_krealloc_zero_frees},
    {"krealloc_grow_preserves", test_krealloc_grow_preserves_data},
    {"krealloc_shrink_preserves", test_krealloc_shrink_preserves_data},
    {"double_free_no_crash", test_double_free_no_crash},
    {"canary_survives_normal", test_canary_survives_normal_use},
    {"split_block_no_underflow", test_split_block_no_underflow},
    {"lock_taken_once_per_call", test_lock_taken_once_per_call},
    {"max_walk_counts_blocks", test_max_walk_counts_blocks},
    {"large_goes_to_pages", test_large_goes_to_pages},
    {"large_rounds_pow2", test_large_rounds_to_power_of_two},
    {"small_stays_in_heap", test_small_and_huge_stay_in_heap},
    {"kfree_foreign_page_panics", test_kfree_foreign_page_panics},
    {"large_failure_budget", test_large_failure_releases_budget},
    {"large_cap", test_large_cap},
    {"krealloc_heap_large", test_krealloc_heap_to_large_and_back},
};

int kmalloc_test_count = sizeof(kmalloc_tests) / sizeof(kmalloc_tests[0]);
