#ifndef ARCHOS_LIB_RBTREE_H
#define ARCHOS_LIB_RBTREE_H

/* Intrusive red-black tree (M1-07): the node lives inside the object, the
 * caller does the key comparisons, the tree only keeps itself balanced.
 *
 * Insert: walk from root->node to the empty child slot where the new node
 * belongs (keeping its parent), then rb_link_node + rb_insert_color.
 * Lookup is the same walk; the tree stays O(log n) deep (at most 2 log2(n+1)).
 * No allocation, no locking: the owner's lock protects the tree. */

#include <stddef.h>
#include <stdint.h>

typedef struct RbNode {
    struct RbNode *parent;
    struct RbNode *left;
    struct RbNode *right;
    uint8_t red;   /* 1 red, 0 black */
} RbNode;

typedef struct {
    RbNode *node;   /* root, or NULL for an empty tree */
} RbRoot;

#define RB_ROOT_INIT \
    { NULL }

/* The object that holds `ptr`, an RbNode member called `member`. */
#define rb_entry(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))

/* Put node in the empty slot *link (a child pointer of parent, or root->node
 * when parent is NULL). Follow with rb_insert_color. */
static inline void rb_link_node(RbNode *node, RbNode *parent, RbNode **link) {
    node->parent = parent;
    node->left = node->right = NULL;
    node->red = 1;
    *link = node;
}

/* Rebalance after rb_link_node. O(log n), at most two rotations. */
void rb_insert_color(RbNode *node, RbRoot *root);

/* Remove node from the tree. O(log n), at most three rotations. */
void rb_erase(RbNode *node, RbRoot *root);

/* Smallest / next node in key order, or NULL. O(log n) each. */
RbNode *rb_first(const RbRoot *root);
RbNode *rb_next(const RbNode *node);

#endif /* ARCHOS_LIB_RBTREE_H */
