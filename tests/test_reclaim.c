/* arc_os — page reclaim and write-back (M1-14) on the real reclaim.c,
 * writeback.c, filemap.c and filemap_mmap.c over the VFS host environment:
 * the watermarks, LRU aging, which pages reclaim frees and which it keeps
 * (dirty, locked, held, mapped and in use), direct reclaim under GFP_RECLAIM
 * only, the flusher's timer (armed only while something is dirty), sync and
 * eviction writing back.
 *
 * kswapd and the flusher never run on the host (thread_create returns NULL):
 * the tests call reclaim_pages and the write-back entry points themselves. */

#include "test_framework.h"
#include "vfs_env.h"

#include "fs/vfs_internal.h"
#include "mm/filemap.h"
#include "mm/reclaim.h"
#include "mm/writeback.h"
#include "mm/pmm.h"
#include "mm/mm.h"
#include "proc/lockdep.h"
#include "uapi/errno.h"
#include "uapi/fcntl.h"

/* --- A file over memory: its "disk" is mf_disk --- */

#define MF_PAGES 64
static uint8_t mf_disk[MF_PAGES * PAGE_SIZE];
static int mf_reads, mf_writes, mf_write_fail;

static int mf_readpage(AddressSpace *as, struct page *pg) {
    (void)as;
    memcpy(page_to_virt(pg), mf_disk + (size_t)pg->index * PAGE_SIZE, PAGE_SIZE);
    mf_reads++;
    filemap_end_read(pg, 0);
    return 0;
}

static int mf_writepage(AddressSpace *as, struct page *pg) {
    (void)as;
    if (mf_write_fail) return -EIO;
    memcpy(mf_disk + (size_t)pg->index * PAGE_SIZE, page_to_virt(pg), PAGE_SIZE);
    mf_writes++;
    return 0;
}

static const AddressSpaceOps mf_ops = {.readpage = mf_readpage, .writepage = mf_writepage};
static Inode mf_ino;
static File mf_file;

/* Forget the last test's file (its pages go, nothing written), then a fresh
 * one of MF_PAGES pages; nothing else on the LRU lists */
static AddressSpace *mf_setup(void) {
    vfs_env_setup();
    if (mf_ino.mapping != NULL) {
        mf_ino.nlink = 0;
        filemap_inode_evict(&mf_ino);
    }
    (void)reclaim_drop_clean();
    wb_sync_all();   /* other suites' listed mappings */
    for (size_t i = 0; i < sizeof(mf_disk); i++) mf_disk[i] = (uint8_t)(i / PAGE_SIZE + 1);
    mf_reads = mf_writes = mf_write_fail = 0;
    memset(&mf_ino, 0, sizeof(mf_ino));
    mf_ino.nlink = 1;
    mf_ino.size = (loff_t)MF_PAGES * PAGE_SIZE;
    if (filemap_inode_init(&mf_ino, &mf_ops, 0) != 0) return NULL;
    memset(&mf_file, 0, sizeof(mf_file));
    mf_file.refcount = 1;   /* ours: a VMA's fput never frees it */
    mf_file.inode = &mf_ino;
    return filemap_of(&mf_ino);
}

/* Read page i into the cache (as a page fault would: no access mark) */
static int mf_get(AddressSpace *as, uint64_t i) {
    unsigned off;
    int err = 0;
    struct page *pg = filemap_get_page(as, i, NULL, 0, &off, &err);
    if (pg == NULL) return err;
    page_put(pg);
    return 0;
}

static struct page *mf_cached(AddressSpace *as, uint64_t i) {
    unsigned off;
    struct page *pg = filemap_find(as, i, &off);
    if (pg != NULL) page_put(pg);   /* the tree still holds it */
    return pg;
}

static int mf_dirty(AddressSpace *as, uint64_t i, uint8_t byte) {
    unsigned off;
    int err = 0;
    struct page *pg = filemap_get_page(as, i, NULL, 0, &off, &err);
    if (pg == NULL) return err;
    page_lock(pg);
    memset(page_to_virt(pg), byte, PAGE_SIZE);
    filemap_set_dirty(pg);
    page_unlock(pg);
    page_put(pg);
    return 0;
}

