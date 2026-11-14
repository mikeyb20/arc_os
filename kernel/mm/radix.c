/* arc_os — radix tree: 64-bit index -> pointer (radix.h, M1-12)
 *
 * Owns: the node slab cache and every node.
 * Invariants: a node at level l (leaves are level 1) holds items in its slots
 * when l == 1, child nodes otherwise; `count` is its non-NULL slots; no node
 * is empty except while an insert is filling it in; every index stored is
 * below 64^height.
 * Locks: none (the caller's).
 * Cost: lookup/insert/delete one step per level (at most 6 for a page index,
 * RADIX_MAX_LEVEL for any 64-bit one); nodes from a slab cache. */

#include "mm/radix.h"
#include "mm/slab.h"
#include "uapi/errno.h"

static struct kmem_cache *node_cache;
static uint64_t nodes_live;
static uint32_t last_steps;

void radix_cache_init(void) {
    node_cache = kmem_cache_create("radix_node", sizeof(RadixNode), 64);
}

void radix_init(RadixTree *t) {
    t->root = NULL;
    t->height = 0;
}

uint64_t radix_nodes_live(void) {
    return __atomic_load_n(&nodes_live, __ATOMIC_RELAXED);
}

uint32_t radix_last_steps(void) {
    return last_steps;
}

static RadixNode *node_new(void) {
    RadixNode *n = kmem_cache_alloc(node_cache, GFP_ZERO);
    if (n != NULL) __atomic_add_fetch(&nodes_live, 1, __ATOMIC_RELAXED);
    return n;
}

static void node_free(RadixNode *n) {
    kmem_cache_free(node_cache, n);
    __atomic_sub_fetch(&nodes_live, 1, __ATOMIC_RELAXED);
}

/* Largest index a tree of `height` levels holds */
static uint64_t max_index(uint32_t height) {
    if (height == 0) return 0;
    if (height * RADIX_SHIFT >= 64) return UINT64_MAX;
    return (1ull << (height * RADIX_SHIFT)) - 1;
}

/* Slot of index in a node at level (1 = leaf) */
static unsigned slot_of(uint64_t index, uint32_t level) {
    uint32_t shift = (level - 1) * RADIX_SHIFT;
    return shift >= 64 ? 0 : (unsigned)((index >> shift) & RADIX_MASK);
}

void *radix_lookup(const RadixTree *t, uint64_t index) {
    uint32_t steps = 0;
    void *p = NULL;
    if (t->height != 0 && index <= max_index(t->height)) {
        RadixNode *n = t->root;
        for (uint32_t level = t->height; n != NULL; level--) {
            steps++;
            p = n->slots[slot_of(index, level)];
            if (level == 1) break;
            n = p;
            p = NULL;
        }
    }
    last_steps = steps;
    return p;
}

/* Add levels on top until index fits. 0 or -ENOMEM. */
static int grow(RadixTree *t, uint64_t index) {
    if (t->height == 0) {
        uint32_t h = 1;
        while (index > max_index(h)) h++;
        t->height = h;   /* root stays NULL: the insert path makes the nodes */
        return 0;
    }
    while (index > max_index(t->height)) {
        RadixNode *n = node_new();
        if (n == NULL) return -ENOMEM;
        if (t->root != NULL) {
            n->slots[0] = t->root;
            n->count = 1;
        }
        t->root = n;
        t->height++;
    }
    return 0;
}

/* Free the empty nodes on index's path, bottom up, then lower the tree */
static void prune(RadixTree *t, uint64_t index);

