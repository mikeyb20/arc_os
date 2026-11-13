/* arc_os — Host-side tests for kernel/mm/mm.c (M1-07)
 *
 * The real mm.c on the real vma.c and rbtree.c (built by test_vma.c and
 * test_rbtree.c). Below it everything is a fake that checks the rules mm.c
 * must keep:
 *   - frames: a pool of real host pages; a double free or a free of a page
 *     whose TLB entry was not flushed since it was unmapped fails the test;
 *   - page tables: a flat list of (address space, page) entries;
 *   - the mm lock: a recorder that fails on a would-be deadlock (taking it
 *     while this thread holds it) and counts read and write holds;
 *   - TLB flushes and CR3 loads: counters. */

#include "test_framework.h"
#include <stdint.h>

#define ARCHOS_MM_VMM_H
#define ARCHOS_MM_PMM_H
#define ARCHOS_MM_SLAB_H
#define ARCHOS_MM_PAGE_H
#define ARCHOS_ARCH_HAL_H
#define ARCHOS_LIB_KPRINTF_H
#define ARCHOS_PROC_RWSEM_H

#include "proc/spinlock.h"
#include "proc/cpumask.h"
#include "mm/gfp.h"
#include "uapi/errno.h"

static inline void kprintf(const char *fmt, ...) {
    (void)fmt;
}
int ksnprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
#define KERNEL_PANIC() abort()

/* --- Constants from page.h / vmm.h --- */
#define PAGE_SIZE          4096
#define PAGE_SHIFT         12
#define PAGE_ALIGN_UP(x)   (((x) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1ULL))
#define PAGE_ALIGN_DOWN(x) ((x) & ~(PAGE_SIZE - 1ULL))
#define USER_STACK_TOP     0x00007FFFFFFFE000ULL
#define VMM_FLAG_WRITABLE  (1 << 0)
#define VMM_FLAG_USER      (1 << 1)
#define VMM_FLAG_NOEXEC    (1 << 2)
#define VMM_FLAG_PROTNONE  (1 << 3)
typedef void (*vmm_defer_free_t)(uint64_t phys, int table, void *ctx);

/* --- Frames --- */
#define POOL_PAGES 256
static uint8_t pool[POOL_PAGES * PAGE_SIZE] __attribute__((aligned(PAGE_SIZE)));

struct page {
    struct page *next;
    int allocated;
    int unmapped_at;   /* flush count when it left the page tables; -1 if mapped */
};
struct page_list {
    struct page *head;
};
#define PAGE_LIST_INIT \
    { NULL }
static struct page pages[POOL_PAGES];
static int pool_limit = POOL_PAGES;   /* allocations stop at this many in use */
static int pages_in_use;
static int frame_errors;              /* double frees, frees before the flush */
static int tlb_flushes;

static struct page *pfn_to_page(uint64_t pfn) {
    return &pages[((pfn << PAGE_SHIFT) - (uint64_t)(uintptr_t)pool) / PAGE_SIZE];
}
static uint64_t page_to_phys(const struct page *pg) {
    return (uint64_t)(uintptr_t)pool + (uint64_t)(pg - pages) * PAGE_SIZE;
}
static void *frame(uint64_t phys) {
    return (void *)(uintptr_t)phys;
}
static int page_list_empty(const struct page_list *l) {
    return l->head == NULL;
}
static struct page *page_list_first(const struct page_list *l) {
    return l->head;
}
static void page_list_push(struct page_list *l, struct page *pg) {
    pg->next = l->head;
    l->head = pg;
}
static void page_list_remove(struct page_list *l, struct page *pg) {
    struct page **pp = &l->head;
    while (*pp != pg) pp = &(*pp)->next;
    *pp = pg->next;
    pg->next = NULL;
}
static struct page *alloc_pages(unsigned order, uint32_t flags) {
    (void)order;
    if (pages_in_use >= pool_limit) return NULL;
    for (int i = 0; i < POOL_PAGES; i++) {
        if (pages[i].allocated) continue;
        pages[i].allocated = 1;
        pages[i].unmapped_at = -1;
        pages_in_use++;
        if (flags & GFP_ZERO) memset(frame(page_to_phys(&pages[i])), 0, PAGE_SIZE);
        return &pages[i];
    }
    return NULL;
}
static void pmm_free_page(uint64_t phys) {
    struct page *pg = pfn_to_page(phys >> PAGE_SHIFT);
    if (!pg->allocated) frame_errors++;                                        /* double free */
    if (pg->unmapped_at >= 0 && pg->unmapped_at == tlb_flushes) frame_errors++;   /* not flushed */
    pg->allocated = 0;
    pages_in_use--;
    memset(frame(phys), 0xEE, PAGE_SIZE);   /* poison: a stale user would see it */
}

/* --- Page tables: one flat list for every address space --- */
#define MAX_PTES 512
static struct {
    uint64_t pml4, virt, phys;
    uint32_t flags;
    int used;
} ptes[MAX_PTES];
static uint64_t next_pml4 = 0x100000;
static int pml4s_live;
static int map_fail;    /* vmm_map_page_in fails (no page table) */
static int move_fail;   /* vmm_move_range_in fails after this many pages (-1: never) */

