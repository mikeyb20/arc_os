/* arc_os — ext2 block map and file pages (ext2_internal.h; read M1-13,
 * write M1-15)
 *
 * Owns: the block map of an inode (12 direct blocks, then indirect, double
 * and triple indirect blocks), giving holes blocks, freeing blocks past a
 * size (truncate), and the regular file's operations: read and write
 * through the page cache (pages to blocks: mm/filemap_block.c), fsync,
 * setattr.
 *
 * The block map keeps, per inode, the last-level indirect block it used last
 * (`ind_cache`: which file blocks it covers and where it is): a sequential
 * read of a large file then costs one entry lookup per block instead of up
 * to three indirect reads. Truncate clears it (the block it names may be
 * freed); giving a hole a block never changes which indirect block covers a
 * file block, so the cache stays right.
 *
 * When blocks are given: write(2) gives a hole its block before copying into
 * the page (prepare_write, with the page locked), so the page is never
 * written to a block it doesn't own and a read never sees a block that was
 * just allocated but not written yet; a page dirtied through a shared
 * mapping over a hole gets its block in writepage. A file written in order
 * gets its blocks in order (next_lblk / next_goal). New indirect blocks are
 * zeroed on the disk before anything points at them (ext2_meta_new_done).
 *
 * Crash: the write-back pass writes data before the metadata that points at
 * it, but a block allocated after a pass took its list of files can reach
 * the disk in that pass's metadata round before its data: after a crash the
 * file may show the block's old bytes. That is ext2's known weakness (ext3's
 * ordered journal is the fix, later); e2fsck finds nothing wrong with it.
 *
 * Locks: block-map changes and truncate run under alloc_lock; lookups take
 * no lock (see ext2.c). A file page's lock is held around prepare_write and
 * writepage, outside alloc_lock.
 *
 * Cost: a lookup is at most 3 metadata page lookups (1 on an ind_cache hit);
 * giving a hole a block adds a bitmap search and at most 3 new indirect
 * blocks (each written synchronously once); truncate reads every indirect
 * block past the new end once and frees runs of contiguous blocks at a time.
 * fsync writes every dirty metadata page of the volume (not only the
 * file's) and flushes the disk twice. */

#include "fs/ext2_internal.h"
#include "drivers/blkdev.h"
#include "lib/mem.h"
#include "lib/kprintf.h"
#include "uapi/errno.h"

/* --- Lookup --- */

/* Entry idx of indirect block `block`: 0 (a hole) or a block number. */
static int64_t ind_entry(Ext2Vol *v, uint32_t block, uint32_t idx) {
    if (block >= v->blocks_count) return ext2_corrupt(v, "indirect block", block);
    uint32_t e;
    int err = ext2_meta_read(v, ext2_block_at(v, block) + (uint64_t)idx * 4, &e, sizeof(e));
    if (err != 0) return err;
    if (e >= v->blocks_count) return ext2_corrupt(v, "block number", e);
    return e;
}

/* Where file block lblk sits in the map: depth 0 (direct, slot lblk) or
 * 1-3 (the tree at slot 12-14, rel the block's index inside it). -1 past
 * the triple indirect tree. */
static int map_path(const Ext2Vol *v, uint32_t lblk, uint32_t *slot, uint64_t *rel) {
    uint32_t sh = v->addr_shift;
    uint64_t apb = 1ull << sh;
    if (lblk < EXT2_NDIR_BLOCKS) {
        *slot = lblk;
        *rel = 0;
        return 0;
    }
    uint64_t r = lblk - EXT2_NDIR_BLOCKS;
    for (int depth = 1; depth <= 3; depth++) {
        uint64_t span = 1ull << (depth * sh);
        if (r < span) {
            *slot = EXT2_IND_BLOCK + (uint32_t)depth - 1;
            *rel = r;
            return depth;
        }
        r -= span;
    }
    (void)apb;
    return -1;
}

