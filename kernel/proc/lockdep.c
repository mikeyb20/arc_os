/* arc_os — Lock-order checker (M1-05, debug builds)
 *
 * Owns: the order rules of proc/lockdep.h. lockdep_check_order is the whole
 * rule and keeps no state, so the host tests run it as is; the rest records
 * which locks are held and panics with both locks named when a rule breaks.
 * Invariants: a CPU's held list holds the spinlocks taken on that CPU and not
 * yet released, in acquisition order (a spinlock is never held across a sleep
 * or a migration, so the list belongs to whichever thread runs there; the one
 * exception, the run-queue lock carried across a context switch, is taken and
 * released on the same CPU). A thread's sleeping list moves with the thread.
 * Locks: none. Each list is touched only by its own CPU or thread, with
 * interrupts off for the per-CPU list (an interrupt handler's locks nest on
 * top and are gone before it returns).
 * Cost: O(locks held) per acquisition and release, debug builds only. */

#include "proc/lockdep.h"
#include "proc/spinlock.h"

const char *lockdep_check_order(const LockdepCpu *cpu, const LockdepThread *thr, uint16_t level, uint16_t subkey) {
    int max_level = -1;
    uint16_t max_subkey = 0;
    if (thr != NULL) {
        for (uint32_t i = 0; i < thr->depth; i++) {
            if (thr->level[i] > max_level) max_level = thr->level[i];
        }
    }
    if (cpu != NULL) {
        for (uint32_t i = 0; i < cpu->depth; i++) {
            const Spinlock *h = cpu->held[i];
            if ((int)h->level > max_level || ((int)h->level == max_level && h->subkey > max_subkey)) {
                max_level = h->level;
                max_subkey = h->subkey;
            }
        }
    }
    if ((int)level > max_level) return NULL;
    if ((int)level == max_level && subkey != 0 && subkey > max_subkey) return NULL;
    return (int)level == max_level ? "same level, not in subkey order" : "lower level than a lock held";
}

const char *lockdep_check_kernel_lock(const LockdepCpu *cpu, const LockdepThread *thr, int resuming) {
    if (cpu != NULL && cpu->depth > 0) return "a spinlock is held";
    /* Taking it back after a sleep may happen under a sleeping lock: anyone
     * waiting for that lock sleeps too, and sleeping gives the kernel lock up */
    if (!resuming && thr != NULL && thr->depth > 0) return "a sleeping lock is held";
    return NULL;
}

#include "lib/kprintf.h"

void lockdep_test_inversion(void) {
    static Spinlock outer = SPINLOCK_INIT(LOCK_LEVEL_PAGE_TABLE, "test outer (page tables)");
    static Spinlock inner = SPINLOCK_INIT(LOCK_LEVEL_WAITQUEUE, "test inner (wait queue)");
    uint64_t f1, f2;
    kprintf("[TEST] lockorder: taking a level %u lock under a level %u one\n", LOCK_LEVEL_PAGE_TABLE,
            LOCK_LEVEL_WAITQUEUE);
    spin_lock_irqsave(&inner, &f1);
    spin_lock_irqsave(&outer, &f2);   /* inverted: a debug build stops here */
    spin_unlock_irqrestore(&outer, f2);
    spin_unlock_irqrestore(&inner, f1);
    kprintf("[TEST] lockorder: the lock checker is off in this build\n");
}

#if ARC_LOCKDEP

#include "proc/thread.h"
#include "proc/sched.h"

static volatile int lockdep_off;   /* set by the first report: the panic path takes locks too */

static void lockdep_dump_held(const LockdepCpu *cpu, const LockdepThread *thr) {
    if (cpu != NULL) {
        for (uint32_t i = 0; i < cpu->depth; i++) {
            const char *n = cpu->held[i]->name;
            kprintf("[LOCKDEP]   held: %s (level %u)\n", n ? n : "?", cpu->held[i]->level);
        }
    }
    if (thr != NULL) {
        for (uint32_t i = 0; i < thr->depth; i++) {
            kprintf("[LOCKDEP]   held: sleeping lock (level %u)\n", thr->level[i]);
        }
    }
}

