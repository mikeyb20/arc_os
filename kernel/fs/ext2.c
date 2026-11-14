/* arc_os — ext2 rev 1: volumes and inodes (ext2.h; read M1-13, write M1-15)
 *
 * Owns: mounting an ext2 volume (read-only, or read-write: M1-15) and
 * shutting it down, the metadata helpers every ext2 file uses, the inodes
 * (inode cache by inode number, reading and writing inode-table slots, new
 * inodes, freeing them at the last reference) and symlink targets. The other
 * parts: ext2_alloc.c (bitmaps, counts, the metadata write order),
 * ext2_map.c (block map, file pages, truncate, fsync), ext2_dir.c
 * (directories, names, symlink creation). ext2_internal.h holds what they
 * share.
 *
 * Two caches carry everything. Metadata (superblock, group descriptors,
 * bitmaps, inode tables, indirect blocks, directory blocks, slow symlink
 * targets) lives in the volume's block-device mapping (`meta`,
 * mm/filemap.h): a block never crosses a page (block size <= 4 KiB and a
 * power of two), so code works on a block in place under a page reference
 * (reading) or the page lock (changing). File data goes between the disk and
 * the files' own cache pages (mm/filemap_block.c). The metadata cache never
 * holds file data, and a metadata block that is freed has its cached page
 * dropped (ext2_free_blocks), so no block is ever cached twice with
 * different bytes.
 *
 * Writing (M1-15) only on volumes with 4 KiB blocks (what arc_os makes,
 * ADR-0008): a block is then exactly one page, so a block freed from one
 * role and given another never shares a cache page with its old role. 1 and
 * 2 KiB volumes stay read-only (logged). A volume with an unknown
 * read-only-compatible feature is never writable (ro_forced), nor one whose
 * device is read-only.
 *
 * Superblock state (spec 5): mounting read-write clears s_state's VALID bit
 * on the disk at once (written and flushed before the mount returns), so a
 * crash leaves a volume marked "not cleanly unmounted"; ext2_shutdown writes
 * everything back and puts the state found at mount back (a volume found
 * dirty stays dirty until e2fsck checks it, as Linux does). A volume found
 * dirty is still mounted, with a warning on the screen (until M5-01 runs
 * e2fsck at boot).
 *
 * Locks: the VFS holds the inode and directory locks; the page cache the
 * page lock around readpage and writepage. Ext2Vol.alloc_lock
 * (LOCK_LEVEL_FS_ALLOC, under a file page's lock) serializes the bitmaps,
 * the counts and every block-map change. A metadata page is locked
 * (LOCK_LEVEL_META_PAGE) only to change it, at most one at a time, never
 * while taking alloc_lock and never while locking a file page; a locked
 * metadata page also waits for its write in flight, so metadata holds still
 * while it is written. Readers take neither lock: block numbers are 32-bit
 * stores, and a new indirect block is zeroed in the cache before its
 * number is stored.
 *
 * Cost: an inode read is one metadata page lookup; a block-map lookup at
 * most 3 (one on an ind_cache hit); mount reads the superblock and the group
 * descriptors (24 bytes per group kept); a read-write mount writes the
 * superblock once. */

#include "fs/ext2_internal.h"
#include "drivers/blkdev.h"
#include "mm/kmalloc.h"
#include "mm/writeback.h"
#include "lib/mem.h"
#include "lib/string.h"
#include "lib/kprintf.h"
#include "uapi/errno.h"

/* Incompatible / read-only-compatible features this driver handles */
#define EXT2_INCOMPAT_OK  EXT2_FEATURE_INCOMPAT_FILETYPE
#define EXT2_RO_COMPAT_OK (EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER | EXT2_FEATURE_RO_COMPAT_LARGE_FILE)
/* Compatible features known to be safe to ignore (spec M1-13): extended
 * attribute blocks are left alone, the resize inode's blocks stay reserved,
 * a changed htree directory loses its index flag */
#define EXT2_COMPAT_SAFE \
    (EXT2_FEATURE_COMPAT_EXT_ATTR | EXT2_FEATURE_COMPAT_RESIZE_INODE | EXT2_FEATURE_COMPAT_DIR_INDEX)