static int pte_find(uint64_t pml4, uint64_t virt) {
    for (int i = 0; i < MAX_PTES; i++) {
        if (ptes[i].used && ptes[i].pml4 == pml4 && ptes[i].virt == virt) return i;
    }
    return -1;
}
static uint64_t vmm_create_user_pml4(void) {
    pml4s_live++;
    next_pml4 += 0x1000;
    return next_pml4;
}
static uint64_t vmm_get_kernel_pml4(void) {
    return 0x1000;
}
static int vmm_map_page_in(uint64_t pml4, uint64_t virt, uint64_t phys, uint32_t flags) {
    if (map_fail) return -ENOMEM;
    int i = pte_find(pml4, virt);
    for (int j = 0; i < 0 && j < MAX_PTES; j++) {
        if (!ptes[j].used) i = j;
    }
    if (i < 0) return -ENOMEM;
    ptes[i].pml4 = pml4;
    ptes[i].virt = virt;
    ptes[i].phys = phys;
    ptes[i].flags = flags;
    ptes[i].used = 1;
    return 0;
}
static int vmm_lookup_in(uint64_t pml4, uint64_t virt, uint64_t *phys) {
    int i = pte_find(pml4, virt);
    if (i < 0) return 0;
    if (phys != NULL) *phys = ptes[i].phys;
    return 1;
}
static uint64_t vmm_unmap_range_in(uint64_t pml4, uint64_t start, uint64_t end, vmm_defer_free_t defer, void *ctx) {
    uint64_t n = 0;
    for (int i = 0; i < MAX_PTES; i++) {
        if (!ptes[i].used || ptes[i].pml4 != pml4 || ptes[i].virt < start || ptes[i].virt >= end) continue;
        ptes[i].used = 0;
        pfn_to_page(ptes[i].phys >> PAGE_SHIFT)->unmapped_at = tlb_flushes;
        defer(ptes[i].phys, 0, ctx);
        n++;
    }
    return n;
}
static void vmm_release_table(uint64_t phys) {
    (void)phys;
}
static void vmm_protect_range_in(uint64_t pml4, uint64_t start, uint64_t end, uint32_t flags) {
    for (int i = 0; i < MAX_PTES; i++) {
        if (ptes[i].used && ptes[i].pml4 == pml4 && ptes[i].virt >= start && ptes[i].virt < end) ptes[i].flags = flags;
    }
}
static int vmm_copy_range_in(uint64_t src, uint64_t dst, uint64_t start, uint64_t end, uint64_t *copied) {
    for (int i = 0; i < MAX_PTES; i++) {
        if (!ptes[i].used || ptes[i].pml4 != src || ptes[i].virt < start || ptes[i].virt >= end) continue;
        struct page *pg = alloc_pages(0, 0);
        if (pg == NULL) return -ENOMEM;
        memcpy(frame(page_to_phys(pg)), frame(ptes[i].phys), PAGE_SIZE);
        if (vmm_map_page_in(dst, ptes[i].virt, page_to_phys(pg), ptes[i].flags) != 0) return -ENOMEM;
        (*copied)++;
    }
    return 0;
}
static int vmm_move_range_in(uint64_t pml4, uint64_t from, uint64_t to, uint64_t len) {
    int moved = 0;
    for (int i = 0; i < MAX_PTES; i++) {
        if (!ptes[i].used || ptes[i].pml4 != pml4 || ptes[i].virt < from || ptes[i].virt >= from + len) continue;
        if (move_fail >= 0 && moved >= move_fail) return -ENOMEM;
        ptes[i].virt = ptes[i].virt - from + to;
        moved++;
    }
    return 0;
}
static void vmm_free_user_pages(uint64_t pml4) {
    for (int i = 0; i < MAX_PTES; i++) {
        if (!ptes[i].used || ptes[i].pml4 != pml4) continue;
        ptes[i].used = 0;
        pfn_to_page(ptes[i].phys >> PAGE_SHIFT)->unmapped_at = -1;   /* nobody has it loaded */
        pmm_free_page(ptes[i].phys);
    }
    pml4s_live--;
}

/* --- HAL: one CPU --- */
static void *cpu_mm;
static int cr3_loads;
static uint64_t cr3;
static int invalidations;
static uint64_t flush_start, flush_end;
static uint32_t hal_cpu_index(void) {
    return 0;
}
static void *hal_this_cpu_mm(void) {
    return cpu_mm;
}
static void hal_set_this_cpu_mm(void *mm) {
    cpu_mm = mm;
}
static int cr3_loads_irqs_on;   /* loads made with interrupts enabled */
static void hal_switch_address_space(uint64_t v) {
    cr3_loads++;
    if (hal_test_irq_enabled) cr3_loads_irqs_on++;
    cr3 = v;
}
static void hal_invalidate_page(uint64_t v) {
    (void)v;
    invalidations++;
}
static uint32_t hal_tlb_flush(const CpuMask *cpus, uint64_t pgd, uint64_t start, uint64_t end) {
    (void)cpus;
    (void)pgd;
    tlb_flushes++;
    flush_start = start;
    flush_end = end;
    return 0;
}

/* --- The mm lock: single-threaded recorder --- */
typedef struct RwSem {
    int readers;
    int writer;
    uint16_t level;
} RwSem;
static int lock_errors, read_holds, write_holds;
static void rwsem_init(RwSem *s, uint16_t level) {
    s->readers = 0;
    s->writer = 0;
    s->level = level;
}
static void rwsem_down_read(RwSem *s) {
    if (s->writer) lock_errors++;   /* would deadlock */
    s->readers++;
    read_holds++;
}
static void rwsem_up_read(RwSem *s) {
    if (s->readers <= 0) lock_errors++;
    s->readers--;
}
static void rwsem_down_write(RwSem *s) {
    if (s->writer || s->readers) lock_errors++;
    s->writer = 1;
    write_holds++;
}
static void rwsem_up_write(RwSem *s) {
    if (!s->writer) lock_errors++;
    s->writer = 0;
}

/* --- Slab: malloc --- */
struct kmem_cache {
    int live;
};
static struct kmem_cache mm_test_cache;
static struct kmem_cache *kmem_cache_create(const char *name, size_t size, size_t align) {
    (void)name;
    (void)size;
    (void)align;
    return &mm_test_cache;
}
static void *kmem_cache_alloc(struct kmem_cache *c, uint32_t flags) {
    (void)flags;
    c->live++;
    return calloc(1, 4096);
}
static void kmem_cache_free(struct kmem_cache *c, void *p) {
    c->live--;
    free(p);
}

