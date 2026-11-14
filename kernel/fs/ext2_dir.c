/* arc_os — ext2 directories and names (ext2_internal.h; read M1-13, write
 * M1-15)
 *
 * Owns: directory entries (lookup, getdents, ".."; adding, removing and
 * repointing entries), and the namespace operations on top of them: create,
 * mkdir, unlink, rmdir, rename and symlink.
 *
 * A directory is a file of blocks holding entries (Ext2DirEntry): inode,
 * rec_len (bytes to the next entry), name. Entries never cross a block; the
 * last one in a block runs to its end. A new entry goes into the first gap
 * big enough: an unused entry (inode 0), or the slack after an entry's own
 * name (the entry is split); else a new block is added at the end, written
 * to the disk before the directory points at it. A removed entry is merged
 * into the one before it in the block (the first of a block becomes unused:
 * inode 0). Directory blocks are metadata: they live in the volume's
 * metadata cache, and freeing one drops its cached page.
 *
 * htree (dir_index) directories are read linearly (the index hides inside
 * entries a linear reader skips); changing one clears its EXT2_INDEX_FL, so
 * Linux and e2fsck read it linearly too (Linux ext2 does the same).
 *
 * Link counts (as Linux): a file's counts its names; a directory's is 2
 * (its name and ".") plus one per subdirectory (their ".."). rename adds the
 * new name before it removes the old one.
 *
 * Locks: the VFS holds the directory locks (shared for lookup and getdents,
 * exclusive to change); a block being changed is locked as a metadata page,
 * one at a time, never across alloc_lock.
 *
 * Cost: lookup, getdents and every change are linear in the directory's
 * blocks (no index); adding a block costs one allocation and one
 * synchronous write. */

#include "fs/ext2_internal.h"
#include "lib/mem.h"
#include "lib/string.h"
#include "lib/kprintf.h"
#include "uapi/dirent.h"
#include "uapi/errno.h"

#define EXT2_DIR_MAX_BYTES 0xFFFFF000ull   /* a directory's size is 32 bits on the disk */

uint8_t ext2_mode_to_ft(uint32_t mode) {
    switch (mode & S_IFMT) {
    case S_IFREG:
        return EXT2_FT_REG_FILE;
    case S_IFDIR:
        return EXT2_FT_DIR;
    case S_IFCHR:
        return EXT2_FT_CHRDEV;
    case S_IFBLK:
        return EXT2_FT_BLKDEV;
    case S_IFIFO:
        return EXT2_FT_FIFO;
    case S_IFSOCK:
        return EXT2_FT_SOCK;
    case S_IFLNK:
        return EXT2_FT_SYMLINK;
    default:
        return 0;
    }
}

/* Is the entry at off of a block of bs bytes well formed? */
static int entry_ok(const Ext2Vol *v, const uint8_t *b, uint32_t off) {
    const Ext2DirEntry *e = (const Ext2DirEntry *)(b + off);
    uint32_t bs = v->block_size;
    uint32_t len = off + EXT2_DIRENT_HEAD <= bs ? e->rec_len : 0;
    return len >= EXT2_DIRENT_HEAD && (len & 3) == 0 && len <= bs - off &&
           (uint32_t)e->name_len + EXT2_DIRENT_HEAD <= len && e->inode <= v->inodes_count;
}

