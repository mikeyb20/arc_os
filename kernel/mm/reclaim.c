/* arc_os — page reclaim: the LRU lists of cache pages, the watermarks, the
 * reclaim thread (kswapd) and direct reclaim (reclaim.h, M1-14)
 *
 * Owns: the inactive and active lists and their counts; the watermarks (set
 * in pmm.c through pmm_set_watermarks); kswapd; the reclaim counters.
 * Invariants: a page of an evictable mapping is on exactly one list (PG_LRU,
 * PG_ACTIVE says which) while it is in its tree, except while reclaim holds
 * it off the lists, locked and with a reference; such a page goes back on a
 * list, or out of its tree, before its lock is released. A page reclaim
 * frees has no page-table entry, no reference but its tree's and reclaim's,
 * is clean and was locked by reclaim throughout.
 * Locks: lru_lock (LOCK_LEVEL_PAGE_LRU) guards the lists; page locks are only
 * ever tried under it. kswapd_lock (LOCK_LEVEL_OBJECT) guards kswapd's
 * wake-up flag. Work on a page (reverse mapping: mapping lock, then each
 * mm's page-table lock; write-back) happens with no spinlock held.
 * Cost: see reclaim.h; the allocation hook is one comparison unless free
 * memory is below low. */

#include "mm/reclaim.h"
#include "mm/filemap.h"
#include "mm/writeback.h"
#include "mm/pmm.h"
#include "proc/spinlock.h"
#include "proc/waitqueue.h"
#include "proc/lockdep.h"
#include "proc/sched.h"
#include "proc/thread.h"
#include "arch/hal_lock.h"
#include "lib/kprintf.h"

/* --- Counters --- */

static ReclaimStats stats;

#define STAT_ADD(field, n) __atomic_add_fetch(&stats.field, (uint64_t)(n), __ATOMIC_RELAXED)

void reclaim_stats(ReclaimStats *out) {
    uint64_t *dst = (uint64_t *)out;
    const uint64_t *src = (const uint64_t *)&stats;
    for (size_t i = 0; i < sizeof(stats) / sizeof(uint64_t); i++) dst[i] = __atomic_load_n(&src[i], __ATOMIC_RELAXED);
}

/* --- The lists: PFN-linked through page->lru (page.h), head = newest --- */

#define LRU_INACTIVE 0
#define LRU_ACTIVE   1

static struct page_list lists[2] = {PAGE_LIST_INIT, PAGE_LIST_INIT};
static Spinlock lru_lock = SPINLOCK_INIT(LOCK_LEVEL_PAGE_LRU, "page LRU");
/* Bumped when there may be something new to reclaim (a page joined a list,
 * write-back cleaned pages): a kswapd run that freed nothing sleeps until
 * this moves */
static uint64_t lru_gen;

static uint64_t *list_count(int which) {
    return which == LRU_ACTIVE ? &stats.nr_active : &stats.nr_inactive;
}

static void list_push_head(int which, struct page *pg) {
    page_list_push(&lists[which], pg);
    __atomic_add_fetch(list_count(which), 1, __ATOMIC_RELAXED);
}

static void list_unlink(int which, struct page *pg) {
    page_list_remove(&lists[which], pg);
    __atomic_sub_fetch(list_count(which), 1, __ATOMIC_RELAXED);
}

static int which_list(const struct page *pg) {
    return page_test(pg, PG_ACTIVE) ? LRU_ACTIVE : LRU_INACTIVE;
}

void lru_add_page(struct page *pg) {
    spin_lock(&lru_lock);
    page_clear(pg, PG_ACTIVE | PG_REFERENCED);
    list_push_head(LRU_INACTIVE, pg);
    page_set(pg, PG_LRU);
    spin_unlock(&lru_lock);
    __atomic_add_fetch(&lru_gen, 1, __ATOMIC_RELAXED);
}

void reclaim_note_cleaned(void) {
    __atomic_add_fetch(&lru_gen, 1, __ATOMIC_RELAXED);
}

void lru_del_page(struct page *pg) {
    spin_lock(&lru_lock);
    if (page_test_and_clear(pg, PG_LRU)) list_unlink(which_list(pg), pg);
    spin_unlock(&lru_lock);
}

