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

#define WAITQUEUE_INIT { .lock = SPINLOCK_INIT, .head = NULL, .tail = NULL }

/* Initialize a wait queue. */
void wq_init(WaitQueue *wq);

/* Sleep on the wait queue.  Caller must hold `lock`; it is released atomically
 * after the thread is enqueued (prevents lost wakeups).  Callers must
 * re-check their condition in a while loop (spurious wakeup safe). */
void wq_sleep(WaitQueue *wq, Spinlock *lock);

/* Wake the first thread on the queue.  Returns 1 if a thread was woken, 0 if
 * the queue was empty. */
int wq_wake(WaitQueue *wq);

/* Wake all threads on the queue.  Returns the number of threads woken. */
int wq_wake_all(WaitQueue *wq);

#endif /* ARCHOS_PROC_WAITQUEUE_H */