static int64_t mf_read(uint64_t at, size_t n) {
    static char buf[PAGE_SIZE];
    IoBuf b = iob_kernel(buf, n);
    loff_t pos = (loff_t)at;
    return filemap_read(&mf_file, &b, &pos);
}

/* A shared mapping of the memory file at MAP_AT in its own address space */
#define MAP_AT 0x20000000ull

typedef struct {
    Mm mm;
    Vma v;
} Map;

static uint64_t next_pgd = 0x900000;

static void mf_map(Map *m) {
    memset(m, 0, sizeof(*m));
    m->mm.pgd = next_pgd += 0x1000;
    spin_lock_init(&m->mm.pt_lock, LOCK_LEVEL_PAGE_TABLE, "pt");
    m->v.start = MAP_AT;
    m->v.end = MAP_AT + (uint64_t)MF_PAGES * PAGE_SIZE;
    m->v.prot = VMA_READ;
    m->v.flags = VMA_SHARED;
    m->v.file = &mf_file;
    m->v.ops = &filemap_vma_ops;
    m->v.mm = &m->mm;
    filemap_vma_ops.open(&m->v);
}

static void mf_unmap(Map *m) {
    mm_zap_range(&m->mm, m->v.start, m->v.end, MM_ZAP_FLUSH);
    filemap_vma_ops.close(&m->v);
}

static int mf_fault(Map *m, uint64_t page, int write) {
    return filemap_vma_ops.fault(&m->mm, &m->v, MAP_AT + page * PAGE_SIZE, write ? MM_ACCESS_WRITE : 0);
}

static int mf_mapped(Map *m, uint64_t page) {
    struct page *pg = NULL;
    return vfs_env_pte(m->mm.pgd, MAP_AT + page * PAGE_SIZE, &pg, NULL);
}

static void mf_touch(Map *m, uint64_t page) {
    vfs_env_pte_touch(m->mm.pgd, MAP_AT + page * PAGE_SIZE);
}

/* --- Watermarks --- */

TEST(watermarks_follow_ram) {
    uint64_t mn, lo, hi;
    reclaim_watermarks_for(131072, &mn, &lo, &hi);   /* 512 MiB: the QEMU test */
    ASSERT_EQ(mn, 724);
    ASSERT_EQ(lo, 1748);
    ASSERT_EQ(hi, 2772);
    reclaim_watermarks_for(1048576, &mn, &lo, &hi);   /* 4 GiB: the low tier */
    ASSERT_EQ(mn, 2048);
    ASSERT_EQ(lo, 4096);
    ASSERT_EQ(hi, 6144);
    reclaim_watermarks_for(8388608, &mn, &lo, &hi);   /* 32 GiB: the high tier */
    ASSERT_EQ(mn, 5792);
    ASSERT_EQ(lo, 22176);
    ASSERT_EQ(hi, 38560);
    reclaim_watermarks_for(1024, &mn, &lo, &hi);   /* 4 MiB */
    ASSERT_EQ(mn, 64);
    ASSERT_EQ(lo, 1088);
    ASSERT_EQ(hi, 2112);
    reclaim_watermarks_for(256, &mn, &lo, &hi);   /* 1 MiB: the 128 KiB floor */
    ASSERT_EQ(mn, 32);
    ASSERT_EQ(lo, 1056);
    ASSERT_EQ(hi, 2080);
    reclaim_watermarks_for(1ull << 32, &mn, &lo, &hi);   /* 16 TiB: the 256 MiB cap */
    ASSERT_EQ(mn, 65536);
    return 0;
}

/* --- LRU aging --- */

