/* arc_os — write-back: the list of mappings with dirty pages, the flusher
 * thread and its timer, sync (writeback.h, M1-14)
 *
 * Owns: the write-back list (AddressSpace.wb_next/wb_prev/wb_state), the
 * flusher thread, the flusher's timer, the write-back counters.
 * Invariants: a mapping is on the list exactly while WB_LISTED is set; the
 * timer is armed whenever the list is not empty and no pass is running that
 * will take every listed mapping; with the list empty it is never armed. A
 * WB_BUSY mapping is alive: wb_mapping_forget waits for the flag to clear
 * before its owner may free it.
 * Locks: wb_lock (LOCK_LEVEL_WB_LIST, IRQ-safe) guards the list, the flags,
 * the flusher's wake-up flag and the timer; wb_mutex (LOCK_LEVEL_WRITEBACK)
 * makes passes one at a time. A mapping is written with neither held except
 * wb_mutex (page locks, the mapping lock, the device below them).
 * Throttling: wb_wait_progress sleeps on progress_wq until a page is
 * written (`progress` moves) or its time runs out (the tick ends waits at
 * wake_at_ms); both under wb_lock.
 * Cost: listing a mapping is O(1); a pass takes the mappings listed when it
 * started, so a mapping dirtied again and again cannot keep it going; a
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
#include "lib/kprintf.h"

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
static AddressSpace *wb_head;   /* oldest first: a pass takes from the head */
static AddressSpace *wb_tail;
static WaitQueue flusher_wq;    /* the flusher waits for wb_kick */
static WaitQueue busy_wq;       /* wb_mapping_forget waits for WB_BUSY to clear */
static int wb_kick;
static Thread *flusher_thread;
static WaitQueue progress_wq;   /* wb_wait_progress waiters */
static uint64_t progress;       /* pages written (under wb_lock) */
static uint64_t wake_at_ms;     /* the earliest waiter's time limit; 0 = nobody waits */

/* Write-back got somewhere: wake the waiters (wb_lock held) */
static void progress_locked(void) {
    progress++;
    if (wake_at_ms != 0) {
        __atomic_store_n(&wake_at_ms, 0, __ATOMIC_RELAXED);   /* each waiter still waiting sets its own again */
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
    uint64_t now = hal_timer_uptime_ms();
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

/* --- The timer (interim: the PIT tick checks a deadline; M1-06 brings
 * one-shot timers, and then this becomes a Timer armed with timer_arm) --- */

static uint64_t deadline_ms;   /* uptime at which the flusher runs; 0 = not armed */

static void kick_locked(void) {
    wb_kick = 1;
    wq_wake(&flusher_wq);
}

static void timer_arm_locked(void) {
    if (__atomic_load_n(&deadline_ms, __ATOMIC_RELAXED) != 0) return;
    uint64_t d = hal_timer_uptime_ms() + WB_DELAY_MS;
    __atomic_store_n(&deadline_ms, d != 0 ? d : 1, __ATOMIC_RELEASE);
    STAT_ADD(timer_arms, 1);
}

static void timer_cancel_locked(void) {
    __atomic_store_n(&deadline_ms, 0, __ATOMIC_RELEASE);
}

int wb_timer_armed(void) {
    return __atomic_load_n(&deadline_ms, __ATOMIC_ACQUIRE) != 0;
}

void wb_tick(void) {
    uint64_t d = __atomic_load_n(&deadline_ms, __ATOMIC_ACQUIRE);
    uint64_t w = __atomic_load_n(&wake_at_ms, __ATOMIC_ACQUIRE);
    if (d == 0 && w == 0) return;
    uint64_t now = hal_timer_uptime_ms();
    if ((d == 0 || now < d) && (w == 0 || now < w)) return;
    spin_lock(&wb_lock);   /* interrupt context: interrupts are off */
    if (d != 0 && now >= d && __atomic_load_n(&deadline_ms, __ATOMIC_RELAXED) == d) {
        timer_cancel_locked();
        kick_locked();
    }
    if (wake_at_ms != 0 && now >= wake_at_ms) {   /* a waiter's time ran out */
        __atomic_store_n(&wake_at_ms, 0, __ATOMIC_RELAXED);
        wq_wake_all(&progress_wq);
    }
    spin_unlock(&wb_lock);
}

/* --- The list --- */

static void wb_list_add(AddressSpace *as) {
    as->wb_next = NULL;
    as->wb_prev = wb_tail;
    if (wb_tail != NULL)
        wb_tail->wb_next = as;
    else
        wb_head = as;
    wb_tail = as;
    STAT_ADD(listed, 1);
}

static void wb_list_del(AddressSpace *as) {
    if (as->wb_prev != NULL)
        as->wb_prev->wb_next = as->wb_next;
    else
        wb_head = as->wb_next;
    if (as->wb_next != NULL)
        as->wb_next->wb_prev = as->wb_prev;
    else
        wb_tail = as->wb_prev;
    as->wb_next = as->wb_prev = NULL;
    STAT_SUB(listed, 1);
    if (wb_head == NULL) timer_cancel_locked();   /* nothing waits for it */
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

/* Take the oldest listed mapping for writing (WB_BUSY), or NULL */
static AddressSpace *take_next(void) {
    uint64_t f;
    spin_lock_irqsave(&wb_lock, &f);
    AddressSpace *as = wb_head;
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

/* Write every mapping listed now (wb_mutex held). Ones listed during the
 * pass wait for the next one (their timer is armed by the listing). */
static void pass(void) {
    uint64_t f;
    spin_lock_irqsave(&wb_lock, &f);
    uint64_t n = __atomic_load_n(&stats.listed, __ATOMIC_RELAXED);
    timer_cancel_locked();   /* this pass takes all of them */
    spin_unlock_irqrestore(&wb_lock, f);
    for (uint64_t i = 0; i < n; i++) {
        AddressSpace *as = take_next();
        if (as == NULL) break;
        (void)filemap_write_range(as, 0, UINT64_MAX);   /* a failed page is dirty again: listed again */
        done_with(as);
    }
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
    if (wb_head != NULL) kick_locked();
    spin_unlock_irqrestore(&wb_lock, f);
}

/* Every writable disk with a volatile write cache writes it out: what the
 * flusher and kswapd wrote earlier may still sit there (sync's flush is
 * Linux's too). A flush changes no data. */
static void flush_disks(void) {
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
    uint64_t until = hal_timer_uptime_ms() + max_ms;
    uint64_t f;
    spin_lock_irqsave(&wb_lock, &f);
    uint64_t seen = progress;
    if (wb_head != NULL) kick_locked();   /* now, not when the 5 s timer fires */
    while (progress == seen && hal_timer_uptime_ms() < until) {
        if (wake_at_ms == 0 || until < wake_at_ms) __atomic_store_n(&wake_at_ms, until, __ATOMIC_RELEASE);
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