int64_t ext2_bmap(Inode *inode, uint32_t lblk) {
    Ext2Vol *v = ext2_vol(inode);
    Ext2Inode *ei = ext2_ei(inode);
    if (lblk < EXT2_NDIR_BLOCKS) {
        uint32_t b = __atomic_load_n(&ei->block[lblk], __ATOMIC_ACQUIRE);
        return b < v->blocks_count ? (int64_t)b : ext2_corrupt(v, "block number", b);
    }
    uint32_t sh = v->addr_shift;
    uint64_t apb = 1ull << sh;
    /* A last-level indirect block used just before (sequential reads) */
    uint64_t c = __atomic_load_n(&ei->ind_cache, __ATOMIC_ACQUIRE);
    if (c != 0 && lblk >= (uint32_t)(c >> 32) && lblk - (uint32_t)(c >> 32) < apb)
        return ind_entry(v, (uint32_t)c, lblk - (uint32_t)(c >> 32));
    uint32_t slot;
    uint64_t rel;
    int depth = map_path(v, lblk, &slot, &rel);
    if (depth < 0) return ext2_corrupt(v, "file block", lblk);   /* beyond triple indirect */
    uint32_t blk = __atomic_load_n(&ei->block[slot], __ATOMIC_ACQUIRE);
    for (int d = depth; d > 0; d--) {
        if (blk == 0) return 0;   /* a hole covers the rest */
        uint32_t idx = (uint32_t)(rel >> ((d - 1) * sh)) & (uint32_t)(apb - 1);
        if (d == 1) __atomic_store_n(&ei->ind_cache, ((uint64_t)(lblk - idx) << 32) | blk, __ATOMIC_RELEASE);
        int64_t next = ind_entry(v, blk, idx);
        if (next < 0) return next;
        blk = (uint32_t)next;
    }
    return blk;
}

/* --- Giving holes blocks (alloc_lock held) --- */

/* Store value into entry idx of indirect block blk */
static int ind_set(Ext2Vol *v, uint32_t blk, uint32_t idx, uint32_t value) {
    struct page *pg;
    int err;
    uint32_t *e = (uint32_t *)ext2_meta_lock(v, ext2_block_at(v, blk) + (uint64_t)idx * 4, &pg, &err);
    if (e == NULL) return err;
    __atomic_store_n(e, value, __ATOMIC_RELEASE);
    ext2_meta_done(pg, 1);
    return 0;
}

/* A new indirect block near goal, zeroed on the disk: its number, or -errno */
static int64_t new_indirect(Inode *inode, uint32_t goal) {
    Ext2Vol *v = ext2_vol(inode);
    uint32_t b;
    int err = ext2_alloc_block(v, goal, &b);
    if (err != 0) return err;
    struct page *pg;
    if (ext2_meta_new(v, b, &pg, &err) != NULL) err = ext2_meta_new_done(v, pg, 1);
    if (err != 0) {
        ext2_free_blocks(v, b, 1, 1);
        return err;
    }
    __atomic_add_fetch(&inode->blocks, v->block_size >> 9, __ATOMIC_RELAXED);
    return b;
}

/* Where a new block for file block lblk should go */
static uint32_t goal_for(Inode *inode, uint32_t lblk) {
    Ext2Vol *v = ext2_vol(inode);
    Ext2Inode *ei = ext2_ei(inode);
    if (ei->next_goal != 0 && lblk == ei->next_lblk) return ei->next_goal;
    if (lblk > 0) {
        int64_t p = ext2_bmap(inode, lblk - 1);
        if (p > 0) return (uint32_t)p + 1;
    }
    uint32_t g = (uint32_t)(inode->ino - 1) / v->inodes_per_group;   /* the inode's group */
    return v->first_data_block + g * v->blocks_per_group;
}

