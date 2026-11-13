/* arc_os — Wait queues
 *
 * Owns: FIFO lists of BLOCKED threads waiting for a condition.
 * Invariants: a thread is on at most one wait queue, linked through
 * Thread.next, and only while it is BLOCKED or on its way to sleep; the waker
 * that unlinks it is the one that makes it runnable (sched_wake).
 * Locks: wq->lock (LOCK_LEVEL_WAITQUEUE, IRQ-safe: interrupt handlers wake
 * sleepers). Taken under the caller's condition lock in wq_sleep; the run-queue
 * lock is taken under it in a wakeup.
 * Cost: sleep and wake-one are O(1); wake-all is O(sleepers). */

#include "proc/waitqueue.h"
#include "proc/sched.h"

void wq_init(WaitQueue *wq) {
    spin_lock_init(&wq->lock, LOCK_LEVEL_WAITQUEUE, "wait queue");
    wq->head = NULL;
    wq->tail = NULL;
}

void wq_sleep(WaitQueue *wq, Spinlock *lock, uint64_t flags) {
    Thread *self = thread_current();

    spin_lock(&wq->lock);   /* interrupts are off: the caller holds lock with irqsave */

    /* Append to wait queue tail */
    self->next = NULL;
    if (wq->tail) {
        wq->tail->next = self;
    } else {
        wq->head = self;
    }
    wq->tail = self;

    /* BLOCKED before either lock goes: a waker that comes in between finds the
     * thread on the queue and sets it running again (sched_wake), so the
     * wakeup is never lost */
    self->state = THREAD_BLOCKED;

    spin_unlock(&wq->lock);
    spin_unlock(lock);

    /* Schedule away (interrupts still off); returns once woken */
    sched_schedule();
    irq_restore(flags);
}

/* Unlink the first sleeper; wq->lock held. */
static Thread *wq_pop(WaitQueue *wq) {
    Thread *t = wq->head;
    if (t == NULL) return NULL;
    wq->head = t->next;
    if (wq->head == NULL) wq->tail = NULL;
    t->next = NULL;
    return t;
}

int wq_wake(WaitQueue *wq) {
    uint64_t flags;
    spin_lock_irqsave(&wq->lock, &flags);
    Thread *t = wq_pop(wq);
    if (t != NULL) sched_wake(t);
    spin_unlock_irqrestore(&wq->lock, flags);
    return t != NULL;
}

int wq_wake_all(WaitQueue *wq) {
    int count = 0;
    uint64_t flags;
    spin_lock_irqsave(&wq->lock, &flags);
    Thread *t;
    while ((t = wq_pop(wq)) != NULL) {
        sched_wake(t);
        count++;
    }
    spin_unlock_irqrestore(&wq->lock, flags);
    return count;
}
