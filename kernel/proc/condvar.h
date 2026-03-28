#ifndef ARCHOS_PROC_CONDVAR_H
#define ARCHOS_PROC_CONDVAR_H

#include "proc/mutex.h"
#include "proc/waitqueue.h"

/* Condition variable — allows threads to block until a condition is met.
 *
 * Usage pattern:
 *   mutex_lock(&m);
 *   while (!condition)
 *       condvar_wait(&cv, &m);
 *   // condition is true, mutex is held
 *   mutex_unlock(&m);
 *
 * MUST NOT be used from interrupt context. */
typedef struct {
    WaitQueue waiters;
} CondVar;

#define CONDVAR_INIT { .waiters = WAITQUEUE_INIT }

/* Initialize a condition variable. */
void condvar_init(CondVar *cv);

/* Atomically release mutex and sleep.  Re-acquires mutex before returning.
 * Caller MUST hold the mutex.  Must re-check condition in a loop. */
void condvar_wait(CondVar *cv, Mutex *m);

/* Wake one thread waiting on the condition variable. */
void condvar_signal(CondVar *cv);

/* Wake all threads waiting on the condition variable. */
void condvar_broadcast(CondVar *cv);

#endif /* ARCHOS_PROC_CONDVAR_H */
