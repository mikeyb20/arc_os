/* arc_os — Host-side tests for signal implementation */

#include "test_framework.h"
#include <stdint.h>
#include <stddef.h>

/* --- Stubs for kernel dependencies --- */

/* Guard out kernel headers */
#define ARCHOS_LIB_MEM_H
#define ARCHOS_MM_KMALLOC_H
#define ARCHOS_PROC_SCHED_H
#define ARCHOS_PROC_THREAD_H
#define ARCHOS_PROC_PROCESS_H
#define ARCHOS_FS_VFS_H

/* Minimal types needed by signal.c */
typedef uint32_t tid_t;
typedef uint32_t pid_t;

/* Thread stub */
typedef struct Thread {
    tid_t   tid;
    uint8_t state;
} Thread;

#define THREAD_DEAD 4

/* Signal header — include it before Process to get SigState */
#define ARCHOS_PROC_SIGNAL_H  /* prevent double include */

/* Signal numbers */
#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGABRT   6
#define SIGKILL   9
#define SIGSEGV  11
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGCHLD  17
#define NSIG     32

typedef void (*sig_handler_t)(int);
#define SIG_DFL  ((sig_handler_t)0)
#define SIG_IGN  ((sig_handler_t)1)

/* User access stub — always valid in host tests */
static int user_ptr_valid(const void *ptr, size_t len) {
    return ptr != NULL && len > 0;
}
#define ARCHOS_USER_ACCESS_H

/* Error codes */
#define EINVAL  22
#define ESRCH    3

/* SignalFrame */
typedef struct {
    uint8_t  trampoline[16];
    uint64_t user_rip;
    uint64_t user_rsp;
    uint64_t user_rflags;
    uint64_t rax;
    uint64_t rbx;
    uint64_t rbp;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rdx;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t signo;
    uint64_t ret_addr;
} SignalFrame;

/* SyscallFrame */
typedef struct {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t rbp;
    uint64_t rbx;
    uint64_t rcx;
    uint64_t r11;
    uint64_t rsp;
} SyscallFrame;

/* SigState */
typedef struct {
    uint32_t      pending;
    sig_handler_t handlers[NSIG];
    uint8_t       restoring;
    SignalFrame   restore_frame;
} SigState;

/* Forward declarations matching signal.c public API */
void sig_init(SigState *ss);
int sig_send(uint32_t pid, int signo);
sig_handler_t sig_set_handler(SigState *ss, int signo, sig_handler_t handler);
int64_t sig_maybe_deliver(SyscallFrame *frame, int64_t syscall_ret);
extern uint64_t sig_pending_arg;

/* Process stub */
#define PROC_ALIVE       0
#define PROC_ZOMBIE      1
#define PROC_TERMINATED  2

typedef struct Process {
    pid_t           pid;
    uint8_t         state;
    int32_t         exit_status;
    Thread         *main_thread;
    SigState        sig;
    struct Process *parent;
    struct Process *next;
} Process;

/* Test process table */
#define MAX_TEST_PROCS 4
static Process test_procs[MAX_TEST_PROCS];
static Thread test_threads[MAX_TEST_PROCS];
static int test_current_idx = 0;
static int sched_schedule_called = 0;

/* memcpy/memset via libc */
static void *memcpy_impl(void *dst, const void *src, size_t n) {
    return memcpy(dst, src, n);
}
static void *memset_impl(void *s, int c, size_t n) {
    return memset(s, c, n);
}

/* Stubs */
static Process *proc_current(void) {
    return &test_procs[test_current_idx];
}

static Process *proc_get_by_pid(uint32_t pid) {
    for (int i = 0; i < MAX_TEST_PROCS; i++) {
        if (test_procs[i].pid == pid && test_procs[i].state != PROC_TERMINATED) {
            return &test_procs[i];
        }
    }
    return NULL;
}

static Thread *thread_current(void) {
    return &test_threads[test_current_idx];
}

static void sched_schedule(void) {
    sched_schedule_called = 1;
}

/* Reset test state */
static void test_reset(void) {
    memset(test_procs, 0, sizeof(test_procs));
    memset(test_threads, 0, sizeof(test_threads));
    for (int i = 0; i < MAX_TEST_PROCS; i++) {
        test_procs[i].pid = (uint32_t)i;
        test_procs[i].state = PROC_ALIVE;
        test_procs[i].main_thread = &test_threads[i];
        test_threads[i].tid = (uint32_t)i;
        sig_init(&test_procs[i].sig);
    }
    test_current_idx = 0;
    sched_schedule_called = 0;
    sig_pending_arg = 0;
}

/* Include signal.c directly */
#include "../kernel/proc/signal.c"

/* --- Tests --- */

TEST(init_clears_state) {
    SigState ss;
    memset(&ss, 0xFF, sizeof(ss));
    sig_init(&ss);
    ASSERT_EQ(ss.pending, 0);
    ASSERT_EQ(ss.restoring, 0);
    for (int i = 0; i < NSIG; i++) {
        ASSERT_EQ((uint64_t)ss.handlers[i], (uint64_t)SIG_DFL);
    }
    return 0;
}

