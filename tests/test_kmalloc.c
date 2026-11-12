/* arc_os — Host-side tests for the slab allocator and kmalloc
 * (kernel/mm/slab.c, kernel/mm/kmalloc.c; M1-02b).
 *
 * The real page.h runs over a host arena: PFN 0 is the arena's first page and
 * the "HHDM" offset is the arena's address, so virt -> page -> cache works as
 * in the kernel. A stub page allocator hands out size-aligned blocks of the
 * arena and checks the descriptor invariants the buddy allocator relies on.
 * Every test runs with the debug checks off and on (slab_debug is a variable
 * here, a constant in the kernel). */

#include "test_framework.h"
#include <stdint.h>
#include <stdlib.h>

/* Guard kernel headers that conflict with the host or are stubbed below */
#define ARCHOS_MM_PMM_H
#define ARCHOS_LIB_KPRINTF_H
#define ARCHOS_LIB_MEM_H        /* libc memset/memcpy */
#define ARCHOS_PROC_SPINLOCK_H  /* stubbed below: records nesting */

/* The descriptor array globals live in test_pmm.c's copy of pmm.c: rename */
#define pmm_page_array  kt_page_array
#define pmm_max_pfn     kt_max_pfn
#define pmm_hhdm_offset kt_hhdm_offset

/* Spinlock stub that records nesting (same API as proc/spinlock.h) */
#include "proc/lockdep.h"
typedef struct Spinlock {
    volatile uint32_t locked;
    uint16_t level;
    uint16_t subkey;
    const char *name;
} Spinlock;
#define SPINLOCK_INIT(lvl, nm) \
    { .locked = 0, .level = (lvl), .subkey = 0, .name = (nm) }
static int lock_depth, lock_max_depth, lock_errors;
static inline void spin_lock_init(Spinlock *l, uint16_t level, const char *name) {
    l->locked = 0;
    l->level = level;
    l->subkey = 0;
    l->name = name;
}
static inline void spin_lock(Spinlock *l) {
    if (l->locked) lock_errors++;   /* would deadlock */
    l->locked = 1;
    if (++lock_depth > lock_max_depth) lock_max_depth = lock_depth;
}
static inline void spin_unlock(Spinlock *l) {
    if (!l->locked) lock_errors++;
    l->locked = 0;
    lock_depth--;
}
static inline void spin_lock_irqsave(Spinlock *l, uint64_t *flags) {
    *flags = 0;
    spin_lock(l);
}
static inline void spin_unlock_irqrestore(Spinlock *l, uint64_t flags) {
    (void)flags;
    spin_unlock(l);
}

static inline void kprintf(const char *fmt, ...) { (void)fmt; }
static int panics;
#define KERNEL_PANIC() \
    do {               \
        panics++;      \
    } while (0)

#include "mm/page.h"
#include "mm/gfp.h"

struct page *kt_page_array;
uint64_t kt_max_pfn;
uint64_t kt_hhdm_offset;

/* --- Stub page allocator over the arena --- */

#define ARENA_PAGES 4096u   /* 16 MiB: room for a 4 MiB block plus slabs */
static uint8_t *arena;
static struct page stub_desc[ARENA_PAGES];
static uint8_t stub_used[ARENA_PAGES];
static int stub_fail_next, stub_violations;
static long stub_pages_out;

static struct page *alloc_pages(unsigned order, uint32_t flags) {
    if (stub_fail_next) {
        stub_fail_next = 0;
        return NULL;
    }
    uint32_t n = 1u << order;
    for (uint32_t pfn = 0; pfn + n <= ARENA_PAGES; pfn += n) {
        uint32_t i = 0;
        while (i < n && !stub_used[pfn + i]) i++;
        if (i < n) continue;
        for (i = 0; i < n; i++) {
            stub_used[pfn + i] = 1;
            if (stub_desc[pfn + i].flags != 0) stub_violations++;   /* free pages have no flags */
        }
        struct page *pg = &stub_desc[pfn];
        pg->flags = order;
        pg->refcount = 1;
        pg->mapcount = 0;
        pg->index = 0;
        pg->mapping = NULL;
        memset(arena + (size_t)pfn * PAGE_SIZE, (flags & GFP_ZERO) ? 0 : 0xEE, (size_t)n * PAGE_SIZE);
        stub_pages_out += n;
        return pg;
    }
    return NULL;
}

