/* arc_os — Host-side tests for kernel/proc/signal.c: actions, masks, default
 * actions, and the Linux rt_sigframe (M1-01) built and read back on a fake
 * user stack. Uses the real proc/signal.h and uapi headers; the scheduler,
 * process table, user copies and the arch FPU calls are stubs. */

#include "test_framework.h"
#include <stdint.h>
#include <stddef.h>

/* --- Stubs for kernel dependencies --- */

#define ARCHOS_LIB_MEM_H         /* host libc memset/memcpy */
#define ARCHOS_MM_KMALLOC_H
#define ARCHOS_PROC_SCHED_H
#define ARCHOS_PROC_SPINLOCK_H
#define ARCHOS_PROC_WAITQUEUE_H
#define ARCHOS_PROC_THREAD_H
#define ARCHOS_PROC_PROCESS_H
#define ARCHOS_FS_VFS_H
#define ARCHOS_ARCH_HAL_H
#define ARCHOS_USER_ACCESS_H
#define ARCHOS_PROC_PROC_SYSCALLS_H

#include "uapi/errno.h"
#include "uapi/sigcontext.h"
#include "proc/signal.h"

typedef uint32_t tid_t;
typedef uint32_t pid_t;

typedef struct Thread {
    tid_t   tid;
    uint8_t state;
} Thread;

#define THREAD_READY    1
#define THREAD_DEAD     4
#define THREAD_STOPPED  5

typedef struct WaitQueue {
    int dummy;
} WaitQueue;
static void wq_wake(WaitQueue *wq) { (void)wq; }

#define PROC_ALIVE       0
#define PROC_ZOMBIE      1
#define PROC_TERMINATED  2
#define PROC_STOPPED     3

typedef struct Process {
    pid_t           pid;
    pid_t           pgid;
    uint32_t uid;
    uint8_t         state;
    int32_t         exit_status;
    uint8_t         term_signal;
    Thread         *main_thread;
    SigState        sig;
    WaitQueue       child_exit_wq;
    struct Process *parent;
    struct Process *next;
} Process;

#define MAX_TEST_PROCS 4
static Process test_procs[MAX_TEST_PROCS];
static Thread test_threads[MAX_TEST_PROCS];
static int test_current_idx = 0;
static int sched_schedule_called = 0;

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

static int proc_foreach(void (*cb)(Process *p, void *ctx), void *ctx) {
    int count = 0;
    for (int i = 0; i < MAX_TEST_PROCS; i++) {
        if (test_procs[i].state != PROC_TERMINATED) {
            cb(&test_procs[i], ctx);
            count++;
        }
    }
    return count;
}

static int sched_remove_called = 0;
static void sched_remove_thread(Thread *t) { (void)t; sched_remove_called = 1; }
static int sched_add_called = 0;
static void sched_add_thread(Thread *t) {
    t->state = THREAD_READY;
    sched_add_called = 1;
}

/* User copies: the fake user stack is ordinary host memory; copy_fail makes
 * the next copy fail as for an unmapped page */
#define USER_ADDR_LIMIT 0x0000800000000000ULL
static int copy_fail;
static int copy_to_user(void *dst, const void *src, size_t len) {
    if (copy_fail) return -EFAULT;
    memcpy(dst, src, len);
    return 0;
}
static int copy_from_user(void *dst, const void *src, size_t len) {
    if (copy_fail) return -EFAULT;
    memcpy(dst, src, len);
    return 0;
}

/* Arch stubs (fpu.h, gdt.h through hal.h) */
#define GDT_USER_DATA   0x18
#define GDT_USER_CODE   0x20
#define TEST_XSAVE_SIZE 832
static int fpu_save_fail, fpu_restore_fail, fpu_init_loads;
static uint64_t fpu_saved_at, fpu_restored_from;
static size_t fpu_sigframe_size(void) {
    return TEST_XSAVE_SIZE + FP_XSTATE_MAGIC2_SIZE;
}
static int fpu_signal_save(Thread *t, uint64_t user_dst) {
    (void)t;
    if (fpu_save_fail) return -EFAULT;
    fpu_saved_at = user_dst;
    memset((void *)user_dst, 0xA5, fpu_sigframe_size());
    return 0;
}
static int fpu_signal_restore(Thread *t, uint64_t user_src) {
    (void)t;
    fpu_restored_from = user_src;
    return fpu_restore_fail ? -EFAULT : 0;
}
static void fpu_load_init(void) {
    fpu_init_loads++;
}

/* signal.c ends a process through proc_exit (proc_syscalls.c, which does not
 * return). This stub records the call and does what the real one does to the
 * process and its parent, then returns so the test can look. */
