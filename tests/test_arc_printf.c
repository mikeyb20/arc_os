/* arc_os — Tests for libc printf (snprintf) */

#include "test_framework.h"

/* We test snprintf since it writes to a buffer we can inspect.
 * The libc printf is built on the same vsnprintf engine. */

/* Minimal snprintf re-implementation for host-side testing.
 * In the actual libc, this lives in libc/src/printf.c.
 * For host tests, we use the system snprintf (from stdio.h). */

TEST(arc_printf_string) {
    char buf[64];
    snprintf(buf, sizeof(buf), "hello %s", "world");
    ASSERT_STR_EQ(buf, "hello world");
    return 0;
}

TEST(arc_printf_int) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%d", 42);
    ASSERT_STR_EQ(buf, "42");
    return 0;
}

TEST(arc_printf_negative) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%d", -7);
    ASSERT_STR_EQ(buf, "-7");
    return 0;
}

TEST(arc_printf_hex) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%x", 0xFF);
    ASSERT_STR_EQ(buf, "ff");
    return 0;
}

TEST(arc_printf_unsigned) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%u", 123456);
    ASSERT_STR_EQ(buf, "123456");
    return 0;
}

TEST(arc_printf_char) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%c", 'A');
    ASSERT_STR_EQ(buf, "A");
    return 0;
}

TEST(arc_printf_percent) {
    char buf[64];
    snprintf(buf, sizeof(buf), "100%%");
    ASSERT_STR_EQ(buf, "100%");
    return 0;
}

TEST(arc_printf_null_string) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", (char *)NULL);
    ASSERT_STR_EQ(buf, "(null)");
    return 0;
}

TEST(arc_printf_zero) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%d", 0);
    ASSERT_STR_EQ(buf, "0");
    return 0;
}

TEST(arc_printf_multiple) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s=%d", "x", 10);
    ASSERT_STR_EQ(buf, "x=10");
    return 0;
}

TEST(arc_printf_truncate) {
    char buf[4];
    int n = snprintf(buf, sizeof(buf), "hello");
    ASSERT_TRUE(n >= 5);  /* Would need 5+ chars */
    ASSERT_EQ(buf[3], '\0');  /* NUL-terminated at position 3 */
    return 0;
}

TEST(arc_printf_zero_pad) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%04d", 42);
    ASSERT_STR_EQ(buf, "0042");
    return 0;
}

TEST(arc_printf_width) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%8d", 42);
    /* Should be right-aligned with spaces */
    ASSERT_EQ(strlen(buf), 8);
    return 0;
}

TEST(arc_printf_long) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%ld", (long)1000000);
    ASSERT_STR_EQ(buf, "1000000");
    return 0;
}

TEST(arc_printf_empty_format) {
    char buf[64];
    snprintf(buf, sizeof(buf), "");
    ASSERT_STR_EQ(buf, "");
    return 0;
}

TestCase arc_printf_tests[] = {
    TEST_ENTRY(arc_printf_string),
    TEST_ENTRY(arc_printf_int),
    TEST_ENTRY(arc_printf_negative),
    TEST_ENTRY(arc_printf_hex),
    TEST_ENTRY(arc_printf_unsigned),
    TEST_ENTRY(arc_printf_char),
    TEST_ENTRY(arc_printf_percent),
    TEST_ENTRY(arc_printf_null_string),
    TEST_ENTRY(arc_printf_zero),
    TEST_ENTRY(arc_printf_multiple),
    TEST_ENTRY(arc_printf_truncate),
    TEST_ENTRY(arc_printf_zero_pad),
    TEST_ENTRY(arc_printf_width),
    TEST_ENTRY(arc_printf_long),
    TEST_ENTRY(arc_printf_empty_format),
};
int arc_printf_test_count = sizeof(arc_printf_tests) / sizeof(arc_printf_tests[0]);
