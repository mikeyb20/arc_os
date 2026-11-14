#ifndef ARCHOS_MM_FILEMAP_H
#define ARCHOS_MM_FILEMAP_H

/* The page cache (M1-12, ADR-0004, ADR-0016).
 *
 * Every regular file (and a raw block device, for filesystem metadata) has an
 * AddressSpace: its cached pages, indexed by page number in a radix tree.
 * read(), write() and mmap() all go through these pages, so mapped and read
 * data are always the same bytes. A file page is an ordinary struct page with
 * PG_FILE set, `mapping` pointing at its AddressSpace and `index` its page
 * number; the tree holds one reference, every page-table entry mapping it
 * another (counted in `mapcount` too).
 *
 * Lookups return the page that holds an index plus the index's offset inside
 * it (always 0 today: only order-0 pages are built, ADR-0019, but callers
 * use the offset so 2 MiB file pages can come later without a rewrite).
 *
 * Filling a page: the page is inserted locked and not up to date, and the
 * owner's readpage starts the read: filemap_io_begin, then any number of
 * pieces (filemap_bdev_read: one per contiguous run of sectors, a FAT32
 * cluster piece for example), then filemap_io_end. The page counts the
 * pieces in flight in its flags (PG_IO_MASK); whichever finishes last, the
 * reader's filemap_io_end or a piece's completion, ends the read with
 * filemap_end_read: PG_UPTODATE (PG_ERROR if any piece failed), page
 * unlocked, waiters woken. A piece is one block-layer request (M1-10); its
 * completion calls filemap_bdev_read_done, usually from the disk interrupt,
 * sometimes at once in the submitter (drivers/blkdev.h lists when).
 *
 * Writing a page back runs the same way (M1-15): the writer locks the page,
 * clears PG_DIRTY, sets PG_WRITEBACK and calls writepage, which adds pieces
 * (filemap_write_piece) and returns; the page is unlocked at once and
 * PG_WRITEBACK clears when the last piece ends. Whoever needs the bytes on
 * the disk waits for that (filemap_wait_range_if, filemap_write_range).
 * Removing a page from its mapping waits for its write first, and a page is
 * never under two writes at once. A page's bytes may change while it is
 * written (write(2), a store through a mapping): it is dirty again then, and
 * written again (a filesystem that needs a page to hold still, ext2's
 * metadata, waits for PG_WRITEBACK before changing it).
 *
 * Locks, outermost first (docs/kernel/locking.md):
 *   inode lock (10)      held by read/write/truncate callers
 *   mm lock (20)         held by page faults
 *   page lock (30)       PG_LOCKED: sleeping, one page; I/O, truncate, mapping
 *                        a page into a page table, write-back
 *   mapping lock (40)    AddressSpace.map_lock: the list of VMAs mapping it
 *   page-table lock (50) the mm's, to change entries mapping cache pages
 *   LRU lock (59)        the inactive and active lists (mm/reclaim.c, spinlock)
 *   index lock (60)      AddressSpace.tree_lock: the radix tree (spinlock)
 *   write-back lock (78) the list of mappings with dirty pages (mm/writeback.c;
 *                        IRQ-safe, above every lock held with interrupts on)
 * Never take the mm lock while holding a page lock, and never fault on user
 * memory while holding one: copies into a locked page prefault the user
 * buffer, copy with faults disabled and retry on a short copy. */

#include <stdint.h>
#include <stddef.h>
#include "mm/page.h"
#include "mm/radix.h"
#include "mm/vma.h"
#include "proc/spinlock.h"
#include "proc/mutex.h"
#include "fs/vfs.h"

typedef struct AddressSpace AddressSpace;
struct BlockDevice;
struct BlkPlug;
struct Vma;

