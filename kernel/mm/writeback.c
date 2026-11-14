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
 * Cost: listing a mapping is O(1); a pass takes the mappings listed when it
 * started, so a mapping dirtied again and again cannot keep it going. */

#include "mm/writeback.h"
#include "mm/filemap.h"
#include "mm/reclaim.h"
#include "proc/spinlock.h"
#include "proc/mutex.h"
#include "proc/waitqueue.h"
#include "proc/lockdep.h"
#include "proc/sched.h"
#include "proc/thread.h"
#include "arch/hal.h"
#include "lib/kprintf.h"

static WbStats stats;

#define STAT_ADD(field, n) __atomic_add_fetch(&stats.field, (uint64_t)(n), __ATOMIC_RELAXED)
#define STAT_SUB(field, n) __atomic_sub_fetch(&stats.field, (uint64_t)(n), __ATOMIC_RELAXED)

void wb_stats(WbStats *out) {
    uint64_t *dst = (uint64_t *)out;
    const uint64_t *src = (const uint64_t *)&stats;
    for (size_t i = 0; i < sizeof(stats) / sizeof(uint64_t); i++) dst[i] = __atomic_load_n(&src[i], __ATOMIC_RELAXED);
}

void wb_page_write_begin(void) {
    STAT_ADD(writeback, 1);
}

void wb_page_write_end(int err) {
    STAT_SUB(writeback, 1);
    if (err == 0) STAT_ADD(written, 1);
}

static Spinlock wb_lock = SPINLOCK_INIT(LOCK_LEVEL_WB_LIST, "write-back list");
static Mutex wb_mutex;
static AddressSpace *wb_head;   /* oldest first: a pass takes from the head */
static AddressSpace *wb_tail;
static WaitQueue flusher_wq;    /* the flusher waits for wb_kick */
static WaitQueue busy_wq;       /* wb_mapping_forget waits for WB_BUSY to clear */
static int wb_kick;

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
    if (d == 0 || hal_timer_uptime_ms() < d) return;
    spin_lock(&wb_lock);   /* interrupt context: interrupts are off */
    if (__atomic_load_n(&deadline_ms, __ATOMIC_RELAXED) == d) {
        timer_cancel_locked();
        kick_locked();
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

void wb_sync_all(void) {
    mutex_lock(&wb_mutex);   /* after a pass in progress: its writes are done */
    pass();
    mutex_unlock(&wb_mutex);
}

void wb_init(void) {
    mutex_init(&wb_mutex, LOCK_LEVEL_WRITEBACK);
    wq_init(&flusher_wq);
    wq_init(&busy_wq);
}

void wb_start(void) {
    Thread *t = thread_create(flusher_main, NULL);
    if (t == NULL) {
        kprintf("[MM] WARNING: no flusher thread; dirty pages wait for sync or reclaim\n");
        return;
    }
    sched_add_thread(t);
}