#include "../kernel/mm/mm.c"

/* ==================================================================
 * Helpers
 * ================================================================== */

static Mm *new_mm(void) {
    static int inited;
    if (!inited) {
        mm_init();
        inited = 1;
    }
    memset(ptes, 0, sizeof(ptes));
    for (int i = 0; i < POOL_PAGES; i++) {
        pages[i].allocated = 0;
        pages[i].unmapped_at = -1;
        pages[i].next = NULL;
    }
    pages_in_use = 0;
    pool_limit = POOL_PAGES;
    frame_errors = lock_errors = read_holds = write_holds = 0;
    tlb_flushes = invalidations = cr3_loads = 0;
    map_fail = 0;
    move_fail = -1;
    cpu_mm = NULL;
    return mm_create();
}

/* The mm's page at v, or NULL if none is mapped */
static uint8_t *page_at(Mm *mm, uint64_t v) {
    uint64_t phys;
    return vmm_lookup_in(mm->pgd, PAGE_ALIGN_DOWN(v), &phys) ? frame(phys) : NULL;
}

static uint32_t flags_at(Mm *mm, uint64_t v) {
    int i = pte_find(mm->pgd, PAGE_ALIGN_DOWN(v));
    return i < 0 ? 0xFFFFFFFF : ptes[i].flags;
}

static int touch(Mm *mm, uint64_t v, int write) {
    return mm_fault(mm, v, write ? MM_ACCESS_WRITE : 0);
}

/* Fault in every page of [start, start + n pages) for writing and stamp it */
static int fill(Mm *mm, uint64_t start, int n, uint8_t stamp) {
    for (int i = 0; i < n; i++) {
        uint64_t a = start + (uint64_t)i * PAGE_SIZE;
        if (touch(mm, a, 1) != MM_FAULT_OK) return -1;
        page_at(mm, a)[0] = (uint8_t)(stamp + i);
    }
    return 0;
}

#define RW   (VMA_READ | VMA_WRITE)
#define BASE 0x10000000ULL

/* Every rule the fakes check, plus no lock left held */
static int clean(Mm *mm) {
    return frame_errors == 0 && lock_errors == 0 && mm->lock.readers == 0 && !mm->lock.writer;
}

/* ==================================================================
 * Tests
 * ================================================================== */

TEST(create_and_put) {
    int live = pml4s_live;
    Mm *mm = new_mm();
    ASSERT_TRUE(mm != NULL);
    ASSERT_EQ(mm->users, 1);
    ASSERT_TRUE(mm->pgd != 0);
    ASSERT_EQ(mm->mmap_base, USER_STACK_TOP - MM_MMAP_GAP);
    ASSERT_EQ(pml4s_live, live + 1);
    ASSERT_EQ(mm_map_anon(mm, BASE, 4 * PAGE_SIZE, RW, 0, MM_MAP_FIXED, 0, 0), (int64_t)BASE);
    ASSERT_EQ(fill(mm, BASE, 4, 1), 0);
    ASSERT_EQ(mm_rss(mm), 4);
    mm_get(mm);
    mm_put(mm);   /* a pin: still there */
    ASSERT_EQ(pml4s_live, live + 1);
    mm_put(mm);
    ASSERT_EQ(pml4s_live, live);
    ASSERT_EQ(pages_in_use, 0);
    ASSERT_EQ(mm_test_cache.live, 0);   /* the mm itself */
    ASSERT_EQ(frame_errors, 0);
    return 0;
}

/* mmap only records a VMA; the first touch maps a zeroed page with the
 * VMA's protection, under the read lock */
TEST(demand_zero_fault) {
    Mm *mm = new_mm();
    int64_t a = mm_map_anon(mm, 0, 2 * PAGE_SIZE, VMA_READ, 0, MM_MAP_HINT, 0, 0);
    ASSERT_TRUE(a > 0 && a + 2 * PAGE_SIZE <= (int64_t)mm->mmap_base);
    ASSERT_EQ(pages_in_use, 0);
    int writes = write_holds;
    ASSERT_EQ(touch(mm, (uint64_t)a + 5, 0), MM_FAULT_OK);
    ASSERT_EQ(write_holds, writes);   /* faults take the lock for reading only */
    uint8_t *p = page_at(mm, (uint64_t)a);
    ASSERT_TRUE(p != NULL);
    ASSERT_EQ(p[0], 0);
    ASSERT_EQ(p[4095], 0);
    ASSERT_EQ(flags_at(mm, (uint64_t)a), VMM_FLAG_USER | VMM_FLAG_NOEXEC);
    ASSERT_EQ(mm_rss(mm), 1);
    ASSERT_EQ(mm_anon_pages(), 1);
    ASSERT_TRUE(clean(mm));
    mm_put(mm);
    ASSERT_EQ(mm_anon_pages(), 0);
    return 0;
}

TEST(fault_errors) {
    Mm *mm = new_mm();
    ASSERT_EQ(mm_map_anon(mm, BASE, PAGE_SIZE, VMA_READ, 0, MM_MAP_FIXED, 0, 0), (int64_t)BASE);
    ASSERT_EQ(mm_map_anon(mm, BASE + 0x10000, PAGE_SIZE, 0, 0, MM_MAP_FIXED, 0, 0), (int64_t)(BASE + 0x10000));
    ASSERT_EQ(touch(mm, BASE - 1, 0), MM_FAULT_MAPERR);            /* no VMA */
    ASSERT_EQ(touch(mm, BASE, 1), MM_FAULT_ACCERR);                /* read-only */
    ASSERT_EQ(mm_fault(mm, BASE, MM_ACCESS_EXEC), MM_FAULT_ACCERR);   /* not executable */
    ASSERT_EQ(touch(mm, BASE + 0x10000, 0), MM_FAULT_ACCERR);      /* PROT_NONE */
    ASSERT_EQ(touch(mm, MM_USER_END, 0), MM_FAULT_MAPERR);
    ASSERT_EQ(touch(mm, 0xFFFF800000000000ULL, 0), MM_FAULT_MAPERR);
    ASSERT_EQ(pages_in_use, 0);
    ASSERT_TRUE(clean(mm));
    mm_put(mm);
    return 0;
}

