/* arc_os — ext2 rev 1, read-only (ext2.h, M1-13)
 *
 * Owns: mounting an ext2 volume, its inodes (inode cache, by inode number),
 * the block map (direct, indirect, double and triple indirect blocks),
 * directories (lookup, getdents, ".."), symlinks (fast: the target in the
 * inode; slow: in a block) and filling file pages for the page cache.
 *
 * Two caches carry everything. Metadata (superblock, group descriptors,
 * inode tables, indirect blocks, directory blocks, slow symlink targets) is
 * read through the volume's block-device mapping (`meta`, mm/filemap.h): a
 * block never crosses a page (block size <= 4 KiB and a power of two), so
 * code reads a block in place under a page reference. File data goes
 * straight from the disk into the file's own cache pages
 * (filemap_bdev_read), one piece per run of contiguous blocks.
 *
 * The block map keeps, per inode, the last-level indirect block it used last
 * (`ind_cache`: which file blocks it covers and where it is): a sequential
 * read of a large file then costs one entry lookup per block instead of up
 * to three indirect reads.
 *
 * Invariants: the volume is read-only, so nothing on disk changes under the
 * caches (M1-15 must invalidate `ind_cache` when it changes a map). Bytes of
 * a file page past the end of the file are zero: the block holding the end is
 * copied from the metadata cache and its tail zeroed, never read straight
 * into the page, so an asynchronous read (M1-10) can't land after the
 * zeroing.
 *
 * Locks: the VFS holds the directory's lock around lookup and iterate, the
 * page cache the page lock around readpage; ext2 adds none. Metadata pages
 * are locked only by the page cache, at LOCK_LEVEL_META_PAGE (below a file
 * page's lock). `ind_cache` is one atomic word.
 *
 * Cost: an inode read is one metadata page lookup; a block-map lookup at
 * most 3 (one on an ind_cache hit); a directory lookup or getdents is linear
 * in the directory's blocks (htree indexes are not used); mount reads the
 * superblock and one word per group descriptor (inode_table: 4 bytes per
 * group kept). */

#include "fs/ext2.h"
#include "drivers/blkdev.h"
#include "mm/kmalloc.h"
#include "mm/filemap.h"
#include "lib/mem.h"
#include "lib/string.h"
#include "lib/kprintf.h"
#include "uapi/dirent.h"
#include "uapi/errno.h"

#define EXT2_MAGIC        0xEF53
#define EXT2_SUPER_OFFSET 1024
#define EXT2_ROOT_INO     2
#define EXT2_NDIR_BLOCKS  12
#define EXT2_N_BLOCKS     15
#define EXT2_FAST_LINK    60   /* bytes of i_block: a fast symlink's target fits */

#define EXT2_VALID_FS 1   /* s_state: unmounted cleanly */
#define EXT2_ERROR_FS 2   /* s_state: errors found */

/* Incompatible / read-only-compatible features this reader handles */
#define EXT2_INCOMPAT_OK  EXT2_FEATURE_INCOMPAT_FILETYPE
#define EXT2_RO_COMPAT_OK (EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER | EXT2_FEATURE_RO_COMPAT_LARGE_FILE)
/* Compatible features known to be safe to ignore (spec M1-13) */
#define EXT2_COMPAT_SAFE \
    (EXT2_FEATURE_COMPAT_EXT_ATTR | EXT2_FEATURE_COMPAT_RESIZE_INODE | EXT2_FEATURE_COMPAT_DIR_INDEX)

/* --- On-disk layout (little-endian, as the CPU) --- */

/* The superblock's first 136 bytes: all this reader uses */
typedef struct __attribute__((packed)) {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t s_uuid[16];
    char s_volume_name[16];
} Ext2DiskSuper;
_Static_assert(sizeof(Ext2DiskSuper) == 136, "ext2 superblock prefix");
_Static_assert(__builtin_offsetof(Ext2DiskSuper, s_magic) == 56, "s_magic");
_Static_assert(__builtin_offsetof(Ext2DiskSuper, s_feature_compat) == 92, "s_feature_compat");