/* Filesystem operations on a mapping. */
typedef struct AddressSpaceOps {
    /* Start filling pg (locked, not up to date; its index set): with
     * filemap_io_begin, pieces, filemap_io_end (see above), or with
     * filemap_end_read at once when no I/O is needed. A return value != 0
     * means the read never started: the caller ends it. */
    int (*readpage)(AddressSpace *as, struct page *pg);
    /* Start filling n pages of consecutive indices (each locked, not up to
     * date, in the tree) as one batch, each as readpage does: readahead hands
     * its windows over in batches of up to FILEMAP_RA_BATCH (128 KiB), so
     * adjacent pieces can reach the device as one request. NULL: readpage for
     * each. */
    void (*readpages)(AddressSpace *as, struct page **pages, uint32_t n);
    /* Start writing pg back (locked, up to date, PG_DIRTY cleared,
     * PG_WRITEBACK set; the caller holds a share of the write): add pieces
     * with filemap_write_piece, or none (nothing to write, or written
     * synchronously), and return without waiting. The write ends when the
     * caller's share and every piece have. 0, or -errno: the write failed
     * (the page is dirtied again, or given up: mm/writeback.h). Must not
     * wait for the inode lock (see M1-14). NULL: nothing to write to (ramfs:
     * the pages are the storage). */
    int (*writepage)(AddressSpace *as, struct page *pg);
    /* Before write(2) copies into bytes [from, from + len) of pg (locked,
     * up to date or about to be overwritten whole): give those bytes a place
     * on the disk (a filesystem allocates blocks for holes there, M1-15).
     * Returns 0; 1 when it allocated, and then the page is up to date (a
     * page that wasn't is filled, zeros for the hole) and must reach the
     * disk even if the copy fails, since a new block holds stale bytes until
     * it is written; or -errno (-ENOSPC, -EFBIG): the write stops before this
     * page. NULL: nothing to do. */
    int (*prepare_write)(AddressSpace *as, struct page *pg, size_t from, size_t len);
    /* Start writing every dirty page, in place of a walk in index order
     * (the flusher, sync, releasing a block-device mapping): a filesystem
     * that must order its metadata writes provides it, waiting inside for
     * the writes that must land first (ext2, M1-15); the caller waits for
     * the rest. 0 or the first error. NULL: every dirty page, in index
     * order. */
    int (*writepages)(AddressSpace *as);
} AddressSpaceOps;

/* AddressSpace.flags */
/* The pages are the storage (ramfs): never evicted, a missing page is a hole
 * of zeros, the host's st_blocks counts the pages */
#define AS_MEMBACKED 0x1

struct AddressSpace {
    RadixTree tree;               /* index -> struct page, under tree_lock */
    Spinlock tree_lock;           /* LOCK_LEVEL_PAGE_CACHE */
    uint64_t nrpages;             /* pages in the tree (atomic) */
    const AddressSpaceOps *ops;
    Inode *host;                  /* the file, or NULL for a block-device mapping */
    struct BlockDevice *bdev;     /* block-device mapping: the device */
    loff_t dev_size;              /* block-device mapping: its size in bytes */
    uint32_t flags;               /* AS_* */
    Mutex map_lock;               /* LOCK_LEVEL_MAPPING: vmas */
    struct Vma *vmas;             /* VMAs mapping this file (Vma.as_next), under map_lock */
    AddressSpace *wb_next;        /* write-back list (mm/writeback.c), under its lock */
    AddressSpace *wb_prev;
    uint32_t wb_state;            /* WB_LISTED, WB_BUSY (mm/writeback.h) */
    uint64_t wb_fail_since;       /* uptime ms its writes began failing; 0: the last one worked (atomic) */
    int wb_err;                   /* error of a page given up since the last fsync (atomic) */
};

#define FILEMAP_RA_MIN_PAGES 32    /* 128 KiB: the first window */
#define FILEMAP_RA_MAX_PAGES 512   /* 2 MiB */
#define FILEMAP_RA_BATCH     32    /* pages handed to readpages at once (128 KiB) */

/* Largest page index a file page can have (struct page.index is 32 bits):
 * files stop at 16 TiB. */
#define FILEMAP_MAX_INDEX 0xFFFFFFFEull

/* Page-cache counters (/proc/meminfo, /proc/vmstat), all in pages. */
typedef struct FilemapStats {
    uint64_t file_pages;   /* pages in every page cache */
    uint64_t dirty;        /* of those, dirty */
    uint64_t dirty_wb;     /* of those, in mappings with a disk: what the dirty limit counts (M1-14) */
    uint64_t mapped;       /* of those, mapped by at least one page table */
    uint64_t pgpgin;       /* pages read by readpage */
    uint64_t ra_pages;     /* pages read ahead of the one asked for */
    uint64_t ra_batches;   /* readahead batches started */
    uint64_t dropped;      /* clean pages taken out by drop_caches and reclaim (M1-14) */
} FilemapStats;

