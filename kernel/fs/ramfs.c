/* arc_os — ramfs: files and directories in memory (ramfs.h, M1-11)
 *
 * Owns: one in-memory filesystem instance per ramfs_create (the root
 * filesystem today): directory entries and file data.
 *
 * Directories: each entry sits in two red-black trees of its directory, by
 * name (lookup) and by a sequence number given at insertion (iteration). A
 * getdents cookie is "the entry with sequence >= cookie - 3", so iteration
 * resumes in O(log n) and is not confused by entries removed in between.
 * Files: sparse; 4 KiB pages in a red-black tree by page index (holes read as
 * zeros), so a file may end far beyond 4 GiB while using only the pages
 * written. M1-12 moves file data into the page cache (inode->mapping).
 *
 * Invariants: a directory entry holds one inode reference, so a linked inode
 * lives at least as long as its entry; an unlinked one lives until its last
 * File or lookup reference goes (then evict frees its pages). nlink counts
 * entries naming the inode, plus "." and each subdirectory's ".." for
 * directories. Data beyond `size` inside the last page is always zero.
 *
 * Locks: the VFS holds the inode lock around every call (vfs.h); ramfs adds
 * none. inode_seq is atomic.
 *
 * Cost: lookup O(log n) name compares; create/unlink/rename O(log n);
 * read/write O(log pages) to find the first page, then O(1) per page. */

#include "fs/ramfs.h"
#include "mm/kmalloc.h"
#include "mm/pmm.h"
#include "mm/page.h"
#include "lib/mem.h"
#include "lib/string.h"
#include "lib/rbtree.h"

#define RAMFS_PAGE    4096u
#define RAMFS_COOKIE0 3   /* cookie of sequence 0: after ".", "..", and 2 = "end of dots" */

typedef struct RamfsDirent {
    RbNode by_name;
    RbNode by_seq;
    uint64_t seq;
    Inode *inode;   /* a reference */
    uint16_t len;
    char name[];
} RamfsDirent;

typedef struct RamfsPage {
    RbNode node;
    uint64_t index;
    struct page *page;
} RamfsPage;