TEST(new_pages_inactive_second_use_activates) {
    AddressSpace *as = mf_setup();
    ASSERT_TRUE(as != NULL);
    ReclaimStats r0, r;
    reclaim_stats(&r0);
    ASSERT_EQ(mf_get(as, 3), 0);
    struct page *pg = mf_cached(as, 3);
    ASSERT_TRUE(pg != NULL);
    reclaim_stats(&r);
    ASSERT_EQ(r.nr_inactive, r0.nr_inactive + 1);
    ASSERT_TRUE(page_test(pg, PG_LRU) && !page_test(pg, PG_ACTIVE) && !page_test(pg, PG_REFERENCED));
    mark_page_accessed(pg);   /* first use: a mark, no list change */
    ASSERT_TRUE(page_test(pg, PG_REFERENCED) && !page_test(pg, PG_ACTIVE));
    mark_page_accessed(pg);   /* second: active */
    reclaim_stats(&r);
    ASSERT_TRUE(page_test(pg, PG_ACTIVE) && page_test(pg, PG_LRU));
    ASSERT_EQ(r.nr_active, r0.nr_active + 1);
    ASSERT_EQ(r.nr_inactive, r0.nr_inactive);
    ASSERT_EQ(r.activate, r0.activate + 1);
    return 0;
}

/* read() marks each page once per run of reads inside it: a reader working
 * through a page in small pieces doesn't make it active */
TEST(small_reads_of_one_page_count_once) {
    AddressSpace *as = mf_setup();
    ASSERT_TRUE(as != NULL);
    for (int i = 0; i < 8; i++) ASSERT_EQ(mf_read(100 + (uint64_t)i * 512, 512), 512);
    struct page *p0 = mf_cached(as, 0);
    ASSERT_TRUE(p0 != NULL);
    ASSERT_TRUE(page_test(p0, PG_REFERENCED) && !page_test(p0, PG_ACTIVE));
    ASSERT_EQ(mf_read(PAGE_SIZE, 16), 16);   /* page 1 */
    ASSERT_EQ(mf_read(0, 16), 16);           /* page 0 again: used twice */
    ASSERT_TRUE(page_test(p0, PG_ACTIVE));
    struct page *p1 = mf_cached(as, 1);
    ASSERT_TRUE(p1 != NULL && !page_test(p1, PG_ACTIVE));
    return 0;
}

/* --- What reclaim frees --- */

TEST(clean_pages_go_oldest_first) {
    AddressSpace *as = mf_setup();
    ASSERT_TRUE(as != NULL);
    for (uint64_t i = 0; i < 40; i++) ASSERT_EQ(mf_get(as, i), 0);
    mark_page_accessed(mf_cached(as, 0));   /* referenced once, not mapped: still clean and free-able */
    FilemapStats f0, f;
    filemap_stats(&f0);
    ReclaimStats r0, r;
    reclaim_stats(&r0);
    uint64_t free0 = pmm_get_free_pages();
    /* Asked for 4: one batch of 32 from the tail (the oldest) */
    ASSERT_EQ(reclaim_pages(4, 0), RECLAIM_BATCH);
    for (uint64_t i = 0; i < RECLAIM_BATCH; i++) ASSERT_TRUE(mf_cached(as, i) == NULL);
    for (uint64_t i = RECLAIM_BATCH; i < 40; i++) ASSERT_TRUE(mf_cached(as, i) != NULL);
    ASSERT_EQ(as->nrpages, 40 - RECLAIM_BATCH);
    ASSERT_EQ(pmm_get_free_pages(), free0 + RECLAIM_BATCH);
    filemap_stats(&f);
    ASSERT_EQ(f0.file_pages - f.file_pages, RECLAIM_BATCH);
    ASSERT_EQ(f.dropped - f0.dropped, RECLAIM_BATCH);
    reclaim_stats(&r);
    ASSERT_EQ(r.scan_direct - r0.scan_direct, RECLAIM_BATCH);
    ASSERT_EQ(r.steal_direct - r0.steal_direct, RECLAIM_BATCH);
    ASSERT_EQ(r.nr_inactive, r0.nr_inactive - RECLAIM_BATCH);
    /* A page read again comes back from the disk */
    ASSERT_EQ(mf_get(as, 5), 0);
    ASSERT_EQ(mf_reads, 41);
    ASSERT_EQ(((uint8_t *)page_to_virt(mf_cached(as, 5)))[0], 6);
    return 0;
}

