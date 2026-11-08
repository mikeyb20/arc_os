/* arc_os — Host-side tests for syscall dispatcher */

#include "test_framework.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* --- Test the dispatch logic in isolation (no kernel dependencies) --- */

/* Same size and numbers as the kernel (kernel/include/uapi/unistd.h, M0-05) */
#include "uapi/unistd.h"
#define SYSCALL_MAX ARC_NR_SYSCALLS

typedef int64_t (*syscall_handler_t)(uint64_t, uint64_t, uint64_t,
                                     uint64_t, uint64_t, uint64_t);

static syscall_handler_t test_syscall_table[SYSCALL_MAX];

static int64_t test_dispatch(uint64_t num, uint64_t a0, uint64_t a1, uint64_t a2,
                             uint64_t a3, uint64_t a4, uint64_t a5) {
    if (num >= SYSCALL_MAX || test_syscall_table[num] == NULL) {
        return -38; /* -ENOSYS */
    }
    return test_syscall_table[num](a0, a1, a2, a3, a4, a5);
}

static void test_register(uint32_t num, syscall_handler_t handler) {
    if (num < SYSCALL_MAX) {
        test_syscall_table[num] = handler;
    }
}

/* --- Test handlers --- */

static int64_t handler_add(uint64_t a, uint64_t b, uint64_t c,
                           uint64_t d, uint64_t e, uint64_t f) {
    (void)c; (void)d; (void)e; (void)f;
    return (int64_t)(a + b);
}

static int64_t handler_negate(uint64_t a, uint64_t b, uint64_t c,
                              uint64_t d, uint64_t e, uint64_t f) {
    (void)b; (void)c; (void)d; (void)e; (void)f;
    return -(int64_t)a;
}

static int64_t handler_six_args(uint64_t a0, uint64_t a1, uint64_t a2,
                                uint64_t a3, uint64_t a4, uint64_t a5) {
    return (int64_t)(a0 + a1 + a2 + a3 + a4 + a5);
}

/* --- Tests --- */

TEST(unknown_syscall_returns_enosys) {
    memset(test_syscall_table, 0, sizeof(test_syscall_table));
    ASSERT_EQ(test_dispatch(99, 0, 0, 0, 0, 0, 0), -38);
    return 0;
}

TEST(register_and_call) {
    memset(test_syscall_table, 0, sizeof(test_syscall_table));
    test_register(10, handler_add);
    ASSERT_EQ(test_dispatch(10, 3, 4, 0, 0, 0, 0), 7);
    return 0;
}

TEST(multiple_handlers) {
    memset(test_syscall_table, 0, sizeof(test_syscall_table));
    test_register(0, handler_add);
    test_register(1, handler_negate);
    ASSERT_EQ(test_dispatch(0, 10, 20, 0, 0, 0, 0), 30);
    ASSERT_EQ(test_dispatch(1, 5, 0, 0, 0, 0, 0), -5);
    return 0;
}

TEST(six_arguments_passed) {
    memset(test_syscall_table, 0, sizeof(test_syscall_table));
    test_register(7, handler_six_args);
    ASSERT_EQ(test_dispatch(7, 1, 2, 3, 4, 5, 6), 21);
    return 0;
}

TEST(max_syscall_number) {
    memset(test_syscall_table, 0, sizeof(test_syscall_table));
    ASSERT_EQ(SYSCALL_MAX, 512);
    ASSERT_EQ(test_dispatch(512, 0, 0, 0, 0, 0, 0), -38);
    ASSERT_EQ(test_dispatch((uint64_t)-1, 0, 0, 0, 0, 0, 0), -38);
    test_register(511, handler_add);
    ASSERT_EQ(test_dispatch(511, 100, 200, 0, 0, 0, 0), 300);
    test_register(512, handler_add);   /* out of range: ignored */
    ASSERT_EQ(test_dispatch(512, 1, 1, 0, 0, 0, 0), -38);
    return 0;
}

TEST(overwrite_handler) {
    memset(test_syscall_table, 0, sizeof(test_syscall_table));
    test_register(5, handler_add);
    ASSERT_EQ(test_dispatch(5, 1, 2, 0, 0, 0, 0), 3);
    test_register(5, handler_negate);
    ASSERT_EQ(test_dispatch(5, 10, 0, 0, 0, 0, 0), -10);
    return 0;
}