static void free_pages(struct page *pg, unsigned order) {
    uint32_t pfn = (uint32_t)(pg - stub_desc), n = 1u << order;
    if ((pg->flags & PG_ORDER_MASK) != order || pg->refcount != 1 || !stub_used[pfn]) {
        stub_violations++;
        return;
    }
    if (pg->flags & PG_SLAB) stub_violations++;   /* slab.c clears its flag first */
    for (uint32_t i = 1; i < n; i++) {
        if (stub_desc[pfn + i].flags != 0 || stub_desc[pfn + i].mapping != NULL) stub_violations++;
    }
    pg->flags = 0;
    pg->refcount = 0;
    for (uint32_t i = 0; i < n; i++) stub_used[pfn + i] = 0;
    stub_pages_out -= n;
}

#define SLAB_TEST_DEBUG_KNOB
static int slab_debug;

#include "../kernel/mm/slab.c"
#include "../kernel/mm/kmalloc.c"

/* Fresh allocator state, debug checks on or off */
static void setup(int debug) {
    if (arena == NULL) arena = aligned_alloc((size_t)ARENA_PAGES * PAGE_SIZE, (size_t)ARENA_PAGES * PAGE_SIZE);
    memset(stub_desc, 0, sizeof(stub_desc));
    memset(stub_used, 0, sizeof(stub_used));
    kt_page_array = stub_desc;
    kt_max_pfn = ARENA_PAGES;
    kt_hhdm_offset = (uint64_t)(uintptr_t)arena;
    stub_fail_next = stub_violations = 0;
    stub_pages_out = 0;
    panics = lock_depth = lock_max_depth = lock_errors = 0;
    registry_head = registry_tail = NULL;
    charged_bytes = 0;
    max_steps = last_steps = 0;
    large_bytes = 0;
    slab_debug = debug;
    kmalloc_init();
}

/* State checks every test ends with */
static int clean_end(void) {
    ASSERT_EQ(panics, 0);
    ASSERT_EQ(stub_violations, 0);
    ASSERT_EQ(lock_errors, 0);
    ASSERT_EQ(lock_depth, 0);
    return 0;
}

/* Run `body(debug)` with debug checks off, then on */
#define BOTH_MODES(body)                     \
    do {                                     \
        for (int dbg = 0; dbg <= 1; dbg++) { \
            setup(dbg);                      \
            if (body(dbg) != 0) return 1;    \
        }                                    \
    } while (0)

static struct kmem_cache *owner(const void *p) {
    return slab_owner(p);
}

/* --- Size classes --- */

static int size_classes_body(int dbg) {
    (void)dbg;
    static const size_t req[] = {1, 8, 16, 17, 32, 33, 64, 65, 128, 129, 256, 257, 512, 513, 1024, 1025, 2048};
    static const uint32_t want[] = {16, 16, 16, 32, 32, 64, 64, 128, 128, 256, 256, 512, 512, 1024, 1024, 2048, 2048};
    for (unsigned i = 0; i < sizeof(req) / sizeof(req[0]); i++) {
        uint8_t *p = kmalloc(req[i], GFP_KERNEL);
        ASSERT_TRUE(p != NULL);
        ASSERT_TRUE(owner(p) != NULL);
        ASSERT_EQ(owner(p)->object_size, want[i]);
        ASSERT_EQ((uintptr_t)p % 16, 0);
        memset(p, 0x5A, req[i]);
        kfree(p);
    }
    return clean_end();
}

static int test_size_classes(void) {
    BOTH_MODES(size_classes_body);
    return 0;
}

static int test_zero_and_null(void) {
    setup(1);
    ASSERT_TRUE(kmalloc(0, GFP_KERNEL) == NULL);
    kfree(NULL);
    kmem_cache_free(&size_caches[0], NULL);
    ASSERT_TRUE(kmalloc(KMALLOC_LARGE_MAX + 1, GFP_KERNEL) == NULL);
    return clean_end();
}

