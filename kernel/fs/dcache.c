/* arc_os — dentry cache (vfs_internal.h, M1-11)
 *
 * Owns: a bounded map (parent inode, name) -> child inode used by path
 * lookup, so a component costs one hash probe instead of a filesystem lookup
 * (a FAT32 directory read from disk).
 *
 * Invariants: an entry holds a reference to its parent and its child, so
 * neither inode can be freed (and its address reused as a key) while the
 * entry exists. Entries are only positive (no "name doesn't exist" entries).
 * Changes to a directory remove the affected names first (dcache_remove under
 * the directory's exclusive lock); additions come from lookups under the
 * directory's shared lock, so an entry is never added for a name that is
 * being removed. At most DCACHE_MAX entries: adding past that frees the least
 * recently used one, which bounds the inodes a disk filesystem keeps in memory.
 * Names of DNAME_INLINE bytes or more are not cached (rare; the filesystem
 * looks them up), so an entry is one slab object and nothing else.
 *
 * Locks: dcache_lock (LOCK_LEVEL_DCACHE, spinlock) guards the hash chains, the
 * LRU list and the count. It is a leaf apart from the slab allocator; inode
 * references of evicted entries are dropped after it is released.
 *
 * Cost: lookup, add and remove are one hash probe (chains average below one
 * entry at DCACHE_MAX over DCACHE_BUCKETS) plus a name compare; eviction
 * takes the LRU tail in O(1). */

#include "fs/vfs_internal.h"
#include "mm/slab.h"
#include "lib/mem.h"
#include "lib/string.h"

#define DCACHE_BUCKETS 4096   /* power of two */
#define DNAME_INLINE   32     /* names up to 31 bytes are cached, in the entry */

typedef struct Dentry {
    struct Dentry *hash_next;
    struct Dentry *lru_prev;   /* toward most recently used */
    struct Dentry *lru_next;   /* toward least recently used */
    Inode *parent;
    Inode *child;
    uint32_t hash;
    char name[DNAME_INLINE];
} Dentry;

_Static_assert(sizeof(Dentry) <= 96, "Dentry must stay within 96 bytes");

static struct kmem_cache *dentry_cache;
static Dentry *buckets[DCACHE_BUCKETS];
static Dentry *lru_head;   /* most recently used */
static Dentry *lru_tail;   /* least recently used */
static uint32_t nr_dentries;
static Spinlock dcache_lock = SPINLOCK_INIT(LOCK_LEVEL_DCACHE, "dentry cache");

void dcache_init(void) {
    dentry_cache = kmem_cache_create("dentry", sizeof(Dentry), 8);
}

/* FNV-1a over the name, mixed with the parent's address */
static uint32_t dentry_hash(const Inode *parent, const char *name) {
    uint32_t h = 2166136261u ^ (uint32_t)((uintptr_t)parent >> 6);
    for (const char *c = name; *c != '\0'; c++) {
        h ^= (uint8_t)*c;
        h *= 16777619u;
    }
    return h;
}

static void lru_unlink(Dentry *d) {
    if (d->lru_prev != NULL)
        d->lru_prev->lru_next = d->lru_next;
    else
        lru_head = d->lru_next;
    if (d->lru_next != NULL)
        d->lru_next->lru_prev = d->lru_prev;
    else
        lru_tail = d->lru_prev;
    d->lru_prev = d->lru_next = NULL;
}

static void lru_push_head(Dentry *d) {
    d->lru_prev = NULL;
    d->lru_next = lru_head;
    if (lru_head != NULL) lru_head->lru_prev = d;
    lru_head = d;
    if (lru_tail == NULL) lru_tail = d;
}

/* The link that points at the entry for (parent, name, hash), or at the
 * chain's NULL end. dcache_lock held. */
static Dentry **chain_find(const Inode *parent, const char *name, uint32_t hash) {
    Dentry **link = &buckets[hash & (DCACHE_BUCKETS - 1)];
    while (*link != NULL) {
        Dentry *d = *link;
        if (d->hash == hash && d->parent == parent && strcmp(d->name, name) == 0) break;
        link = &d->hash_next;
    }
    return link;
}

/* Take d out of the cache (chain link known). dcache_lock held. */
static void dentry_unlink(Dentry **link, Dentry *d) {
    *link = d->hash_next;
    lru_unlink(d);
    nr_dentries--;
}

/* Free an unlinked entry and drop its references. No lock held. */
static void dentry_free(Dentry *d) {
    iput(d->child);
    iput(d->parent);
    kmem_cache_free(dentry_cache, d);
}

Inode *dcache_lookup(Inode *dir, const char *name) {
    uint32_t hash = dentry_hash(dir, name);
    uint64_t flags;
    spin_lock_irqsave(&dcache_lock, &flags);
    Dentry *d = *chain_find(dir, name, hash);
    Inode *child = NULL;
    if (d != NULL) {
        child = igrab(d->child);   /* the entry's own reference keeps it above 0 */
        lru_unlink(d);
        lru_push_head(d);
    }
    spin_unlock_irqrestore(&dcache_lock, flags);
    return child;
}

void dcache_add(Inode *dir, const char *name, Inode *child) {
    size_t len = strlen(name);
    if (len >= DNAME_INLINE) return;
    Dentry *d = kmem_cache_alloc(dentry_cache, 0);
    if (d == NULL) return;
    memcpy(d->name, name, len + 1);
    d->hash = dentry_hash(dir, name);
    d->parent = igrab(dir);
    d->child = igrab(child);

    Dentry *victim = NULL;
    uint64_t flags;
    spin_lock_irqsave(&dcache_lock, &flags);
    Dentry **link = chain_find(dir, name, d->hash);
    if (*link != NULL) {   /* another lookup added it first */
        spin_unlock_irqrestore(&dcache_lock, flags);
        dentry_free(d);
        return;
    }
    d->hash_next = NULL;
    *link = d;
    lru_push_head(d);
    nr_dentries++;
    if (nr_dentries > DCACHE_MAX) {
        victim = lru_tail;
        dentry_unlink(chain_find(victim->parent, victim->name, victim->hash), victim);
    }
    spin_unlock_irqrestore(&dcache_lock, flags);
    if (victim != NULL) dentry_free(victim);
}

void dcache_remove(Inode *dir, const char *name) {
    uint32_t hash = dentry_hash(dir, name);
    uint64_t flags;
    spin_lock_irqsave(&dcache_lock, &flags);
    Dentry **link = chain_find(dir, name, hash);
    Dentry *d = *link;
    if (d != NULL) dentry_unlink(link, d);
    spin_unlock_irqrestore(&dcache_lock, flags);
    if (d != NULL) dentry_free(d);
}

uint32_t dcache_count(void) {
    return __atomic_load_n(&nr_dentries, __ATOMIC_RELAXED);
}

void vfs_drop_caches(void) {
    /* At most the entries there now: lookups adding new ones meanwhile can't
     * keep it going */
    for (uint32_t n = dcache_count(); n > 0; n--) {
        uint64_t flags;
        spin_lock_irqsave(&dcache_lock, &flags);
        Dentry *d = lru_tail;
        if (d != NULL) dentry_unlink(chain_find(d->parent, d->name, d->hash), d);
        spin_unlock_irqrestore(&dcache_lock, flags);
        if (d == NULL) return;
        dentry_free(d);   /* may free a disk inode only this entry held */
    }
}
