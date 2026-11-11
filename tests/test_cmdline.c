/* arc_os — Host-side tests for kernel/lib/cmdline.c and kernel/lib/testmode.c */

#include "test_framework.h"
#include <setjmp.h>
#include <stdint.h>

#include "../kernel/lib/cmdline.c"

/* --- testmode.c with the debug-exit port stubbed --- */

#define ARCHOS_ARCH_X86_64_DEBUG_EXIT_H
static jmp_buf  exit_jmp;
static int      exit_called;
static uint32_t exit_value;

/* Stand-in for the I/O port write: record the value and unwind, so the
 * halt loop after it in testmode_exit() is never reached on the host. */
static void hal_debug_exit(uint32_t value) {
    exit_called = 1;
    exit_value = value;
    longjmp(exit_jmp, 1);
}

#include "../kernel/lib/testmode.c"

/* --- cmdline_get --- */

TEST(get_value) {
    char v[32];
    ASSERT_EQ(cmdline_get("quiet arc.test=smoke foo=1", "arc.test", v, sizeof(v)), 1);
    ASSERT_STR_EQ(v, "smoke");
    return 0;
}

TEST(get_first_and_last) {
    char v[32];
    ASSERT_EQ(cmdline_get("a=1 b=2", "a", v, sizeof(v)), 1);
    ASSERT_STR_EQ(v, "1");
    ASSERT_EQ(cmdline_get("a=1 b=2", "b", v, sizeof(v)), 1);
    ASSERT_STR_EQ(v, "2");
    return 0;
}

TEST(get_bare_flag) {
    char v[8] = "x";
    ASSERT_EQ(cmdline_get("quiet debug", "debug", v, sizeof(v)), 1);
    ASSERT_STR_EQ(v, "");
    return 0;
}

TEST(get_missing) {
    char v[8];
    ASSERT_EQ(cmdline_get("arc.testing=1 xarc.test=2", "arc.test", v, sizeof(v)), 0);
    ASSERT_EQ(cmdline_get("", "arc.test", v, sizeof(v)), 0);
    ASSERT_EQ(cmdline_get(NULL, "arc.test", v, sizeof(v)), 0);
    return 0;
}

TEST(get_truncates) {
    char v[4];
    ASSERT_EQ(cmdline_get("k=abcdefgh", "k", v, sizeof(v)), 1);
    ASSERT_STR_EQ(v, "abc");
    return 0;
}

TEST(set_and_raw) {
    cmdline_set("arc.test=smoke");
    ASSERT_STR_EQ(cmdline_raw(), "arc.test=smoke");
    cmdline_set(NULL);
    ASSERT_STR_EQ(cmdline_raw(), "");
    return 0;
}

/* --- testmode --- */

TEST(testmode_off_by_default) {
    testmode_init("quiet");
    ASSERT_FALSE(testmode_active());
    ASSERT_STR_EQ(testmode_suite(), "");
    testmode_init("arc.test=");
    ASSERT_FALSE(testmode_active());
    return 0;
}

TEST(testmode_on) {
    testmode_init("arc.test=userland");
    ASSERT_TRUE(testmode_active());
    ASSERT_STR_EQ(testmode_suite(), "userland");
    return 0;
}

TEST(init_exit_ends_run) {
    testmode_init("arc.test=smoke");
    testmode_set_init_pid(1);
    exit_called = 0;
    if (setjmp(exit_jmp) == 0) {
        testmode_process_exited(2, 0);   /* not init: ignored */
        ASSERT_EQ(exit_called, 0);
        testmode_process_exited(1, 3);
    }
    ASSERT_EQ(exit_called, 1);
    ASSERT_EQ(exit_value, 3);
    return 0;
}

TEST(init_exit_status_clamped) {
    testmode_init("arc.test=smoke");
    testmode_set_init_pid(1);
    exit_called = 0;
    if (setjmp(exit_jmp) == 0) {
        testmode_process_exited(1, 200);
    }
    ASSERT_EQ(exit_called, 1);
    ASSERT_EQ(exit_value, 0x3e);
    return 0;
}

TEST(init_exit_ignored_outside_test_mode) {
    testmode_init("");
    testmode_set_init_pid(1);
    exit_called = 0;
    testmode_process_exited(1, 0);
    ASSERT_EQ(exit_called, 0);
    return 0;
}

static int end_hook_calls;
static void count_end_hook(void) { end_hook_calls++; }

TEST(end_hook_runs_once_before_exit) {
    testmode_init("arc.test=smoke");
    testmode_set_init_pid(1);
    testmode_set_end_hook(count_end_hook);
    end_hook_calls = 0;
    exit_called = 0;
    if (setjmp(exit_jmp) == 0) {
        testmode_process_exited(2, 0);   /* not init: no hook */
        ASSERT_EQ(end_hook_calls, 0);
        testmode_process_exited(1, 0);
    }
    ASSERT_EQ(end_hook_calls, 1);
    ASSERT_EQ(exit_called, 1);
    /* A panic ends the run through testmode_exit, which skips the hook */
    end_hook_calls = 0;
    if (setjmp(exit_jmp) == 0) {
        testmode_exit(TESTMODE_PANIC_CODE);
    }
    ASSERT_EQ(end_hook_calls, 0);
    testmode_set_end_hook(NULL);
    testmode_init("");
    return 0;
}

TestCase cmdline_tests[] = {
    TEST_ENTRY(get_value),
    TEST_ENTRY(get_first_and_last),
    TEST_ENTRY(get_bare_flag),
    TEST_ENTRY(get_missing),
    TEST_ENTRY(get_truncates),
    TEST_ENTRY(set_and_raw),
    TEST_ENTRY(testmode_off_by_default),
    TEST_ENTRY(testmode_on),
    TEST_ENTRY(init_exit_ends_run),
    TEST_ENTRY(init_exit_status_clamped),
    TEST_ENTRY(init_exit_ignored_outside_test_mode),
    TEST_ENTRY(end_hook_runs_once_before_exit),
};
int cmdline_test_count = sizeof(cmdline_tests) / sizeof(cmdline_tests[0]);