static int gfp_zero_body(int dbg) {
    (void)dbg;
    uint8_t *p = kmalloc(200, GFP_KERNEL);
    memset(p, 0xFF, 200);
    kfree(p);
    uint8_t *q = kmalloc(200, GFP_ZERO);   /* same object comes back */
    ASSERT_TRUE(q == p);
    for (int i = 0; i < 256; i++) ASSERT_EQ(q[i], 0);
    kfree(q);
    uint8_t *big = kmalloc(10000, GFP_ZERO);
    for (int i = 0; i < 16384; i++) ASSERT_EQ(big[i], 0);
    kfree(big);
    return clean_end();
}

static int test_gfp_zero(void) {
    BOTH_MODES(gfp_zero_body);
    return 0;
}

/* Release builds keep objects back to back; debug adds a red zone */
static int test_layout(void) {
    setup(0);
    ASSERT_EQ(size_caches[2].stride, 64);
    ASSERT_EQ(size_caches[2].per_slab, 64);
    ASSERT_EQ(size_caches[2].order, 0);
    ASSERT_EQ(size_caches[6].order, 1);   /* 1024 B: order-1 slab, 8 objects */
    ASSERT_EQ(size_caches[6].per_slab, 8);
    ASSERT_EQ(size_caches[7].order, 2);   /* 2048 B: order-2 slab, 8 objects */
    setup(1);
    ASSERT_EQ(size_caches[2].stride, 80);   /* 64 + 8 red zone, 16-aligned */
    ASSERT_EQ(size_caches[0].stride, 32);
    return clean_end();
}

/* --- Bounded steps, no overlap (ADR-0019) --- */

#define LIVE_MAX 600
static uint8_t *live_ptr[LIVE_MAX];
static size_t live_len[LIVE_MAX];
static uint8_t live_tag[LIVE_MAX];

static uint32_t lcg_state;
static uint32_t lcg(void) {
    lcg_state = lcg_state * 1103515245u + 12345u;
    return lcg_state >> 8;
}

static int check_tag(int i) {
    for (size_t b = 0; b < live_len[i]; b++) {
        if (live_ptr[i][b] != live_tag[i]) return -1;
    }
    return 0;
}

static int mixed_body(int dbg) {
    (void)dbg;
    memset(live_ptr, 0, sizeof(live_ptr));
    lcg_state = 12345;
    uint32_t worst = 0;
    for (int op = 0; op < 100000; op++) {
        int i = (int)(lcg() % LIVE_MAX);
        if (live_ptr[i] != NULL) {
            ASSERT_EQ(check_tag(i), 0);   /* nobody wrote over it */
            int slab = live_len[i] <= KMALLOC_SLAB_MAX;
            kfree(live_ptr[i]);
            live_ptr[i] = NULL;
            if (slab && slab_last_steps() > worst) worst = slab_last_steps();
            continue;
        }
        uint32_t r = lcg();
        size_t len = (r % 100 == 0) ? 2049 + r % 30000 : 1 + r % KMALLOC_SLAB_MAX;
        uint8_t *p = kmalloc(len, GFP_KERNEL);
        ASSERT_TRUE(p != NULL);
        if (len <= KMALLOC_SLAB_MAX && slab_last_steps() > worst) worst = slab_last_steps();
        live_ptr[i] = p;
        live_len[i] = len;
        live_tag[i] = (uint8_t)(op & 0xFF);
        memset(p, live_tag[i], len);
    }
    ASSERT_TRUE(worst <= 3);
    ASSERT_TRUE(slab_max_steps() <= 3);
    for (int i = 0; i < LIVE_MAX; i++) {
        if (live_ptr[i] != NULL) {
            ASSERT_EQ(check_tag(i), 0);
            kfree(live_ptr[i]);
        }
    }
    /* Everything freed: only the one reserve slab per cache is left */
    long reserve = 0;
    for (unsigned c = 0; c < KMALLOC_NR_CLASSES; c++) {
        ASSERT_EQ(size_caches[c].active, 0);
        ASSERT_TRUE(size_caches[c].nr_slabs <= 1);
        reserve += (long)size_caches[c].nr_slabs << size_caches[c].order;
    }
    ASSERT_EQ(stub_pages_out, reserve);
    ASSERT_EQ(large_bytes, 0);
    ASSERT_EQ(lock_max_depth, 1);   /* one cache lock at a time */
    return clean_end();
}