typedef struct __attribute__((packed)) {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint8_t bg_reserved[12];
} Ext2DiskGroup;
_Static_assert(sizeof(Ext2DiskGroup) == 32, "ext2 group descriptor");

/* The first 128 bytes of an inode (a 256-byte inode's rest is ignored) */
typedef struct __attribute__((packed)) {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;   /* 512-byte units */
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[EXT2_N_BLOCKS];
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_size_high;   /* regular files: the top 32 bits of the size (old name i_dir_acl) */
    uint32_t i_faddr;
    uint8_t l_i_frag;
    uint8_t l_i_fsize;
    uint16_t l_i_blocks_hi;
    uint16_t l_i_uid_high;
    uint16_t l_i_gid_high;
    uint32_t l_i_reserved2;
} Ext2DiskInode;
_Static_assert(sizeof(Ext2DiskInode) == 128, "ext2 inode");
_Static_assert(__builtin_offsetof(Ext2DiskInode, i_block) == 40, "i_block");
_Static_assert(__builtin_offsetof(Ext2DiskInode, i_size_high) == 108, "i_size_high");
_Static_assert(__builtin_offsetof(Ext2DiskInode, l_i_uid_high) == 120, "l_i_uid_high");

typedef struct __attribute__((packed)) {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t name_len;
    uint8_t file_type;   /* with `filetype`; else the high byte of name_len (always 0 for names <= 255) */
    char name[];
} Ext2DirEntry;
#define EXT2_DIRENT_HEAD 8

/* --- In memory --- */

typedef struct Ext2Vol {
    SuperBlock sb;   /* first: an inode's sb is its volume */
    BlockDevice *dev;
    AddressSpace meta;        /* the device's page cache: all metadata */
    uint32_t block_size;
    uint32_t block_shift;     /* log2(block_size) */
    uint32_t addr_shift;      /* log2(block numbers per indirect block) */
    uint32_t blocks_count;
    uint32_t inodes_count;
    uint32_t inodes_per_group;
    uint32_t inode_size;
    uint32_t groups;
    uint32_t *inode_table;    /* per group: the first block of its inode table */
    int filetype;             /* directory entries carry a type byte */
    int ro_forced;            /* an unknown ro_compat feature: never writable */
    char name[16];            /* the device, for the log */
} Ext2Vol;

/* Inode.priv */
typedef struct Ext2Inode {
    uint32_t block[EXT2_N_BLOCKS];   /* i_block: the map, or a fast symlink's target */
    uint32_t fast_link;   /* a symlink whose target is block[]'s bytes */
    /* Last-level indirect block used last: (first file block it maps << 32)
     * | its disk block, or 0. One atomic word. */
    uint64_t ind_cache;
} Ext2Inode;

static const InodeOps ext2_dir_iops;
static const InodeOps ext2_link_iops;
static const FileOps ext2_dir_fops;
static const FileOps ext2_file_fops;
static const FileOps ext2_special_fops;
static const AddressSpaceOps ext2_aops;
static const SuperOps ext2_sops;

static Ext2Vol *ext2_vol(const Inode *inode) {
    return (Ext2Vol *)inode->sb;
}

static Ext2Inode *ei_of(const Inode *inode) {
    return (Ext2Inode *)inode->priv;
}

/* --- Metadata through the block-device cache --- */

/* The cached bytes at disk byte `at` (a range that stays inside one block):
 * a pointer into a metadata page, valid until page_put(*pg). NULL with *err
 * (-EIO: unreadable or past the device). */
static const uint8_t *meta_map(Ext2Vol *v, uint64_t at, struct page **pg, int *err) {
    unsigned off;
    *pg = filemap_get_page(&v->meta, at >> PAGE_SHIFT, NULL, 0, &off, err);
    if (*pg == NULL) {
        *err = -EIO;
        return NULL;
    }
    return (const uint8_t *)page_to_virt(*pg) + off + (at & (PAGE_SIZE - 1));
}

