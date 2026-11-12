#ifndef ARCHOS_PROC_SPINLOCK_H
#define ARCHOS_PROC_SPINLOCK_H

/* Spinlocks (M1-05).
 *
 * The caller keeps the saved interrupt flags, never the lock (work-blocks §7):
 *
 *     uint64_t flags;
 *     spin_lock_irqsave(&a, &flags);
 *     ...
 *     spin_unlock_irqrestore(&a, flags);
 *
 * so two locks may be released in any order and interrupts come back on only
 * with the outermost release. Use spin_lock / spin_unlock only for a lock no
 * interrupt handler takes, or where interrupts are known to be off already.
 *
 * Every lock has a level from proc/lockdep.h (docs/kernel/locking.md); debug
 * builds check the order of every acquisition. Holding a spinlock also stops
 * the slice timer from preempting this CPU (hal_lock.h). */

#include <stdint.h>
#include "arch/hal_lock.h"
#include "proc/lockdep.h"

typedef struct Spinlock {
    volatile uint32_t locked;
    uint16_t level;         /* LOCK_LEVEL_* (lockdep.h) */
    uint16_t subkey;        /* order among same-level locks (run queues: CPU index + 1) */
    const char *name;       /* for lock-order reports */
} Spinlock;

_Static_assert(sizeof(Spinlock) == 16, "Spinlock is embedded in hot structures");

#define SPINLOCK_INIT(lvl, nm) \
    { .locked = 0, .level = (lvl), .subkey = 0, .name = (nm) }

/* Initialise a lock at run time (same as SPINLOCK_INIT). */
static inline void spin_lock_init(Spinlock *lock, uint16_t level, const char *name) {
    lock->locked = 0;
    lock->level = level;
    lock->subkey = 0;
    lock->name = name;
}

/* Take lock without touching the interrupt flag. */
static inline void spin_lock(Spinlock *lock) {
    lockdep_acquire(lock);
    hal_lock_depth_inc();
    while (__atomic_exchange_n(&lock->locked, 1, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&lock->locked, __ATOMIC_RELAXED)) hal_cpu_relax();
    }
}

/* Take lock if it is free. Returns 1 if taken. */
static inline int spin_trylock(Spinlock *lock) {
    if (__atomic_exchange_n(&lock->locked, 1, __ATOMIC_ACQUIRE)) return 0;
    lockdep_acquire(lock);
    hal_lock_depth_inc();
    return 1;
}

/* Release lock without touching the interrupt flag. */
static inline void spin_unlock(Spinlock *lock) {
    __atomic_store_n(&lock->locked, 0, __ATOMIC_RELEASE);
    hal_lock_depth_dec();
    lockdep_release(lock);
}

/* Disable interrupts, save the previous state in *flags, take lock. */
static inline void spin_lock_irqsave(Spinlock *lock, uint64_t *flags) {
    *flags = irq_save();
    spin_lock(lock);
}

/* Release lock, then restore the interrupt state saved by spin_lock_irqsave. */
static inline void spin_unlock_irqrestore(Spinlock *lock, uint64_t flags) {
    spin_unlock(lock);
    irq_restore(flags);
}

/* 1 if some CPU holds lock (a hint: it may change at once). */
static inline int spin_is_locked(const Spinlock *lock) {
    return __atomic_load_n(&lock->locked, __ATOMIC_RELAXED) != 0;
}

#endif /* ARCHOS_PROC_SPINLOCK_H */
