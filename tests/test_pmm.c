/* arc_os — Host-side tests for PMM bitmap helpers */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define ASSERT_TRUE(expr) do { \
    if (!(expr)) { \
        printf("  FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (0)

#define ASSERT_EQ(a, b) do { \
    long long _a = (long long)(a); \
    long long _b = (long long)(b); \
    if (_a != _b) { \
        printf("  FAIL: %s:%d: %s == %s (got %lld vs %lld)\n", \
               __FILE__, __LINE__, #a, #b, _a, _b); \
        return 1; \
    } \
} while (0)

/* Import bitmap helpers directly — they're self-contained */
void pmm_bitmap_set(uint64_t *bitmap, uint64_t bit);
void pmm_bitmap_clear(uint64_t *bitmap, uint64_t bit);
int  pmm_bitmap_test(const uint64_t *bitmap, uint64_t bit);

/* Pull in just the bitmap helper functions from pmm.c.
 * We can't include all of pmm.c because it references BootInfo and kprintf.
 * Instead, we compile the helpers separately. */

/* Inline implementations matching pmm.c */
void pmm_bitmap_set(uint64_t *bm, uint64_t bit) {
    bm[bit / 64] |= (1ULL << (bit % 64));
}

void pmm_bitmap_clear(uint64_t *bm, uint64_t bit) {
    bm[bit / 64] &= ~(1ULL << (bit % 64));
}

int pmm_bitmap_test(const uint64_t *bm, uint64_t bit) {
    return (bm[bit / 64] >> (bit % 64)) & 1;
}

typedef int (*test_fn)(void);
typedef struct {
    const char *name;
    test_fn fn;
} TestCase;

/* --- Tests --- */

static int test_bitmap_set_clear(void) {
    uint64_t bitmap[2] = {0, 0};

    /* Set bit 0 */
    pmm_bitmap_set(bitmap, 0);
    ASSERT_EQ(pmm_bitmap_test(bitmap, 0), 1);

    /* Clear bit 0 */
    pmm_bitmap_clear(bitmap, 0);
    ASSERT_EQ(pmm_bitmap_test(bitmap, 0), 0);

    return 0;
}

static int test_bitmap_various_positions(void) {
    uint64_t bitmap[4] = {0, 0, 0, 0};

    /* Test bit 63 (last bit of first qword) */
    pmm_bitmap_set(bitmap, 63);
    ASSERT_EQ(pmm_bitmap_test(bitmap, 63), 1);
    ASSERT_EQ(pmm_bitmap_test(bitmap, 62), 0);
    ASSERT_EQ(pmm_bitmap_test(bitmap, 64), 0);

    /* Test bit 64 (first bit of second qword) */
    pmm_bitmap_set(bitmap, 64);
    ASSERT_EQ(pmm_bitmap_test(bitmap, 64), 1);

    /* Test bit 127 */
    pmm_bitmap_set(bitmap, 127);
    ASSERT_EQ(pmm_bitmap_test(bitmap, 127), 1);

    return 0;
}

static int test_bitmap_no_side_effects(void) {
    uint64_t bitmap[2] = {0, 0};

    /* Setting one bit shouldn't affect neighbors */
    pmm_bitmap_set(bitmap, 5);
    for (int i = 0; i < 128; i++) {
        if (i == 5) {
            ASSERT_EQ(pmm_bitmap_test(bitmap, (uint64_t)i), 1);
        } else {
            ASSERT_EQ(pmm_bitmap_test(bitmap, (uint64_t)i), 0);
        }
    }

    return 0;
}

static int test_bitmap_all_set(void) {
    uint64_t bitmap[2];
    memset(bitmap, 0xFF, sizeof(bitmap));

    /* All bits should be set */
    for (int i = 0; i < 128; i++) {
        ASSERT_EQ(pmm_bitmap_test(bitmap, (uint64_t)i), 1);
    }

    /* Clear one and verify */
    pmm_bitmap_clear(bitmap, 42);
    ASSERT_EQ(pmm_bitmap_test(bitmap, 42), 0);
    ASSERT_EQ(pmm_bitmap_test(bitmap, 41), 1);
    ASSERT_EQ(pmm_bitmap_test(bitmap, 43), 1);

    return 0;
}

static int test_bitmap_roundtrip(void) {
    /* Simulate alloc/free: set a range, clear it, verify */
    uint64_t bitmap[4] = {0, 0, 0, 0};

    /* "Allocate" pages 10-19 */
    for (uint64_t i = 10; i < 20; i++) {
        pmm_bitmap_set(bitmap, i);
    }

    /* Verify allocated */
    for (uint64_t i = 10; i < 20; i++) {
        ASSERT_EQ(pmm_bitmap_test(bitmap, i), 1);
    }

    /* "Free" pages 10-19 */
    for (uint64_t i = 10; i < 20; i++) {
        pmm_bitmap_clear(bitmap, i);
    }

    /* Verify all free */
    for (int i = 0; i < 256; i++) {
        ASSERT_EQ(pmm_bitmap_test(bitmap, (uint64_t)i), 0);
    }

    return 0;
}

static int test_bitmap_first_free_scan(void) {
    /* Manually scan for first free bit (simulating pmm_alloc logic) */
    uint64_t bitmap[2];
    memset(bitmap, 0xFF, sizeof(bitmap));

    /* All allocated — no free bit */
    int found = 0;
    for (int i = 0; i < 128; i++) {
        if (!pmm_bitmap_test(bitmap, (uint64_t)i)) {
            found = 1;
            break;
        }
    }
    ASSERT_EQ(found, 0);

    /* Free bit 73 */
    pmm_bitmap_clear(bitmap, 73);
    uint64_t first_free = 128;
    for (uint64_t i = 0; i < 128; i++) {
        if (!pmm_bitmap_test(bitmap, i)) {
            first_free = i;
            break;
        }
    }
    ASSERT_EQ(first_free, 73);

    return 0;
}

static int test_bitmap_contiguous_scan(void) {
    /* Simulate contiguous allocation scan */
    uint64_t bitmap[2] = {0, 0};

    /* Mark some pages as allocated to create gaps */
    pmm_bitmap_set(bitmap, 3);
    pmm_bitmap_set(bitmap, 7);

    /* Look for 3 contiguous free pages */
    uint64_t run_start = 128;
    size_t run_len = 0;
    for (uint64_t i = 0; i < 128; i++) {
        if (!pmm_bitmap_test(bitmap, i)) {
            if (run_len == 0) run_start = i;
            run_len++;
            if (run_len >= 3) break;
        } else {
            run_len = 0;
        }
    }
    ASSERT_TRUE(run_len >= 3);
    /* First run of 3: pages 0,1,2 */
    ASSERT_EQ(run_start, 0);

    return 0;
}

/* --- Test suite export --- */

TestCase pmm_tests[] = {
    { "bitmap_set_clear",       test_bitmap_set_clear },
    { "bitmap_various_positions", test_bitmap_various_positions },
    { "bitmap_no_side_effects", test_bitmap_no_side_effects },
    { "bitmap_all_set",         test_bitmap_all_set },
    { "bitmap_roundtrip",       test_bitmap_roundtrip },
    { "bitmap_first_free_scan", test_bitmap_first_free_scan },
    { "bitmap_contiguous_scan", test_bitmap_contiguous_scan },
};

int pmm_test_count = sizeof(pmm_tests) / sizeof(pmm_tests[0]);
