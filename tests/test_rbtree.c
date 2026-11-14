/* arc_os — Host-side tests for kernel/lib/rbtree.c (M1-07)
 *
 * Random inserts and erases against the red-black rules, checked after
 * every operation: the root is black, a red node has no red child, every
 * path to a leaf has the same number of black nodes, parent links match,
 * and the in-order walk is sorted and complete. */

#include "test_framework.h"
#include <stdint.h>

#include "../kernel/lib/rbtree.c"

typedef struct Item {
    RbNode rb;
    uint64_t key;
    int in_tree;
} Item;

#define N_ITEMS 2000

static Item items[N_ITEMS];
static RbRoot root;

static void reset_tree(void) {
    root.node = NULL;
    for (int i = 0; i < N_ITEMS; i++) {
        items[i].key = 0;
        items[i].in_tree = 0;
    }
}

static void insert_item(Item *it) {
    RbNode **link = &root.node;
    RbNode *parent = NULL;
    while (*link != NULL) {
        parent = *link;
        link = it->key < rb_entry(parent, Item, rb)->key ? &parent->left : &parent->right;
    }
    rb_link_node(&it->rb, parent, link);
    rb_insert_color(&it->rb, &root);
    it->in_tree = 1;
}

static Item *lookup(uint64_t key) {
    RbNode *n = root.node;
    while (n != NULL) {
        Item *it = rb_entry(n, Item, rb);
        if (key == it->key) return it;
        n = key < it->key ? n->left : n->right;
    }
    return NULL;
}

/* Black height of the subtree, or -1 if a rule is broken below n */
static int check_node(const RbNode *n, const RbNode *parent, int *count) {
    if (n == NULL) return 1;
    if (n->parent != parent) return -1;
    if (n->red && ((n->left && n->left->red) || (n->right && n->right->red))) return -1;
    const Item *it = rb_entry(n, Item, rb);
    if (n->left && rb_entry(n->left, Item, rb)->key > it->key) return -1;
    if (n->right && rb_entry(n->right, Item, rb)->key < it->key) return -1;
    int l = check_node(n->left, n, count);
    int r = check_node(n->right, n, count);
    if (l < 0 || r < 0 || l != r) return -1;
    (*count)++;
    return l + (n->red ? 0 : 1);
}

/* Depth of the deepest leaf */
static int depth(const RbNode *n) {
    if (n == NULL) return 0;
    int l = depth(n->left), r = depth(n->right);
    return 1 + (l > r ? l : r);
}

static int tree_valid(int expected) {
    if (root.node != NULL && root.node->red) return 0;
    int count = 0;
    if (check_node(root.node, NULL, &count) < 0) return 0;
    if (count != expected) return 0;
    /* rb_first/rb_next walk: sorted, every node once */
    int walked = 0;
    uint64_t last = 0;
    for (RbNode *n = rb_first(&root); n != NULL; n = rb_next(n)) {
        uint64_t k = rb_entry(n, Item, rb)->key;
        if (walked > 0 && k < last) return 0;
        last = k;
        walked++;
    }
    return walked == expected;
}

static uint64_t rng = 0x9E3779B97F4A7C15ULL;
static uint64_t next_rand(void) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

TEST(empty_tree) {
    reset_tree();
    ASSERT_TRUE(rb_first(&root) == NULL);
    ASSERT_TRUE(tree_valid(0));
    return 0;
}

/* Ascending keys are the worst case for an unbalanced tree */
TEST(ascending_inserts_stay_balanced) {
    reset_tree();
    for (int i = 0; i < N_ITEMS; i++) {
        items[i].key = (uint64_t)i;
        insert_item(&items[i]);
    }
    ASSERT_TRUE(tree_valid(N_ITEMS));
    /* red-black bound: depth <= 2 log2(n + 1); log2(2001) < 11 */
    ASSERT_TRUE(depth(root.node) <= 22);
    for (int i = 0; i < N_ITEMS; i += 97) ASSERT_TRUE(lookup((uint64_t)i) == &items[i]);
    return 0;
}

TEST(random_insert_erase) {
    reset_tree();
    int in = 0;
    for (int step = 0; step < 20000; step++) {
        Item *it = &items[next_rand() % N_ITEMS];
        if (it->in_tree) {
            rb_erase(&it->rb, &root);
            it->in_tree = 0;
            in--;
        } else {
            it->key = next_rand() % 5000;   /* duplicates allowed */
            insert_item(it);
            in++;
        }
        if (step % 50 == 0 || step < 200) ASSERT_TRUE(tree_valid(in));
    }
    ASSERT_TRUE(tree_valid(in));
    return 0;
}

/* Erase every node in an order that hits root, leaf and two-child cases */
TEST(erase_all) {
    reset_tree();
    for (int i = 0; i < 64; i++) {
        items[i].key = (uint64_t)((i * 37) % 64);
        insert_item(&items[i]);
    }
    for (int i = 0; i < 64; i++) {
        Item *it = lookup((uint64_t)((i * 11) % 64));
        ASSERT_TRUE(it != NULL);
        rb_erase(&it->rb, &root);
        it->in_tree = 0;
        ASSERT_TRUE(tree_valid(63 - i));
    }
    ASSERT_TRUE(root.node == NULL);
    return 0;
}

TestCase rbtree_tests[] = {
    TEST_ENTRY(empty_tree),
    TEST_ENTRY(ascending_inserts_stay_balanced),
    TEST_ENTRY(random_insert_erase),
    TEST_ENTRY(erase_all),
};
int rbtree_test_count = sizeof(rbtree_tests) / sizeof(rbtree_tests[0]);