/* A fault on a page that is there (another CPU mapped it, or a stale TLB
 * entry) rewrites the entry and drops the local TLB entry: no new page */
TEST(fault_on_present_page) {
    Mm *mm = new_mm();
    mm_map_anon(mm, BASE, PAGE_SIZE, RW, 0, MM_MAP_FIXED, 0, 0);
    ASSERT_EQ(fill(mm, BASE, 1, 7), 0);
    int i = pte_find(mm->pgd, BASE);
    ptes[i].flags = VMM_FLAG_USER | VMM_FLAG_NOEXEC;   /* an older, narrower entry */
    ASSERT_EQ(touch(mm, BASE, 1), MM_FAULT_OK);
    ASSERT_EQ(flags_at(mm, BASE), VMM_FLAG_USER | VMM_FLAG_WRITABLE | VMM_FLAG_NOEXEC);
    ASSERT_EQ(page_at(mm, BASE)[0], 7);
    ASSERT_EQ(pages_in_use, 1);
    ASSERT_EQ(mm_rss(mm), 1);
    ASSERT_EQ(invalidations, 1);
    ASSERT_TRUE(clean(mm));
    mm_put(mm);
    return 0;
}

/* No frame, or no page table: OOM, nothing leaked, the counters unchanged */
TEST(fault_out_of_memory) {
    Mm *mm = new_mm();
    mm_map_anon(mm, BASE, 2 * PAGE_SIZE, RW, 0, MM_MAP_FIXED, 0, 0);
    pool_limit = 0;
    ASSERT_EQ(touch(mm, BASE, 1), MM_FAULT_OOM);
    pool_limit = POOL_PAGES;
    map_fail = 1;
    ASSERT_EQ(touch(mm, BASE, 1), MM_FAULT_OOM);
    map_fail = 0;
    ASSERT_EQ(pages_in_use, 0);
    ASSERT_EQ(mm_rss(mm), 0);
    ASSERT_TRUE(clean(mm));
    mm_put(mm);
    return 0;
}

/* munmap in the middle splits the VMA; the page is freed after the flush */
TEST(munmap_splits_and_frees_after_flush) {
    Mm *mm = new_mm();
    mm_map_anon(mm, BASE, 4 * PAGE_SIZE, RW, 0, MM_MAP_FIXED, 0, 0);
    ASSERT_EQ(fill(mm, BASE, 4, 10), 0);
    ASSERT_EQ(mm_unmap(mm, BASE + PAGE_SIZE, PAGE_SIZE), 0);
    ASSERT_EQ(mm->vmas.count, 2);
    ASSERT_EQ(mm->vmas.head->end, BASE + PAGE_SIZE);
    ASSERT_EQ(mm->vmas.tail->start, BASE + 2 * PAGE_SIZE);
    ASSERT_EQ(tlb_flushes, 1);
    ASSERT_EQ(flush_start, BASE + PAGE_SIZE);
    ASSERT_EQ(flush_end, BASE + 2 * PAGE_SIZE);
    ASSERT_EQ(pages_in_use, 3);
    ASSERT_EQ(mm_rss(mm), 3);
    ASSERT_TRUE(page_at(mm, BASE + PAGE_SIZE) == NULL);
    ASSERT_EQ(page_at(mm, BASE + 2 * PAGE_SIZE)[0], 12);
    ASSERT_EQ(touch(mm, BASE + PAGE_SIZE, 0), MM_FAULT_MAPERR);
    /* A range over a hole and both pieces: one call removes all */
    ASSERT_EQ(mm_unmap(mm, BASE, 4 * PAGE_SIZE), 0);
    ASSERT_EQ(mm->vmas.count, 0);
    ASSERT_EQ(pages_in_use, 0);
    /* Nothing there: no flush */
    int flushes = tlb_flushes;
    ASSERT_EQ(mm_unmap(mm, BASE, PAGE_SIZE), 0);
    ASSERT_EQ(tlb_flushes, flushes);
    ASSERT_TRUE(clean(mm));
    mm_put(mm);
    return 0;
}

TEST(map_fixed_replaces_noreplace_refuses) {
    Mm *mm = new_mm();
    mm_map_anon(mm, BASE, 3 * PAGE_SIZE, RW, 0, MM_MAP_FIXED, 0, 0);
    ASSERT_EQ(fill(mm, BASE, 3, 1), 0);
    ASSERT_EQ(mm_map_anon(mm, BASE + PAGE_SIZE, PAGE_SIZE, RW, 0, MM_MAP_NOREPLACE, 0, 0), -EEXIST);
    ASSERT_EQ(pages_in_use, 3);
    ASSERT_EQ(mm_map_anon(mm, BASE + PAGE_SIZE, PAGE_SIZE, VMA_READ, 0, MM_MAP_FIXED, 0, 0),
              (int64_t)(BASE + PAGE_SIZE));
    ASSERT_EQ(pages_in_use, 2);   /* the old page went */
    ASSERT_TRUE(page_at(mm, BASE + PAGE_SIZE) == NULL);
    ASSERT_EQ(mm->vmas.count, 3);
    ASSERT_EQ(touch(mm, BASE + PAGE_SIZE, 0), MM_FAULT_OK);
    ASSERT_EQ(page_at(mm, BASE + PAGE_SIZE)[0], 0);   /* a fresh zero page */
    ASSERT_EQ(mm_map_anon(mm, BASE + 0x100000, PAGE_SIZE, RW, 0, MM_MAP_NOREPLACE, 0, 0), (int64_t)(BASE + 0x100000));
    ASSERT_TRUE(clean(mm));
    mm_put(mm);
    return 0;
}

