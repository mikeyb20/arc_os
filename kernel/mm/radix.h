#ifndef ARCHOS_MM_RADIX_H
#define ARCHOS_MM_RADIX_H

/* Radix tree from a 64-bit index to a pointer (M1-12, the page cache's
 * index: page index -> struct page).
 *
 * Each node has 64 slots (6 bits of the index per level). The tree is only as
 * tall as its largest index needs: 1 level covers indices below 64, 4 levels
 * below 2^24 (64 GiB of 4 KiB pages), 6 levels the 2^32 pages a struct page
 * index can name. A lookup is one step per level; nodes come from their own
 * slab cache and are freed as soon as they empty, and the tree shrinks again
 * when only its first slot is in use.
 *
 * No locking here: the owner (filemap.c) holds its index lock around every
 * call. Nothing sleeps; an insert may allocate from the slab. */

#include <stdint.h>

#define RADIX_SHIFT     6
#define RADIX_SLOTS     (1u << RADIX_SHIFT)
#define RADIX_MASK      (RADIX_SLOTS - 1)
#define RADIX_MAX_LEVEL 11   /* ceil(64 / 6): any 64-bit index */

typedef struct RadixNode {
    void *slots[RADIX_SLOTS];
    uint32_t count;   /* non-NULL slots */
} RadixNode;

typedef struct RadixTree {
    void *root;        /* RadixNode * when height > 0; NULL when empty */
    uint32_t height;   /* levels; 0 = empty */
} RadixTree;

/* Boot: the node slab cache. */
void radix_cache_init(void);

/* An empty tree. */
void radix_init(RadixTree *t);

/* The item at index, or NULL. O(height). */
void *radix_lookup(const RadixTree *t, uint64_t index);

/* Store item (not NULL) at index. 0, -EEXIST (the slot is taken; nothing
 * changes) or -ENOMEM (no node; the tree is as before apart from levels added
 * on top, which are harmless). O(height). */
int radix_insert(RadixTree *t, uint64_t index, void *item);

/* Remove the item at index and return it (NULL if there was none). Frees the
 * nodes this empties and lowers the tree while only slot 0 of the root is
 * used. O(height). */
void *radix_delete(RadixTree *t, uint64_t index);

/* The first item at an index >= start, with its index in *found, or NULL.
 * O(height) per subtree skipped plus one step per empty slot passed in the
 * nodes visited. */
void *radix_next(const RadixTree *t, uint64_t start, uint64_t *found);

/* Nodes allocated now, over all trees (leak checks in host tests). */
uint64_t radix_nodes_live(void);

/* Levels walked by the last radix_lookup (host tests check the cost line). */
uint32_t radix_last_steps(void);

#endif /* ARCHOS_MM_RADIX_H */
