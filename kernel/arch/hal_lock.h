#ifndef ARCHOS_ARCH_HAL_LOCK_H
#define ARCHOS_ARCH_HAL_LOCK_H

/* The HAL pieces locks need (M1-05): interrupt flag save/restore, the spin
 * hint and this CPU's spinlock depth. Kept apart from hal.h so proc/spinlock.h
 * stays small, and so host tests (ARC_HOST_TEST) can run the real lock code
 * against a simulated interrupt flag and depth.
 *
 * The spinlock depth is the number of spinlocks this CPU holds. While it is
 * not zero the slice timer does not preempt (a preempted lock holder would
 * leave its CPU's next thread spinning), and debug builds refuse to sleep. */

#include <stdint.h>

#ifdef ARC_HOST_TEST

/* Host tests: each host thread is one simulated CPU. Defined in
 * tests/test_spinlock.c. */
extern _Thread_local int hal_test_irq_enabled;
extern _Thread_local uint32_t hal_test_lock_depth;

static inline uint64_t irq_save(void) {
    uint64_t flags = (uint64_t)hal_test_irq_enabled;
    hal_test_irq_enabled = 0;
    return flags;
}
static inline void irq_restore(uint64_t flags) {
    hal_test_irq_enabled = (int)flags;
}
static inline int hal_irqs_enabled(void) {
    return hal_test_irq_enabled;
}
static inline void hal_cpu_relax(void) {
}
static inline void hal_irq_window(void) {
}
static inline void hal_lock_depth_inc(void) {
    hal_test_lock_depth++;
}
static inline void hal_lock_depth_dec(void) {
    hal_test_lock_depth--;
}
static inline uint32_t hal_lock_depth(void) {
    return hal_test_lock_depth;
}

#else

#include "arch/x86_64/percpu.h"

/* Disable interrupts on this CPU and return the previous RFLAGS. */
static inline uint64_t irq_save(void) {
    uint64_t flags;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

/* Restore RFLAGS (and so the interrupt flag) saved by irq_save(). */
static inline void irq_restore(uint64_t flags) {
    __asm__ volatile("push %0; popf" : : "r"(flags) : "memory", "cc");
}

/* 1 if interrupts are enabled on this CPU. */
static inline int hal_irqs_enabled(void) {
    uint64_t flags;
    __asm__ volatile("pushf; pop %0" : "=r"(flags));
    return (flags & (1u << 9)) != 0;
}

/* Spin-wait hint for busy loops (PAUSE on x86). */
static inline void hal_cpu_relax(void) {
    __asm__ volatile("pause" ::: "memory");
}

/* With interrupts off: let pending interrupts in for one instruction, then
 * turn them off again (STI holds them off until after the next instruction). */
static inline void hal_irq_window(void) {
    __asm__ volatile("sti; nop; cli" ::: "memory");
}

/* This CPU's spinlock depth. One GS-relative instruction each, so an
 * interrupt on the same CPU can't split the update. */
static inline void hal_lock_depth_inc(void) {
    __asm__ volatile("incl %%gs:%c0" : : "i"(PERCPU_LOCK_DEPTH) : "memory", "cc");
}
static inline void hal_lock_depth_dec(void) {
    __asm__ volatile("decl %%gs:%c0" : : "i"(PERCPU_LOCK_DEPTH) : "memory", "cc");
}
static inline uint32_t hal_lock_depth(void) {
    uint32_t d;
    __asm__ volatile("movl %%gs:%c1, %0" : "=r"(d) : "i"(PERCPU_LOCK_DEPTH));
    return d;
}

#endif /* ARC_HOST_TEST */

#endif /* ARCHOS_ARCH_HAL_LOCK_H */
