/* arc_os — Sleeping mutex
 *
 * Owns: owner tracking and the waiter queue of one mutex.
 * Invariants: owner is the thread that last took the mutex, or NULL.
 * Locks: m->guard (LOCK_LEVEL_OBJECT) protects owner and is the condition lock
 * passed to wq_sleep. The mutex itself is a sleeping lock at m->level: debug
 * builds check its order and that no spinlock is held when it is taken. */

#include "proc/mutex.h"
#include "proc/sched.h"

void mutex_init(Mutex *m, uint16_t level) {
    spin_lock_init(&m->guard, LOCK_LEVEL_OBJECT, "mutex guard");
    m->owner = NULL;
    m->level = level;
    wq_init(&m->waiters);
}

void mutex_lock(Mutex *m) {
    lockdep_sleep_acquire(m->level, "mutex");
    uint64_t flags;
    spin_lock_irqsave(&m->guard, &flags);
    while (m->owner != NULL) {
        /* Sleep until mutex_unlock; another thread may take it between the
         * wakeup and this thread taking the guard again, so loop */
        wq_sleep(&m->waiters, &m->guard, flags);
        spin_lock_irqsave(&m->guard, &flags);
    }
    m->owner = thread_current();
    spin_unlock_irqrestore(&m->guard, flags);
}

void mutex_unlock(Mutex *m) {
    uint64_t flags;
    spin_lock_irqsave(&m->guard, &flags);
    m->owner = NULL;
    /* Wake one waiter — they will set themselves as owner */
    wq_wake(&m->waiters);
    spin_unlock_irqrestore(&m->guard, flags);
    lockdep_sleep_release(m->level);
}

int mutex_trylock(Mutex *m) {
    uint64_t flags;
    spin_lock_irqsave(&m->guard, &flags);
    if (m->owner != NULL) {
        spin_unlock_irqrestore(&m->guard, flags);
        return -1;
    }
    m->owner = thread_current();
    spin_unlock_irqrestore(&m->guard, flags);
    lockdep_sleep_acquire(m->level, "mutex");
    return 0;
}
