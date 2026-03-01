/* arc_os — Host-side tests for kernel/mm/kmalloc.c */

#include "test_framework.h"
#include <stdint.h>

/* Guard kernel headers that conflict with host or need stubbing */
#define ARCHOS_MM_PMM_H
#define ARCHOS_MM_VMM_H
#define ARCHOS_LIB_KPRINTF_H
#define ARCHOS_MM_KMALLOC_H
#define ARCHOS_LIB_MEM_H        /* Use libc memset/memcpy */

#define PAGE_SIZE 4096
#define GFP_KERNEL  0x00
#define GFP_ZERO    0x01
#define VMM_FLAG_WRITABLE 1
#define VMM_FLAG_NOEXEC   4

/* Stub kprintf */
static inline void kprintf(const char *fmt, ...) { (void)fmt; }

/* Static arena replaces the kernel heap region */
#define ARENA_SIZE (512 * 1024)
static uint8_t arena[ARENA_SIZE] __attribute__((aligned(4096)));

/* Stub PMM: returns incrementing fake physical addresses (static to avoid linker clash) */
static uint64_t fake_phys_next;

static uint64_t pmm_alloc_page(void) {
    uint64_t addr = fake_phys_next;
    fake_phys_next += 4096;
    return addr;
}

/* Stub VMM: no-op (static to avoid linker clash) */
static void vmm_map_page(uint64_t virt, uint64_t phys, uint32_t flags) {
    (void)virt; (void)phys; (void)flags;
}

/* Include kmalloc.c — HEAP_START/HEAP_MAX will be defined as kernel addresses,
 * but we bypass kmalloc_init and manually set up the heap in the arena. */
#include "../kernel/mm/kmalloc.c"

/* Reset the heap state before each test — manually init into our arena,
 * bypassing kmalloc_init (which uses HEAP_START that points at kernel VA). */
static void setup_heap(void) {
    memset(arena, 0, ARENA_SIZE);
    fake_phys_next = 0x100000;

    /* Manually set up a 16 KB initial heap in the arena */
    uint64_t arena_base = (uint64_t)(uintptr_t)arena;
    heap_current_end = arena_base + 4 * PAGE_SIZE;

    heap_start_block = (BlockHeader *)arena;
    heap_start_block->magic = BLOCK_MAGIC;
    heap_start_block->size  = (4 * PAGE_SIZE) - HEADER_SIZE;
    heap_start_block->free  = 1;
    heap_start_block->next  = NULL;
    heap_start_block->prev  = NULL;
}

/* --- Tests --- */

static int test_basic_alloc_free(void) {
    setup_heap();
    void *p = kmalloc(64, GFP_KERNEL);
    ASSERT_TRUE(p != NULL);
    kfree(p);
    return 0;
}

static int test_kmalloc_zero_returns_null(void) {
    setup_heap();
    void *p = kmalloc(0, GFP_KERNEL);
    ASSERT_TRUE(p == NULL);
    return 0;
}

static int test_gfp_zero_zeroes_memory(void) {
    setup_heap();
    uint8_t *p = (uint8_t *)kmalloc(128, GFP_ZERO);
    ASSERT_TRUE(p != NULL);
    for (int i = 0; i < 128; i++) {
        ASSERT_EQ(p[i], 0);
    }
    kfree(p);
    return 0;
}

static int test_split_block(void) {
    setup_heap();
    void *a = kmalloc(32, GFP_KERNEL);
    void *b = kmalloc(32, GFP_KERNEL);
    ASSERT_TRUE(a != NULL);
    ASSERT_TRUE(b != NULL);
    ASSERT_TRUE(a != b);
    kfree(a);
    kfree(b);
    return 0;
}

static int test_coalesce(void) {
    setup_heap();
    void *a = kmalloc(64, GFP_KERNEL);
    void *b = kmalloc(64, GFP_KERNEL);
    ASSERT_TRUE(a != NULL);
    ASSERT_TRUE(b != NULL);

    /* Free both — they should coalesce */
    kfree(a);
    kfree(b);

    /* Now allocate combined size — should succeed from the coalesced block */
    void *c = kmalloc(128 + 64, GFP_KERNEL);
    ASSERT_TRUE(c != NULL);
    kfree(c);
    return 0;
}

