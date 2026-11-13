/* arc_os — Big kernel lock (M1-05, interim; see kernel_lock.h)
 *
 * Owns: the kernel lock word and its holder.
 * Invariants: held only with interrupts off; holder is the thread that took
 * it (NULL in an interrupt handler or when free); a thread holds it at most
 * once (no recursion); a thread that switches away gives it up first.
 * Locks: this is lock level 0 itself; it is taken with no other lock held
 * (taken back after a sleep, only spinlocks are ruled out).
 * Cost: one atomic exchange to take, one store to release. Contention costs
 * the waiting CPU's time, not other CPUs': user code never waits for it. */

#include "proc/kernel_lock.h"
#include "proc/lockdep.h"
#include "proc/thread.h"
#include "arch/hal_lock.h"
#include "lib/kprintf.h"

static volatile uint32_t kernel_lock_word;
static Thread *volatile kernel_lock_holder;

static int kernel_lock_try(void) {
    return __atomic_exchange_n(&kernel_lock_word, 1, __ATOMIC_ACQUIRE) == 0;
}

/* Spin for the lock word, then record the holder. */
static void kernel_lock_acquire(void) {
    while (!kernel_lock_try()) {
        /* Wait with interrupts let in: no spinlock is held, and a CPU that stays
         * deaf while it waits would miss halt IPIs (panic) and stall its timer */
        while (__atomic_load_n(&kernel_lock_word, __ATOMIC_RELAXED)) {
            hal_irq_window();
            hal_cpu_relax();
        }
    }
    Thread *t = thread_current();
    kernel_lock_holder = t;
    if (t != NULL) t->kernel_lock = 1;
}

void kernel_lock(void) {
    lockdep_kernel_lock(0);
    kernel_lock_acquire();
}

void kernel_lock_irq(void) {
    while (!kernel_lock_try()) {
        while (__atomic_load_n(&kernel_lock_word, __ATOMIC_RELAXED)) hal_cpu_relax();
    }
    kernel_lock_holder = NULL;
}

void kernel_unlock(void) {
    Thread *t = kernel_lock_holder;
    if (t != NULL) t->kernel_lock = 0;
    kernel_lock_holder = NULL;
    __atomic_store_n(&kernel_lock_word, 0, __ATOMIC_RELEASE);
}

int kernel_lock_held(void) {
    Thread *t = thread_current();
    return t != NULL && t->kernel_lock;
}

int kernel_lock_suspend(void) {
    if (!kernel_lock_held()) return 0;
    kernel_unlock();
    return 1;
}

void kernel_lock_resume(int was) {
    if (!was) return;
    lockdep_kernel_lock(1);   /* sleeping locks may be held across the sleep */
    kernel_lock_acquire();
}