TEST(send_sets_pending_bit) {
    test_reset();
    int ret = sig_send(0, SIGINT);
    ASSERT_EQ(ret, 0);
    ASSERT_TRUE(test_procs[0].sig.pending & (1u << SIGINT));
    return 0;
}

TEST(send_invalid_signo) {
    test_reset();
    ASSERT_EQ(sig_send(0, 0), -EINVAL);
    ASSERT_EQ(sig_send(0, NSIG), -EINVAL);
    ASSERT_EQ(sig_send(0, -1), -EINVAL);
    return 0;
}

TEST(send_nonexistent_pid) {
    test_reset();
    ASSERT_EQ(sig_send(99, SIGINT), -ESRCH);
    return 0;
}

TEST(set_handler_returns_old) {
    test_reset();
    SigState *ss = &test_procs[0].sig;
    sig_handler_t old = sig_set_handler(ss, SIGINT, SIG_IGN);
    ASSERT_EQ((uint64_t)old, (uint64_t)SIG_DFL);
    old = sig_set_handler(ss, SIGINT, SIG_DFL);
    ASSERT_EQ((uint64_t)old, (uint64_t)SIG_IGN);
    return 0;
}

TEST(set_handler_rejects_sigkill) {
    test_reset();
    SigState *ss = &test_procs[0].sig;
    sig_handler_t old = sig_set_handler(ss, SIGKILL, SIG_IGN);
    ASSERT_EQ((uint64_t)old, (uint64_t)SIG_DFL);
    /* Handler should remain SIG_DFL */
    ASSERT_EQ((uint64_t)ss->handlers[SIGKILL], (uint64_t)SIG_DFL);
    return 0;
}

TEST(default_sigterm_terminates) {
    test_reset();
    test_procs[0].sig.pending = (1u << SIGTERM);
    SyscallFrame frame = {0};
    frame.rsp = 0x7FFFFFFFE000ULL;  /* valid user stack */
    sig_maybe_deliver(&frame, 0);
    ASSERT_EQ(test_procs[0].state, PROC_ZOMBIE);
    ASSERT_EQ(test_procs[0].exit_status, 128 + SIGTERM);
    ASSERT_TRUE(sched_schedule_called);
    return 0;
}

TEST(default_sigchld_ignored) {
    test_reset();
    test_procs[0].sig.pending = (1u << SIGCHLD);
    SyscallFrame frame = {0};
    frame.rsp = 0x7FFFFFFFE000ULL;
    int64_t ret = sig_maybe_deliver(&frame, 42);
    ASSERT_EQ(ret, 42);
    ASSERT_EQ(test_procs[0].state, PROC_ALIVE);
    ASSERT_EQ(test_procs[0].sig.pending & (1u << SIGCHLD), 0);
    return 0;
}

TEST(sigkill_always_terminates) {
    test_reset();
    /* Set a handler for SIGKILL (should be ignored) */
    test_procs[0].sig.handlers[SIGKILL] = SIG_IGN;
    test_procs[0].sig.pending = (1u << SIGKILL);
    SyscallFrame frame = {0};
    frame.rsp = 0x7FFFFFFFE000ULL;
    sig_maybe_deliver(&frame, 0);
    ASSERT_EQ(test_procs[0].state, PROC_ZOMBIE);
    ASSERT_EQ(test_procs[0].exit_status, 128 + SIGKILL);
    return 0;
}

TEST(sig_ign_skips_signal) {
    test_reset();
    test_procs[0].sig.handlers[SIGINT] = SIG_IGN;
    test_procs[0].sig.pending = (1u << SIGINT);
    SyscallFrame frame = {0};
    frame.rsp = 0x7FFFFFFFE000ULL;
    int64_t ret = sig_maybe_deliver(&frame, 42);
    ASSERT_EQ(ret, 42);
    ASSERT_EQ(test_procs[0].state, PROC_ALIVE);
    ASSERT_EQ(test_procs[0].sig.pending & (1u << SIGINT), 0);
    return 0;
}

TEST(pending_cleared_after_delivery) {
    test_reset();
    test_procs[0].sig.pending = (1u << SIGTERM);
    SyscallFrame frame = {0};
    frame.rsp = 0x7FFFFFFFE000ULL;
    sig_maybe_deliver(&frame, 0);
    ASSERT_EQ(test_procs[0].sig.pending & (1u << SIGTERM), 0);
    return 0;
}

TEST(lowest_signal_first) {
    test_reset();
    /* Set both SIGCHLD (ignore) and SIGTERM (terminate) pending */
    test_procs[0].sig.pending = (1u << SIGTERM) | (1u << SIGINT);
    SyscallFrame frame = {0};
    frame.rsp = 0x7FFFFFFFE000ULL;
    /* SIGINT (2) < SIGTERM (15), so SIGINT delivered first — terminates */
    sig_maybe_deliver(&frame, 0);
    ASSERT_EQ(test_procs[0].exit_status, 128 + SIGINT);
    /* SIGTERM should still be pending */
    ASSERT_TRUE(test_procs[0].sig.pending & (1u << SIGTERM));
    return 0;
}