/* Top-down placement below mmap_base; neighbours with the same protection
 * merge; a freed range is found again through the hint */
TEST(map_placement) {
    Mm *mm = new_mm();
    int64_t a = mm_map_anon(mm, 0, 2 * PAGE_SIZE, RW, 0, MM_MAP_HINT, 0, 0);
    int64_t b = mm_map_anon(mm, 0, PAGE_SIZE, RW, 0, MM_MAP_HINT, 0, 0);
    ASSERT_EQ((uint64_t)a, mm->mmap_base - 2 * PAGE_SIZE);
    ASSERT_EQ(b, a - PAGE_SIZE);
    ASSERT_EQ(mm->vmas.count, 1);   /* merged */
    int64_t c = mm_map_anon(mm, 0, PAGE_SIZE, VMA_READ, 0, MM_MAP_HINT, 0, 0);
    ASSERT_EQ(c, b - PAGE_SIZE);
    ASSERT_EQ(mm->vmas.count, 2);   /* other protection */
    /* A free hint is taken as is */
    ASSERT_EQ(mm_map_anon(mm, BASE + 123, PAGE_SIZE, RW, 0, MM_MAP_HINT, 0, 0), (int64_t)(BASE + PAGE_SIZE));
    /* A hint below mmap_min_addr is ignored */
    int64_t d = mm_map_anon(mm, 0x1000, PAGE_SIZE, RW, 0, MM_MAP_HINT, 0, 0);
    ASSERT_TRUE((uint64_t)d >= MM_MMAP_MIN && (uint64_t)d < mm->mmap_base);
    /* Freed space at the top is used again */
    ASSERT_EQ(mm_unmap(mm, (uint64_t)a, 2 * PAGE_SIZE), 0);
    ASSERT_EQ(mm_map_anon(mm, 0, 2 * PAGE_SIZE, RW, 0, MM_MAP_HINT, 0, 0), a);
    /* MAP_32BIT: the second GiB */
    int64_t e = mm_map_anon(mm, 0, PAGE_SIZE, RW, 0, MM_MAP_HINT, 0, 1);
    ASSERT_TRUE(e >= 0x40000000 && e < 0x80000000);
    ASSERT_TRUE(clean(mm));
    mm_put(mm);
    return 0;
}

TEST(map_populate) {
    Mm *mm = new_mm();
    int64_t a = mm_map_anon(mm, 0, 3 * PAGE_SIZE, RW, 0, MM_MAP_HINT, 1, 0);
    ASSERT_TRUE(a > 0);
    ASSERT_EQ(pages_in_use, 3);
    ASSERT_EQ(mm_rss(mm), 3);
    /* best effort: out of memory still returns the mapping */
    pool_limit = 4;
    int64_t b = mm_map_anon(mm, 0, 3 * PAGE_SIZE, RW, 0, MM_MAP_HINT, 1, 0);
    ASSERT_TRUE(b > 0);
    ASSERT_EQ(pages_in_use, 4);
    ASSERT_TRUE(clean(mm));
    mm_put(mm);
    return 0;
}

/* mprotect splits, rewrites the entries, flushes, and merges back */
TEST(mprotect_split_and_merge) {
    Mm *mm = new_mm();
    mm_map_anon(mm, BASE, 3 * PAGE_SIZE, RW, 0, MM_MAP_FIXED, 0, 0);
    ASSERT_EQ(fill(mm, BASE, 3, 1), 0);
    int flushes = tlb_flushes;
    ASSERT_EQ(mm_protect(mm, BASE + PAGE_SIZE, PAGE_SIZE, VMA_READ), 0);
    ASSERT_EQ(tlb_flushes, flushes + 1);
    ASSERT_EQ(mm->vmas.count, 3);
    ASSERT_EQ(flags_at(mm, BASE + PAGE_SIZE), VMM_FLAG_USER | VMM_FLAG_NOEXEC);
    ASSERT_EQ(flags_at(mm, BASE), VMM_FLAG_USER | VMM_FLAG_WRITABLE | VMM_FLAG_NOEXEC);
    ASSERT_EQ(touch(mm, BASE + PAGE_SIZE, 1), MM_FAULT_ACCERR);
    ASSERT_EQ(mm_protect(mm, BASE + PAGE_SIZE, PAGE_SIZE, 0), 0);   /* PROT_NONE keeps the page */
    ASSERT_EQ(flags_at(mm, BASE + PAGE_SIZE), VMM_FLAG_PROTNONE);
    ASSERT_EQ(pages_in_use, 3);
    ASSERT_EQ(mm_protect(mm, BASE + PAGE_SIZE, PAGE_SIZE, RW), 0);
    ASSERT_EQ(mm->vmas.count, 1);   /* merged back */
    ASSERT_EQ(page_at(mm, BASE + PAGE_SIZE)[0], 2);   /* contents kept throughout */
    /* A hole: -ENOMEM, nothing changed */
    mm_unmap(mm, BASE + PAGE_SIZE, PAGE_SIZE);
    ASSERT_EQ(mm_protect(mm, BASE, 3 * PAGE_SIZE, VMA_READ), -ENOMEM);
    ASSERT_EQ(flags_at(mm, BASE), VMM_FLAG_USER | VMM_FLAG_WRITABLE | VMM_FLAG_NOEXEC);
    ASSERT_TRUE(clean(mm));
    mm_put(mm);
    return 0;
}

