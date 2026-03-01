/* arc_os — Host-side tests for kernel/lib/mem.c */

#include <stdio.h>
#include <string.h>

/* Pull in the test macros from test_main.c via shared header convention.
 * We re-declare them here since this is a minimal framework. */
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

#define ASSERT_MEM_EQ(a, b, n) do { \
    if (memcmp((a), (b), (n)) != 0) { \
        printf("  FAIL: %s:%d: memcmp(%s, %s, %d) != 0\n", \
               __FILE__, __LINE__, #a, #b, (int)(n)); \
        return 1; \
    } \
} while (0)

/* We include the kernel mem.c directly so we test the actual implementation.
 * The kernel's mem.h uses <stddef.h> which works on the host. */
#include "../kernel/lib/mem.c"

typedef int (*test_fn)(void);
typedef struct {
    const char *name;
    test_fn fn;
} TestCase;

/* --- memcpy tests --- */

static int test_memcpy_basic(void) {
    char src[] = "hello";
    char dst[6] = {0};
    void *ret = memcpy(dst, src, 6);
    ASSERT_MEM_EQ(dst, "hello", 6);
    ASSERT_TRUE(ret == dst);
    return 0;
}

static int test_memcpy_zero_length(void) {
    char src[] = "abc";
    char dst[] = "xyz";
    memcpy(dst, src, 0);
    ASSERT_MEM_EQ(dst, "xyz", 3);
    return 0;
}

static int test_memcpy_single_byte(void) {
    char src = 0x42;
    char dst = 0;
    memcpy(&dst, &src, 1);
    ASSERT_EQ(dst, 0x42);
    return 0;
}

static int test_memcpy_large(void) {
    unsigned char src[1024];
    unsigned char dst[1024];
    for (int i = 0; i < 1024; i++) src[i] = (unsigned char)(i & 0xFF);
    memcpy(dst, src, 1024);
    ASSERT_MEM_EQ(dst, src, 1024);
    return 0;
}

/* --- memset tests --- */

static int test_memset_zero(void) {
    char buf[16] = "abcdefghijklmno";
    memset(buf, 0, 16);
    for (int i = 0; i < 16; i++) {
        ASSERT_EQ(buf[i], 0);
    }
    return 0;
}

static int test_memset_pattern(void) {
    unsigned char buf[8];
    void *ret = memset(buf, 0xAB, 8);
    for (int i = 0; i < 8; i++) {
        ASSERT_EQ(buf[i], 0xAB);
    }
    ASSERT_TRUE(ret == buf);
    return 0;
}

static int test_memset_zero_length(void) {
    char buf[] = "abc";
    memset(buf, 'x', 0);
    ASSERT_MEM_EQ(buf, "abc", 3);
    return 0;
}

static int test_memset_partial(void) {
    char buf[] = "hello world";
    memset(buf + 5, '-', 1);
    ASSERT_MEM_EQ(buf, "hello-world", 11);
    return 0;
}

/* --- memmove tests --- */

static int test_memmove_no_overlap(void) {
    char src[] = "abcdef";
    char dst[7] = {0};
    void *ret = memmove(dst, src, 7);
    ASSERT_MEM_EQ(dst, "abcdef", 7);
    ASSERT_TRUE(ret == dst);
    return 0;
}

static int test_memmove_overlap_forward(void) {
    /* dst > src, overlapping: copy backward needed */
    char buf[] = "abcdefgh";
    memmove(buf + 2, buf, 6);
    ASSERT_MEM_EQ(buf, "ababcdef", 8);
    return 0;
}

static int test_memmove_overlap_backward(void) {
    /* dst < src, overlapping: copy forward needed */
    char buf[] = "abcdefgh";
    memmove(buf, buf + 2, 6);
    ASSERT_MEM_EQ(buf, "cdefghgh", 8);
    return 0;
}

static int test_memmove_same_pointer(void) {
    char buf[] = "test";
    memmove(buf, buf, 5);
    ASSERT_MEM_EQ(buf, "test", 5);
    return 0;
}

static int test_memmove_zero_length(void) {
    char buf[] = "abc";
    memmove(buf + 1, buf, 0);
    ASSERT_MEM_EQ(buf, "abc", 3);
    return 0;
}

/* --- memcmp tests --- */

static int test_memcmp_equal(void) {
    ASSERT_EQ(memcmp("abc", "abc", 3), 0);
    return 0;
}

static int test_memcmp_less(void) {
    ASSERT_TRUE(memcmp("abc", "abd", 3) < 0);
    return 0;
}

static int test_memcmp_greater(void) {
    ASSERT_TRUE(memcmp("abd", "abc", 3) > 0);
    return 0;
}

static int test_memcmp_zero_length(void) {
    ASSERT_EQ(memcmp("abc", "xyz", 0), 0);
    return 0;
}

static int test_memcmp_first_byte_differs(void) {
    ASSERT_TRUE(memcmp("\x00\x01", "\x01\x00", 2) < 0);
    return 0;
}

static int test_memcmp_partial_match(void) {
    ASSERT_EQ(memcmp("abcxyz", "abcdef", 3), 0);
    return 0;
}

/* --- Test suite export --- */

TestCase mem_tests[] = {
    { "memcpy_basic",            test_memcpy_basic },
    { "memcpy_zero_length",      test_memcpy_zero_length },
    { "memcpy_single_byte",      test_memcpy_single_byte },
    { "memcpy_large",            test_memcpy_large },
    { "memset_zero",             test_memset_zero },
    { "memset_pattern",          test_memset_pattern },
    { "memset_zero_length",      test_memset_zero_length },
    { "memset_partial",          test_memset_partial },
    { "memmove_no_overlap",      test_memmove_no_overlap },
    { "memmove_overlap_forward", test_memmove_overlap_forward },
    { "memmove_overlap_backward",test_memmove_overlap_backward },
    { "memmove_same_pointer",    test_memmove_same_pointer },
    { "memmove_zero_length",     test_memmove_zero_length },
    { "memcmp_equal",            test_memcmp_equal },
    { "memcmp_less",             test_memcmp_less },
    { "memcmp_greater",          test_memcmp_greater },
    { "memcmp_zero_length",      test_memcmp_zero_length },
    { "memcmp_first_byte_differs", test_memcmp_first_byte_differs },
    { "memcmp_partial_match",    test_memcmp_partial_match },
};

int mem_test_count = sizeof(mem_tests) / sizeof(mem_tests[0]);