/* Copy n bytes at disk byte `at` (inside one block) into dst. */
static int meta_read(Ext2Vol *v, uint64_t at, void *dst, size_t n) {
    struct page *pg;
    int err;
    const uint8_t *p = meta_map(v, at, &pg, &err);
    if (p == NULL) return err;
    memcpy(dst, p, n);
    page_put(pg);
    return 0;
}

static uint64_t block_at(const Ext2Vol *v, uint32_t block) {
    return (uint64_t)block << v->block_shift;
}

static int corrupt(const Ext2Vol *v, const char *what, uint64_t n) {
    kprintf("[EXT2] %s: bad %s (%lu)\n", v->name, what, n);
    return -EIO;
}

/* --- Block map --- */

/* Entry idx of indirect block `block`: 0 (a hole) or a block number. */
static int64_t ind_entry(Ext2Vol *v, uint32_t block, uint32_t idx) {
    if (block >= v->blocks_count) return corrupt(v, "indirect block", block);
    uint32_t e;
    int err = meta_read(v, block_at(v, block) + (uint64_t)idx * 4, &e, sizeof(e));
    if (err != 0) return err;
    if (e >= v->blocks_count) return corrupt(v, "block number", e);
    return e;
}

/* The disk block holding file block lblk of inode: > 0, 0 for a hole, or
 * -errno. */
static int64_t ext2_bmap(Inode *inode, uint32_t lblk) {
    Ext2Vol *v = ext2_vol(inode);
    Ext2Inode *ei = ei_of(inode);
    if (lblk < EXT2_NDIR_BLOCKS) {
        uint32_t b = ei->block[lblk];
        return b < v->blocks_count ? (int64_t)b : corrupt(v, "block number", b);
    }
    uint32_t sh = v->addr_shift;
    uint64_t apb = 1ull << sh;
    /* A last-level indirect block used just before (sequential reads) */
    uint64_t c = __atomic_load_n(&ei->ind_cache, __ATOMIC_RELAXED);
    if (c != 0 && lblk >= (uint32_t)(c >> 32) && lblk - (uint32_t)(c >> 32) < apb)
        return ind_entry(v, (uint32_t)c, lblk - (uint32_t)(c >> 32));

    uint64_t rel = lblk - EXT2_NDIR_BLOCKS;
    int depth;
    uint32_t blk;
    if (rel < apb) {
        depth = 1;
        blk = ei->block[12];
    } else if ((rel -= apb) < apb << sh) {
        depth = 2;
        blk = ei->block[13];
    } else if ((rel -= apb << sh) < apb << (2 * sh)) {
        depth = 3;
        blk = ei->block[14];
    } else {
        return corrupt(v, "file block", lblk);   /* beyond triple indirect */
    }
    for (int d = depth; d > 0; d--) {
        if (blk == 0) return 0;   /* a hole covers the rest */
        uint32_t idx = (uint32_t)(rel >> ((d - 1) * sh)) & (uint32_t)(apb - 1);
        if (d == 1) __atomic_store_n(&ei->ind_cache, ((uint64_t)(lblk - idx) << 32) | blk, __ATOMIC_RELAXED);
        int64_t next = ind_entry(v, blk, idx);
        if (next < 0) return next;
        blk = (uint32_t)next;
    }
    return blk;
}

/* --- Inodes --- */

static int read_disk_inode(Ext2Vol *v, uint32_t ino, Ext2DiskInode *out) {
    if (ino < 1 || ino > v->inodes_count) return corrupt(v, "inode number", ino);
    uint32_t g = (ino - 1) / v->inodes_per_group;
    uint32_t i = (ino - 1) % v->inodes_per_group;
    return meta_read(v, block_at(v, v->inode_table[g]) + (uint64_t)i * v->inode_size, out, sizeof(*out));
}

/* A fast symlink keeps its target in i_block (no data block; an extended
 * attribute block doesn't count), as Linux decides it */