TEST(active_pages_age_back_then_go) {
    AddressSpace *as = mf_setup();
    ASSERT_TRUE(as != NULL);
    ASSERT_EQ(mf_get(as, 0), 0);
    struct page *pg = mf_cached(as, 0);
    mark_page_accessed(pg);
    mark_page_accessed(pg);
    ASSERT_TRUE(page_test(pg, PG_ACTIVE));
    ReclaimStats r0, r;
    reclaim_stats(&r0);
    ASSERT_EQ(reclaim_pages(1, 0), 1);   /* more active than inactive: one goes back, then out */
    reclaim_stats(&r);
    ASSERT_EQ(r.deactivate - r0.deactivate, 1);
    ASSERT_EQ(r.nr_active, r0.nr_active - 1);
    ASSERT_TRUE(mf_cached(as, 0) == NULL);
    return 0;
}

TEST(locked_and_held_pages_stay) {
    AddressSpace *as = mf_setup();
    ASSERT_TRUE(as != NULL);
    ASSERT_EQ(mf_get(as, 0), 0);
    ASSERT_EQ(mf_get(as, 1), 0);
    struct page *locked = mf_cached(as, 0);
    unsigned off;
    struct page *held = filemap_find(as, 1, &off);   /* a reader's reference */
    ASSERT_TRUE(locked != NULL && held != NULL);
    page_lock(locked);   /* I/O, truncate or a fault has it */
    ASSERT_EQ(reclaim_pages(2, 0), 0);
    ASSERT_TRUE(mf_cached(as, 0) == locked && mf_cached(as, 1) == held);
    ASSERT_TRUE(page_test(locked, PG_LRU) && page_test(held, PG_LRU));   /* still on the lists */
    ASSERT_EQ(reclaim_drop_clean(), 0);
    page_unlock(locked);
    page_put(held);
    ASSERT_EQ(reclaim_pages(2, 0), 2);
    ASSERT_EQ(as->nrpages, 0);
    return 0;
}

TEST(dirty_pages_wait_for_kswapd_or_the_flusher) {
    AddressSpace *as = mf_setup();
    ASSERT_TRUE(as != NULL);
    ASSERT_EQ(mf_dirty(as, 2, 0x5A), 0);
    ReclaimStats r0, r;
    reclaim_stats(&r0);
    /* Direct reclaim never writes: the page stays dirty, the flusher is asked */
    ASSERT_EQ(reclaim_pages(1, 0), 0);
    ASSERT_TRUE(mf_cached(as, 2) != NULL && page_test(mf_cached(as, 2), PG_DIRTY));
    ASSERT_EQ(mf_writes, 0);
    /* kswapd writes it, then frees it */
    ASSERT_EQ(reclaim_pages(1, 1), 1);
    ASSERT_EQ(mf_writes, 1);
    ASSERT_TRUE(mf_cached(as, 2) == NULL);
    ASSERT_EQ(mf_disk[2 * PAGE_SIZE], 0x5A);
    reclaim_stats(&r);
    ASSERT_EQ(r.written - r0.written, 1);
    ASSERT_EQ(r.steal_kswapd - r0.steal_kswapd, 1);
    /* A write that fails leaves it dirty and cached */
    ASSERT_EQ(mf_dirty(as, 3, 0x6B), 0);
    mf_write_fail = 1;
    ASSERT_EQ(reclaim_pages(1, 1), 0);
    ASSERT_TRUE(mf_cached(as, 3) != NULL && page_test(mf_cached(as, 3), PG_DIRTY));
    mf_write_fail = 0;
    wb_sync_all();
    ASSERT_EQ(mf_disk[3 * PAGE_SIZE], 0x6B);
    return 0;
}

/* A mapped page in use survives one look (its accessed bit is cleared); not
 * used again, it is unmapped (TLB flushed) and freed on the next */
