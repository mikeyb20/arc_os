/* arc_os — write-back: the list of mappings with dirty pages, the flusher
 * thread and its timer, sync (writeback.h, M1-14; two lists and batched
 * asynchronous writes, M1-15)
 *
 * Owns: the write-back lists (AddressSpace.wb_next/wb_prev/wb_state), the
 * flusher thread, the flusher's timer, the write-back counters.
 * A pass: file data first, a cache flush of the disks if any data was
 * written and metadata waits, then the metadata (block-device mappings), so
 * a disk never holds metadata pointing at data not yet on it. Each list is
 * written in batches of WB_BATCH mappings: every page of the batch is
 * started (the writes go out together, the disk queues them), then waited
 * for.
 * Invariants: a mapping is on a list exactly while WB_LISTED is set; the
 * timer is armed whenever the list is not empty and no pass is running that
 * will take every listed mapping; with the list empty it is never armed. A
 * WB_BUSY mapping is alive: wb_mapping_forget waits for the flag to clear
 * before its owner may free it.
 * Locks: wb_lock (LOCK_LEVEL_WB_LIST, IRQ-safe) guards the list, the flags,
 * the flusher's wake-up flag and the timer; wb_mutex (LOCK_LEVEL_WRITEBACK)
 * makes passes one at a time. A mapping is written with neither held except
 * wb_mutex (page locks, the mapping lock, the device below them).
 * Throttling: wb_wait_progress sleeps on progress_wq until a page is
 * written (`progress` moves) or its time runs out (wake_timer, armed for the
 * earliest waiter's wake_at_ns); both under wb_lock.
 * Timers (M1-06): wb_timer (the flusher's 5 s) and wake_timer; wb_lock is
 * the one owner of both (armed and cancelled under it). Their callbacks run
 * in the timer interrupt with no lock held and take wb_lock, so a callback
 * that raced a cancel or re-arm checks its timer is still meant to fire.
 * Cost: listing a mapping is O(1); a pass takes the mappings listed when it
 * started, so a mapping dirtied again and again cannot keep it going (at
 * most WB_BATCH of them are held busy at once); a
 * page written takes wb_lock once; the dirty check of a write is two
 * counter reads below the background share. */

#include "mm/writeback.h"
#include "mm/filemap.h"
#include "mm/reclaim.h"
#include "mm/pmm.h"
#include "proc/spinlock.h"
#include "proc/mutex.h"
#include "proc/waitqueue.h"
#include "proc/lockdep.h"
#include "proc/sched.h"
#include "proc/thread.h"
#include "arch/hal.h"
#include "drivers/blkdev.h"
#include "time/clock.h"
#include "time/timer.h"
#include "lib/kprintf.h"
#include "uapi/errno.h"

static WbStats stats;

#define STAT_ADD(field, n) __atomic_add_fetch(&stats.field, (uint64_t)(n), __ATOMIC_RELAXED)
#define STAT_SUB(field, n) __atomic_sub_fetch(&stats.field, (uint64_t)(n), __ATOMIC_RELAXED)

void wb_stats(WbStats *out) {
    uint64_t *dst = (uint64_t *)out;
    const uint64_t *src = (const uint64_t *)&stats;
    for (size_t i = 0; i < sizeof(stats) / sizeof(uint64_t); i++) dst[i] = __atomic_load_n(&src[i], __ATOMIC_RELAXED);
}

static Spinlock wb_lock = SPINLOCK_INIT(LOCK_LEVEL_WB_LIST, "write-back list");
static Mutex wb_mutex;
/* Two lists, oldest first (a pass takes from the head): file data, and
 * block-device mappings (filesystem metadata), which a pass writes after the
 * data (M1-15: a disk never holds metadata pointing at blocks whose data
 * isn't written yet) */