static const InodeOps ext2_link_iops;
static const FileOps ext2_special_fops;
static const SuperOps ext2_sops;

int ext2_corrupt(const Ext2Vol *v, const char *what, uint64_t n) {
    kprintf("[EXT2] %s: bad %s (%lu)\n", v->name, what, n);
    return -EIO;
}

/* --- Metadata through the block-device cache --- */

const uint8_t *ext2_meta_map(Ext2Vol *v, uint64_t at, struct page **pg, int *err) {
    unsigned off;
    *pg = filemap_get_page(&v->meta, at >> PAGE_SHIFT, NULL, 0, &off, err);
    if (*pg == NULL) {
        *err = -EIO;
        return NULL;
    }
    return (const uint8_t *)page_to_virt(*pg) + ((size_t)off << PAGE_SHIFT) + (at & (PAGE_SIZE - 1));
}

int ext2_meta_read(Ext2Vol *v, uint64_t at, void *dst, size_t n) {
    struct page *pg;
    int err;
    const uint8_t *p = ext2_meta_map(v, at, &pg, &err);
    if (p == NULL) return err;
    memcpy(dst, p, n);
    page_put(pg);
    return 0;
}

uint8_t *ext2_meta_lock(Ext2Vol *v, uint64_t at, struct page **pg, int *err) {
    for (;;) {
        const uint8_t *p = ext2_meta_map(v, at, pg, err);
        if (p == NULL) return NULL;
        page_lock(*pg);
        if ((*pg)->mapping == &v->meta) {
            filemap_wait_writeback(*pg);   /* metadata holds still while it is written */
            return (uint8_t *)p;
        }
        page_unlock(*pg);   /* dropped meanwhile: look it up again */
        page_put(*pg);
    }
}

void ext2_meta_done(struct page *pg, int changed) {
    if (changed) filemap_set_dirty(pg);
    page_unlock(pg);
    page_put(pg);
}

uint8_t *ext2_meta_new(Ext2Vol *v, uint32_t block, struct page **pg, int *err) {
    *pg = filemap_grab_zeroed(&v->meta, ext2_block_at(v, block) >> PAGE_SHIFT, err);
    return *pg != NULL ? page_to_virt(*pg) : NULL;
}

int ext2_meta_new_done(Ext2Vol *v, struct page *pg, int write_now) {
    filemap_set_dirty(pg);
    int err = write_now ? filemap_writepage_locked(&v->meta, pg) : 0;
    page_unlock(pg);
    if (write_now) {
        filemap_wait_writeback(pg);
        if (err == 0 && page_test_and_clear(pg, PG_ERROR)) err = -EIO;
    }
    page_put(pg);
    return err;
}

/* --- Inodes --- */

uint64_t ext2_inode_at(const Ext2Vol *v, uint32_t ino) {
    uint32_t g = (ino - 1) / v->inodes_per_group;
    uint32_t i = (ino - 1) % v->inodes_per_group;
    return ext2_block_at(v, v->grp[g].inode_table) + (uint64_t)i * v->inode_size;
}

static int read_disk_inode(Ext2Vol *v, uint32_t ino, Ext2DiskInode *out) {
    if (ino < 1 || ino > v->inodes_count) return ext2_corrupt(v, "inode number", ino);
    return ext2_meta_read(v, ext2_inode_at(v, ino), out, sizeof(*out));
}

/* A fast symlink keeps its target in i_block (no data block; an extended
 * attribute block doesn't count), as Linux decides it */
static int is_fast_symlink(const Ext2Vol *v, const Ext2DiskInode *d) {
    uint32_t ea_blocks = d->i_file_acl != 0 ? v->block_size >> 9 : 0;
    return d->i_blocks - ea_blocks == 0;
}

/* Operations by type: 0, or -EIO for a mode no file has */
static int set_ops(Ext2Vol *v, Inode *inode) {
    switch (inode->mode & S_IFMT) {
    case S_IFREG:
        inode->iop = &ext2_file_iops;
        inode->fop = &ext2_file_fops;
        return filemap_inode_init(inode, &ext2_aops, 0);
    case S_IFDIR:
        inode->iop = &ext2_dir_iops;
        inode->fop = &ext2_dir_fops;
        return 0;
    case S_IFLNK:
        inode->iop = &ext2_link_iops;
        return 0;
    case S_IFCHR:
    case S_IFBLK:
    case S_IFIFO:
    case S_IFSOCK:
        inode->fop = &ext2_special_fops;
        return 0;
    default:
        return ext2_corrupt(v, "inode mode", inode->mode);
    }
}

