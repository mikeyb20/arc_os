/* arc_os — Host-side tests for kernel/mm/pmm.c (bitmap helpers + alloc API) */

#include "test_framework.h"
#include <stdint.h>

/* Guard headers that conflict with host environment or need stubbing */
#define ARCHOS_LIB_KPRINTF_H   /* Prevent kprintf.h declaration */
#define ARCHOS_LIB_MEM_H       /* Use libc memset/memcmp instead of kernel's */

/* Stub kprintf — silences all kernel prints */
static inline void kprintf(const char *fmt, ...) { (void)fmt; }

/* Include the real PMM implementation */
#include "../kernel/mm/pmm.c"

/* ============================================================
 * Part 1: Bitmap helper tests (unit tests, no pmm_init needed)
 * ============================================================ */

static int test_bitmap_set_clear(void) {
    uint64_t bitmap[2] = {0, 0};

    pmm_bitmap_set(bitmap, 0);
    ASSERT_EQ(pmm_bitmap_test(bitmap, 0), 1);

    pmm_bitmap_clear(bitmap, 0);
    ASSERT_EQ(pmm_bitmap_test(bitmap, 0), 0);

    return 0;
}

static int test_bitmap_various_positions(void) {
    uint64_t bm[4] = {0, 0, 0, 0};

    /* Last bit of first qword */
    pmm_bitmap_set(bm, 63);
    ASSERT_EQ(pmm_bitmap_test(bm, 63), 1);
    ASSERT_EQ(pmm_bitmap_test(bm, 62), 0);
    ASSERT_EQ(pmm_bitmap_test(bm, 64), 0);

    /* First bit of second qword */
    pmm_bitmap_set(bm, 64);
    ASSERT_EQ(pmm_bitmap_test(bm, 64), 1);

    /* Bit 127 */
    pmm_bitmap_set(bm, 127);
    ASSERT_EQ(pmm_bitmap_test(bm, 127), 1);

    return 0;
}

static int test_bitmap_no_side_effects(void) {
    uint64_t bm[2] = {0, 0};

    pmm_bitmap_set(bm, 5);
    for (int i = 0; i < 128; i++) {
        if (i == 5) {
            ASSERT_EQ(pmm_bitmap_test(bm, (uint64_t)i), 1);
        } else {
            ASSERT_EQ(pmm_bitmap_test(bm, (uint64_t)i), 0);
        }
    }

    return 0;
}

static int test_bitmap_all_set(void) {
    uint64_t bm[2];
    memset(bm, 0xFF, sizeof(bm));

    for (int i = 0; i < 128; i++) {
        ASSERT_EQ(pmm_bitmap_test(bm, (uint64_t)i), 1);
    }

    pmm_bitmap_clear(bm, 42);
    ASSERT_EQ(pmm_bitmap_test(bm, 42), 0);
    ASSERT_EQ(pmm_bitmap_test(bm, 41), 1);
    ASSERT_EQ(pmm_bitmap_test(bm, 43), 1);

    return 0;
}

static int test_bitmap_roundtrip(void) {
    uint64_t bm[4] = {0, 0, 0, 0};

    /* "Allocate" pages 10-19 */
    for (uint64_t i = 10; i < 20; i++) {
        pmm_bitmap_set(bm, i);
    }
    for (uint64_t i = 10; i < 20; i++) {
        ASSERT_EQ(pmm_bitmap_test(bm, i), 1);
    }

    /* "Free" pages 10-19 */
    for (uint64_t i = 10; i < 20; i++) {
        pmm_bitmap_clear(bm, i);
    }
    for (int i = 0; i < 256; i++) {
        ASSERT_EQ(pmm_bitmap_test(bm, (uint64_t)i), 0);
    }

    return 0;
}

