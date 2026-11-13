/* arc_os — inodes and the inode cache (vfs.h, M1-11)
 *
 * Owns: inode allocation and lifetime (igrab/iput), the inode cache keyed by
 * (superblock, inode number), the anonymous superblock of pipes and sockets,
 * permission checks and stat filling.
 *
 * Invariants: an inode lives while its refcount is above 0; a filesystem that
 * keeps an inode for its own reasons (a ramfs directory entry, a mount, the
 * dentry cache) holds a reference like anyone else. A hashed inode's count
 * goes from 1 to 0 only under icache_lock, in the same critical section that
 * unhashes it, so icache_get never revives an inode being freed. I_STATIC
 * inodes are counted but never freed.
 *
 * Locks: icache_lock (LOCK_LEVEL_ICACHE, spinlock) guards the hash chains and
 * I_HASHED; it is a leaf. evict_inode runs with no lock held.
 *
 * Cost: igrab is one atomic add; iput of an unhashed inode one atomic sub;
 * of a hashed one, the cache lock and the chain walk to unlink it. Lookups
 * hash to one of ICACHE_BUCKETS chains. */

#include "fs/vfs.h"
#include "mm/kmalloc.h"
#include "mm/slab.h"
#include "lib/mem.h"
#include "arch/hal.h"
#include "proc/process.h"
#include "fs/vfs_internal.h"
#include "uapi/dirent.h"

#define ICACHE_BUCKETS 512   /* power of two */

/* An inode is created and freed for every file a lookup brings in; keep it in
 * four cache lines (ADR-0019). */
_Static_assert(sizeof(Inode) <= 256, "Inode must stay within 256 bytes");

static struct kmem_cache *inode_cache;
static Inode *icache[ICACHE_BUCKETS];
static Spinlock icache_lock = SPINLOCK_INIT(LOCK_LEVEL_ICACHE, "inode cache");
static uint32_t next_dev = 1;   /* st_dev numbers; 0 is never used */
static SuperBlock anon_sb;

static uint32_t icache_hash(const SuperBlock *sb, uint64_t ino) {
    uint64_t h = ((uint64_t)(uintptr_t)sb >> 4) ^ ino ^ (ino >> 29);
    h *= 0x9E3779B97F4A7C15ull;
    return (uint32_t)(h >> 32) & (ICACHE_BUCKETS - 1);
}

void vfs_sb_init(SuperBlock *sb, const char *type, const SuperOps *sop) {
    sb->type = type;
    sb->sop = sop;
    sb->dev = __atomic_fetch_add(&next_dev, 1, __ATOMIC_RELAXED);
    mutex_init(&sb->rename_lock, LOCK_LEVEL_RENAME);
}

SuperBlock *vfs_anon_sb(void) {
    return &anon_sb;
}

void vfs_now(struct timespec *ts) {
    uint64_t ms = hal_timer_uptime_ms();
    ts->tv_sec = (int64_t)(ms / 1000);
    ts->tv_nsec = (int64_t)(ms % 1000) * 1000000;
}

static void inode_setup(Inode *inode, SuperBlock *sb, uint64_t ino, uint32_t mode) {
    inode->ino = ino;
    inode->mode = mode;
    inode->nlink = 1;
    inode->sb = sb;
    inode->refcount = 1;
    vfs_now(&inode->mtime);
    inode->atime = inode->mtime;
    inode->ctime = inode->mtime;
    rwsem_init(&inode->lock, LOCK_LEVEL_INODE);
}

Inode *inode_new(SuperBlock *sb, uint64_t ino, uint32_t mode) {
    Inode *inode = kmem_cache_alloc(inode_cache, GFP_ZERO);
    if (inode == NULL) return NULL;
    inode_setup(inode, sb, ino, mode);
    return inode;
}

void inode_init_static(Inode *inode, SuperBlock *sb, uint64_t ino, uint32_t mode) {
    memset(inode, 0, sizeof(*inode));
    inode_setup(inode, sb, ino, mode);
    inode->flags = I_STATIC;
}

Inode *igrab(Inode *inode) {
    __atomic_add_fetch(&inode->refcount, 1, __ATOMIC_RELAXED);
    return inode;
}

/* Unlink inode from its chain. icache_lock held. */
static void icache_unhash(Inode *inode) {
    Inode **link = &icache[icache_hash(inode->sb, inode->ino)];
    while (*link != NULL && *link != inode) link = &(*link)->hash_next;
    if (*link == inode) *link = inode->hash_next;
    inode->hash_next = NULL;
    __atomic_and_fetch(&inode->flags, ~(uint32_t)I_HASHED, __ATOMIC_RELAXED);
}