/* Fill a new inode from its disk copy: type, owner, size, times, ops. */
static int inode_fill(Ext2Vol *v, Inode *inode, const Ext2DiskInode *d) {
    Ext2Inode *ei = ext2_ei(inode);
    memcpy(ei->block, d->i_block, sizeof(ei->block));
    ei->flags = d->i_flags;
    ei->generation = d->i_generation;
    ei->on_disk = 1;
    inode->uid = d->i_uid | ((uint32_t)d->l_i_uid_high << 16);
    inode->gid = d->i_gid | ((uint32_t)d->l_i_gid_high << 16);
    inode->nlink = d->i_links_count;
    inode->size = (loff_t)d->i_size;
    inode->blocks = d->i_blocks;
    inode->atime.tv_sec = (int32_t)d->i_atime;   /* signed 32-bit seconds, as Linux reads ext2 */
    inode->mtime.tv_sec = (int32_t)d->i_mtime;
    inode->ctime.tv_sec = (int32_t)d->i_ctime;
    inode->atime.tv_nsec = inode->mtime.tv_nsec = inode->ctime.tv_nsec = 0;
    if (S_ISREG(inode->mode)) {
        inode->size |= (loff_t)((uint64_t)d->i_size_high << 32);
        if (inode->size < 0) return ext2_corrupt(v, "file size", d->i_size_high);
    } else if (S_ISLNK(inode->mode)) {
        ei->fast_link = is_fast_symlink(v, d);
        if (inode->size >= (loff_t)v->block_size || (ei->fast_link && inode->size >= EXT2_FAST_LINK))
            return ext2_corrupt(v, "symlink size", (uint64_t)inode->size);
    } else if (S_ISCHR(inode->mode) || S_ISBLK(inode->mode)) {
        /* Linux's old (major << 8 | minor) or new encoding: both are st_rdev's */
        inode->rdev = d->i_block[0] != 0 ? d->i_block[0] : d->i_block[1];
    }
    return set_ops(v, inode);
}

