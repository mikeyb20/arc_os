#include "arch/x86_64/syscall.h"
#include "arch/x86_64/msr.h"
#include "arch/x86_64/gdt.h"
#include "proc/process.h"
#include "proc/proc_syscalls.h"
#include "fs/fs_syscalls.h"
#include "fs/vfs.h"
#include "mm/mm_syscalls.h"
#include "net/net_syscalls.h"
#include "lib/kprintf.h"

/* Exported from syscall_entry.asm: user context saved during SYSCALL */
extern uint64_t syscall_saved_user_rip;
extern uint64_t syscall_saved_user_rsp;
extern uint64_t syscall_saved_user_rflags;
extern uint64_t syscall_saved_user_rbp;
extern uint64_t syscall_saved_user_rbx;
extern uint64_t syscall_saved_user_r12;
extern uint64_t syscall_saved_user_r13;
extern uint64_t syscall_saved_user_r14;
extern uint64_t syscall_saved_user_r15;
extern uint64_t syscall_saved_user_rdi;
extern uint64_t syscall_saved_user_rsi;
extern uint64_t syscall_saved_user_rdx;
extern uint64_t syscall_saved_user_r10;
extern uint64_t syscall_saved_user_r8;
extern uint64_t syscall_saved_user_r9;

/* RFLAGS bits cleared by SFMASK on SYSCALL entry */
#define RFLAGS_IF  (1ULL << 9)   /* Interrupt Flag */
#define RFLAGS_DF  (1ULL << 10)  /* Direction Flag */

/* Syscall handler table, indexed by Linux number. 512 slots, 4 KiB. */
static syscall_handler_t syscall_table[ARC_NR_SYSCALLS];

/* Kernel RSP for SYSCALL entry — set by scheduler on context switch */
uint64_t syscall_kernel_rsp;

/* fork_return_to_user (usermode.asm) reads ForkContext by these offsets */
_Static_assert(__builtin_offsetof(ForkContext, user_r15) == 64, "ForkContext layout");
_Static_assert(__builtin_offsetof(ForkContext, user_rdi) == 72, "ForkContext layout");
_Static_assert(__builtin_offsetof(ForkContext, user_r9) == 112, "ForkContext layout");

void syscall_save_user_context(struct ForkContext *ctx) {
    ctx->user_rip    = syscall_saved_user_rip;
    ctx->user_rsp    = syscall_saved_user_rsp;
    ctx->user_rflags = syscall_saved_user_rflags;
    ctx->user_rbp    = syscall_saved_user_rbp;
    ctx->user_rbx    = syscall_saved_user_rbx;
    ctx->user_r12    = syscall_saved_user_r12;
    ctx->user_r13    = syscall_saved_user_r13;
    ctx->user_r14    = syscall_saved_user_r14;
    ctx->user_r15    = syscall_saved_user_r15;
    ctx->user_rdi    = syscall_saved_user_rdi;
    ctx->user_rsi    = syscall_saved_user_rsi;
    ctx->user_rdx    = syscall_saved_user_rdx;
    ctx->user_r10    = syscall_saved_user_r10;
    ctx->user_r8     = syscall_saved_user_r8;
    ctx->user_r9     = syscall_saved_user_r9;
}

uint64_t syscall_user_rsp(void) {
    return syscall_saved_user_rsp;
}

/* --- Dispatcher --- */

int64_t syscall_dispatch(uint64_t num, uint64_t a0, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    if (num >= ARC_NR_SYSCALLS || syscall_table[num] == NULL) {
        return -ENOSYS;
    }
    return syscall_table[num](a0, a1, a2, a3, a4, a5);
}

/* --- Registration --- */

void syscall_register(uint32_t num, syscall_handler_t handler) {
    if (num < ARC_NR_SYSCALLS) {
        syscall_table[num] = handler;
    }
}

/* --- Initialization --- */

void syscall_init(void) {
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
    uint64_t star = ((uint64_t)(GDT_KERNEL_DATA | 3) << 48) | ((uint64_t)GDT_KERNEL_CODE << 32);
    wrmsr(MSR_STAR, star);

    /* 3. Set LSTAR = syscall entry point */
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);

    /* 4. Set SFMASK: clear IF (bit 9) and DF (bit 10) on SYSCALL entry */
    wrmsr(MSR_SFMASK, RFLAGS_IF | RFLAGS_DF);

    /* 5. Register handlers: each subsystem owns its syscalls */
    fs_syscalls_init();
    proc_syscalls_init();
    mm_syscalls_init();
    net_syscalls_init();

    kprintf("[SYSCALL] Initialized (LSTAR=0x%lx, STAR=0x%lx)\n",
            (uint64_t)syscall_entry, star);
}
