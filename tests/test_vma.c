/* arc_os — Host-side tests for kernel/mm/vma.c (M1-07)
 *
 * The real vma.c on the real rbtree.c (built by test_rbtree.c); the slab
 * cache is malloc. After every operation the set is checked whole: list
 * sorted and linked both ways, head/tail right, ranges page aligned and
 * non-overlapping, the tree holding exactly the list's VMAs, count and page
 * total matching. */

#include "test_framework.h"
#include <stdint.h>

#define ARCHOS_MM_SLAB_H
struct kmem_cache {
    int allocs;
    int frees;
};
static struct kmem_cache vma_test_cache;
static int vma_test_alloc_fail;
static struct kmem_cache *vma_test_cache_create(const char *name, size_t size, size_t align) {
    (void)name;
    (void)size;
    (void)align;
    return &vma_test_cache;
}
static void *vma_test_cache_alloc(struct kmem_cache *c, uint32_t flags) {
    (void)flags;
    if (vma_test_alloc_fail) return NULL;
    c->allocs++;
    return calloc(1, 96);
}
static void vma_test_cache_free(struct kmem_cache *c, void *obj) {
    c->frees++;
    free(obj);
}
#define kmem_cache_create vma_test_cache_create
#define kmem_cache_alloc  vma_test_cache_alloc
#define kmem_cache_free   vma_test_cache_free

#include "../kernel/mm/vma.c"

#define PG 0x1000ULL

static VmaSet set;

static int count_tree(const RbNode *n) {
    return n == NULL ? 0 : 1 + count_tree(n->left) + count_tree(n->right);
}

static int set_valid(void) {
    uint32_t n = 0;
    uint64_t pages = 0;
    const Vma *prev = NULL;
    for (const Vma *v = set.head; v != NULL; v = v->next) {
        if (v->prev != prev) return 0;
        if (v->start >= v->end || (v->start & (PG - 1)) || (v->end & (PG - 1))) return 0;
        if (prev != NULL && prev->end > v->start) return 0;
        if (vma_find_containing(&set, v->start) != v) return 0;   /* reachable in the tree */
        pages += (v->end - v->start) / PG;
        n++;
        prev = v;
    }
    return set.tail == prev && n == set.count && pages == set.pages && count_tree(set.tree.node) == (int)n;
}

static Vma *add(uint64_t start, uint64_t end, uint32_t prot) {
    Vma *v = vma_alloc();
    v->start = start;
    v->end = end;
    v->prot = prot;
    v->flags = VMA_ANON;
    vma_insert(&set, v);
    return v;
}

static void reset_set(void) {
    while (set.head != NULL) {
        Vma *v = set.head;
        vma_remove(&set, v);
        vma_free(v);
    }
    vma_set_init(&set);
    vma_cache_init();
    vma_test_alloc_fail = 0;
}

TEST(insert_keeps_order) {
    reset_set();
    add(0x5000, 0x6000, VMA_READ);
    add(0x1000, 0x2000, VMA_READ);
    add(0x9000, 0xB000, VMA_READ);
    add(0x3000, 0x4000, VMA_READ);
    ASSERT_TRUE(set_valid());
    ASSERT_EQ(set.count, 4);
    ASSERT_EQ(set.pages, 5);
    ASSERT_EQ(set.head->start, 0x1000);
    ASSERT_EQ(set.tail->start, 0x9000);
    return 0;
}

TEST(find_and_range_free) {
    reset_set();
    Vma *a = add(0x1000, 0x3000, VMA_READ);
    Vma *b = add(0x8000, 0x9000, VMA_READ);
    ASSERT_TRUE(vma_find(&set, 0) == a);
    ASSERT_TRUE(vma_find(&set, 0x2FFF) == a);
    ASSERT_TRUE(vma_find(&set, 0x3000) == b);   /* next one up */
    ASSERT_TRUE(vma_find(&set, 0x9000) == NULL);
    ASSERT_TRUE(vma_find_containing(&set, 0x4000) == NULL);
    ASSERT_TRUE(vma_find_containing(&set, 0x8000) == b);
    ASSERT_TRUE(vma_range_free(&set, 0x3000, 0x8000));
    ASSERT_FALSE(vma_range_free(&set, 0x3000, 0x8001));
    ASSERT_FALSE(vma_range_free(&set, 0x2000, 0x4000));
    ASSERT_TRUE(vma_range_free(&set, 0x9000, 0x10000));
    return 0;
}

TEST(split_and_merge) {
    reset_set();
    Vma *a = add(0x10000, 0x20000, VMA_READ | VMA_WRITE);
    Vma *hi = vma_split(&set, a, 0x18000);
    ASSERT_TRUE(hi != NULL && set_valid());
    ASSERT_EQ(a->end, 0x18000);
    ASSERT_EQ(hi->start, 0x18000);
    ASSERT_EQ(hi->prot, VMA_READ | VMA_WRITE);
    Vma *mid = vma_split(&set, a, 0x14000);
    ASSERT_TRUE(set_valid());
    ASSERT_EQ(set.count, 3);
    mid->prot = VMA_READ;              /* mprotect the middle */
    ASSERT_TRUE(vma_merge_around(&set, mid) == mid);   /* differs: no merge */
    ASSERT_EQ(set.count, 3);
    mid->prot = VMA_READ | VMA_WRITE;  /* and back: all three merge */
    Vma *m = vma_merge_around(&set, mid);
    ASSERT_TRUE(set_valid());
    ASSERT_EQ(set.count, 1);
    ASSERT_TRUE(m == a);
    ASSERT_EQ(m->start, 0x10000);
    ASSERT_EQ(m->end, 0x20000);
    ASSERT_EQ(vma_test_cache.allocs - vma_test_cache.frees, 1);   /* the absorbed ones were freed */
    return 0;
}

