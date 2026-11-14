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
 * already running. */
void wb_sync_all(void);

/* 1 while the flusher's timer is armed. */
int wb_timer_armed(void);

/* Count a page write: begin before writepage, end after. */
void wb_page_write_begin(void);
void wb_page_write_end(int err);

/* The timer tick (interim until M1-06's one-shot timers): fires the
 * flusher's timer once its time has come. Interrupt context. */
void wb_tick(void);

/* Counters. */
void wb_stats(WbStats *out);

#endif /* ARCHOS_MM_WRITEBACK_H */
