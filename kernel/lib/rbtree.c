/* arc_os — intrusive red-black tree (rbtree.h, M1-07)
 *
 * Owns: nothing; it rebalances trees whose nodes live inside other objects.
 * Invariants (checked by tests/test_rbtree.c): the root is black; a red node
 * has no red child; every path from a node down to a NULL child crosses the
 * same number of black nodes. So the tree is at most 2 log2(n+1) deep.
 * Locks: none; the tree's owner serializes all calls.
 * Cost: insert and erase O(log n) with at most 2 (insert) or 3 (erase)
 * rotations; rb_next O(log n) worst case, O(1) amortized over a walk. */

#include "lib/rbtree.h"

static void rb_set_child(RbRoot *root, RbNode *parent, RbNode *old, RbNode *new_node) {
    if (parent == NULL)
        root->node = new_node;
    else if (parent->left == old)
        parent->left = new_node;
    else
        parent->right = new_node;
}

/* x's right child y takes x's place; x becomes y's left child. */
static void rb_rotate_left(RbRoot *root, RbNode *x) {
    RbNode *y = x->right;
    x->right = y->left;
    if (y->left != NULL) y->left->parent = x;
    y->parent = x->parent;
    rb_set_child(root, x->parent, x, y);
    y->left = x;
    x->parent = y;
}

/* Mirror of rb_rotate_left. */
static void rb_rotate_right(RbRoot *root, RbNode *x) {
    RbNode *y = x->left;
    x->left = y->right;
    if (y->right != NULL) y->right->parent = x;
    y->parent = x->parent;
    rb_set_child(root, x->parent, x, y);
    y->right = x;
    x->parent = y;
}

void rb_insert_color(RbNode *node, RbRoot *root) {
    while (node->parent != NULL && node->parent->red) {
        RbNode *parent = node->parent;
        RbNode *grand = parent->parent;   /* exists: a red node is never the root */
        if (parent == grand->left) {
            RbNode *uncle = grand->right;
            if (uncle != NULL && uncle->red) {   /* recolour and move up */
                parent->red = 0;
                uncle->red = 0;
                grand->red = 1;
                node = grand;
                continue;
            }
            if (node == parent->right) {   /* make it the outer case */
                rb_rotate_left(root, parent);
                node = parent;
                parent = node->parent;
            }
            parent->red = 0;
            grand->red = 1;
            rb_rotate_right(root, grand);
        } else {
            RbNode *uncle = grand->left;
            if (uncle != NULL && uncle->red) {
                parent->red = 0;
                uncle->red = 0;
                grand->red = 1;
                node = grand;
                continue;
            }
            if (node == parent->left) {
                rb_rotate_right(root, parent);
                node = parent;
                parent = node->parent;
            }
            parent->red = 0;
            grand->red = 1;
            rb_rotate_left(root, grand);
        }
    }
    root->node->red = 0;
}

/* Restore the black height after removing a black node: x (maybe NULL) now
 * sits where it was, under parent, one black short. */
static void rb_erase_fixup(RbRoot *root, RbNode *x, RbNode *parent) {
    while (x != root->node && (x == NULL || !x->red)) {
        if (x == parent->left) {
            RbNode *w = parent->right;   /* non-NULL: parent's right side is a black taller */
            if (w->red) {
                w->red = 0;
                parent->red = 1;
                rb_rotate_left(root, parent);
                w = parent->right;
            }
            if ((w->left == NULL || !w->left->red) && (w->right == NULL || !w->right->red)) {
                w->red = 1;
                x = parent;
                parent = x->parent;
                continue;
            }
            if (w->right == NULL || !w->right->red) {
                w->left->red = 0;
                w->red = 1;
                rb_rotate_right(root, w);
                w = parent->right;
            }
            w->red = parent->red;
            parent->red = 0;
            w->right->red = 0;
            rb_rotate_left(root, parent);
            x = root->node;
            break;
        } else {
            RbNode *w = parent->left;
            if (w->red) {
                w->red = 0;
                parent->red = 1;
                rb_rotate_right(root, parent);
                w = parent->left;
            }
            if ((w->left == NULL || !w->left->red) && (w->right == NULL || !w->right->red)) {
                w->red = 1;
                x = parent;
                parent = x->parent;
                continue;
            }
            if (w->left == NULL || !w->left->red) {
                w->right->red = 0;
                w->red = 1;
                rb_rotate_left(root, w);
                w = parent->left;
            }
            w->red = parent->red;
            parent->red = 0;
            w->left->red = 0;
            rb_rotate_right(root, parent);
            x = root->node;
            break;
        }
    }
    if (x != NULL) x->red = 0;
}

void rb_erase(RbNode *node, RbRoot *root) {
    RbNode *child;
    RbNode *parent;
    uint8_t removed_red;

    if (node->left != NULL && node->right != NULL) {
        /* Two children: the in-order successor (leftmost of the right
         * subtree, no left child) takes node's place and colour */
        RbNode *succ = node->right;
        while (succ->left != NULL) succ = succ->left;
        removed_red = succ->red;
        child = succ->right;
        if (succ->parent == node) {
            parent = succ;
        } else {
            parent = succ->parent;
            parent->left = child;
            if (child != NULL) child->parent = parent;
            succ->right = node->right;
            node->right->parent = succ;
        }
        succ->left = node->left;
        node->left->parent = succ;
        succ->parent = node->parent;
        succ->red = node->red;
        rb_set_child(root, node->parent, node, succ);
    } else {
        child = node->left != NULL ? node->left : node->right;
        parent = node->parent;
        removed_red = node->red;
        if (child != NULL) child->parent = parent;
        rb_set_child(root, parent, node, child);
    }

    if (!removed_red) {
        if (root->node == NULL) return;
        rb_erase_fixup(root, child, parent);
    }
}

RbNode *rb_first(const RbRoot *root) {
    RbNode *n = root->node;
    if (n == NULL) return NULL;
    while (n->left != NULL) n = n->left;
    return n;
}

RbNode *rb_next(const RbNode *node) {
    if (node->right != NULL) {
        RbNode *n = node->right;
        while (n->left != NULL) n = n->left;
        return n;
    }
    while (node->parent != NULL && node == node->parent->right) node = node->parent;
    return node->parent;
}