TEST(no_pending_returns_immediately) {
    test_reset();
    SyscallFrame frame = {0};
    frame.rsp = 0x7FFFFFFFE000ULL;
    int64_t ret = sig_maybe_deliver(&frame, 99);
    ASSERT_EQ(ret, 99);
    ASSERT_EQ(test_procs[0].state, PROC_ALIVE);
    return 0;
}

TEST(sigreturn_restores_context) {
    test_reset();
    SigState *ss = &test_procs[0].sig;
    ss->restoring = 1;
    ss->restore_frame.user_rip = 0x401000;
    ss->restore_frame.user_rsp = 0x7FFFFFFFD000ULL;
    ss->restore_frame.user_rflags = 0x202;
    ss->restore_frame.rax = 77;
    ss->restore_frame.rbx = 11;
    ss->restore_frame.rbp = 22;
    ss->restore_frame.r12 = 33;
    ss->restore_frame.r13 = 44;
    ss->restore_frame.r14 = 55;
    ss->restore_frame.r15 = 66;

    SyscallFrame frame = {0};
    int64_t ret = sig_maybe_deliver(&frame, 0);
    ASSERT_EQ(ret, 77);
    ASSERT_EQ(frame.rcx, 0x401000);
    ASSERT_EQ(frame.rsp, 0x7FFFFFFFD000ULL);
    ASSERT_EQ(frame.r11, 0x202);
    ASSERT_EQ(frame.rbx, 11);
    ASSERT_EQ(frame.rbp, 22);
    ASSERT_EQ(frame.r12, 33);
    ASSERT_EQ(frame.r13, 44);
    ASSERT_EQ(frame.r14, 55);
    ASSERT_EQ(frame.r15, 66);
    ASSERT_EQ(ss->restoring, 0);
    return 0;
}

static void dummy_handler(int signo) { (void)signo; }

TEST(user_handler_modifies_frame) {
    test_reset();
    test_procs[0].sig.handlers[SIGINT] = dummy_handler;
    test_procs[0].sig.pending = (1u << SIGINT);

    /* Use a real heap buffer as fake user stack so writes don't segfault */
    uint8_t *fake_stack = (uint8_t *)malloc(4096);
    ASSERT_TRUE(fake_stack != NULL);
    uint64_t stack_top = (uint64_t)(fake_stack + 4096);

    SyscallFrame frame = {0};
    frame.rcx = 0x401000;       /* original RIP */
    frame.rsp = stack_top;
    frame.r11 = 0x202;
    frame.rbx = 0xBB;
    frame.rbp = 0xBBBB;
    frame.r12 = 0x1212;
    frame.r13 = 0x1313;
    frame.r14 = 0x1414;
    frame.r15 = 0x1515;

    int64_t ret = sig_maybe_deliver(&frame, 42);

    /* Frame should be redirected to handler */
    ASSERT_EQ(frame.rcx, (uint64_t)dummy_handler);
    ASSERT_TRUE(frame.rsp < stack_top);  /* stack grew */
    ASSERT_EQ(sig_pending_arg, SIGINT);

    /* Callee-saved regs zeroed for handler */
    ASSERT_EQ(frame.rbx, 0);
    ASSERT_EQ(frame.rbp, 0);
    ASSERT_EQ(frame.r12, 0);
    ASSERT_EQ(frame.r13, 0);
    ASSERT_EQ(frame.r14, 0);
    ASSERT_EQ(frame.r15, 0);

    /* Check SignalFrame saved original context */
    SignalFrame *sf = (SignalFrame *)frame.rsp;
    ASSERT_EQ(sf->user_rip, 0x401000);
    ASSERT_EQ(sf->rax, 42);
    ASSERT_EQ(sf->rbx, 0xBB);
    ASSERT_EQ(sf->signo, SIGINT);

    (void)ret;
    free(fake_stack);
    return 0;
}

/* --- Suite --- */

TestCase signal_tests[] = {
    TEST_ENTRY(init_clears_state),
    TEST_ENTRY(send_sets_pending_bit),
    TEST_ENTRY(send_invalid_signo),
    TEST_ENTRY(send_nonexistent_pid),
    TEST_ENTRY(set_handler_returns_old),
    TEST_ENTRY(set_handler_rejects_sigkill),
    TEST_ENTRY(default_sigterm_terminates),
    TEST_ENTRY(default_sigchld_ignored),
    TEST_ENTRY(sigkill_always_terminates),
    TEST_ENTRY(sig_ign_skips_signal),
    TEST_ENTRY(pending_cleared_after_delivery),
    TEST_ENTRY(lowest_signal_first),
    TEST_ENTRY(no_pending_returns_immediately),
    TEST_ENTRY(sigreturn_restores_context),
    TEST_ENTRY(user_handler_modifies_frame),
};
int signal_test_count = sizeof(signal_tests) / sizeof(signal_tests[0]);
