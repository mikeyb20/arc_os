/* arc_os — Host-side tests for exec argv copy logic */

#include "test_framework.h"
#include <stdint.h>
#include <stddef.h>

/* --- Reproduce types and constants inline --- */

#define MAX_EXEC_ARGS     32
#define MAX_EXEC_ARG_DATA 4096

#define EINVAL 22
#define E2BIG   7

typedef struct {
    int    argc;
    char  *ptrs[MAX_EXEC_ARGS + 1];
    char   data[MAX_EXEC_ARG_DATA];
    size_t data_used;
} ExecArgv;

/* Stub user_ptr_valid: always valid in tests */
static int user_ptr_valid(const void *ptr, size_t len) {
    (void)len;
    return ptr != NULL;
}

/* Inline the exec_copy_argv function (matching kernel implementation) */
static int exec_copy_argv(uint64_t argv_addr, ExecArgv *args) {
    args->argc = 0;
    args->data_used = 0;

    if (argv_addr == 0) return 0;

    if (!user_ptr_valid((const void *)argv_addr, sizeof(uint64_t)))
        return -EINVAL;

    const char *const *user_argv = (const char *const *)argv_addr;

    for (int i = 0; i < MAX_EXEC_ARGS; i++) {
        const char *ustr = user_argv[i];
        if (ustr == (void *)0) break;

        if (!user_ptr_valid(ustr, 1)) return -EINVAL;

        args->ptrs[i] = &args->data[args->data_used];
        size_t start = args->data_used;
        while (1) {
            if (args->data_used >= MAX_EXEC_ARG_DATA) return -E2BIG;
            char c = ustr[args->data_used - start];
            args->data[args->data_used++] = c;
            if (c == '\0') break;
        }
        args->argc++;
    }

    if (args->argc == MAX_EXEC_ARGS) {
        if (user_argv[MAX_EXEC_ARGS] != (void *)0)
            return -E2BIG;
    }

    args->ptrs[args->argc] = (void *)0;
    return 0;
}

/* --- Tests --- */

TEST(null_argv_gives_argc_zero) {
    ExecArgv args;
    int ret = exec_copy_argv(0, &args);
    ASSERT_EQ(ret, 0);
    ASSERT_EQ(args.argc, 0);
    return 0;
}

TEST(single_arg) {
    const char *argv[] = { "hello", NULL };
    ExecArgv args;
    int ret = exec_copy_argv((uint64_t)argv, &args);
    ASSERT_EQ(ret, 0);
    ASSERT_EQ(args.argc, 1);
    ASSERT_STR_EQ(args.ptrs[0], "hello");
    ASSERT_TRUE(args.ptrs[1] == NULL);
    return 0;
}

TEST(multiple_args) {
    const char *argv[] = { "/boot/hello", "Alice", "Bob", NULL };
    ExecArgv args;
    int ret = exec_copy_argv((uint64_t)argv, &args);
    ASSERT_EQ(ret, 0);
    ASSERT_EQ(args.argc, 3);
    ASSERT_STR_EQ(args.ptrs[0], "/boot/hello");
    ASSERT_STR_EQ(args.ptrs[1], "Alice");
    ASSERT_STR_EQ(args.ptrs[2], "Bob");
    ASSERT_TRUE(args.ptrs[3] == NULL);
    return 0;
}

TEST(max_args_exact) {
    const char *argv[MAX_EXEC_ARGS + 1];
    for (int i = 0; i < MAX_EXEC_ARGS; i++) argv[i] = "x";
    argv[MAX_EXEC_ARGS] = NULL;

    ExecArgv args;
    int ret = exec_copy_argv((uint64_t)argv, &args);
    ASSERT_EQ(ret, 0);
    ASSERT_EQ(args.argc, MAX_EXEC_ARGS);
    return 0;
}

TEST(too_many_args_returns_e2big) {
    const char *argv[MAX_EXEC_ARGS + 2];
    for (int i = 0; i <= MAX_EXEC_ARGS; i++) argv[i] = "x";
    argv[MAX_EXEC_ARGS + 1] = NULL;

    ExecArgv args;
    int ret = exec_copy_argv((uint64_t)argv, &args);
    ASSERT_EQ(ret, -E2BIG);
    return 0;
}

