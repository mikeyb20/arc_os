#ifndef ARCHOS_PROC_MUTEX_H
#define ARCHOS_PROC_MUTEX_H

#include "proc/spinlock.h"
#include "proc/waitqueue.h"
#include "proc/thread.h"

/* Sleeping mutex — blocks (yields) the calling thread when contended.
 *
 * MUST NOT be acquired from interrupt context (will deadlock).
 * NOT recursive — re-locking from the same thread is undefined behavior.
 * Interrupts remain enabled while blocked (unlike spinlocks). */
typedef struct {
    Spinlock  guard;      /* Protects internal state */
    Thread   *owner;      /* Current holder (NULL if unlocked) */
    WaitQueue waiters;    /* Threads waiting to acquire */
} Mutex;

#define MUTEX_INIT { \
    .guard = SPINLOCK_INIT, \
    .owner = NULL, \
    .waiters = WAITQUEUE_INIT \
}

/* Initialize a mutex. */
void mutex_init(Mutex *m);

/* Acquire the mutex.  Blocks (sleeps) if already held by another thread. */
void mutex_lock(Mutex *m);

/* Release the mutex.  Wakes one blocked waiter if any. */
void mutex_unlock(Mutex *m);

/* Try to acquire without blocking.  Returns 0 on success, -1 if busy. */
int mutex_trylock(Mutex *m);

#endif /* ARCHOS_PROC_MUTEX_H */
