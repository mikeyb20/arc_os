#ifndef ARCHOS_ARCH_X86_64_PERCPU_H
#define ARCHOS_ARCH_X86_64_PERCPU_H

#include <stdint.h>
#include "proc/thread.h"
#include "proc/spinlock.h"
#include "arch/x86_64/gdt.h"

/* Maximum CPUs supported */
#define MAX_CPUS 16

/* Offset of kernel_rsp in PerCpu — must match assembly */
#define PERCPU_KERNEL_RSP_OFFSET 24

/* Per-CPU data structure — one per processor */
typedef struct __attribute__((aligned(64))) {
    /* Identity */
    uint32_t cpu_id;
    uint32_t apic_id;

    /* Current thread/process */
    Thread  *current_thread;
    uint64_t kernel_rsp;        /* Kernel stack top for SYSCALL entry */

    /* Idle thread for this CPU */
    Thread  *idle_thread;

    /* Per-CPU GDT and TSS */
    GDTEntry gdt[7];
    TSS      tss;

    /* Per-CPU run queue */
    Thread  *run_queue_head;
    Thread  *run_queue_tail;
    Spinlock sched_lock;

    /* AP startup synchronization */
    volatile int online;
} PerCpu;

/* Global array of per-CPU data */
extern PerCpu percpu_data[MAX_CPUS];
extern uint32_t cpu_count;

/* Initialize per-CPU data for the BSP (CPU 0). */
void percpu_init_bsp(void);

/* Initialize per-CPU data for an AP. Called on the AP itself. */
void percpu_init_ap(uint32_t cpu_id, uint32_t apic_id);

/* Get the current CPU's PerCpu structure via GS base. */
static inline PerCpu *this_cpu(void) {
    PerCpu *p;
    __asm__ volatile (
        "mov %%gs:0, %0"  /* PerCpu starts with cpu_id, but we read the GS base */
        : "=r"(p)
    );
    /* Actually, we store the pointer to PerCpu in GS base directly.
     * Reading GS:0 gives the first 8 bytes of the struct starting at GS base.
     * Instead, read MSR_GS_BASE for the pointer. */
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000101));
    return (PerCpu *)((uint64_t)hi << 32 | lo);
}

/* Set GS base to point to the given PerCpu struct. */
static inline void percpu_set_gs_base(PerCpu *p) {
    uint64_t addr = (uint64_t)p;
    __asm__ volatile ("wrmsr" : : "c"(0xC0000101),
                      "a"((uint32_t)addr), "d"((uint32_t)(addr >> 32)));
}

#endif /* ARCHOS_ARCH_X86_64_PERCPU_H */
