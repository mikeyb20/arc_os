#ifndef ARCHOS_MM_RECLAIM_H
#define ARCHOS_MM_RECLAIM_H

/* Page reclaim (M1-14, ADR-0004, ADR-0016): the page cache gives memory back
 * when free memory runs low. No swap: anonymous pages are never evicted, only
 * page-cache pages of evictable mappings (files on disk, block-device
 * mappings; never ramfs, whose pages are the storage).
 *
 * Cache pages sit on two LRU lists, inactive and active. A new page starts at
 * the head of the inactive list; reclaim takes pages from the tail. A page
 * that is used again moves to the active list: a second read() of it
 * (mark_page_accessed), or a second scan that finds its page-table entries
 * accessed (reverse mapping through the file's VMA list). The active list is
 * kept no longer than the inactive one by moving its tail pages back.
 *
 * Watermarks (pmm.h): an allocation that leaves fewer free pages than low
 * wakes the reclaim thread (kswapd), which reclaims until high. A GFP_RECLAIM
 * allocation that finds fewer than min reclaims directly before it returns.
 * Dirty pages are written by kswapd when it meets them (and by the flusher,
 * mm/writeback.h); direct reclaim skips them and wakes the flusher, and when
 * they are all it found, waits for write-back (WB_THROTTLE_MS at a time, at
 * most WB_THROTTLE_STALLS waits without progress) before it gives up.
 *
 * Locks: the LRU lock (LOCK_LEVEL_PAGE_LRU, spinlock) guards both lists and
 * PG_LRU/PG_ACTIVE. Reclaim takes pages off a list with page_trylock under
 * it, then works on them with the list lock dropped, holding each page's lock
 * (not recorded by the lock checker: it never waits for it) and a reference.
 * While it holds them, nobody can take them out of their file.
 *
 * Cost: every reclaim step handles at most RECLAIM_BATCH pages per hold of
 * the LRU lock; work is proportional to the pages scanned, never to total
 * RAM; kswapd stops at the high watermark; reverse mapping walks only the
 * VMAs that map the page's file. */

#include <stdint.h>
#include "mm/page.h"

#define RECLAIM_BATCH 32   /* pages taken off a list per hold of the LRU lock */

/* Reclaim counters (/proc/vmstat), pages unless noted. */
typedef struct ReclaimStats {
    uint64_t nr_active;       /* on the active list */
    uint64_t nr_inactive;     /* on the inactive list */
    uint64_t scan_kswapd;     /* pages scanned by kswapd */
    uint64_t scan_direct;     /* by direct reclaim */
    uint64_t steal_kswapd;    /* pages freed by kswapd */
    uint64_t steal_direct;    /* by direct reclaim */
    uint64_t activate;        /* moved to the active list */
    uint64_t deactivate;      /* moved back to the inactive list */
    uint64_t unmapped;        /* mapped pages taken out of page tables to be freed */
    uint64_t written;         /* dirty pages kswapd wrote back itself */
    uint64_t allocstall;      /* GFP_RECLAIM allocations that reclaimed directly */
    uint64_t kswapd_wakeups;  /* times kswapd was woken */
} ReclaimStats;

/* Boot, after pmm_init: set the watermarks from the RAM size and install the
 * allocation hook. Direct reclaim works from then on (once threads can
 * block); kswapd from reclaim_start. */
void reclaim_init(void);

/* Start the kswapd thread (after the scheduler is set up). */
void reclaim_start(void);

/* The watermarks for total pages of RAM (min from Linux's formula,
 * sqrt(16 x KiB of RAM), low and high above it by a gap that holds at
 * least two of the largest readahead windows). Pure: host tests call it. */
void reclaim_watermarks_for(uint64_t total_pages, uint64_t *min, uint64_t *low, uint64_t *high);

/* --- The LRU lists (filemap.c) --- */

/* A new page of an evictable mapping went into its tree: onto the inactive
 * list's head. */
void lru_add_page(struct page *pg);

/* A page is leaving its tree (the caller holds its lock): off its list, if it
 * is on one (reclaim may hold it off the lists itself). */
void lru_del_page(struct page *pg);

/* read() used the page: the first use sets PG_REFERENCED, the next one
 * moves an inactive page to the active list. */
void mark_page_accessed(struct page *pg);

/* Write-back cleaned pages: kswapd may find work again after a run that
 * freed nothing. */
void reclaim_note_cleaned(void);

/* A cache page lost its last page-table entry: if kswapd sleeps after a run
 * that freed nothing, it may free this page now. */
void reclaim_note_freeable(void);

/* --- Reclaim --- */

/* Free up to target pages from the LRU lists (kswapd: 1, also writing dirty
 * pages it meets; 0: direct reclaim). Scans in batches, at most about twice
 * the lists' length in one call. Thread context, no spinlock held. Returns
 * pages freed. */
uint64_t reclaim_pages(uint64_t target, int kswapd);

/* Drop every clean, unmapped page of the LRU lists that nobody is using
 * (/proc/sys/vm/drop_caches 1): one pass in batches, the LRU lock dropped
 * between them. Returns pages freed. */
uint64_t reclaim_drop_clean(void);

/* Counters. */
void reclaim_stats(ReclaimStats *out);

#endif /* ARCHOS_MM_RECLAIM_H */
