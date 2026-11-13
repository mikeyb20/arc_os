/* arc_os — Shared test framework header */

#ifndef ARCHOS_TEST_FRAMEWORK_H
#define ARCHOS_TEST_FRAMEWORK_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Assertion macros */
#define ASSERT_TRUE(expr) do { \
    if (!(expr)) { \
        printf("  FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (0)

#define ASSERT_FALSE(expr) ASSERT_TRUE(!(expr))

#define ASSERT_EQ(a, b) do { \
    long long _a = (long long)(a); \
    long long _b = (long long)(b); \
    if (_a != _b) { \
        printf("  FAIL: %s:%d: %s == %s (got %lld vs %lld)\n", \
               __FILE__, __LINE__, #a, #b, _a, _b); \
        return 1; \
    } \
} while (0)

#define ASSERT_NEQ(a, b) do { \
    long long _a = (long long)(a); \
    long long _b = (long long)(b); \
    if (_a == _b) { \
        printf("  FAIL: %s:%d: %s != %s (both %lld)\n", \
               __FILE__, __LINE__, #a, #b, _a); \
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

#define ASSERT_STR_EQ(a, b) do { \
    const char *_a = (a); \
    const char *_b = (b); \
    if (strcmp(_a, _b) != 0) { \
        printf("  FAIL: %s:%d: %s == %s\n    got:      \"%s\"\n    expected: \"%s\"\n", \
               __FILE__, __LINE__, #a, #b, _a, _b); \
        return 1; \
    } \
} while (0)

/* Test case type */
typedef int (*test_fn)(void);

typedef struct {
    const char *name;
    test_fn fn;
} TestCase;

#define TEST(name) static int test_##name(void)
#define TEST_ENTRY(name) { #name, test_##name }

/* A named list of tests; count points at the list's length variable. */
typedef struct {
    const char *name;
    TestCase *tests;
    int *count;
} TestSuite;

/* Run every suite, or only the one named by `--suite NAME`, and print the
 * results under title. Returns the process exit status (1 if any failed). */
int test_harness_main(int argc, char **argv, const char *title, const TestSuite *suites, int nsuites);

#endif /* ARCHOS_TEST_FRAMEWORK_H */