static int test_bitmap_first_free_scan(void) {
    uint64_t bm[2];
    memset(bm, 0xFF, sizeof(bm));

    /* All allocated — no free bit */
    int found = 0;
    for (int i = 0; i < 128; i++) {
        if (!pmm_bitmap_test(bm, (uint64_t)i)) {
            found = 1;
            break;
        }
    }
    ASSERT_EQ(found, 0);

    /* Free bit 73 */
    pmm_bitmap_clear(bm, 73);
    uint64_t first_free = 128;
    for (uint64_t i = 0; i < 128; i++) {
        if (!pmm_bitmap_test(bm, i)) {
            first_free = i;
            break;
        }
    }
    ASSERT_EQ(first_free, 73);

    return 0;
}

static int test_bitmap_contiguous_scan(void) {
    uint64_t bm[2] = {0, 0};

    pmm_bitmap_set(bm, 3);
    pmm_bitmap_set(bm, 7);

    /* Look for 3 contiguous free pages */
    uint64_t run_start = 128;
    size_t run_len = 0;
    for (uint64_t i = 0; i < 128; i++) {
        if (!pmm_bitmap_test(bm, i)) {
            if (run_len == 0) run_start = i;
            run_len++;
            if (run_len >= 3) break;
        } else {
            run_len = 0;
        }
    }
    ASSERT_TRUE(run_len >= 3);
    ASSERT_EQ(run_start, 0);

    return 0;
}

/* ============================================================
 * Part 2: PMM alloc API tests (require pmm_init with fake memory)
 * ============================================================ */

/* Fake physical memory: 64 pages. We use base=0 with hhdm_offset pointing at
 * the arena, so pmm_init sees a small physical address range (0 to 64*4K)
 * and bitmap = (phys_addr + hhdm_offset) resolves to valid host memory. */
#define FAKE_PAGES 64
static uint8_t fake_phys_mem[FAKE_PAGES * PAGE_SIZE]
    __attribute__((aligned(PAGE_SIZE)));

static void setup_pmm(void) {
    /* Reset static state before each alloc test */
    bitmap = NULL;
    bitmap_size = 0;
    total_pages = 0;
    free_pages = 0;
    highest_addr = 0;
    hhdm_offset = 0;

    BootInfo info = {0};
    /* Use base=PAGE_SIZE to avoid bitmap_phys==0 (which pmm_init treats as error).
     * hhdm_offset translates "physical" addresses back to our host arena. */
    info.memory_map_count = 1;
    info.memory_map[0].base = PAGE_SIZE;
    info.memory_map[0].length = (FAKE_PAGES - 1) * PAGE_SIZE;
    info.memory_map[0].type = MEMMAP_USABLE;
    info.hhdm_offset = (uint64_t)(uintptr_t)fake_phys_mem - PAGE_SIZE;

    pmm_init(&info);
}

static int test_pmm_init_nonzero(void) {
    setup_pmm();
    ASSERT_TRUE(pmm_get_total_pages() > 0);
    ASSERT_TRUE(pmm_get_free_pages() > 0);
    /* Free pages < total because bitmap + page 0 guard take some */
    ASSERT_TRUE(pmm_get_free_pages() < pmm_get_total_pages());
    return 0;
}

static int test_pmm_alloc_returns_nonzero(void) {
    setup_pmm();
    uint64_t page = pmm_alloc_page();
    ASSERT_TRUE(page != 0);
    return 0;
}

static int test_pmm_alloc_decrements_free(void) {
    setup_pmm();
    uint64_t before = pmm_get_free_pages();
    pmm_alloc_page();
    ASSERT_EQ(pmm_get_free_pages(), before - 1);
    return 0;
}

static int test_pmm_successive_allocs_distinct(void) {
    setup_pmm();
    uint64_t a = pmm_alloc_page();
    uint64_t b = pmm_alloc_page();
    ASSERT_TRUE(a != 0);
    ASSERT_TRUE(b != 0);
    ASSERT_TRUE(a != b);
    return 0;
}

static int test_pmm_free_increments_count(void) {
    setup_pmm();
    uint64_t before = pmm_get_free_pages();
    uint64_t page = pmm_alloc_page();
    ASSERT_EQ(pmm_get_free_pages(), before - 1);
    pmm_free_page(page);
    ASSERT_EQ(pmm_get_free_pages(), before);
    return 0;
}

