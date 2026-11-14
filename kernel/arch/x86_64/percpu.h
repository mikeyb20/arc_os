#ifndef ARCHOS_ARCH_X86_64_PERCPU_H
#define ARCHOS_ARCH_X86_64_PERCPU_H

#include <stdint.h>
#include <stddef.h>
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/cpu.h"
#include "proc/cpumask.h"

/* Per-CPU data, found through GS (M1-03).
 *
 * In the kernel, GS base points at the running CPU's PerCpu block and
 * this_cpu() is one GS-relative load. In user mode GS base holds the user's
 * value (always 0 today: no ARCH_SET_GS, no FSGSBASE) and KERNEL_GS_BASE holds
 * the block; every kernel entry from user mode (SYSCALL, interrupts, exceptions)
 * runs SWAPGS first and every return to user mode runs it last. NMI, #DF, #GP and
 * #MC can arrive with either value loaded, so they read MSR_GS_BASE instead: a
 * kernel address (sign bit set) means the kernel's is already there.
 *
 * Each AP's block sits in one 16 KiB area from the page allocator: three 4 KiB
 * IST stacks (#DF, NMI, #MC), then the PerCpu page. The BSP's are static. */

#define MAX_CPUS CPU_MAX

/* One CPU's TLB shootdown request (tlb.c): the range other CPUs flush, and
 * how many have not done it yet. */
typedef struct TlbRequest {
    uint64_t pgd;      /* address space (PML4 phys), or 0: kernel mappings, every CPU */
    uint64_t start;
    uint64_t end;
    uint32_t acks;     /* CPUs still to flush; the sender waits for 0 */
} TlbRequest;

#define PERCPU_AREA_ORDER 2                               /* 16 KiB */
#define PERCPU_AREA_SIZE  (4096u << PERCPU_AREA_ORDER)
#define PERCPU_IST_SIZE   4096u                           /* each IST stack */

/* Field offsets the assembly entry stubs use (syscall_entry.asm); checked by
 * _Static_assert in percpu.c */
#define PERCPU_SELF       0
#define PERCPU_KERNEL_RSP 8
#define PERCPU_USER_RSP   16
#define PERCPU_CURRENT    24
#define PERCPU_LOCK_DEPTH 32   /* hal_lock.h */

struct Thread;

/* Per-CPU interrupt counts kept by the arch handlers (M1-06) */
#define PERCPU_STAT_TIMER   0   /* local APIC timer */
#define PERCPU_STAT_RESCHED 1   /* reschedule IPIs */
#define PERCPU_STAT_TLB     2   /* TLB shootdown IPIs */
#define PERCPU_STAT_CALL    3   /* function-call IPIs */
#define PERCPU_STATS        4

/* One per CPU. The fields the entry paths and this_cpu() touch come first and
 * share one cache line that no other CPU writes. */
typedef struct __attribute__((aligned(64))) PerCpu {
    struct PerCpu *self;              /* this block: this_cpu() reads GS:0 */
    uint64_t kernel_rsp;              /* SYSCALL stack = current thread's kernel stack top */
    uint64_t user_rsp;                /* SYSCALL scratch: user RSP until it is pushed */
    struct Thread *current_thread;    /* thread running on this CPU */
    uint32_t lock_depth;              /* spinlocks held on this CPU (hal_lock.h) */
    uint32_t cpu_index;               /* 0 = BSP, then MADT order */
    uint32_t apic_id;                 /* 32-bit (x2APIC) local APIC ID */
    uint8_t core_type;                /* CPU_CORE_* from CPUID 0x1A (hybrid parts) */
    volatile uint8_t online;          /* set by the CPU itself once it is up */
    TSS *tss;                         /* this CPU's TSS (tss_storage) */
    void *sched;                      /* this CPU's run queue (proc/sched.c) */
    void *mm;                         /* user address space loaded here, or NULL (mm/mm.c) */
    void *timers;                     /* this CPU's timer queue (time/timer.c), NULL until set */
    uint64_t irq_stat[PERCPU_STATS];  /* per-CPU interrupt counts (PERCPU_STAT_*, /proc/interrupts) */
    TlbRequest tlb_req;               /* this CPU's TLB shootdown request (tlb.c) */
    CpuMask tlb_from;                 /* CPUs whose request this CPU still has to run (tlb.c) */

    GDTEntry gdt[GDT_ENTRY_COUNT];    /* this CPU's GDT (LTR marks its TSS busy) */
    TSS tss_storage;                  /* RSP0 + IST pointers */
} PerCpu;

/* Number of CPUs online (BSP + application processors that came up) */
extern uint32_t cpu_count;

/* This CPU's PerCpu block: one GS-relative load. Valid from percpu_init_bsp()
 * (BSP) or percpu_load() (AP) on, in kernel mode. Volatile because the answer
 * changes when a thread moves to another CPU (M1-05). */
static inline PerCpu *this_cpu(void) {
    PerCpu *p;
    __asm__ volatile("mov %%gs:0, %0" : "=r"(p));
    return p;
}

/* The running thread, read straight through GS. */
static inline struct Thread *this_cpu_thread(void) {
    struct Thread *t;
    __asm__ volatile("mov %%gs:%c1, %0" : "=r"(t) : "i"(PERCPU_CURRENT));
    return t;
}

/* Point GS at the BSP's (still empty) block, first thing at boot: spinlocks
 * count their depth through GS before percpu_init_bsp runs. */
void percpu_early_init(void);

/* Set up the BSP's block (index 0, static area): its GDT and TSS with IST
 * stacks, GS base, KERNEL_GS_BASE = 0. First thing after the boot info is
 * parsed: thread_current() reads GS. */
void percpu_init_bsp(void);

/* Allocate and fill the block for CPU index with the given APIC ID (BSP, while
 * starting the APs). Records the APIC ID in the APIC ID -> index map. Returns
 * NULL when out of memory or index >= MAX_CPUS. */
PerCpu *percpu_alloc(uint32_t index, uint32_t apic_id);

/* On the CPU itself: load its GDT and TSS, then point GS at its block and
 * record its core type. */
void percpu_load(PerCpu *cpu);

/* This CPU's index; 0 before the BSP's block exists (GS not set up yet), so
 * the panic path can call it at any point of boot. */
uint32_t percpu_current_index(void);

/* Block of CPU index, or NULL. */
PerCpu *percpu_get(uint32_t index);

/* Set the BSP's APIC ID (known once the APIC layer is up) and add it to the map. */
void percpu_set_bsp_apic_id(uint32_t apic_id);

/* CPU index for an APIC ID, or -1. O(log CPUs): binary search of a sorted map. */
int percpu_index_of_apic(uint32_t apic_id);

/* Point this CPU's kernel entry stacks at top: TSS.RSP0 (interrupts from user
 * mode) and the SYSCALL stack. Called on every context switch. */
static inline void percpu_set_kernel_stack(uint64_t top) {
    PerCpu *c = this_cpu();
    c->tss->rsp0 = top;
    c->kernel_rsp = top;
}

#endif /* ARCHOS_ARCH_X86_64_PERCPU_H */
