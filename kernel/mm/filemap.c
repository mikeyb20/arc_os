/* arc_os — the page cache: file pages, reads and writes through them,
 * readahead, truncate, write-back, dropping clean pages (filemap.h, M1-12)
 *
 * Owns: every AddressSpace and the pages in it; the page lock and its wait
 * buckets; the list of evictable pages (LRU, filled here, aged by M1-14's
 * reclaim); the page-cache counters.
 * Invariants: a page is in at most one tree, at its index, with PG_FILE,
 * mapping = its AddressSpace and one reference held by the tree; a page leaves
 * the tree only while locked, and then mapping becomes NULL; nrpages counts
 * the tree's pages; a page of an evictable mapping is on the LRU list (PG_LRU)
 * exactly while it is in the tree; PG_DIRTY pages are counted in `dirty`.
 * Locks: see filemap.h. The index lock (60) and the LRU lock (59) are never
 * held together except by filemap_drop_clean (LRU, then index); everything
 * else takes one, drops it, then the other.
 * Cost: a lookup is one radix walk (<= 4 levels below 64 GiB) under the index
 * lock; a read or write copies each byte once between user memory and the
 * cache page (no bounce buffer); readahead issues windows of 128 KiB to 2 MiB;
 * truncate and write-back walk the pages they touch (the tree skips holes);
 * drop_caches walks the LRU list once. */

#include "mm/filemap.h"
#include "mm/pmm.h"
#include "mm/slab.h"
#include "proc/waitqueue.h"
#include "proc/lockdep.h"
#include "drivers/blkdev.h"
#include "lib/mem.h"
#include "user_access.h"
#include "uapi/errno.h"

/* --- Counters --- */

static FilemapStats stats;

#define STAT_ADD(field, n) __atomic_add_fetch(&stats.field, (uint64_t)(n), __ATOMIC_RELAXED)
#define STAT_SUB(field, n) __atomic_sub_fetch(&stats.field, (uint64_t)(n), __ATOMIC_RELAXED)

void filemap_stats(FilemapStats *out) {
    out->file_pages = __atomic_load_n(&stats.file_pages, __ATOMIC_RELAXED);
    out->dirty = __atomic_load_n(&stats.dirty, __ATOMIC_RELAXED);
    out->mapped = __atomic_load_n(&stats.mapped, __ATOMIC_RELAXED);
    out->pgpgin = __atomic_load_n(&stats.pgpgin, __ATOMIC_RELAXED);
    out->ra_pages = __atomic_load_n(&stats.ra_pages, __ATOMIC_RELAXED);
    out->ra_batches = __atomic_load_n(&stats.ra_batches, __ATOMIC_RELAXED);
    out->dropped = __atomic_load_n(&stats.dropped, __ATOMIC_RELAXED);
}

/* --- Setup --- */

static struct kmem_cache *as_cache;

static void page_waits_init(void);

void filemap_init(void) {
    page_waits_init();
    radix_cache_init();
    as_cache = kmem_cache_create("address_space", sizeof(AddressSpace), 8);
}

static void as_init(AddressSpace *as, const AddressSpaceOps *ops, uint32_t flags) {
    radix_init(&as->tree);
    spin_lock_init(&as->tree_lock, LOCK_LEVEL_PAGE_CACHE, "page cache index");
    mutex_init(&as->map_lock, LOCK_LEVEL_MAPPING);
    as->nrpages = 0;
    as->ops = ops;
    as->flags = flags;
    as->vmas = NULL;
}

int filemap_inode_init(Inode *inode, const AddressSpaceOps *ops, uint32_t flags) {
    AddressSpace *as = kmem_cache_alloc(as_cache, GFP_ZERO);
    if (as == NULL) return -ENOMEM;
    as_init(as, ops, flags);
    as->host = inode;
    inode->mapping = as;
    return 0;
}

loff_t filemap_size(const AddressSpace *as) {
    return as->host != NULL ? __atomic_load_n(&as->host->size, __ATOMIC_ACQUIRE) : as->dev_size;
}

/* Pages needed to hold size bytes */
static uint64_t size_pages(loff_t size) {
    return ((uint64_t)size + PAGE_SIZE - 1) >> PAGE_SHIFT;
}

uint64_t filemap_end_index(const AddressSpace *as) {
    return size_pages(filemap_size(as));
}