static int is_fast_symlink(const Ext2Vol *v, const Ext2DiskInode *d) {
    uint32_t ea_blocks = d->i_file_acl != 0 ? v->block_size >> 9 : 0;
    return d->i_blocks - ea_blocks == 0;
}

/* Fill a new inode from its disk copy: type, owner, size, times, ops. */
static int inode_fill(Ext2Vol *v, Inode *inode, const Ext2DiskInode *d) {
    Ext2Inode *ei = ei_of(inode);
    memcpy(ei->block, d->i_block, sizeof(ei->block));
    inode->uid = d->i_uid | ((uint32_t)d->l_i_uid_high << 16);
    inode->gid = d->i_gid | ((uint32_t)d->l_i_gid_high << 16);
    inode->nlink = d->i_links_count;
    inode->size = (loff_t)d->i_size;
    inode->blocks = d->i_blocks;
    inode->atime.tv_sec = (int32_t)d->i_atime;   /* signed 32-bit seconds, as Linux reads ext2 */
    inode->mtime.tv_sec = (int32_t)d->i_mtime;
    inode->ctime.tv_sec = (int32_t)d->i_ctime;
    inode->atime.tv_nsec = inode->mtime.tv_nsec = inode->ctime.tv_nsec = 0;
    switch (inode->mode & S_IFMT) {
    case S_IFREG:
        inode->size |= (loff_t)((uint64_t)d->i_size_high << 32);
        if (inode->size < 0) return corrupt(v, "file size", d->i_size_high);
        inode->fop = &ext2_file_fops;
        return filemap_inode_init(inode, &ext2_aops, 0);
    case S_IFDIR:
        inode->iop = &ext2_dir_iops;
        inode->fop = &ext2_dir_fops;
        return 0;
    case S_IFLNK:
        ei->fast_link = is_fast_symlink(v, d);
        if (inode->size >= (loff_t)v->block_size || (ei->fast_link && inode->size >= EXT2_FAST_LINK))
            return corrupt(v, "symlink size", (uint64_t)inode->size);
        inode->iop = &ext2_link_iops;
        return 0;
    case S_IFCHR:
    case S_IFBLK:
        /* Linux's old (major << 8 | minor) or new encoding: both are st_rdev's */
        inode->rdev = d->i_block[0] != 0 ? d->i_block[0] : d->i_block[1];
        inode->fop = &ext2_special_fops;
        return 0;
    case S_IFIFO:
    case S_IFSOCK:
        inode->fop = &ext2_special_fops;
        return 0;
    default:
        return corrupt(v, "inode mode", inode->mode);
    }
}

/* The inode numbered ino (from the inode cache, or read now): 0 with *out (a
 * reference), or -errno. */
static int ext2_iget(Ext2Vol *v, uint32_t ino, Inode **out) {
    Inode *inode = icache_get(&v->sb, ino);
    if (inode != NULL) {
        *out = inode;
        return 0;
    }
    Ext2DiskInode d;
    int err = read_disk_inode(v, ino, &d);
    if (err != 0) return err;
    if (d.i_links_count == 0) return corrupt(v, "link to a free inode", ino);
    Ext2Inode *ei = kmalloc(sizeof(*ei), GFP_ZERO);
    inode = ei != NULL ? inode_new(&v->sb, ino, d.i_mode) : NULL;
    if (inode == NULL) {
        kfree(ei);
        return -ENOMEM;
    }
    inode->priv = ei;
    err = inode_fill(v, inode, &d);
    if (err != 0) {
        iput(inode);
        return err;
    }
    *out = icache_add(inode);   /* a racing lookup's copy wins; ours is freed */
    return 0;
}

static void ext2_evict(Inode *inode) {
    filemap_inode_evict(inode);   /* a regular file's pages (nothing for others) */
    kfree(inode->priv);
}

/* --- Directories --- */

/* Called for each used entry from a position on: its position and the next
 * entry's. Returns 0 to go on, nonzero to stop (dir_walk's result). */