enum { WB_DATA, WB_META, WB_KINDS };
static AddressSpace *wb_head[WB_KINDS];
static AddressSpace *wb_tail[WB_KINDS];
static uint64_t wb_count[WB_KINDS];
static WaitQueue flusher_wq;    /* the flusher waits for wb_kick */
static WaitQueue busy_wq;       /* wb_mapping_forget waits for WB_BUSY to clear */
static int wb_kick;
static Thread *flusher_thread;
static WaitQueue progress_wq;   /* wb_wait_progress waiters */
static uint64_t progress;       /* pages written (under wb_lock) */
static uint64_t wake_at_ns;     /* the earliest waiter's time limit (clock ns); 0 = nobody waits */
static Timer wake_timer;        /* fires at wake_at_ns */

/* Write-back got somewhere: wake the waiters (wb_lock held) */
static void progress_locked(void) {
    progress++;
    if (wake_at_ns != 0) {
        wake_at_ns = 0;   /* each waiter still waiting sets its own again */
        timer_cancel(&wake_timer);
        wq_wake_all(&progress_wq);
    }
}

void wb_page_write_begin(void) {
    STAT_ADD(writeback, 1);
}

void wb_page_write_end(int err) {
    STAT_SUB(writeback, 1);
    if (err != 0) return;   /* no progress: the page is dirty again (or given up) */
    STAT_ADD(written, 1);
    uint64_t f;
    spin_lock_irqsave(&wb_lock, &f);
    progress_locked();
    spin_unlock_irqrestore(&wb_lock, f);
}

void wb_device_flushed(void) {
    STAT_ADD(device_flushes, 1);
}

int wb_write_result(AddressSpace *as, int err) {
    if (err == 0) {
        if (__atomic_load_n(&as->wb_fail_since, __ATOMIC_RELAXED) != 0)
            __atomic_store_n(&as->wb_fail_since, 0, __ATOMIC_RELAXED);
        return 0;
    }
    if (err == -ENOSPC || err == -EROFS) {
        /* No block for a page dirtied through a mapping (a hole, full disk),
         * or a page dirtied after its volume was shut down (read-only from
         * then on): trying again would only wake the flusher forever.
         * Dropped at once, as Linux does; the next fsync reports it. */
        if (__atomic_exchange_n(&as->wb_err, err, __ATOMIC_RELAXED) == 0)
            kprintf("[WB] %s for a page of %s: dropped\n", err == -ENOSPC ? "no space" : "read-only volume",
                    as->host == NULL && as->bdev != NULL ? as->bdev->name : "a file");
        STAT_ADD(dropped, 1);
        return 0;
    }
    if (err != -EIO) return 1;   /* no memory and the like: not the disk failing, so no clock and no loss */
    uint64_t now = clock_mono_ns() / NSEC_PER_MSEC;
    uint64_t since = 0;
    if (__atomic_compare_exchange_n(&as->wb_fail_since, &since, now != 0 ? now : 1, 0, __ATOMIC_RELAXED,
                                    __ATOMIC_RELAXED))
        return 1;   /* the first failure: try again */
    if (since != UINT64_MAX && now - since < WB_GIVE_UP_MS) return 1;
    if (since != UINT64_MAX) {
        __atomic_store_n(&as->wb_fail_since, UINT64_MAX, __ATOMIC_RELAXED);   /* given up: logged once */
        kprintf("[WB] write-back failing for %u s (%d): dirty pages of %s are dropped\n", WB_GIVE_UP_MS / 1000, err,
                as->host == NULL && as->bdev != NULL ? as->bdev->name : "a file");
    }
    __atomic_store_n(&as->wb_err, err, __ATOMIC_RELAXED);   /* data lost: the next fsync says so */
    STAT_ADD(dropped, 1);
    return 0;
}

/* --- The flusher's timer --- */

static Timer wb_timer;
static int wb_timer_set;   /* armed and not cancelled since (wb_lock): the callback acts only then */

static void kick_locked(void) {
    wb_kick = 1;
    wq_wake(&flusher_wq);
}