static int test_mixed_100k_bounded_steps(void) {
    BOTH_MODES(mixed_body);
    return 0;
}

/* --- Slab lifecycle --- */

static int empty_slabs_body(int dbg) {
    (void)dbg;
    struct kmem_cache *c = &size_caches[2];   /* kmalloc-64 */
    static void *objs[400];
    for (int i = 0; i < 400; i++) objs[i] = kmalloc(64, GFP_KERNEL);
    uint64_t slabs = c->nr_slabs;
    ASSERT_TRUE(slabs >= 400u / c->per_slab);
    ASSERT_EQ(c->active, 400);
    for (int i = 0; i < 400; i++) kfree(objs[i]);
    ASSERT_EQ(c->active, 0);
    ASSERT_EQ(c->nr_slabs, 1);   /* reserve */
    ASSERT_EQ(c->nr_empty, 1);
    ASSERT_EQ(stub_pages_out, 1);
    ASSERT_EQ(kmem_cache_shrink(c), 1);
    ASSERT_EQ(c->nr_slabs, 0);
    ASSERT_EQ(stub_pages_out, 0);
    ASSERT_EQ(kmem_charged_bytes(), 0);
    ASSERT_EQ(kmem_cache_shrink(c), 0);
    /* The cache still works after a shrink */
    void *p = kmalloc(64, GFP_KERNEL);
    ASSERT_TRUE(p != NULL);
    kfree(p);
    return clean_end();
}

static int test_empty_slabs_returned(void) {
    BOTH_MODES(empty_slabs_body);
    return 0;
}

/* Full -> partial -> empty transitions on one cache */
static int lists_body(int dbg) {
    (void)dbg;
    struct kmem_cache *c = &size_caches[7];   /* kmalloc-2048, few per slab */
    unsigned n = c->per_slab;
    static void *objs[64];
    for (unsigned i = 0; i < 2 * n; i++) objs[i] = kmalloc(2048, GFP_KERNEL);
    ASSERT_EQ(c->nr_slabs, 2);
    ASSERT_TRUE(page_list_empty(&c->partial));
    ASSERT_FALSE(page_list_empty(&c->full));
    kfree(objs[0]);   /* first slab: full -> partial */
    ASSERT_FALSE(page_list_empty(&c->partial));
    void *again = kmalloc(2048, GFP_KERNEL);
    ASSERT_TRUE(again == objs[0]);   /* free list reused before a new slab */
    ASSERT_EQ(c->nr_slabs, 2);
    for (unsigned i = 0; i < 2 * n; i++) kfree(objs[i]);
    ASSERT_EQ(c->nr_slabs, 1);
    ASSERT_TRUE(page_list_empty(&c->full));
    ASSERT_TRUE(page_list_empty(&c->partial));
    return clean_end();
}

static int test_slab_list_transitions(void) {
    BOTH_MODES(lists_body);
    return 0;
}

/* Objects past the first page of a multi-page slab find their slab */
static int multipage_body(int dbg) {
    (void)dbg;
    struct kmem_cache *c = &size_caches[6];   /* kmalloc-1024: order 1, order 2 with red zones */
    ASSERT_TRUE(c->order >= 1);
    static void *objs[16];
    for (unsigned i = 0; i < c->per_slab; i++) objs[i] = kmalloc(1024, GFP_KERNEL);
    uint8_t *last = objs[c->per_slab - 1];
    struct page *pg = virt_to_page(last);
    ASSERT_TRUE(pg->flags & PG_SLAB);
    ASSERT_TRUE((page_to_pfn(pg) & ((1u << c->order) - 1)) != 0);   /* a tail page */
    ASSERT_TRUE(owner(last) == c);
    for (unsigned i = 0; i < c->per_slab; i++) kfree(objs[i]);
    ASSERT_EQ(kmem_cache_shrink(c), 1u << c->order);
    return clean_end();   /* the stub checks tails were cleared */
}