/* --- Reading --- */

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
        if (lblk > UINT32_MAX) return ext2_corrupt(v, "directory size", size);
        int64_t pblk = ext2_bmap(dir, (uint32_t)lblk);
        if (pblk < 0) return (int)pblk;
        if (pblk == 0) {   /* a hole in a directory: nothing there */
            pos = base + bs;
            continue;
        }
        struct page *pg;
        int err;
        const uint8_t *b = ext2_meta_map(v, ext2_block_at(v, (uint32_t)pblk), &pg, &err);
        if (b == NULL) return err;
        int rc = 0;
        for (uint32_t off = 0; off < bs && rc == 0;) {
            const Ext2DirEntry *e = (const Ext2DirEntry *)(b + off);
            if (!entry_ok(v, b, off)) {
                rc = ext2_corrupt(v, "directory entry in inode", dir->ino);
                break;
            }
            if (base + off >= pos && e->inode != 0 && e->name_len != 0)
                rc = visit(e, base + off, base + off + e->rec_len, ctx);
            off += e->rec_len;
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

static int not_dots_visit(const Ext2DirEntry *e, uint64_t pos, uint64_t next, void *arg) {
    (void)pos, (void)next, (void)arg;
    int dot = e->name[0] == '.' && (e->name_len == 1 || (e->name_len == 2 && e->name[1] == '.'));
    return !dot;
}

/* 1 if dir has no entries but "." and "..", 0 if it has, or -errno */
static int dir_empty(Inode *dir) {
    int rc = dir_walk(dir, 0, not_dots_visit, NULL);
    return rc < 0 ? rc : rc == 0;
}

/* --- Changing entries (directory locked exclusive) --- */

void ext2_dir_unindex(Inode *dir) {
    Ext2Inode *ei = ext2_ei(dir);
    if (!(ei->flags & EXT2_INDEX_FL)) return;
    ei->flags &= ~(uint32_t)EXT2_INDEX_FL;
    ext2_write_inode(dir);
}

/* Where an entry is: its block, its offset there, the offset of the entry
 * before it in that block (-1: it is the first) */
typedef struct {
    uint32_t pblk;
    uint32_t off;
    int32_t prev;
    uint32_t ino;
} DirSlot;

/* Find name in dir: 0 with *out, -ENOENT, or -errno */
static int find_entry(Inode *dir, const char *name, DirSlot *out) {
    Ext2Vol *v = ext2_vol(dir);
    size_t len = strlen(name);
    uint64_t nblocks = (uint64_t)dir->size >> v->block_shift;
    for (uint64_t lblk = 0; lblk < nblocks; lblk++) {
        int64_t pblk = ext2_bmap(dir, (uint32_t)lblk);
        if (pblk < 0) return (int)pblk;
        if (pblk == 0) continue;
        struct page *pg;
        int err;
        const uint8_t *b = ext2_meta_map(v, ext2_block_at(v, (uint32_t)pblk), &pg, &err);
        if (b == NULL) return err;
        int32_t prev = -1;
        for (uint32_t off = 0; off < v->block_size;) {
            if (!entry_ok(v, b, off)) {
                page_put(pg);
                return ext2_corrupt(v, "directory entry in inode", dir->ino);
            }
            const Ext2DirEntry *e = (const Ext2DirEntry *)(b + off);
            if (e->inode != 0 && e->name_len == len && memcmp(e->name, name, len) == 0) {
                *out = (DirSlot){(uint32_t)pblk, off, prev, e->inode};
                page_put(pg);
                return 0;
            }
            prev = (int32_t)off;
            off += e->rec_len;
        }
        page_put(pg);
    }
    return -ENOENT;
}

/* Lock the block of a slot found with find_entry: the entry, or NULL */
static Ext2DirEntry *slot_lock(Ext2Vol *v, const DirSlot *s, uint8_t **block, struct page **pg, int *err) {
    *block = ext2_meta_lock(v, ext2_block_at(v, s->pblk), pg, err);
    return *block != NULL ? (Ext2DirEntry *)(*block + s->off) : NULL;
}

static void fill_entry(Ext2Vol *v, Ext2DirEntry *e, const char *name, size_t len, uint32_t ino, uint8_t ft) {
    e->inode = ino;
    e->name_len = (uint8_t)len;
    e->file_type = v->filetype ? ft : 0;
    memcpy(e->name, name, len);
}

/* Remove name from dir. 0 or -errno. */
static int remove_entry(Inode *dir, const char *name) {
    Ext2Vol *v = ext2_vol(dir);
    DirSlot s;
    int err = find_entry(dir, name, &s);
    if (err != 0) return err;
    uint8_t *b;
    struct page *pg;
    Ext2DirEntry *e = slot_lock(v, &s, &b, &pg, &err);
    if (e == NULL) return err;
    if (s.prev >= 0)
        ((Ext2DirEntry *)(b + s.prev))->rec_len += e->rec_len;   /* merged into the one before */
    else
        e->inode = 0;   /* the first of its block: unused */
    ext2_meta_done(pg, 1);
    ext2_dir_unindex(dir);
    return 0;
}

/* Point dir's entry name at ino (rename over a name, a moved directory's
 * ".."). 0 or -errno. */
static int set_entry(Inode *dir, const char *name, uint32_t ino, uint8_t ft) {
    Ext2Vol *v = ext2_vol(dir);
    DirSlot s;
    int err = find_entry(dir, name, &s);
    if (err != 0) return err;
    uint8_t *b;
    struct page *pg;
    Ext2DirEntry *e = slot_lock(v, &s, &b, &pg, &err);
    if (e == NULL) return err;
    e->inode = ino;
    if (v->filetype) e->file_type = ft;
    ext2_meta_done(pg, 1);
    ext2_dir_unindex(dir);
    return 0;
}

/* Put the entry into block pblk if it has room: 1 if it did, 0, or -errno */
static int add_in_block(Inode *dir, uint32_t pblk, const char *name, size_t len, uint32_t ino, uint8_t ft) {
    Ext2Vol *v = ext2_vol(dir);
    uint32_t need = ext2_rec_len((uint32_t)len);
    struct page *pg;
    int err;
    uint8_t *b = ext2_meta_lock(v, ext2_block_at(v, pblk), &pg, &err);
    if (b == NULL) return err;
    for (uint32_t off = 0; off < v->block_size;) {
        if (!entry_ok(v, b, off)) {
            ext2_meta_done(pg, 0);
            return ext2_corrupt(v, "directory entry in inode", dir->ino);
        }
        Ext2DirEntry *e = (Ext2DirEntry *)(b + off);
        uint32_t used = e->inode != 0 ? ext2_rec_len(e->name_len) : 0;
        if (e->rec_len - used >= need) {
            if (used != 0) {   /* the slack after e's name: split */
                Ext2DirEntry *n = (Ext2DirEntry *)(b + off + used);
                n->rec_len = (uint16_t)(e->rec_len - used);
                e->rec_len = (uint16_t)used;
                e = n;
            }
            fill_entry(v, e, name, len, ino, ft);
            ext2_meta_done(pg, 1);
            return 1;
        }
        off += e->rec_len;
    }
    ext2_meta_done(pg, 0);
    return 0;
}

/* A new block for dir (or a directory's first): one entry spanning it, or
 * "." and ".." when parent != 0; written before anything points at it, then
 * linked as file block lblk. 0 or -errno. */
static int add_block(Inode *dir, uint32_t lblk, const char *name, size_t len, uint32_t ino, uint8_t ft,
                     uint32_t parent) {
    Ext2Vol *v = ext2_vol(dir);
    uint32_t bs = v->block_size;
    int64_t prev = lblk > 0 ? ext2_bmap(dir, lblk - 1) : 0;
    uint32_t goal = prev > 0
                        ? (uint32_t)prev + 1
                        : v->first_data_block + (uint32_t)(dir->ino - 1) / v->inodes_per_group * v->blocks_per_group;
    uint32_t blk;
    mutex_lock(&v->alloc_lock);
    int err = ext2_alloc_block(v, goal, &blk);
    struct page *pg;
    uint8_t *b = err == 0 ? ext2_meta_new(v, blk, &pg, &err) : NULL;
    if (b != NULL) {
        Ext2DirEntry *e = (Ext2DirEntry *)b;
        if (parent != 0) {   /* a new directory: "." and ".." */
            e->rec_len = (uint16_t)ext2_rec_len(1);
            fill_entry(v, e, ".", 1, ino, EXT2_FT_DIR);
            e = (Ext2DirEntry *)(b + e->rec_len);
            e->rec_len = (uint16_t)(bs - ext2_rec_len(1));
            fill_entry(v, e, "..", 2, parent, EXT2_FT_DIR);
        } else {
            e->rec_len = (uint16_t)bs;
            fill_entry(v, e, name, len, ino, ft);
        }
        err = ext2_meta_new_done(v, pg, 1);
    }
    if (err == 0) {
        int fresh;
        int64_t got = ext2_map_alloc(dir, lblk, blk, &fresh);
        if (got < 0) err = (int)got;
    }
    if (err != 0 && b != NULL) ext2_free_blocks(v, blk, 1, 1);
    mutex_unlock(&v->alloc_lock);
    if (err != 0) return err;
    __atomic_store_n(&dir->size, (loff_t)((uint64_t)(lblk + 1) << v->block_shift), __ATOMIC_RELEASE);
    ext2_write_inode(dir);
    return 0;
}

/* Add name -> ino to dir (the name is not there yet). 0 or -errno. */
static int add_entry(Inode *dir, const char *name, uint32_t ino, uint8_t ft) {
    Ext2Vol *v = ext2_vol(dir);
    size_t len = strlen(name);
    if (len == 0 || len > 255) return -ENAMETOOLONG;
    uint64_t nblocks = (uint64_t)dir->size >> v->block_shift;
    for (uint64_t lblk = 0; lblk < nblocks; lblk++) {
        int64_t pblk = ext2_bmap(dir, (uint32_t)lblk);
        if (pblk < 0) return (int)pblk;
        if (pblk == 0) continue;
        int rc = add_in_block(dir, (uint32_t)pblk, name, len, ino, ft);
        if (rc < 0) return rc;
        if (rc == 1) {
            ext2_dir_unindex(dir);
            return 0;
        }
    }
    if (((nblocks + 1) << v->block_shift) > EXT2_DIR_MAX_BYTES) return -EFBIG;
    int err = add_block(dir, (uint32_t)nblocks, name, len, ino, ft, 0);
    if (err == 0) ext2_dir_unindex(dir);
    return err;
}

/* --- Namespace operations --- */

static int ext2_create(Inode *dir, const char *name, uint32_t mode, uint32_t uid, uint32_t gid, Inode **out) {
    Inode *inode;
    int err = ext2_new_inode(dir, mode, uid, gid, &inode);
    if (err != 0) return err;
    err = add_entry(dir, name, (uint32_t)inode->ino, ext2_mode_to_ft(mode));
    if (err != 0) {
        inode->nlink = 0;   /* its eviction gives the number back */
        iput(inode);
        return err;
    }
    *out = inode;
    return 0;
}

static int ext2_mkdir(Inode *dir, const char *name, uint32_t mode, uint32_t uid, uint32_t gid) {
    Inode *inode;
    int err = ext2_new_inode(dir, mode, uid, gid, &inode);
    if (err != 0) return err;
    err = add_block(inode, 0, NULL, 0, (uint32_t)inode->ino, EXT2_FT_DIR, (uint32_t)dir->ino);
    if (err == 0) err = add_entry(dir, name, (uint32_t)inode->ino, EXT2_FT_DIR);
    if (err != 0) {
        inode->nlink = 0;   /* its eviction frees its block and number */
        iput(inode);
        return err;
    }
    dir->nlink++;   /* the new directory's ".." */
    iput(inode);
    return 0;
}

static int ext2_unlink(Inode *dir, const char *name, Inode *victim) {
    if (!ext2_vol(dir)->writable) return -EROFS;
    DirSlot s;
    int err = find_entry(dir, name, &s);
    if (err != 0) return err;
    if (s.ino != victim->ino) return -ENOENT;
    err = remove_entry(dir, name);
    if (err != 0) return err;
    victim->nlink--;
    ext2_write_inode(victim);
    return 0;
}

static int ext2_rmdir(Inode *dir, const char *name, Inode *victim) {
    if (!ext2_vol(dir)->writable) return -EROFS;
    DirSlot s;
    int err = find_entry(dir, name, &s);
    if (err != 0) return err;
    if (s.ino != victim->ino) return -ENOENT;
    int empty = dir_empty(victim);
    if (empty <= 0) return empty < 0 ? empty : -ENOTEMPTY;
    err = remove_entry(dir, name);
    if (err != 0) return err;
    victim->nlink = 0;   /* its name and its "." */
    dir->nlink--;        /* its ".." */
    ext2_write_inode(victim);
    return 0;
}

static int ext2_rename(Inode *odir, const char *oname, Inode *src, Inode *ndir, const char *nname, Inode *victim) {
    if (!ext2_vol(odir)->writable) return -EROFS;
    DirSlot s;
    int err = find_entry(odir, oname, &s);
    if (err != 0) return err;
    if (s.ino != src->ino) return -ENOENT;
    int isdir = S_ISDIR(src->mode);
    uint8_t ft = ext2_mode_to_ft(src->mode);
    if (victim != NULL) {
        DirSlot t;
        err = find_entry(ndir, nname, &t);
        if (err != 0) return err;
        if (t.ino != victim->ino) return -ENOENT;
        if (S_ISDIR(victim->mode)) {
            int empty = dir_empty(victim);
            if (empty <= 0) return empty < 0 ? empty : -ENOTEMPTY;
        }
        /* The new name first: from here on src has it */
        err = set_entry(ndir, nname, (uint32_t)src->ino, ft);
        if (err != 0) return err;
        /* A directory replaces a directory: ndir loses victim's ".." and
         * gains src's, so its count stays */
        victim->nlink = S_ISDIR(victim->mode) ? 0 : victim->nlink - 1;
        ext2_write_inode(victim);
    } else {
        err = add_entry(ndir, nname, (uint32_t)src->ino, ft);
        if (err != 0) return err;
        if (isdir) ndir->nlink++;
    }
    err = remove_entry(odir, oname);
    if (err != 0)
        kprintf("[EXT2] %s: rename left the old name of inode %lu (%d)\n", ext2_vol(odir)->name, src->ino, err);
    if (isdir) {
        if (odir != ndir) (void)set_entry(src, "..", (uint32_t)ndir->ino, EXT2_FT_DIR);
        odir->nlink--;
    }
    return 0;   /* the VFS writes both directories and src (vfs_inode_dirty) */
}

static int ext2_symlink(Inode *dir, const char *name, const char *target, uint32_t uid, uint32_t gid) {
    Ext2Vol *v = ext2_vol(dir);
    size_t len = strlen(target);
    if (len >= v->block_size) return -ENAMETOOLONG;
    Inode *inode;
    int err = ext2_new_inode(dir, S_IFLNK | 0777, uid, gid, &inode);
    if (err != 0) return err;
    Ext2Inode *ei = ext2_ei(inode);
    if (len < EXT2_FAST_LINK) {   /* fast: the target in i_block */
        memcpy(ei->block, target, len);
        ei->fast_link = 1;
    } else {   /* slow: in a block, written before the inode points at it */
        uint32_t blk;
        mutex_lock(&v->alloc_lock);
        err = ext2_alloc_block(v, v->first_data_block, &blk);
        struct page *pg;
        uint8_t *b = err == 0 ? ext2_meta_new(v, blk, &pg, &err) : NULL;
        if (b != NULL) {
            memcpy(b, target, len);
            err = ext2_meta_new_done(v, pg, 1);
            int fresh;
            int64_t got = err == 0 ? ext2_map_alloc(inode, 0, blk, &fresh) : 0;
            if (got < 0) err = (int)got;
            if (err != 0) ext2_free_blocks(v, blk, 1, 1);
        }
        mutex_unlock(&v->alloc_lock);
    }
    if (err == 0) {
        __atomic_store_n(&inode->size, (loff_t)len, __ATOMIC_RELEASE);
        ext2_write_inode(inode);
        err = add_entry(dir, name, (uint32_t)inode->ino, EXT2_FT_SYMLINK);
    }
    if (err != 0) inode->nlink = 0;   /* its eviction frees its block and number */
    iput(inode);
    return err;
}

const InodeOps ext2_dir_iops = {
    .lookup = ext2_lookup,
    .create = ext2_create,
    .mkdir = ext2_mkdir,
    .unlink = ext2_unlink,
    .rmdir = ext2_rmdir,
    .rename = ext2_rename,
    .symlink = ext2_symlink,
};

const FileOps ext2_dir_fops = {
    .iterate = ext2_iterate,
    .fsync = ext2_fsync,
};
