/* arc_os — SYSCALL setup and dispatch
 *
 * Owns: the SYSCALL MSRs (per CPU), the handler table and the user context a
 * syscall saved (the SyscallFrame at the top of the thread's kernel stack).
 * Invariants: syscall_entry.asm builds the SyscallFrame at the very top of the
 * current thread's kernel stack, so it is found from thread_current() alone;
 * the table is written only at boot.
 * Locks: none. */

#include "arch/x86_64/syscall.h"
#include "arch/x86_64/msr.h"
#include "arch/x86_64/gdt.h"
#include "proc/process.h"
#include "proc/signal.h"
#include "proc/thread.h"
#include "proc/proc_syscalls.h"
#include "proc/kernel_lock.h"
#include "fs/fs_syscalls.h"
#include "fs/vfs.h"
#include "mm/mm_syscalls.h"
#include "net/net_syscalls.h"
#include "time/time_syscalls.h"
#include "lib/kprintf.h"

/* RFLAGS bits cleared by SFMASK on SYSCALL entry. TF too: a user who sets it
 * would otherwise take a debug trap on the first kernel instruction. */
#define RFLAGS_TF           (1ULL << 8)   /* Trap Flag */
#define RFLAGS_IF  (1ULL << 9)   /* Interrupt Flag */
#define RFLAGS_DF  (1ULL << 10)  /* Direction Flag */
#define RFLAGS_NT           (1ULL << 14)  /* Nested Task */
#define RFLAGS_AC           (1ULL << 18)  /* Alignment Check (SMAP override) */
#define SYSCALL_RFLAGS_MASK (RFLAGS_TF | RFLAGS_IF | RFLAGS_DF | RFLAGS_NT | RFLAGS_AC)

/* Syscall handler table, indexed by Linux number. 512 slots, 4 KiB. */
static syscall_handler_t syscall_table[ARC_NR_SYSCALLS];

/* syscall_entry.asm reads SyscallFrame by these offsets */
_Static_assert(__builtin_offsetof(SyscallFrame, ret_rcx) == 0, "SyscallFrame layout");
_Static_assert(__builtin_offsetof(SyscallFrame, ret_r11) == 8, "SyscallFrame layout");
_Static_assert(__builtin_offsetof(SyscallFrame, r9) == 16, "SyscallFrame layout");
_Static_assert(__builtin_offsetof(SyscallFrame, rcx) == 112, "SyscallFrame layout");
_Static_assert(__builtin_offsetof(SyscallFrame, r11) == 120, "SyscallFrame layout");
_Static_assert(__builtin_offsetof(SyscallFrame, rsp) == 128, "SyscallFrame layout");

/* fork_return_to_user (usermode.asm) reads ForkContext by these offsets */
_Static_assert(__builtin_offsetof(ForkContext, user_r15) == 64, "ForkContext layout");
_Static_assert(__builtin_offsetof(ForkContext, user_rdi) == 72, "ForkContext layout");
_Static_assert(__builtin_offsetof(ForkContext, user_r9) == 112, "ForkContext layout");

/* The SyscallFrame of the syscall the current thread is in */
static const SyscallFrame *syscall_frame(void) {
    return (const SyscallFrame *)(thread_current()->kernel_stack_top - sizeof(SyscallFrame));
}

void syscall_save_user_context(struct ForkContext *ctx) {
    const SyscallFrame *f = syscall_frame();
    ctx->user_rip = f->rcx;
    ctx->user_rsp = f->rsp;
    ctx->user_rflags = f->r11;
    ctx->user_rbp = f->rbp;
    ctx->user_rbx = f->rbx;
    ctx->user_r12 = f->r12;
    ctx->user_r13 = f->r13;
    ctx->user_r14 = f->r14;
    ctx->user_r15 = f->r15;
    ctx->user_rdi = f->rdi;
    ctx->user_rsi = f->rsi;
    ctx->user_rdx = f->rdx;
    ctx->user_r10 = f->r10;
    ctx->user_r8 = f->r8;
    ctx->user_r9 = f->r9;
}

uint64_t syscall_user_rsp(void) {
    return syscall_frame()->rsp;
}

/* --- Dispatcher --- */

/* Syscalls run under the kernel lock (proc/kernel_lock.h, M1-05): taken here,
 * released by syscall_exit_work, or by proc_enter_user for execve. */
int64_t syscall_dispatch(uint64_t num, uint64_t a0, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    kernel_lock();
    if (num >= ARC_NR_SYSCALLS || syscall_table[num] == NULL) {
        return -ENOSYS;
    }
    return syscall_table[num](a0, a1, a2, a3, a4, a5);
}

int64_t syscall_exit_work(SyscallFrame *frame, int64_t syscall_ret) {
    int64_t ret = sig_maybe_deliver(frame, syscall_ret);
    kernel_unlock();
    return ret;
}

/* --- Registration --- */

void syscall_register(uint32_t num, syscall_handler_t handler) {
    if (num < ARC_NR_SYSCALLS) {
        syscall_table[num] = handler;
    }
}

/* --- Initialization --- */

static uint64_t syscall_star(void) {
    return ((uint64_t)(GDT_KERNEL_DATA | 3) << 48) | ((uint64_t)GDT_KERNEL_CODE << 32);
}

void syscall_cpu_init(void) {
    /* 1. Enable SYSCALL/SYSRET in EFER */
    uint64_t efer = rdmsr(MSR_EFER);
    wrmsr(MSR_EFER, efer | EFER_SCE);

    /* 2. Set STAR MSR: kernel CS/SS and user CS/SS base
     *    STAR[47:32] = kernel CS (0x08); kernel SS = CS + 8 = 0x10
     *    STAR[63:48] = user base 0x10 | RPL 3; SYSRET loads CS = base+16 = 0x23, SS = base+8 = 0x1b
     *    This matches our GDT layout: 0x08=kcode, 0x10=kdata, 0x18=udata, 0x20=ucode.
     *    The RPL bits must be in STAR itself: Intel ORs 3 into both selectors, but AMD
     *    loads SS = STAR[63:48]+8 as is. Without them user code ran with SS = 0x18
     *    (RPL 0) on AMD, and the first IRETQ back to user mode raised #GP(0x18). */
    wrmsr(MSR_STAR, syscall_star());

    /* 3. Set LSTAR = syscall entry point */
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);

    /* 4. Set SFMASK: flags cleared on SYSCALL entry */
    wrmsr(MSR_SFMASK, SYSCALL_RFLAGS_MASK);
}

void syscall_init(void) {
    syscall_cpu_init();

    /* Register handlers: each subsystem owns its syscalls */
    fs_syscalls_init();
    proc_syscalls_init();
    mm_syscalls_init();
    net_syscalls_init();
    time_syscalls_init();

    kprintf("[SYSCALL] Initialized (LSTAR=0x%lx, STAR=0x%lx)\n", (uint64_t)syscall_entry, syscall_star());
}
