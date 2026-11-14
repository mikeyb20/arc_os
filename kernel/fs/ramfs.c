/* arc_os — ramfs: files and directories in memory (ramfs.h, M1-11)
 *
 * Owns: one in-memory filesystem instance per ramfs_create (the root
 * filesystem today): directory entries and file data.
 *
 * Directories: each entry sits in two red-black trees of its directory, by
 * name (lookup) and by a sequence number given at insertion (iteration). A
 * getdents cookie is "the entry with sequence >= cookie - 3", so iteration
 * resumes in O(log n) and is not confused by entries removed in between.
 * Files: their data is their page-cache pages (inode->mapping, M1-12), so
 * read, write and mmap share them; sparse (a missing page reads as zeros), so
 * a file may end far beyond 4 GiB while using only the pages written.
 *
 * Invariants: a directory entry holds one inode reference, so a linked inode
 * lives at least as long as its entry; an unlinked one lives until its last
 * File or lookup reference goes (then evict frees its pages). nlink counts
 * entries naming the inode, plus "." and each subdirectory's ".." for
 * directories. Data beyond `size` inside the last page is always zero
 * (filemap.c keeps it so).
 *
 * Locks: the VFS holds the inode lock around every call (vfs.h); ramfs adds
 * none. inode_seq is atomic.
 *
 * Cost: lookup O(log n) name compares; create/unlink/rename O(log n);
 * read/write one page-cache lookup per page. */

#include "fs/ramfs.h"
#include "mm/kmalloc.h"
#include "mm/filemap.h"
#include "lib/mem.h"
#include "lib/string.h"
#include "lib/rbtree.h"

#define RAMFS_COOKIE0 3   /* cookie of sequence 0: after ".", "..", and 2 = "end of dots" */

typedef struct RamfsDirent {
    RbNode by_name;
    RbNode by_seq;
    uint64_t seq;
    Inode *inode;   /* a reference */
    uint16_t len;
    char name[];
} RamfsDirent;

typedef struct RamfsInfo {
    RbRoot a;             /* directory: entries by name */
    RbRoot b;             /* directory: entries by sequence */
    uint64_t next_seq;    /* directory */
    uint32_t count;       /* directory: entries */
    Inode *parent;        /* directory: ".." (no reference: a linked directory's parent can't go
                           * away; changed by rename and rmdir under this directory's lock) */
} RamfsInfo;

typedef struct RamfsSb {
    SuperBlock sb;
    uint64_t next_ino;
} RamfsSb;

static const InodeOps ramfs_dir_iops;
static const FileOps ramfs_dir_fops;
static const FileOps ramfs_file_fops;
static const InodeOps ramfs_file_iops;

static RamfsInfo *info(const Inode *inode) {
    return (RamfsInfo *)inode->priv;
}

/* --- Files: the data lives in the page cache (mm/filemap.h); its pages
 * are the storage (AS_MEMBACKED: never evicted, holes read as zeros) --- */

static int ramfs_setattr(Inode *inode, const InodeAttr *attr) {
    if (attr->valid & ATTR_SIZE) {
        filemap_truncate(inode, attr->size);
        vfs_now(&inode->mtime);
    }
    if (attr->valid & ATTR_MODE) inode->mode = (inode->mode & S_IFMT) | (attr->mode & 07777);
    if (attr->valid & ATTR_UID) inode->uid = attr->uid;
    if (attr->valid & ATTR_GID) inode->gid = attr->gid;
    return 0;
}

/* --- Directories --- */

static RamfsDirent *dirent_find(const RamfsInfo *ri, const char *name) {
    RbNode *n = ri->a.node;
    while (n != NULL) {
        RamfsDirent *d = rb_entry(n, RamfsDirent, by_name);
        int c = strcmp(name, d->name);
        if (c == 0) return d;
        n = c < 0 ? n->left : n->right;
    }
    return NULL;
}