/* MADV_DONTNEED frees the pages and keeps the VMA: the next touch is zero */
TEST(dontneed_frees_pages) {
    Mm *mm = new_mm();
    mm_map_anon(mm, BASE, 2 * PAGE_SIZE, RW, 0, MM_MAP_FIXED, 0, 0);
    ASSERT_EQ(fill(mm, BASE, 2, 9), 0);
    ASSERT_EQ(mm_dontneed(mm, BASE, 2 * PAGE_SIZE), 0);
    ASSERT_EQ(pages_in_use, 0);
    ASSERT_EQ(mm_rss(mm), 0);
    ASSERT_EQ(mm->vmas.count, 1);
    ASSERT_EQ(touch(mm, BASE, 0), MM_FAULT_OK);
    ASSERT_EQ(page_at(mm, BASE)[0], 0);
    ASSERT_EQ(mm_dontneed(mm, BASE, 4 * PAGE_SIZE), -ENOMEM);   /* runs past the VMA */
    ASSERT_EQ(pages_in_use, 0);                                 /* but its part was done */
    ASSERT_TRUE(mm_range_mapped(mm, BASE, 2 * PAGE_SIZE));
    ASSERT_FALSE(mm_range_mapped(mm, BASE, 3 * PAGE_SIZE));
    ASSERT_TRUE(clean(mm));
    mm_put(mm);
    return 0;
}

/* mremap: shrink, grow in place, move with the same frames, fixed, errors */
TEST(mremap_cases) {
    Mm *mm = new_mm();
    mm_map_anon(mm, BASE, 4 * PAGE_SIZE, RW, 0, MM_MAP_FIXED, 0, 0);
    ASSERT_EQ(fill(mm, BASE, 4, 20), 0);
    ASSERT_EQ(mm_remap(mm, BASE, 4 * PAGE_SIZE, 2 * PAGE_SIZE, 0, 0, 0), (int64_t)BASE);   /* shrink */
    ASSERT_EQ(pages_in_use, 2);
    /* same size from inside the VMA: a no-op, no split */
    ASSERT_EQ(mm_remap(mm, BASE, PAGE_SIZE, PAGE_SIZE, 0, 0, 0), (int64_t)BASE);
    ASSERT_EQ(mm->vmas.count, 1);
    ASSERT_EQ(mm_remap(mm, BASE, 2 * PAGE_SIZE, 8 * PAGE_SIZE, 0, 0, 0), (int64_t)BASE);   /* grow in place */
    ASSERT_EQ(mm->vmas.head->end, BASE + 8 * PAGE_SIZE);
    /* blocked above: without MAYMOVE -ENOMEM, with it the pages move */
    mm_map_anon(mm, BASE + 8 * PAGE_SIZE, PAGE_SIZE, VMA_READ, 0, MM_MAP_FIXED, 0, 0);
    ASSERT_EQ(mm_remap(mm, BASE, 8 * PAGE_SIZE, 16 * PAGE_SIZE, 0, 0, 0), -ENOMEM);
    uint8_t *before = page_at(mm, BASE + PAGE_SIZE);
    int64_t n = mm_remap(mm, BASE, 8 * PAGE_SIZE, 16 * PAGE_SIZE, 1, 0, 0);
    ASSERT_TRUE(n > 0 && (uint64_t)n != BASE);
    ASSERT_TRUE(page_at(mm, (uint64_t)n + PAGE_SIZE) == before);   /* moved, not copied */
    ASSERT_EQ(page_at(mm, (uint64_t)n)[0], 20);
    ASSERT_TRUE(page_at(mm, BASE) == NULL);
    ASSERT_EQ(touch(mm, BASE, 0), MM_FAULT_MAPERR);
    ASSERT_EQ(touch(mm, (uint64_t)n + 15 * PAGE_SIZE, 1), MM_FAULT_OK);   /* the grown part */
    ASSERT_EQ(pages_in_use, 3);
    /* MREMAP_FIXED to a chosen place; overlap with the source refused */
    ASSERT_EQ(mm_remap(mm, (uint64_t)n, PAGE_SIZE, PAGE_SIZE, 1, 1, (uint64_t)n), -EINVAL);
    ASSERT_EQ(mm_remap(mm, (uint64_t)n, PAGE_SIZE, PAGE_SIZE, 1, 1, BASE + 0x100000), (int64_t)(BASE + 0x100000));
    ASSERT_EQ(page_at(mm, BASE + 0x100000)[0], 20);
    /* the source must lie in one VMA */
    ASSERT_EQ(mm_remap(mm, BASE + 0x200000, PAGE_SIZE, 2 * PAGE_SIZE, 1, 0, 0), -EFAULT);
    ASSERT_TRUE(clean(mm));
    mm_put(mm);
    ASSERT_EQ(pages_in_use, 0);
    return 0;
}

/* A move that fails halfway puts every page back where it was */
TEST(mremap_move_failure_restores) {
    Mm *mm = new_mm();
    mm_map_anon(mm, BASE, 4 * PAGE_SIZE, RW, 0, MM_MAP_FIXED, 0, 0);
    ASSERT_EQ(fill(mm, BASE, 4, 40), 0);
    mm_map_anon(mm, BASE + 4 * PAGE_SIZE, PAGE_SIZE, VMA_READ, 0, MM_MAP_FIXED, 0, 0);
    move_fail = 2;
    ASSERT_EQ(mm_remap(mm, BASE, 4 * PAGE_SIZE, 8 * PAGE_SIZE, 1, 0, 0), -ENOMEM);
    move_fail = -1;
    for (int i = 0; i < 4; i++) ASSERT_EQ(page_at(mm, BASE + (uint64_t)i * PAGE_SIZE)[0], 40 + i);
    ASSERT_EQ(mm->vmas.head->start, BASE);
    ASSERT_EQ(mm->vmas.head->end, BASE + 4 * PAGE_SIZE);
    ASSERT_EQ(pages_in_use, 4);
    ASSERT_EQ(mm_rss(mm), 4);
    ASSERT_TRUE(clean(mm));
    mm_put(mm);
    return 0;
}