static int proc_exit_called;
static int32_t proc_exit_status;
static uint8_t proc_exit_signal;
static void proc_exit(struct Process *p, int32_t exit_status, uint8_t term_signal) {
    proc_exit_called++;
    proc_exit_status = exit_status;
    proc_exit_signal = term_signal;
    p->exit_status = exit_status;
    p->term_signal = term_signal;
    p->state = PROC_ZOMBIE;
    if (p->parent != NULL) sig_send(p->parent->pid, SIGCHLD);
    sched_schedule();
}

#include "../kernel/proc/signal.c"

static void test_reset(void) {
    memset(test_procs, 0, sizeof(test_procs));
    memset(test_threads, 0, sizeof(test_threads));
    for (int i = 0; i < MAX_TEST_PROCS; i++) {
        test_procs[i].pid = (uint32_t)i;
        test_procs[i].pgid = (uint32_t)i;
        test_procs[i].uid = 1000 + (uint32_t)i;
        test_procs[i].state = PROC_ALIVE;
        test_procs[i].main_thread = &test_threads[i];
        test_threads[i].tid = (uint32_t)i;
        test_threads[i].state = THREAD_READY;
        sig_init(&test_procs[i].sig);
    }
    test_current_idx = 0;
    sched_schedule_called = 0;
    sched_remove_called = 0;
    sched_add_called = 0;
    proc_exit_called = 0;
    copy_fail = 0;
    fpu_save_fail = 0;
    fpu_restore_fail = 0;
    fpu_init_loads = 0;
    fpu_saved_at = 0;
    fpu_restored_from = 0;
}

static void on_signal(int signo) {
    (void)signo;
}
static void restorer(void) {}

/* Install on_signal for signo on process 0 */
static void install(int signo, uint64_t flags, uint64_t mask) {
    struct kernel_sigaction act = {
        .sa_handler = (uint64_t)on_signal,
        .sa_flags = flags | SA_RESTORER,
        .sa_restorer = (uint64_t)restorer,
        .sa_mask = mask,
    };
    sig_set_action(&test_procs[0].sig, signo, &act, NULL);
}

/* A fake user stack and a frame of distinct register values on it */
#define STACK_BYTES 16384
static uint8_t fake_stack[STACK_BYTES] __attribute__((aligned(64)));

static SyscallFrame user_frame(void) {
    SyscallFrame f;
    f.rdi = 0xD1;
    f.rsi = 0x51;
    f.rdx = 0xD2;
    f.r10 = 0x10;
    f.r8 = 0x08;
    f.r9 = 0x09;
    f.r15 = 0x15;
    f.r14 = 0x14;
    f.r13 = 0x13;
    f.r12 = 0x12;
    f.rbp = 0xB9;
    f.rbx = 0xBB;
    f.rcx = 0x401000;                               /* user RIP */
    f.r11 = 0x246 | RFLAGS_DF;                      /* user RFLAGS, DF set */
    f.rsp = (uint64_t)(fake_stack + STACK_BYTES) - 72;   /* deliberately unaligned */
    return f;
}

/* --- Actions --- */

TEST(init_clears_state) {
    SigState ss;
    memset(&ss, 0xFF, sizeof(ss));
    sig_init(&ss);
    ASSERT_EQ(ss.pending, 0);
    ASSERT_EQ(ss.blocked, 0);
    ASSERT_EQ(ss.restoring, 0);
    for (int i = 0; i < NSIG; i++) {
        ASSERT_EQ(ss.action[i].sa_handler, (uint64_t)SIG_DFL);
        ASSERT_EQ(ss.action[i].sa_flags, 0);
    }
    return 0;
}