int ext2_iget(Ext2Vol *v, uint32_t ino, Inode **out) {
    Inode *inode = icache_get(&v->sb, ino);
    if (inode != NULL) {
        *out = inode;
        return 0;
    }
    Ext2DiskInode d;
    int err = read_disk_inode(v, ino, &d);
    if (err != 0) return err;
    if (d.i_links_count == 0) return ext2_corrupt(v, "link to a free inode", ino);
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

void ext2_write_inode(Inode *inode) {
    Ext2Vol *v = ext2_vol(inode);
    Ext2Inode *ei = ext2_ei(inode);
    if (!v->writable || ei == NULL || !ei->on_disk) return;
    struct page *pg;
    int err;
    Ext2DiskInode *d = (Ext2DiskInode *)ext2_meta_lock(v, ext2_inode_at(v, (uint32_t)inode->ino), &pg, &err);
    if (d == NULL) {
        kprintf("[EXT2] %s: inode %lu not written (%d)\n", v->name, inode->ino, err);
        return;
    }
    uint64_t size = (uint64_t)__atomic_load_n(&inode->size, __ATOMIC_ACQUIRE);
    d->i_mode = (uint16_t)inode->mode;
    d->i_uid = (uint16_t)inode->uid;
    d->l_i_uid_high = (uint16_t)(inode->uid >> 16);
    d->i_gid = (uint16_t)inode->gid;
    d->l_i_gid_high = (uint16_t)(inode->gid >> 16);
    d->i_size = (uint32_t)size;
    if (S_ISREG(inode->mode)) d->i_size_high = (uint32_t)(size >> 32);
    d->i_atime = (uint32_t)inode->atime.tv_sec;
    d->i_ctime = (uint32_t)inode->ctime.tv_sec;
    d->i_mtime = (uint32_t)inode->mtime.tv_sec;
    d->i_dtime = ei->dtime;
    d->i_links_count = (uint16_t)inode->nlink;
    d->i_blocks = (uint32_t)__atomic_load_n(&inode->blocks, __ATOMIC_RELAXED);
    d->i_flags = ei->flags;
    for (int i = 0; i < EXT2_N_BLOCKS; i++) d->i_block[i] = __atomic_load_n(&ei->block[i], __ATOMIC_RELAXED);
    d->i_generation = ei->generation;
    ext2_meta_done(pg, 1);
    if (S_ISREG(inode->mode) && size > 0x7FFFFFFFu &&
        !(__atomic_fetch_or(&v->ro_compat, EXT2_FEATURE_RO_COMPAT_LARGE_FILE, __ATOMIC_RELAXED) &
          EXT2_FEATURE_RO_COMPAT_LARGE_FILE))
        __atomic_store_n(&v->super_dirty, 1, __ATOMIC_RELAXED);   /* the superblock says so at the next commit */
}

int ext2_new_inode(Inode *dir, uint32_t mode, uint32_t uid, uint32_t gid, Inode **out) {
    Ext2Vol *v = ext2_vol(dir);
    if (!v->writable) return -EROFS;
    uint32_t ino;
    mutex_lock(&v->alloc_lock);
    int err = ext2_alloc_ino(v, (uint32_t)dir->ino, S_ISDIR(mode), &ino);
    mutex_unlock(&v->alloc_lock);
    if (err != 0) return err;
    /* The slot starts as zeros: a 256-byte inode's second half too */
    struct page *pg;
    uint8_t *slot = ext2_meta_lock(v, ext2_inode_at(v, ino), &pg, &err);
    Ext2Inode *ei = slot != NULL ? kmalloc(sizeof(*ei), GFP_ZERO) : NULL;
    Inode *inode = ei != NULL ? inode_new(&v->sb, ino, mode) : NULL;
    if (slot != NULL) {
        if (inode != NULL) memset(slot, 0, v->inode_size);
        ext2_meta_done(pg, inode != NULL);
    }
    if (inode == NULL) {
        kfree(ei);
        mutex_lock(&v->alloc_lock);
        ext2_free_ino(v, ino, S_ISDIR(mode));
        mutex_unlock(&v->alloc_lock);
        return slot == NULL ? err : -ENOMEM;
    }
    inode->priv = ei;
    inode->uid = uid;
    inode->gid = gid;
    inode->nlink = S_ISDIR(mode) ? 2 : 1;
    inode->blocks = 0;
    inode->atime.tv_nsec = inode->mtime.tv_nsec = inode->ctime.tv_nsec = 0;   /* ext2 keeps seconds */
    ei->generation = __atomic_add_fetch(&v->generation, 1, __ATOMIC_RELAXED);
    ei->on_disk = 1;
    err = set_ops(v, inode);
    if (err == 0) ext2_write_inode(inode);
    if (err != 0) {
        inode->nlink = 0;   /* its eviction frees the number */
        iput(inode);
        return err;
    }
    *out = icache_add(inode);
    return 0;
}

/* The last reference went: a file with no name left gives back its blocks
 * and its number (blocks first, then the inode's dtime, then the bitmap:
 * the write order of ext2_alloc.c keeps a crash between them a leak) */
static void ext2_evict(Inode *inode) {
    Ext2Vol *v = ext2_vol(inode);
    Ext2Inode *ei = ext2_ei(inode);
    filemap_inode_evict(inode);   /* a regular file's pages (written first if it is still linked) */
    if (v->writable && ei != NULL && ei->on_disk && inode->nlink == 0) {
        struct timespec now;
        vfs_now(&now);
        mutex_lock(&v->alloc_lock);
        if (!(S_ISLNK(inode->mode) && ei->fast_link)) ext2_truncate_blocks(inode, 0);
        __atomic_store_n(&inode->size, 0, __ATOMIC_RELEASE);
        ei->dtime = (uint32_t)now.tv_sec;
        ext2_write_inode(inode);
        ext2_free_ino(v, (uint32_t)inode->ino, S_ISDIR(inode->mode));
        mutex_unlock(&v->alloc_lock);
    }
    kfree(ei);
}

/* --- Symlinks and special files --- */

static int ext2_readlink(Inode *inode, char *buf, size_t size) {
    Ext2Vol *v = ext2_vol(inode);
    size_t len = (size_t)inode->size;
    size_t n = len < size ? len : size;
    if (ext2_ei(inode)->fast_link) {
        memcpy(buf, ext2_ei(inode)->block, n);   /* fast: in the inode */
        return (int)len;
    }
    int64_t b = ext2_bmap(inode, 0);
    if (b < 0) return (int)b;
    if (b == 0) return ext2_corrupt(v, "symlink block in inode", inode->ino);
    int err = ext2_meta_read(v, ext2_block_at(v, (uint32_t)b), buf, n);
    return err != 0 ? err : (int)len;
}

/* Devices, FIFOs and sockets on an arc_os volume have no driver behind them
 * (no device-number registry yet): opening one is -ENXIO */
static int ext2_special_open(Inode *inode, File *file) {
    (void)inode, (void)file;
    return -ENXIO;
}

static const InodeOps ext2_link_iops = {
    .readlink = ext2_readlink,
};

static const FileOps ext2_special_fops = {
    .open = ext2_special_open,
};

static const SuperOps ext2_sops = {
    .evict_inode = ext2_evict,
    .write_inode = ext2_write_inode,
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
    v->blocks_per_group = s->s_blocks_per_group;
    v->inode_size = s->s_inode_size;
    v->first_data_block = s->s_first_data_block;
    v->first_ino = s->s_first_ino;
    v->ro_compat = s->s_feature_ro_compat;
    v->filetype = (s->s_feature_incompat & EXT2_FEATURE_INCOMPAT_FILETYPE) != 0;
    v->max_bytes = ext2_max_size(v->block_shift);
    uint32_t first = v->first_data_block;
    if (v->blocks_per_group == 0 || v->blocks_per_group > v->block_size * 8 || v->inodes_per_group == 0 ||
        v->inodes_per_group > v->block_size * 8 || v->blocks_count <= first + 1 ||
        first != (v->block_size == 1024 ? 1u : 0u) || (uint64_t)v->blocks_count << v->block_shift > bytes ||
        (uint64_t)v->inodes_per_group * v->inode_size % v->block_size != 0 || v->first_ino <= EXT2_ROOT_INO) {
        kprintf("[EXT2] %s: bad geometry\n", name);
        return -EINVAL;
    }
    v->groups = (v->blocks_count - first + v->blocks_per_group - 1) / v->blocks_per_group;
    if (v->inodes_count < EXT2_ROOT_INO || (uint64_t)v->inodes_count > (uint64_t)v->groups * v->inodes_per_group) {
        kprintf("[EXT2] %s: bad inode count\n", name);
        return -EINVAL;
    }
    v->gdt_blocks = (uint32_t)(((uint64_t)v->groups * sizeof(Ext2DiskGroup) + v->block_size - 1) >> v->block_shift);
    v->free_blocks = s->s_free_blocks_count;
    v->free_inodes = s->s_free_inodes_count;
    v->mount_state = s->s_state;
    v->generation = s->s_mtime ^ s->s_wtime ^ s->s_free_blocks_count;
    return 0;
}

/* Is block b inside group g's range? */
static int in_group(const Ext2Vol *v, uint32_t g, uint32_t b) {
    uint64_t start = v->first_data_block + (uint64_t)g * v->blocks_per_group;
    return b >= start && b < start + v->blocks_per_group && b < v->blocks_count;
}

/* Read the group descriptors (after the superblock's block) and check each
 * group's bitmaps and inode table lie inside the group. The free counts
 * are summed from the groups (the superblock's are only a summary). */
static int read_groups(Ext2Vol *v, const char *name) {
    v->grp = kmalloc((size_t)v->groups * sizeof(Ext2Group), GFP_ZERO);
    v->grp_dirty = kmalloc(((size_t)v->groups + 63) / 64 * sizeof(uint64_t), GFP_ZERO);
    if (v->grp == NULL || v->grp_dirty == NULL) return -ENOMEM;
    uint32_t table_blocks = (uint32_t)((uint64_t)v->inodes_per_group * v->inode_size >> v->block_shift);
    uint64_t gdt = ext2_block_at(v, v->first_data_block + 1);
    uint64_t free_blocks = 0, free_inodes = 0;
    for (uint32_t g = 0; g < v->groups; g++) {
        Ext2DiskGroup gd;
        int err = ext2_meta_read(v, gdt + (uint64_t)g * sizeof(gd), &gd, sizeof(gd));
        if (err != 0) return err;
        /* M1-13 accepted any position inside the volume; a writer needs
         * each in its own group (as mke2fs places them without flex_bg) */
        int inside =
            gd.bg_inode_table > v->first_data_block && (uint64_t)gd.bg_inode_table + table_blocks <= v->blocks_count;
        if (!inside) {
            kprintf("[EXT2] %s: group %u: inode table outside the volume\n", name, g);
            return -EINVAL;
        }
        if (!in_group(v, g, gd.bg_block_bitmap) || !in_group(v, g, gd.bg_inode_bitmap) ||
            gd.bg_free_inodes_count > v->inodes_per_group || gd.bg_free_blocks_count > v->blocks_per_group) {
            v->ro_forced = 1;   /* readable; a writer would trust a wrong bitmap */
            kprintf("[EXT2] %s: group %u: bitmaps or counts out of place (read-only)\n", name, g);
        }
        Ext2Group *gr = &v->grp[g];
        gr->block_bitmap = gd.bg_block_bitmap;
        gr->inode_bitmap = gd.bg_inode_bitmap;
        gr->inode_table = gd.bg_inode_table;
        gr->free_blocks = gd.bg_free_blocks_count;
        gr->free_inodes = gd.bg_free_inodes_count;
        gr->used_dirs = gd.bg_used_dirs_count;
        free_blocks += gr->free_blocks;
        free_inodes += gr->free_inodes;
    }
    if (free_blocks != v->free_blocks || free_inodes != v->free_inodes) v->super_dirty = 1;
    v->free_blocks = (uint32_t)free_blocks;
    v->free_inodes = (uint32_t)free_inodes;
    return 0;
}

/* Why the volume can't be written, or NULL */
static const char *why_read_only(const Ext2Vol *v) {
    if (v->ro_forced) return "unknown features or damaged descriptors";
    if (v->block_size != PAGE_SIZE) return "writing needs 4 KiB blocks";
    if (v->dev->flags & BLK_RDONLY) return "the device is read-only";
    return NULL;
}

/* Write the superblock's state, mount count and times now, and flush: the
 * disk says "in use" before anything else changes (mount), or "as found"
 * after everything else is on it (shutdown). 0 or -errno. */
static int write_state(Ext2Vol *v, uint16_t state, int mounting) {
    struct page *pg;
    int err;
    Ext2DiskSuper *s = (Ext2DiskSuper *)ext2_meta_lock(v, EXT2_SUPER_OFFSET, &pg, &err);
    if (s == NULL) return err;
    struct timespec now;
    vfs_now(&now);
    s->s_state = state;
    s->s_wtime = (uint32_t)now.tv_sec;
    if (mounting) {
        s->s_mnt_count++;
        s->s_mtime = (uint32_t)now.tv_sec;
    }
    filemap_set_dirty(pg);
    err = filemap_writepage_locked(&v->meta, pg);
    page_unlock(pg);
    filemap_wait_writeback(pg);
    if (err == 0 && page_test_and_clear(pg, PG_ERROR)) err = -EIO;
    page_put(pg);
    if (err == 0) err = filemap_flush_device(&v->meta);
    return err;
}

/* Make a checked volume writable: state "in use" on the disk first */
static void go_writable(Ext2Vol *v, const char *name) {
    const char *why = why_read_only(v);
    if (why != NULL) {
        kprintf("[EXT2] %s: read-only (%s)\n", name, why);
        return;
    }
    if (!(v->mount_state & EXT2_VALID_FS) || (v->mount_state & EXT2_ERROR_FS))
        kprintf("[EXT2] %s: WARNING: not cleanly unmounted or has errors; mounted read-write unchecked "
                "(e2fsck at boot comes with M5-01)\n",
                name);
    int err = write_state(v, (uint16_t)(v->mount_state & ~EXT2_VALID_FS), 1);
    if (err != 0) {
        kprintf("[EXT2] %s: superblock not writable (%d): read-only\n", name, err);
        return;
    }
    v->writable = 1;
    v->sb.flags &= ~(uint32_t)SB_RDONLY;
}

static void mount_fail(Ext2Vol *v) {
    filemap_bdev_release(&v->meta);
    kfree(v->grp);
    kfree(v->grp_dirty);
    kfree(v);
}

SuperBlock *ext2_mount(BlockDevice *dev, uint64_t bytes, const char *name, uint32_t flags, int *err) {
    *err = -EINVAL;
    if (dev == NULL || bytes < EXT2_SUPER_OFFSET * 2) return NULL;
    Ext2Vol *v = kmalloc(sizeof(*v), GFP_ZERO);
    if (v == NULL) {
        *err = -ENOMEM;
        return NULL;
    }
    v->dev = dev;
    strncpy(v->name, name != NULL ? name : "?", sizeof(v->name) - 1);
    mutex_init(&v->alloc_lock, LOCK_LEVEL_FS_ALLOC);
    filemap_bdev_init(&v->meta, dev, (loff_t)bytes, &ext2_meta_aops);
    vfs_sb_init(&v->sb, "ext2", &ext2_sops);
    v->sb.flags = SB_RDONLY;
    v->sb.bdev = dev;
    Ext2DiskSuper s;
    *err = ext2_meta_read(v, EXT2_SUPER_OFFSET, &s, sizeof(s));
    if (*err == 0) *err = check_super(v, &s, bytes, v->name);
    if (*err == 0) *err = read_groups(v, v->name);
    v->blocks = (FilemapBlocks){dev, v->block_shift, ext2_get_block};
    Inode *root = NULL;
    if (*err == 0) *err = ext2_iget(v, EXT2_ROOT_INO, &root);
    if (*err == 0 && !S_ISDIR(root->mode)) {
        kprintf("[EXT2] %s: root is not a directory\n", v->name);
        *err = -EINVAL;
    }
    if (*err != 0) {
        if (root != NULL) iput(root);   /* frees it: the volume had no other user */
        mount_fail(v);
        return NULL;
    }
    v->sb.root = root;   /* the superblock's reference: volumes are never unmounted */
    if (!(v->mount_state & EXT2_VALID_FS) || (v->mount_state & EXT2_ERROR_FS)) {
        if (!(flags & EXT2_MOUNT_RW))
            kprintf("[EXT2] %s: not cleanly unmounted or has errors (read-only, carrying on)\n", v->name);
    }
    if (flags & EXT2_MOUNT_RW) go_writable(v, v->name);
    kprintf("[EXT2] %s: %u blocks of %u bytes, %u groups, %u inodes of %u bytes, %s\n", v->name, v->blocks_count,
            v->block_size, v->groups, v->inodes_count, v->inode_size, v->writable ? "read-write" : "read-only");
    return &v->sb;
}

int ext2_shutdown(SuperBlock *sb) {
    Ext2Vol *v = (Ext2Vol *)sb;
    if (!v->writable) return 0;
    __atomic_or_fetch(&sb->flags, SB_RDONLY, __ATOMIC_RELAXED);   /* new writes, creates and removes: -EROFS */
    wb_sync_all();   /* every file's data, then the metadata (ext2_meta_writepages), flushed */
    mutex_lock(&v->alloc_lock);
    ext2_commit(v);
    mutex_unlock(&v->alloc_lock);
    int err = filemap_write_all(&v->meta);   /* anything a last eviction changed */
    if (err == 0) err = filemap_flush_device(&v->meta);
    /* Last, alone: the state found at mount (clean stays clean, dirty stays dirty) */
    if (err == 0) err = write_state(v, v->mount_state, 0);
    v->writable = 0;
    if (err != 0)
        kprintf("[EXT2] %s: shutdown write failed (%d): the volume stays marked in use\n", v->name, err);
    else
        kprintf("[EXT2] %s: written back, %s, read-only\n", v->name,
                (v->mount_state & EXT2_VALID_FS) && !(v->mount_state & EXT2_ERROR_FS) ? "clean" : "still unchecked");
    return err;
}

int ext2_ro_forced(const SuperBlock *sb) {
    return ((const Ext2Vol *)sb)->ro_forced;
}

int ext2_writable(const SuperBlock *sb) {
    return ((const Ext2Vol *)sb)->writable;
}