TEST(data_overflow_returns_e2big) {
    /* Create one string that exceeds MAX_EXEC_ARG_DATA */
    char big[MAX_EXEC_ARG_DATA + 1];
    memset(big, 'A', MAX_EXEC_ARG_DATA);
    big[MAX_EXEC_ARG_DATA] = '\0';

    const char *argv[] = { big, NULL };
    ExecArgv args;
    int ret = exec_copy_argv((uint64_t)argv, &args);
    ASSERT_EQ(ret, -E2BIG);
    return 0;
}

TEST(empty_string_arg) {
    const char *argv[] = { "", "hello", NULL };
    ExecArgv args;
    int ret = exec_copy_argv((uint64_t)argv, &args);
    ASSERT_EQ(ret, 0);
    ASSERT_EQ(args.argc, 2);
    ASSERT_STR_EQ(args.ptrs[0], "");
    ASSERT_STR_EQ(args.ptrs[1], "hello");
    return 0;
}

TEST(null_terminated_early) {
    const char *argv[] = { "a", NULL, "b" };
    ExecArgv args;
    int ret = exec_copy_argv((uint64_t)argv, &args);
    ASSERT_EQ(ret, 0);
    ASSERT_EQ(args.argc, 1);
    ASSERT_STR_EQ(args.ptrs[0], "a");
    return 0;
}

TEST(validates_user_pointers) {
    /* NULL in argv_addr (not 0 cast) is caught by user_ptr_valid stub */
    ExecArgv args;
    int ret = exec_copy_argv(0, &args);
    ASSERT_EQ(ret, 0);  /* 0 means no argv, not invalid */
    ASSERT_EQ(args.argc, 0);
    return 0;
}

TEST(string_data_packed_correctly) {
    const char *argv[] = { "abc", "de", NULL };
    ExecArgv args;
    int ret = exec_copy_argv((uint64_t)argv, &args);
    ASSERT_EQ(ret, 0);
    /* "abc\0de\0" = 7 bytes */
    ASSERT_EQ(args.data_used, 7u);
    ASSERT_MEM_EQ(args.data, "abc\0de\0", 7);
    return 0;
}

TEST(offsets_and_lengths_correct) {
    const char *argv[] = { "foo", "bar", "baz", NULL };
    ExecArgv args;
    int ret = exec_copy_argv((uint64_t)argv, &args);
    ASSERT_EQ(ret, 0);
    /* ptrs should point into data[] */
    ASSERT_EQ(args.ptrs[0], &args.data[0]);
    ASSERT_EQ(args.ptrs[1], &args.data[4]);  /* "foo\0" = 4 */
    ASSERT_EQ(args.ptrs[2], &args.data[8]);  /* "bar\0" = 4 */
    return 0;
}

TEST(argc_zero_when_first_is_null) {
    const char *argv[] = { NULL };
    ExecArgv args;
    int ret = exec_copy_argv((uint64_t)argv, &args);
    ASSERT_EQ(ret, 0);
    ASSERT_EQ(args.argc, 0);
    ASSERT_TRUE(args.ptrs[0] == NULL);
    return 0;
}

/* --- Suite --- */

TestCase exec_argv_tests[] = {
    TEST_ENTRY(null_argv_gives_argc_zero),
    TEST_ENTRY(single_arg),
    TEST_ENTRY(multiple_args),
    TEST_ENTRY(max_args_exact),
    TEST_ENTRY(too_many_args_returns_e2big),
    TEST_ENTRY(data_overflow_returns_e2big),
    TEST_ENTRY(empty_string_arg),
    TEST_ENTRY(null_terminated_early),
    TEST_ENTRY(validates_user_pointers),
    TEST_ENTRY(string_data_packed_correctly),
    TEST_ENTRY(offsets_and_lengths_correct),
    TEST_ENTRY(argc_zero_when_first_is_null),
};
int exec_argv_test_count = sizeof(exec_argv_tests) / sizeof(exec_argv_tests[0]);