static int test_multipage_slab(void) {
    BOTH_MODES(multipage_body);
    return 0;
}

/* --- Named caches --- */

static int named_body(int dbg) {
    (void)dbg;
    struct kmem_cache *c = kmem_cache_create("test-obj", 40, 64);
    ASSERT_TRUE(c != NULL);
    ASSERT_TRUE(owner(c) == &cache_cache);   /* comes from the cache of caches */
    void *a = kmem_cache_alloc(c, GFP_ZERO);
    void *b = kmem_cache_alloc(c, GFP_KERNEL);
    ASSERT_EQ((uintptr_t)a % 64, 0);
    ASSERT_EQ((uintptr_t)b % 64, 0);
    ASSERT_TRUE(a != b);
    ASSERT_EQ(slab_object_size(a), 40);
    kmem_cache_free(c, a);
    kfree(b);   /* kfree works on named-cache objects too */
    ASSERT_EQ(c->active, 0);

    ASSERT_TRUE(kmem_cache_create("bad", 0, 0) == NULL);
    ASSERT_TRUE(kmem_cache_create("bad", SLAB_OBJECT_MAX + 1, 0) == NULL);
    ASSERT_TRUE(kmem_cache_create("bad", 32, 24) == NULL);
    ASSERT_EQ(cache_cache.active, 1);   /* failed creates gave their object back */

    struct kmem_cache *big = kmem_cache_create("test-big", 4000, 0);
    ASSERT_EQ(big->order, 3);
    ASSERT_TRUE(big->per_slab >= 7);
    struct kmem_cache *mid = kmem_cache_create("test-mid", 600, 0);
    ASSERT_EQ(mid->order, 1);
    return clean_end();
}

static int test_named_caches(void) {
    BOTH_MODES(named_body);
    return 0;
}

static int test_slabinfo_and_totals(void) {
    setup(0);
    void *p = kmalloc(100, GFP_KERNEL);   /* kmalloc-128 */
    void *q = kmalloc(100, GFP_KERNEL);
    SlabInfo si;
    ASSERT_EQ(slab_get_info(0, &si), 0);
    ASSERT_STR_EQ(si.name, "kmem_cache");
    ASSERT_EQ(slab_get_info(4, &si), 0);
    ASSERT_STR_EQ(si.name, "kmalloc-128");
    ASSERT_EQ(si.object_size, 128);
    ASSERT_EQ(si.active_objs, 2);
    ASSERT_EQ(si.total_objs, 32);
    ASSERT_EQ(si.slabs, 1);
    ASSERT_EQ(si.pages_per_slab, 1);
    ASSERT_EQ(slab_get_info(1 + KMALLOC_NR_CLASSES, &si), -1);
    HeapStats hs;
    kmalloc_get_stats(&hs);
    ASSERT_EQ(hs.total_used, 256);
    ASSERT_EQ(hs.heap_mapped, 4096);
    ASSERT_EQ(hs.total_free, 4096 - 256);
    ASSERT_EQ(hs.large_bytes, 0);
    ASSERT_EQ(lock_max_depth, 2);   /* registry lock, then one cache lock */
    kfree(p);
    kfree(q);
    return clean_end();
}

/* --- Large blocks --- */

static int large_body(int dbg) {
    (void)dbg;
    uint8_t *a = kmalloc(2049, GFP_KERNEL);   /* just above the classes: one page */
    ASSERT_EQ(((uintptr_t)a) % PAGE_SIZE, 0);
    ASSERT_TRUE(owner(a) == NULL);
    ASSERT_TRUE(virt_to_page(a)->flags & PG_KMALLOC);
    ASSERT_EQ(page_order(virt_to_page(a)), 0);
    uint8_t *b = kmalloc(8193, GFP_KERNEL);
    ASSERT_EQ(page_order(virt_to_page(b)), 2);
    uint8_t *c = kmalloc(KMALLOC_LARGE_MAX, GFP_KERNEL);
    ASSERT_TRUE(c != NULL);
    ASSERT_EQ(page_order(virt_to_page(c)), PAGE_MAX_ORDER);
    ASSERT_EQ(large_bytes, 4096u + 16384u + KMALLOC_LARGE_MAX);
    ASSERT_EQ(kmem_charged_bytes(), large_bytes);
    kfree(a);
    kfree(b);
    kfree(c);
    ASSERT_EQ(large_bytes, 0);
    ASSERT_EQ(kmem_charged_bytes(), 0);
    ASSERT_EQ(stub_pages_out, 0);
    return clean_end();
}