typedef int (*DirVisit)(const Ext2DirEntry *e, uint64_t pos, uint64_t next, void *ctx);

/* Walk dir's entries from byte position pos (an entry's start, or anything
 * inside a block: entries before it are skipped). The visitor's stop value,
 * 0 at the end, or -errno. A malformed entry is -EIO (and logged). */
static int dir_walk(Inode *dir, uint64_t pos, DirVisit visit, void *ctx) {
    Ext2Vol *v = ext2_vol(dir);
    uint32_t bs = v->block_size;
    uint64_t size = (uint64_t)dir->size;
    while (pos < size) {
        uint64_t lblk = pos >> v->block_shift;
        uint64_t base = lblk << v->block_shift;
        if (lblk > UINT32_MAX) return corrupt(v, "directory size", size);
        int64_t pblk = ext2_bmap(dir, (uint32_t)lblk);
        if (pblk < 0) return (int)pblk;
        if (pblk == 0) {   /* a hole in a directory: nothing there */
            pos = base + bs;
            continue;
        }
        struct page *pg;
        int err;
        const uint8_t *b = meta_map(v, block_at(v, (uint32_t)pblk), &pg, &err);
        if (b == NULL) return err;
        int rc = 0;
        for (uint32_t off = 0; off < bs && rc == 0;) {
            const Ext2DirEntry *e = (const Ext2DirEntry *)(b + off);
            uint32_t len = off + EXT2_DIRENT_HEAD <= bs ? e->rec_len : 0;
            if (len < EXT2_DIRENT_HEAD || (len & 3) != 0 || len > bs - off ||
                (uint32_t)e->name_len + EXT2_DIRENT_HEAD > len || e->inode > v->inodes_count) {
                rc = corrupt(v, "directory entry in inode", dir->ino);
                break;
            }
            if (base + off >= pos && e->inode != 0 && e->name_len != 0)
                rc = visit(e, base + off, base + off + len, ctx);
            off += len;
        }
        page_put(pg);
        if (rc != 0) return rc;
        pos = base + bs;
    }
    return 0;
}

typedef struct {
    const char *name;
    size_t len;
    uint32_t ino;
} LookupCtx;

static int lookup_visit(const Ext2DirEntry *e, uint64_t pos, uint64_t next, void *arg) {
    (void)pos, (void)next;
    LookupCtx *c = arg;
    if (e->name_len != c->len || memcmp(e->name, c->name, c->len) != 0) return 0;
    c->ino = e->inode;
    return 1;
}

/* Also ".." (the on-disk entry): the walk's way up */
static int ext2_lookup(Inode *dir, const char *name, Inode **out) {
    LookupCtx c = {name, strlen(name), 0};
    if (c.len > VFS_NAME_MAX) return -ENOENT;
    int rc = dir_walk(dir, 0, lookup_visit, &c);
    if (rc < 0) return rc;
    if (rc == 0) return -ENOENT;
    return ext2_iget(ext2_vol(dir), c.ino, out);
}

/* EXT2_FT_* -> DT_* */
static uint8_t ft_to_dtype(uint8_t ft) {
    static const uint8_t map[8] = {DT_UNKNOWN, DT_REG, DT_DIR, DT_CHR, DT_BLK, DT_FIFO, DT_SOCK, DT_LNK};
    return ft < 8 ? map[ft] : DT_UNKNOWN;
}

typedef struct {
    DirContext *dctx;
    int filetype;
} IterCtx;

static int iter_visit(const Ext2DirEntry *e, uint64_t pos, uint64_t next, void *arg) {
    IterCtx *c = arg;
    c->dctx->pos = (loff_t)pos;
    uint8_t dtype = c->filetype ? ft_to_dtype(e->file_type) : DT_UNKNOWN;
    return dir_emit(c->dctx, e->name, e->name_len, e->inode, dtype, (loff_t)next) ? 0 : 1;
}

/* getdents: the cookie is the byte position of the next entry ("." and ".."
 * are the directory's own first two entries) */