TEST(split_out_of_memory_leaves_vma) {
    reset_set();
    Vma *a = add(0x10000, 0x20000, VMA_READ);
    vma_test_alloc_fail = 1;
    ASSERT_TRUE(vma_split(&set, a, 0x18000) == NULL);
    vma_test_alloc_fail = 0;
    ASSERT_TRUE(set_valid());
    ASSERT_EQ(a->end, 0x20000);
    return 0;
}

TEST(merge_needs_same_kind) {
    reset_set();
    Vma *a = add(0x1000, 0x2000, VMA_READ);
    Vma *b = add(0x2000, 0x3000, VMA_READ);
    Vma *c = add(0x4000, 0x5000, VMA_READ);   /* not adjacent */
    ASSERT_TRUE(vma_can_merge(a, b));
    ASSERT_FALSE(vma_can_merge(b, c));
    b->flags |= VMA_HEAP;
    ASSERT_FALSE(vma_can_merge(a, b));
    b->flags = a->flags;
    static int fake_file;
    b->file = &fake_file;
    ASSERT_FALSE(vma_can_merge(a, b));   /* file VMAs: M1-12 */
    return 0;
}

TEST(remove_head_tail_middle) {
    reset_set();
    Vma *a = add(0x1000, 0x2000, VMA_READ);
    Vma *b = add(0x3000, 0x4000, VMA_READ);
    Vma *c = add(0x5000, 0x6000, VMA_READ);
    vma_remove(&set, b);
    vma_free(b);
    ASSERT_TRUE(set_valid() && a->next == c && c->prev == a);
    vma_remove(&set, a);
    vma_free(a);
    ASSERT_TRUE(set_valid() && set.head == c);
    vma_remove(&set, c);
    vma_free(c);
    ASSERT_TRUE(set_valid() && set.head == NULL && set.tail == NULL && set.pages == 0);
    return 0;
}

/* Top-down gap search: highest fit at or below the hint, inside [low, high) */
TEST(find_gap) {
    reset_set();
    ASSERT_EQ(vma_find_gap(&set, 0x2000, 0x10000, 0x100000, 0x100000), 0x100000 - 0x2000);
    add(0xF0000, 0x100000, VMA_READ);
    add(0xE0000, 0xEF000, VMA_READ);   /* leaves a one-page hole at 0xEF000 */
    ASSERT_EQ(vma_find_gap(&set, 0x1000, 0x10000, 0x100000, 0x100000), 0xEF000);
    ASSERT_EQ(vma_find_gap(&set, 0x2000, 0x10000, 0x100000, 0x100000), 0xE0000 - 0x2000);
    /* a hint below a VMA: search starts there */
    ASSERT_EQ(vma_find_gap(&set, 0x1000, 0x10000, 0x100000, 0x50000), 0x4F000);
    /* a hint inside a VMA: below its start */
    ASSERT_EQ(vma_find_gap(&set, 0x1000, 0x10000, 0x100000, 0xE8000), 0xDF000);
    /* nothing fits */
    ASSERT_EQ(vma_find_gap(&set, 0x100000, 0x10000, 0x100000, 0x100000), 0);
    add(0x10000, 0xE0000, VMA_READ);
    ASSERT_EQ(vma_find_gap(&set, 0x2000, 0x10000, 0x100000, 0x100000), 0);
    ASSERT_EQ(vma_find_gap(&set, 0x1000, 0x10000, 0x100000, 0x100000), 0xEF000);
    ASSERT_EQ(vma_find_gap(&set, 0, 0x10000, 0x100000, 0x100000), 0);
    return 0;
}

/* Many VMAs inserted in random order, removed in another: still valid */
TEST(random_insert_remove) {
    reset_set();
    static Vma *slots[256];
    for (int i = 0; i < 256; i++) slots[i] = NULL;
    uint64_t r = 12345;
    for (int step = 0; step < 4000; step++) {
        r = r * 6364136223846793005ULL + 1442695040888963407ULL;
        int i = (int)((r >> 33) % 256);
        if (slots[i] != NULL) {
            vma_remove(&set, slots[i]);
            vma_free(slots[i]);
            slots[i] = NULL;
        } else {
            uint64_t start = (uint64_t)i * 4 * PG;
            slots[i] = add(start, start + (1 + (r >> 60) % 3) * PG, VMA_READ);
        }
        if (step % 100 == 0) ASSERT_TRUE(set_valid());
    }
    ASSERT_TRUE(set_valid());
    return 0;
}

TestCase vma_tests[] = {
    TEST_ENTRY(insert_keeps_order),
    TEST_ENTRY(find_and_range_free),
    TEST_ENTRY(split_and_merge),
    TEST_ENTRY(split_out_of_memory_leaves_vma),
    TEST_ENTRY(merge_needs_same_kind),
    TEST_ENTRY(remove_head_tail_middle),
    TEST_ENTRY(find_gap),
    TEST_ENTRY(random_insert_remove),
};
int vma_test_count = sizeof(vma_tests) / sizeof(vma_tests[0]);