/* Boot: the radix-node and address-space slab caches. */
void filemap_init(void);

/* Give a regular-file inode its mapping (inode->mapping). 0 or -ENOMEM. */
int filemap_inode_init(Inode *inode, const AddressSpaceOps *ops, uint32_t flags);

/* The mapping of a regular file (NULL for other inodes). */
static inline AddressSpace *filemap_of(const Inode *inode) {
    return (AddressSpace *)inode->mapping;
}

/* The inode is going away (evict_inode, no references left): drop every
 * page and free the mapping. */
void filemap_inode_evict(Inode *inode);

/* A block-device mapping (the raw device as one file of `size` bytes,
 * 8 sectors per page): for filesystem metadata (ext2, M1-13). Its clean
 * pages can be dropped like a file's; the mapping lives until
 * filemap_bdev_release. ops NULL: the plain device operations; a filesystem
 * that orders its metadata writes passes its own, built on
 * filemap_bdev_readpage and filemap_bdev_writepage (ext2, M1-15). */
void filemap_bdev_init(AddressSpace *as, struct BlockDevice *dev, loff_t size, const AddressSpaceOps *ops);
void filemap_bdev_release(AddressSpace *as);

/* The plain device operations of a block-device mapping, for a filesystem's
 * own table: read a page from the device; start writing one back. */
int filemap_bdev_readpage(AddressSpace *as, struct page *pg);
int filemap_bdev_writepage(AddressSpace *as, struct page *pg);

/* Bytes in the mapping (the file's size, or the device's). */
loff_t filemap_size(const AddressSpace *as);

/* Pages the mapping's size covers: the index of the first page past the end. */
uint64_t filemap_end_index(const AddressSpace *as);

/* --- Pages --- */

/* Lock a page (sleeps). The page lock is a sleeping lock at LOCK_LEVEL_PAGE. */
void page_lock(struct page *pg);
/* Take the lock if it is free. 1 if taken. Never sleeps (usable under spinlocks;
 * not recorded by the lock-order checker). */
int page_trylock(struct page *pg);
/* Release a lock taken with page_lock. */
void page_unlock(struct page *pg);
/* Sleep until the page is unlocked (I/O done). */
void page_wait_unlocked(struct page *pg);

/* Sleep until no write of the page is in flight (PG_WRITEBACK clear). Any
 * thread; before the scheduler runs it polls the disks. */
void filemap_wait_writeback(struct page *pg);

/* End a readpage: PG_UPTODATE on success, PG_ERROR on failure, then unlock
 * and wake waiters. Any context (an I/O completion runs in an interrupt). */
void filemap_end_read(struct page *pg, int err);

/* Drop a page reference (frees the page when it was the last). */
void page_put(struct page *pg);

/* The up-to-date page holding index, with a reference, reading it (and the
 * readahead window ra asks for; NULL: just this page) if needed. *off is
 * index's page offset inside it. NULL with *err = -EIO, -ENOMEM, or -ERANGE
 * (index at or past the end, and not AS_MEMBACKED-with-create). */
struct page *filemap_get_page(AddressSpace *as, uint64_t index, FileRa *ra, int seq_hint, unsigned *off, int *err);

/* The cached page for index with a reference, or NULL; never reads. */
struct page *filemap_find(AddressSpace *as, uint64_t index, unsigned *off);

/* Mark a cache page dirty (counted once). */
void filemap_set_dirty(struct page *pg);

/* --- Files --- */

/* read(2) through the cache. The caller holds the inode lock shared. */
int64_t filemap_read(File *file, const IoBuf *buf, loff_t *pos);

/* write(2) through the cache. The caller holds the inode lock exclusive. */
int64_t filemap_write(File *file, const IoBuf *buf, loff_t *pos);

/* Size change with the inode lock exclusive: shrinking drops the pages past
 * the new end (unmapping them from every process first) and zeroes the rest
 * of the last page; growing zeroes the old last page's tail. */
void filemap_truncate(Inode *inode, loff_t size);