typedef struct RamfsInfo {
    RbRoot a;             /* directory: entries by name; file: pages by index */
    RbRoot b;             /* directory: entries by sequence */
    uint64_t next_seq;    /* directory */
    uint32_t count;       /* directory: entries */
    uint64_t parent_ino;  /* directory: ".." */
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

/* --- Pages --- */

static RamfsPage *page_find_ge(const RamfsInfo *ri, uint64_t index) {
    RbNode *n = ri->a.node;
    RamfsPage *best = NULL;
    while (n != NULL) {
        RamfsPage *p = rb_entry(n, RamfsPage, node);
        if (p->index >= index) {
            best = p;
            n = n->left;
        } else {
            n = n->right;
        }
    }
    return best;
}

static RamfsPage *page_get(RamfsInfo *ri, uint64_t index, Inode *inode) {
    RbNode **link = &ri->a.node, *parent = NULL;
    while (*link != NULL) {
        RamfsPage *p = rb_entry(*link, RamfsPage, node);
        if (p->index == index) return p;
        parent = *link;
        link = index < p->index ? &(*link)->left : &(*link)->right;
    }
    RamfsPage *p = kmalloc(sizeof(*p), 0);
    if (p == NULL) return NULL;
    p->page = alloc_pages(0, GFP_KERNEL | GFP_ZERO);
    if (p->page == NULL) {
        kfree(p);
        return NULL;
    }
    p->index = index;
    rb_link_node(&p->node, parent, link);
    rb_insert_color(&p->node, &ri->a);
    inode->blocks += RAMFS_PAGE / 512;
    return p;
}

static void page_drop(RamfsInfo *ri, RamfsPage *p, Inode *inode) {
    rb_erase(&p->node, &ri->a);
    free_pages(p->page, 0);
    kfree(p);
    inode->blocks -= RAMFS_PAGE / 512;
}

static int64_t ramfs_read(File *file, const IoBuf *buf, loff_t *pos) {
    Inode *inode = file->inode;
    RamfsInfo *ri = info(inode);
    if (*pos >= inode->size) return 0;
    uint64_t start = (uint64_t)*pos;
    uint64_t avail = (uint64_t)inode->size - start;
    size_t total = buf->len < avail ? buf->len : (size_t)avail;
    size_t done = 0;
    RamfsPage *p = page_find_ge(ri, start / RAMFS_PAGE);
    while (done < total) {
        uint64_t at = start + done;
        uint64_t index = at / RAMFS_PAGE;
        size_t in_page = (size_t)(at % RAMFS_PAGE);
        size_t n = RAMFS_PAGE - in_page;
        if (n > total - done) n = total - done;
        while (p != NULL && p->index < index) {
            RbNode *next = rb_next(&p->node);
            p = next != NULL ? rb_entry(next, RamfsPage, node) : NULL;
        }
        int err = (p != NULL && p->index == index)
                      ? iob_copy_out(buf, done, (uint8_t *)page_to_virt(p->page) + in_page, n)
                      : iob_zero(buf, done, n);   /* a hole */
        if (err != 0) {
            if (done == 0) return err;
            break;
        }
        done += n;
    }
    *pos += (loff_t)done;
    return (int64_t)done;
}

static int64_t ramfs_write(File *file, const IoBuf *buf, loff_t *pos) {
    Inode *inode = file->inode;
    RamfsInfo *ri = info(inode);
    uint64_t start = (uint64_t)*pos;
    size_t done = 0;
    while (done < buf->len) {
        uint64_t at = start + done;
        size_t in_page = (size_t)(at % RAMFS_PAGE);
        size_t n = RAMFS_PAGE - in_page;
        if (n > buf->len - done) n = buf->len - done;
        RamfsPage *p = page_get(ri, at / RAMFS_PAGE, inode);
        int err = p == NULL ? -ENOMEM : iob_copy_in((uint8_t *)page_to_virt(p->page) + in_page, buf, done, n);
        if (err != 0) {
            if (done == 0) return err;
            break;
        }
        done += n;
        if ((loff_t)(start + done) > inode->size) inode->size = (loff_t)(start + done);
    }
    *pos += (loff_t)done;
    return (int64_t)done;
}

/* Cut the file to size: free whole pages past it, zero the rest of the
 * last page (so growing again reads zeros there). */
static void ramfs_truncate(Inode *inode, loff_t size) {
    RamfsInfo *ri = info(inode);
    if (size < inode->size) {
        uint64_t keep = ((uint64_t)size + RAMFS_PAGE - 1) / RAMFS_PAGE;   /* pages [0, keep) stay */
        RamfsPage *p = page_find_ge(ri, keep);
        while (p != NULL) {
            RbNode *next = rb_next(&p->node);
            page_drop(ri, p, inode);
            p = next != NULL ? rb_entry(next, RamfsPage, node) : NULL;
        }
        size_t tail = (size_t)((uint64_t)size % RAMFS_PAGE);
        if (tail != 0) {
            p = page_find_ge(ri, (uint64_t)size / RAMFS_PAGE);
            if (p != NULL && p->index == (uint64_t)size / RAMFS_PAGE) {
                memset((uint8_t *)page_to_virt(p->page) + tail, 0, RAMFS_PAGE - tail);
            }
        }
    }
    inode->size = size;
    vfs_now(&inode->mtime);
}

static int ramfs_setattr(Inode *inode, const InodeAttr *attr) {
    if (attr->valid & ATTR_SIZE) ramfs_truncate(inode, attr->size);
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
    }
    return inode;
}

static int ramfs_lookup(Inode *dir, const char *name, Inode **out) {
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
    info(inode)->parent_ino = dir->ino;
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
        info(src)->parent_ino = ndir->ino;
    }
    return 0;
}

static int ramfs_iterate(File *file, DirContext *ctx) {
    Inode *dir = file->inode;
    RamfsInfo *ri = info(dir);
    if (!dir_emit_dots(ctx, dir->ino, ri->parent_ino != 0 ? ri->parent_ino : dir->ino)) return 0;
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
    RamfsInfo *ri = info(inode);
    if (ri == NULL) return;
    if (S_ISREG(inode->mode)) {
        RamfsPage *p = page_find_ge(ri, 0);
        while (p != NULL) {
            RbNode *next = rb_next(&p->node);
            page_drop(ri, p, inode);
            p = next != NULL ? rb_entry(next, RamfsPage, node) : NULL;
        }
    }
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
    .read = ramfs_read,
    .write = ramfs_write,
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
    rs->sb.root = root;   /* the superblock's reference, never dropped */
    return &rs->sb;
}