void iput(Inode *inode) {
    if (inode == NULL) return;
    if (__atomic_load_n(&inode->flags, __ATOMIC_RELAXED) & I_HASHED) {
        /* The last reference must leave the cache in the same step, or
         * icache_get could hand out an inode that is being freed */
        uint64_t flags;
        spin_lock_irqsave(&icache_lock, &flags);
        if (__atomic_sub_fetch(&inode->refcount, 1, __ATOMIC_ACQ_REL) != 0) {
            spin_unlock_irqrestore(&icache_lock, flags);
            return;
        }
        icache_unhash(inode);
        spin_unlock_irqrestore(&icache_lock, flags);
    } else if (__atomic_sub_fetch(&inode->refcount, 1, __ATOMIC_ACQ_REL) != 0) {
        return;
    }
    if (inode->flags & I_STATIC) return;
    if (inode->sb != NULL && inode->sb->sop != NULL && inode->sb->sop->evict_inode != NULL) {
        inode->sb->sop->evict_inode(inode);
    }
    kmem_cache_free(inode_cache, inode);
}

Inode *icache_get(SuperBlock *sb, uint64_t ino) {
    uint64_t flags;
    spin_lock_irqsave(&icache_lock, &flags);
    Inode *inode = icache[icache_hash(sb, ino)];
    while (inode != NULL && !(inode->sb == sb && inode->ino == ino)) inode = inode->hash_next;
    if (inode != NULL) igrab(inode);   /* hashed: its count can't reach 0 while we hold the lock */
    spin_unlock_irqrestore(&icache_lock, flags);
    return inode;
}

Inode *icache_add(Inode *inode) {
    uint32_t b = icache_hash(inode->sb, inode->ino);
    uint64_t flags;
    spin_lock_irqsave(&icache_lock, &flags);
    Inode *old = icache[b];
    while (old != NULL && !(old->sb == inode->sb && old->ino == inode->ino)) old = old->hash_next;
    if (old != NULL) {
        igrab(old);
        spin_unlock_irqrestore(&icache_lock, flags);
        iput(inode);   /* not hashed: freed outside the lock */
        return old;
    }
    __atomic_or_fetch(&inode->flags, I_HASHED, __ATOMIC_RELAXED);
    inode->hash_next = icache[b];
    icache[b] = inode;
    spin_unlock_irqrestore(&icache_lock, flags);
    return inode;
}

int vfs_check_perm(const Inode *inode, uint32_t uid, uint32_t gid, int want) {
    if (uid == 0) {
        /* Root may do anything, except run a file nobody may run */
        if (!(want & X_OK) || S_ISDIR(inode->mode) || (inode->mode & 0111)) return 0;
        return -EACCES;
    }
    uint32_t perm;
    if (uid == inode->uid) {
        perm = (inode->mode >> 6) & 7;
    } else if (gid == inode->gid) {
        perm = (inode->mode >> 3) & 7;
    } else {
        perm = inode->mode & 7;
    }
    return ((perm & (uint32_t)want) == (uint32_t)want) ? 0 : -EACCES;
}

int inode_permission(const Inode *inode, int want) {
    Process *p = proc_current();
    if (p == NULL) return 0;   /* the kernel itself (boot setup) */
    return vfs_check_perm(inode, p->euid, p->egid, want);
}

void vfs_fill_stat(const Inode *inode, struct stat *st) {
    memset(st, 0, sizeof(*st));
    st->st_dev = inode->sb != NULL ? inode->sb->dev : 0;
    st->st_ino = inode->ino;
    st->st_nlink = inode->nlink;
    st->st_mode = inode->mode;
    st->st_uid = inode->uid;
    st->st_gid = inode->gid;
    st->st_rdev = inode->rdev;
    st->st_size = inode->size;
    st->st_blksize = 4096;
    st->st_blocks = (int64_t)inode->blocks;
    st->st_atime = (uint64_t)inode->atime.tv_sec;
    st->st_atime_nsec = (uint64_t)inode->atime.tv_nsec;
    st->st_mtime = (uint64_t)inode->mtime.tv_sec;
    st->st_mtime_nsec = (uint64_t)inode->mtime.tv_nsec;
    st->st_ctime = (uint64_t)inode->ctime.tv_sec;
    st->st_ctime_nsec = (uint64_t)inode->ctime.tv_nsec;
}

int dir_emit_dots(DirContext *ctx, uint64_t self_ino, uint64_t parent_ino) {
    if (ctx->pos == 0 && !dir_emit(ctx, ".", 1, self_ino, DT_DIR, 1)) return 0;
    if (ctx->pos == 1 && !dir_emit(ctx, "..", 2, parent_ino, DT_DIR, 2)) return 0;
    return 1;
}

void vfs_init(void) {
    inode_cache = kmem_cache_create("inode", sizeof(Inode), 64);
    vfs_file_init();
    dcache_init();
    vfs_sb_init(&anon_sb, "anon", NULL);
}
