#include "proc/mutex.h"
#include "proc/sched.h"

void mutex_init(Mutex *m) {
    m->guard = (Spinlock)SPINLOCK_INIT;
    m->owner = NULL;
    wq_init(&m->waiters);
}

void mutex_lock(Mutex *m) {
    spinlock_acquire(&m->guard);

    while (m->owner != NULL) {
        /* Sleep until woken by mutex_unlock.  Re-check in loop because
         * another thread may acquire between our wakeup and guard re-acquire. */
        wq_sleep(&m->waiters, &m->guard);
        spinlock_acquire(&m->guard);
    }

    m->owner = thread_current();
    spinlock_release(&m->guard);
}

void mutex_unlock(Mutex *m) {
    spinlock_acquire(&m->guard);
    m->owner = NULL;
    /* Wake one waiter — they will set themselves as owner */
    wq_wake(&m->waiters);
    spinlock_release(&m->guard);
}

int mutex_trylock(Mutex *m) {
    spinlock_acquire(&m->guard);
    if (m->owner != NULL) {
        spinlock_release(&m->guard);
        return -1;
    }
    m->owner = thread_current();
    spinlock_release(&m->guard);
    return 0;
}
