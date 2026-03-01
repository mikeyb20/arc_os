#ifndef ARCHOS_PROC_SPINLOCK_H
#define ARCHOS_PROC_SPINLOCK_H

#include <stdint.h>

typedef struct {
    volatile uint32_t locked;
    uint64_t saved_flags;
} Spinlock;

#define SPINLOCK_INIT { .locked = 0, .saved_flags = 0 }

/* Acquire spinlock: save flags, disable interrupts, spin until acquired. */
static inline void spinlock_acquire(Spinlock *lock) {
    uint64_t flags;
    __asm__ volatile ("pushf; pop %0; cli" : "=r"(flags));
    while (__sync_lock_test_and_set(&lock->locked, 1)) {
        __asm__ volatile ("pause");
    }
    lock->saved_flags = flags;
}

/* Release spinlock: release lock, restore saved interrupt flags. */
static inline void spinlock_release(Spinlock *lock) {
    uint64_t flags = lock->saved_flags;
    __sync_lock_release(&lock->locked);
    __asm__ volatile ("push %0; popf" : : "r"(flags) : "memory", "cc");
}

#endif /* ARCHOS_PROC_SPINLOCK_H */