int64_t ext2_map_alloc(Inode *inode, uint32_t lblk, uint32_t newblk, int *fresh) {
    Ext2Vol *v = ext2_vol(inode);
    Ext2Inode *ei = ext2_ei(inode);
    *fresh = 0;
    int64_t have = ext2_bmap(inode, lblk);
    if (have != 0) return have;   /* mapped already, or an error */
    uint32_t slot;
    uint64_t rel;
    int depth = map_path(v, lblk, &slot, &rel);
    if (depth < 0) return -EFBIG;
    uint32_t sh = v->addr_shift;
    uint32_t goal = goal_for(inode, lblk);
    /* The indirect blocks on the way first, each written before it is linked */
    uint32_t parent = 0, idx = slot;   /* where the next block is linked: the inode (parent 0) or a block */
    for (int d = depth; d > 0; d--) {
        uint32_t cur = parent == 0 ? __atomic_load_n(&ei->block[idx], __ATOMIC_ACQUIRE) : 0;
        if (parent != 0) {
            int64_t e = ind_entry(v, parent, idx);
            if (e < 0) return e;
            cur = (uint32_t)e;
        }
        if (cur == 0) {
            int64_t n = new_indirect(inode, goal);
            if (n < 0) return n;
            int err = 0;
            if (parent == 0)
                __atomic_store_n(&ei->block[idx], (uint32_t)n, __ATOMIC_RELEASE);
            else
                err = ind_set(v, parent, idx, (uint32_t)n);
            if (err != 0) {
                ext2_free_blocks(v, (uint32_t)n, 1, 1);
                __atomic_sub_fetch(&inode->blocks, v->block_size >> 9, __ATOMIC_RELAXED);
                return err;
            }
            cur = (uint32_t)n;
            goal = cur + 1;
        }
        parent = cur;
        idx = (uint32_t)(rel >> ((d - 1) * sh)) & ((1u << sh) - 1);
    }
    /* The data block */
    uint32_t data = newblk;
    if (data == 0) {
        int err = ext2_alloc_block(v, goal, &data);
        if (err != 0) {
            if (parent != 0) ext2_write_inode(inode);   /* the new indirect blocks stay linked (empty) */
            return err;
        }
    }
    int err = 0;
    if (parent == 0)
        __atomic_store_n(&ei->block[idx], data, __ATOMIC_RELEASE);
    else
        err = ind_set(v, parent, idx, data);
    if (err != 0) {
        if (newblk == 0) ext2_free_blocks(v, data, 1, 0);
        ext2_write_inode(inode);
        return err;
    }
    __atomic_add_fetch(&inode->blocks, v->block_size >> 9, __ATOMIC_RELAXED);
    *fresh = newblk == 0;
    ei->next_lblk = lblk + 1;
    ei->next_goal = data + 1;
    ext2_write_inode(inode);
    return data;
}

int64_t ext2_get_block(Inode *inode, uint64_t lblk, int create) {
    Ext2Vol *v = ext2_vol(inode);
    if (lblk > UINT32_MAX) return create ? -EFBIG : ext2_corrupt(v, "file size", (uint64_t)inode->size);
    int64_t p = ext2_bmap(inode, (uint32_t)lblk);
    if (p != 0 || !create) return p;
    if (!v->writable) return -EROFS;
    int fresh;
    mutex_lock(&v->alloc_lock);
    p = ext2_map_alloc(inode, (uint32_t)lblk, 0, &fresh);
    mutex_unlock(&v->alloc_lock);
    return p;
}

/* --- Truncate (alloc_lock held) --- */

/* Blocks to free, gathered into runs of contiguous blocks */
typedef struct {
    Inode *inode;
    uint32_t first, count;
    int meta;
} FreeRun;

static void run_flush(FreeRun *r) {
    if (r->count == 0) return;
    Ext2Vol *v = ext2_vol(r->inode);
    ext2_free_blocks(v, r->first, r->count, r->meta);
    uint64_t units = (uint64_t)r->count * (v->block_size >> 9);
    uint64_t had = __atomic_load_n(&r->inode->blocks, __ATOMIC_RELAXED);
    __atomic_store_n(&r->inode->blocks, had > units ? had - units : 0, __ATOMIC_RELAXED);
    r->count = 0;
}