void mark_page_accessed(struct page *pg) {
    if (!page_test(pg, PG_REFERENCED)) {
        page_set(pg, PG_REFERENCED);   /* first use since the last scan: no lock */
        return;
    }
    if (page_test(pg, PG_ACTIVE)) return;
    spin_lock(&lru_lock);
    /* Second use: to the active list, unless reclaim holds it off the lists
     * (it sees PG_REFERENCED and decides) */
    if (page_test(pg, PG_LRU) && !page_test(pg, PG_ACTIVE)) {
        list_unlink(LRU_INACTIVE, pg);
        page_set(pg, PG_ACTIVE);
        page_clear(pg, PG_REFERENCED);
        list_push_head(LRU_ACTIVE, pg);
        STAT_ADD(activate, 1);
    }
    spin_unlock(&lru_lock);
}

/* Take up to max pages off the tail of a list, each locked (page_trylock)
 * and with a reference; a page whose lock is taken goes to the head instead.
 * Every page looked at counts in *scanned. */
static uint32_t isolate(int which, struct page **out, uint32_t max, uint64_t *scanned) {
    uint32_t n = 0;
    spin_lock(&lru_lock);
    struct page *pg;
    for (uint32_t i = 0; i < max && (pg = page_list_last(&lists[which])) != NULL; i++) {
        (*scanned)++;
        list_unlink(which, pg);
        if (!page_trylock(pg)) {   /* I/O, truncate, a fault: leave it, try it last next time */
            list_push_head(which, pg);
            continue;
        }
        page_clear(pg, PG_LRU);
        page_ref_get(pg);   /* in its tree (on a list), so alive */
        out[n++] = pg;
    }
    spin_unlock(&lru_lock);
    return n;
}

/* Back onto a list's head, then let go of the page */
static void putback(struct page *pg, int which) {
    spin_lock(&lru_lock);
    if (which == LRU_ACTIVE)
        page_set(pg, PG_ACTIVE);
    else
        page_clear(pg, PG_ACTIVE);
    list_push_head(which, pg);
    page_set(pg, PG_LRU);
    spin_unlock(&lru_lock);
    page_unlock_trylocked(pg);
    page_put(pg);
}

/* --- Scanning --- */

typedef struct ScanCtx {
    int kswapd;          /* 1: may write dirty pages; counters go to kswapd's */
    int unmap;           /* 0: leave mapped pages (drop_caches) */
    uint64_t scanned;
    uint64_t freed;
    uint64_t dirty_seen; /* dirty pages left for the flusher */
} ScanCtx;

/* Free a locked, isolated, clean page: out of every page table, then out of
 * its file if nobody else holds it. 1 if freed (the page is gone, lock and
 * reference included). */
static int try_free(ScanCtx *sc, AddressSpace *as, struct page *pg) {
    if (page_test(pg, PG_DIRTY)) return 0;
    if (__atomic_load_n(&pg->mapcount, __ATOMIC_RELAXED) != 0) {
        if (!sc->unmap) return 0;
        filemap_unmap_page(as, pg);   /* every mm that maps it, with the TLB shootdown */
        STAT_ADD(unmapped, 1);
        /* No new entry can appear: mapping a cache page takes its lock */
        if (__atomic_load_n(&pg->mapcount, __ATOMIC_RELAXED) != 0) return 0;
    }
    if (!filemap_remove_unused(as, pg)) return 0;
    page_unlock_trylocked(pg);
    page_put(pg);   /* reclaim's reference: the last one */
    return 1;
}

/* One batch from the inactive list's tail */
static void shrink_inactive(ScanCtx *sc) {
    struct page *batch[RECLAIM_BATCH];
    uint32_t n = isolate(LRU_INACTIVE, batch, RECLAIM_BATCH, &sc->scanned);
    for (uint32_t i = 0; i < n; i++) {
        struct page *pg = batch[i];
        AddressSpace *as = pg->mapping;   /* locked and in its tree: stable */
        int young = 0;
        if (sc->unmap && __atomic_load_n(&pg->mapcount, __ATOMIC_RELAXED) != 0) young = filemap_page_young(as, pg);
        int refd = page_test_and_clear(pg, PG_REFERENCED);
        if (young != 0) {
            /* Mapped and used: a page used through two scans (or by two
             * mappings) is active, a first use gets one more round */
            if (refd || young > 1) {
                STAT_ADD(activate, 1);
                putback(pg, LRU_ACTIVE);
            } else {
                page_set(pg, PG_REFERENCED);
                putback(pg, LRU_INACTIVE);
            }
            continue;
        }
        if (page_test(pg, PG_DIRTY)) {
            if (!sc->kswapd || filemap_writepage_locked(as, pg) != 0 || page_test(pg, PG_DIRTY)) {
                sc->dirty_seen++;
                putback(pg, LRU_INACTIVE);
                continue;
            }
            STAT_ADD(written, 1);
        }
        if (try_free(sc, as, pg))
            sc->freed++;
        else
            putback(pg, LRU_INACTIVE);
    }
}