/* Put a new entry for inode (taking over the caller's reference) in dir. */
static int dirent_add(Inode *dir, const char *name, Inode *inode) {
    RamfsInfo *ri = info(dir);
    size_t len = strlen(name);
    RamfsDirent *d = kmalloc(sizeof(*d) + len + 1, 0);
    if (d == NULL) return -ENOMEM;
    memcpy(d->name, name, len + 1);
    d->len = (uint16_t)len;
    d->inode = inode;
    d->seq = ri->next_seq++;

    RbNode **link = &ri->a.node, *parent = NULL;
    while (*link != NULL) {
        parent = *link;
        link = strcmp(name, rb_entry(*link, RamfsDirent, by_name)->name) < 0 ? &(*link)->left : &(*link)->right;
    }
    rb_link_node(&d->by_name, parent, link);
    rb_insert_color(&d->by_name, &ri->a);

    link = &ri->b.node;
    parent = NULL;
    while (*link != NULL) {   /* sequence numbers only grow: always the rightmost slot */
        parent = *link;
        link = &(*link)->right;
    }
    rb_link_node(&d->by_seq, parent, link);
    rb_insert_color(&d->by_seq, &ri->b);
    ri->count++;
    return 0;
}

/* Take the entry out of dir and free it; returns its inode reference. */
static Inode *dirent_del(Inode *dir, RamfsDirent *d) {
    RamfsInfo *ri = info(dir);
    rb_erase(&d->by_name, &ri->a);
    rb_erase(&d->by_seq, &ri->b);
    ri->count--;
    Inode *inode = d->inode;
    kfree(d);
    return inode;
}

static Inode *ramfs_new_inode(SuperBlock *sb, uint32_t mode, uint32_t uid, uint32_t gid) {
    RamfsSb *rs = (RamfsSb *)sb;
    Inode *inode = inode_new(sb, __atomic_fetch_add(&rs->next_ino, 1, __ATOMIC_RELAXED), mode);
    if (inode == NULL) return NULL;
    inode->priv = kmalloc(sizeof(RamfsInfo), GFP_ZERO);
    if (inode->priv == NULL) {
        iput(inode);
        return NULL;
    }
    inode->uid = uid;
    inode->gid = gid;
    if (S_ISDIR(mode)) {
        inode->iop = &ramfs_dir_iops;
        inode->fop = &ramfs_dir_fops;
        inode->nlink = 2;
    } else {
        inode->iop = &ramfs_file_iops;
        inode->fop = &ramfs_file_fops;
        if (filemap_inode_init(inode, NULL, AS_MEMBACKED) != 0) {
            iput(inode);
            return NULL;
        }
    }
    return inode;
}

static int ramfs_lookup(Inode *dir, const char *name, Inode **out) {
    if (name[0] == '.' && name[1] == '.' && name[2] == '\0') {
        Inode *parent = info(dir)->parent;
        if (parent == NULL) return -ENOENT;   /* removed */
        *out = igrab(parent);
        return 0;
    }
    RamfsDirent *d = dirent_find(info(dir), name);
    if (d == NULL) return -ENOENT;
    *out = igrab(d->inode);
    return 0;
}

static int ramfs_create(Inode *dir, const char *name, uint32_t mode, uint32_t uid, uint32_t gid, Inode **out) {
    Inode *inode = ramfs_new_inode(dir->sb, mode, uid, gid);
    if (inode == NULL) return -ENOMEM;
    int err = dirent_add(dir, name, inode);
    if (err != 0) {
        iput(inode);
        return err;
    }
    *out = igrab(inode);
    return 0;
}

static int ramfs_mkdir(Inode *dir, const char *name, uint32_t mode, uint32_t uid, uint32_t gid) {
    Inode *inode = ramfs_new_inode(dir->sb, mode, uid, gid);
    if (inode == NULL) return -ENOMEM;
    info(inode)->parent = dir;
    int err = dirent_add(dir, name, inode);
    if (err != 0) {
        iput(inode);
        return err;
    }
    dir->nlink++;   /* the new directory's ".." */
    return 0;
}

static int ramfs_unlink(Inode *dir, const char *name, Inode *victim) {
    RamfsDirent *d = dirent_find(info(dir), name);
    if (d == NULL || d->inode != victim) return -ENOENT;
    victim->nlink--;
    iput(dirent_del(dir, d));
    return 0;
}

static int ramfs_rmdir(Inode *dir, const char *name, Inode *victim) {
    RamfsDirent *d = dirent_find(info(dir), name);
    if (d == NULL || d->inode != victim) return -ENOENT;
    if (info(victim)->count != 0) return -ENOTEMPTY;
    victim->nlink = 0;
    info(victim)->parent = NULL;   /* the VFS holds victim's lock */
    dir->nlink--;
    iput(dirent_del(dir, d));
    return 0;
}

