#ifndef ARCHOS_PROC_WAITQUEUE_H
#define ARCHOS_PROC_WAITQUEUE_H

#include "proc/spinlock.h"
#include "proc/thread.h"

/* Wait queue: threads sleep here until woken by another thread or IRQ.
 * Standard sleep/wakeup primitive — used for pipes, TTY, sys_wait, etc. */
typedef struct WaitQueue {
    Spinlock lock;
    Thread  *head;
    Thread  *tail;
} WaitQueue;

#define WAITQUEUE_INIT \
    { .lock = SPINLOCK_INIT(LOCK_LEVEL_WAITQUEUE, "wait queue"), .head = NULL, .tail = NULL }

/* Initialize a wait queue. */
void wq_init(WaitQueue *wq);

/* Sleep on the wait queue. The caller holds `lock`, taken with
 * spin_lock_irqsave(lock, &flags); wq_sleep releases it once the thread is
 * queued (no lost wakeups), sleeps, and returns with lock NOT held and the
 * interrupt state `flags` restored. Callers re-check their condition in a
 * loop, taking the lock again. */
void wq_sleep(WaitQueue *wq, Spinlock *lock, uint64_t flags);

/* Wake the first thread on the queue.  Returns 1 if a thread was woken, 0 if
 * the queue was empty. */
int wq_wake(WaitQueue *wq);

/* Wake all threads on the queue.  Returns the number of threads woken. */
int wq_wake_all(WaitQueue *wq);

#endif /* ARCHOS_PROC_WAITQUEUE_H */