/* --- The page lock: a bit in the page, waiters on a hashed wait queue --- */

#define PAGE_WAIT_BUCKETS 64

typedef struct PageWait {
    Spinlock lock;   /* LOCK_LEVEL_OBJECT: the wait queue's condition lock; IRQ-safe (I/O completions) */
    WaitQueue wq;
} PageWait;

static PageWait page_waits[PAGE_WAIT_BUCKETS];

/* At boot (filemap_init), before any page lock is taken */
static void page_waits_init(void) {
    for (int i = 0; i < PAGE_WAIT_BUCKETS; i++) {
        spin_lock_init(&page_waits[i].lock, LOCK_LEVEL_OBJECT, "page wait");
        wq_init(&page_waits[i].wq);
    }
}

static PageWait *wait_of(const struct page *pg) {
    return &page_waits[(page_to_pfn(pg) * 0x9E3779B97F4A7C15ull) >> 58];
}

void page_wait_unlocked(struct page *pg) {
    while (page_test(pg, PG_LOCKED)) {
        PageWait *w = wait_of(pg);
        uint64_t f;
        spin_lock_irqsave(&w->lock, &f);
        if (!page_test(pg, PG_LOCKED)) {   /* unlocked before this thread queued: no lost wakeup */
            spin_unlock_irqrestore(&w->lock, f);
            return;
        }
        wq_sleep(&w->wq, &w->lock, f);
    }
}

void page_lock(struct page *pg) {
    lockdep_sleep_acquire(LOCK_LEVEL_PAGE, "page");
    while (page_test_and_set(pg, PG_LOCKED)) page_wait_unlocked(pg);
}

int page_trylock(struct page *pg) {
    return !page_test_and_set(pg, PG_LOCKED);
}

/* Clear the lock bit and wake whoever waits on this bucket */
static void unlock_bit(struct page *pg) {
    page_clear(pg, PG_LOCKED);
    PageWait *w = wait_of(pg);
    uint64_t f;
    spin_lock_irqsave(&w->lock, &f);
    wq_wake_all(&w->wq);
    spin_unlock_irqrestore(&w->lock, f);
}

void page_unlock(struct page *pg) {
    unlock_bit(pg);
    lockdep_sleep_release(LOCK_LEVEL_PAGE);
}

void filemap_end_read(struct page *pg, int err) {
    if (err == 0) {
        page_clear(pg, PG_ERROR);
        page_set(pg, PG_UPTODATE);
    } else {
        page_set(pg, PG_ERROR);
    }
    unlock_bit(pg);   /* the reader's lock-order record ended when readpage returned */
}

