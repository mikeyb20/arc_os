/* arc_os — page I/O for files kept as a list of blocks (filemap.h, M1-15)
 *
 * Owns: the one walk from a file page to its disk blocks, for filesystems
 * whose files are lists of fixed-size blocks (ext2; FAT32 moves onto it in
 * the next block that edits fat32.c): file block to disk block through the
 * filesystem's get_block, each run of blocks contiguous on the disk as one
 * piece, holes and bytes past the end of the file as zeros. Linux's
 * mpage_readpage and block_write_full_page.
 *
 * Blocks of 512 bytes to 64 KiB: smaller than a page (several per page) or
 * larger (a page is part of one block).
 *
 * The end of the file: the block holding it is read synchronously into the
 * page, then the bytes past the end are zeroed, so an asynchronous piece
 * can't land after the zeroing (bytes past the end of a file always read as
 * zeros, even when the disk holds something else there). Writes send whole
 * blocks, after zeroing the page's bytes past the end: a store through a
 * shared mapping may have put something there (POSIX lets it), and it must
 * not reach the disk, where growing the file later would show it (Linux's
 * block_write_full_page does the same).
 *
 * Locks: none of its own. Called inside readpage / writepage (the page
 * locked); get_block takes what the filesystem needs.
 *
 * Cost: one get_block per block (per page for blocks of a page or more); one
 * block request per run; one synchronous read per file, for its last page. */

#include "mm/filemap.h"
#include "drivers/blkdev.h"
#include "lib/mem.h"
#include "uapi/errno.h"

/* Where file byte `at` (inside a block) is, in 512-byte sectors */
static uint64_t sector_of(const FilemapBlocks *fb, int64_t block, uint64_t at) {
    uint64_t in_block = at & ((1ull << fb->blkbits) - 1);
    return ((uint64_t)block << (fb->blkbits - 9)) + (in_block >> 9);
}

/* Bytes of the block holding file byte `at` that lie in the page from off */
static uint32_t span_of(const FilemapBlocks *fb, uint64_t at, uint32_t off) {
    uint64_t left = (1ull << fb->blkbits) - (at & ((1ull << fb->blkbits) - 1));
    return left < PAGE_SIZE - off ? (uint32_t)left : PAGE_SIZE - off;
}

void filemap_blocks_read(AddressSpace *as, struct page *pg, struct BlkPlug *plug, const FilemapBlocks *fb) {
    Inode *inode = as->host;
    uint32_t sh = fb->blkbits;
    uint64_t base = (uint64_t)pg->index << PAGE_SHIFT;
    uint64_t size = (uint64_t)__atomic_load_n(&inode->size, __ATOMIC_ACQUIRE);
    uint8_t *mem = page_to_virt(pg);
    int err = 0;
    filemap_io_begin(pg);
    for (uint32_t off = 0; off < PAGE_SIZE && err == 0;) {
        uint64_t at = base + off;
        if (at >= size) {
            memset(mem + off, 0, PAGE_SIZE - off);
            break;
        }
        uint64_t lblk = at >> sh;
        uint32_t n = span_of(fb, at, off);
        int64_t p = fb->get_block(inode, lblk, 0);
        if (p <= 0) {   /* a hole (or an error: the read fails) */
            if (p == 0) memset(mem + off, 0, n);
            err = (int)p;
            off += n;
            continue;
        }
        if (at + n > size) {   /* the end of the file: read, then zero the rest (see the top) */
            uint32_t valid = (uint32_t)(size - at);
            uint32_t bytes = (valid + BLK_SECTOR_SIZE - 1) & ~(BLK_SECTOR_SIZE - 1);
            err = blk_rw_sync(fb->dev, BLK_OP_READ, sector_of(fb, p, at), mem + off, bytes) != 0 ? -EIO : 0;
            memset(mem + off + valid, 0, n - valid);
            off += n;
            continue;
        }
        uint32_t len = n;   /* whole blocks contiguous on the disk: one piece */
        for (uint64_t k = 1; off + len < PAGE_SIZE; k++) {
            uint32_t m = span_of(fb, at + len, off + len);
            if (at + len + m > size || fb->get_block(inode, lblk + k, 0) != p + (int64_t)k) break;
            len += m;
        }
        err = filemap_bdev_read(plug, fb->dev, sector_of(fb, p, at), len / BLK_SECTOR_SIZE, pg, off);
        off += len;
    }
    filemap_io_end(pg, err);
}

int filemap_blocks_write(AddressSpace *as, struct page *pg, const FilemapBlocks *fb) {
    Inode *inode = as->host;
    uint32_t sh = fb->blkbits;
    uint64_t base = (uint64_t)pg->index << PAGE_SHIFT;
    uint64_t size = (uint64_t)__atomic_load_n(&inode->size, __ATOMIC_ACQUIRE);
    if (size > base && size - base < PAGE_SIZE) {   /* the page holding the end (see the top) */
        uint32_t valid = (uint32_t)(size - base);
        memset((uint8_t *)page_to_virt(pg) + valid, 0, PAGE_SIZE - valid);
    }
    for (uint32_t off = 0; off < PAGE_SIZE;) {
        uint64_t at = base + off;
        if (at >= size) break;   /* past the end: nothing to write */
        uint64_t lblk = at >> sh;
        int64_t p = fb->get_block(inode, lblk, 1);   /* a hole dirtied through a mapping gets its block now */
        if (p <= 0) return p < 0 ? (int)p : -EIO;
        uint32_t len = span_of(fb, at, off);
        for (uint64_t k = 1; off + len < PAGE_SIZE && at + len < size; k++) {
            int64_t q = fb->get_block(inode, lblk + k, 1);
            if (q < 0) return (int)q;   /* the pieces already started still end the write */
            if (q != p + (int64_t)k) break;
            len += span_of(fb, at + len, off + len);
        }
        int err = filemap_write_piece(fb->dev, sector_of(fb, p, at), len / BLK_SECTOR_SIZE, pg, off);
        if (err != 0) return err;
        off += len;
    }
    return 0;
}
