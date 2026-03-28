#include "proc/condvar.h"
#include "proc/sched.h"

void condvar_init(CondVar *cv) {
    wq_init(&cv->waiters);
}

void condvar_wait(CondVar *cv, Mutex *m) {
    /* Enqueue self on the condvar's wait queue, then release the mutex.
     * We use the condvar's internal spinlock (inside waiters) as the
     * sleep lock to get atomic enqueue-and-release semantics. */
    spinlock_acquire(&cv->waiters.lock);

    /* Append to wait queue */
    Thread *self = thread_current();
    self->next = NULL;
    if (cv->waiters.tail) {
        cv->waiters.tail->next = self;
    } else {
        cv->waiters.head = self;
    }
    cv->waiters.tail = self;
    self->state = THREAD_BLOCKED;

    spinlock_release(&cv->waiters.lock);

    /* Release the mutex — other threads can now modify the condition */
    mutex_unlock(m);

    /* Schedule away — returns when condvar_signal/broadcast wakes us */
    sched_yield();

    /* Re-acquire the mutex before returning to caller */
    mutex_lock(m);
}

void condvar_signal(CondVar *cv) {
    wq_wake(&cv->waiters);
}

void condvar_broadcast(CondVar *cv) {
    wq_wake_all(&cv->waiters);
}