static int test_pmm_free_page_zero_ignored(void) {
    setup_pmm();
    uint64_t before = pmm_get_free_pages();
    pmm_free_page(0);
    ASSERT_EQ(pmm_get_free_pages(), before);
    return 0;
}

static int test_pmm_exhaust_then_fail(void) {
    setup_pmm();
    /* Drain all pages */
    while (pmm_get_free_pages() > 0) {
        uint64_t p = pmm_alloc_page();
        ASSERT_TRUE(p != 0);
    }
    /* Next alloc should fail */
    ASSERT_EQ(pmm_alloc_page(), 0);
    return 0;
}

static int test_pmm_alloc_contiguous_success(void) {
    setup_pmm();
    uint64_t before = pmm_get_free_pages();
    uint64_t base = pmm_alloc_contiguous(4);
    ASSERT_TRUE(base != 0);
    ASSERT_EQ(pmm_get_free_pages(), before - 4);
    return 0;
}

static int test_pmm_contiguous_pages_sequential(void) {
    setup_pmm();
    uint64_t base = pmm_alloc_contiguous(4);
    ASSERT_TRUE(base != 0);
    /* Pages should be physically contiguous */
    ASSERT_EQ(base % PAGE_SIZE, 0);
    /* Free them individually and check count goes up by 4 */
    uint64_t before = pmm_get_free_pages();
    for (int i = 0; i < 4; i++) {
        pmm_free_page(base + (uint64_t)i * PAGE_SIZE);
    }
    ASSERT_EQ(pmm_get_free_pages(), before + 4);
    return 0;
}

static int test_pmm_alloc_after_free_reuses(void) {
    setup_pmm();
    uint64_t page = pmm_alloc_page();
    pmm_free_page(page);
    /* Allocating again should give us the same page (first-fit) */
    uint64_t page2 = pmm_alloc_page();
    ASSERT_EQ(page, page2);
    return 0;
}

static int test_pmm_contiguous_zero_returns_zero(void) {
    setup_pmm();
    ASSERT_EQ(pmm_alloc_contiguous(0), 0);
    return 0;
}

/* --- Test suite export --- */

TestCase pmm_tests[] = {
    /* Bitmap helpers */
    { "bitmap_set_clear",         test_bitmap_set_clear },
    { "bitmap_various_positions", test_bitmap_various_positions },
    { "bitmap_no_side_effects",   test_bitmap_no_side_effects },
    { "bitmap_all_set",           test_bitmap_all_set },
    { "bitmap_roundtrip",         test_bitmap_roundtrip },
    { "bitmap_first_free_scan",   test_bitmap_first_free_scan },
    { "bitmap_contiguous_scan",   test_bitmap_contiguous_scan },
    /* Alloc API */
    { "pmm_init_nonzero",           test_pmm_init_nonzero },
    { "pmm_alloc_returns_nonzero",  test_pmm_alloc_returns_nonzero },
    { "pmm_alloc_decrements_free",  test_pmm_alloc_decrements_free },
    { "pmm_successive_allocs_distinct", test_pmm_successive_allocs_distinct },
    { "pmm_free_increments_count",  test_pmm_free_increments_count },
    { "pmm_free_page_zero_ignored", test_pmm_free_page_zero_ignored },
    { "pmm_exhaust_then_fail",      test_pmm_exhaust_then_fail },
    { "pmm_alloc_contiguous_success", test_pmm_alloc_contiguous_success },
    { "pmm_contiguous_pages_sequential", test_pmm_contiguous_pages_sequential },
    { "pmm_alloc_after_free_reuses", test_pmm_alloc_after_free_reuses },
    { "pmm_contiguous_zero_returns_zero", test_pmm_contiguous_zero_returns_zero },
};

int pmm_test_count = sizeof(pmm_tests) / sizeof(pmm_tests[0]);