static int ext2_iterate(File *file, DirContext *ctx) {
    Inode *dir = file->inode;
    if (ctx->pos < 0 || ctx->pos >= dir->size) return 0;
    IterCtx c = {ctx, ext2_vol(dir)->filetype};
    int rc = dir_walk(dir, (uint64_t)ctx->pos, iter_visit, &c);
    if (rc == 0) ctx->pos = dir->size;
    return rc < 0 ? rc : 0;
}

/* --- Symlinks --- */

static int ext2_readlink(Inode *inode, char *buf, size_t size) {
    Ext2Vol *v = ext2_vol(inode);
    size_t len = (size_t)inode->size;
    size_t n = len < size ? len : size;
    if (ei_of(inode)->fast_link) {
        memcpy(buf, ei_of(inode)->block, n);   /* fast: in the inode */
        return (int)len;
    }
    int64_t b = ext2_bmap(inode, 0);
    if (b < 0) return (int)b;
    if (b == 0) return corrupt(v, "symlink block in inode", inode->ino);
    int err = meta_read(v, block_at(v, (uint32_t)b), buf, n);
    return err != 0 ? err : (int)len;
}

/* --- File pages --- */

/* Start filling one page of a regular file: holes and blocks past the end
 * zeroed, runs of contiguous blocks read straight into the page (one piece
 * each), the block holding the end copied from the metadata cache with its
 * tail zeroed (see the top). Pieces go on plug (NULL: submitted at once).
 * The read ends when its last piece does. */
static void ext2_start_page(Inode *inode, struct page *pg, BlkPlug *plug) {
    Ext2Vol *v = ext2_vol(inode);
    uint32_t sh = v->block_shift, bs = v->block_size;
    uint32_t bpp = PAGE_SIZE >> sh;
    uint64_t base = (uint64_t)pg->index << PAGE_SHIFT;
    uint64_t size = (uint64_t)inode->size;
    uint8_t *mem = page_to_virt(pg);
    int err = 0;
    filemap_io_begin(pg);
    for (uint32_t i = 0; i < bpp && err == 0;) {
        uint64_t at = base + ((uint64_t)i << sh);
        if (at >= size) {
            memset(mem + ((size_t)i << sh), 0, (size_t)(bpp - i) << sh);
            break;
        }
        if ((at >> sh) > UINT32_MAX) {   /* a size no block map reaches (1 KiB blocks) */
            err = corrupt(v, "file size", size);
            break;
        }
        uint32_t lblk = (uint32_t)(at >> sh);
        int64_t p = ext2_bmap(inode, lblk);
        if (p <= 0) {   /* a hole (or an error: the read fails) */
            if (p == 0) memset(mem + ((size_t)i << sh), 0, bs);
            err = (int)p;
            i++;
            continue;
        }
        if (at + bs > size) {   /* the end of the file: copy, then zero the tail */
            size_t valid = (size_t)(size - at);
            err = meta_read(v, block_at(v, (uint32_t)p), mem + ((size_t)i << sh), valid);
            memset(mem + ((size_t)i << sh) + valid, 0, bs - valid);
            i++;
            continue;
        }
        uint32_t n = 1;   /* whole blocks contiguous on disk: one piece */
        while (i + n < bpp && base + ((uint64_t)(i + n + 1) << sh) <= size && ext2_bmap(inode, lblk + n) == p + n) n++;
        err = filemap_bdev_read(plug, v->dev, (uint64_t)p << (sh - 9), n << (sh - 9), pg, (size_t)i << sh);
        i += n;
    }
    filemap_io_end(pg, err);
}

static int ext2_readpage(AddressSpace *as, struct page *pg) {
    ext2_start_page(as->host, pg, NULL);
    return 0;
}

static void ext2_readpages(AddressSpace *as, struct page **pages, uint32_t n) {
    BlkPlug plug;   /* the pages' pieces reach the disk together */
    blk_plug_init(&plug);
    for (uint32_t i = 0; i < n; i++) ext2_start_page(as->host, pages[i], &plug);
    blk_plug_flush(&plug);
}