/* Write back the dirty pages with index in [first, last] and wait, also for
 * writes of them already under way (the flusher's, kswapd's). 0 or the first
 * error (-EIO for a write that failed on the disk). */
int filemap_write_range(AddressSpace *as, uint64_t first, uint64_t last);

/* The same for the indices want(ctx, index) says yes to (a filesystem
 * writing its metadata in an order, ext2). want is called with no lock held,
 * for cached pages only. */
int filemap_write_range_if(AddressSpace *as, uint64_t first, uint64_t last, int (*want)(void *ctx, uint64_t index),
                           void *ctx);

/* Start writing the dirty pages in [first, last] that want(ctx, index)
 * says yes to (NULL: all; asked only about dirty or locked pages), without
 * waiting. 0 or the first error a writepage returned. */
int filemap_start_range_if(AddressSpace *as, uint64_t first, uint64_t last, int (*want)(void *ctx, uint64_t index),
                           void *ctx);

/* Wait for the writes in flight of the cached pages in [first, last] that
 * want(ctx, index) says yes to (NULL: all). -EIO if one of them failed
 * since it was last waited for (told once; -ENOSPC when the mapping's kept
 * error says the file system had no block for it), else 0. */
int filemap_wait_range_if(AddressSpace *as, uint64_t first, uint64_t last, int (*want)(void *ctx, uint64_t index),
                          void *ctx);

/* Start writing every dirty page of the mapping, the way its owner writes
 * them (AddressSpaceOps.writepages, else in index order); wait for every
 * write in flight; both. 0 or the first error. */
int filemap_start_all(AddressSpace *as);
int filemap_wait_all(AddressSpace *as);
int filemap_write_all(AddressSpace *as);

/* The page for index made new: locked (recorded for the lock checker),
 * zeroed, up to date, with a reference; whatever it held is gone (a block a
 * filesystem just allocated holds nothing worth reading). NULL with *err
 * (-ENOMEM, -EFBIG). Release with page_unlock and page_put. */
struct page *filemap_grab_zeroed(AddressSpace *as, uint64_t index, int *err);

/* Take the cached page for index out of the mapping, dirty or not (a
 * filesystem freed the block it caches: its bytes must never be written
 * there again). Waits for its lock. Nothing if it isn't cached. */
void filemap_drop_page(AddressSpace *as, uint64_t index);

/* --- For reclaim (mm/reclaim.c): pages it took with page_trylock --- */

/* Start writing one dirty page now: wait for a write of it still in
 * flight, write-protect it in every shared mapping, clear PG_DIRTY, set
 * PG_WRITEBACK, writepage (it is dirty again if that fails). pg locked; the
 * caller unlocks it and need not wait. 0 or writepage's -errno. */
int filemap_writepage_locked(AddressSpace *as, struct page *pg);

/* Take a locked page out of its tree if nothing but the tree and the
 * caller holds a reference (no reader, no page-table entry, no waiter); it
 * must be off the LRU lists and clean. Drops the tree's reference and
 * returns 1 (the page stays locked, the caller's reference is the last), or
 * returns 0 and changes nothing. */
int filemap_remove_unused(AddressSpace *as, struct page *pg);

/* Release a page lock taken with page_trylock (not known to the lock
 * checker). */
void page_unlock_trylocked(struct page *pg);

/* How many page-table entries mapping pg were accessed since the last call:
 * tests and clears their accessed bits, through the VMAs mapping as. pg
 * locked. No TLB flush: a CPU still caching an entry may skip setting the
 * bit again, which only makes the page look older (as Linux). */
int filemap_page_young(AddressSpace *as, struct page *pg);

/* fsync / fdatasync for files whose data lives in the cache: write the
 * dirty pages and wait, flush the device's write cache (block-device
 * mappings; a file system with a disk flushes in its own fsync, M1-15), and
 * report an error kept from a page given up since the last fsync. */
int filemap_fsync(File *file);

/* Send BLK_OP_FLUSH to the device of a block-device mapping (its volatile
 * write cache to the medium) and wait. 0 at once for other mappings and for
 * disks without a write cache; -EIO. */
int filemap_flush_device(AddressSpace *as);

/* The VMA operations of every file mapping (FileOps.mmap_ops of files
 * whose data lives in the cache): faults, msync, MADV_WILLNEED. */
