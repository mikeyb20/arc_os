#ifndef ARCHOS_PROC_SEMAPHORE_H
#define ARCHOS_PROC_SEMAPHORE_H

#include "proc/spinlock.h"
#include "proc/waitqueue.h"
#include <stdint.h>

/* Counting semaphore — blocks when count reaches zero.
 *
 * MUST NOT be used from interrupt context (sem_wait sleeps).
 * sem_post (V operation) MAY be called from any context. */
typedef struct {
    Spinlock  guard;
    int32_t   count;
    WaitQueue waiters;
} Semaphore;

#define SEMAPHORE_INIT(n) { \
    .guard = SPINLOCK_INIT, \
    .count = (n), \
    .waiters = WAITQUEUE_INIT \
}

/* Initialize a semaphore with the given starting count. */
void sem_init(Semaphore *s, int32_t initial_count);

/* Decrement (P / wait).  Blocks if count is zero. */
void sem_wait(Semaphore *s);

/* Increment (V / post).  Wakes one blocked waiter if any. */
void sem_post(Semaphore *s);

/* Try to decrement without blocking.  Returns 0 on success, -1 if zero. */
int sem_trywait(Semaphore *s);

/* Return current count (snapshot — may change immediately). */
int32_t sem_getvalue(Semaphore *s);

#endif /* ARCHOS_PROC_SEMAPHORE_H */