int radix_insert(RadixTree *t, uint64_t index, void *item) {
    if (item == NULL) return -EINVAL;
    if (grow(t, index) != 0) return -ENOMEM;
    if (t->root == NULL) {
        t->root = node_new();
        if (t->root == NULL) {
            t->height = 0;
            return -ENOMEM;
        }
    }
    RadixNode *n = t->root;
    for (uint32_t level = t->height; level > 1; level--) {
        unsigned s = slot_of(index, level);
        if (n->slots[s] == NULL) {
            RadixNode *c = node_new();
            if (c == NULL) {
                prune(t, index);   /* the nodes this call added are still empty */
                return -ENOMEM;
            }
            n->slots[s] = c;
            n->count++;
        }
        n = n->slots[s];
    }
    unsigned s = slot_of(index, 1);
    if (n->slots[s] != NULL) return -EEXIST;
    n->slots[s] = item;
    n->count++;
    return 0;
}

/* Lower the tree while the root uses only slot 0; free an empty root */
static void shrink(RadixTree *t) {
    while (t->height > 1) {
        RadixNode *r = t->root;
        if (r->count != 1 || r->slots[0] == NULL) break;
        t->root = r->slots[0];
        t->height--;
        node_free(r);
    }
    if (t->height != 0 && t->root != NULL && ((RadixNode *)t->root)->count == 0) {
        node_free(t->root);
        t->root = NULL;
        t->height = 0;
    }
}

/* Walk index's path: path[level] = node at that level, slot[level] = the
 * slot index takes in it. Returns the lowest level reached (1 = the leaf
 * node; more when a node on the way is missing). */
static uint32_t radix_path(const RadixTree *t, uint64_t index, RadixNode **path, unsigned *slot) {
    RadixNode *n = t->root;
    uint32_t level = t->height;
    for (;; level--) {
        path[level] = n;
        slot[level] = slot_of(index, level);
        if (level == 1 || n->slots[slot[level]] == NULL) return level;
        n = n->slots[slot[level]];
    }
}

/* From `level` up: free each empty non-root node and clear its parent's slot */
static void free_empty_up(RadixTree *t, RadixNode **path, unsigned *slot, uint32_t level) {
    for (; level < t->height; level++) {
        if (path[level]->count != 0) break;
        node_free(path[level]);
        path[level + 1]->slots[slot[level + 1]] = NULL;
        path[level + 1]->count--;
    }
    shrink(t);
}

static void prune(RadixTree *t, uint64_t index) {
    if (t->height == 0 || t->root == NULL) return;
    RadixNode *path[RADIX_MAX_LEVEL + 1];
    unsigned slot[RADIX_MAX_LEVEL + 1];
    free_empty_up(t, path, slot, radix_path(t, index, path, slot));
}

void *radix_delete(RadixTree *t, uint64_t index) {
    if (t->height == 0 || t->root == NULL || index > max_index(t->height)) return NULL;
    RadixNode *path[RADIX_MAX_LEVEL + 1];
    unsigned slot[RADIX_MAX_LEVEL + 1];
    if (radix_path(t, index, path, slot) != 1) return NULL;
    void *item = path[1]->slots[slot[1]];
    if (item == NULL) return NULL;
    path[1]->slots[slot[1]] = NULL;
    path[1]->count--;
    free_empty_up(t, path, slot, 1);
    return item;
}

/* First item >= start in the subtree n at level; base = first index n covers */
static void *next_in(RadixNode *n, uint32_t level, uint64_t base, uint64_t start, uint64_t *found) {
    uint32_t shift = (level - 1) * RADIX_SHIFT;
    unsigned s = start > base ? slot_of(start, level) : 0;
    for (; s < RADIX_SLOTS; s++) {
        void *p = n->slots[s];
        if (p == NULL) continue;
        uint64_t sbase = base + ((uint64_t)s << shift);
        if (level == 1) {
            *found = sbase;
            return p;
        }
        void *r = next_in(p, level - 1, sbase, start > sbase ? start : sbase, found);
        if (r != NULL) return r;
    }
    return NULL;
}

void *radix_next(const RadixTree *t, uint64_t start, uint64_t *found) {
    if (t->height == 0 || t->root == NULL || start > max_index(t->height)) return NULL;
    return next_in(t->root, t->height, 0, start, found);
}