extern const VmaOps filemap_vma_ops;

/* Read [first, first + count) into the cache now (MADV_WILLNEED, readahead
 * of a window). Pages already there are skipped. Returns pages read. */
uint64_t filemap_readahead(AddressSpace *as, uint64_t first, uint64_t count);


/* Counters. */
void filemap_stats(FilemapStats *out);

/* Bookkeeping for page-table entries that map cache pages (mm.c,
 * filemap_mmap.c): a new entry (under the page-table lock, with the entry),
 * and one taken out (also drops the entry's reference: call only after the
 * TLB flush that makes it unreachable). */
void filemap_page_mapped(struct page *pg);
void filemap_page_unmapped(struct page *pg);

/* Through the VMAs that map as (filemap_mmap.c): take every entry for pages
 * >= first out of every process (private copies of them too: truncate);
 * take out the entries that map one page (a private copy made from it stays:
 * reclaim, a fault that raced truncate); write-protect one page in every
 * shared mapping (write-back). The page functions want the page locked. */
void filemap_unmap_from(AddressSpace *as, uint64_t first);
void filemap_unmap_page(AddressSpace *as, struct page *pg);
void filemap_mkclean(AddressSpace *as, struct page *pg);

/* --- Files kept as a list of blocks (filemap_block.c, M1-15) --- */

/* A filesystem's block map, for readpage and writepage. */
typedef struct FilemapBlocks {
    struct BlockDevice *dev;   /* the volume's device; block b starts at sector b << (blkbits - 9) */
    uint32_t blkbits;          /* log2 of the block size: 9 to 16 */
    /* The disk block holding file block lblk of inode: > 0, 0 for a hole,
     * or -errno. With create, a hole gets a block (never 0 then). Called
     * with the page locked and no inode lock. */
    int64_t (*get_block)(Inode *inode, uint64_t lblk, int create);
} FilemapBlocks;

/* readpage / readpages: start filling pg from its blocks (holes and bytes
 * past the end zeros; runs of blocks contiguous on the disk one piece each,
 * on plug, NULL: submitted at once; the block holding the end of the file
 * read synchronously, then its tail zeroed). */
void filemap_blocks_read(AddressSpace *as, struct page *pg, struct BlkPlug *plug, const FilemapBlocks *fb);

/* writepage: start writing pg to its blocks, giving holes a block
 * (get_block with create), one piece per run; blocks past the end of the
 * file are left alone. 0 or -errno (-ENOSPC: no block for a hole). */
int filemap_blocks_write(AddressSpace *as, struct page *pg, const FilemapBlocks *fb);

/* --- Page-cache I/O on a block device (the only calls that reach one) --- */

/* A read of pg in pieces: begin (pg locked, not up to date: no pieces yet,
 * PG_ERROR cleared), any number of filemap_bdev_read, then end. err != 0
 * makes the read fail (an error found without I/O). The read ends when the
 * last of the pieces and the end call finishes. */
void filemap_io_begin(struct page *pg);
void filemap_io_end(struct page *pg, int err);

/* One piece: count sectors from sector into pg at byte_off (inside the page),
 * as one block request, queued on plug (the caller flushes it) or, with plug
 * NULL, submitted at once. Its completion calls filemap_bdev_read_done.
 * Thread context, no spinlock held. Returns 0 when the piece was started
 * (its result arrives through the completion, even a refusal), -ENOMEM when
 * it never was. */
int filemap_bdev_read(struct BlkPlug *plug, struct BlockDevice *dev, uint64_t sector, uint32_t count, struct page *pg,
                      size_t byte_off);

/* A piece finished (any context, an interrupt included): err != 0 fails the
 * read; the last piece ends it. */
void filemap_bdev_read_done(struct page *pg, int err);

/* One piece of a page's write (inside writepage): count sectors at sector
 * from pg at byte_off, submitted at once. Its completion counts it off the
 * write. Returns 0 when started (a refusal arrives through the completion),
 * -ENOMEM when it never was. */
int filemap_write_piece(struct BlockDevice *dev, uint64_t sector, uint32_t count, struct page *pg, size_t byte_off);

#endif /* ARCHOS_MM_FILEMAP_H */