TEST(mapped_pages_need_two_idle_looks) {
    AddressSpace *as = mf_setup();
    ASSERT_TRUE(as != NULL);
    Map m;
    mf_map(&m);
    ASSERT_EQ(mf_fault(&m, 0, 0), 0);   /* page 0: mapped and used, the oldest */
    for (uint64_t i = 1; i < MF_PAGES; i++) ASSERT_EQ(mf_get(as, i), 0);
    struct page *p0 = mf_cached(as, 0);
    ASSERT_EQ(filemap_page_young(as, p0), 1);   /* the fault's access */
    ASSERT_EQ(filemap_page_young(as, p0), 0);   /* cleared by the look */
    mf_touch(&m, 0);
    ReclaimStats r0, r;
    reclaim_stats(&r0);
    /* Batch 1 (pages 0-31): page 0 young, kept; 31 freed. Batch 2: 32 more */
    ASSERT_EQ(reclaim_pages(32, 0), 63);
    ASSERT_TRUE(mf_cached(as, 0) == p0 && mf_mapped(&m, 0));
    ASSERT_TRUE(page_test(p0, PG_REFERENCED) && !page_test(p0, PG_ACTIVE));
    reclaim_stats(&r);
    ASSERT_EQ(r.unmapped, r0.unmapped);
    /* Not used since: unmapped and freed */
    int flushes = vfs_env_tlb_flushes;
    ASSERT_EQ(reclaim_pages(1, 0), 1);
    ASSERT_TRUE(mf_cached(as, 0) == NULL && !mf_mapped(&m, 0));
    ASSERT_TRUE(vfs_env_tlb_flushes > flushes);
    reclaim_stats(&r);
    ASSERT_EQ(r.unmapped - r0.unmapped, 1);
    /* The mapping still works: the next access faults it back in */
    ASSERT_EQ(mf_fault(&m, 0, 0), 0);
    ASSERT_TRUE(mf_mapped(&m, 0));
    mf_unmap(&m);
    return 0;
}

/* Used again after its first look, or used through two mappings: active */
TEST(mapped_pages_in_use_activate) {
    AddressSpace *as = mf_setup();
    ASSERT_TRUE(as != NULL);
    Map a, b;
    mf_map(&a);
    mf_map(&b);
    ASSERT_EQ(mf_fault(&a, 0, 0), 0);
    ASSERT_EQ(mf_fault(&a, 1, 0), 0);
    ASSERT_EQ(mf_fault(&b, 1, 0), 0);   /* page 1: two mappings use it */
    for (uint64_t i = 2; i < MF_PAGES; i++) ASSERT_EQ(mf_get(as, i), 0);
    ReclaimStats r0, r;
    reclaim_stats(&r0);
    ASSERT_EQ(reclaim_pages(32, 0), 62);
    struct page *p0 = mf_cached(as, 0), *p1 = mf_cached(as, 1);
    ASSERT_TRUE(p0 != NULL && p1 != NULL);
    ASSERT_TRUE(!page_test(p0, PG_ACTIVE) && page_test(p0, PG_REFERENCED));
    ASSERT_TRUE(page_test(p1, PG_ACTIVE));
    mf_touch(&a, 0);   /* page 0 used again */
    reclaim_stats(&r);
    ASSERT_EQ(r.activate - r0.activate, 1);
    /* Page 0 is now the oldest inactive page, behind 62 newer ones; used
     * since its first look, it goes active, the others go */
    for (uint64_t i = 2; i < MF_PAGES; i++) ASSERT_EQ(mf_get(as, i), 0);
    ASSERT_EQ(reclaim_pages(32, 0), 62);
    ASSERT_TRUE(mf_cached(as, 0) == p0 && page_test(p0, PG_ACTIVE) && mf_mapped(&a, 0));
    reclaim_stats(&r);
    ASSERT_EQ(r.activate - r0.activate, 2);
    /* Not used again, both age back and go; with so few pages left one
     * call scans them over and over (each look is one aging step) */
    ASSERT_EQ(reclaim_pages(2, 0), 2);
    ASSERT_EQ(as->nrpages, 0);
    ASSERT_TRUE(!mf_mapped(&a, 0) && !mf_mapped(&a, 1) && !mf_mapped(&b, 1));
    reclaim_stats(&r);
    ASSERT_EQ(r.deactivate - r0.deactivate, 2);
    mf_unmap(&a);
    mf_unmap(&b);
    return 0;
}