/* brk: a [heap] VMA from brk_start; one free page kept above; shrinking
 * frees; out-of-range requests return the break unchanged */
TEST(brk_heap) {
    Mm *mm = new_mm();
    ASSERT_EQ(mm_setup_stack(mm, 0, BASE + 0x123), 0);
    uint64_t start = BASE + PAGE_SIZE;
    ASSERT_EQ(mm_brk(mm, 0), start);
    ASSERT_EQ(mm_brk(mm, start + 3 * PAGE_SIZE + 5), start + 3 * PAGE_SIZE + 5);
    Vma *heap = vma_find_containing(&mm->vmas, start);
    ASSERT_TRUE(heap != NULL && (heap->flags & VMA_HEAP));
    ASSERT_EQ(heap->end, start + 4 * PAGE_SIZE);
    ASSERT_EQ(fill(mm, start, 4, 1), 0);
    ASSERT_EQ(mm_brk(mm, start + PAGE_SIZE), start + PAGE_SIZE);
    ASSERT_EQ(pages_in_use, 1);
    ASSERT_EQ(mm_brk(mm, start - 1), start + PAGE_SIZE);   /* below brk_start */
    /* a mapping just above the heap: no growing into it, nor its guard page */
    mm_map_anon(mm, start + 3 * PAGE_SIZE, PAGE_SIZE, RW, 0, MM_MAP_FIXED, 0, 0);
    ASSERT_EQ(mm_brk(mm, start + 3 * PAGE_SIZE), start + PAGE_SIZE);
    ASSERT_EQ(mm_brk(mm, start + 2 * PAGE_SIZE), start + 2 * PAGE_SIZE);
    ASSERT_TRUE(clean(mm));
    mm_put(mm);
    return 0;
}

/* The stack VMA grows down on a fault below it, up to 8 MiB, and not into
 * the guard gap above another VMA */
TEST(stack_growth) {
    Mm *mm = new_mm();
    ASSERT_EQ(mm_setup_stack(mm, 0, BASE), 0);
    Vma *st = mm->vmas.tail;
    ASSERT_TRUE(st->flags & VMA_STACK);
    ASSERT_EQ(st->end, USER_STACK_TOP);
    ASSERT_EQ(st->start, USER_STACK_TOP - MM_STACK_INIT);
    uint64_t below = USER_STACK_TOP - MM_STACK_INIT - 3 * PAGE_SIZE;
    int writes = write_holds;
    ASSERT_EQ(touch(mm, below + 8, 1), MM_FAULT_OK);
    ASSERT_EQ(write_holds, writes + 1);   /* growing takes the write lock once */
    ASSERT_EQ(st->start, below);
    /* a VMA 1 MiB + 2 pages under the stack: growth stops a guard gap above it */
    uint64_t low = st->start - MM_STACK_GUARD - 2 * PAGE_SIZE;
    mm_map_anon(mm, low - PAGE_SIZE, PAGE_SIZE, RW, 0, MM_MAP_FIXED, 0, 0);
    ASSERT_EQ(touch(mm, st->start - PAGE_SIZE, 1), MM_FAULT_OK);
    ASSERT_EQ(touch(mm, st->start - 2 * PAGE_SIZE, 1), MM_FAULT_MAPERR);
    ASSERT_TRUE(clean(mm));
    mm_put(mm);

    mm = new_mm();
    ASSERT_EQ(mm_setup_stack(mm, 0, BASE), 0);
    st = mm->vmas.tail;
    ASSERT_EQ(touch(mm, USER_STACK_TOP - MM_STACK_MAX, 1), MM_FAULT_OK);   /* exactly 8 MiB */
    ASSERT_EQ(st->start, USER_STACK_TOP - MM_STACK_MAX);
    ASSERT_EQ(touch(mm, USER_STACK_TOP - MM_STACK_MAX - 1, 1), MM_FAULT_MAPERR);
    ASSERT_TRUE(clean(mm));
    mm_put(mm);
    return 0;
}

/* fork: same VMAs and heap, private copies of every page */
TEST(fork_copies) {
    Mm *mm = new_mm();
    int live = pml4s_live;
    mm_setup_stack(mm, 0, BASE);
    mm_map_anon(mm, BASE + 0x100000, 2 * PAGE_SIZE, RW, 0, MM_MAP_FIXED, 0, 0);
    ASSERT_EQ(fill(mm, BASE + 0x100000, 2, 50), 0);
    mm_brk(mm, BASE + 2 * PAGE_SIZE);
    Mm *child = mm_fork(mm);
    ASSERT_TRUE(child != NULL);
    ASSERT_EQ(child->vmas.count, mm->vmas.count);
    ASSERT_EQ(child->brk, mm->brk);
    ASSERT_EQ(child->brk_start, mm->brk_start);
    ASSERT_EQ(mm_rss(child), 2);
    uint8_t *pp = page_at(mm, BASE + 0x100000), *cp = page_at(child, BASE + 0x100000);
    ASSERT_TRUE(cp != NULL && cp != pp);
    ASSERT_EQ(cp[0], 50);
    cp[0] = 99;
    ASSERT_EQ(pp[0], 50);
    mm_put(child);
    ASSERT_EQ(pages_in_use, 2);
    /* out of memory halfway: nothing leaks */
    pool_limit = 3;
    ASSERT_TRUE(mm_fork(mm) == NULL);
    ASSERT_EQ(pages_in_use, 2);
    ASSERT_EQ(pml4s_live, live);
    ASSERT_TRUE(clean(mm));
    mm_put(mm);
    return 0;
}