static void run_add(FreeRun *r, uint32_t b, int meta) {
    if (r->count != 0 && (b != r->first + r->count || meta != r->meta)) run_flush(r);
    if (r->count == 0) {
        r->first = b;
        r->meta = meta;
    }
    r->count++;
}

#define FREE_CHUNK 64   /* entries taken out of a last-level indirect block per page lock */

/* Free what indirect block blk (depth 1: entries are data blocks) maps from
 * its file block `from` on, clearing the entries. Returns 1 when from is 0
 * (everything it mapped is gone: the caller frees blk itself). */
static int free_tree(FreeRun *r, uint32_t blk, int depth, uint64_t from, int meta) {
    Ext2Vol *v = ext2_vol(r->inode);
    uint32_t sh = v->addr_shift;
    uint32_t apb = 1u << sh;
    if (blk >= v->blocks_count || blk <= v->first_data_block) {
        (void)ext2_corrupt(v, "indirect block", blk);
        return 0;
    }
    uint64_t child_span = 1ull << ((depth - 1) * sh);
    uint32_t idx0 = (uint32_t)(from / child_span);
    if (depth == 1) {
        for (uint32_t i = idx0; i < apb; i += FREE_CHUNK) {
            uint32_t got[FREE_CHUNK];
            uint32_t n = apb - i < FREE_CHUNK ? apb - i : FREE_CHUNK;
            struct page *pg;
            int err;
            uint32_t *e = (uint32_t *)ext2_meta_lock(v, ext2_block_at(v, blk) + (uint64_t)i * 4, &pg, &err);
            if (e == NULL) return 0;   /* unreadable: its blocks leak, the block stays */
            int any = 0;
            for (uint32_t k = 0; k < n; k++) {
                got[k] = e[k];
                any |= got[k] != 0;
                e[k] = 0;
            }
            ext2_meta_done(pg, any);
            for (uint32_t k = 0; k < n; k++) {
                if (got[k] == 0) continue;
                if (got[k] >= v->blocks_count)
                    (void)ext2_corrupt(v, "block number", got[k]);
                else
                    run_add(r, got[k], meta);
            }
        }
        return from == 0;
    }
    for (uint32_t i = idx0; i < apb; i++) {
        int64_t child = ind_entry(v, blk, i);
        if (child <= 0) continue;   /* a hole (or unreadable: left alone) */
        uint64_t sub = i == idx0 ? from - (uint64_t)idx0 * child_span : 0;
        if (free_tree(r, (uint32_t)child, depth - 1, sub, meta) && ind_set(v, blk, i, 0) == 0)
            run_add(r, (uint32_t)child, 1);
    }
    return from == 0;
}

void ext2_truncate_blocks(Inode *inode, uint64_t size) {
    Ext2Vol *v = ext2_vol(inode);
    Ext2Inode *ei = ext2_ei(inode);
    uint64_t first = (size + v->block_size - 1) >> v->block_shift;   /* the first block past the end */
    int meta = !S_ISREG(inode->mode);   /* a directory's or symlink's blocks are metadata */
    FreeRun r = {inode, 0, 0, 0};
    for (uint64_t i = first; i < EXT2_NDIR_BLOCKS; i++) {
        uint32_t b = __atomic_exchange_n(&ei->block[i], 0, __ATOMIC_ACQ_REL);
        if (b != 0) run_add(&r, b, meta);
    }
    uint64_t start = EXT2_NDIR_BLOCKS;   /* first file block of each tree */
    for (int depth = 1; depth <= 3; depth++) {
        uint64_t span = 1ull << (depth * v->addr_shift);
        uint32_t slot = EXT2_IND_BLOCK + (uint32_t)depth - 1;
        uint32_t root = __atomic_load_n(&ei->block[slot], __ATOMIC_ACQUIRE);
        if (root != 0 && first < start + span) {
            uint64_t from = first > start ? first - start : 0;
            if (free_tree(&r, root, depth, from, meta)) {
                __atomic_store_n(&ei->block[slot], 0, __ATOMIC_RELEASE);
                run_add(&r, root, 1);
            }
        }
        start += span;
    }
    run_flush(&r);
    __atomic_store_n(&ei->ind_cache, 0, __ATOMIC_RELEASE);   /* it may name a freed block */
    ei->next_lblk = ei->next_goal = 0;
    ext2_write_inode(inode);
}

