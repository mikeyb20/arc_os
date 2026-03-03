/* arc_os — Host-side tests for kernel/lib/string.c */

#include "test_framework.h"

/* Guard kernel header — we use libc string functions in test_framework.h,
 * so we test the kernel implementations by including the .c directly
 * with renamed function names via macros. */
#define ARCHOS_LIB_STRING_H  /* Guard the kernel header */

/* Rename kernel functions to avoid libc collisions */
#define strlen  k_strlen
#define strcmp  k_strcmp
#define strncmp k_strncmp
#define strncpy k_strncpy
#define strchr  k_strchr

#include "../kernel/lib/string.c"

#undef strlen
#undef strcmp
#undef strncmp
#undef strncpy
#undef strchr

/* --- Tests --- */

static int test_strlen_basic(void) {
    ASSERT_EQ(k_strlen("hello"), 5);
    ASSERT_EQ(k_strlen("a"), 1);
    ASSERT_EQ(k_strlen("arc_os"), 6);
    return 0;
}

static int test_strlen_empty(void) {
    ASSERT_EQ(k_strlen(""), 0);
    return 0;
}

static int test_strcmp_equal(void) {
    ASSERT_EQ(k_strcmp("abc", "abc"), 0);
    ASSERT_EQ(k_strcmp("", ""), 0);
    return 0;
}

static int test_strcmp_less(void) {
    ASSERT_TRUE(k_strcmp("abc", "abd") < 0);
    ASSERT_TRUE(k_strcmp("ab", "abc") < 0);
    return 0;
}

static int test_strcmp_greater(void) {
    ASSERT_TRUE(k_strcmp("abd", "abc") > 0);
    ASSERT_TRUE(k_strcmp("abc", "ab") > 0);
    return 0;
}

static int test_strncmp_with_limit(void) {
    ASSERT_EQ(k_strncmp("abcdef", "abcxyz", 3), 0);
    ASSERT_TRUE(k_strncmp("abcdef", "abcxyz", 4) != 0);
    ASSERT_EQ(k_strncmp("abc", "abc", 10), 0);
    return 0;
}

static int test_strncpy_basic(void) {
    char buf[16];
    memset(buf, 'X', sizeof(buf));
    k_strncpy(buf, "hello", sizeof(buf));
    ASSERT_MEM_EQ(buf, "hello\0\0\0\0\0\0\0\0\0\0\0", 16);
    return 0;
}

static int test_strchr_found(void) {
    const char *s = "hello world";
    char *p = k_strchr(s, 'w');
    ASSERT_TRUE(p != NULL);
    ASSERT_EQ(p - s, 6);
    return 0;
}

static int test_strchr_not_found(void) {
    ASSERT_TRUE(k_strchr("hello", 'z') == NULL);
    return 0;
}

/* --- Test suite export --- */

TestCase string_tests[] = {
    { "strlen_basic",       test_strlen_basic },
    { "strlen_empty",       test_strlen_empty },
    { "strcmp_equal",        test_strcmp_equal },
    { "strcmp_less",         test_strcmp_less },
    { "strcmp_greater",      test_strcmp_greater },
    { "strncmp_with_limit", test_strncmp_with_limit },
    { "strncpy_basic",      test_strncpy_basic },
    { "strchr_found",       test_strchr_found },
    { "strchr_not_found",   test_strchr_not_found },
};

int string_test_count = sizeof(string_tests) / sizeof(string_tests[0]);
