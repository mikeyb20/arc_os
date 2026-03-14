/* arc_os — Host-side tests for user pointer validation */

#include "test_framework.h"
#include <stdint.h>
#include <stddef.h>

/* Reproduce the validation function (no kernel deps) */
#define USER_ADDR_LIMIT 0x0000800000000000ULL

static int user_ptr_valid(const void *ptr, size_t len) {
    if (ptr == NULL || len == 0) return 0;
    uint64_t start = (uint64_t)ptr;
    uint64_t end = start + len;
    return (end > start) && (end <= USER_ADDR_LIMIT);
}

/* --- Tests --- */

TEST(valid_user_pointer) {
    ASSERT_TRUE(user_ptr_valid((void *)0x400000, 4096));
    return 0;
}

TEST(null_pointer_invalid) {
    ASSERT_FALSE(user_ptr_valid(NULL, 1));
    return 0;
}

TEST(kernel_pointer_invalid) {
    ASSERT_FALSE(user_ptr_valid((void *)0xFFFFFFFF80000000ULL, 1));
    return 0;
}

TEST(boundary_pointer) {
    /* Exactly at limit - 1 byte: valid */
    ASSERT_TRUE(user_ptr_valid((void *)(USER_ADDR_LIMIT - 1), 1));
    /* At limit: invalid */
    ASSERT_FALSE(user_ptr_valid((void *)USER_ADDR_LIMIT, 1));
    return 0;
}

TEST(overflow_wraps_invalid) {
    ASSERT_FALSE(user_ptr_valid((void *)0xFFFFFFFFFFFFFFFFULL, 2));
    return 0;
}

TEST(zero_length_invalid) {
    ASSERT_FALSE(user_ptr_valid((void *)0x400000, 0));
    return 0;
}

TEST(crosses_boundary_invalid) {
    ASSERT_FALSE(user_ptr_valid((void *)(USER_ADDR_LIMIT - 10), 20));
    return 0;
}

/* --- Suite --- */

TestCase user_access_tests[] = {
    TEST_ENTRY(valid_user_pointer),
    TEST_ENTRY(null_pointer_invalid),
    TEST_ENTRY(kernel_pointer_invalid),
    TEST_ENTRY(boundary_pointer),
    TEST_ENTRY(overflow_wraps_invalid),
    TEST_ENTRY(zero_length_invalid),
    TEST_ENTRY(crosses_boundary_invalid),
};
int user_access_test_count = sizeof(user_access_tests) / sizeof(user_access_tests[0]);