/* --- Regular files --- */

/* The page holding size, shrunk to: its bytes past the end zeroed on the
 * disk too, so a later size increase reads zeros there */
static void zero_tail_on_disk(Inode *inode, uint64_t size) {
    if ((size & (PAGE_SIZE - 1)) == 0 || ext2_bmap(inode, (uint32_t)(size >> PAGE_SHIFT)) <= 0) return;
    AddressSpace *as = filemap_of(inode);
    unsigned off;
    int err;
    struct page *pg = filemap_get_page(as, size >> PAGE_SHIFT, NULL, 0, &off, &err);
    if (pg == NULL) return;   /* unreadable: the tail stays as it is on the disk */
    page_lock(pg);
    if (pg->mapping == as && page_test(pg, PG_UPTODATE)) {
        size_t in_page = (size_t)(size & (PAGE_SIZE - 1));
        memset((uint8_t *)page_to_virt(pg) + ((size_t)off << PAGE_SHIFT) + in_page, 0, PAGE_SIZE - in_page);
        filemap_set_dirty(pg);
    }
    page_unlock(pg);
    page_put(pg);
}

static int ext2_prepare_write(AddressSpace *as, struct page *pg, size_t from, size_t len) {
    Inode *inode = as->host;
    Ext2Vol *v = ext2_vol(inode);
    if (!v->writable) return -EROFS;
    if (((uint64_t)pg->index << PAGE_SHIFT) + from + len > v->max_bytes) return -EFBIG;
    int64_t p = ext2_bmap(inode, pg->index);   /* 4 KiB blocks: block = page */
    if (p != 0) return p < 0 ? (int)p : 0;
    int fresh;
    mutex_lock(&v->alloc_lock);
    p = ext2_map_alloc(inode, pg->index, 0, &fresh);
    mutex_unlock(&v->alloc_lock);
    if (p < 0) return (int)p;
    if (!page_test(pg, PG_UPTODATE)) {   /* it was a hole: zeros, then the copy */
        memset(page_to_virt(pg), 0, PAGE_SIZE);
        page_set(pg, PG_UPTODATE);
    }
    return 1;
}

static int64_t ext2_file_write(File *file, const IoBuf *buf, loff_t *pos) {
    Inode *inode = file->inode;
    Ext2Vol *v = ext2_vol(inode);
    /* SB_RDONLY too: a file opened before ext2_shutdown began gets -EROFS
     * while shutdown writes back (nothing new is dirtied behind it) */
    if (!v->writable || (__atomic_load_n(&v->sb.flags, __ATOMIC_RELAXED) & SB_RDONLY)) return -EROFS;
    int64_t n = filemap_write(file, buf, pos);
    /* A page past the end that got a block and then no bytes (the copy
     * faulted, or ENOSPC on a later page): the block goes back, so no block
     * lies past the end of the file */
    uint64_t end = ((uint64_t)inode->size + v->block_size - 1) >> v->block_shift;
    if (ext2_ei(inode)->next_lblk > end) {
        mutex_lock(&v->alloc_lock);
        ext2_truncate_blocks(inode, (uint64_t)inode->size);
        mutex_unlock(&v->alloc_lock);
    }
    return n;
}