/* drop_caches leaves mapped pages alone (Linux's too) */
TEST(drop_clean_keeps_mapped_pages) {
    AddressSpace *as = mf_setup();
    ASSERT_TRUE(as != NULL);
    Map m;
    mf_map(&m);
    ASSERT_EQ(mf_fault(&m, 0, 0), 0);
    ASSERT_EQ(mf_get(as, 1), 0);
    ASSERT_EQ(mf_dirty(as, 2, 1), 0);
    uint64_t cached = as->nrpages;   /* the fault read ahead */
    ASSERT_EQ(reclaim_drop_clean(), cached - 2);
    ASSERT_TRUE(mf_cached(as, 0) != NULL && mf_mapped(&m, 0));
    ASSERT_TRUE(mf_cached(as, 1) == NULL);
    ASSERT_TRUE(mf_cached(as, 2) != NULL);
    mf_unmap(&m);
    wb_sync_all();
    return 0;
}

/* --- Direct reclaim --- */

static Spinlock test_lock = SPINLOCK_INIT(LOCK_LEVEL_OBJECT, "test");

TEST(direct_reclaim_below_min_only_when_allowed) {
    AddressSpace *as = mf_setup();
    ASSERT_TRUE(as != NULL);
    for (uint64_t i = 0; i < MF_PAGES; i++) ASSERT_EQ(mf_get(as, i), 0);
    reclaim_init();   /* installs the hook; the test's own watermarks below */
    uint64_t free0 = pmm_get_free_pages();
    PmmWatermarks wm = {free0 + 8, free0 + 40, free0 + 48};   /* already below min */
    pmm_set_watermarks(&wm);
    ReclaimStats r0, r;
    reclaim_stats(&r0);
    /* GFP_KERNEL never reclaims: it may be called with anything held */
    struct page *a = alloc_pages(0, GFP_KERNEL);
    ASSERT_TRUE(a != NULL);
    reclaim_stats(&r);
    ASSERT_EQ(r.allocstall, r0.allocstall);
    ASSERT_EQ(as->nrpages, MF_PAGES);
    /* GFP_RECLAIM under a spinlock: not either */
    spin_lock(&test_lock);
    struct page *b = alloc_pages(0, GFP_RECLAIM);
    spin_unlock(&test_lock);
    ASSERT_TRUE(b != NULL);
    reclaim_stats(&r);
    ASSERT_EQ(r.allocstall, r0.allocstall);
    /* GFP_RECLAIM, nothing held: this thread reclaims until free >= min */
    struct page *c = alloc_pages(0, GFP_RECLAIM);
    ASSERT_TRUE(c != NULL);
    reclaim_stats(&r);
    ASSERT_EQ(r.allocstall - r0.allocstall, 1);
    ASSERT_EQ(r.steal_direct - r0.steal_direct, RECLAIM_BATCH);
    ASSERT_TRUE(pmm_get_free_pages() >= wm.min);
    ASSERT_EQ(as->nrpages, MF_PAGES - RECLAIM_BATCH);
    /* Above min, below low: kswapd's job, not this thread's */
    struct page *d = alloc_pages(0, GFP_RECLAIM);
    reclaim_stats(&r);
    ASSERT_EQ(r.allocstall - r0.allocstall, 1);
    free_pages(a, 0);
    free_pages(b, 0);
    free_pages(c, 0);
    free_pages(d, 0);
    pmm_set_pressure_hook(NULL);
    wm.min = wm.low = wm.high = 0;
    pmm_set_watermarks(&wm);
    return 0;
}

/* A failed allocation reclaims even above the watermarks; with nothing
 * reclaimable (only a dirty page) one round ends it, and the retry is the
 * caller's answer (NULL would send a page fault to the OOM killer) */
TEST(direct_reclaim_on_a_failed_allocation) {
    AddressSpace *as = mf_setup();
    ASSERT_TRUE(as != NULL);
    ASSERT_EQ(mf_dirty(as, 0, 7), 0);
    reclaim_init();
    PmmWatermarks wm = {0, 0, 0};
    pmm_set_watermarks(&wm);
    ReclaimStats r0, r;
    reclaim_stats(&r0);
    vfs_env_fail_alloc_in(1);   /* the arena "runs out" for one allocation */
    struct page *pg = alloc_pages(0, GFP_RECLAIM);
    vfs_env_fail_alloc_in(0);
    reclaim_stats(&r);
    ASSERT_EQ(r.allocstall - r0.allocstall, 1);
    ASSERT_EQ(r.steal_direct, r0.steal_direct);
    ASSERT_TRUE(pg != NULL);   /* the retry after the reclaim round got one */
    free_pages(pg, 0);
    ASSERT_TRUE(mf_cached(as, 0) != NULL);
    pmm_set_pressure_hook(NULL);
    wb_sync_all();
    return 0;
}

