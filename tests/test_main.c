/* arc_os — Minimal host-side test framework */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Test result tracking */
static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;
static const char *current_test = NULL;

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

/* Test runner */
typedef int (*test_fn)(void);

typedef struct {
    const char *name;
    test_fn fn;
} TestCase;

#define TEST(name) static int test_##name(void)
#define TEST_ENTRY(name) { #name, test_##name }

/* Collect tests from test files */
extern TestCase mem_tests[];
extern int mem_test_count;
extern TestCase pmm_tests[];
extern int pmm_test_count;

static void run_suite(const char *suite_name, TestCase *tests, int count) {
    printf("[%s] Running %d tests\n", suite_name, count);
    for (int i = 0; i < count; i++) {
        current_test = tests[i].name;
        tests_run++;
        int result = tests[i].fn();
        if (result == 0) {
            tests_passed++;
            printf("  PASS: %s\n", tests[i].name);
        } else {
            tests_failed++;
        }
    }
}

int main(void) {
    printf("=== arc_os host-side tests ===\n\n");

    run_suite("mem", mem_tests, mem_test_count);
    run_suite("pmm", pmm_tests, pmm_test_count);

    printf("\n=== Results: %d passed, %d failed, %d total ===\n",
           tests_passed, tests_failed, tests_run);

    return tests_failed > 0 ? 1 : 0;
}