TEST(send_sets_pending_bit_and_sender) {
    test_reset();
    test_current_idx = 2;
    ASSERT_EQ(sig_send(0, SIGINT), 0);
    ASSERT_TRUE(test_procs[0].sig.pending & (1u << SIGINT));
    ASSERT_EQ(test_procs[0].sig.sender_pid[SIGINT], 2);
    ASSERT_EQ(test_procs[0].sig.sender_uid[SIGINT], 1002);
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

TEST(set_action_returns_old) {
    test_reset();
    SigState *ss = &test_procs[0].sig;
    struct kernel_sigaction act = {.sa_handler = (uint64_t)SIG_IGN, .sa_flags = SA_RESTART};
    struct kernel_sigaction old;
    memset(&old, 0x55, sizeof(old));
    ASSERT_EQ(sig_set_action(ss, SIGINT, &act, &old), 0);
    ASSERT_EQ(old.sa_handler, (uint64_t)SIG_DFL);
    ASSERT_EQ(old.sa_flags, 0);
    ASSERT_EQ(sig_set_action(ss, SIGINT, NULL, &old), 0);   /* query only */
    ASSERT_EQ(old.sa_handler, (uint64_t)SIG_IGN);
    ASSERT_EQ(old.sa_flags, SA_RESTART);
    return 0;
}

/* rt_sigaction(sig, &a, &a): the old action comes back, the new one sticks */
TEST(set_action_same_buffer) {
    test_reset();
    SigState *ss = &test_procs[0].sig;
    install(SIGUSR1, 0, 0);
    struct kernel_sigaction a = {.sa_handler = (uint64_t)SIG_IGN};
    ASSERT_EQ(sig_set_action(ss, SIGUSR1, &a, &a), 0);
    ASSERT_EQ(a.sa_handler, (uint64_t)on_signal);
    ASSERT_EQ(ss->action[SIGUSR1].sa_handler, (uint64_t)SIG_IGN);
    return 0;
}

TEST(set_action_rejects_kill_stop_and_bad_numbers) {
    test_reset();
    SigState *ss = &test_procs[0].sig;
    struct kernel_sigaction act = {.sa_handler = (uint64_t)SIG_IGN};
    struct kernel_sigaction old;
    ASSERT_EQ(sig_set_action(ss, SIGKILL, &act, NULL), -EINVAL);
    ASSERT_EQ(sig_set_action(ss, SIGSTOP, &act, NULL), -EINVAL);
    ASSERT_EQ(sig_set_action(ss, 0, &act, NULL), -EINVAL);
    ASSERT_EQ(sig_set_action(ss, NSIG, &act, NULL), -EINVAL);
    ASSERT_EQ(sig_set_action(ss, SIGKILL, NULL, &old), 0);   /* querying is fine */
    ASSERT_EQ(ss->action[SIGKILL].sa_handler, (uint64_t)SIG_DFL);
    return 0;
}

TEST(set_action_mask_drops_kill_and_stop) {
    test_reset();
    install(SIGUSR1, 0, SIGSET_BIT(SIGKILL) | SIGSET_BIT(SIGSTOP) | SIGSET_BIT(SIGUSR2));
    ASSERT_EQ(test_procs[0].sig.action[SIGUSR1].sa_mask, SIGSET_BIT(SIGUSR2));
    return 0;
}

TEST(fork_copy_keeps_actions_and_mask_not_pending) {
    test_reset();
    install(SIGUSR1, SA_NODEFER, 0);
    test_procs[0].sig.blocked = SIGSET_BIT(SIGUSR2);
    test_procs[0].sig.pending = 1u << SIGUSR2;
    test_procs[0].sig.restoring = 1;
    SigState child;
    memset(&child, 0x77, sizeof(child));
    sig_fork_copy(&child, &test_procs[0].sig);
    ASSERT_EQ(child.action[SIGUSR1].sa_handler, (uint64_t)on_signal);
    ASSERT_EQ(child.action[SIGUSR1].sa_flags, SA_NODEFER | SA_RESTORER);
    ASSERT_EQ(child.blocked, SIGSET_BIT(SIGUSR2));
    ASSERT_EQ(child.pending, 0);
    ASSERT_EQ(child.restoring, 0);
    return 0;
}

TEST(exec_reset_keeps_ignored_mask_and_pending) {
    test_reset();
    SigState *ss = &test_procs[0].sig;
    install(SIGUSR1, 0, 0);
    struct kernel_sigaction ign = {.sa_handler = (uint64_t)SIG_IGN, .sa_flags = SA_RESTART};
    sig_set_action(ss, SIGINT, &ign, NULL);
    ss->blocked = SIGSET_BIT(SIGTERM);
    ss->pending = 1u << SIGTERM;
    sig_exec_reset(ss);
    ASSERT_EQ(ss->action[SIGUSR1].sa_handler, (uint64_t)SIG_DFL);
    ASSERT_EQ(ss->action[SIGUSR1].sa_flags, 0);
    ASSERT_EQ(ss->action[SIGINT].sa_handler, (uint64_t)SIG_IGN);
    ASSERT_EQ(ss->action[SIGINT].sa_flags, 0);
    ASSERT_EQ(ss->blocked, SIGSET_BIT(SIGTERM));
    ASSERT_EQ(ss->pending, 1u << SIGTERM);
    return 0;
}

/* --- Default actions and masks --- */

TEST(default_sigterm_terminates) {
    test_reset();
    test_procs[0].sig.pending = (1u << SIGTERM);
    SyscallFrame frame = user_frame();
    sig_maybe_deliver(&frame, 0);
    ASSERT_EQ(test_procs[0].state, PROC_ZOMBIE);
    ASSERT_EQ(test_procs[0].exit_status, 128 + SIGTERM);
    ASSERT_TRUE(sched_schedule_called);
    return 0;
}

/* A signal death must look like exit to the parent: wait4 status says
 * WIFSIGNALED, parent gets SIGCHLD (and is woken; wq_wake is a stub here). */
TEST(sigterm_death_notifies_parent) {
    test_reset();
    test_current_idx = 1;
    test_procs[1].parent = &test_procs[0];
    test_procs[1].sig.pending = (1u << SIGTERM);
    SyscallFrame frame = user_frame();
    sig_maybe_deliver(&frame, 0);
    ASSERT_EQ(test_procs[1].state, PROC_ZOMBIE);
    ASSERT_EQ(test_procs[1].term_signal, SIGTERM);
    ASSERT_EQ(proc_exit_called, 1);
    ASSERT_EQ(proc_exit_status, 128 + SIGTERM);
    ASSERT_EQ(proc_exit_signal, SIGTERM);
    ASSERT_TRUE(test_procs[0].sig.pending & (1u << SIGCHLD));
    return 0;
}

TEST(default_ignored_signals) {
    int sigs[] = {SIGCHLD, SIGURG, SIGWINCH};
    for (unsigned i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
        test_reset();
        test_procs[0].sig.pending = (1u << sigs[i]);
        SyscallFrame frame = user_frame();
        ASSERT_EQ(sig_maybe_deliver(&frame, 42), 42);
        ASSERT_EQ(test_procs[0].state, PROC_ALIVE);
        ASSERT_EQ(test_procs[0].sig.pending, 0);
    }
    return 0;
}

TEST(default_stop_signals) {
    int sigs[] = {SIGTSTP, SIGTTIN, SIGTTOU};
    for (unsigned i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
        test_reset();
        test_procs[0].sig.pending = (1u << sigs[i]);
        SyscallFrame frame = user_frame();
        sig_maybe_deliver(&frame, 0);
        ASSERT_EQ(test_procs[0].state, PROC_STOPPED);
        ASSERT_EQ(test_threads[0].state, THREAD_STOPPED);
        ASSERT_TRUE(sched_schedule_called && sched_remove_called);
    }
    return 0;
}

TEST(sigkill_always_terminates) {
    test_reset();
    test_procs[0].sig.action[SIGKILL].sa_handler = (uint64_t)SIG_IGN;   /* forced in */
    test_procs[0].sig.blocked = ~0ULL;                                 /* forced in */
    test_procs[0].sig.pending = (1u << SIGKILL);
    SyscallFrame frame = user_frame();
    sig_maybe_deliver(&frame, 0);
    ASSERT_EQ(test_procs[0].state, PROC_ZOMBIE);
    ASSERT_EQ(test_procs[0].exit_status, 128 + SIGKILL);
    return 0;
}

TEST(sigstop_uncatchable) {
    test_reset();
    test_procs[0].sig.action[SIGSTOP].sa_handler = (uint64_t)SIG_IGN;
    test_procs[0].sig.blocked = ~0ULL;
    test_procs[0].sig.pending = (1u << SIGSTOP);
    SyscallFrame frame = user_frame();
    sig_maybe_deliver(&frame, 0);
    ASSERT_EQ(test_procs[0].state, PROC_STOPPED);
    return 0;
}

TEST(sig_ign_skips_signal) {
    test_reset();
    test_procs[0].sig.action[SIGINT].sa_handler = (uint64_t)SIG_IGN;
    test_procs[0].sig.pending = (1u << SIGINT);
    SyscallFrame frame = user_frame();
    ASSERT_EQ(sig_maybe_deliver(&frame, 42), 42);
    ASSERT_EQ(test_procs[0].state, PROC_ALIVE);
    ASSERT_EQ(test_procs[0].sig.pending & (1u << SIGINT), 0);
    return 0;
}

TEST(blocked_signal_stays_pending) {
    test_reset();
    test_procs[0].sig.blocked = SIGSET_BIT(SIGTERM);
    test_procs[0].sig.pending = (1u << SIGTERM);
    SyscallFrame frame = user_frame();
    ASSERT_EQ(sig_maybe_deliver(&frame, 7), 7);
    ASSERT_EQ(test_procs[0].state, PROC_ALIVE);
    ASSERT_TRUE(test_procs[0].sig.pending & (1u << SIGTERM));
    test_procs[0].sig.blocked = 0;
    sig_maybe_deliver(&frame, 7);
    ASSERT_EQ(test_procs[0].state, PROC_ZOMBIE);
    return 0;
}

TEST(lowest_signal_first) {
    test_reset();
    test_procs[0].sig.pending = (1u << SIGTERM) | (1u << SIGINT);
    SyscallFrame frame = user_frame();
    /* SIGINT (2) < SIGTERM (15), so SIGINT delivered first — terminates */
    sig_maybe_deliver(&frame, 0);
    ASSERT_EQ(test_procs[0].exit_status, 128 + SIGINT);
    ASSERT_TRUE(test_procs[0].sig.pending & (1u << SIGTERM));
    return 0;
}

TEST(no_pending_returns_immediately) {
    test_reset();
    SyscallFrame frame = user_frame();
    SyscallFrame before = frame;
    ASSERT_EQ(sig_maybe_deliver(&frame, 99), 99);
    ASSERT_EQ(memcmp(&frame, &before, sizeof(frame)), 0);
    ASSERT_EQ(test_procs[0].state, PROC_ALIVE);
    return 0;
}

/* --- The Linux rt_sigframe --- */

TEST(handler_frame_layout) {
    test_reset();
    test_current_idx = 0;
    install(SIGUSR1, 0, SIGSET_BIT(SIGUSR2));
    test_procs[0].sig.blocked = SIGSET_BIT(SIGHUP);
    test_current_idx = 3;
    sig_send(0, SIGUSR1);                     /* sent by pid 3, uid 1003 */
    test_current_idx = 0;

    SyscallFrame frame = user_frame();
    SyscallFrame old = frame;
    int64_t ret = sig_maybe_deliver(&frame, -4);
    ASSERT_EQ(ret, 0);                         /* RAX = 0 at handler entry */

    /* Registers at handler entry */
    uint64_t sf_addr = frame.rsp;
    struct rt_sigframe *sf = (struct rt_sigframe *)sf_addr;
    ASSERT_EQ(frame.rcx, (uint64_t)on_signal);
    ASSERT_EQ((frame.rsp + 8) % 16, 0);       /* as after a call */
    ASSERT_EQ(frame.rdi, SIGUSR1);
    ASSERT_EQ(frame.rsi, sf_addr + offsetof(struct rt_sigframe, info));
    ASSERT_EQ(frame.rdx, sf_addr + offsetof(struct rt_sigframe, uc));
    ASSERT_EQ(frame.r11 & RFLAGS_DF, 0);       /* DF cleared */
    ASSERT_EQ(frame.r11 & 0x200, 0x200);       /* IF kept */

    /* Placement: XSAVE image 64-byte aligned, below the red zone; the frame
     * below the image */
    uint64_t fp = sf->uc.uc_mcontext.fpstate;
    ASSERT_EQ(fp, fpu_saved_at);
    ASSERT_EQ(fp % 64, 0);
    ASSERT_TRUE(fp + fpu_sigframe_size() <= old.rsp - SIG_RED_ZONE);
    ASSERT_TRUE(sf_addr + sizeof(struct rt_sigframe) <= fp);
    ASSERT_TRUE(old.rsp - sf_addr <= SIG_RED_ZONE + sig_frame_max_size());

    /* Frame contents */
    ASSERT_EQ(sf->pretcode, (uint64_t)restorer);
    ASSERT_EQ(sf->uc.uc_flags, UC_FP_XSTATE | UC_SIGCONTEXT_SS | UC_STRICT_RESTORE_SS);
    ASSERT_EQ(sf->uc.uc_link, 0);
    ASSERT_EQ(sf->uc.uc_stack.ss_flags, SS_DISABLE);
    ASSERT_EQ(sf->uc.uc_sigmask, SIGSET_BIT(SIGHUP));
    const struct sigcontext *sc = &sf->uc.uc_mcontext;
    ASSERT_EQ(sc->rdi, 0xD1);
    ASSERT_EQ(sc->rsi, 0x51);
    ASSERT_EQ(sc->rdx, 0xD2);
    ASSERT_EQ(sc->r10, 0x10);
    ASSERT_EQ(sc->r8, 0x08);
    ASSERT_EQ(sc->r9, 0x09);
    ASSERT_EQ(sc->rbx, 0xBB);
    ASSERT_EQ(sc->rbp, 0xB9);
    ASSERT_EQ(sc->r12, 0x12);
    ASSERT_EQ(sc->r15, 0x15);
    ASSERT_EQ(sc->rax, (uint64_t)-4);
    ASSERT_EQ(sc->rip, 0x401000);
    ASSERT_EQ(sc->rsp, old.rsp);
    ASSERT_EQ(sc->eflags, old.r11);
    ASSERT_EQ(sc->cs, 0x23);
    ASSERT_EQ(sc->ss, 0x1B);
    ASSERT_EQ(sc->oldmask, SIGSET_BIT(SIGHUP));
    ASSERT_EQ(sf->info.si_signo, SIGUSR1);
    ASSERT_EQ(sf->info.si_code, SI_USER);
    ASSERT_EQ(sf->info.si_pid, 3);
    ASSERT_EQ(sf->info.si_uid, 1003);

    /* The handler runs with sa_mask and the signal itself blocked, and with a
     * clean FPU state */
    ASSERT_EQ(test_procs[0].sig.blocked, SIGSET_BIT(SIGHUP) | SIGSET_BIT(SIGUSR1) | SIGSET_BIT(SIGUSR2));
    ASSERT_EQ(fpu_init_loads, 1);
    ASSERT_EQ(test_procs[0].sig.pending, 0);
    return 0;
}

TEST(nodefer_and_resethand) {
    test_reset();
    install(SIGUSR1, SA_NODEFER | SA_RESETHAND, 0);
    test_procs[0].sig.pending = 1u << SIGUSR1;
    SyscallFrame frame = user_frame();
    ASSERT_EQ(sig_maybe_deliver(&frame, 0), 0);
    ASSERT_EQ(frame.rcx, (uint64_t)on_signal);
    ASSERT_EQ(test_procs[0].sig.blocked, 0);                     /* SA_NODEFER */
    ASSERT_EQ(test_procs[0].sig.action[SIGUSR1].sa_handler, (uint64_t)SIG_DFL);   /* SA_RESETHAND */
    return 0;
}

/* Each failure Linux answers with SIGSEGV: no SA_RESTORER, a handler address
 * SYSRET can't take, a stack that can't hold the frame */
static int deliver_expect_segv(void) {
    SyscallFrame frame = user_frame();
    test_procs[0].sig.pending = 1u << SIGUSR1;
    sig_maybe_deliver(&frame, 0);
    if (test_procs[0].state != PROC_ZOMBIE || test_procs[0].term_signal != SIGSEGV) return 1;
    return 0;
}

TEST(no_restorer_is_sigsegv) {
    test_reset();
    struct kernel_sigaction act = {.sa_handler = (uint64_t)on_signal};
    sig_set_action(&test_procs[0].sig, SIGUSR1, &act, NULL);
    ASSERT_EQ(deliver_expect_segv(), 0);
    return 0;
}

TEST(kernel_handler_address_is_sigsegv) {
    test_reset();
    install(SIGUSR1, 0, 0);
    test_procs[0].sig.action[SIGUSR1].sa_handler = 0xFFFF800000001000ULL;
    ASSERT_EQ(deliver_expect_segv(), 0);
    test_reset();
    install(SIGUSR1, 0, 0);
    test_procs[0].sig.action[SIGUSR1].sa_handler = SIG_USER_RIP_LIMIT;   /* first bad one */
    ASSERT_EQ(deliver_expect_segv(), 0);
    return 0;
}

TEST(unwritable_stack_is_sigsegv) {
    test_reset();
    install(SIGUSR1, 0, 0);
    copy_fail = 1;
    ASSERT_EQ(deliver_expect_segv(), 0);
    test_reset();
    install(SIGUSR1, 0, 0);
    fpu_save_fail = 1;
    ASSERT_EQ(deliver_expect_segv(), 0);
    return 0;
}

TEST(stack_too_low_is_sigsegv) {
    test_reset();
    install(SIGUSR1, 0, 0);
    SyscallFrame frame = user_frame();
    frame.rsp = 512;                           /* would wrap below 0 */
    test_procs[0].sig.pending = 1u << SIGUSR1;
    sig_maybe_deliver(&frame, 0);
    ASSERT_EQ(test_procs[0].term_signal, SIGSEGV);
    return 0;
}

/* --- rt_sigreturn --- */

/* Deliver SIGUSR1, then return through the frame the way the restorer does
 * (rt_sigreturn with RSP = frame + 8): every register, RAX and the mask come
 * back, the FPU image is read from fpstate */
TEST(sigreturn_round_trip) {
    test_reset();
    install(SIGUSR1, 0, SIGSET_BIT(SIGUSR2));
    test_procs[0].sig.blocked = SIGSET_BIT(SIGHUP);
    test_procs[0].sig.pending = 1u << SIGUSR1;
    SyscallFrame frame = user_frame();
    SyscallFrame old = frame;
    sig_maybe_deliver(&frame, 1234);
    uint64_t sf_addr = frame.rsp;
    uint64_t fp = ((struct rt_sigframe *)sf_addr)->uc.uc_mcontext.fpstate;

    /* The handler ran: registers are anything by now */
    memset(&frame, 0x3C, sizeof(frame));
    frame.r11 = 0x202;                         /* RFLAGS at the rt_sigreturn syscall */
    sig_request_return(&test_procs[0].sig, (sf_addr + 8) - sizeof(uint64_t));
    int64_t ret = sig_maybe_deliver(&frame, 0);

    ASSERT_EQ(ret, 1234);
    ASSERT_EQ(fpu_restored_from, fp);
    ASSERT_EQ(frame.rcx, old.rcx);
    ASSERT_EQ(frame.rsp, old.rsp);
    ASSERT_EQ(frame.rdi, old.rdi);
    ASSERT_EQ(frame.rsi, old.rsi);
    ASSERT_EQ(frame.rdx, old.rdx);
    ASSERT_EQ(frame.r10, old.r10);
    ASSERT_EQ(frame.r8, old.r8);
    ASSERT_EQ(frame.r9, old.r9);
    ASSERT_EQ(frame.rbx, old.rbx);
    ASSERT_EQ(frame.rbp, old.rbp);
    ASSERT_EQ(frame.r12, old.r12);
    ASSERT_EQ(frame.r13, old.r13);
    ASSERT_EQ(frame.r14, old.r14);
    ASSERT_EQ(frame.r15, old.r15);
    ASSERT_EQ(frame.r11, 0x202 | (old.r11 & SIG_FIX_EFLAGS));   /* ZF PF DF from the frame, IF kept */
    ASSERT_EQ(test_procs[0].sig.blocked, SIGSET_BIT(SIGHUP));
    ASSERT_EQ(test_procs[0].sig.restoring, 0);
    ASSERT_EQ(test_procs[0].state, PROC_ALIVE);
    return 0;
}

/* What a handler may change in the frame, and what rt_sigreturn refuses */
TEST(sigreturn_takes_edits_and_sanitizes_flags) {
    test_reset();
    install(SIGUSR1, 0, 0);
    test_procs[0].sig.pending = 1u << SIGUSR1;
    SyscallFrame frame = user_frame();
    sig_maybe_deliver(&frame, 0);
    uint64_t sf_addr = frame.rsp;
    struct ucontext *uc = &((struct rt_sigframe *)sf_addr)->uc;
    uc->uc_mcontext.rip = 0x402000;
    uc->uc_mcontext.rax = 5;
    uc->uc_mcontext.eflags = 0xFFFFFFFFULL;    /* IOPL, TF, IF ... all set */
    uc->uc_sigmask = ~0ULL;                    /* SIGKILL and SIGSTOP too */

    frame.r11 = 0x202;
    sig_request_return(&test_procs[0].sig, sf_addr);
    ASSERT_EQ(sig_maybe_deliver(&frame, 0), 5);
    ASSERT_EQ(frame.rcx, 0x402000);
    ASSERT_EQ(frame.r11, 0x202 | SIG_FIX_EFLAGS);   /* only the allowed bits */
    ASSERT_EQ(test_procs[0].sig.blocked, ~0ULL & ~SIG_UNBLOCKABLE);
    return 0;
}

static uint64_t test_rip_override;
static int sigreturn_expect_segv(void) {
    test_procs[0].sig.pending = 1u << SIGUSR1;
    SyscallFrame frame = user_frame();
    sig_maybe_deliver(&frame, 0);
    uint64_t sf_addr = frame.rsp;
    struct ucontext *uc = &((struct rt_sigframe *)sf_addr)->uc;
    if (test_rip_override) uc->uc_mcontext.rip = test_rip_override;
    sig_request_return(&test_procs[0].sig, sf_addr);
    sig_maybe_deliver(&frame, 0);
    if (test_procs[0].state != PROC_ZOMBIE || test_procs[0].term_signal != SIGSEGV) return 1;
    return 0;
}

TEST(corrupt_sigreturn_is_sigsegv) {
    /* Non-canonical / kernel RIP: SYSRET would fault in ring 0 */
    test_reset();
    install(SIGUSR1, 0, 0);
    test_rip_override = 0x0000800000000000ULL;
    ASSERT_EQ(sigreturn_expect_segv(), 0);
    test_rip_override = 0;
    /* XSAVE image fpu_signal_restore rejects */
    test_reset();
    install(SIGUSR1, 0, 0);
    fpu_restore_fail = 1;
    ASSERT_EQ(sigreturn_expect_segv(), 0);
    return 0;
}

TEST(sigreturn_unreadable_frame_is_sigsegv) {
    test_reset();
    SyscallFrame frame = user_frame();
    sig_request_return(&test_procs[0].sig, (uint64_t)fake_stack);
    copy_fail = 1;
    sig_maybe_deliver(&frame, 0);
    ASSERT_EQ(test_procs[0].term_signal, SIGSEGV);
    return 0;
}

/* A signal blocked during the handler is delivered as soon as rt_sigreturn
 * restores the mask, from the restored context */
TEST(sigreturn_then_pending_signal) {
    test_reset();
    install(SIGUSR1, 0, SIGSET_BIT(SIGUSR2));
    install(SIGUSR2, 0, 0);
    test_procs[0].sig.pending = 1u << SIGUSR1;
    SyscallFrame frame = user_frame();
    SyscallFrame old = frame;
    sig_maybe_deliver(&frame, 77);
    uint64_t first = frame.rsp;

    /* SIGUSR2 arrives while the SIGUSR1 handler runs: blocked */
    sig_send(0, SIGUSR2);
    SyscallFrame mid = frame;
    ASSERT_EQ(sig_maybe_deliver(&mid, 0), 0);
    ASSERT_TRUE(test_procs[0].sig.pending & (1u << SIGUSR2));

    sig_request_return(&test_procs[0].sig, first);
    ASSERT_EQ(sig_maybe_deliver(&frame, 0), 0);     /* straight into the SIGUSR2 handler */
    ASSERT_EQ(frame.rdi, SIGUSR2);
    struct rt_sigframe *sf = (struct rt_sigframe *)frame.rsp;
    ASSERT_EQ(sf->uc.uc_mcontext.rip, old.rcx);     /* saved: the original context */
    ASSERT_EQ(sf->uc.uc_mcontext.rax, 77);
    ASSERT_EQ(sf->uc.uc_sigmask, 0);
    ASSERT_EQ(test_procs[0].sig.pending, 0);
    return 0;
}

TEST(frame_max_size) {
    /* Linux's formula with an 832-byte image: 440 + 15 + 836 + 63 = 1354 -> 1360 */
    ASSERT_EQ(sig_frame_max_size(), 1360);
    return 0;
}

/* --- SIGCONT and groups --- */

TEST(sigcont_resumes_stopped) {
    test_reset();
    test_procs[1].state = PROC_STOPPED;
    test_threads[1].state = THREAD_STOPPED;
    sig_send(1, SIGCONT);
    ASSERT_EQ(test_procs[1].state, PROC_ALIVE);
    ASSERT_TRUE(sched_add_called);
    return 0;
}

TEST(sigcont_noop_if_not_stopped) {
    test_reset();
    sig_send(0, SIGCONT);
    ASSERT_EQ(test_procs[0].state, PROC_ALIVE);
    ASSERT_FALSE(sched_add_called);
    return 0;
}

TEST(send_group_delivers_to_matching) {
    test_reset();
    test_procs[0].pgid = 10;
    test_procs[1].pgid = 10;
    test_procs[2].pgid = 20;
    test_current_idx = 2;
    ASSERT_EQ(sig_send_group(10, SIGTERM), 0);
    ASSERT_TRUE(test_procs[0].sig.pending & (1u << SIGTERM));
    ASSERT_TRUE(test_procs[1].sig.pending & (1u << SIGTERM));
    ASSERT_FALSE(test_procs[2].sig.pending & (1u << SIGTERM));
    ASSERT_EQ(test_procs[1].sig.sender_pid[SIGTERM], 2);
    return 0;
}

TEST(send_group_empty_returns_esrch) {
    test_reset();
    ASSERT_EQ(sig_send_group(999, SIGTERM), -ESRCH);
    return 0;
}

/* --- Suite --- */

TestCase signal_tests[] = {
    TEST_ENTRY(init_clears_state),
    TEST_ENTRY(send_sets_pending_bit_and_sender),
    TEST_ENTRY(send_invalid_signo),
    TEST_ENTRY(send_nonexistent_pid),
    TEST_ENTRY(set_action_returns_old),
    TEST_ENTRY(set_action_same_buffer),
    TEST_ENTRY(set_action_rejects_kill_stop_and_bad_numbers),
    TEST_ENTRY(set_action_mask_drops_kill_and_stop),
    TEST_ENTRY(fork_copy_keeps_actions_and_mask_not_pending),
    TEST_ENTRY(exec_reset_keeps_ignored_mask_and_pending),
    TEST_ENTRY(default_sigterm_terminates),
    TEST_ENTRY(sigterm_death_notifies_parent),
    TEST_ENTRY(default_ignored_signals),
    TEST_ENTRY(default_stop_signals),
    TEST_ENTRY(sigkill_always_terminates),
    TEST_ENTRY(sigstop_uncatchable),
    TEST_ENTRY(sig_ign_skips_signal),
    TEST_ENTRY(blocked_signal_stays_pending),
    TEST_ENTRY(lowest_signal_first),
    TEST_ENTRY(no_pending_returns_immediately),
    TEST_ENTRY(handler_frame_layout),
    TEST_ENTRY(nodefer_and_resethand),
    TEST_ENTRY(no_restorer_is_sigsegv),
    TEST_ENTRY(kernel_handler_address_is_sigsegv),
    TEST_ENTRY(unwritable_stack_is_sigsegv),
    TEST_ENTRY(stack_too_low_is_sigsegv),
    TEST_ENTRY(sigreturn_round_trip),
    TEST_ENTRY(sigreturn_takes_edits_and_sanitizes_flags),
    TEST_ENTRY(corrupt_sigreturn_is_sigsegv),
    TEST_ENTRY(sigreturn_unreadable_frame_is_sigsegv),
    TEST_ENTRY(sigreturn_then_pending_signal),
    TEST_ENTRY(frame_max_size),
    TEST_ENTRY(sigcont_resumes_stopped),
    TEST_ENTRY(sigcont_noop_if_not_stopped),
    TEST_ENTRY(send_group_delivers_to_matching),
    TEST_ENTRY(send_group_empty_returns_esrch),
};
int signal_test_count = sizeof(signal_tests) / sizeof(signal_tests[0]);