static int test_large_blocks(void) {
    BOTH_MODES(large_body);
    return 0;
}

static int test_large_failure_and_cap(void) {
    setup(0);
    stub_fail_next = 1;
    ASSERT_TRUE(kmalloc(10000, GFP_KERNEL) == NULL);
    ASSERT_EQ(kmem_charged_bytes(), 0);   /* charge undone */
    stub_fail_next = 1;
    ASSERT_TRUE(kmalloc(64, GFP_KERNEL) == NULL);   /* no slab to be had */
    ASSERT_EQ(kmem_charged_bytes(), 0);
    ASSERT_EQ(size_caches[2].nr_slabs, 0);

    charged_bytes = KMEM_CAP_BYTES - 4096;   /* cap nearly used */
    ASSERT_TRUE(kmalloc(8192, GFP_KERNEL) == NULL);
    void *p = kmalloc(4096, GFP_KERNEL);   /* exactly fits */
    ASSERT_TRUE(p != NULL);
    ASSERT_TRUE(kmalloc(16, GFP_KERNEL) == NULL);   /* a new slab would pass the cap */
    kfree(p);
    ASSERT_EQ(kmem_charged_bytes(), KMEM_CAP_BYTES - 4096);
    charged_bytes = 0;
    return clean_end();
}

/* --- Bad frees --- */

static int bad_free_body(int dbg) {
    (void)dbg;
    static uint8_t not_heap[64];
    kfree(not_heap);   /* outside the arena: owner_page fails */
    ASSERT_EQ(panics, 1);
    kfree(arena + 100 * PAGE_SIZE);   /* a free page of the arena */
    ASSERT_EQ(panics, 2);
    uint8_t *big = kmalloc(16384, GFP_KERNEL);
    kfree(big + PAGE_SIZE);   /* tail page of a large block */
    ASSERT_EQ(panics, 3);
    kfree(big + 8);   /* inside the head page, not its start */
    ASSERT_EQ(panics, 4);
    ASSERT_TRUE(krealloc(not_heap, 10) == NULL);
    ASSERT_EQ(panics, 5);
    kfree(big);
    panics = 0;
    return clean_end();
}

static int test_kfree_bad_pointer_panics(void) {
    BOTH_MODES(bad_free_body);
    return 0;
}

static int test_debug_double_free(void) {
    setup(1);
    void *p = kmalloc(32, GFP_KERNEL);
    void *keep = kmalloc(32, GFP_KERNEL);   /* keeps the slab alive */
    kfree(p);
    kfree(p);
    ASSERT_EQ(panics, 1);
    ASSERT_EQ(size_caches[1].active, 1);   /* the second free changed nothing */
    kfree(keep);
    panics = 0;
    return clean_end();
}

static int test_debug_overflow_caught(void) {
    setup(1);
    uint8_t *p = kmalloc(64, GFP_KERNEL);
    p[64] = 0;   /* one byte past the object: into the red zone */
    kfree(p);
    ASSERT_EQ(panics, 1);
    panics = 0;
    return clean_end();
}

static int test_debug_write_after_free_caught(void) {
    setup(1);
    uint8_t *p = kmalloc(64, GFP_KERNEL);
    void *keep = kmalloc(64, GFP_KERNEL);
    kfree(p);
    for (int i = 0; i < 64; i++) ASSERT_EQ(p[i], POISON_FREE);
    p[10] = 1;   /* use after free */
    void *q = kmalloc(64, GFP_KERNEL);   /* reuses p: poison check fails */
    ASSERT_TRUE(q == NULL);
    ASSERT_EQ(panics, 1);
    kfree(keep);
    panics = 0;
    return 0;
}