/* One batch from the active list's tail: pages whose entries were not
 * accessed since the last look go back to the inactive list */
static void shrink_active(ScanCtx *sc) {
    struct page *batch[RECLAIM_BATCH];
    uint32_t n = isolate(LRU_ACTIVE, batch, RECLAIM_BATCH, &sc->scanned);
    for (uint32_t i = 0; i < n; i++) {
        struct page *pg = batch[i];
        int young = 0;
        if (sc->unmap && __atomic_load_n(&pg->mapcount, __ATOMIC_RELAXED) != 0)
            young = filemap_page_young(pg->mapping, pg);
        page_clear(pg, PG_REFERENCED);
        if (young != 0) {
            putback(pg, LRU_ACTIVE);
        } else {
            STAT_ADD(deactivate, 1);
            putback(pg, LRU_INACTIVE);
        }
    }
}

static uint64_t nr_on_lists(void) {
    return __atomic_load_n(&stats.nr_active, __ATOMIC_RELAXED) + __atomic_load_n(&stats.nr_inactive, __ATOMIC_RELAXED);
}

#define RECLAIM_PRIORITIES 12   /* first round scans 1/4096 of the lists, each next one twice as much */

uint64_t reclaim_pages(uint64_t target, int kswapd) {
    ScanCtx sc = {kswapd, 1, 0, 0, 0};
    for (int prio = RECLAIM_PRIORITIES; prio >= 0 && sc.freed < target; prio--) {
        uint64_t want = nr_on_lists() >> prio;
        if (want < RECLAIM_BATCH) want = RECLAIM_BATCH;
        uint64_t stop = sc.scanned + want;
        while (sc.scanned < stop && sc.freed < target) {
            uint64_t before = sc.scanned;
            if (__atomic_load_n(&stats.nr_active, __ATOMIC_RELAXED) >
                __atomic_load_n(&stats.nr_inactive, __ATOMIC_RELAXED))
                shrink_active(&sc);
            shrink_inactive(&sc);
            if (sc.scanned == before) break;   /* both lists empty */
        }
    }
    if (kswapd) {
        STAT_ADD(scan_kswapd, sc.scanned);
        STAT_ADD(steal_kswapd, sc.freed);
    } else {
        STAT_ADD(scan_direct, sc.scanned);
        STAT_ADD(steal_direct, sc.freed);
    }
    if (sc.dirty_seen != 0) wb_wake();   /* write them, so the next round can free them */
    return sc.freed;
}

uint64_t reclaim_drop_clean(void) {
    ScanCtx sc = {0, 0, 0, 0, 0};
    for (int which = LRU_INACTIVE; which <= LRU_ACTIVE; which++) {
        /* Each page once: the ones kept go to the head, the walk takes the tail */
        uint64_t left = __atomic_load_n(list_count(which), __ATOMIC_RELAXED);
        while (left > 0) {
            struct page *batch[RECLAIM_BATCH];
            uint64_t scanned = 0;
            uint32_t n = isolate(which, batch, left < RECLAIM_BATCH ? (uint32_t)left : RECLAIM_BATCH, &scanned);
            if (scanned == 0) break;
            left = scanned < left ? left - scanned : 0;
            for (uint32_t i = 0; i < n; i++) {
                if (try_free(&sc, batch[i]->mapping, batch[i]))
                    sc.freed++;
                else
                    putback(batch[i], which);
            }
        }
    }
    return sc.freed;
}

/* --- Watermarks (kept by pmm.c: one copy) --- */

static uint64_t isqrt(uint64_t v) {
    uint64_t r = 0;
    for (uint64_t bit = 1ull << 62; bit != 0; bit >>= 2) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
    }
    return r;
}

void reclaim_watermarks_for(uint64_t total_pages, uint64_t *min, uint64_t *low, uint64_t *high) {
    /* Linux's min_free_kbytes: sqrt(16 x KiB of RAM), 128 KiB to 256 MiB */
    uint64_t kib = isqrt(total_pages * (PAGE_SIZE / 1024) * 16);
    if (kib < 128) kib = 128;
    if (kib > 262144) kib = 262144;
    uint64_t m = kib / (PAGE_SIZE / 1024);
    /* low and high: kswapd's room to work before an allocation must reclaim
     * itself. At least two 2 MiB readahead windows, so a reader that crosses
     * low with a window in flight stays above min while kswapd catches up */
    uint64_t gap = m / 4;
    if (gap < total_pages / 512) gap = total_pages / 512;
    if (gap < 1024) gap = 1024;
    *min = m;
    *low = m + gap;
    *high = m + 2 * gap;
}