static int ramfs_rename(Inode *odir, const char *oname, Inode *src, Inode *ndir, const char *nname, Inode *victim) {
    RamfsDirent *d = dirent_find(info(odir), oname);
    if (d == NULL || d->inode != src) return -ENOENT;
    RamfsDirent *v = NULL;
    if (victim != NULL) {
        v = dirent_find(info(ndir), nname);
        if (v == NULL || v->inode != victim) return -ENOENT;
        if (S_ISDIR(victim->mode) && info(victim)->count != 0) return -ENOTEMPTY;
    }
    /* The new entry first: if it can't be made, nothing has changed */
    int err = dirent_add(ndir, nname, igrab(src));
    if (err != 0) {
        iput(src);
        return err;
    }
    if (v != NULL) {
        if (S_ISDIR(victim->mode)) {
            victim->nlink = 0;
            info(victim)->parent = NULL;
            ndir->nlink--;
        } else {
            victim->nlink--;
        }
        iput(dirent_del(ndir, v));
    }
    iput(dirent_del(odir, d));   /* the old entry's reference; the new one holds src */
    if (S_ISDIR(src->mode) && odir != ndir) {
        odir->nlink--;
        ndir->nlink++;
        info(src)->parent = ndir;   /* the VFS holds src's lock (a moved directory) */
    }
    return 0;
}

static int ramfs_iterate(File *file, DirContext *ctx) {
    Inode *dir = file->inode;
    RamfsInfo *ri = info(dir);
    if (!dir_emit_dots(ctx, dir->ino, ri->parent != NULL ? ri->parent->ino : dir->ino)) return 0;
    uint64_t from = ctx->pos >= RAMFS_COOKIE0 ? (uint64_t)ctx->pos - RAMFS_COOKIE0 : 0;
    RbNode *n = ri->b.node;
    RamfsDirent *d = NULL;
    while (n != NULL) {   /* first entry with seq >= from */
        RamfsDirent *e = rb_entry(n, RamfsDirent, by_seq);
        if (e->seq >= from) {
            d = e;
            n = n->left;
        } else {
            n = n->right;
        }
    }
    while (d != NULL) {
        if (!dir_emit(ctx, d->name, d->len, d->inode->ino, vfs_dtype(d->inode->mode),
                      (loff_t)(d->seq + 1 + RAMFS_COOKIE0)))
            return 0;
        RbNode *next = rb_next(&d->by_seq);
        d = next != NULL ? rb_entry(next, RamfsDirent, by_seq) : NULL;
    }
    return 0;
}

static void ramfs_evict(Inode *inode) {
    filemap_inode_evict(inode);   /* a regular file's pages (nothing for others) */
    RamfsInfo *ri = info(inode);
    if (ri == NULL) return;
    /* A directory is evicted only once empty (rmdir) or never (the root) */
    kfree(ri);
    inode->priv = NULL;
}

static const InodeOps ramfs_dir_iops = {
    .lookup = ramfs_lookup,
    .create = ramfs_create,
    .mkdir = ramfs_mkdir,
    .unlink = ramfs_unlink,
    .rmdir = ramfs_rmdir,
    .rename = ramfs_rename,
    .setattr = ramfs_setattr,
};

static const InodeOps ramfs_file_iops = {
    .setattr = ramfs_setattr,
};

static const FileOps ramfs_dir_fops = {
    .iterate = ramfs_iterate,
};

static const FileOps ramfs_file_fops = {
    .read = filemap_read,
    .write = filemap_write,
    .mmap_ops = &filemap_vma_ops,
    .fsync = filemap_fsync,
};

static const SuperOps ramfs_sops = {
    .evict_inode = ramfs_evict,
};

SuperBlock *ramfs_create_sb(void) {
    RamfsSb *rs = kmalloc(sizeof(*rs), GFP_ZERO);
    if (rs == NULL) return NULL;
    vfs_sb_init(&rs->sb, "ramfs", &ramfs_sops);
    rs->next_ino = 1;
    Inode *root = ramfs_new_inode(&rs->sb, S_IFDIR | 0755, 0, 0);
    if (root == NULL) {
        kfree(rs);
        return NULL;
    }
    info(root)->parent = root;   /* never asked: the VFS stops ".." at a mount root */
    rs->sb.root = root;   /* the superblock's reference, never dropped */
    return &rs->sb;
}