static void timer_arm_locked(void) {
    if (wb_timer_set) return;
    __atomic_store_n(&wb_timer_set, 1, __ATOMIC_RELAXED);
    timer_arm(&wb_timer, clock_mono_ns() + (uint64_t)WB_DELAY_MS * NSEC_PER_MSEC);
    STAT_ADD(timer_arms, 1);
}

static void timer_cancel_locked(void) {
    __atomic_store_n(&wb_timer_set, 0, __ATOMIC_RELAXED);
    timer_cancel(&wb_timer);
}

int wb_timer_armed(void) {
    return __atomic_load_n(&wb_timer_set, __ATOMIC_RELAXED);
}

/* Timer interrupt, no lock held */
static void wb_timer_fn(void *arg) {
    (void)arg;
    spin_lock(&wb_lock);   /* interrupts are off */
    if (wb_timer_set && !timer_pending(&wb_timer)) {   /* not cancelled or re-armed meanwhile */
        __atomic_store_n(&wb_timer_set, 0, __ATOMIC_RELAXED);
        kick_locked();
    }
    spin_unlock(&wb_lock);
}

/* A waiter's time ran out (timer interrupt, no lock held) */
static void wake_timer_fn(void *arg) {
    (void)arg;
    spin_lock(&wb_lock);
    if (wake_at_ns != 0 && !timer_pending(&wake_timer)) {
        if (clock_mono_ns() >= wake_at_ns) {
            wake_at_ns = 0;
            wq_wake_all(&progress_wq);
        } else {
            timer_arm(&wake_timer, wake_at_ns);   /* defensive: not reached (wake_at_ns moves only with an arm) */
        }
    }
    spin_unlock(&wb_lock);
}

/* --- The lists --- */

static int kind_of(const AddressSpace *as) {
    return as->host == NULL ? WB_META : WB_DATA;
}

static int lists_empty(void) {
    return wb_head[WB_DATA] == NULL && wb_head[WB_META] == NULL;
}

static void wb_list_add(AddressSpace *as) {
    int k = kind_of(as);
    as->wb_next = NULL;
    as->wb_prev = wb_tail[k];
    if (wb_tail[k] != NULL)
        wb_tail[k]->wb_next = as;
    else
        wb_head[k] = as;
    wb_tail[k] = as;
    wb_count[k]++;
    STAT_ADD(listed, 1);
}

static void wb_list_del(AddressSpace *as) {
    int k = kind_of(as);
    if (as->wb_prev != NULL)
        as->wb_prev->wb_next = as->wb_next;
    else
        wb_head[k] = as->wb_next;
    if (as->wb_next != NULL)
        as->wb_next->wb_prev = as->wb_prev;
    else
        wb_tail[k] = as->wb_prev;
    as->wb_next = as->wb_prev = NULL;
    wb_count[k]--;
    STAT_SUB(listed, 1);
    if (lists_empty()) timer_cancel_locked();   /* nothing waits for it */
}

void wb_mapping_dirtied(AddressSpace *as) {
    /* Pairs with the fence in take_next: either this sees WB_LISTED cleared
     * and lists the mapping again, or the pass that cleared it sees the
     * page's PG_DIRTY (set just before this call) and writes it */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (__atomic_load_n(&as->wb_state, __ATOMIC_RELAXED) & WB_LISTED) return;
    uint64_t f;
    spin_lock_irqsave(&wb_lock, &f);
    if (!(as->wb_state & WB_LISTED)) {
        __atomic_or_fetch(&as->wb_state, WB_LISTED, __ATOMIC_RELAXED);
        wb_list_add(as);
        timer_arm_locked();
    }
    spin_unlock_irqrestore(&wb_lock, f);
}

void wb_mapping_forget(AddressSpace *as) {
    uint64_t f;
    spin_lock_irqsave(&wb_lock, &f);
    while (as->wb_state & WB_BUSY) {
        wq_sleep(&busy_wq, &wb_lock, f);
        spin_lock_irqsave(&wb_lock, &f);
    }
    if (as->wb_state & WB_LISTED) wb_list_del(as);
    __atomic_store_n(&as->wb_state, 0, __ATOMIC_RELAXED);
    spin_unlock_irqrestore(&wb_lock, f);
}