/* ELF setup: a VMA may share its first page with the one below */
TEST(setup_vma_overlap_rules) {
    Mm *mm = new_mm();
    ASSERT_EQ(mm_setup_vma(mm, 0x400000, 0x402000, VMA_READ | VMA_EXEC, 0), 0);
    ASSERT_EQ(mm_setup_vma(mm, 0x401000, 0x404000, RW, 0), 0);   /* shares 0x401000 */
    Vma *shared = vma_find_containing(&mm->vmas, 0x401000);
    ASSERT_EQ(shared->start, 0x401000);
    ASSERT_EQ(shared->end, 0x402000);
    ASSERT_EQ(shared->prot, VMA_READ | VMA_WRITE | VMA_EXEC);
    ASSERT_EQ(vma_find_containing(&mm->vmas, 0x400000)->prot, VMA_READ | VMA_EXEC);
    ASSERT_EQ(vma_find_containing(&mm->vmas, 0x402000)->prot, RW);
    ASSERT_EQ(mm_setup_vma(mm, 0x402000, 0x405000, RW, 0), -EINVAL);   /* more than one page */
    ASSERT_EQ(mm_setup_vma(mm, 0x500000, 0x500000, RW, 0), -EINVAL);
    ASSERT_TRUE(clean(mm));
    mm_put(mm);
    return 0;
}

/* /proc/<pid>/maps lines, Linux format: names padded to column 73 */
TEST(format_maps) {
    Mm *mm = new_mm();
    mm_setup_vma(mm, 0x400000, 0x401000, VMA_READ | VMA_EXEC, 0);
    mm_setup_stack(mm, 0, 0x401000);
    mm_brk(mm, 0x402000);
    static char buf[1024];
    int n = mm_format_maps(mm, buf, sizeof(buf));
    ASSERT_EQ(n, (int)strlen(buf));
    const char *l1 = "00400000-00401000 r-xp 00000000 00:00 0 \n";
    ASSERT_TRUE(strncmp(buf, l1, strlen(l1)) == 0);
    const char *heap = buf + strlen(l1);
    ASSERT_TRUE(strncmp(heap, "00401000-00402000 rw-p 00000000 00:00 0 ", 40) == 0);
    ASSERT_TRUE(strncmp(heap + 73, "[heap]\n", 7) == 0);
    for (int i = 40; i < 73; i++) ASSERT_EQ(heap[i], ' ');
    ASSERT_TRUE(strstr(buf, "7fffffffe000 rw-p 00000000 00:00 0") != NULL);
    ASSERT_TRUE(strstr(buf, "[stack]\n") != NULL);
    /* a small buffer: whole lines only */
    n = mm_format_maps(mm, buf, 50);
    ASSERT_EQ(n, (int)strlen(l1));
    ASSERT_TRUE(clean(mm));
    mm_put(mm);
    return 0;
}

/* The OOM reaper frees every page now and leaves the VMAs for exit */
TEST(reap_frees_pages) {
    Mm *mm = new_mm();
    mm_map_anon(mm, BASE, 3 * PAGE_SIZE, RW, 0, MM_MAP_FIXED, 0, 0);
    mm_map_anon(mm, BASE + 0x100000, 2 * PAGE_SIZE, VMA_READ, 0, MM_MAP_FIXED, 0, 0);
    ASSERT_EQ(fill(mm, BASE, 3, 0), 0);
    ASSERT_EQ(touch(mm, BASE + 0x100000, 0), MM_FAULT_OK);
    ASSERT_EQ(mm_reap(mm), 4);
    ASSERT_EQ(pages_in_use, 0);
    ASSERT_EQ(mm_rss(mm), 0);
    ASSERT_EQ(mm->vmas.count, 2);
    ASSERT_TRUE(clean(mm));
    mm_put(mm);
    return 0;
}

/* CR3 loads only on a change of mm; the loaded-CPU set follows */
TEST(switch_tracks_cpus) {
    Mm *a = new_mm();
    Mm *b = mm_create();
    hal_test_irq_enabled = 1; /* callers like exec run with interrupts on */
    mm_switch(a);
    ASSERT_EQ(cr3_loads_irqs_on, 0);
    ASSERT_EQ(hal_test_irq_enabled, 1); /* restored */
    ASSERT_EQ(cr3_loads, 1);
    ASSERT_EQ(cr3, a->pgd);
    ASSERT_TRUE(cpumask_test(&a->cpus, 0));
    mm_switch(a);
    ASSERT_EQ(cr3_loads, 1);
    mm_switch(b);
    ASSERT_EQ(cr3, b->pgd);
    ASSERT_FALSE(cpumask_test(&a->cpus, 0));
    ASSERT_TRUE(cpumask_test(&b->cpus, 0));
    mm_switch(NULL);
    ASSERT_EQ(cr3, 0x1000);
    ASSERT_FALSE(cpumask_test(&b->cpus, 0));
    ASSERT_TRUE(hal_this_cpu_mm() == NULL);
    mm_put(a);
    mm_put(b);
    return 0;
}

TestCase mm_tests[] = {
    TEST_ENTRY(create_and_put),
    TEST_ENTRY(demand_zero_fault),
    TEST_ENTRY(fault_errors),
    TEST_ENTRY(fault_on_present_page),
    TEST_ENTRY(fault_out_of_memory),
    TEST_ENTRY(munmap_splits_and_frees_after_flush),
    TEST_ENTRY(map_fixed_replaces_noreplace_refuses),
    TEST_ENTRY(map_placement),
    TEST_ENTRY(map_populate),
    TEST_ENTRY(mprotect_split_and_merge),
    TEST_ENTRY(dontneed_frees_pages),
    TEST_ENTRY(mremap_cases),
    TEST_ENTRY(mremap_move_failure_restores),
    TEST_ENTRY(brk_heap),
    TEST_ENTRY(stack_growth),
    TEST_ENTRY(fork_copies),
    TEST_ENTRY(setup_vma_overlap_rules),
    TEST_ENTRY(format_maps),
    TEST_ENTRY(reap_frees_pages),
    TEST_ENTRY(switch_tracks_cpus),
};
int mm_test_count = sizeof(mm_tests) / sizeof(mm_tests[0]);