/* --- Write-back --- */

TEST(flusher_timer_armed_only_while_dirty) {
    AddressSpace *as = mf_setup();
    ASSERT_TRUE(as != NULL);
    WbStats w0, w;
    wb_stats(&w0);
    ASSERT_FALSE(wb_timer_armed());
    ASSERT_EQ(w0.listed, 0);
    /* First dirty page: listed, timer armed once */
    ASSERT_EQ(mf_dirty(as, 0, 0x11), 0);
    ASSERT_EQ(mf_dirty(as, 1, 0x22), 0);
    wb_stats(&w);
    ASSERT_TRUE(wb_timer_armed());
    ASSERT_EQ(w.listed, 1);
    ASSERT_EQ(w.timer_arms - w0.timer_arms, 1);
    ASSERT_EQ(as->wb_state, WB_LISTED);
    /* Not yet: the tick before the deadline does nothing */
    vfs_env_uptime_ms += WB_DELAY_MS - 1;
    wb_tick();
    ASSERT_TRUE(wb_timer_armed());
    /* Its time: the timer fires (and is gone), the flusher is kicked */
    vfs_env_uptime_ms += 1;
    wb_tick();
    ASSERT_FALSE(wb_timer_armed());
    ASSERT_EQ(mf_writes, 0);   /* the flusher thread writes; here, sync stands in */
    wb_sync_all();
    wb_stats(&w);
    ASSERT_EQ(mf_writes, 2);
    ASSERT_EQ(w.written - w0.written, 2);
    ASSERT_EQ(w.listed, 0);
    ASSERT_EQ(w.writeback, 0);
    ASSERT_EQ(as->wb_state, 0);
    ASSERT_FALSE(wb_timer_armed());
    ASSERT_EQ(mf_disk[0], 0x11);
    ASSERT_EQ(mf_disk[PAGE_SIZE], 0x22);
    /* Clean and idle: ticks do nothing, sync writes nothing */
    vfs_env_uptime_ms += 60000;
    wb_tick();
    wb_sync_all();
    ASSERT_EQ(mf_writes, 2);
    ASSERT_FALSE(wb_timer_armed());
    /* Dirty again: armed again */
    ASSERT_EQ(mf_dirty(as, 0, 0x33), 0);
    wb_stats(&w);
    ASSERT_TRUE(wb_timer_armed());
    ASSERT_EQ(w.timer_arms - w0.timer_arms, 2);
    /* Going away: off the list, and with nothing listed the timer is gone */
    mf_ino.nlink = 0;   /* unlinked: nothing to write */
    filemap_inode_evict(&mf_ino);
    wb_stats(&w);
    ASSERT_FALSE(wb_timer_armed());
    ASSERT_EQ(w.listed, 0);
    ASSERT_EQ(mf_writes, 2);
    return 0;
}

TEST(ramfs_never_arms_the_flusher) {
    vfs_env_setup();
    wb_sync_all();
    WbStats w0, w;
    wb_stats(&w0);
    File *f = NULL;
    ASSERT_EQ(vfs_open("/tmp/wb_ramfs", O_RDWR | O_CREAT, 0644, &f), 0);
    IoBuf b = iob_kernel("data", 4);
    ASSERT_EQ(vfs_pwrite(f, &b, 0), 4);
    wb_stats(&w);
    ASSERT_FALSE(wb_timer_armed());
    ASSERT_EQ(w.listed, w0.listed);
    ASSERT_EQ(w.timer_arms, w0.timer_arms);
    fput(f);
    ASSERT_EQ(vfs_unlink("/tmp/wb_ramfs"), 0);
    return 0;
}