static int test_debug_interior_and_wrong_cache(void) {
    setup(1);
    uint8_t *p = kmalloc(128, GFP_KERNEL);
    kfree(p + 16);   /* inside an object */
    ASSERT_EQ(panics, 1);
    kmem_cache_free(&size_caches[0], p);   /* object of kmalloc-128 freed to kmalloc-16 */
    ASSERT_EQ(panics, 2);
    uint8_t *never = p + 4 * size_caches[4].stride;   /* object start never handed out */
    kfree(never);
    ASSERT_EQ(panics, 3);
    kfree(p);
    ASSERT_EQ(panics, 3);
    panics = 0;
    return clean_end();
}

/* Release builds carry none of the debug checks: a double free is not seen */
static int test_release_has_no_checks(void) {
    setup(0);
    uint8_t *p = kmalloc(64, GFP_KERNEL);
    void *keep = kmalloc(64, GFP_KERNEL);
    p[63] = 0x11;
    kfree(p);
    ASSERT_EQ(p[63], 0x11);   /* no poison */
    ASSERT_EQ(panics, 0);
    kfree(keep);
    return clean_end();
}

/* --- krealloc --- */

static int krealloc_body(int dbg) {
    (void)dbg;
    uint8_t *p = krealloc(NULL, 20);
    ASSERT_TRUE(p != NULL);
    ASSERT_EQ(owner(p)->object_size, 32);
    for (int i = 0; i < 20; i++) p[i] = (uint8_t)i;
    ASSERT_TRUE(krealloc(p, 30) == p);   /* same class: in place */
    uint8_t *q = krealloc(p, 300);       /* bigger class: moves */
    ASSERT_TRUE(q != p);
    for (int i = 0; i < 20; i++) ASSERT_EQ(q[i], i);
    uint8_t *r = krealloc(q, 100000);    /* to a large block */
    ASSERT_EQ((uintptr_t)r % PAGE_SIZE, 0);
    for (int i = 0; i < 20; i++) ASSERT_EQ(r[i], i);
    ASSERT_TRUE(krealloc(r, 120000) == r);   /* same 128 KiB block */
    uint8_t *s = krealloc(r, 10);        /* back to a slab, keeps the first 10 */
    ASSERT_EQ(owner(s)->object_size, 16);
    for (int i = 0; i < 10; i++) ASSERT_EQ(s[i], i);
    ASSERT_EQ(large_bytes, 0);
    stub_fail_next = 1;
    ASSERT_TRUE(krealloc(s, 50000) == NULL);   /* failure: old block untouched */
    for (int i = 0; i < 10; i++) ASSERT_EQ(s[i], i);
    ASSERT_TRUE(krealloc(s, KMALLOC_LARGE_MAX + 1) == NULL);
    ASSERT_TRUE(krealloc(s, 0) == NULL);   /* frees */
    ASSERT_EQ(size_caches[0].active, 0);
    return clean_end();
}

static int test_krealloc(void) {
    BOTH_MODES(krealloc_body);
    return 0;
}

/* --- Suite --- */

TestCase kmalloc_tests[] = {
    {"size_classes", test_size_classes},
    {"zero_and_null", test_zero_and_null},
    {"gfp_zero", test_gfp_zero},
    {"layout", test_layout},
    {"mixed_100k_bounded_steps", test_mixed_100k_bounded_steps},
    {"empty_slabs_returned", test_empty_slabs_returned},
    {"slab_list_transitions", test_slab_list_transitions},
    {"multipage_slab", test_multipage_slab},
    {"named_caches", test_named_caches},
    {"slabinfo_and_totals", test_slabinfo_and_totals},
    {"large_blocks", test_large_blocks},
    {"large_failure_and_cap", test_large_failure_and_cap},
    {"kfree_bad_pointer_panics", test_kfree_bad_pointer_panics},
    {"debug_double_free", test_debug_double_free},
    {"debug_overflow_caught", test_debug_overflow_caught},
    {"debug_write_after_free_caught", test_debug_write_after_free_caught},
    {"debug_interior_and_wrong_cache", test_debug_interior_and_wrong_cache},
    {"release_has_no_checks", test_release_has_no_checks},
    {"krealloc", test_krealloc},
};
int kmalloc_test_count = sizeof(kmalloc_tests) / sizeof(kmalloc_tests[0]);