/* --- kswapd --- */

static Spinlock kswapd_lock = SPINLOCK_INIT(LOCK_LEVEL_OBJECT, "kswapd");
static WaitQueue kswapd_wq;
static int kswapd_wanted;
static Thread *kswapd_thread;
static uint64_t kswapd_failed_gen = UINT64_MAX;   /* lru_gen when a run freed nothing */

/* Wake kswapd unless it is awake already or has nothing new to try. Only
 * with no spinlock held (its locks rank above any caller's). */
static void kswapd_wake(void) {
    if (kswapd_thread == NULL || hal_lock_depth() != 0) return;
    if (__atomic_load_n(&kswapd_wanted, __ATOMIC_RELAXED)) return;
    if (__atomic_load_n(&lru_gen, __ATOMIC_RELAXED) == __atomic_load_n(&kswapd_failed_gen, __ATOMIC_RELAXED)) return;
    uint64_t f;
    spin_lock_irqsave(&kswapd_lock, &f);
    if (!kswapd_wanted) {
        kswapd_wanted = 1;
        STAT_ADD(kswapd_wakeups, 1);
        wq_wake(&kswapd_wq);
    }
    spin_unlock_irqrestore(&kswapd_lock, f);
}

static void kswapd_main(void *arg) {
    (void)arg;
    for (;;) {
        uint64_t f;
        spin_lock_irqsave(&kswapd_lock, &f);
        while (!kswapd_wanted) {
            wq_sleep(&kswapd_wq, &kswapd_lock, f);
            spin_lock_irqsave(&kswapd_lock, &f);
        }
        spin_unlock_irqrestore(&kswapd_lock, f);
        PmmWatermarks wm;
        pmm_get_watermarks(&wm);
        for (;;) {
            uint64_t free = pmm_get_free_pages();
            if (free >= wm.high) break;
            uint64_t gen = __atomic_load_n(&lru_gen, __ATOMIC_RELAXED);
            if (reclaim_pages(wm.high - free, 1) == 0) {
                /* Nothing reclaimable now: sleep until the lists gain pages
                 * or write-back cleans some, not on every allocation */
                __atomic_store_n(&kswapd_failed_gen, gen, __ATOMIC_RELAXED);
                break;
            }
        }
        /* Clear the request last: allocations meanwhile saw it set and
         * didn't wake; the loop above already looked at the free count */
        __atomic_store_n(&kswapd_wanted, 0, __ATOMIC_RELEASE);
        if (pmm_get_free_pages() < wm.low) kswapd_wake();
    }
}

/* --- The allocation hook --- */

#define DIRECT_TRIES 16

static struct page *on_pressure(struct page *got, unsigned order, uint32_t flags) {
    PmmWatermarks wm;
    pmm_get_watermarks(&wm);
    uint64_t free = pmm_get_free_pages();
    if (free < wm.low) kswapd_wake();
    if (!(flags & GFP_RECLAIM) || hal_lock_depth() != 0 || !sched_can_block()) return got;
    if (got != NULL && free >= wm.min) return got;
    /* Below min: this thread reclaims before it goes on */
    STAT_ADD(allocstall, 1);
    for (int i = 0; i < DIRECT_TRIES; i++) {
        uint64_t n = reclaim_pages((uint64_t)RECLAIM_BATCH << order, 0);
        if (got == NULL) got = pmm_alloc_block(order, flags);
        if (got != NULL && pmm_get_free_pages() >= wm.min) break;
        if (n == 0) break;   /* nothing reclaimable: the caller copes (fault: OOM kill) */
    }
    return got;
}

void reclaim_init(void) {
    PmmWatermarks wm;
    reclaim_watermarks_for(pmm_get_total_pages(), &wm.min, &wm.low, &wm.high);
    wq_init(&kswapd_wq);
    pmm_set_watermarks(&wm);
    pmm_set_pressure_hook(on_pressure);
    kprintf("[MM] watermarks: min %lu, low %lu, high %lu pages\n", wm.min, wm.low, wm.high);
}

void reclaim_start(void) {
    Thread *t = thread_create(kswapd_main, NULL);
    if (t == NULL) {
        kprintf("[MM] WARNING: no kswapd thread; reclaim is direct only\n");
        return;
    }
    kswapd_thread = t;
    sched_add_thread(t);
}