/* Devices, FIFOs and sockets on an arc_os volume have no driver behind them
 * (no device-number registry yet): opening one is -ENXIO */
static int ext2_special_open(Inode *inode, File *file) {
    (void)inode, (void)file;
    return -ENXIO;
}

static const AddressSpaceOps ext2_aops = {
    .readpage = ext2_readpage,
    .readpages = ext2_readpages,
};

static const InodeOps ext2_dir_iops = {
    .lookup = ext2_lookup,
};

static const InodeOps ext2_link_iops = {
    .readlink = ext2_readlink,
};

static const FileOps ext2_dir_fops = {
    .iterate = ext2_iterate,
};

static const FileOps ext2_file_fops = {
    .read = filemap_read,
    .mmap_ops = &filemap_vma_ops,
};

static const FileOps ext2_special_fops = {
    .open = ext2_special_open,
};

static const SuperOps ext2_sops = {
    .evict_inode = ext2_evict,
};

/* --- Mount --- */

static void log_features(const char *name, const char *kind, uint32_t bits) {
    for (uint32_t b = 0; b < 32; b++) {
        if (bits & (1u << b)) kprintf("[EXT2] %s: %s feature 0x%x\n", name, kind, 1u << b);
    }
}

/* Check the superblock and fill v's geometry. 0 or -EINVAL. */
static int check_super(Ext2Vol *v, const Ext2DiskSuper *s, uint64_t bytes, const char *name) {
    if (s->s_magic != EXT2_MAGIC) {
        kprintf("[EXT2] %s: no ext2 superblock\n", name);
        return -EINVAL;
    }
    if (s->s_rev_level != 1) {
        kprintf("[EXT2] %s: revision %u not supported (rev 1 only)\n", name, s->s_rev_level);
        return -EINVAL;
    }
    if (s->s_log_block_size > 2) {
        kprintf("[EXT2] %s: block size %u not supported\n", name, 1024u << (s->s_log_block_size & 15));
        return -EINVAL;
    }
    if (s->s_inode_size != 128 && s->s_inode_size != 256) {
        kprintf("[EXT2] %s: inode size %u not supported\n", name, s->s_inode_size);
        return -EINVAL;
    }
    uint32_t incompat = s->s_feature_incompat & ~(uint32_t)EXT2_INCOMPAT_OK;
    if (incompat != 0) {
        log_features(name, "incompatible", incompat);
        kprintf("[EXT2] %s: refused (incompatible features)\n", name);
        return -EINVAL;
    }
    uint32_t ro = s->s_feature_ro_compat & ~(uint32_t)EXT2_RO_COMPAT_OK;
    if (ro != 0) {
        log_features(name, "read-only-compatible", ro);
        kprintf("[EXT2] %s: read-only for good (unknown ro_compat features)\n", name);
        v->ro_forced = 1;
    }
    uint32_t compat = s->s_feature_compat & ~(uint32_t)EXT2_COMPAT_SAFE;
    if (compat != 0) log_features(name, "ignored compatible", compat);

    v->block_shift = 10 + s->s_log_block_size;
    v->block_size = 1u << v->block_shift;
    v->addr_shift = v->block_shift - 2;
    v->blocks_count = s->s_blocks_count;
    v->inodes_count = s->s_inodes_count;
    v->inodes_per_group = s->s_inodes_per_group;
    v->inode_size = s->s_inode_size;
    v->filetype = (s->s_feature_incompat & EXT2_FEATURE_INCOMPAT_FILETYPE) != 0;
    uint32_t first = s->s_first_data_block;
    if (s->s_blocks_per_group == 0 || v->inodes_per_group == 0 || v->blocks_count <= first + 1 ||
        first != (v->block_size == 1024 ? 1u : 0u) || (uint64_t)v->blocks_count << v->block_shift > bytes ||
        (uint64_t)v->inodes_per_group * v->inode_size % v->block_size != 0) {
        kprintf("[EXT2] %s: bad geometry\n", name);
        return -EINVAL;
    }
    v->groups = (v->blocks_count - first + s->s_blocks_per_group - 1) / s->s_blocks_per_group;
    if (v->inodes_count < EXT2_ROOT_INO || (uint64_t)v->inodes_count > (uint64_t)v->groups * v->inodes_per_group) {
        kprintf("[EXT2] %s: bad inode count\n", name);
        return -EINVAL;
    }
    if (!(s->s_state & EXT2_VALID_FS) || (s->s_state & EXT2_ERROR_FS))
        kprintf("[EXT2] %s: not cleanly unmounted or has errors (read-only, carrying on)\n", name);
    return 0;
}