TEST(failed_write_back_is_listed_again) {
    AddressSpace *as = mf_setup();
    ASSERT_TRUE(as != NULL);
    ASSERT_EQ(mf_dirty(as, 4, 0x44), 0);
    mf_write_fail = 1;
    wb_sync_all();
    WbStats w;
    wb_stats(&w);
    ASSERT_TRUE(page_test(mf_cached(as, 4), PG_DIRTY));
    ASSERT_EQ(w.listed, 1);   /* dirty again: waits for the next pass */
    ASSERT_TRUE(wb_timer_armed());
    mf_write_fail = 0;
    wb_sync_all();
    ASSERT_EQ(mf_disk[4 * PAGE_SIZE], 0x44);
    ASSERT_FALSE(wb_timer_armed());
    return 0;
}

/* fsync waits for a page another writer has locked for write-back */
TEST(fsync_writes_through_the_list) {
    AddressSpace *as = mf_setup();
    ASSERT_TRUE(as != NULL);
    ASSERT_EQ(mf_dirty(as, 5, 0x55), 0);
    ASSERT_EQ(filemap_fsync(&mf_file), 0);
    ASSERT_EQ(mf_disk[5 * PAGE_SIZE], 0x55);
    ASSERT_FALSE(page_test(mf_cached(as, 5), PG_DIRTY));
    /* Still listed (fsync doesn't touch the list): the next pass finds it clean */
    WbStats w;
    wb_stats(&w);
    ASSERT_EQ(w.listed, 1);
    wb_sync_all();
    ASSERT_EQ(mf_writes, 1);
    ASSERT_FALSE(wb_timer_armed());
    return 0;
}

/* An inode leaving memory while its file is still linked: written first */
TEST(evicting_a_linked_inode_writes_it) {
    AddressSpace *as = mf_setup();
    ASSERT_TRUE(as != NULL);
    FilemapStats f0, f;
    filemap_stats(&f0);   /* other suites' ramfs pages count as dirty too */
    ASSERT_EQ(mf_dirty(as, 6, 0x66), 0);
    ASSERT_EQ(mf_dirty(as, 7, 0x77), 0);
    filemap_inode_evict(&mf_ino);   /* nlink 1 */
    ASSERT_EQ(mf_writes, 2);
    ASSERT_EQ(mf_disk[6 * PAGE_SIZE], 0x66);
    ASSERT_EQ(mf_disk[7 * PAGE_SIZE], 0x77);
    ASSERT_FALSE(wb_timer_armed());
    filemap_stats(&f);
    ASSERT_EQ(f.dirty, f0.dirty);
    ASSERT_EQ(f.file_pages, f0.file_pages);
    return 0;
}

/* Last: leave no file, hook or watermarks behind for later suites */
TEST(cleanup) {
    vfs_env_setup();
    if (mf_ino.mapping != NULL) {
        mf_ino.nlink = 0;
        filemap_inode_evict(&mf_ino);
    }
    pmm_set_pressure_hook(NULL);
    PmmWatermarks wm = {0, 0, 0};
    pmm_set_watermarks(&wm);
    ASSERT_FALSE(wb_timer_armed());
    return 0;
}

TestCase reclaim_tests[] = {
    TEST_ENTRY(watermarks_follow_ram),
    TEST_ENTRY(new_pages_inactive_second_use_activates),
    TEST_ENTRY(small_reads_of_one_page_count_once),
    TEST_ENTRY(clean_pages_go_oldest_first),
    TEST_ENTRY(active_pages_age_back_then_go),
    TEST_ENTRY(locked_and_held_pages_stay),
    TEST_ENTRY(dirty_pages_wait_for_kswapd_or_the_flusher),
    TEST_ENTRY(mapped_pages_need_two_idle_looks),
    TEST_ENTRY(mapped_pages_in_use_activate),
    TEST_ENTRY(drop_clean_keeps_mapped_pages),
    TEST_ENTRY(direct_reclaim_below_min_only_when_allowed),
    TEST_ENTRY(direct_reclaim_on_a_failed_allocation),
    TEST_ENTRY(flusher_timer_armed_only_while_dirty),
    TEST_ENTRY(ramfs_never_arms_the_flusher),
    TEST_ENTRY(failed_write_back_is_listed_again),
    TEST_ENTRY(fsync_writes_through_the_list),
    TEST_ENTRY(evicting_a_linked_inode_writes_it),
    TEST_ENTRY(cleanup),
};
int reclaim_test_count = sizeof(reclaim_tests) / sizeof(reclaim_tests[0]);
