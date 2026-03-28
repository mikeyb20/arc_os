/* arc_os — Tests for libc string functions */

#include "test_framework.h"

/* We test the same functions that libc provides, but using the kernel's
 * implementations for host-side testing. The logic is identical. */

/* --- strlen --- */
TEST(arc_str_strlen_empty) { ASSERT_EQ(strlen(""), 0); return 0; }
TEST(arc_str_strlen_hello) { ASSERT_EQ(strlen("hello"), 5); return 0; }
TEST(arc_str_strlen_one) { ASSERT_EQ(strlen("x"), 1); return 0; }

/* --- strcmp --- */
TEST(arc_str_strcmp_equal) { ASSERT_EQ(strcmp("abc", "abc"), 0); return 0; }
TEST(arc_str_strcmp_less) { ASSERT_TRUE(strcmp("abc", "abd") < 0); return 0; }
TEST(arc_str_strcmp_greater) { ASSERT_TRUE(strcmp("abd", "abc") > 0); return 0; }
TEST(arc_str_strcmp_prefix) { ASSERT_TRUE(strcmp("ab", "abc") < 0); return 0; }
TEST(arc_str_strcmp_empty) { ASSERT_EQ(strcmp("", ""), 0); return 0; }

/* --- strncmp --- */
TEST(arc_str_strncmp_eq) { ASSERT_EQ(strncmp("abc", "abd", 2), 0); return 0; }
TEST(arc_str_strncmp_diff) { ASSERT_TRUE(strncmp("abc", "abd", 3) != 0); return 0; }
TEST(arc_str_strncmp_zero) { ASSERT_EQ(strncmp("x", "y", 0), 0); return 0; }

/* --- strchr --- */
TEST(arc_str_strchr_found) { ASSERT_TRUE(strchr("hello", 'l') != NULL); return 0; }
TEST(arc_str_strchr_not_found) { ASSERT_TRUE(strchr("hello", 'z') == NULL); return 0; }
TEST(arc_str_strchr_nul) { ASSERT_TRUE(strchr("abc", '\0') != NULL); return 0; }

/* --- strstr --- */
TEST(arc_str_strstr_found) { ASSERT_TRUE(strstr("hello world", "world") != NULL); return 0; }
TEST(arc_str_strstr_not_found) { ASSERT_TRUE(strstr("hello", "xyz") == NULL); return 0; }
TEST(arc_str_strstr_empty_needle) { ASSERT_TRUE(strstr("hello", "") != NULL); return 0; }
TEST(arc_str_strstr_start) { ASSERT_TRUE(strstr("hello", "hel") != NULL); return 0; }

/* --- strcat --- */
TEST(arc_str_strcat) {
    char buf[32] = "hello";
    strcat(buf, " world");
    ASSERT_STR_EQ(buf, "hello world");
    return 0;
}

/* --- memcpy --- */
TEST(arc_str_memcpy) {
    char src[] = "test";
    char dst[5];
    memcpy(dst, src, 5);
    ASSERT_MEM_EQ(dst, src, 5);
    return 0;
}

/* --- memset --- */
TEST(arc_str_memset) {
    char buf[4];
    memset(buf, 'A', 4);
    ASSERT_EQ(buf[0], 'A');
    ASSERT_EQ(buf[3], 'A');
    return 0;
}

/* --- memmove overlapping --- */
TEST(arc_str_memmove_overlap) {
    char buf[10] = "abcdef";
    memmove(buf + 2, buf, 4);
    ASSERT_EQ(buf[2], 'a');
    ASSERT_EQ(buf[3], 'b');
    ASSERT_EQ(buf[4], 'c');
    ASSERT_EQ(buf[5], 'd');
    return 0;
}

/* --- memcmp --- */
TEST(arc_str_memcmp_equal) { ASSERT_EQ(memcmp("abc", "abc", 3), 0); return 0; }
TEST(arc_str_memcmp_diff) { ASSERT_TRUE(memcmp("abc", "abd", 3) != 0); return 0; }

TestCase arc_string_tests[] = {
    TEST_ENTRY(arc_str_strlen_empty),
    TEST_ENTRY(arc_str_strlen_hello),
    TEST_ENTRY(arc_str_strlen_one),
    TEST_ENTRY(arc_str_strcmp_equal),
    TEST_ENTRY(arc_str_strcmp_less),
    TEST_ENTRY(arc_str_strcmp_greater),
    TEST_ENTRY(arc_str_strcmp_prefix),
    TEST_ENTRY(arc_str_strcmp_empty),
    TEST_ENTRY(arc_str_strncmp_eq),
    TEST_ENTRY(arc_str_strncmp_diff),
    TEST_ENTRY(arc_str_strncmp_zero),
    TEST_ENTRY(arc_str_strchr_found),
    TEST_ENTRY(arc_str_strchr_not_found),
    TEST_ENTRY(arc_str_strchr_nul),
    TEST_ENTRY(arc_str_strstr_found),
    TEST_ENTRY(arc_str_strstr_not_found),
    TEST_ENTRY(arc_str_strstr_empty_needle),
    TEST_ENTRY(arc_str_strstr_start),
    TEST_ENTRY(arc_str_strcat),
    TEST_ENTRY(arc_str_memcpy),
    TEST_ENTRY(arc_str_memset),
    TEST_ENTRY(arc_str_memmove_overlap),
    TEST_ENTRY(arc_str_memcmp_equal),
    TEST_ENTRY(arc_str_memcmp_diff),
};
int arc_string_test_count = sizeof(arc_string_tests) / sizeof(arc_string_tests[0]);
