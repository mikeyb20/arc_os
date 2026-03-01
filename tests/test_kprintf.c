/* arc_os — Host-side tests for kernel/lib/kprintf.c */

#include "test_framework.h"
#include <stdint.h>
#include <limits.h>

/* Capture buffer for serial_putchar output */
static char capture_buf[4096];
static int capture_pos;

static void capture_reset(void) {
    capture_pos = 0;
    memset(capture_buf, 0, sizeof(capture_buf));
}

static const char *capture_get(void) {
    capture_buf[capture_pos] = '\0';
    return capture_buf;
}

/* Stub serial_putchar — appends to capture buffer */
void serial_putchar(char c) {
    if (capture_pos < (int)sizeof(capture_buf) - 1) {
        capture_buf[capture_pos++] = c;
    }
}

/* Guard against the real serial.h being included (kprintf.c includes it) */
#define ARCHOS_ARCH_X86_64_SERIAL_H
#define SERIAL_COM1 0x3F8
void serial_init(void) {}
void serial_puts(const char *s) { (void)s; }

/* Include the real kprintf implementation */
#include "../kernel/lib/kprintf.c"

/* --- Tests --- */

static int test_plain_string(void) {
    capture_reset();
    kprintf("hello world");
    ASSERT_STR_EQ(capture_get(), "hello world");
    return 0;
}

static int test_format_s(void) {
    capture_reset();
    kprintf("name=%s", "arc_os");
    ASSERT_STR_EQ(capture_get(), "name=arc_os");
    return 0;
}

static int test_format_s_null(void) {
    capture_reset();
    kprintf("%s", (char *)NULL);
    ASSERT_STR_EQ(capture_get(), "(null)");
    return 0;
}

static int test_format_d_positive(void) {
    capture_reset();
    kprintf("%d", 42);
    ASSERT_STR_EQ(capture_get(), "42");
    return 0;
}

static int test_format_d_negative(void) {
    capture_reset();
    kprintf("%d", -7);
    ASSERT_STR_EQ(capture_get(), "-7");
    return 0;
}

static int test_format_d_zero(void) {
    capture_reset();
    kprintf("%d", 0);
    ASSERT_STR_EQ(capture_get(), "0");
    return 0;
}

static int test_format_ld_int64_min(void) {
    capture_reset();
    kprintf("%ld", (int64_t)INT64_MIN);
    ASSERT_STR_EQ(capture_get(), "-9223372036854775808");
    return 0;
}

static int test_format_u(void) {
    capture_reset();
    kprintf("%u", (uint32_t)4294967295U);
    ASSERT_STR_EQ(capture_get(), "4294967295");
    return 0;
}

static int test_format_lu(void) {
    capture_reset();
    kprintf("%lu", (uint64_t)18446744073709551615ULL);
    ASSERT_STR_EQ(capture_get(), "18446744073709551615");
    return 0;
}

static int test_format_x(void) {
    capture_reset();
    kprintf("%x", (uint32_t)0xDEAD);
    ASSERT_STR_EQ(capture_get(), "dead");
    return 0;
}

static int test_format_lx(void) {
    capture_reset();
    kprintf("%lx", (uint64_t)0xCAFEBABEDEADBEEFULL);
    ASSERT_STR_EQ(capture_get(), "cafebabedeadbeef");
    return 0;
}

static int test_format_p(void) {
    capture_reset();
    kprintf("%p", (void *)0xFFFF800000001000ULL);
    ASSERT_STR_EQ(capture_get(), "0xffff800000001000");
    return 0;
}

static int test_format_p_null(void) {
    capture_reset();
    kprintf("%p", (void *)NULL);
    ASSERT_STR_EQ(capture_get(), "0x0000000000000000");
    return 0;
}

static int test_format_percent_literal(void) {
    capture_reset();
    kprintf("100%%");
    ASSERT_STR_EQ(capture_get(), "100%");
    return 0;
}

static int test_unknown_specifier(void) {
    capture_reset();
    kprintf("%q");
    ASSERT_STR_EQ(capture_get(), "%q");
    return 0;
}

static int test_unknown_specifier_with_length(void) {
    capture_reset();
    kprintf("%lq");
    ASSERT_STR_EQ(capture_get(), "%lq");
    return 0;
}

static int test_format_x_zero(void) {
    capture_reset();
    kprintf("%x", (uint32_t)0);
    ASSERT_STR_EQ(capture_get(), "0");
    return 0;
}

static int test_format_s_empty(void) {
    capture_reset();
    kprintf("%s", "");
    ASSERT_STR_EQ(capture_get(), "");
    return 0;
}

static int test_mixed_format(void) {
    capture_reset();
    kprintf("[%s] %d pages (%lu KB) at 0x%lx",
            "PMM", 42, (uint64_t)168, (uint64_t)0x1000);
    ASSERT_STR_EQ(capture_get(), "[PMM] 42 pages (168 KB) at 0x1000");
    return 0;
}

/* --- Test suite export --- */

TestCase kprintf_tests[] = {
    { "plain_string",               test_plain_string },
    { "format_s",                   test_format_s },
    { "format_s_null",              test_format_s_null },
    { "format_d_positive",          test_format_d_positive },
    { "format_d_negative",          test_format_d_negative },
    { "format_d_zero",              test_format_d_zero },
    { "format_ld_int64_min",        test_format_ld_int64_min },
    { "format_u",                   test_format_u },
    { "format_lu",                  test_format_lu },
    { "format_x",                   test_format_x },
    { "format_lx",                  test_format_lx },
    { "format_p",                   test_format_p },
    { "format_p_null",              test_format_p_null },
    { "format_percent_literal",     test_format_percent_literal },
    { "unknown_specifier",          test_unknown_specifier },
    { "unknown_specifier_with_len", test_unknown_specifier_with_length },
    { "format_x_zero",             test_format_x_zero },
    { "format_s_empty",            test_format_s_empty },
    { "mixed_format",              test_mixed_format },
};

int kprintf_test_count = sizeof(kprintf_tests) / sizeof(kprintf_tests[0]);