static int test_multiple_alloc_free_cycles(void) {
    setup_heap();
    void *ptrs[32];
    for (int i = 0; i < 32; i++) {
        ptrs[i] = kmalloc(64, GFP_KERNEL);
        ASSERT_TRUE(ptrs[i] != NULL);
    }
    for (int i = 0; i < 32; i++) {
        kfree(ptrs[i]);
    }
    return 0;
}

static int test_alignment(void) {
    setup_heap();
    for (size_t sz = 1; sz <= 256; sz++) {
        void *p = kmalloc(sz, GFP_KERNEL);
        ASSERT_TRUE(p != NULL);
        ASSERT_EQ((uintptr_t)p % 16, 0);
        kfree(p);
    }
    return 0;
}

static int test_krealloc_null_acts_as_kmalloc(void) {
    setup_heap();
    void *p = krealloc(NULL, 128);
    ASSERT_TRUE(p != NULL);
    kfree(p);
    return 0;
}

static int test_krealloc_zero_frees(void) {
    setup_heap();
    void *p = kmalloc(64, GFP_KERNEL);
    ASSERT_TRUE(p != NULL);
    void *q = krealloc(p, 0);
    ASSERT_TRUE(q == NULL);
    return 0;
}

static int test_krealloc_grow_preserves_data(void) {
    setup_heap();
    uint8_t *p = (uint8_t *)kmalloc(32, GFP_KERNEL);
    ASSERT_TRUE(p != NULL);
    for (int i = 0; i < 32; i++) p[i] = (uint8_t)i;

    uint8_t *q = (uint8_t *)krealloc(p, 256);
    ASSERT_TRUE(q != NULL);
    for (int i = 0; i < 32; i++) {
        ASSERT_EQ(q[i], (uint8_t)i);
    }
    kfree(q);
    return 0;
}

static int test_krealloc_shrink_preserves_data(void) {
    setup_heap();
    uint8_t *p = (uint8_t *)kmalloc(256, GFP_KERNEL);
    ASSERT_TRUE(p != NULL);
    for (int i = 0; i < 64; i++) p[i] = (uint8_t)(i + 100);

    uint8_t *q = (uint8_t *)krealloc(p, 64);
    ASSERT_TRUE(q != NULL);
    for (int i = 0; i < 64; i++) {
        ASSERT_EQ(q[i], (uint8_t)(i + 100));
    }
    kfree(q);
    return 0;
}

static int test_double_free_no_crash(void) {
    setup_heap();
    void *p = kmalloc(64, GFP_KERNEL);
    ASSERT_TRUE(p != NULL);
    kfree(p);
    /* Double free — should print warning (silenced) but not crash */
    kfree(p);
    return 0;
}

static int test_canary_survives_normal_use(void) {
    setup_heap();
    void *p = kmalloc(64, GFP_KERNEL);
    ASSERT_TRUE(p != NULL);

    /* Check that the block header magic is intact */
    uint64_t *header = (uint64_t *)((uint8_t *)p - HEADER_SIZE);
    ASSERT_EQ(*header, BLOCK_MAGIC);

    kfree(p);
    return 0;
}

/* --- Test suite export --- */

TestCase kmalloc_tests[] = {
    { "basic_alloc_free",           test_basic_alloc_free },
    { "kmalloc_zero_returns_null",  test_kmalloc_zero_returns_null },
    { "gfp_zero_zeroes_memory",     test_gfp_zero_zeroes_memory },
    { "split_block",                test_split_block },
    { "coalesce",                   test_coalesce },
    { "multiple_alloc_free_cycles", test_multiple_alloc_free_cycles },
    { "alignment",                  test_alignment },
    { "krealloc_null_as_kmalloc",   test_krealloc_null_acts_as_kmalloc },
    { "krealloc_zero_frees",        test_krealloc_zero_frees },
    { "krealloc_grow_preserves",    test_krealloc_grow_preserves_data },
    { "krealloc_shrink_preserves",  test_krealloc_shrink_preserves_data },
    { "double_free_no_crash",       test_double_free_no_crash },
    { "canary_survives_normal",     test_canary_survives_normal_use },
};

int kmalloc_test_count = sizeof(kmalloc_tests) / sizeof(kmalloc_tests[0]);
