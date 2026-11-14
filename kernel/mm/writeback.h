#ifndef ARCHOS_MM_WRITEBACK_H
#define ARCHOS_MM_WRITEBACK_H

/* Write-back of dirty page-cache pages (M1-14, ADR-0004, ADR-0017).
 *
 * Dirty pages reach their disk:
 *   - on fsync/fdatasync/msync (filemap_write_range, the caller waits);
 *   - when kswapd meets them on the inactive list (reclaim.c);
 *   - on sync(2) and when a mapping goes away (an inode is evicted, a
 *     block-device mapping released: unmount);
 *   - from the flusher thread, WB_DELAY_MS after a mapping gets its first
 *     dirty page. The flusher's timer is armed only while some mapping is
 *     waiting for it: with nothing dirty there is no timer at all, so an idle
 *     machine gets no wake-ups from write-back.
 *
 * Mappings whose pages are the storage (ramfs: no writepage) never wait for
 * the flusher; their pages still count as dirty in /proc/meminfo.
 *
 * Dirty pages that wait for a disk are limited (Linux's dirty ratios): above
 * WB_DIRTY_BACKGROUND_PCT of RAM the flusher starts at once; above
 * WB_DIRTY_LIMIT_PCT a writer waits for write-back before write() returns.
 * Direct reclaim that finds only dirty pages also waits for write-back
 * (wb_wait_progress) before the allocation fails and a fault OOM-kills.
 *
 * sync(2) also flushes the volatile write cache (BLK_OP_FLUSH) of every
 * writable disk that has one, and releasing a block-device mapping (unmount)
 * flushes its disk: the data is on the medium when they return. A file
 * system's fsync flushes its own device (M1-15).
 *
 * A page whose write fails stays dirty and is tried again by the next pass;
 * once its mapping has failed for WB_GIVE_UP_MS without one write getting
 * through (a pulled USB key), failed pages are dropped clean, the error is
 * kept for the mapping's next fsync, and the flusher stops waking for them.
 *
 * A mapping with dirty pages is on the write-back list (WB_LISTED) until the
 * flusher or sync takes it off to write it (WB_BUSY while it does). A page
 * dirtied after that puts the mapping back on the list.
 *
 * Locks: wb_lock (LOCK_LEVEL_WB_LIST, spinlock, IRQ-safe: the timer runs in
 * an interrupt) guards the list, the WB_* flags and the timer; it is taken
 * with a page lock held (a page becoming dirty). wb_mutex
 * (LOCK_LEVEL_WRITEBACK, sleeping, outermost) makes one flush pass or sync
 * at a time, so sync waits for a pass in progress.
 *
 * Cost: making a page dirty is two atomic operations unless its mapping is
 * not on the list yet (then wb_lock once); a flush walks the cached pages of
 * each listed mapping (its radix tree, holes skipped). */

#include <stdint.h>

typedef struct AddressSpace AddressSpace;

#define WB_DELAY_MS 5000   /* a dirty page waits at most this long for the flusher */

#define WB_DIRTY_BACKGROUND_PCT 10   /* dirty pages for a disk above this share of RAM: flush now */
#define WB_DIRTY_LIMIT_PCT      20   /* above this, a writer waits for write-back */
#define WB_THROTTLE_MS          100  /* one wait for write-back progress (Linux's reclaim_throttle) */
#define WB_THROTTLE_STALLS      3    /* waits that saw no progress before a writer or reclaim gives up */
#define WB_GIVE_UP_MS           30000 /* a mapping failing this long: its failed pages are dropped */

/* AddressSpace.wb_state */
#define WB_LISTED 0x1   /* on the write-back list */
#define WB_BUSY   0x2   /* the flusher or sync is writing it now */

/* Write-back counters (/proc/vmstat). */
typedef struct WbStats {
    uint64_t flushes;       /* flusher passes */
    uint64_t timer_arms;    /* times the flusher's timer was armed */
    uint64_t writeback;     /* pages being written now (/proc/meminfo Writeback) */
    uint64_t written;       /* pages written by any write-back path */
    uint64_t listed;        /* mappings on the list now */
    uint64_t throttled;     /* waits for write-back progress (direct reclaim, writers over the limit) */
    uint64_t dropped;       /* dirty pages given up after WB_GIVE_UP_MS of failed writes */
    uint64_t device_flushes; /* BLK_OP_FLUSH sent by sync and release */
} WbStats;

/* Boot: the locks (before any page can become dirty). */
void wb_init(void);

/* Start the flusher thread (after the scheduler is set up). */
void wb_start(void);

/* A page of as just became dirty (filemap_set_dirty, page locked). Lists
 * as and arms the flusher's timer if they are not already. */
void wb_mapping_dirtied(AddressSpace *as);

/* as is going away (its pages are gone or written): take it off the list and
 * wait until no flusher or sync is writing it. Thread context. */
void wb_mapping_forget(AddressSpace *as);

/* Ask the flusher to write everything listed now (reclaim met dirty pages). */
void wb_wake(void);

/* sync(2): write every listed mapping and wait, including a flusher pass
 * already running, then flush every writable disk's write cache. */
void wb_sync_all(void);

/* Wait until write-back writes a page, at most max_ms; wakes the flusher first. Thread context, no spinlock held; sleeping
 * locks may be held (the wait is bounded, so a lock the flusher needs only
 * costs the time). 1 if write-back made progress, 0 if the time ran out. The
 * flusher itself returns 0 at once. */
int wb_wait_progress(uint32_t max_ms);

/* A write dirtied pages of a mapping that is written back (filemap_write;
 * no page lock held): above the background share the flusher starts now,
 * above the limit the writer waits for write-back (giving up after
 * WB_THROTTLE_STALLS waits without progress). */
void wb_balance_dirty(void);

/* The dirty thresholds in pages for total pages of RAM. Pure. */
void wb_dirty_limits_for(uint64_t total_pages, uint64_t *background, uint64_t *limit);

/* 1 while the flusher's timer is armed. */
int wb_timer_armed(void);

/* Count a page write: begin before writepage, end after (a page written
 * wakes wb_wait_progress waiters). */
void wb_page_write_begin(void);
void wb_page_write_end(int err);

/* A write of as failed (err) or worked (0); with an error, returns 1 while
 * the page should stay dirty for another try, 0 once as has failed with
 * -EIO for WB_GIVE_UP_MS (the page is dropped clean, the error kept for
 * fsync). Other errors (-ENOMEM) never start the clock or drop data.
 * Page locked. */
int wb_write_result(AddressSpace *as, int err);

/* Count a BLK_OP_FLUSH sent for write-back (filemap_flush_device). */
void wb_device_flushed(void);

/* The timer tick (interim until M1-06's one-shot timers): fires the
 * flusher's timer once its time has come, and ends wb_wait_progress waits
 * whose time ran out. Interrupt context. */
void wb_tick(void);

/* Counters. */
void wb_stats(WbStats *out);

#endif /* ARCHOS_MM_WRITEBACK_H */