/* Take the oldest listed mapping of a kind for writing (WB_BUSY), or NULL */
static AddressSpace *take_next(int kind) {
    uint64_t f;
    spin_lock_irqsave(&wb_lock, &f);
    AddressSpace *as = wb_head[kind];
    if (as != NULL) {
        wb_list_del(as);
        __atomic_store_n(&as->wb_state, WB_BUSY, __ATOMIC_RELAXED);
    }
    spin_unlock_irqrestore(&wb_lock, f);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);   /* see wb_mapping_dirtied */
    return as;
}

static void done_with(AddressSpace *as) {
    uint64_t f;
    spin_lock_irqsave(&wb_lock, &f);
    __atomic_and_fetch(&as->wb_state, ~(uint32_t)WB_BUSY, __ATOMIC_RELAXED);
    wq_wake_all(&busy_wq);
    spin_unlock_irqrestore(&wb_lock, f);
}

static void flush_disks(void);
static uint64_t written_at_flush;   /* stats.written when flush_disks last ran (wb_mutex) */

#define WB_BATCH 16   /* mappings whose writes are in flight together */

/* Write the first n listed mappings of a kind and wait for them (wb_mutex
 * held): up to WB_BATCH mappings start their writes, then the pass waits
 * for all of them, so the disk sees many requests at once (it merges
 * neighbours) and a pass never ends with its writes still in flight */
static void write_listed(int kind, uint64_t n) {
    AddressSpace *batch[WB_BATCH];
    while (n > 0) {
        uint32_t k = 0;
        for (; k < WB_BATCH && n > 0; n--) {
            AddressSpace *as = take_next(kind);
            if (as == NULL) break;   /* the list ran dry: this round is the last */
            (void)filemap_start_all(as);   /* a failed page is dirty again: listed again */
            batch[k++] = as;
        }
        for (uint32_t i = 0; i < k; i++) {
            (void)filemap_wait_all(batch[i]);
            done_with(batch[i]);
        }
        if (k < WB_BATCH) break;
    }
}

/* Write every mapping listed now (wb_mutex held): file data first, then the
 * metadata, including metadata the data writes just changed (a filesystem
 * allocating blocks for holes in its writepage), with a device cache flush
 * between when both wrote something: on the medium, data comes before the
 * metadata that points at it (M1-15, ADR-0008's write order). Data mappings
 * listed during the pass wait for the next one (their listing arms the
 * timer). */
static void pass(void) {
    uint64_t f;
    spin_lock_irqsave(&wb_lock, &f);
    uint64_t n = wb_count[WB_DATA];
    timer_cancel_locked();   /* this pass takes all of them */
    spin_unlock_irqrestore(&wb_lock, f);
    write_listed(WB_DATA, n);
    spin_lock_irqsave(&wb_lock, &f);
    uint64_t m = wb_count[WB_META];
    spin_unlock_irqrestore(&wb_lock, f);
    /* The data on the medium before the metadata pointing at it: whatever
     * wrote it since the last flush (this pass, or kswapd, an inode leaving
     * memory, earlier) */
    if (m != 0 && __atomic_load_n(&stats.written, __ATOMIC_RELAXED) != written_at_flush) flush_disks();
    write_listed(WB_META, m);
    reclaim_note_cleaned();
}

static void flusher_main(void *arg) {
    (void)arg;
    for (;;) {
        uint64_t f;
        spin_lock_irqsave(&wb_lock, &f);
        while (!wb_kick) {
            wq_sleep(&flusher_wq, &wb_lock, f);
            spin_lock_irqsave(&wb_lock, &f);
        }
        wb_kick = 0;
        spin_unlock_irqrestore(&wb_lock, f);
        mutex_lock(&wb_mutex);
        pass();
        mutex_unlock(&wb_mutex);
        STAT_ADD(flushes, 1);
    }
}