static void lockdep_fail(const char *what, const char *name, uint16_t level, const char *reason) {
    if (__atomic_exchange_n(&lockdep_off, 1, __ATOMIC_SEQ_CST)) return;
    LockdepCpu *cpu = sched_this_lockdep();
    Thread *t = thread_current();
    kprintf("[LOCKDEP] LOCK ORDER VIOLATION: %s %s (level %u): %s\n", what, name ? name : "?", level, reason);
    lockdep_dump_held(cpu, t ? &t->lockdep : NULL);
    KERNEL_PANIC();
}

static int lockdep_active(void) {
    return !__atomic_load_n(&lockdep_off, __ATOMIC_RELAXED);
}

void lockdep_acquire(const Spinlock *lock, int trylock) {
    if (!lockdep_active()) return;
    LockdepCpu *cpu = sched_this_lockdep();
    if (cpu == NULL) return;   /* this CPU's run queue doesn't exist yet (early boot) */
    uint64_t flags = irq_save();
    Thread *t = thread_current();
    /* A trylock never waits, so it can't deadlock: record it, skip the order */
    const char *why = trylock ? NULL : lockdep_check_order(cpu, t ? &t->lockdep : NULL, lock->level, lock->subkey);
    if (why == NULL && cpu->depth >= LOCKDEP_MAX_HELD) why = "too many spinlocks held";
    if (why != NULL) {
        lockdep_fail("taking", lock->name, lock->level, why);
    } else {
        cpu->held[cpu->depth++] = lock;
    }
    irq_restore(flags);
}

void lockdep_release(const Spinlock *lock) {
    if (!lockdep_active()) return;
    LockdepCpu *cpu = sched_this_lockdep();
    if (cpu == NULL) return;
    uint64_t flags = irq_save();
    for (uint32_t i = cpu->depth; i-- > 0;) {   /* innermost first: the usual case is the top */
        if (cpu->held[i] != lock) continue;
        for (uint32_t j = i; j + 1 < cpu->depth; j++) cpu->held[j] = cpu->held[j + 1];
        cpu->depth--;
        irq_restore(flags);
        return;
    }
    irq_restore(flags);
    /* Not on this CPU's list: taken before the list existed (boot), else a bug */
}

void lockdep_assert_can_sleep(const char *what) {
    if (!lockdep_active() || hal_lock_depth() == 0) return;
    LockdepCpu *cpu = sched_this_lockdep();
    const char *name = (cpu != NULL && cpu->depth > 0) ? cpu->held[cpu->depth - 1]->name : "?";
    uint16_t level = (cpu != NULL && cpu->depth > 0) ? cpu->held[cpu->depth - 1]->level : 0;
    lockdep_fail(what, name, level, "sleeping while a spinlock is held");
}

void lockdep_sleep_acquire(uint16_t level, const char *name) {
    if (!lockdep_active()) return;
    lockdep_assert_can_sleep("taking sleeping lock");
    Thread *t = thread_current();
    if (t == NULL) return;
    const char *why = lockdep_check_order(NULL, &t->lockdep, level, 0);
    if (why == NULL && t->lockdep.depth >= LOCKDEP_MAX_SLEEP_HELD) why = "too many sleeping locks held";
    if (why != NULL) {
        lockdep_fail("taking", name, level, why);
        return;
    }
    t->lockdep.level[t->lockdep.depth++] = level;
}

void lockdep_sleep_release(uint16_t level) {
    if (!lockdep_active()) return;
    Thread *t = thread_current();
    if (t == NULL) return;
    for (uint32_t i = t->lockdep.depth; i-- > 0;) {
        if (t->lockdep.level[i] != level) continue;
        for (uint32_t j = i; j + 1 < t->lockdep.depth; j++) t->lockdep.level[j] = t->lockdep.level[j + 1];
        t->lockdep.depth--;
        return;
    }
}

void lockdep_kernel_lock(int resuming) {
    if (!lockdep_active()) return;
    Thread *t = thread_current();
    const char *why = lockdep_check_kernel_lock(sched_this_lockdep(), t != NULL ? &t->lockdep : NULL, resuming);
    if (why != NULL) lockdep_fail("taking", "kernel lock", LOCK_LEVEL_KERNEL, why);
}

#endif /* ARC_LOCKDEP */