void page_put(struct page *pg) {
    /* Not the last reference: drop it without the allocator's lock (every
     * read and fault puts a page; only the last put frees) */
    uint32_t c = __atomic_load_n(&pg->refcount, __ATOMIC_RELAXED);
    while (c > 1) {
        if (__atomic_compare_exchange_n(&pg->refcount, &c, c - 1, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) return;
    }
    free_pages(pg, page_order(pg));
}

/* --- The evictable-page list --- */

static struct page_list lru = PAGE_LIST_INIT;
static Spinlock lru_lock = SPINLOCK_INIT(LOCK_LEVEL_PAGE_LRU, "page LRU");

static void lru_add(struct page *pg) {
    spin_lock(&lru_lock);
    page_list_push(&lru, pg);
    page_set(pg, PG_LRU);
    spin_unlock(&lru_lock);
}

static void lru_del(struct page *pg) {
    spin_lock(&lru_lock);
    if (page_test_and_clear(pg, PG_LRU)) page_list_remove(&lru, pg);
    spin_unlock(&lru_lock);
}

/* --- The index --- */

struct page *filemap_find(AddressSpace *as, uint64_t index, unsigned *off) {
    spin_lock(&as->tree_lock);
    struct page *pg = radix_lookup(&as->tree, index);
    if (pg != NULL) page_ref_get(pg);
    spin_unlock(&as->tree_lock);
    *off = 0;   /* order-0 pages: index is the page itself */
    return pg;
}

static void blocks_add(AddressSpace *as, int64_t pages) {
    if ((as->flags & AS_MEMBACKED) && as->host != NULL)
        __atomic_add_fetch(&as->host->blocks, (uint64_t)(pages * (PAGE_SIZE / 512)), __ATOMIC_RELAXED);
}

/* A new page for index, locked and in the tree (the caller keeps the
 * allocation's reference; the tree takes another). NULL with *err set:
 * -ENOMEM, or -EEXIST when another thread put one there first. */
static struct page *add_new_page(AddressSpace *as, uint64_t index, int *err) {
    if (index > FILEMAP_MAX_INDEX) {
        *err = -EFBIG;
        return NULL;
    }
    struct page *pg = alloc_pages(0, GFP_KERNEL);
    if (pg == NULL) {
        *err = -ENOMEM;
        return NULL;
    }
    pg->flags |= PG_FILE | PG_LOCKED;   /* nobody else can see it yet */
    pg->mapping = as;
    pg->index = (uint32_t)index;
    pg->mapcount = 0;
    page_ref_get(pg);   /* the tree's */
    spin_lock(&as->tree_lock);
    int r = radix_insert(&as->tree, index, pg);
    if (r == 0) __atomic_add_fetch(&as->nrpages, 1, __ATOMIC_RELAXED);
    spin_unlock(&as->tree_lock);
    if (r != 0) {
        pg->mapping = NULL;
        pg->flags &= ~(PG_FILE | PG_LOCKED);
        page_put(pg);
        page_put(pg);
        *err = r;
        return NULL;
    }
    STAT_ADD(file_pages, 1);
    blocks_add(as, 1);
    if (!(as->flags & AS_MEMBACKED)) lru_add(pg);
    return pg;
}

/* Take a locked page out of its tree; drops the tree's reference (the
 * caller still holds its own). */
static void remove_page(AddressSpace *as, struct page *pg) {
    spin_lock(&as->tree_lock);
    struct page *was = radix_delete(&as->tree, pg->index);
    if (was == pg) __atomic_sub_fetch(&as->nrpages, 1, __ATOMIC_RELAXED);
    spin_unlock(&as->tree_lock);
    if (was != pg) return;   /* not there: never happens while the caller holds the lock and mapping == as */
    lru_del(pg);
    if (page_test_and_clear(pg, PG_DIRTY)) STAT_SUB(dirty, 1);
    pg->mapping = NULL;
    STAT_SUB(file_pages, 1);
    blocks_add(as, -1);
    page_put(pg);
}

/* Take a page this thread just added (locked, never read) out again */
static void discard_new_page(AddressSpace *as, struct page *pg) {
    remove_page(as, pg);
    unlock_bit(pg);   /* a lookup may wait on it: it finds it gone and looks again */
    page_put(pg);
}

void filemap_set_dirty(struct page *pg) {
    if (!page_test_and_set(pg, PG_DIRTY)) STAT_ADD(dirty, 1);
}

void filemap_page_mapped(struct page *pg) {
    if (__atomic_fetch_add(&pg->mapcount, 1, __ATOMIC_RELAXED) == 0) STAT_ADD(mapped, 1);
}

void filemap_page_unmapped(struct page *pg) {
    if (__atomic_sub_fetch(&pg->mapcount, 1, __ATOMIC_RELAXED) == 0) STAT_SUB(mapped, 1);
    page_put(pg);
}

/* --- Reading pages in --- */

/* Start filling a locked page that is in the tree. Ends with the page
 * unlocked once the read is done (today: before this returns). */
static void start_read(AddressSpace *as, struct page *pg) {
    if (as->ops == NULL || as->ops->readpage == NULL) {
        memset(page_to_virt(pg), 0, PAGE_SIZE);   /* a hole of a memory-backed file */
        filemap_end_read(pg, 0);
        return;
    }
    STAT_ADD(pgpgin, 1);
    int err = as->ops->readpage(as, pg);
    if (err != 0) filemap_end_read(pg, err);
}

/* Start filling n new locked pages of consecutive indices, then drop the
 * caller's references (the tree keeps them) */
static void start_batch(AddressSpace *as, struct page **pages, uint32_t n) {
    if (n == 0) return;
    if (as->ops != NULL && as->ops->readpages != NULL) {
        STAT_ADD(pgpgin, n);
        as->ops->readpages(as, pages, n);
    } else {
        for (uint32_t i = 0; i < n; i++) start_read(as, pages[i]);
    }
    for (uint32_t i = 0; i < n; i++) page_put(pages[i]);
}

/* Read [first, first + count) (clamped to the end) into the cache: the pages
 * not cached yet are added locked and read in batches of up to
 * FILEMAP_RA_BATCH consecutive pages. Returns pages read, or -ENOMEM if not
 * even the first could be had. */
static int64_t read_window(AddressSpace *as, uint64_t first, uint64_t count) {
    uint64_t end = filemap_end_index(as);
    if (first >= end) return 0;
    if (count > end - first) count = end - first;
    struct page *batch[FILEMAP_RA_BATCH];
    uint32_t n = 0;
    int64_t read = 0;
    for (uint64_t i = first; i < first + count; i++) {
        unsigned off;
        struct page *pg = filemap_find(as, i, &off);
        if (pg != NULL) {   /* cached: the batch so far ends here (it must be consecutive) */
            page_put(pg);
            start_batch(as, batch, n);
            n = 0;
            continue;
        }
        int err;
        pg = add_new_page(as, i, &err);
        if (pg == NULL) {
            if (err == -EEXIST) {
                start_batch(as, batch, n);
                n = 0;
                continue;
            }
            if (read == 0 && i == first) return err;
            break;   /* out of memory: what was added is read below */
        }
        if (i >= filemap_end_index(as)) {
            /* A truncate set a smaller size after `end` was read, and its
             * walk may have passed i before the page went in: take it out
             * again, or it stays past the end of the file (the size is
             * stored before that walk takes the index lock, and the insert
             * took the lock after it) */
            discard_new_page(as, pg);
            break;
        }
        batch[n++] = pg;
        read++;
        if (n == FILEMAP_RA_BATCH) {
            start_batch(as, batch, n);
            n = 0;
        }
    }
    start_batch(as, batch, n);
    return read;
}

uint64_t filemap_readahead(AddressSpace *as, uint64_t first, uint64_t count) {
    if (as->flags & AS_MEMBACKED) return 0;   /* the pages are the data: nothing to read */
    int64_t n = read_window(as, first, count);
    if (n > 0) {
        STAT_ADD(ra_batches, 1);
        STAT_ADD(ra_pages, n);
    }
    return n > 0 ? (uint64_t)n : 0;
}

/* A miss at index: read it and the window ra says. seq_hint > 0: the reader
 * said sequential (MADV_SEQUENTIAL); < 0: random (MADV_RANDOM, no
 * readahead). Returns 0 or -ENOMEM. */
static int read_on_miss(AddressSpace *as, uint64_t index, FileRa *ra, int seq_hint) {
    uint32_t win = 1;
    if (ra != NULL && seq_hint >= 0 && !(as->flags & AS_MEMBACKED)) {
        if (seq_hint > 0)
            win = FILEMAP_RA_MAX_PAGES;
        else if (ra->size != 0 && index == ra->next)
            win = ra->size * 2 > FILEMAP_RA_MAX_PAGES ? FILEMAP_RA_MAX_PAGES : ra->size * 2;   /* sequential: grow */
        else
            win = FILEMAP_RA_MIN_PAGES;
        ra->size = win;
        ra->next = (uint32_t)(index + win);
    }
    int64_t n = read_window(as, index, win);
    if (n < 0) return (int)n;
    if (win > 1 && n > 1) {
        STAT_ADD(ra_batches, 1);
        STAT_ADD(ra_pages, n - 1);
    }
    return 0;
}

/* Read a locked page again after a failed or missing read; the caller's
 * lock-order record of the page lock ends here. */
static void reread_locked(AddressSpace *as, struct page *pg) {
    page_clear(pg, PG_ERROR);
    start_read(as, pg);
    lockdep_sleep_release(LOCK_LEVEL_PAGE);
}

struct page *filemap_get_page(AddressSpace *as, uint64_t index, FileRa *ra, int seq_hint, unsigned *off, int *err) {
    if (index > FILEMAP_MAX_INDEX) {
        *err = -EFBIG;
        return NULL;
    }
    for (int tries = 0;; tries++) {
        struct page *pg = filemap_find(as, index, off);
        if (pg == NULL) {
            int e = read_on_miss(as, index, ra, seq_hint);
            if (e != 0) {
                *err = e;
                return NULL;
            }
            pg = filemap_find(as, index, off);
            if (pg == NULL) {
                if (tries >= 2 || index >= filemap_end_index(as)) {
                    *err = -ERANGE;   /* past the end (truncated meanwhile) */
                    return NULL;
                }
                continue;
            }
        }
        if (!page_test(pg, PG_UPTODATE)) page_wait_unlocked(pg);
        if (page_test(pg, PG_UPTODATE)) return pg;
        /* The read failed, or a writer gave up on it: try once more */
        if (tries == 0) {
            page_lock(pg);
            if (pg->mapping == as && !page_test(pg, PG_UPTODATE)) {
                reread_locked(as, pg);
            } else {
                page_unlock(pg);
            }
            page_put(pg);
            continue;
        }
        page_put(pg);
        *err = -EIO;
        return NULL;
    }
}

/* --- read(2) and write(2) --- */

int64_t filemap_read(File *file, const IoBuf *buf, loff_t *pos) {
    AddressSpace *as = filemap_of(file->inode);
    loff_t size = file->inode->size;
    if (*pos >= size) return 0;
    uint64_t start = (uint64_t)*pos;
    size_t total = buf->len < (uint64_t)(size - *pos) ? buf->len : (size_t)(size - *pos);
    size_t done = 0;
    int err = 0;
    while (done < total) {
        uint64_t at = start + done;
        uint64_t index = at >> PAGE_SHIFT;
        size_t in_page = (size_t)(at & (PAGE_SIZE - 1));
        size_t n = PAGE_SIZE - in_page;
        if (n > total - done) n = total - done;
        unsigned off = 0;
        struct page *pg;
        if (as->flags & AS_MEMBACKED) {
            pg = filemap_find(as, index, &off);
            /* A page a fault is filling right now (no inode lock there) has
             * no data yet: wait for it; one still not up to date is a hole */
            if (pg != NULL && !page_test(pg, PG_UPTODATE)) page_wait_unlocked(pg);
            if (pg != NULL && !page_test(pg, PG_UPTODATE)) {
                page_put(pg);
                pg = NULL;
            }
            if (pg == NULL) {   /* a hole */
                err = iob_zero(buf, done, n);
                if (err != 0) break;
                done += n;
                continue;
            }
        } else {
            pg = filemap_get_page(as, index, &file->ra, 0, &off, &err);
            if (pg == NULL) break;
        }
        /* A reference keeps the page; the copy may fault (no page lock held) */
        err = iob_copy_out(buf, done, (uint8_t *)page_to_virt(pg) + ((size_t)off << PAGE_SHIFT) + in_page, n);
        page_put(pg);
        if (err != 0) break;
        done += n;
    }
    if (err == -ERANGE) err = 0;   /* truncated under the reader: a short read */
    if (done == 0 && err != 0) return err;
    *pos += (loff_t)done;
    return (int64_t)done;
}

/* Zero the cached bytes of the page holding pos from pos to the page's end
 * (the file's old or new end): bytes past the end of a file are always zero,
 * even after a shared mapping wrote there. */
static void zero_tail(AddressSpace *as, loff_t pos) {
    size_t in_page = (size_t)((uint64_t)pos & (PAGE_SIZE - 1));
    if (in_page == 0) return;
    unsigned off;
    struct page *pg = filemap_find(as, (uint64_t)pos >> PAGE_SHIFT, &off);
    if (pg == NULL) return;
    page_lock(pg);
    if (pg->mapping == as && page_test(pg, PG_UPTODATE))
        memset((uint8_t *)page_to_virt(pg) + ((size_t)off << PAGE_SHIFT) + in_page, 0, PAGE_SIZE - in_page);
    page_unlock(pg);
    page_put(pg);
}

/* The page for a write of n bytes at in_page of page index, locked and
 * either up to date or about to be overwritten whole. NULL with *err. */
static struct page *write_begin(AddressSpace *as, uint64_t index, size_t in_page, size_t n, loff_t size, int *err) {
    int whole = in_page == 0 && n == PAGE_SIZE;
    for (;;) {
        unsigned off;
        struct page *pg = filemap_find(as, index, &off);
        if (pg == NULL) {
            pg = add_new_page(as, index, err);
            if (pg == NULL) {
                if (*err == -EEXIST) continue;
                return NULL;
            }
            lockdep_sleep_acquire(LOCK_LEVEL_PAGE, "page");   /* this thread holds the new page's lock */
            if (!whole && ((as->flags & AS_MEMBACKED) || index >= size_pages(size))) {
                memset(page_to_virt(pg), 0, PAGE_SIZE);   /* a hole, or past the end: zeros */
                page_set(pg, PG_UPTODATE);
            } else if (!whole) {
                reread_locked(as, pg);
                page_put(pg);
                continue;   /* look it up again, now with data */
            }
            return pg;
        }
        page_lock(pg);
        if (pg->mapping != as) {   /* truncated meanwhile */
            page_unlock(pg);
            page_put(pg);
            continue;
        }
        if (!whole && !page_test(pg, PG_UPTODATE)) {
            if (page_test(pg, PG_ERROR)) {
                page_unlock(pg);
                page_put(pg);
                *err = -EIO;
                return NULL;
            }
            reread_locked(as, pg);
            page_wait_unlocked(pg);
            if (!page_test(pg, PG_UPTODATE)) page_set(pg, PG_ERROR);   /* the next round fails with -EIO */
            page_put(pg);
            continue;
        }
        return pg;
    }
}

/* Make sure the user bytes [addr, addr + n) are mapped (n <= one page, so at
 * most two pages). 0, or -EFAULT if the first one can't be. */
static int fault_in_readable(const IoBuf *buf, size_t off, size_t n) {
    if (!buf->user || n == 0) return 0;
    uint8_t c;
    if (copy_from_user(&c, buf->base + off, 1) != 0) return -EFAULT;
    (void)copy_from_user(&c, buf->base + off + n - 1, 1);   /* a later short copy finds out */
    return 0;
}

/* Copy into a locked page: never faults (see filemap.h) */
static size_t copy_in_nofault(void *dst, const IoBuf *b, size_t off, size_t n) {
    if (!b->user) {
        memcpy(dst, b->base + off, n);
        return n;
    }
    return copy_from_user_nofault(dst, b->base + off, n);
}

int64_t filemap_write(File *file, const IoBuf *buf, loff_t *pos) {
    Inode *inode = file->inode;
    AddressSpace *as = filemap_of(inode);
    uint64_t start = (uint64_t)*pos;
    uint64_t limit = (FILEMAP_MAX_INDEX + 1) << PAGE_SHIFT;
    if (start >= limit) return -EFBIG;
    size_t len = buf->len < limit - start ? buf->len : (size_t)(limit - start);
    if ((loff_t)start > inode->size) zero_tail(as, inode->size);   /* the gap reads as zeros */
    size_t done = 0;
    size_t cap = PAGE_SIZE;   /* after a short whole-page copy: write less, so the page is read first */
    int err = 0;
    while (done < len) {
        uint64_t at = start + done;
        uint64_t index = at >> PAGE_SHIFT;
        size_t in_page = (size_t)(at & (PAGE_SIZE - 1));
        size_t n = PAGE_SIZE - in_page;
        if (n > len - done) n = len - done;
        if (n > cap) n = cap;
        cap = PAGE_SIZE;
        err = fault_in_readable(buf, done, n);
        if (err != 0) break;
        loff_t size = inode->size;
        struct page *pg = write_begin(as, index, in_page, n, size, &err);
        if (pg == NULL) break;
        size_t got = copy_in_nofault((uint8_t *)page_to_virt(pg) + in_page, buf, done, n);
        if (got < n && !page_test(pg, PG_UPTODATE)) {
            /* A whole-page write fell short: the rest of the page has no data yet */
            if ((as->flags & AS_MEMBACKED) || index >= size_pages(size)) {
                memset((uint8_t *)page_to_virt(pg) + got, 0, PAGE_SIZE - got);   /* a hole or past the end */
                page_set(pg, PG_UPTODATE);   /* all of it is data now, even if got is 0 */
            } else {
                cap = got != 0 ? got : PAGE_SIZE;   /* next round: read the page, then copy what can be */
                got = 0;
            }
        }
        if (got > 0) {
            page_set(pg, PG_UPTODATE);
            filemap_set_dirty(pg);
            if ((loff_t)(at + got) > inode->size) __atomic_store_n(&inode->size, (loff_t)(at + got), __ATOMIC_RELEASE);
        } else if (index >= size_pages(inode->size) && pg->mapping == as) {
            remove_page(as, pg);   /* nothing written past the end: no page there */
        }
        page_unlock(pg);
        page_put(pg);
        done += got;
        /* Nothing copied although the prefault worked: the buffer was
         * unmapped meanwhile. Stop rather than retry forever. */
        if (got == 0 && cap == PAGE_SIZE) {
            err = -EFAULT;
            break;
        }
    }
    if (done == 0 && err != 0) return err;
    *pos += (loff_t)done;
    return (int64_t)done;
}

/* --- Truncate --- */

/* Drop every page with index >= first. */
static void drop_pages_from(AddressSpace *as, uint64_t first) {
    for (;;) {
        uint64_t idx = 0;
        spin_lock(&as->tree_lock);
        struct page *pg = radix_next(&as->tree, first, &idx);
        if (pg != NULL) page_ref_get(pg);
        spin_unlock(&as->tree_lock);
        if (pg == NULL) return;
        page_lock(pg);
        if (pg->mapping == as) {
            if (__atomic_load_n(&pg->mapcount, __ATOMIC_RELAXED) != 0)
                filemap_unmap_page(as, pg);   /* a fault won the race */
            remove_page(as, pg);
        }
        page_unlock(pg);
        page_put(pg);
        if (idx == UINT64_MAX) return;
        first = idx + 1;
    }
}

void filemap_truncate(Inode *inode, loff_t size) {
    AddressSpace *as = filemap_of(inode);
    loff_t old = inode->size;
    if (size >= old) {
        zero_tail(as, old);
        __atomic_store_n(&inode->size, size, __ATOMIC_RELEASE);
        return;
    }
    __atomic_store_n(&inode->size, size, __ATOMIC_RELEASE);   /* faults from here on see the new end */
    uint64_t first = size_pages(size);
    if (as->vmas != NULL) filemap_unmap_from(as, first);
    drop_pages_from(as, first);
    /* Again: a private write fault that copied a page before the new size
     * showed installs its copy under the page lock, so after drop_pages_from
     * has had every page's lock no new copy can appear past the end */
    if (as->vmas != NULL) filemap_unmap_from(as, first);
    zero_tail(as, size);
}

void filemap_inode_evict(Inode *inode) {
    AddressSpace *as = filemap_of(inode);
    if (as == NULL) return;
    drop_pages_from(as, 0);   /* no VMA maps it: each holds a File reference, so the inode */
    kmem_cache_free(as_cache, as);
    inode->mapping = NULL;
}

/* --- Write-back --- */

int filemap_write_range(AddressSpace *as, uint64_t first, uint64_t last) {
    int ret = 0;
    while (first <= last) {
        uint64_t idx = 0;
        spin_lock(&as->tree_lock);
        struct page *pg = radix_next(&as->tree, first, &idx);
        if (pg != NULL && idx <= last)
            page_ref_get(pg);
        else
            pg = NULL;
        spin_unlock(&as->tree_lock);
        if (pg == NULL) break;
        if (page_test(pg, PG_DIRTY)) {
            page_lock(pg);
            if (pg->mapping == as && page_test(pg, PG_DIRTY)) {
                /* Write-protect first, so a store after the copy faults and dirties it again */
                if (as->vmas != NULL) filemap_mkclean(as, pg);
                if (page_test_and_clear(pg, PG_DIRTY)) STAT_SUB(dirty, 1);
                int e = as->ops != NULL && as->ops->writepage != NULL ? as->ops->writepage(as, pg) : 0;
                if (e != 0) {
                    filemap_set_dirty(pg);
                    if (ret == 0) ret = e;
                }
            }
            page_unlock(pg);
        }
        page_put(pg);
        if (idx == UINT64_MAX) break;
        first = idx + 1;
    }
    return ret;
}

int filemap_fsync(File *file) {
    AddressSpace *as = filemap_of(file->inode);
    return as != NULL ? filemap_write_range(as, 0, UINT64_MAX) : 0;
}

/* --- Dropping clean pages --- */

uint64_t filemap_drop_clean(void) {
    uint64_t freed = 0;
    spin_lock(&lru_lock);
    struct page *pg = page_list_first(&lru);
    while (pg != NULL) {
        struct page *next = pg->lru.next != PFN_NONE ? pfn_to_page(pg->lru.next) : NULL;
        if (!page_test(pg, PG_DIRTY) && __atomic_load_n(&pg->mapcount, __ATOMIC_RELAXED) == 0 && page_trylock(pg)) {
            /* On the list, so still in its tree and its mapping alive */
            AddressSpace *as = pg->mapping;
            int drop = 0;
            spin_lock(&as->tree_lock);
            /* Only the tree's reference: nobody is reading, mapping or waiting on it */
            if (page_ref_count(pg) == 1 && !page_test(pg, PG_DIRTY) && radix_delete(&as->tree, pg->index) == pg) {
                __atomic_sub_fetch(&as->nrpages, 1, __ATOMIC_RELAXED);
                drop = 1;
            }
            spin_unlock(&as->tree_lock);
            if (drop) {
                page_clear(pg, PG_LRU);
                page_list_remove(&lru, pg);
                pg->mapping = NULL;
                STAT_SUB(file_pages, 1);
                STAT_ADD(dropped, 1);
                page_clear(pg, PG_LOCKED);   /* no waiters: they would hold a reference */
                page_put(pg);
                freed++;
            } else {
                unlock_bit(pg);
            }
        }
        pg = next;
    }
    spin_unlock(&lru_lock);
    return freed;
}

/* --- Block-device mappings --- */

/* --- Reads in pieces --- */

void filemap_io_begin(struct page *pg) {
    page_clear(pg, PG_ERROR);
    __atomic_add_fetch(&pg->flags, PG_IO_ONE, __ATOMIC_ACQ_REL);   /* the reader's own share */
}

/* Drop one share; the last one ends the read (any context) */
static void io_put(struct page *pg, int err) {
    if (err != 0) page_set(pg, PG_ERROR);
    uint32_t old = __atomic_fetch_sub(&pg->flags, PG_IO_ONE, __ATOMIC_ACQ_REL);
    if ((old & PG_IO_MASK) == PG_IO_ONE) filemap_end_read(pg, page_test(pg, PG_ERROR) ? -EIO : 0);
}

void filemap_io_end(struct page *pg, int err) {
    io_put(pg, err);
}

void filemap_bdev_read_done(struct page *pg, int err) {
    io_put(pg, err);
}

int filemap_bdev_read(struct BlockDevice *dev, uint64_t sector, uint32_t count, struct page *pg, size_t byte_off) {
    __atomic_add_fetch(&pg->flags, PG_IO_ONE, __ATOMIC_ACQ_REL);
    /* Synchronous until M1-10: the piece is done when the call returns */
    int r = dev->read(dev, sector, count, (uint8_t *)page_to_virt(pg) + byte_off);
    filemap_bdev_read_done(pg, r != 0 ? -EIO : 0);
    return 0;
}

int filemap_bdev_write(struct BlockDevice *dev, uint64_t sector, uint32_t count, struct page *pg, size_t byte_off) {
    return dev->write(dev, sector, count, (uint8_t *)page_to_virt(pg) + byte_off) != 0 ? -EIO : 0;
}

#define BDEV_SECTOR 512u

static uint32_t bdev_sectors(const AddressSpace *as, const struct page *pg) {
    uint64_t start = (uint64_t)pg->index << PAGE_SHIFT;
    uint64_t left = (uint64_t)as->dev_size - start;
    uint64_t n = left < PAGE_SIZE ? left : PAGE_SIZE;
    return (uint32_t)((n + BDEV_SECTOR - 1) / BDEV_SECTOR);
}

static int bdev_readpage(AddressSpace *as, struct page *pg) {
    uint32_t count = bdev_sectors(as, pg);
    memset(page_to_virt(pg), 0, PAGE_SIZE);   /* past a device end that is not page aligned */
    filemap_io_begin(pg);
    int err = filemap_bdev_read(as->bdev, (uint64_t)pg->index * (PAGE_SIZE / BDEV_SECTOR), count, pg, 0);
    filemap_io_end(pg, err);
    return 0;
}

static int bdev_writepage(AddressSpace *as, struct page *pg) {
    return filemap_bdev_write(as->bdev, (uint64_t)pg->index * (PAGE_SIZE / BDEV_SECTOR), bdev_sectors(as, pg), pg, 0);
}

static const AddressSpaceOps bdev_aops = {
    .readpage = bdev_readpage,
    .writepage = bdev_writepage,
};

void filemap_bdev_init(AddressSpace *as, struct BlockDevice *dev, loff_t size) {
    as_init(as, &bdev_aops, 0);
    as->host = NULL;
    as->bdev = dev;
    as->dev_size = size;
}

void filemap_bdev_release(AddressSpace *as) {
    drop_pages_from(as, 0);
}
