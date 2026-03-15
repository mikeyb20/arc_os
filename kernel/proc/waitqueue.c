#include "proc/waitqueue.h"
#include "proc/sched.h"

void wq_init(WaitQueue *wq) {
    wq->lock = (Spinlock)SPINLOCK_INIT;
    wq->head = NULL;
    wq->tail = NULL;
}

void wq_sleep(WaitQueue *wq, Spinlock *lock) {
    Thread *self = thread_current();

    spinlock_acquire(&wq->lock);

    /* Append to wait queue tail */
    self->next = NULL;
    if (wq->tail) {
        wq->tail->next = self;
    } else {
        wq->head = self;
    }
    wq->tail = self;

    self->state = THREAD_BLOCKED;

    /* Release wq->lock first (IF stays 0 — nested under caller's lock),
     * then release caller's lock (IF restored to 1). This avoids holding
     * wq->lock with interrupts enabled. */
    spinlock_release(&wq->lock);
    spinlock_release(lock);

    /* Schedule away — returns when another thread calls wq_wake. */
    sched_yield();
}

int wq_wake(WaitQueue *wq) {
    spinlock_acquire(&wq->lock);

    Thread *t = wq->head;
    if (t == NULL) {
        spinlock_release(&wq->lock);
        return 0;
    }

    wq->head = t->next;
    if (wq->head == NULL) {
        wq->tail = NULL;
    }
    t->next = NULL;

    sched_add_thread(t);

    spinlock_release(&wq->lock);
    return 1;
}

int wq_wake_all(WaitQueue *wq) {
    int count = 0;

    spinlock_acquire(&wq->lock);

    while (wq->head) {
        Thread *t = wq->head;
        wq->head = t->next;
        t->next = NULL;

        sched_add_thread(t);
        count++;
    }
    wq->tail = NULL;

    spinlock_release(&wq->lock);
    return count;
}