void wb_wake(void) {
    uint64_t f;
    spin_lock_irqsave(&wb_lock, &f);
    if (!lists_empty()) kick_locked();
    spin_unlock_irqrestore(&wb_lock, f);
}

/* Every writable disk with a volatile write cache writes it out: what the
 * flusher and kswapd wrote earlier may still sit there (sync's flush is
 * Linux's too). A flush changes no data. */
static void flush_disks(void) {
    written_at_flush = __atomic_load_n(&stats.written, __ATOMIC_RELAXED);   /* writes ended by now are covered */
    BlockDevice *d;
    for (unsigned i = 0; (d = blk_get(i)) != NULL; i++) {
        if (d->disk != d || (d->flags & (BLK_CACHE | BLK_RDONLY)) != BLK_CACHE) continue;
        STAT_ADD(device_flushes, 1);
        if (blk_rw_sync(d, BLK_OP_FLUSH, 0, NULL, 0) != 0) kprintf("[WB] %s: cache flush failed\n", d->name);
    }
}

void wb_sync_all(void) {
    mutex_lock(&wb_mutex);   /* after a pass in progress: its writes are done */
    pass();
    flush_disks();
    mutex_unlock(&wb_mutex);
}

/* --- Throttling: waiting for write-back --- */

int wb_wait_progress(uint32_t max_ms) {
    if (flusher_thread != NULL && thread_current() == flusher_thread) return 0;   /* it would wait for itself */
    STAT_ADD(throttled, 1);
    uint64_t until = clock_mono_ns() + (uint64_t)max_ms * NSEC_PER_MSEC;
    uint64_t f;
    spin_lock_irqsave(&wb_lock, &f);
    uint64_t seen = progress;
    if (!lists_empty()) kick_locked();   /* now, not when the 5 s timer fires */
    while (progress == seen && clock_mono_ns() < until) {
        if (wake_at_ns == 0 || until < wake_at_ns) {
            wake_at_ns = until;
            timer_arm(&wake_timer, until);
        }
        wq_sleep(&progress_wq, &wb_lock, f);
        spin_lock_irqsave(&wb_lock, &f);
    }
    int moved = progress != seen;
    spin_unlock_irqrestore(&wb_lock, f);
    return moved;
}

void wb_dirty_limits_for(uint64_t total_pages, uint64_t *background, uint64_t *limit) {
    *background = total_pages * WB_DIRTY_BACKGROUND_PCT / 100;
    *limit = total_pages * WB_DIRTY_LIMIT_PCT / 100;
}

static uint64_t dirty_for_disks(void) {
    FilemapStats fs;
    filemap_stats(&fs);
    return fs.dirty_wb;
}

void wb_balance_dirty(void) {
    uint64_t background, limit;
    wb_dirty_limits_for(pmm_get_total_pages(), &background, &limit);
    if (dirty_for_disks() <= background) return;
    wb_wake();
    for (int stalls = 0; stalls < WB_THROTTLE_STALLS && dirty_for_disks() > limit;) {
        if (!wb_wait_progress(WB_THROTTLE_MS)) stalls++;
    }
}

void wb_init(void) {
    mutex_init(&wb_mutex, LOCK_LEVEL_WRITEBACK);
    wq_init(&flusher_wq);
    wq_init(&busy_wq);
    wq_init(&progress_wq);
    timer_init(&wb_timer, wb_timer_fn, NULL);
    timer_init(&wake_timer, wake_timer_fn, NULL);
}

void wb_start(void) {
    Thread *t = thread_create(flusher_main, NULL);
    if (t == NULL) {
        kprintf("[MM] WARNING: no flusher thread; dirty pages wait for sync or reclaim\n");
        return;
    }
    flusher_thread = t;
    sched_add_thread(t);
}