TEST(null_slot_returns_enosys) {
    memset(test_syscall_table, 0, sizeof(test_syscall_table));
    test_register(3, handler_add);
    ASSERT_EQ(test_dispatch(4, 0, 0, 0, 0, 0, 0), -38);
    return 0;
}

TEST(file_syscall_numbers_routed) {
    memset(test_syscall_table, 0, sizeof(test_syscall_table));
    test_register(__NR_lseek, handler_add);
    test_register(__NR_stat, handler_negate);
    test_register(__NR_mkdir, handler_add);
    test_register(__NR_getdents64, handler_negate);
    test_register(__NR_unlink, handler_add);
    ASSERT_EQ(test_dispatch(8, 10, 20, 0, 0, 0, 0), 30);       /* lseek */
    ASSERT_EQ(test_dispatch(4, 5, 0, 0, 0, 0, 0), -5);         /* stat */
    ASSERT_EQ(test_dispatch(83, 1, 2, 0, 0, 0, 0), 3);         /* mkdir */
    ASSERT_EQ(test_dispatch(217, 7, 0, 0, 0, 0, 0), -7);       /* getdents64 */
    ASSERT_EQ(test_dispatch(87, 100, 200, 0, 0, 0, 0), 300);   /* unlink */
    return 0;
}

TEST(fork_exec_wait_numbers_routed) {
    memset(test_syscall_table, 0, sizeof(test_syscall_table));
    test_register(__NR_fork, handler_add);
    test_register(__NR_execve, handler_negate);
    test_register(__NR_wait4, handler_add);
    ASSERT_EQ(test_dispatch(57, 3, 4, 0, 0, 0, 0), 7);         /* fork */
    ASSERT_EQ(test_dispatch(59, 42, 0, 0, 0, 0, 0), -42);      /* execve */
    ASSERT_EQ(test_dispatch(61, 50, 50, 0, 0, 0, 0), 100);     /* wait4 */
    return 0;
}

TEST(old_arc_numbers_are_not_linux_ones) {
    /* The pre-M0-05 numbers collide with different Linux calls (ADR-0001) */
    ASSERT_EQ(__NR_read, 0);
    ASSERT_EQ(__NR_write, 1);
    ASSERT_EQ(__NR_open, 2);
    ASSERT_EQ(__NR_exit, 60);
    ASSERT_EQ(__NR_getpid, 39);
    ASSERT_EQ(__NR_rt_sigreturn, 15);
    return 0;
}

TEST(unregistered_file_syscalls_return_enosys) {
    memset(test_syscall_table, 0, sizeof(test_syscall_table));
    ASSERT_EQ(test_dispatch(__NR_lseek, 0, 0, 0, 0, 0, 0), -38);
    ASSERT_EQ(test_dispatch(__NR_stat, 0, 0, 0, 0, 0, 0), -38);
    ASSERT_EQ(test_dispatch(__NR_mkdir, 0, 0, 0, 0, 0, 0), -38);
    ASSERT_EQ(test_dispatch(__NR_getdents64, 0, 0, 0, 0, 0, 0), -38);
    ASSERT_EQ(test_dispatch(__NR_mmap, 0, 0, 0, 0, 0, 0), -38);
    return 0;
}

/* --- Suite --- */

TestCase syscall_tests[] = {
    TEST_ENTRY(unknown_syscall_returns_enosys),
    TEST_ENTRY(register_and_call),
    TEST_ENTRY(multiple_handlers),
    TEST_ENTRY(six_arguments_passed),
    TEST_ENTRY(max_syscall_number),
    TEST_ENTRY(overwrite_handler),
    TEST_ENTRY(null_slot_returns_enosys),
    TEST_ENTRY(file_syscall_numbers_routed),
    TEST_ENTRY(fork_exec_wait_numbers_routed),
    TEST_ENTRY(old_arc_numbers_are_not_linux_ones),
    TEST_ENTRY(unregistered_file_syscalls_return_enosys),
};
int syscall_test_count = sizeof(syscall_tests) / sizeof(syscall_tests[0]);