/* Read where each group's inode table is (group descriptors follow the
 * superblock's block) and check it lies inside the volume. */
static int read_groups(Ext2Vol *v, uint32_t first_data_block, const char *name) {
    v->inode_table = kmalloc((size_t)v->groups * sizeof(uint32_t), 0);
    if (v->inode_table == NULL) return -ENOMEM;
    uint32_t table_blocks = (uint32_t)((uint64_t)v->inodes_per_group * v->inode_size >> v->block_shift);
    uint64_t gdt = block_at(v, first_data_block + 1);
    for (uint32_t g = 0; g < v->groups; g++) {
        Ext2DiskGroup gd;
        int err = meta_read(v, gdt + (uint64_t)g * sizeof(gd), &gd, sizeof(gd));
        if (err != 0) return err;
        if (gd.bg_inode_table <= first_data_block || (uint64_t)gd.bg_inode_table + table_blocks > v->blocks_count) {
            kprintf("[EXT2] %s: group %u: inode table outside the volume\n", name, g);
            return -EINVAL;
        }
        v->inode_table[g] = gd.bg_inode_table;
    }
    return 0;
}

static void mount_fail(Ext2Vol *v) {
    filemap_bdev_release(&v->meta);
    kfree(v->inode_table);
    kfree(v);
}

SuperBlock *ext2_mount(BlockDevice *dev, uint64_t bytes, const char *name, int *err) {
    *err = -EINVAL;
    if (dev == NULL || bytes < EXT2_SUPER_OFFSET * 2) return NULL;
    Ext2Vol *v = kmalloc(sizeof(*v), GFP_ZERO);
    if (v == NULL) {
        *err = -ENOMEM;
        return NULL;
    }
    v->dev = dev;
    strncpy(v->name, name != NULL ? name : "?", sizeof(v->name) - 1);
    filemap_bdev_init(&v->meta, dev, (loff_t)bytes);
    vfs_sb_init(&v->sb, "ext2", &ext2_sops);
    v->sb.flags = SB_RDONLY;
    v->sb.bdev = dev;
    Ext2DiskSuper s;
    *err = meta_read(v, EXT2_SUPER_OFFSET, &s, sizeof(s));
    if (*err == 0) *err = check_super(v, &s, bytes, name);
    if (*err == 0) *err = read_groups(v, s.s_first_data_block, name);
    Inode *root = NULL;
    if (*err == 0) *err = ext2_iget(v, EXT2_ROOT_INO, &root);
    if (*err == 0 && !S_ISDIR(root->mode)) {
        kprintf("[EXT2] %s: root is not a directory\n", name);
        *err = -EINVAL;
    }
    if (*err != 0) {
        if (root != NULL) iput(root);   /* frees it: the volume had no other user */
        mount_fail(v);
        return NULL;
    }
    v->sb.root = root;   /* the superblock's reference: volumes are never unmounted */
    kprintf("[EXT2] %s: %u blocks of %u bytes, %u groups, %u inodes of %u bytes, read-only\n", name, v->blocks_count,
            v->block_size, v->groups, v->inodes_count, v->inode_size);
    return &v->sb;
}

int ext2_ro_forced(const SuperBlock *sb) {
    return ((const Ext2Vol *)sb)->ro_forced;
}