int ext2_fsync(File *file) {
    Inode *inode = file->inode;
    Ext2Vol *v = ext2_vol(inode);
    AddressSpace *as = filemap_of(inode);
    if (!v->writable)   /* read-only, or shut down: only a page given up since (-EROFS) to tell */
        return as != NULL ? __atomic_exchange_n(&as->wb_err, 0, __ATOMIC_RELAXED) : 0;
    int e = as != NULL ? filemap_write_range(as, 0, UINT64_MAX) : 0;
    int f = filemap_flush_device(&v->meta);   /* the data on the medium before what points at it */
    int m = filemap_write_all(&v->meta);       /* the inode, the map, directories, then the bitmaps */
    int g = filemap_flush_device(&v->meta);
    int kept = as != NULL ? __atomic_exchange_n(&as->wb_err, 0, __ATOMIC_RELAXED) : 0;
    int kept_meta = __atomic_exchange_n(&v->meta.wb_err, 0, __ATOMIC_RELAXED);
    int errs[] = {e, f, m, g, kept, kept_meta};
    for (unsigned i = 0; i < sizeof(errs) / sizeof(errs[0]); i++) {
        if (errs[i] != 0) return errs[i];
    }
    return 0;
}

int ext2_setattr(Inode *inode, const InodeAttr *attr) {
    Ext2Vol *v = ext2_vol(inode);
    if (!v->writable) return -EROFS;
    if (attr->valid & ATTR_SIZE) {
        uint64_t size = (uint64_t)attr->size;
        if (size > v->max_bytes) return -EFBIG;
        uint64_t old = (uint64_t)inode->size;
        filemap_truncate(inode, attr->size);   /* pages past the end gone (their writes ended) */
        if (size < old) {
            mutex_lock(&v->alloc_lock);
            ext2_truncate_blocks(inode, size);
            mutex_unlock(&v->alloc_lock);
            zero_tail_on_disk(inode, size);
        }
        vfs_now(&inode->mtime);
        inode->mtime.tv_nsec = 0;
    }
    if (attr->valid & ATTR_MODE) inode->mode = (inode->mode & S_IFMT) | (attr->mode & 07777);
    if (attr->valid & ATTR_UID) inode->uid = attr->uid;
    if (attr->valid & ATTR_GID) inode->gid = attr->gid;
    return 0;   /* the VFS sets ctime and writes the inode (vfs_inode_dirty) */
}

static int ext2_readpage(AddressSpace *as, struct page *pg) {
    filemap_blocks_read(as, pg, NULL, &ext2_vol(as->host)->blocks);
    return 0;
}

static void ext2_readpages(AddressSpace *as, struct page **pages, uint32_t n) {
    BlkPlug plug;   /* the pages' pieces reach the disk together */
    blk_plug_init(&plug);
    for (uint32_t i = 0; i < n; i++) filemap_blocks_read(as, pages[i], &plug, &ext2_vol(as->host)->blocks);
    blk_plug_flush(&plug);
}

static int ext2_writepage(AddressSpace *as, struct page *pg) {
    Ext2Vol *v = ext2_vol(as->host);
    if (!v->writable) return -EROFS;
    return filemap_blocks_write(as, pg, &v->blocks);
}

const AddressSpaceOps ext2_aops = {
    .readpage = ext2_readpage,
    .readpages = ext2_readpages,
    .writepage = ext2_writepage,
    .prepare_write = ext2_prepare_write,
};

const AddressSpaceOps ext2_meta_aops = {
    .readpage = filemap_bdev_readpage,
    .writepage = filemap_bdev_writepage,
    .writepages = ext2_meta_writepages,
};

const FileOps ext2_file_fops = {
    .read = filemap_read,
    .write = ext2_file_write,
    .mmap_ops = &filemap_vma_ops,
    .fsync = ext2_fsync,
};

const InodeOps ext2_file_iops = {
    .setattr = ext2_setattr,
};
